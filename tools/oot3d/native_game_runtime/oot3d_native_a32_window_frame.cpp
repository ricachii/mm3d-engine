// Native A32 window session: one frame of the loop and the renderer-side work it schedules.
// First: on the Switch it brings in glad, which must precede any GL header.
#include "oot3d_native_gpu_frame_timer.h"
#include "oot3d_native_a32_window_session.h"
#include "oot3d_native_capture_clock.h"
#include <fstream>
#include <sstream>
#include "fast/renderer3ds/pica_surface_debug.h"
#include "oot3d_native_room_shadow_shell.h"
#include "oot3d_switch_threads.h"
#include "oot3d_switch_clocks.h"
#include "oot3d_hang_watchdog.h"
#include "oot3d_heap_bin_check.h"
#include "oot3d_switch_guard_alloc.h"
#include "oot3d_fps_counter.h"
#if defined(__SWITCH__)
#include "fast/oot3d/debug_menu.h"
#include "oot3d/renderer/azahar_texture_pack.h"
#endif
// Portable: ExitGameRequested() se usa sin condicion mas abajo.
#include "fast/oot3d/debug_menu.h"
#include "fast/oot3d/pica_texture_predecode.h"
#include "oot3d/renderer/azahar_texture_pack_settings.h"
#include <imgui.h>
#include "fast/backends/gl_frame_profiler.h"
#include "fast/backends/gl_pica_dynamic_scale.h"
#include "fast/backends/gfx_deko3d.h"
#include "ship/resource/File.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"
#if defined(__SWITCH__)
namespace Fast {
// gfx_opengl.h (its GL headers stay out).
using WindowAcquireHook = void (*)(bool after);
void SetWindowAcquireHook(WindowAcquireHook hook);
} // namespace Fast
#endif
#if defined(__SWITCH__)
#include <malloc.h>
#include <switch.h>
#endif

extern "C" {
// Provided only by a Mesa built with the Horizon shader cache (mesa_switch.h).
unsigned mesa_switch_get_stats(uint64_t* out, unsigned count) __attribute__((weak));
const char* mesa_switch_shader_cache_status(void) __attribute__((weak));
#if defined(__SWITCH__)
// newlib's running totals (mallocr.c: arena = memory taken with sbrk), kept
// up to date by malloc itself: reading them takes no lock, unlike mallinfo(),
// which holds the malloc lock shared with Mesa while it walks every free list.
extern struct mallinfo __malloc_current_mallinfo __attribute__((weak));
extern size_t __malloc_max_sbrked_mem __attribute__((weak));
#endif
}

namespace Oot3dA32Window {

namespace {

// The performance overlay (L3+R3 > Video, config/fps_counter); game thread only.
Oot3dNativeGame::PerfOverlay &PerfOverlayState() {
  static Oot3dNativeGame::PerfOverlay overlay;
  return overlay;
}

uint64_t HashDiagnosticBytes(std::span<const uint8_t> bytes) noexcept {
  uint64_t hash = 14695981039346656037ULL;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool IsNativeFrontendOpaqueTargetInitializationDraw(
    const Oot3dNativeGame::Oot3dPicaDrawSubmission &draw) {
  const auto &state = draw.State;
  const auto &blend = state.OutputMerger.Blend;
  const auto &depth = state.OutputMerger.Depth;
  const bool hasTexture =
      std::any_of(state.Textures.begin(), state.Textures.end(),
                  [](const auto &texture) { return texture.Enabled; });
  // Native UI targets include rotated/padded storage outside the logical
  // viewport, so framebuffer dimensions cannot establish whether this copy
  // quad initializes the frontend canvas. The replacement blend/depth state
  // identifies that operation; its association with CommonBackground00's
  // target AND viewport is checked by the caller, preserving the scene clear.
  return !hasTexture && state.VertexInput.Indexed &&
         state.VertexInput.VertexCount == 4U &&
         state.OutputMerger.ColorWriteMask == 0x0FU &&
         state.OutputMerger.LogicOperation ==
             Oot3dNativeGame::Oot3dPicaLogicOperation::Copy &&
         blend.Enabled &&
         blend.SourceColor == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationColor ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         blend.SourceAlpha == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationAlpha ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         depth.TestEnabled && depth.WriteEnabled &&
         depth.Compare == Oot3dNativeGame::Oot3dPicaCompareFunction::Always;
}

Oot3dNativeGame::NativeGuestClockDeadlineResolution AdvanceGuestClockTo(
    Oot3dNativeGame::NativeA32Process &process,
    Oot3dNativeGame::NativeA32CtrHostServices &hostServices,
    Oot3dNativeGame::NativeA32ProcessRunResult &processResult,
    uint64_t deadline, uint32_t wholeAotBlockBudget,
    const std::function<void()> *betweenRuns) {
  auto resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.OvershootTicks != 0U) {
    return resolution;
  }
  while (processResult.Kind !=
         Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    const auto wakeTick = hostServices.NextSleepWakeTick();
    if (!wakeTick.has_value() || *wakeTick > deadline) {
      break;
    }
    const uint64_t currentTick = hostServices.SystemTicks();
    hostServices.AdvanceSystemTicks(
        *wakeTick > currentTick ? *wakeTick - currentTick : 0U);
    processResult = RunUntilGuestWait(process, wholeAotBlockBudget, betweenRuns);
    RequireRunnableGuest(processResult);
    resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
        hostServices.SystemTicks(), deadline);
    if (resolution.OvershootTicks != 0U) {
      return resolution;
    }
  }
  resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.AdvanceTicks != 0U) {
    hostServices.AdvanceSystemTicks(resolution.AdvanceTicks);
  }
  return resolution;
}

// GPU time of the presented frames (render thread only; one session).
Oot3dNativeGame::NativeGpuFrameTimer &GpuFrameTimer() {
  static Oot3dNativeGame::NativeGpuFrameTimer timer;
  return timer;
}

// Blocking window acquire inside the render-thread frame (render thread).
Oot3dNativeGame::NativeRenderWaitClock &RenderWaitClock() {
  static Oot3dNativeGame::NativeRenderWaitClock clock;
  return clock;
}
// The governor's render-thread cost keeps the acquire wait (pgo5i, the
// default again since pgo5l); config/interp_cpu_exclude_wait leaves it out
// (994ccd7d, with config/interp_margin_ms 5 as in pgo5k).
bool gCountRenderWait = true;
// config/gl_pacing_flush: glFlush between the frame's last command and the
// pacer's wait (994ccd7d). Off by default since pgo5l (pgo5i: scanout and UI
// reach the GPU at the swap).
bool gFlushBeforePacing = false;

#if defined(__SWITCH__)
void MarkGpuFrameAcquire(bool after) {
  GpuFrameTimer().MarkAcquire(after);
  // armGetSystemTick (cntpct_el0); cntvct_el0 faults on Horizon.
  static uint64_t beginTick = 0U;
  const uint64_t tick = armGetSystemTick();
  if (after) {
    RenderWaitClock().End(tick);
    // tour19: without the display's vsync event, an acquire that blocked
    // returned at a vsync (oot3d_vsync_clock.h, AcquireVsyncFilter).
    if (auto *clock =
            Oot3dNativeGame::gAcquireVsyncClock.load(std::memory_order_relaxed);
        clock != nullptr && beginTick != 0U && tick > beginTick) {
      const double waitMs =
          static_cast<double>(armTicksToNs(tick - beginTick)) / 1.0e6;
      const int64_t nowNs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      clock->ObserveAcquire(nowNs, waitMs);
    }
  } else {
    RenderWaitClock().Begin(tick);
    beginTick = tick;
  }
}
#endif

} // namespace

void NativeA32WindowSession::EnsureInterpolationPacing() {
  auto &api = *apiPointer;
  auto &realtimePacer = *realtimePacerStorage;
  const uint32_t rateHz = realtimePacer.TargetRateHz() != 0U
                              ? realtimePacer.TargetRateHz()
                              : 60U;
  // L3+R3 > Video > Resolution at 60 FPS, published by the menu: a new
  // choice reconfigures the pacing as a rate change does.
  {
    const auto &liveSixty = Oot3dNativeGame::LiveDynamicResolutionSixty();
    const uint64_t revision = liveSixty.Revision.load(std::memory_order_acquire);
    if (revision != liveDynamicResolutionSixtyRevision) {
      liveDynamicResolutionSixtyRevision = revision;
      interpolationPacingOptions.DynamicResolutionSixty = liveSixty.Load();
    }
  }
  if (interpolationPacingConfigured && rateHz == interpolationPacingRateHz &&
      interpolationPacingOptions.DynamicResolutionSixty == dynamicResolutionSixtyMode) {
    // A backend reset (InitNativePica) forgets the namespace scale.
    if (interpolationPacing.ReducedAvailable() &&
        api.PicaNamespaceResolutionScale(kReducedVisualRenderTargetNamespace) !=
            interpolationPacing.Config().ReducedScale) {
      (void)api.SetPicaNamespaceResolutionScale(
          kReducedVisualRenderTargetNamespace,
          interpolationPacing.Config().ReducedScale);
    }
    return;
  }
  // First call, or a new rate (L3+R3 > Video > FPS applied at a frame
  // boundary, or the PC's F1 menu): every governor starts over as at a start
  // at this rate (oot3d_native_rate_pacing.h).
  const bool firstConfiguration = !interpolationPacingConfigured;
  // The scale 60 FPS last ran at (a switch back to 60 begins there).
  if (!firstConfiguration && dynamicResolutionActive)
    dynamicResolutionMemory.Leave(interpolationPacingRateHz, dynamicResolution);
  interpolationPacingConfigured = true;
  interpolationPacingRateHz = rateHz;
  const auto &options = interpolationPacingOptions;
  Oot3dNativeGame::RatePacingConfig pacing;
  pacing.BaseScale =
      api.PicaNamespaceResolutionScale(kVisualInterpolationRenderTargetNamespace);
  auto &config = pacing.Interpolation;
  config.KeepGameSpeed = options.KeepGameSpeed;
  config.ReducedScale = Oot3dNativeGame::ParseInterpolationReducedScale(
      options.ReducedScaleText, pacing.BaseScale);
  if (const auto margin =
          Oot3dNativeGame::ParseInterpolationMarginMs(options.MarginText))
    config.MarginMs = *margin;
  config.ReducedMode = options.ReducedAlways
                           ? Oot3dNativeGame::InterpolationReducedScaleMode::Always
                           : Oot3dNativeGame::InterpolationReducedScaleMode::Auto;
  config.EntryRecover = options.PacerEntryRecover;
  pacing.ReducedAvailable =
      config.ReducedScale > 0.0F &&
      api.SetPicaNamespaceResolutionScale(kReducedVisualRenderTargetNamespace,
                                          config.ReducedScale);
  const bool reducedAvailable = pacing.ReducedAvailable;
  const float baseScale = pacing.BaseScale;
  // Dynamic resolution (config/dynamic_resolution, off by default): between
  // the backend's scale and the configured lowest one, on the same pair
  // budget (oot3d_native_dynamic_resolution.h). Back at the top scale.
  auto &backendScale = Fast::PicaGlDynamicScale::gState;
  pacing.Resolution.MaxScale = backendScale.MaxScale;
  pacing.Resolution.MinScale = Oot3dNativeGame::ParseDynamicResolutionMinScale(
      options.DynamicResolutionText, pacing.Resolution.MaxScale);
  dynamicResolutionActive = options.DynamicResolution &&
                            backendScale.Supported &&
                            pacing.Resolution.MinScale < pacing.Resolution.MaxScale;
  // deko3d times its frames itself, nothing hidden: its own thresholds
  // (log16: the GL calibration kept every 60 FPS area at the floor).
  bool drsDeko3dTiming = false;
#if defined(__SWITCH__)
  drsDeko3dTiming = Fast::Deko3dRendererSelected();
#endif
  if (drsDeko3dTiming)
    Oot3dNativeGame::UseDeko3dDynamicResolutionTiming(pacing.Resolution);
  pacing.ResolutionSixty = options.DynamicResolutionSixty;
  dynamicResolutionSixtyMode = options.DynamicResolutionSixty;
  pacing.ResolutionStartScale = dynamicResolutionMemory.StartScale(
      rateHz, !firstConfiguration, pacing.Resolution);
  backendScale.Requested = 0.0F;
  drsRenderBusyAverageMs = 0.0;
  // Presentation cadence (config/fps60_mode): legacy at any rate but 60.
  pacing.Cadence = options.Fps60Mode;
  Oot3dNativeGame::ConfigureRatePacing(rateHz, pacing, interpolationPacing,
                                       dynamicResolution, cadenceGovernor);
  cadenceGovernorConfigured = true;
  cadenceLockedThirty.store(false, std::memory_order_relaxed);
  if (options.DynamicResolution)
    std::fprintf(stderr,
                 "TRIAEVUM_DYNAMIC_RESOLUTION active=%d supported=%d "
                 "max=%.2f min=%.2f budget_ms=%.1f timing=%s down=%.2f "
                 "up=%.2f hidden_ms=%.1f drs_60=%s held_full=%d\n",
                 dynamicResolutionActive ? 1 : 0,
                 backendScale.Supported ? 1 : 0, pacing.Resolution.MaxScale,
                 dynamicResolution.Config().MinScale,
                 dynamicResolution.BudgetMs(),
                 drsDeko3dTiming ? "deko3d" : "gl",
                 dynamicResolution.Config().DownFraction,
                 dynamicResolution.Config().UpFraction,
                 dynamicResolution.Config().HiddenGpuMs,
                 Oot3dNativeGame::DynamicResolutionSixtyModeName(
                     dynamicResolutionSixtyMode).data(),
                 dynamicResolution.LowestLevel() == 0U ? 1 : 0);
  {
    std::lock_guard lock(interpolationPerfMutex);
    interpolationPerf.DrsActive = dynamicResolutionActive;
    interpolationPerf.DrsMinScale = dynamicResolution.Config().MinScale;
    interpolationPerf.DrsMode = dynamicResolutionSixtyMode;
    interpolationPerf.RateHz = rateHz;
    interpolationPerf.CadenceMode = cadenceGovernor.Config().Mode;
  }
  if (!firstConfiguration)
    std::fprintf(stderr,
                 "TRIAEVUM_FRAME_RATE rate_hz=%u pair_budget_ms=%.1f "
                 "decision_budget_ms=%.1f fps60_mode=%s interpolation=%d "
                 "drs_60=%s\n",
                 rateHz, interpolationPacing.PairBudgetMs(),
                 interpolationPacing.DecisionBudgetMs(),
                 Oot3dNativeGame::Fps60ModeName(cadenceGovernor.Config().Mode)
                     .data(),
                 visualInterpolationEnabled ? 1 : 0,
                 Oot3dNativeGame::DynamicResolutionSixtyModeName(
                     dynamicResolutionSixtyMode).data());
  if (firstConfiguration) {
#if defined(__SWITCH__)
    // The window acquire's idle gap is not GPU work (gpu_frame_timer.h);
    // config/interp_count_acquire_wait counts it as before.
    const bool countAcquireWait =
        std::ifstream("sdmc:/switch/oot3dre/config/interp_count_acquire_wait")
            .good();
    gCountRenderWait =
        !std::ifstream("sdmc:/switch/oot3dre/config/interp_cpu_exclude_wait")
             .good();
    gFlushBeforePacing =
        std::ifstream("sdmc:/switch/oot3dre/config/gl_pacing_flush").good();
    GpuFrameTimer().SetExcludeAcquireWait(!countAcquireWait);
    // The hook also times the render thread's wait (perf rt_wait_ms_*).
    Fast::SetWindowAcquireHook(&MarkGpuFrameAcquire);
#endif
    skipNativeReplay =
        !options.ForceNativeReplay && !api.PicaNativeNamespaceObservable();
    std::fprintf(stderr,
                 "TRIAEVUM_INTERP_PACING keep_speed=%d reduced_scale=%.2f "
                 "reduced_available=%d mode=%s base_scale=%.2f "
                 "native_replay=%d fps60_mode=%s cpu_count_wait=%d "
                 "flush_before_pacing=%d margin_ms=%.2f entry_recover=%d "
                 "entry_probe_interval=%u entry_decisions=%u "
                 "max_probe_interval=%u\n",
                 config.KeepGameSpeed ? 1 : 0, config.ReducedScale,
                 reducedAvailable ? 1 : 0,
                 options.ReducedAlways ? "always" : "auto", baseScale,
                 skipNativeReplay ? 0 : 1,
                 Oot3dNativeGame::Fps60ModeName(options.Fps60Mode.Mode).data(),
                 gCountRenderWait ? 1 : 0, gFlushBeforePacing ? 1 : 0,
                 config.MarginMs, config.EntryRecover ? 1 : 0,
                 config.EntryProbeInterval, config.EntryDecisions,
                 config.MaxProbeInterval);
  }
  std::lock_guard lock(interpolationPerfMutex);
  interpolationPerf.ReducedScale = reducedAvailable ? config.ReducedScale : 0.0F;
}

void NativeA32WindowSession::FinishRenderFrameCost() {
  auto &timer = GpuFrameTimer();
  const auto kind = renderFrameKind;
  const double cpuMs = renderFrameCpuMs;
  renderFrameKind = 0U;
  renderFrameCpuMs = 0.0;
  // Render-thread waits inside the frame (the window acquire) are not its
  // work: the governor learns the busy part (gpu_frame_timer.h).
  uint64_t waitTicks = RenderWaitClock().Take();
#if defined(__SWITCH__)
  const double waitMs = static_cast<double>(armTicksToNs(waitTicks)) / 1.0e6;
#else
  (void)waitTicks;
  const double waitMs = 0.0;
#endif
  // config/pacer_entry_recover: the render thread's holds at a scene's start
  // (config/texture_pack_hold, config/grass_hold) waited for the scene's
  // entry work; the frame did no work then, and a held frame must not count
  // towards a drop nor inflate the in-between ratio the recovery rests on.
  double holdMs = 0.0;
  bool entryWork = false;
#if defined(__SWITCH__)
  holdMs = static_cast<double>(Fast::Deko3dTakeRenderHoldMicros()) / 1000.0;
  // And the entry work still running (HD pack decodes, grass placements):
  // the governor probes 60 again soon after it ends. Asked only in an
  // entry window (one pack mutex a frame there).
  if (interpolationPacing.Config().EntryRecover && interpolationPacing.InEntry() &&
      Fast::Deko3dRendererSelected()) {
    entryWork = Fast::Deko3dEntryWorkPending();
    interpolationPacing.SetEntryWork(entryWork);
  }
#endif
  const double workCpuMs = interpolationPacing.Config().EntryRecover
                               ? std::max(0.0, cpuMs - holdMs)
                               : cpuMs;
  const double costCpuMs =
      gCountRenderWait
          ? workCpuMs
          : Oot3dNativeGame::NativeRenderThreadBusyMs(workCpuMs, waitMs);
  // logs/spikes.txt: this frame's render-thread work and acquire wait.
  spikeRenderBusyMs = Oot3dNativeGame::NativeRenderThreadBusyMs(cpuMs, waitMs);
  spikeRenderWaitMs = waitMs;
  if (spikeRenderBusyMs > 0.0 && std::isfinite(spikeRenderBusyMs))
    drsRenderBusyAverageMs =
        drsRenderBusyAverageMs > 0.0
            ? (7.0 * drsRenderBusyAverageMs +
               std::min(spikeRenderBusyMs, 2.0 * drsRenderBusyAverageMs)) /
                  8.0
            : spikeRenderBusyMs;
  const auto observe = [&](uint8_t frameKind, double costMs) {
    switch (static_cast<Oot3dNativeGame::RenderFrameKind>(frameKind)) {
    case Oot3dNativeGame::RenderFrameKind::Step:
      interpolationPacing.ObserveStep(costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Full:
      interpolationPacing.ObserveInterpolated(
          Oot3dNativeGame::InterpolatedSampleLevel::Full, costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Reduced:
      interpolationPacing.ObserveInterpolated(
          Oot3dNativeGame::InterpolatedSampleLevel::Reduced, costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Dropped:
      interpolationPacing.ObserveInterpolated(
          Oot3dNativeGame::InterpolatedSampleLevel::Dropped, costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Other:
      break;
    }
  };
  std::array<double, Oot3dNativeGame::kRenderFrameKindCount> gpuMs{};
  std::array<uint64_t, Oot3dNativeGame::kRenderFrameKindCount> gpuFrames{};
#if defined(__SWITCH__)
  // deko3d: the device times its frames itself (gfx_deko3d_device.cpp, busy
  // = scene + tail, the acquire wait left out like MarkAcquire does here).
  const bool deko3dTiming = Fast::Deko3dRendererSelected();
#else
  const bool deko3dTiming = false;
#endif
  const bool gpuAvailable = timer.Available() || deko3dTiming;
  if (deko3dTiming)
    Fast::Deko3dTagFrame(kind, costCpuMs);
  timer.EndFrame(kind, costCpuMs);
#if defined(__SWITCH__)
  // The frame's last commands (scanout, UI) and its end timestamp go to the
  // GPU now, not at the swap after the pacer's wait: the GPU works while the
  // render thread waits, and the end timestamp no longer lands after the
  // wait (an idle gap counted as GPU work).
  if (gFlushBeforePacing && !Fast::Deko3dRendererSelected())
    glFlush();
#endif
  const auto onGpuResult = [&](const Oot3dNativeGame::NativeGpuFrameTime &result) {
    if (result.Kind >= gpuMs.size()) return;
    spikeGpuMs = result.Milliseconds; // newest GPU result (logs/spikes.txt)
    gpuMs[result.Kind] += result.Milliseconds;
    ++gpuFrames[result.Kind];
    // Dynamic resolution learns the GPU side only (a smaller image saves
    // no render-thread time): the step frame, twice while interpolating
    // (the in-between frame is one more replay of the same scene).
    // tour19: on the governor's terms (oot3d_native_dynamic_resolution.h):
    // its shedding of in-between frames counts when the GPU, not the render
    // thread's own work, is the larger side of the pair.
    if (dynamicResolutionActive &&
        result.Kind ==
            static_cast<uint8_t>(Oot3dNativeGame::RenderFrameKind::Step)) {
      const double pairMs = visualInterpolationEnabled
                                ? 2.0 * result.Milliseconds
                                : result.Milliseconds;
      const bool shedding =
          visualInterpolationEnabled &&
          (interpolationPacing.Level() != interpolationPacing.PreferredLevel() ||
           cadenceGovernor.DropsInBetween());
      const double busyPairMs = 2.0 * drsRenderBusyAverageMs;
      (void)dynamicResolution.ObservePair(
          pairMs, shedding &&
                      pairMs + dynamicResolution.Config().HiddenGpuMs >=
                          busyPairMs);
    }
    // Both sides of a frame overlap the neighbouring frames: the pair fits
    // only if each fits.
    observe(result.Kind, std::max(result.Milliseconds, result.CpuMilliseconds));
  };
  timer.Poll(onGpuResult);
  for (Fast::Deko3dGpuFrame frame; deko3dTiming && Fast::Deko3dTakeGpuFrame(frame);) {
    Oot3dNativeGame::NativeGpuFrameTime result;
    result.Kind = frame.Kind;
    result.Milliseconds = frame.Milliseconds;
    result.CpuMilliseconds = frame.CpuMilliseconds;
    onGpuResult(result);
  }
  if (!gpuAvailable) {
    // No GPU clock (yet): render-thread time only. The swap is left out:
    // it includes the vsync wait, which made every pair look over budget
    // in the first seconds and dropped in-between frames that fit.
    observe(kind, costCpuMs);
  }
  {
    using Kind = Oot3dNativeGame::RenderFrameKind;
    const auto frameKind = static_cast<Kind>(kind);
    if (frameKind == Kind::Step || frameKind == Kind::Full || frameKind == Kind::Reduced)
      newImagesPresented.fetch_add(1U, std::memory_order_relaxed);
  }
  std::lock_guard lock(interpolationPerfMutex);
  auto &perf = interpolationPerf;
  if (kind < perf.Frames.size()) {
    ++perf.Frames[kind];
    perf.CpuMs[kind] += cpuMs;
    perf.WaitMs[kind] += waitMs;
  }
  for (size_t index = 0U; index < gpuMs.size(); ++index) {
    perf.GpuMs[index] += gpuMs[index];
    perf.GpuFrames[index] += gpuFrames[index];
  }
  if (holdMs > 0.0) {
    ++perf.HoldFrames;
    perf.HoldMs += holdMs;
  }
  if (entryWork)
    ++perf.EntryWorkFrames;
  const auto &stats = interpolationPacing.Stats();
  perf.Probes = stats.Probes;
  perf.FailedProbes = stats.FailedProbes;
  perf.EntryProbes = stats.EntryProbes;
  perf.StaleProbes = stats.StaleProbes;
  perf.Entries = stats.Entries;
  perf.EntryBelowDecisions = stats.EntryBelowDecisions;
  perf.ProbeInterval = interpolationPacing.ProbeIntervalNow(false);
  perf.LevelChanges = stats.LevelChanges;
  perf.Level = static_cast<uint8_t>(interpolationPacing.Level());
  perf.GpuTimer = gpuAvailable;
  perf.GpuFactor = timer.NanosecondsPerTick();
  perf.StepCostMs = interpolationPacing.StepCostMs();
  perf.FullEstimateMs = interpolationPacing.Estimate(
      Oot3dNativeGame::InterpolatedSampleLevel::Full);
  perf.ReducedEstimateMs = interpolationPacing.Estimate(
      Oot3dNativeGame::InterpolatedSampleLevel::Reduced);
  perf.BudgetMs = interpolationPacing.PairBudgetMs();
  perf.DecisionBudgetMs = interpolationPacing.DecisionBudgetMs();
  perf.DrsScale = dynamicResolutionActive ? dynamicResolution.Scale() : 0.0F;
  perf.DrsPairMs = dynamicResolution.AveragePairMs();
  perf.Drs = dynamicResolution.Stats();
}

void NativeA32WindowSession::ObservePresentCadence(uint8_t frameKind) {
  if (!cadenceGovernorConfigured)
    return;
  using Kind = Oot3dNativeGame::RenderFrameKind;
  using Picture = Oot3dNativeGame::PresentedPicture;
  const auto kind = static_cast<Kind>(frameKind);
  const Picture picture = kind == Kind::Step ? Picture::Step
                          : kind == Kind::Full || kind == Kind::Reduced
                              ? Picture::InBetween
                              : Picture::Repeat;
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const double stepMs = interpolationPacing.StepCostMs();
  const double gpuPairMs =
      stepMs > 0.0
          ? stepMs + interpolationPacing.Estimate(
                         Oot3dNativeGame::InterpolatedSampleLevel::Full)
          : 0.0;
  cadenceGovernor.ObservePresent(picture, now, gpuPairMs);
  if (hitchWriter.has_value()) {
    const auto gpu = apiPointer->GetNativePicaBackendStats();
    const Oot3dNativeGame::HitchSample sample{hitchGuestMs, hitchGameBusyMs,
                                              gpu.TexturesCreated, gpu.ProgramsLive, hitchArea};
    const uint32_t rateHz = cadenceGovernor.DropsInBetween()
                                ? 30U
                                : std::max<uint32_t>(interpolationPacingRateHz, 1U);
    if (auto line = hitchRecorder.Observe(now, hitchPresents, 1000.0 / rateHz, sample)) {
      (void)hitchWriter->Push(std::move(*line));
      for (size_t cause = 0U; cause < hitchCounts.size(); ++cause)
        hitchCounts[cause].store(
            hitchRecorder.Count(static_cast<Oot3dNativeGame::HitchCause>(cause)),
            std::memory_order_relaxed);
    }
    ++hitchPresents;
  }
  cadenceLockedThirty.store(cadenceGovernor.DropsInBetween(),
                            std::memory_order_relaxed);
  std::lock_guard lock(interpolationPerfMutex);
  auto &perf = interpolationPerf;
  perf.Cadence = cadenceGovernor.Stats();
  perf.CadenceThirty = cadenceGovernor.DropsInBetween();
  perf.CadenceHoldSeconds = cadenceGovernor.HoldSeconds();
  perf.GamePairP90Ms = cadenceGovernor.GamePairP90Ms();
}

void NativeA32WindowSession::ObserveFrameSpike(uint8_t frameKind,
                                               bool thirtyStep,
                                               double pacerWaitMs,
                                               uint64_t guestFrame,
                                               bool skipped) {
  if (!frameSpikesEnabled)
    return;
  using Kind = Oot3dNativeGame::RenderFrameKind;
  const int64_t nowNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const auto vsync = vsyncClock.Read();
  const auto &pacer = *realtimePacerStorage;
  const uint32_t targetHz =
      pacer.Enabled() && pacer.TargetRateHz() != 0U ? pacer.TargetRateHz() : 60U;
  const double displayNs = vsync.Valid ? vsync.PeriodNs : 1.0e9 / 60.0;
  const uint32_t refreshes = spikeRefreshCounter.Observe(
      nowNs, vsync.Valid, vsync.LastVsyncNs, vsync.PeriodNs, 1.0e9 / 60.0);
  ++spikePresents;
  // The game thread's cost of the host frame that made the previous
  // present arrives (render stream) just before this one: that sample is
  // complete now. Classified and recorded one present late.
  const auto flushPending = [&] {
    if (!spikePending.has_value())
      return;
    spikePending->GuestMs = static_cast<float>(hitchGuestMs);
    spikePending->GameBusyMs = static_cast<float>(hitchGameBusyMs);
    (void)Oot3dNativeGame::ObserveFrameSpike(*spikePending, spikePendingPeriodMs,
                                             spikeTotals, spikeRing);
    spikePending.reset();
  };
  flushPending();
  if (skipped)
    return;
  const auto kind = static_cast<Kind>(frameKind);
  if (kind == Kind::Other)
    Oot3dNativeGame::AddOwner(spikeHoldPresents, 1U);
  // Context: what changed since the previous present (cumulative totals).
  const auto gpu = apiPointer->GetNativePicaBackendStats();
  uint64_t shaders = 0U;
  if (mesa_switch_get_stats != nullptr) {
    std::array<uint64_t, 9> mesa{};
    if (mesa_switch_get_stats(mesa.data(), static_cast<unsigned>(mesa.size())) >= 9U)
      shaders = mesa[0] + mesa[8]; // glsl_compiles + nvc0_translates
  }
  const uint64_t drsChanges = dynamicResolution.Stats().Changes;
  const auto delta = [](uint64_t now, uint64_t before) {
    return now > before ? now - before : uint64_t{0};
  };
  const auto clamp16 = [](uint64_t value) {
    return static_cast<uint16_t>(std::min<uint64_t>(value, 0xFFFFU));
  };
  Oot3dNativeGame::FrameSpikeSample sample;
  if (spikeHasPrevious) {
    sample.TexturesNew = clamp16(delta(gpu.TexturesCreated, spikePrevTextures));
    sample.ProgramsNew = clamp16(delta(gpu.ProgramsLive, spikePrevPrograms));
    sample.ShadersNew = clamp16(delta(shaders, spikePrevShaders));
    if (sample.TexturesNew != 0U) sample.Context |= Oot3dNativeGame::kSpikeTexUpload;
    if (sample.ProgramsNew != 0U) sample.Context |= Oot3dNativeGame::kSpikeProgram;
    if (sample.ShadersNew != 0U) sample.Context |= Oot3dNativeGame::kSpikeShader;
    if (delta(gpu.OutOfMemoryEvents, spikePrevOom) != 0U)
      sample.Context |= Oot3dNativeGame::kSpikeGlOom;
    if (delta(gpu.TexturesEvicted, spikePrevEvicted) != 0U)
      sample.Context |= Oot3dNativeGame::kSpikeEvict;
    if (drsChanges != spikePrevDrsChanges)
      sample.Context |= Oot3dNativeGame::kSpikeDrs;
  }
  // A scene change in the last ~2 s (120 presents): warp/loading.
  if (spikeSceneChangePresent != ~uint64_t{0} &&
      spikePresents - spikeSceneChangePresent <= 120U)
    sample.Context |= Oot3dNativeGame::kSpikeLoad;
  spikeHasPrevious = true;
  spikePrevTextures = gpu.TexturesCreated;
  spikePrevPrograms = gpu.ProgramsLive;
  spikePrevShaders = shaders;
  spikePrevOom = gpu.OutOfMemoryEvents;
  spikePrevEvicted = gpu.TexturesEvicted;
  spikePrevDrsChanges = drsChanges;

  sample.HostFrame = spikePresents;
  sample.GuestFrame = guestFrame;
  sample.TimeSeconds = static_cast<double>(nowNs) / 1.0e9;
  sample.Kind = kind == Kind::Step ? Oot3dNativeGame::SpikeFrameKind::Step
                : kind == Kind::Full || kind == Kind::Reduced
                    ? Oot3dNativeGame::SpikeFrameKind::Interp
                    : Oot3dNativeGame::SpikeFrameKind::Hold;
  // 60 Hz display: a 30 Hz target shows each picture two refreshes, and so
  // does a step of the locked 30.
  const uint32_t perPicture = std::max<uint32_t>(
      1U, static_cast<uint32_t>(std::lround(60.0 / static_cast<double>(targetHz))));
  sample.Expected = static_cast<uint8_t>(std::min<uint32_t>(
      perPicture * (thirtyStep ? 2U : 1U), 255U));
  sample.Refreshes = static_cast<uint8_t>(std::min<uint32_t>(refreshes, 255U));
  sample.RenderBusyMs = static_cast<float>(spikeRenderBusyMs);
  sample.RenderWaitMs = static_cast<float>(spikeRenderWaitMs);
  sample.PacerWaitMs = static_cast<float>(pacerWaitMs);
  sample.GpuMs = static_cast<float>(spikeGpuMs);
  sample.SwapMs = static_cast<float>(lastSwapMs);
#if defined(__SWITCH__)
  if (Fast::Deko3dRendererSelected()) {
    const Fast::Deko3dSwapTimes swap = Fast::Deko3dLastSwapTimes();
    sample.DkFinishMs = swap.FinishMs;
    sample.DkFenceMs = swap.FenceMs;
    sample.DkSubmitMs = swap.SubmitMs;
    sample.DkPresentMs = swap.PresentMs;
    sample.DkWaitPresentMs = swap.WaitPresentMs;
    sample.DkAcquireMs = swap.AcquireMs;
    sample.DkQueueBufferMs = swap.QueueBufferMs;
  }
#endif
  float topMs = 0.0F;
  uint8_t top = 0xFFU;
  if (Fast::GlPerf::LastFrameTopStage(&top, &topMs)) {
    sample.TopStage = top;
    sample.TopStageMs = topMs;
  }
  spikePending = sample;
  spikePendingPeriodMs = displayNs / 1.0e6;
}

void NativeA32WindowSession::DrainFrameSpikes() {
  if (!spikeWriter.has_value())
    return;
  std::string batch;
  char line[512];
  (void)spikeRing.Drain([&](const Oot3dNativeGame::FrameSpikeRecord &record) {
    const int size = Oot3dNativeGame::FormatFrameSpike(record, line, sizeof(line));
    if (size > 0)
      batch.append(line, std::min<size_t>(static_cast<size_t>(size), sizeof(line) - 1U));
  });
  if (!batch.empty())
    (void)spikeWriter->Push(std::move(batch));
}

void NativeA32WindowSession::PublishPresentationDeadline(
    uint64_t serial, std::chrono::steady_clock::time_point deadline) {
  const size_t index = serial % earlySlotDeadlineSerial.size();
  // Serial 0 marks an empty entry: store serial + 1.
  earlySlotDeadlineSerial[index].store(0U, std::memory_order_relaxed);
  earlySlotDeadlineNs[index].store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          deadline.time_since_epoch())
          .count(),
      std::memory_order_relaxed);
  earlySlotDeadlineSerial[index].store(serial + 1U, std::memory_order_release);
}

std::optional<std::chrono::steady_clock::time_point>
NativeA32WindowSession::PublishedPresentationDeadline(uint64_t serial) const {
  const size_t index = serial % earlySlotDeadlineSerial.size();
  if (earlySlotDeadlineSerial[index].load(std::memory_order_acquire) !=
      serial + 1U) {
    return std::nullopt;
  }
  const int64_t ns = earlySlotDeadlineNs[index].load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (earlySlotDeadlineSerial[index].load(std::memory_order_relaxed) !=
      serial + 1U) {
    return std::nullopt;
  }
  return std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::nanoseconds(ns)));
}

bool NativeA32WindowSession::RunFrame() {
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  auto &api = *apiPointer;
  auto &preparationScreen = *preparationScreenStorage;
  auto &ocarinaText = *ocarinaTextStorage;
  auto &n64UiRenderer = *n64UiRendererStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  auto &presentationScheduler = *presentationSchedulerStorage;
  auto &realtimePacer = *realtimePacerStorage;
  // Hang watchdog: one host frame of progress, then the stage markers below.
  Oot3dNativeGame::HangWatch::Advance();
  Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::FrameBegin);
  const auto frameHeadStart = std::chrono::steady_clock::now();
  const auto phaseBeforeFrame = phaseTiming.Values();
  const double nestedGuestBeforeFrame = phaseTiming.GuestInsidePicaSubmitSeconds;
  const double guestClockBeforeFrame = phaseTiming.GuestClockSeconds;
  widescreenProjection.CurrentHostFrame =
      static_cast<uint32_t>(presentationFrameCount);
  // The render thread saw the exit request: leave before simulating more.
  if (renderStream.ShutdownRequested()) {
    window.Close();
    return false;
  }
  WindowDemoFrameTiming frameTiming;
  if (!PrepareNextWindowDemoFrame(hostArgs, window, timing, frameTiming)) {
    return true;
  }
  // L3+R3 > Video > FPS (LiveFrameRate): a new choice replaces the launch's
  // FrameRate mode here, at the frame boundary, before the policy below is
  // resolved; the change then goes through the same path as the PC's F1
  // menu (pacer, composition, interpolation history) and the render thread
  // reconfigures its governors for the new rate (EnsureInterpolationPacing).
  if (launchFrameRateMode.has_value()) {
    if (const auto live = Oot3dNativeGame::TakeLiveFrameRateMode(
            Oot3dNativeGame::LiveFrameRate(), liveFrameRateRevision,
            frameRateContract.Mode)) {
      launchFrameRateMode = *live;
    }
  }
  const auto requestedPresentationPolicy =
      Fast::Oot3d::ResolvePresentationPacingPolicy(EffectiveFrameRateMode());
  if (Fast::Oot3d::PresentationPacingPolicyChanged(
          presentationPacingPolicy, requestedPresentationPolicy)) {
    const auto requestedCompositionPolicy =
        ResolveFrameCompositionPolicy(requestedPresentationPolicy);
    const bool previousInterpolationEnabled = guestInterpolationEnabled;
    const bool requestedInterpolationEnabled =
        Oot3dNativeGame::ShouldUseNativeVisualInterpolation(
            frameRateContract,
            requestedCompositionPolicy.InterpolationEnabled());
    const bool compositionModeChanged =
        guestCompositionPolicy.Interpolation !=
            requestedCompositionPolicy.Interpolation ||
        guestCompositionPolicy.FixedSampleMultiplier !=
            requestedCompositionPolicy.FixedSampleMultiplier;
    presentationPacingPolicy = requestedPresentationPolicy;
    guestCompositionPolicy = requestedCompositionPolicy;
    guestInterpolationEnabled = requestedInterpolationEnabled;
#if defined(__SWITCH__)
    gSwitchPresentationRateHz = requestedPresentationPolicy.TargetRateHz;
    gSwitchVisualInterpolation = requestedInterpolationEnabled;
#endif
    renderStream.Submit([&, requestedPresentationPolicy,
                         requestedCompositionPolicy,
                         requestedInterpolationEnabled,
                         previousInterpolationEnabled,
                         compositionModeChanged] {
    frameCompositionPolicy = requestedCompositionPolicy;
    realtimePacer.Configure(requestedPresentationPolicy.Enabled &&
                                !hostArgs.ThroughputBenchmark,
                            requestedPresentationPolicy.TargetRateHz);
    visualSampleCadence.Configure(
        frameCompositionPolicy.FixedMultiplier()
            ? frameCompositionPolicy.FixedSampleMultiplier
            : 0U);
    visualInterpolationEnabled = requestedInterpolationEnabled;
    if (compositionModeChanged) {
      ++visualContinuityEpoch;
    }
    if (previousInterpolationEnabled != visualInterpolationEnabled) {
      preparedVisualTransition.Reset();
      visualContinuityTracker = {};
      previousVisualFrame.reset();
      latestVisualFrame.reset();
      latestVisualTransitionContinuous = false;
      visualSampleCadence.Reset();
    }
#if defined(__SWITCH__)
    // Grass budgets of the new rate (as a start at it would choose them).
    if (Fast::Deko3dRendererSelected())
      Fast::Deko3dSetGrassFrameRate(visualInterpolationEnabled);
#endif
    });
  }
  const auto presentationTime = std::chrono::steady_clock::now();
  untimed.HeadPolicy += std::chrono::duration<double>(presentationTime - frameHeadStart).count();
  renderStream.Submit([&, presentationTime] {
    realtimePacer.ObservePresentation(presentationTime);
  });
  // config/early_frame_slot: this frame waited (at the previous frame's end
  // submission) for the slot of picture S-2, released when its pacer began;
  // its refresh clock is that picture's deadline, so a wake-up before the
  // swap does not shift the refresh phase. Without a published deadline
  // (slot free, skipped frame) the clock is `now`.
  auto refreshClock = presentationTime;
  if (earlyFrameSlot && presentationFrameCount >= 2U) {
    if (const auto deadline =
            PublishedPresentationDeadline(presentationFrameCount - 2U)) {
      refreshClock = *deadline;
      ++earlySlotClockFrames;
    }
  }
  const double presentationElapsedSeconds =
      hostArgs.FixedDeltaSeconds > 0.0
          ? hostArgs.FixedDeltaSeconds
          : std::max(0.0, std::chrono::duration<double>(refreshClock -
                                                        lastPresentationTime)
                              .count());
  lastPresentationTime = std::max(lastPresentationTime, refreshClock);
#if defined(__SWITCH__)
  // Where the game is after the previous host frame (scene, room): for the
  // hitch of that frame and the perf.txt window (both Switch logs).
  if (processStorage) frameArea = Oot3dNativeGame::ReadNativeArea(processStorage->Memory());
  // deko3d grass (config/grass = draw): a culled selection belongs to the
  // area it was made in; a scene or room change drops it (one store).
  if (Fast::Deko3dGrassWantsColliders()) Fast::Deko3dSetGrassArea(frameArea.Scene, frameArea.Room);
  perfAreaWindow.Observe(frameArea);
  // HD texture pack (deko3d): a scene change reaches the pack runtime on the
  // render thread, after the previous frame's draws and before the new
  // scene's first: its listed world images start decoding, and the renderer's
  // hold counts its first frames (SWITCH_PORT.md "File select, world textures
  // and grass without pop-in").
  if (Fast::Deko3dRendererSelected() && frameArea.Scene != texturePackScene) {
    texturePackScene = frameArea.Scene;
    renderStream.Submit([scene = texturePackScene] {
      ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().NoteScene(scene);
    });
  }
  // Paced interpolation: a new scene (the load's -1 in between counts as a
  // change, so a reload of the same scene is an entry too) opens the
  // governor's entry window on the render thread, before the scene's first
  // frame (config/pacer_entry_recover; entries are counted either way).
  if (frameArea.Scene != pacerEntryScene) {
    pacerEntryScene = frameArea.Scene;
    if (frameArea.Scene >= 0)
      renderStream.Submit([this] { interpolationPacing.NoteEntry(); });
  }
#endif
  const auto headAreaEnd = std::chrono::steady_clock::now();
  untimed.HeadArea += std::chrono::duration<double>(headAreaEnd - presentationTime).count();
  // Presentation cadence (config/fps60_mode): the previous host frame's
  // game-thread cost (its duration minus the waits for the renderer) goes
  // to the render thread's governor.
  {
    const auto waits = renderStream.GetWaitStats();
    const uint64_t waitNanoseconds =
        waits.SubmitWaitNanoseconds + waits.DrainWaitNanoseconds;
    auto &previous = previousHostFrameCost;
    if (previous.Valid) {
      const double frameMs = std::chrono::duration<double, std::milli>(
                                 presentationTime - previous.Start)
                                 .count();
      const double waitMs =
          static_cast<double>(waitNanoseconds - previous.WaitNanoseconds) /
          1.0e6;
      const double busyMs = std::max(0.0, frameMs - waitMs);
      const double guestMs =
          (phaseTiming.GuestSeconds - previous.GuestSeconds) * 1000.0;
      renderStream.Submit([&, busyMs, guestMs, area = frameArea,
                           refreshes = previous.Refreshes,
                           step = hostFrameCompletedStep] {
        cadenceGovernor.ObserveGameFrame(busyMs, guestMs, refreshes, step);
        hitchGameBusyMs = busyMs;
        hitchGuestMs = guestMs;
        hitchArea = area;
      });
    }
    previous.Valid = true;
    previous.Start = presentationTime;
    previous.GuestSeconds = phaseTiming.GuestSeconds;
    previous.WaitNanoseconds = waitNanoseconds;
    hostFrameCompletedStep = false;
  }
  // adaptive at 60: one refresh per host frame (a refresh of debt instead
  // of a two-refresh catch-up frame), or both refreshes of a step at the
  // locked 30 (oot3d_native_cadence_governor.h). Otherwise as before.
  const bool cadencePlanning =
      guestInterpolationEnabled && presentationPacingPolicy.Enabled &&
      presentationPacingPolicy.TargetRateHz == 60U &&
      hostArgs.FixedDeltaSeconds <= 0.0 && !hostArgs.ThroughputBenchmark &&
      interpolationPacingOptions.Fps60Mode.Mode ==
          Oot3dNativeGame::Fps60Mode::Adaptive;
  const bool cadenceThirty =
      cadencePlanning && cadenceLockedThirty.load(std::memory_order_relaxed);
  const Oot3dNativeGame::NativePresentationScheduler::RefreshPlanner
      refreshPlanner = nullptr;
  // PlanSixtyRefreshes (debt instead of a two-refresh frame) slowed the game
  // in the simulation of integ4 (52 of 60 refreshes/s): not used until it
  // keeps the game speed. The scheduler plans as legacy in every mode.
  (void)cadenceThirty;
  const auto presentationStateBeforeFrame = presentationScheduler.CaptureState();
  const auto presentationStep = presentationScheduler.AdvancePlanned(
      presentationElapsedSeconds, refreshPlanner);
  const uint32_t guestRefreshesDue = presentationStep.GuestRefreshesDue;
  previousHostFrameCost.Refreshes = guestRefreshesDue;
  // The locked 30 keeps the 60 Hz host loop (in-between frames dropped).
  // Running both refreshes of a step in one host frame (PlanThirtyRefreshes)
  // serialised the game thread and the GPU on the console (integ4: 8.7% of
  // pictures held 3 refreshes while locked) and is no longer used.
  const bool cadenceThirtyPair = false;
  const float visualInterpolationAlpha = static_cast<float>(
      std::clamp(presentationStep.InterpolationAlpha, 0.0, 1.0));
  const auto headCadenceEnd = std::chrono::steady_clock::now();
  untimed.HeadCadence += std::chrono::duration<double>(headCadenceEnd - headAreaEnd).count();
  ApplyTopScreenConfigSnapshot();
  uiLifecycleBridge.BeginHostFrame(
      static_cast<uint32_t>(presentationFrameCount));
  if (topScreenTextureOverrideRuntime.has_value()) {
    topScreenTextureOverrideRuntime->Observe(uiLifecycleBridge);
  }

  const uint32_t width = std::max<uint32_t>(1, window.GetWidth());
  const uint32_t height = std::max<uint32_t>(1, window.GetHeight());
  hdFonts.SetOutputHeight(height);
  UpdateScenePresentation();
  phaseStart = std::chrono::steady_clock::now();
  untimed.HeadUi += std::chrono::duration<double>(phaseStart - headCadenceEnd).count();
  untimed.FrameHead += std::chrono::duration<double>(phaseStart - frameHeadStart).count();
  Fast::Renderer3ds::SlowStepTrace hostStartSteps("host_frame_start");
  window.GetMouseStateManager()->StartFrame();
  hostStartSteps.Mark("mouse");
  gui->StartDraw();
  hostStartSteps.Mark("gui_start_draw");
  // config/second_core: the host frame start below (overlay, input polling,
  // savestate requests, settings schedule, renderer frame begin) touches no
  // kernel, thread or host-service state, and its guest memory reads take
  // the kernel lock themselves: the second core keeps its SVCs meanwhile
  // (HostWaitRelease; nothing with the second core off). The menus drawn
  // by StartDraw above stay under the lock.
  std::optional<Oot3dNativeGame::SecondCore::HostWaitRelease> hostWorkRelease;
  hostWorkRelease.emplace();
  const auto startLockBefore = Oot3dNativeGame::SecondCore::SampleCore0Lock();
  // Performance overlay (L3+R3 > Video, config/fps_counter): new images
  // per second (steps and fps60 in-betweens, each once however many
  // refreshes it stays; a dropped in-between is not one), the longest time
  // one image stayed, the game speed of the last half second, and the clocks,
  // in the top-left corner. The flag is read every frame (the menu was drawn
  // just above, so a toggle shows in this frame); the text only changes when
  // a window closes, and the clocks come from the reader thread's cache.
  static bool perfOverlayShown = false;
  const bool perfOverlayEnabled =
      Oot3dNativeGame::PerfOverlayEnabled().load(std::memory_order_relaxed);
  if (!perfOverlayEnabled) {
    if (perfOverlayShown) {
      // Off: the next start measures from scratch (no interval spanning the
      // time it was hidden).
      PerfOverlayState() = Oot3dNativeGame::PerfOverlay{};
      perfOverlayShown = false;
    }
  } else {
    perfOverlayShown = true;
    auto &overlay = PerfOverlayState();
    // Images the render thread presented since the last host frame.
    static uint64_t overlayImagesSeen = 0U;
    const uint64_t imagesPresented = newImagesPresented.load(std::memory_order_relaxed);
    const auto newImages = static_cast<uint32_t>(imagesPresented - overlayImagesSeen);
    overlayImagesSeen = imagesPresented;
    if (overlay.ObserveFrame(std::chrono::duration<double>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count(),
                             newImages)) {
      overlay.SetClocks(Oot3dNativeGame::SwitchClockLatest());
    }
    // ProggyClean (13 px) scaled to a 32nd of the height: 22.5 px at 720p,
    // 34 px at 1080p; the longest line is about a third of the width.
    const float fontSize = std::max(16.0F, static_cast<float>(height) / 32.0F);
    const ImVec2 position(fontSize * 0.5F, fontSize * 0.4F);
    const float lineStep = fontSize * 1.1F;
    auto *drawList = ImGui::GetForegroundDrawList();
    const char *const lines[] = {overlay.FrameLine(), overlay.ClockLine()};
    for (size_t line = 0; line < 2U; ++line) {
      const ImVec2 at(position.x, position.y + lineStep * static_cast<float>(line));
      drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(at.x + 2.0F, at.y + 2.0F),
                        IM_COL32(0, 0, 0, 200), lines[line]);
      drawList->AddText(ImGui::GetFont(), fontSize, at, IM_COL32(255, 255, 80, 255),
                        lines[line]);
    }
  }
  renderStream.Submit([&] {
    window.StartFrame();
    // The window's events are pumped here, on the render thread: an exit
    // request (Quit, HOME > Close) seen now ends rendering before the rest of
    // this frame and any frame queued behind it runs.
    // (A run that closes itself completes its frames: see Finish.)
    if (!window.IsRunning() && !scriptedExit.load())
      renderStream.BeginShutdown();
  });
  hostStartSteps.Mark("render_start_frame");
  phaseTiming.HostFrameStartSeconds += SecondsSince(phaseStart);
  const auto inputPhaseStart = std::chrono::steady_clock::now();
  // F5 saves the game moment and F8 loads the last state; with Shift, the
  // settings are saved beside it / applied from it.
  const bool quickSaveKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F5);
  const bool quickLoadKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F8);
  const bool shiftDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_SHIFT) ||
                         window.IsKeyDown(Ship::KbScancode::LUS_KB_RSHIFT);
  if (quickLoadKeyDown && !quickLoadKeyWasDown) {
    savestateLibrary->Request(
        {Oot3dNativeGame::SavestateRequestKind::Load, shiftDown, {}});
  } else if (quickSaveKeyDown && !quickSaveKeyWasDown) {
    savestateLibrary->Request(
        {Oot3dNativeGame::SavestateRequestKind::Save, shiftDown, {}});
  }
  quickSaveKeyWasDown = quickSaveKeyDown;
  quickLoadKeyWasDown = quickLoadKeyDown;
  bool nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
      process.Memory(), uiLifecycleBridge,
      nativeCandidateDispatch.TopScreenUiProfile);
  uint32_t topScreenNativeGameMode = UINT32_MAX;
  const bool topScreenGameplayInputMode =
      process.Memory().Read32(kNativeGameMode, &topScreenNativeGameMode) &&
      Oot3dNativeGame::IsTopScreenGameplayInputMode(topScreenNativeGameMode);
  picaCompositionDomain =
      topScreenGameplayInputMode
          ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
          : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  picaRefreshCompositionDomain = picaCompositionDomain;
  bool topScreenStartRoutingEligible =
      uiLifecycleBridge.NativeGameplayPresentationActive() &&
      topScreenGameplayInputMode &&
      Oot3dNativeGame::IsTopScreenStartRoutingMode(topScreenNativeGameMode);
  Oot3dNativeGame::NativeAimProfileTransform aimProfileTransform;
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    const auto topScreenAim =
        Oot3dNativeGame::ResolveTopScreenCStickAimPolicy(
            activeTopScreenConfig);
    aimProfileTransform.RightStickScale = topScreenAim.SpeedMultiplier;
    aimProfileTransform.RightStickSmoothingCoefficient =
        Oot3dNativeGame::ResolveTopScreenCStickSmoothingCoefficient(
            activeTopScreenConfig.FreeCameraSmoothing);
    aimProfileTransform.RightStickInvertX = topScreenAim.InvertX;
    aimProfileTransform.RightStickInvertY = topScreenAim.InvertY;
  }
  const bool hostGuiVisible =
      gui->GetMenuOrMenubarVisible() || gui->GetAnyGuiWindowVisible();
  const auto inputPollStart = std::chrono::steady_clock::now();
  auto physicalInputFrame =
      PollNativeA32Input(window, nativeFrontendTouchEnabled,
                         ResolveNativeA32TouchPresentation(
                             process.Memory(),
                             nativeCandidateDispatch.TopScreenUiProfile),
                         hostGuiVisible,
                         nativeCandidateDispatch.TopScreenUiProfile,
                         activeTopScreenConfig.FreeCameraEnabled,
                         *controlConfigRuntime, aimProfileTransform,
                         presentationElapsedSeconds,
                         guestRefreshesDue != 0U,
                         nativeControlPollingState,
                         lastTopScreenPrimitivesBySubsystem[static_cast<
                             std::size_t>(oot3d::ui::UiSubsystem::GameplayHud)],
                         WidescreenMenuCornersActive());
  untimed.InputPoll += SecondsSince(inputPollStart);
  // Exit game / Close game in the L3+R3 menu leave like an exit input: the
  // window closes and Finish discards the frames still queued, as for HOME >
  // Close (main then ends the application).
  if (physicalInputFrame.Exit || devLabQuit || Fast::Oot3d::ExitGameRequested()) {
    if (devLabQuit) scriptedExit = true;
    window.Close();
    return false;
  }
  // Developer qualification: scheduled settings take the F1 commit path
  // (applied, not persisted) before this frame is rendered.
  for (const auto *step : graphicsSettingsSchedule.TakeDue(runFrameCount)) {
    auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
    const auto current = graphicsSettings.Snapshot();
    const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
        Oot3dNativeGame::MergeScheduledGraphics(
            Fast::Oot3d::SerializeGraphicsSettings(current), step->Graphics),
        current);
    const auto applied = graphicsSettings.Apply(decoded.Value, false);
    std::fprintf(stderr,
                 "TRIAEVUM_GRAPHICS_SETTINGS_SCHEDULE frame=%u accepted=%d "
                 "decode_issues=%zu apply_issues=%zu\n",
                 step->Frame, applied.Accepted() ? 1 : 0,
                 decoded.Issues.size(), applied.Issues.size());
    if (decoded.UnsupportedFutureVersion || !applied.Accepted())
      throw std::runtime_error(
          "graphics settings schedule step at frame " +
          std::to_string(step->Frame) + " was rejected");
  }
  if (!inputTimeline.Enabled()) {
    const uint32_t start = Oot3dNativeGame::NativeA32HidButtonMask(
        Oot3dNativeGame::NativeA32HidButton::Start);
    const bool physicalStartHeld =
        (physicalInputFrame.Hid.Buttons & start) != 0U;
    Oot3dNativeGame::ObserveNativeA32PolledButton(
        physicalStartHeld, nativeCandidateDispatch.StartButtonLatch);
    Oot3dNativeGame::ReleaseTopScreenStartRoutingLatch(
        physicalStartHeld, nativeCandidateDispatch.TopScreenStartRouting);
  }
  Oot3dNativeGame::NativeA32InputFrame inputFrame;
  phaseTiming.InputPollSeconds += SecondsSince(inputPhaseStart);
  const auto rendererFrameStart = std::chrono::steady_clock::now();
  CpuProbe::Scope cpuRendererStart(CpuProbe::Phase::RendererStart);
  const auto beginRendererFrame = [&, width, height,
                                   presentationSerial =
                                       presentationFrameCount] {
    api.UpdateFramebufferParameters(0, width, height, 1, false, true, true,
                                    true);
    if (titleRenderBackend != nullptr) {
      // Display settings are window operations: applied only by a frame
      // begun on the window's thread.
      titleRenderBackend->SetPresentationSettingsHeld(
          !renderStream.InSynchronousCommand());
    }
    api.StartFrame();
    rendererFrameSkipped = !api.HasActiveFrame();
    if (rendererFrameSkipped) {
      rendererFrameWasInactive.store(true);
      return;
    }
    picaPresentationScheduler.BeginPresentation(presentationSerial);
    api.StartDrawToFramebuffer(0, 1.0F);
    api.SetClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    api.ClearFramebuffer(true, true);
    api.SetViewport(0, 0, static_cast<int>(width), static_cast<int>(height));
    api.SetScissor(0, 0, static_cast<int>(width), static_cast<int>(height));
  };
  // A frame begin may recreate targets (and so read back GPU copies into
  // guest memory) after a resize or a graphics settings change, may apply
  // display settings (window operations), and an inactive frame must not
  // advance the guest: those begin synchronously, on this thread.
  // Otherwise the guest goes on while the renderer finishes the previous
  // frame.
  const auto renderSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance()
                                  .SnapshotWithRevision()
                                  .Revision;
  const bool renderNativePresentation =
      Fast::Oot3d::GraphicsSettingsRuntime::Instance()
          .NativePresentationOverrideActive();
  const bool presentationTransactionActive =
      Fast::Oot3d::GraphicsSettingsRuntime::Instance()
          .PresentationStatus()
          .Phase != Fast::Oot3d::PresentationTransactionPhase::Idle;
  const bool previousRendererFrameInactive =
      rendererFrameWasInactive.exchange(false);
  const bool synchronousFrameBegin =
      renderStream.CurrentMode() ==
          Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline ||
      presentationTransactionActive || previousRendererFrameInactive ||
      width != guestFrameBeginWidth ||
      height != guestFrameBeginHeight ||
      renderSettings != guestFrameBeginSettings ||
      renderNativePresentation != guestFrameBeginNativePresentation;
  guestFrameBeginWidth = width;
  guestFrameBeginHeight = height;
  guestFrameBeginSettings = renderSettings;
  guestFrameBeginNativePresentation = renderNativePresentation;
  bool rendererFrameActive = true;
  if (synchronousFrameBegin) {
    renderStream.Run(beginRendererFrame);
    // The renderer is idle after Run: its frame state is readable.
    rendererFrameActive = !rendererFrameSkipped;
  } else {
    renderStream.Submit(beginRendererFrame);
  }
  if (!rendererFrameActive) {
    // A minimized or changing surface is not a guest/PICA failure. Keep events
    // and UI balanced, but do not consume guest time without an acquired frame.
    if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
      throw std::runtime_error("cannot restore suspended presentation clock: " + error);
    }
    gui->EndDraw();
    renderStream.Submit([&] { window.EndFrame(); }, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
    lastPresentationTime = std::chrono::steady_clock::now();
    return true;
  }
  const auto preparationStatus = programPreparation != nullptr
      ? programPreparation->PreparationStatus()
      : Fast::Renderer3ds::PicaPreparationStatus{};
  if (preparationScreen.HoldsGuest(preparationStatus,
                                   std::chrono::steady_clock::now())) {
    // The guest waits for the initial preparation: its clock does not run
    // and the frame shows the progress instead of a game image.
    if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
      throw std::runtime_error("cannot restore the presentation clock during preparation: " + error);
    }
    preparationScreen.Draw(preparationStatus);
    gui->EndDraw();
    renderStream.Submit([&] { window.EndFrame(); }, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
    lastPresentationTime = std::chrono::steady_clock::now();
    lastCompletedFrameTime = lastPresentationTime;
    return true;
  }
  phaseTiming.RendererFrameStartSeconds += SecondsSince(rendererFrameStart);
  cpuRendererStart.Stop();
  phaseTiming.FrameStartSeconds += SecondsSince(phaseStart);
  {
    const auto startLockAfter = Oot3dNativeGame::SecondCore::SampleCore0Lock();
    untimed.StartLockTakes += startLockAfter.Takes - startLockBefore.Takes;
    untimed.StartLockWaits += startLockAfter.Waits - startLockBefore.Waits;
    untimed.StartLockWait +=
        static_cast<double>(startLockAfter.WaitNanoseconds - startLockBefore.WaitNanoseconds) / 1.0e9;
    const auto reacquireStart = std::chrono::steady_clock::now();
    hostWorkRelease.reset();
    untimed.KernelReacquire += SecondsSince(reacquireStart);
  }
  const uint32_t guestRefreshIterations =
      std::max<uint32_t>(1U, guestRefreshesDue);
  uint32_t guestRefreshesRun = 0U;
  for (uint32_t guestRefreshIndex = 0;
       guestRefreshIndex < guestRefreshIterations; ++guestRefreshIndex) {
    const bool advanceGuest = guestRefreshIndex < guestRefreshesDue;
    const bool presentHostFrame =
        guestRefreshIndex + 1U == guestRefreshIterations;
    if (advanceGuest) {
      const auto refreshPrepStart = std::chrono::steady_clock::now();
      if (!SampleGuestRefreshInput(
              guestRefreshIndex,
              nativeFrontendTouchEnabled,
              topScreenNativeGameMode,
              topScreenStartRoutingEligible,
              physicalInputFrame,
              inputFrame)) {
        break;
      }
      if (!UpdateGuestServicesForRefresh(topScreenStartRoutingEligible, inputFrame)) {
        break;
      }
      untimed.RefreshPrep += SecondsSince(refreshPrepStart);
      Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::GuestRefresh);
      RunGuestRefresh();
    }
    // Deferred DSP frames go into the renderer wait when there is one
    // (tour24: after the presentation they delayed the next host frame).
    if (deferredDspAudioFrames != 0U && !dspOnSecondCore &&
        renderStream.FrameSlotsFull()) {
      ++dspAudioInRenderWait;
      DeliverDspAudioFrames(std::exchange(deferredDspAudioFrames, 0U));
    }
    if (presentHostFrame) {
      Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::PresentVisual);
      PresentVisualFrame(presentationElapsedSeconds, visualInterpolationAlpha);
    }
    guestRefreshesRun += advanceGuest ? 1U : 0U;
    frameCount += advanceGuest ? 1U : 0U;
  }
  if (deferredDspAudioFrames != 0U && !dspOnSecondCore) {
    Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::GuestRefresh);
    DeliverDspAudioFrames(std::exchange(deferredDspAudioFrames, 0U));
  }
  if (perfOverlayEnabled) {
    // Game speed: refreshes run out of those owed this frame (the planned
    // ones plus the ones the scheduler dropped because the game fell behind).
    PerfOverlayState().ObserveRefreshes(guestRefreshesRun, guestRefreshesDue,
                                        presentationStep.DroppedGuestRefreshes);
  }

  if (launch.ExtendedDiagnostics) {
    inputConsumerDiagnostics.Observe(process.Memory(), frameCount);
  }

  // config/second_core: the UI submission and the presentation hand-off
  // read guest memory only (through paths that take the kernel lock) and
  // talk to the render thread: released as the frame start. With
  // config/kernel_ui_lock (absent = on) the UI submission keeps the lock
  // and the release starts at the presentation hand-off: one acquisition
  // instead of one per guest memory read, each of which Horizon may hand to
  // a waiting second core (oot3d_native_kernel_options.h).
  if (!uiSubmitHoldsKernel) hostWorkRelease.emplace();
  const auto uiSubmitStart = std::chrono::steady_clock::now();
  const double uiPrepareBefore = untimed.UiPrepare + untimed.UiAhead;
  const auto uiProviderBefore = nativeUiTextureProviderStorage->Stats();
  const auto uiLockBefore = Oot3dNativeGame::SecondCore::SampleCore0Lock();
  Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::UiSubmit);
  // The HUD's texture pack images decode before its first frame: its
  // textures are asked for ahead, one per frame, as soon as the game has
  // loaded them (title screen), and again at each new UI texture revision
  // (the pack's index ready, HD textures turned on or off).
  if (const auto revision = nativeUiTextureProviderStorage->Revision();
      hudTexturesAheadRevision != revision) {
    bool complete = false;
    const auto ahead = uiLifecycleBridge.HudTexturesAhead(
        launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen,
        &complete);
    if (n64UiRenderer.RequestAhead(ahead) && complete) {
      hudTexturesAheadRevision = revision;
    }
  }
  auto uiMark = std::chrono::steady_clock::now();
  untimed.UiAhead += std::chrono::duration<double>(uiMark - uiSubmitStart).count();
  const auto uiSplit = [&uiMark](double &total) {
    const auto now = std::chrono::steady_clock::now();
    total += std::chrono::duration<double>(now - uiMark).count();
    uiMark = now;
  };
  for (std::size_t subsystemIndex = 0;
       subsystemIndex < oot3d::ui::kUiSubsystemCount; ++subsystemIndex) {
    const auto subsystem =
        static_cast<oot3d::ui::UiSubsystem>(subsystemIndex);
    const bool topScreenProfile =
        launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
    if (topScreenProfile &&
        subsystem != oot3d::ui::UiSubsystem::GameplayHud &&
        subsystem != oot3d::ui::UiSubsystem::TouchControls) {
      continue;
    }
    const bool topScreenPresentation = topScreenProfile;
    if (!topScreenPresentation && !uiLifecycleBridge.Runtime()
                                       .PlanFrame(subsystem)
                                       .run_host_presentation) {
      continue;
    }
    auto primitives =
        topScreenPresentation
            ? uiLifecycleBridge.BuildTopScreenPresentation(subsystem)
            : uiLifecycleBridge.BuildShadowPresentation(subsystem);
    if (topScreenPresentation &&
        subsystem == oot3d::ui::UiSubsystem::GameplayHud) {
      Oot3dNativeGame::AppendTopScreenFreeCameraOptionPresentation(
          nativeCandidateDispatch.TopScreenCamera.Camera, primitives);
    }
    if (topScreenPresentation && subsystem == oot3d::ui::UiSubsystem::TouchControls) {
      ocarinaText.Append(primitives);
    }
    if (topScreenPresentation) {
      lastTopScreenPrimitivesBySubsystem[subsystemIndex] = primitives;
    }
    uiSplit(untimed.UiBuild);
    const auto canvasMode =
        topScreenPresentation
            ? oot3d::ui::N64UiCanvasMode::NativeTopScreen400x240
            : oot3d::ui::N64UiCanvasMode::Widescreen16x9;
    // Texture payloads are read from guest memory here; the renderer
    // only uploads them.
    auto preparedTextures = n64UiRenderer.PrepareTextures(primitives);
    uiSplit(untimed.UiPrepare);
    renderStream.Submit([&, primitives = std::move(primitives), width,
                         height, canvasMode,
                         prepared = std::move(preparedTextures)]() mutable {
      if (rendererFrameSkipped) {
        n64UiRenderer.Stage(std::move(prepared));
        return;
      }
      struct UiCost {
        double &Total;
        std::chrono::steady_clock::time_point Start =
            std::chrono::steady_clock::now();
        ~UiCost() { Total += SecondsSince(Start) * 1000.0; }
      } uiCost{renderFrameCpuMs};
      if (!n64UiRenderer.Render(primitives, width, height, canvasMode,
                                std::move(prepared), &renderError)) {
        throw std::runtime_error("integrated N64 UI presentation failed: " +
                                 renderError);
      }
    });
    uiSplit(untimed.UiQueue);
  }

  {
    const auto uiLockAfter = Oot3dNativeGame::SecondCore::SampleCore0Lock();
    untimed.UiLockTakes += uiLockAfter.Takes - uiLockBefore.Takes;
    untimed.UiLockWaits += uiLockAfter.Waits - uiLockBefore.Waits;
    untimed.UiLockWait +=
        static_cast<double>(uiLockAfter.WaitNanoseconds - uiLockBefore.WaitNanoseconds) / 1.0e9;
  }
  untimed.UiSubmit += SecondsSince(uiSubmitStart);
  {
    // A texture pack change in progress (deko3d.txt `texture pack change ...
    // game thread: ... frames, longest frame, longest HUD texture
    // preparation, over budget, HUD resolves, decoded`): this frame from the
    // last one's UI submission (waits for the render thread included), its
    // HUD texture preparation (asked ahead included) and the HUD textures it
    // resolved and decoded. Game thread only.
    static std::chrono::steady_clock::time_point previousUiSubmit;
    const auto micros = [](double seconds) { return static_cast<uint64_t>(seconds * 1.0e6); };
    const uint64_t frameMicros =
        previousUiSubmit.time_since_epoch().count() == 0
            ? 0U
            : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        uiSubmitStart - previousUiSubmit)
                                        .count());
    previousUiSubmit = uiSubmitStart;
    const auto &uiProvider = nativeUiTextureProviderStorage->Stats();
    auto &packChangeWatch = Oot3d::Renderer::LiveTexturePackChangeGameWatch();
    // The audio stream's stats take its lock: read only while a change runs.
    const uint64_t concealEvents = packChangeWatch.Active() && hostAudioStream != nullptr
                                       ? hostAudioStream->GetStats().ConcealEvents
                                       : ~0ULL;
    packChangeWatch.Observe(frameMicros, micros(untimed.UiPrepare + untimed.UiAhead - uiPrepareBefore),
                            uiProvider.resolve_calls - uiProviderBefore.resolve_calls,
                            uiProvider.identity_decodes - uiProviderBefore.identity_decodes, concealEvents);
  }
  if (uiSubmitHoldsKernel) hostWorkRelease.emplace();
  Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::PresentSubmit);
  phaseStart = std::chrono::steady_clock::now();
  CpuProbe::Scope cpuPresent(CpuProbe::Phase::Present);
  Fast::Renderer3ds::SlowStepTrace presentSteps("present");
  // The first frames render under the preparation screen.
  preparationScreen.Draw(preparationStatus);
  gui->EndDraw();
  presentSteps.Mark("gui_end_draw");
  renderStream.Submit([&, width, height,
                       presentationSerial = presentationFrameCount,
                       spikeGuestFrame = frameCount,
                       cadenceThirtyPair,
                       forcedScreenshot = std::exchange(pendingForcedScreenshot, {})] {
    if (!forcedScreenshot.empty()) screenshotState.ForcedPath = forcedScreenshot;
    if (rendererFrameSkipped) {
      capturedTemporalSample.reset();
      window.EndFrame();
      realtimePacer.ResetDeadline();
      ObserveFrameSpike(0U, false, 0.0, spikeGuestFrame, true);
      return;
    }
    MaybeWriteFramebufferScreenshot(
        hostArgs, api, width, height,
        static_cast<uint32_t>(presentationSerial), screenshotState,
        capturedTemporalSample ? &*capturedTemporalSample : nullptr);
    capturedTemporalSample.reset();
    const uint8_t presentedKind = renderFrameKind;
    FinishRenderFrameCost();
    const auto waitStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuPacing(CpuProbe::Phase::Pacing);
    Fast::GlPerf::Mark(Fast::GlPerf::Stage::Pacing);
    // Locked 30: a step two periods after the previous picture, and a late
    // one rebases the clock (hold 3 then 2, 2... instead of 3 then 1).
    const bool thirtyStep =
        cadenceThirtyPair &&
        presentedKind ==
            static_cast<uint8_t>(Oot3dNativeGame::RenderFrameKind::Step);
    {
      Oot3dNativeGame::HangWatch::StageScope hangPacer(
          Oot3dNativeGame::HangWatch::Stage::RenderPacer);
      if (earlyFrameSlot) {
        struct EarlySlot {
          NativeA32WindowSession* Session;
          uint64_t Serial;
        } early{this, presentationSerial};
        realtimePacer.WaitForNextRefresh(
            thirtyStep ? 2U : 1U, thirtyStep,
            [](void* user, std::chrono::steady_clock::time_point deadline) {
              auto& slot = *static_cast<EarlySlot*>(user);
              slot.Session->PublishPresentationDeadline(slot.Serial, deadline);
              slot.Session->renderStream.ReleaseFrameSlot();
            },
            &early);
      } else {
        realtimePacer.WaitForNextRefresh(thirtyStep ? 2U : 1U, thirtyStep);
      }
    }
    cpuPacing.Stop();
    const double pacerWaitSeconds = SecondsSince(waitStart);
    rendererTiming->PacingWaitSeconds += pacerWaitSeconds;
    const auto swapStart = std::chrono::steady_clock::now();
    {
      Oot3dNativeGame::HangWatch::StageScope hangSwap(
          Oot3dNativeGame::HangWatch::Stage::RenderSwap);
      window.EndFrame();
    }
    // An acquire inside the swap (a frame that never drew to the window) is
    // not part of the next frame's render-thread time.
    (void)RenderWaitClock().Take();
    lastSwapMs = SecondsSince(swapStart) * 1000.0;
    ObservePresentCadence(presentedKind);
    ObserveFrameSpike(presentedKind, thirtyStep, pacerWaitSeconds * 1000.0,
                      spikeGuestFrame, false);
  }, true);
  presentSteps.Mark("render_end_frame");
  picaSemanticTrace.RecordFrameBoundary(
      presentationFrameCount, frameCount, guestRefreshesDue != 0U);
  phaseTiming.PresentSeconds += SecondsSince(phaseStart);
  cpuPresent.Stop();
  {
    const auto reacquireStart = std::chrono::steady_clock::now();
    hostWorkRelease.reset();
    untimed.KernelReacquire += SecondsSince(reacquireStart);
  }
  ++presentationFrameCount;
  ++runFrameCount;
  Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::FrameEnd);
  if (guestProfiler.Active()) {
    // Guest profile by frame sampling: on measured frames the profile
    // receives every block; on the others the original callback returns with the
    // hook filter, and the game runs at normal speed.
    const bool sample = guestProfiler.OnHostFrame(runFrameCount);
    if (sample != guestProfilerInstalled && processStorage) {
      guestProfilerInstalled = sample;
      const bool forAll =
          launch.ProfileA32Blocks || widescreenProjection.TraceA32Blocks;
      if (sample) {
        processStorage->SetBlockEntryCallback(
            &Oot3dNativeGame::GuestBlockProfiler::OnBlockEntry, &guestProfiler,
            std::vector<uint32_t>{});
        nativeCandidateDispatch.BlockEntry =
            &Oot3dNativeGame::GuestBlockProfiler::OnBlockEntry;
        nativeCandidateDispatch.BlockEntryUser = &guestProfiler;
        nativeBlockEntryPcs = {};
      } else {
        processStorage->SetBlockEntryCallback(
            &ApplyNativeWidescreenProjectionPolicy, &widescreenProjection,
            forAll ? std::vector<uint32_t>{} : blockEntryHooks);
        nativeCandidateDispatch.BlockEntry = &ApplyNativeWidescreenProjectionPolicy;
        nativeCandidateDispatch.BlockEntryUser = &widescreenProjection;
        nativeBlockEntryPcs = forAll ? std::span<const uint32_t>{}
                                     : std::span<const uint32_t>{blockEntryHooks};
      }
      nativeCandidateDispatch.BlockEntryPcs = nativeBlockEntryPcs;
      nativeCandidateDispatch.MassAotBlockEntryFilter =
          Oot3dNativeGame::BuildOot3dMassAotPcFilter(nativeBlockEntryPcs,
                                                     nativeBlockEntryPcs.empty());
      guestProfiler.Interrupt();
    }
  }
#if defined(__SWITCH__)
  gSwitchRuntimeHostFrames = runFrameCount;
  gSwitchRuntimeGuestFrames = frameCount;
  // Physical-console profile: every 150 host frames, append the rate and the
  // per-frame milliseconds of each phase over that window to logs/perf.txt.
  {
    static auto windowStart = std::chrono::steady_clock::now();
    static std::array<double, 15> windowValues{};
    static uint64_t windowHostFrames = 0U;
    static uint64_t windowGuestFrames = 0U;
    static double windowGuestInPica = 0.0;
    if (runFrameCount - windowHostFrames >= 150U) {
      Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::PerfWindow);
      const auto now = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(now - windowStart).count();
      const auto values = phaseTiming.Values();
      const uint64_t hostFrames = runFrameCount - windowHostFrames;
      static constexpr std::array<const char*, 15> kNames{
          "guest", "frame_start", "host_frame_start", "input", "renderer_frame_start",
          "dsp_mix", "audio_out", "pica_submit", "pica_plan", "pica_backend",
          "pica_diag", "visual_present", "visual_replay", "present", "pacing_wait"};
      // The line is built in memory and written in one piece: by the
      // service thread (config/offload_perf) or here, as before.
      const auto perfLineStart = std::chrono::steady_clock::now();
      std::ostringstream perf;
      perf << "host_frames=" << runFrameCount << " fps=" << (hostFrames / seconds)
           << " guest_fps=" << ((frameCount - windowGuestFrames) / seconds) << " ms/frame:";
      for (size_t index = 0; index < values.size(); ++index) {
        perf << ' ' << kNames[index] << '='
             << ((values[index] - windowValues[index]) * 1000.0 / hostFrames);
      }
      // The guest resumed inside the PICA submissions (part of `guest`).
      perf << " guest_in_pica="
           << ((phaseTiming.GuestInsidePicaSubmitSeconds - windowGuestInPica) * 1000.0 / hostFrames)
           << " total_ms=" << (seconds * 1000.0 / hostFrames);
      // Where the game spent most of the window (oot3d_native_area.h).
      const auto area = perfAreaWindow.Dominant();
      const std::string areaText = Oot3dNativeGame::FormatNativeArea(area.Area);
      perf << ' ' << areaText << " scene_changes=" << area.SceneChanges;
      perfAreaWindow.Reset();
      // Clocks of this window (sys-clk IPC, read only; -1 when unknown): one
      // IPC per window, after `now`, so it is not billed to this window.
      {
        Oot3dNativeGame::HangWatch::StageScope hangClocks(
            Oot3dNativeGame::HangWatch::Stage::PerfClocks);
        perf << ' ' << Oot3dNativeGame::SwitchClockPerfSummary();
      }
      if (hostAudioStream != nullptr) {
        perf << ' ' << hostAudioStream->PerfSummary();
      }
      // Plan builds off the guest thread (deferred planning): ms per host
      // frame on the plan worker and on the render thread / dependency
      // flushes ("late": the worker had not reached them yet).
      {
        static Oot3dNativeGame::Oot3dPicaDeferredPlanStats windowPlanStats{};
        const auto planStats = picaPlanBuilder.Stats();
        const auto perFrameMs = [&](uint64_t now, uint64_t before) {
          return static_cast<double>(now - before) / 1.0e6 / hostFrames;
        };
        perf << " pica_plan_deferred=" << (picaPlanBuilder.Deferred() ? 1 : 0)
             << " plan_worker=" << (picaPlanBuilder.WorkerEnabled() ? 1 : 0)
             << " plan_worker_ms="
             << perFrameMs(planStats.WorkerBuildNanoseconds,
                           windowPlanStats.WorkerBuildNanoseconds)
             << " plan_late_ms="
             << perFrameMs(planStats.ConsumerBuildNanoseconds,
                           windowPlanStats.ConsumerBuildNanoseconds)
             << " plan_late_draws="
             << (planStats.ConsumerBuilds - windowPlanStats.ConsumerBuilds)
             << " plan_serial_ms="
             << perFrameMs(planStats.SerialBuildNanoseconds,
                           windowPlanStats.SerialBuildNanoseconds)
             // Must stay small and constant (recycled batches).
             << " plan_batches_live=" << planStats.LiveBatches
             << " plan_batches_queued=" << planStats.QueuedBatches
             << " plan_batches_allocated=" << planStats.AllocatedBatches
             // Build-mutex contention, ms per host frame and count: render
             // thread (resolves) and worker; resolve_locks = acquisitions by
             // the resolves (runs of up to 32 draws, not one per draw).
             << " plan_lock_wait_render_ms="
             << perFrameMs(planStats.ConsumerLockWaitNanoseconds,
                           windowPlanStats.ConsumerLockWaitNanoseconds)
             << " plan_lock_waits_render="
             << (planStats.ConsumerLockWaits - windowPlanStats.ConsumerLockWaits)
             << " plan_lock_wait_worker_ms="
             << perFrameMs(planStats.WorkerLockWaitNanoseconds,
                           windowPlanStats.WorkerLockWaitNanoseconds)
             << " plan_resolve_locks="
             << (planStats.ConsumerLocks - windowPlanStats.ConsumerLocks)
             << " plan_worker_idle_ms="
             << perFrameMs(planStats.WorkerIdleNanoseconds,
                           windowPlanStats.WorkerIdleNanoseconds);
        // Plan build cost per draw (config/plan_full_identity,
        // config/plan_copy_build, config/plan_one_lock): the options in use,
        // the worker's builds per host frame, the mean build time per draw
        // (worker and late builds) and, with parallel builds, how often and
        // how long (ms per host frame) a resolve waited for a slot the
        // worker was still building.
        {
          const auto options = picaPlanBuilder.Options();
          const uint64_t builds =
              (planStats.WorkerBuilds - windowPlanStats.WorkerBuilds) +
              (planStats.ConsumerBuilds - windowPlanStats.ConsumerBuilds);
          const uint64_t buildNanoseconds =
              (planStats.WorkerBuildNanoseconds -
               windowPlanStats.WorkerBuildNanoseconds) +
              (planStats.ConsumerBuildNanoseconds -
               windowPlanStats.ConsumerBuildNanoseconds);
          perf << " plan_identity=" << (options.CanonicalIdentity ? 1 : 0)
               << " plan_in_place=" << (options.InPlace ? 1 : 0)
               << " plan_parallel=" << (picaPlanBuilder.Parallel() ? 1 : 0)
               << " plan_worker_draws="
               << static_cast<double>(planStats.WorkerBuilds -
                                      windowPlanStats.WorkerBuilds) /
                      hostFrames
               << " plan_us_draw="
               << (builds != 0U ? static_cast<double>(buildNanoseconds) /
                                      1.0e3 / static_cast<double>(builds)
                                : 0.0)
               << " plan_slot_waits="
               << (planStats.ConsumerSlotWaits - windowPlanStats.ConsumerSlotWaits)
               << " plan_slot_wait_ms="
               << perFrameMs(planStats.ConsumerSlotWaitNanoseconds,
                             windowPlanStats.ConsumerSlotWaitNanoseconds);
        }
        windowPlanStats = planStats;
      }
      // Waits between threads, ms per host frame: the guest blocked by the
      // frames-in-flight limit (renderer behind) or in Drain/Run (synchronous
      // renderer work), the render thread without commands (guest behind),
      // and the pacer's tail (spin = busy-wait, oversleep = woke late).
      {
        static Oot3dNativeGame::NativeRenderCommandStream::WaitStats previousWaits{};
        static Oot3dNativeGame::NativePacerTailCounters previousTail{};
        const auto waits = renderStream.GetWaitStats();
        const auto tail = realtimePacer.TailCounters();
        const auto perFrameMs = [&](uint64_t now, uint64_t before) {
          return static_cast<double>(now - before) / 1.0e6 / hostFrames;
        };
        perf << " sync: guest_wait_frames_ms="
             << perFrameMs(waits.SubmitWaitNanoseconds, previousWaits.SubmitWaitNanoseconds)
             << " guest_wait_frames_n=" << (waits.SubmitWaits - previousWaits.SubmitWaits)
             << " guest_wait_drain_ms="
             << perFrameMs(waits.DrainWaitNanoseconds, previousWaits.DrainWaitNanoseconds)
             << " guest_wait_drain_n=" << (waits.DrainWaits - previousWaits.DrainWaits)
             << " render_idle_ms="
             << perFrameMs(waits.WorkerIdleNanoseconds, previousWaits.WorkerIdleNanoseconds)
             << " signals_skipped=" << (waits.SkippedNotifies - previousWaits.SkippedNotifies)
             // Submissions that woke the idle render thread, per host frame.
             << " render_wakes="
             << static_cast<double>(waits.WorkerWakes - previousWaits.WorkerWakes) / hostFrames
             << " pacer_spin_ms="
             << perFrameMs(tail.SpinNanoseconds, previousTail.SpinNanoseconds)
             << " pacer_oversleep_ms="
             << perFrameMs(tail.OversleepNanoseconds, previousTail.OversleepNanoseconds)
             << " pacer_oversleep_max_ms="
             << static_cast<double>(realtimePacer.TakeMaximumOversleepNanoseconds()) / 1.0e6
             << " early_slot=" << (earlyFrameSlot ? 1 : 0)
             << " early_slot_releases="
             << (waits.EarlyFrameSlotReleases - previousWaits.EarlyFrameSlotReleases)
             << " early_slot_clock_frames=" << earlySlotClockFrames
             << ' ' << Oot3dNativeGame::SwitchThreadPerfSummary()
             // Hang watchdog (logs/hang.txt): on, and reports written.
             << " hang_wd=" << (Oot3dNativeGame::HangWatch::Running() ? 1 : 0)
             << " hang_reports=" << Oot3dNativeGame::HangWatch::ReportsWritten();
#if defined(__SWITCH__)
        // Guard pages (config/guard_alloc): blocks made, live, quarantined.
        if (const auto guard = Oot3dNativeGame::GuardAlloc::CurrentStats(); guard.Enabled) {
          perf << " guard_alloc=1 guard_made=" << guard.Guarded
               << " guard_live=" << guard.Live
               << " guard_quarantine=" << guard.Quarantined
               << " guard_fallbacks=" << guard.Fallbacks
               << " guard_skipped=" << guard.Skipped
               << " guard_canary=" << (guard.Canary ? 1 : 0)
               << " guard_canary_hits=" << guard.CanaryHits
               << " guard_double_frees=" << guard.DoubleFrees;
        }
#endif
        previousWaits = waits;
        previousTail = tail;
      }
      // Paced interpolation (config/fps60) and frame costs: frames of each
      // kind in the window, their GPU ms (GL_TIMESTAMP; gpu_timer=0 until
      // the clock is calibrated) and render-thread ms, the governor's level
      // and estimates, and the native replays skipped.
      {
        static NativeA32WindowSession::InterpolationPerfCounters previous{};
        NativeA32WindowSession::InterpolationPerfCounters current;
        {
          std::lock_guard lock(interpolationPerfMutex);
          current = interpolationPerf;
        }
        const auto frames = [&](size_t kind) {
          return current.Frames[kind] - previous.Frames[kind];
        };
        perf << " interp: pacing=" << (interpolationPacingOptions.KeepGameSpeed ? 1 : 0)
             << " interp_scale=" << current.ReducedScale
             << " level=" << Oot3dNativeGame::InterpolatedSampleLevelName(
                    static_cast<Oot3dNativeGame::InterpolatedSampleLevel>(current.Level))
             << " step_frames=" << frames(1)
             << " interp_frames=" << frames(2)
             << " interp_reduced=" << frames(3)
             << " interp_dropped=" << frames(4)
             // New images per second (steps and in-betweens; fps counts
             // refreshes, so a held image counts there twice).
             << " new_fps=" << ((frames(1) + frames(2) + frames(3)) / seconds)
             << " other_frames=" << frames(0)
             << " reduced_refused=" << (current.ReducedRefused - previous.ReducedRefused)
             << " repeat_fallbacks=" << (current.RepeatFallbacks - previous.RepeatFallbacks)
             << " native_replay_skipped=" << (current.NativeReplaysSkipped - previous.NativeReplaysSkipped)
             << " probes=" << (current.Probes - previous.Probes)
             // config/pacer_entry_recover: probes whose pair did not fit,
             // probes of an entry's shorter interval, scene entries, the
             // time below full in the entry windows (s: in-between
             // decisions x the pair) and the probe interval now (60 doubled
             // per failed probe in a row); render-thread holds left out of
             // the frame cost (frames, ms) and the frames whose entry work
             // (pack decodes, grass placements) was still running;
             // stale_probes: probes made at once for a stale full-frame
             // ratio (InterpolationPacingGovernor::StaleRatio).
             << " entry_recover=" << (interpolationPacingOptions.PacerEntryRecover ? 1 : 0)
             << " failed_probes=" << (current.FailedProbes - previous.FailedProbes)
             << " entry_probes=" << (current.EntryProbes - previous.EntryProbes)
             << " stale_probes=" << (current.StaleProbes - previous.StaleProbes)
             << " entries=" << (current.Entries - previous.Entries)
             << " entry_30_s="
             << static_cast<double>(current.EntryBelowDecisions - previous.EntryBelowDecisions) *
                    current.BudgetMs / 1000.0
             << " probe_interval=" << current.ProbeInterval
             << " held_frames=" << (current.HoldFrames - previous.HoldFrames)
             << " held_ms=" << (current.HoldMs - previous.HoldMs)
             << " entry_work_frames=" << (current.EntryWorkFrames - previous.EntryWorkFrames)
             << " level_changes=" << (current.LevelChanges - previous.LevelChanges)
             << " gpu_timer=" << (current.GpuTimer ? 1 : 0)
             << " gpu_ns_per_tick=" << current.GpuFactor;
        for (size_t kind = 0U; kind < Oot3dNativeGame::kRenderFrameKindCount; ++kind) {
          const uint64_t gpuFrames = current.GpuFrames[kind] - previous.GpuFrames[kind];
          if (gpuFrames != 0U) {
            perf << " gpu_ms_" << Oot3dNativeGame::kRenderFrameKindNames[kind] << '='
                 << ((current.GpuMs[kind] - previous.GpuMs[kind]) / gpuFrames);
          }
          if (frames(kind) != 0U) {
            perf << " rt_ms_" << Oot3dNativeGame::kRenderFrameKindNames[kind] << '='
                 << ((current.CpuMs[kind] - previous.CpuMs[kind]) / frames(kind))
                 // Part of rt_ms_ blocked at the window acquire (not costed
                 // with config/interp_cpu_exclude_wait).
                 << " rt_wait_ms_" << Oot3dNativeGame::kRenderFrameKindNames[kind] << '='
                 << ((current.WaitMs[kind] - previous.WaitMs[kind]) / frames(kind));
          }
        }
        // Visual-frame preparation before the first draw (matching and
        // analysis of the new guest frame) and its samples, ms per host frame.
        const auto visualMs = [&](uint64_t now, uint64_t before) {
          return static_cast<double>(now - before) / 1.0e6 / hostFrames;
        };
        perf << " rt_prepare_ms="
             << visualMs(current.VisualTiming.MatchingNanoseconds +
                             current.VisualTiming.AnalysisNanoseconds,
                         previous.VisualTiming.MatchingNanoseconds +
                             previous.VisualTiming.AnalysisNanoseconds)
             << " rt_match_ms="
             << visualMs(current.VisualTiming.MatchingNanoseconds,
                         previous.VisualTiming.MatchingNanoseconds)
             << " rt_sample_ms="
             << visualMs(current.VisualTiming.SamplingNanoseconds,
                         previous.VisualTiming.SamplingNanoseconds);
        // Draws without a partner (config/interp_exact_step,
        // config/interp_carry_unmatched): per transition in the window.
        {
          const auto &now = current.Unmatched;
          const auto &before = previous.Unmatched;
          const auto transitions = now.Transitions - before.Transitions;
          const auto perTransition = [&](uint64_t a, uint64_t b) {
            return transitions != 0U ? static_cast<double>(a - b) / transitions
                                     : 0.0;
          };
          const auto options = preparedVisualTransition.Options();
          perf << " exact_step=" << (options.ExactStep ? 1 : 0)
               << " carry_unmatched=" << (options.CarryUnmatched ? 1 : 0)
               << " transitions=" << transitions
               << " trans_unmatched=" << (now.WithUnmatched - before.WithUnmatched)
               << " unmatched_prev=" << perTransition(now.UnmatchedPrevious, before.UnmatchedPrevious)
               << " unmatched_cur=" << perTransition(now.UnmatchedCurrent, before.UnmatchedCurrent)
               << " carried_cur=" << perTransition(now.CarriedCurrent, before.CarriedCurrent)
               << " inserted_prev=" << perTransition(now.InsertedPrevious, before.InsertedPrevious)
               << " unmatched_no_camera=" << perTransition(now.WithoutCamera, before.WithoutCamera)
               << " camera_measured=" << (now.CameraMeasured - before.CameraMeasured)
               << " discontinuities=" << (now.Discontinuities - before.Discontinuities);
        }
        perf << " est_step_ms=" << current.StepCostMs
             << " est_full_ms=" << current.FullEstimateMs
             << " est_reduced_ms=" << current.ReducedEstimateMs
             << " pair_budget_ms=" << current.BudgetMs
             << " decision_budget_ms=" << current.DecisionBudgetMs;
        // Dynamic resolution (config/dynamic_resolution): on, the scale now
        // and its lowest, the mean scale of the presented top-screen frames
        // in the window, the controller's pair average (GPU ms) and its
        // changes (down/up, ups undone soon) in the window.
        {
          static uint64_t previousPresented = 0U, previousMilli = 0U,
                          previousApplied = 0U, previousAppliedDraw = 0U;
          const auto &backendScale = Fast::PicaGlDynamicScale::gState;
          const uint64_t presented =
              backendScale.PresentedFrames.load(std::memory_order_relaxed);
          const uint64_t milli =
              backendScale.PresentedScaleMilli.load(std::memory_order_relaxed);
          const uint64_t applied =
              backendScale.Applied.load(std::memory_order_relaxed);
          const uint64_t appliedDraw =
              backendScale.AppliedAtDraw.load(std::memory_order_relaxed);
          const uint64_t presentedWindow = presented - previousPresented;
          perf << " drs=" << (current.DrsActive ? 1 : 0)
               << " drs_scale=" << current.DrsScale
               << " drs_min=" << current.DrsMinScale << " drs_mode="
               << Oot3dNativeGame::DynamicResolutionSixtyModeName(current.DrsMode)
               << " drs_scale_avg="
               << (presentedWindow != 0U
                       ? static_cast<double>(milli - previousMilli) /
                             1000.0 / static_cast<double>(presentedWindow)
                       : 0.0)
               << " drs_pair_ms=" << current.DrsPairMs
               << " drs_changes=" << (current.Drs.Changes - previous.Drs.Changes)
               << " drs_downs=" << (current.Drs.Downs - previous.Drs.Downs)
               << " drs_ups=" << (current.Drs.Ups - previous.Drs.Ups)
               << " drs_bounces=" << (current.Drs.Bounces - previous.Drs.Bounces)
               << " drs_applied=" << (applied - previousApplied)
               << " drs_applied_draw=" << (appliedDraw - previousAppliedDraw)
               << " drs_shed=" << (current.Drs.Shedding - previous.Drs.Shedding)
               << " drs_exempt="
               << backendScale.ExemptTargets.load(std::memory_order_relaxed);
          previousAppliedDraw = appliedDraw;
          previousPresented = presented;
          previousMilli = milli;
          previousApplied = applied;
        }
        previous = current;
      }
      // Presentation cadence (config/fps60_mode, oot3d_native_cadence_governor.h),
      // per window: presents 1/2/3/4+ refreshes after the previous one, how
      // many refreshes each new picture stayed (hold_), repeats of the same
      // picture, changes of hold between consecutive pictures, late steps,
      // guest frames never shown, the lock (60/30) and its changes, ms at
      // 30, and the game thread's step pair (mean and p90 of the last 64).
      {
        static Oot3dNativeGame::CadenceStats previousCadence{};
        static uint64_t previousSuperseded = 0U;
        Oot3dNativeGame::CadenceStats cadence;
        bool thirty = false;
        double holdSeconds = 0.0;
        double gameP90 = 0.0;
        uint64_t superseded = 0U;
        uint32_t cadenceRateHz = 0U;
        auto cadenceMode = Oot3dNativeGame::Fps60Mode::Legacy;
        {
          std::lock_guard lock(interpolationPerfMutex);
          cadence = interpolationPerf.Cadence;
          thirty = interpolationPerf.CadenceThirty;
          holdSeconds = interpolationPerf.CadenceHoldSeconds;
          gameP90 = interpolationPerf.GamePairP90Ms;
          superseded = interpolationPerf.SupersededFrames;
          cadenceRateHz = interpolationPerf.RateHz;
          cadenceMode = interpolationPerf.CadenceMode;
        }
        const auto delta = [](uint64_t now, uint64_t before) {
          return now - before;
        };
        static constexpr std::array<const char *, 4> kBuckets{"1", "2", "3",
                                                              "4p"};
        // fps60_mode: the mode in effect (legacy at 30); rate_hz: the
        // presentation rate at the end of the window (L3+R3 > Video > FPS).
        perf << " cadence: fps60_mode=" << Oot3dNativeGame::Fps60ModeName(cadenceMode)
             << " rate_hz=" << cadenceRateHz << " lock=" << (thirty ? 30 : 60);
        for (size_t bucket = 0U; bucket < kBuckets.size(); ++bucket)
          perf << " present_" << kBuckets[bucket] << '='
               << delta(cadence.Intervals[bucket],
                        previousCadence.Intervals[bucket]);
        for (size_t bucket = 0U; bucket < kBuckets.size(); ++bucket)
          perf << " hold_" << kBuckets[bucket] << '='
               << delta(cadence.Holds[bucket], previousCadence.Holds[bucket]);
        const uint64_t pairs = cadence.GamePairs - previousCadence.GamePairs;
        perf << " repeats=" << delta(cadence.Repeats, previousCadence.Repeats)
             << " cadence_switches="
             << delta(cadence.CadenceSwitches, previousCadence.CadenceSwitches)
             << " late_steps="
             << delta(cadence.LateSteps, previousCadence.LateSteps)
             << " superseded_frames=" << (superseded - previousSuperseded)
             << " lock_changes="
             << delta(cadence.LockChanges, previousCadence.LockChanges)
             << " lock30_ms="
             << (cadence.ThirtySeconds - previousCadence.ThirtySeconds) * 1000.0
             << " hold_s=" << holdSeconds << " game_pair_ms="
             << (pairs != 0U ? (cadence.GamePairMsSum -
                                previousCadence.GamePairMsSum) /
                                   static_cast<double>(pairs)
                             : 0.0)
             << " game_pair_p90_ms=" << gameP90;
        // Late presentations by cause since the start (logs/hitches.txt).
        for (size_t cause = 0U; cause < hitchCounts.size(); ++cause)
          perf << " hitch_"
               << Oot3dNativeGame::HitchCauseName(
                      static_cast<Oot3dNativeGame::HitchCause>(cause))
               << '=' << hitchCounts[cause].load(std::memory_order_relaxed);
        // Frames that lost their vsync in this window (logs/spikes.txt):
        // refreshes lost, by the stage that went over budget.
        if (frameSpikesEnabled) {
          static Oot3dNativeGame::FrameSpikeTotals::Snapshot previousSpikes;
          static uint64_t previousHolds = 0U;
          const auto spikes = spikeTotals.Take();
          const uint64_t holds = spikeHoldPresents.load(std::memory_order_relaxed);
          perf << " missed_refresh="
               << (spikes.MissedRefreshes - previousSpikes.MissedRefreshes)
               << " missed_frames="
               << (spikes.MissedFrames - previousSpikes.MissedFrames);
          for (size_t stage = 0U; stage < Oot3dNativeGame::kSpikeStageCount; ++stage)
            perf << " missed_by_stage_"
                 << Oot3dNativeGame::SpikeStageName(
                        static_cast<Oot3dNativeGame::SpikeStage>(stage))
                 << '='
                 << (spikes.MissedByStage[stage] - previousSpikes.MissedByStage[stage]);
          perf << " hold_presents=" << (holds - previousHolds)
               << " spikes_dropped=" << spikeRing.Dropped();
          const auto vsync = vsyncClock.Read();
          const auto tail = realtimePacer.TailCounters();
          perf << " vsync_clock=" << (vsync.Valid ? 1 : 0)
               << " vsync_period_us=" << (vsync.PeriodNs / 1000.0)
               << " vsync_resets=" << vsync.Resets
               << " pacer_vsync=" << (pacerVsyncConfigured ? 1 : 0)
               << " pacer_vsync_waits=" << tail.VsyncAnchoredWaits
               << " pacer_vsync_snap_max_us="
               << (tail.MaximumVsyncSnapNanoseconds / 1000U);
          previousSpikes = spikes;
          previousHolds = holds;
          DrainFrameSpikes();
        }
        perf << " audio_deferred=" << (deferDspAudio ? 1 : 0)
             << " audio_in_render_wait=" << dspAudioInRenderWait;
        // Textures the plan worker decoded ahead (hit) or the render thread
        // still decoded itself (miss), since the start.
        {
          const auto predecode =
              Fast::Oot3d::SharedPicaPredecodeCache().Snapshot();
          perf << " predecode_hit=" << predecode.Hits
               << " predecode_miss=" << predecode.Misses
               << " predecode_evicted=" << predecode.Evicted
               << " predecode_kb=" << (predecode.Bytes >> 10U);
        }
        previousCadence = cadence;
        previousSuperseded = superseded;
      }
      // GPU residency and memory: every live count and byte total must stay
      // bounded over a long session (a texture cache that never released
      // ran the console out of memory after ~8 minutes).
      {
        constexpr double kMb = 1024.0 * 1024.0;
        const auto gpu = api.GetNativePicaBackendStats();
        perf << " gl_textures_live=" << gpu.TexturesLive
             << " gl_tex_mb=" << (gpu.TextureBytes / kMb)
             << " gl_tex_budget_mb=" << (gpu.TextureBudgetBytes / kMb)
             << " gl_tex_created=" << gpu.TexturesCreated
             << " gl_tex_evicted=" << gpu.TexturesEvicted
             << " gl_luts_live=" << gpu.LutsLive
             << " gl_lut_mb=" << (gpu.LutBytes / kMb)
             << " gl_programs=" << gpu.ProgramsLive
             << " gl_targets=" << gpu.TargetsLive
             << " gl_displays=" << gpu.DisplaysLive
             << " gl_buffers_live=" << gpu.GeometryBuffersLive
             << " gl_buffer_mb=" << (gpu.GeometryBufferBytes / kMb)
             << " gl_geometry_entries=" << gpu.GeometryRegistryEntries
             << " gl_ring_orphans=" << gpu.StreamRingOrphans
             << " gl_oom=" << gpu.OutOfMemoryEvents
             << " gl_draws_skipped=" << gpu.SkippedDraws;
        // config/skip_bottom_screen: samples by decision (cumulative).
        if (picaPresentationScheduler.SkipBottomScreen()) {
          const auto bottom = picaPresentationScheduler.TakeBottomSkipCounters();
          perf << " bottom_skip: draws=" << bottom.SkippedDraws
               << " missing_sources=" << bottom.MissingSources;
          for (size_t reason = 0; reason < bottom.Samples.size(); ++reason) {
            perf << ' '
                 << Oot3dNativeGame::BottomSkipReasonName(
                        static_cast<Oot3dNativeGame::BottomSkipReason>(reason))
                 << '=' << bottom.Samples[reason];
          }
        }
        // Lock-free arena every window. The bytes in use need mallinfo(),
        // which holds the malloc lock shared with Mesa and walks the heap
        // (a corrupted heap crashes there): opt-in only, with
        // config/heap_stats or config/gpu_timing, every 20th window.
        static uint32_t heapWindows = 0U;
        static const bool heapStats =
            std::ifstream("sdmc:/switch/oot3dre/config/heap_stats").good() ||
            std::ifstream("sdmc:/switch/oot3dre/config/gpu_timing").good();
        if (&__malloc_current_mallinfo != nullptr) {
          const volatile size_t& arena = __malloc_current_mallinfo.arena;
          perf << " heap_arena_mb=" << (static_cast<double>(arena) / kMb);
        }
        if (&__malloc_max_sbrked_mem != nullptr) {
          const volatile size_t& peak = __malloc_max_sbrked_mem;
          perf << " heap_arena_peak_mb=" << (static_cast<double>(peak) / kMb);
        }
        // config/heap_check_windows (1-1000): walk the heap (mallinfo) every
        // N windows; a corruption is then found within ~N*5 s of when it
        // happened. Also enabled every 20 windows by config/heap_stats or
        // config/gpu_timing. Absent: never (mallinfo holds the malloc lock and
        // is what trips over a damaged heap).
        static const uint32_t heapCheckWindows = [] {
          std::ifstream input("sdmc:/switch/oot3dre/config/heap_check_windows");
          long long value = 0;
          return input && (input >> value) && value >= 1 && value <= 1000
                     ? static_cast<uint32_t>(value)
                     : 0U;
        }();
        const uint32_t heapEvery = heapCheckWindows != 0U ? heapCheckWindows : 20U;
        // Not mallinfo(): with a corrupted free list (a double free leaves
        // p->bk == p) its walk never ends while it holds the malloc lock, and
        // the whole process freezes without a crash report (switch-logs/
        // crash1, 2026-10-17). The bounded walk checks every link and reports
        // instead (oot3d_heap_bin_check.h); after a corruption it stops.
        static bool heapCorrupt = false;
        if ((heapStats || heapCheckWindows != 0U) && !heapCorrupt &&
            heapWindows++ % heapEvery == 0U) {
          uint64_t usedBytes = 0U;
          Oot3dNativeGame::HeapBinCheckResult heap;
          {
            Oot3dNativeGame::HangWatch::StageScope hangHeap(
                Oot3dNativeGame::HangWatch::Stage::PerfHeapWalk);
            heap = Oot3dNativeGame::CheckSwitchMallocHeap(&usedBytes);
          }
          perf << " heap_used_mb=" << (static_cast<double>(usedBytes) / kMb)
               << " heap_free_chunks=" << heap.FreeChunks;
          if (!heap.Ok) {
            heapCorrupt = true;
            char details[512];
            Oot3dNativeGame::HangWatch::Text text(details, sizeof(details));
            text.Str("heap_corrupt=").Str(heap.Reason).Str(" bin=").Dec(heap.Bin)
                .Str(" chunk=").Hex(heap.Chunk).Str(" size=").Hex(heap.ChunkSize)
                .Str(" fd=").Hex(heap.ChunkFd).Str(" bk=").Hex(heap.ChunkBk)
                .Str(" steps=").Dec(heap.Steps).Str(" host_frames=").Dec(runFrameCount);
            perf << ' ' << text.Data();
            Oot3dNativeGame::HangWatch::ReportEvent(
                "HEAP CORRUPTED (malloc bin walk)", text.Data());
          }
        }
        uint64_t usedMemory = 0U;
        uint64_t totalMemory = 0U;
        if (R_SUCCEEDED(svcGetInfo(&usedMemory, InfoType_UsedMemorySize,
                                   CUR_PROCESS_HANDLE, 0)) &&
            R_SUCCEEDED(svcGetInfo(&totalMemory, InfoType_TotalMemorySize,
                                   CUR_PROCESS_HANDLE, 0))) {
          perf << " proc_used_mb=" << (usedMemory / kMb)
               << " proc_total_mb=" << (totalMemory / kMb);
        }
      }
      // OpenGL PICA draw path (config/gl_stats), per host frame: draws,
      // fixed-function GL calls issued and dropped by the state cache
      // (config/gl_state_cache), binds, uniform uploads and render-thread
      // time inside SubmitPicaDraw.
      {
        const auto gl = api.GetNativePicaBackendStats();
        if (gl.GlStatsEnabled) {
          static decltype(api.GetNativePicaBackendStats()) windowGl{};
          if (gl.PicaDraws < windowGl.PicaDraws ||
              gl.PicaPresents < windowGl.PicaPresents) {
            windowGl = {}; // backend state was recreated
          }
          const auto perFrame = [&](uint64_t now, uint64_t before) {
            return static_cast<double>(now - before) / hostFrames;
          };
          const uint64_t draws = gl.PicaDraws - windowGl.PicaDraws;
          const uint64_t issued =
              gl.GlStateCallsIssued - windowGl.GlStateCallsIssued;
          const uint64_t skipped =
              gl.GlStateCallsSkipped - windowGl.GlStateCallsSkipped;
          const uint64_t nanoseconds =
              gl.PicaDrawNanoseconds - windowGl.PicaDrawNanoseconds;
          perf << " gl_cache=" << (gl.GlStateCacheEnabled ? 1 : 0)
               << " gl_draws=" << perFrame(gl.PicaDraws, windowGl.PicaDraws)
               << " gl_presents="
               << perFrame(gl.PicaPresents, windowGl.PicaPresents)
               << " gl_state_calls=" << static_cast<double>(issued) / hostFrames
               << " gl_state_skipped="
               << static_cast<double>(skipped) / hostFrames
               << " gl_state_calls_per_draw="
               << (draws != 0U ? static_cast<double>(issued) / draws : 0.0)
               << " gl_program_binds="
               << perFrame(gl.GlProgramBinds, windowGl.GlProgramBinds)
               << " gl_texture_binds="
               << perFrame(gl.GlTextureBinds, windowGl.GlTextureBinds)
               << " gl_vao_binds="
               << perFrame(gl.GlVertexArrayBinds, windowGl.GlVertexArrayBinds)
               << " gl_fb_binds="
               << perFrame(gl.GlFramebufferBinds, windowGl.GlFramebufferBinds)
               << " gl_ubo_uploads="
               << perFrame(gl.PublishedUniformUploads,
                           windowGl.PublishedUniformUploads)
               << " gl_draw_ms="
               << static_cast<double>(nanoseconds) / 1.0e6 / hostFrames
               << " gl_us_per_draw="
               << (draws != 0U ? static_cast<double>(nanoseconds) / 1.0e3 / draws
                               : 0.0);
          windowGl = gl;
        }
      }
      // Work taken off the game thread (config/offload_*), per host frame:
      // the guest thread's time inside the draw submissions (part of
      // `guest`: decode, vertex/index/texture snapshots, hashes) and its
      // bytes, pica_submit without the guest resumed inside it, and the
      // game-thread cost of the previous perf line (format + write/enqueue).
      {
        static Oot3dNativeGame::Oot3dNativePicaSubmissionQueue::CaptureCounters
            previousCapture{};
        static double previousGuestInPica = 0.0;
        static double previousPicaSubmit = 0.0;
        static uint64_t previousWriterNanoseconds = 0U;
        const auto capture = submissionQueueStorage->Counters();
        const double picaSubmitSeconds = phaseTiming.PicaSubmitSeconds;
        perf << " offload: offload_capture=" << (offload.Capture ? 1 : 0)
             << " offload_plan=" << (offload.Plan ? 1 : 0)
             << " offload_perf=" << (perfWriter.has_value() ? 1 : 0)
             << " offload_actors=" << (offload.Actors ? 1 : 0)
             << " pica_capture_ms="
             << static_cast<double>(capture.Nanoseconds - previousCapture.Nanoseconds) /
                    1.0e6 / hostFrames
             << " pica_capture_draws="
             << static_cast<double>(capture.Draws - previousCapture.Draws) / hostFrames
             << " capture_copy_kb="
             << static_cast<double>(capture.CopiedBytes - previousCapture.CopiedBytes) /
                    1024.0 / hostFrames
             << " capture_hash_kb="
             << static_cast<double>(capture.HashedBytes - previousCapture.HashedBytes) /
                    1024.0 / hostFrames
             << " index_range_reuses="
             << static_cast<double>(capture.IndexRangeReuses -
                                    previousCapture.IndexRangeReuses) /
                    hostFrames
             << " geometry_misses="
             << static_cast<double>(capture.GeometryMisses - previousCapture.GeometryMisses) /
                    hostFrames
             << " geometry_evictions="
             << static_cast<double>(capture.GeometryEvictions -
                                    previousCapture.GeometryEvictions) /
                    hostFrames
             << " pica_submit_host_ms="
             << ((picaSubmitSeconds - previousPicaSubmit) -
                 (phaseTiming.GuestInsidePicaSubmitSeconds - previousGuestInPica)) *
                    1000.0 / hostFrames
             << " perf_line_ms=" << perfLineGameMs;
        if (perfWriter.has_value()) {
          const uint64_t writer = perfWriter->WriteNanoseconds();
          perf << " perf_writer_ms="
               << static_cast<double>(writer - previousWriterNanoseconds) / 1.0e6
               << " perf_dropped=" << perfWriter->Dropped();
          previousWriterNanoseconds = writer;
        }
        // boot-status.txt heartbeat (config/async_status_writes): ms this
        // thread spent on it in the window (format + post, or the whole SD
        // replace with the switch at 0), the service thread's write ms, and
        // the hitches.txt writer's (the render thread only queues its lines).
        {
          static uint64_t previousStatusGame = 0U, previousStatusWriter = 0U,
                          previousHitchWriter = 0U;
          const auto status = Oot3dNativeGame::SwitchBootStatusFile().Stats();
          perf << " status_async=" << (asyncStatusWrites ? 1 : 0)
               << " status_updates=" << statusUpdates
               << " status_game_ms="
               << static_cast<double>(statusGameNanoseconds - previousStatusGame) / 1.0e6
               << " status_writer_ms="
               << static_cast<double>(status.WriteNanoseconds - previousStatusWriter) / 1.0e6
               << " status_superseded=" << status.Superseded;
          previousStatusGame = statusGameNanoseconds;
          previousStatusWriter = status.WriteNanoseconds;
          if (hitchWriter.has_value()) {
            const uint64_t hitch = hitchWriter->WriteNanoseconds();
            perf << " hitch_writer_ms="
                 << static_cast<double>(hitch - previousHitchWriter) / 1.0e6;
            previousHitchWriter = hitch;
          }
        }
        // Capture sub-stages (SWITCH_PORT.md "Capture delta"): µs per draw
        // of the queue's part of pica_capture_ms (register file, uniforms and
        // default attributes, the rest of the packet, decode, index/vertex
        // and texture snapshots, hashing inside them, enqueue), the bytes of
        // state written on this thread, the frontend's list walk and lighting
        // LUT work (ms per host frame), the replay of capture-delta logs off
        // this thread and the placeholder plan per draw (pica_plan).
        {
          static Oot3dNativeGame::Oot3dPicaFrontendCaptureStats previousFrontend{};
          static Oot3dNativeGame::Oot3dPicaCaptureDeltaLog::Totals previousMaterialize{};
          static double previousPlanSeconds = 0.0;
          const auto& frontend = picaFrontend.CaptureStats();
          const auto materialize =
              Oot3dNativeGame::Oot3dPicaCaptureDeltaLog::MaterializeTotals();
          const uint64_t draws = capture.Draws - previousCapture.Draws;
          const auto usPerDraw = [draws](uint64_t now, uint64_t before) {
            return draws != 0U
                       ? Oot3dNativeGame::CaptureTicksToNanoseconds(now - before) /
                             1.0e3 / static_cast<double>(draws)
                       : 0.0;
          };
          const auto msPerFrame = [hostFrames](uint64_t now, uint64_t before) {
            return Oot3dNativeGame::CaptureTicksToNanoseconds(now - before) / 1.0e6 /
                   hostFrames;
          };
          const auto perFrame = [hostFrames](uint64_t now, uint64_t before) {
            return static_cast<double>(now - before) / hostFrames;
          };
          const uint64_t materialized = materialize.Draws - previousMaterialize.Draws;
          perf << " capture: capture_delta=" << (submissionQueueStorage->CaptureDelta() ? 1 : 0)
               << " cap_regs_us=" << usPerDraw(capture.RegisterTicks, previousCapture.RegisterTicks)
               << " cap_uniforms_us=" << usPerDraw(capture.UniformTicks, previousCapture.UniformTicks)
               << " cap_packet_us=" << usPerDraw(capture.PacketTicks, previousCapture.PacketTicks)
               << " cap_decode_us=" << usPerDraw(capture.DecodeTicks, previousCapture.DecodeTicks)
               << " cap_vertex_us=" << usPerDraw(capture.VertexTicks, previousCapture.VertexTicks)
               << " cap_texture_us=" << usPerDraw(capture.TextureTicks, previousCapture.TextureTicks)
               << " cap_hash_us=" << usPerDraw(capture.HashTicks, previousCapture.HashTicks)
               << " cap_enqueue_us=" << usPerDraw(capture.EnqueueTicks, previousCapture.EnqueueTicks)
               << " cap_state_kb="
               << static_cast<double>(capture.StateBytes - previousCapture.StateBytes) / 1024.0 /
                      hostFrames
               << " cap_delta_draws=" << perFrame(capture.DeltaDraws, previousCapture.DeltaDraws)
               << " cap_keyframes=" << perFrame(capture.Keyframes, previousCapture.Keyframes)
               << " cap_materialize_us="
               << (materialized != 0U
                       ? Oot3dNativeGame::CaptureTicksToNanoseconds(materialize.Ticks -
                                                                    previousMaterialize.Ticks) /
                             1.0e3 / static_cast<double>(materialized)
                       : 0.0)
               << " cap_materialized=" << perFrame(materialize.Draws, previousMaterialize.Draws)
               << " cap_plan_us="
               << (draws != 0U ? (phaseTiming.PicaPlanSeconds - previousPlanSeconds) * 1.0e6 /
                                     static_cast<double>(draws)
                               : 0.0)
               << " list_ms=" << msPerFrame(frontend.ListTicks, previousFrontend.ListTicks)
               << " lists=" << perFrame(frontend.Lists, previousFrontend.Lists)
               << " list_kwords="
               << perFrame(frontend.ListWords, previousFrontend.ListWords) / 1024.0
               << " list_walk_fast=" << (picaFrontend.ListWalkFast() ? 1 : 0)
               << " list_walk_ms="
               << msPerFrame(frontend.ListTicks - frontend.SinkTicks,
                             previousFrontend.ListTicks - previousFrontend.SinkTicks)
               << " list_walk_ns_word="
               << (frontend.ListWords != previousFrontend.ListWords
                       ? Oot3dNativeGame::CaptureTicksToNanoseconds(
                             (frontend.ListTicks - frontend.SinkTicks) -
                             (previousFrontend.ListTicks - previousFrontend.SinkTicks)) /
                             static_cast<double>(frontend.ListWords - previousFrontend.ListWords)
                       : 0.0)
               << " list_batched_kwords="
               << perFrame(frontend.BatchedWords, previousFrontend.BatchedWords) / 1024.0
               << " lut_ms=" << msPerFrame(frontend.LutTicks, previousFrontend.LutTicks)
               << " lut_hashes=" << perFrame(frontend.LutHashes, previousFrontend.LutHashes)
               << " lut_reuses=" << perFrame(frontend.LutReuses, previousFrontend.LutReuses)
               << " lut_copies=" << perFrame(frontend.LutCopies, previousFrontend.LutCopies)
               << " table_copies=" << perFrame(frontend.TableCopies, previousFrontend.TableCopies)
               << " same_value_writes="
               << perFrame(frontend.SameValueWrites, previousFrontend.SameValueWrites);
          previousFrontend = frontend;
          previousMaterialize = materialize;
          previousPlanSeconds = phaseTiming.PicaPlanSeconds;
        }
        // Index/vertex snapshot steps (SWITCH_PORT.md "Vertex snapshot hit
        // path", config/vertex_snapshot_fast): per host frame, the resources
        // looked up, proven unchanged by their certificate or not, without
        // a certificate, KiB of the unproven resources and KiB actually
        // compared, certified and changed pages, resources left to the
        // previous path; µs per draw of cap_vertex_us spent viewing and
        // certifying, comparing, copying, and the rest (lookup, LRU, index
        // range, the snapshot list).
        {
          const uint64_t draws = capture.Draws - previousCapture.Draws;
          const auto usPerDraw = [draws](uint64_t now, uint64_t before) {
            return draws != 0U
                       ? Oot3dNativeGame::CaptureTicksToNanoseconds(now - before) /
                             1.0e3 / static_cast<double>(draws)
                       : 0.0;
          };
          const auto perFrame = [hostFrames](uint64_t now, uint64_t before) {
            return static_cast<double>(now - before) / hostFrames;
          };
          const uint64_t steps = (capture.GeometryViewTicks - previousCapture.GeometryViewTicks) +
                                 (capture.GeometryCompareTicks - previousCapture.GeometryCompareTicks) +
                                 (capture.GeometryCopyTicks - previousCapture.GeometryCopyTicks);
          const uint64_t vertex = capture.VertexTicks - previousCapture.VertexTicks;
          perf << " vsnap: vertex_snapshot_fast=" << (submissionQueueStorage->VertexSnapshotFast() ? 1 : 0)
               << " vs_resources=" << perFrame(capture.GeometryResources, previousCapture.GeometryResources)
               << " vs_version_hits="
               << perFrame(capture.GeometryVersionHits, previousCapture.GeometryVersionHits)
               << " vs_version_misses="
               << perFrame(capture.GeometryVersionMisses, previousCapture.GeometryVersionMisses)
               << " vs_uncertified="
               << perFrame(capture.GeometryUncertified, previousCapture.GeometryUncertified)
               << " vs_unproven_kb="
               << perFrame(capture.GeometryUnprovenBytes, previousCapture.GeometryUnprovenBytes) / 1024.0
               << " vs_compared_kb="
               << perFrame(capture.GeometryComparedBytes, previousCapture.GeometryComparedBytes) / 1024.0
               << " vs_pages=" << perFrame(capture.GeometryPages, previousCapture.GeometryPages)
               << " vs_dirty_pages="
               << perFrame(capture.GeometryDirtyPages, previousCapture.GeometryDirtyPages)
               << " vs_fallbacks=" << perFrame(capture.GeometryFallbacks, previousCapture.GeometryFallbacks)
               << " vs_view_us=" << usPerDraw(capture.GeometryViewTicks, previousCapture.GeometryViewTicks)
               << " vs_compare_us="
               << usPerDraw(capture.GeometryCompareTicks, previousCapture.GeometryCompareTicks)
               << " vs_copy_us=" << usPerDraw(capture.GeometryCopyTicks, previousCapture.GeometryCopyTicks)
               << " vs_rest_us=" << usPerDraw(vertex >= steps ? vertex - steps : 0U, 0U);
          // Texture snapshot steps (SWITCH_PORT.md "Texture snapshot and
          // packet stages", config/texture_snapshot_fast), the same fields
          // for textures: per host frame, textures looked up (with GPU copy
          // references: tx_copy_referenced), proven unchanged or not, without
          // a certificate, KiB unproven and compared, certified and changed
          // pages, left to the previous path, made anew; µs per draw of
          // cap_texture_us viewing (with the lookup), comparing, making new
          // snapshots, and the rest.
          const uint64_t textureSteps =
              (capture.TextureViewTicks - previousCapture.TextureViewTicks) +
              (capture.TextureCompareTicks - previousCapture.TextureCompareTicks) +
              (capture.TextureCopyTicks - previousCapture.TextureCopyTicks);
          const uint64_t texture = capture.TextureTicks - previousCapture.TextureTicks;
          perf << " tsnap: texture_snapshot_fast=" << (submissionQueueStorage->TextureSnapshotFast() ? 1 : 0)
               << " tx_resources=" << perFrame(capture.TextureResources, previousCapture.TextureResources)
               << " tx_copy_referenced="
               << perFrame(capture.TextureCopyReferenced, previousCapture.TextureCopyReferenced)
               << " tx_version_hits="
               << perFrame(capture.TextureVersionHits, previousCapture.TextureVersionHits)
               << " tx_version_misses="
               << perFrame(capture.TextureVersionMisses, previousCapture.TextureVersionMisses)
               << " tx_uncertified="
               << perFrame(capture.TextureUncertified, previousCapture.TextureUncertified)
               << " tx_unproven_kb="
               << perFrame(capture.TextureUnprovenBytes, previousCapture.TextureUnprovenBytes) / 1024.0
               << " tx_compared_kb="
               << perFrame(capture.TextureComparedBytes, previousCapture.TextureComparedBytes) / 1024.0
               << " tx_pages=" << perFrame(capture.TexturePages, previousCapture.TexturePages)
               << " tx_dirty_pages="
               << perFrame(capture.TextureDirtyPages, previousCapture.TextureDirtyPages)
               << " tx_fallbacks=" << perFrame(capture.TextureFallbacks, previousCapture.TextureFallbacks)
               << " tx_misses=" << perFrame(capture.TextureMisses, previousCapture.TextureMisses)
               << " tx_view_us=" << usPerDraw(capture.TextureViewTicks, previousCapture.TextureViewTicks)
               << " tx_compare_us="
               << usPerDraw(capture.TextureCompareTicks, previousCapture.TextureCompareTicks)
               << " tx_copy_us=" << usPerDraw(capture.TextureCopyTicks, previousCapture.TextureCopyTicks)
               << " tx_rest_us=" << usPerDraw(texture >= textureSteps ? texture - textureSteps : 0U, 0U);
        }
        previousCapture = capture;
        previousGuestInPica = phaseTiming.GuestInsidePicaSubmitSeconds;
        previousPicaSubmit = picaSubmitSeconds;
      }
      // Composition spans lost per window (deko3d "lighting transient" lines
      // name the draws that lost them): spans crossing a submission,
      // rejected over an older pending span, mesh packets outside any owner,
      // evicted pending spans, CMB passes not recognised. Reset() (state
      // loads) restarts the counters.
      {
        static Oot3dNativeGame::Oot3dNativePicaCompositionStats previousComposition{};
        const auto& composition = picaCompositionTracker.Stats();
        const auto delta = [](uint64_t now, uint64_t before) { return now >= before ? now - before : now; };
        perf << " composition: spans_consumed="
             << delta(composition.ConsumedPacketSpans, previousComposition.ConsumedPacketSpans)
             << " spans_partial="
             << delta(composition.PartiallyOverlappingSubmissions,
                      previousComposition.PartiallyOverlappingSubmissions)
             << " spans_overlapping="
             << delta(composition.OverlappingPacketSpans, previousComposition.OverlappingPacketSpans)
             << " spans_unattributed="
             << delta(composition.UnattributedPacketSpans, previousComposition.UnattributedPacketSpans)
             << " spans_evicted=" << delta(composition.EvictedPacketSpans, previousComposition.EvictedPacketSpans)
             << " cmb_invalid="
             << delta(composition.InvalidCmbPassEntries + composition.InvalidCmbReturnAddresses,
                      previousComposition.InvalidCmbPassEntries + previousComposition.InvalidCmbReturnAddresses);
        // Mesh/primitive packets whose range could not be recorded (no span
        // for their draws, and no other counter says so).
        const auto invalid = [](const Oot3dNativeGame::Oot3dNativePicaCompositionStats& stats) {
          return stats.InvalidPacketSpans + stats.ReadFailures + stats.InvalidPrimitivePacketRanges +
                 stats.NestedPrimitivePacketEntries + stats.InvalidPrimitivePacketReturnAddresses +
                 stats.UnmatchedPrimitivePacketExits + stats.InvalidAtmosphereCommandRanges;
        };
        perf << " spans_invalid=" << delta(invalid(composition), invalid(previousComposition));
        // Whole CMB passes recorded, handed to the frontend, and lost
        // (crossing a submission or another pass, or evicted).
        perf << " passes=" << delta(composition.RecordedPassSpans, previousComposition.RecordedPassSpans)
             << " passes_consumed=" << delta(composition.ConsumedPassSpans, previousComposition.ConsumedPassSpans)
             << " passes_lost="
             << delta(composition.PartialPassSpans + composition.EvictedPassSpans,
                      previousComposition.PartialPassSpans + previousComposition.EvictedPassSpans);
        previousComposition = composition;
        // How the frontend attributed draws without a plain span hit
        // (Oot3dPicaCompositionResolveStats): late lists (kicked after the
        // next frame's HUD phase began) and their draws, draws left UI inside
        // a world span, scene draws in no span (of them inside one CMB
        // owner), scene draws of lists without spans.
        static Oot3dNativeGame::Oot3dPicaCompositionResolveStats previousResolve{};
        const auto& resolve = picaFrontend.CompositionResolveStats();
        perf << " late_lists=" << delta(resolve.LateLists, previousResolve.LateLists)
             << " late_draws=" << delta(resolve.LateListDraws, previousResolve.LateListDraws)
             << " ui_world_draws=" << delta(resolve.UiListWorldSpanDraws, previousResolve.UiListWorldSpanDraws)
             << " no_span_draws=" << delta(resolve.NoSpanDraws, previousResolve.NoSpanDraws)
             << " owner_holes=" << delta(resolve.SameOwnerHoleDraws, previousResolve.SameOwnerHoleDraws)
             << " pass_draws=" << delta(resolve.PassSpanDraws, previousResolve.PassSpanDraws)
             << " bare_list_draws=" << delta(resolve.BareListDraws, previousResolve.BareListDraws);
        previousResolve = resolve;
      }
      // Game-thread work outside the phases above, ms per host frame.
      {
        static UntimedSeconds previousUntimed{};
        const auto perFrame = [&](double now, double before) {
          return (now - before) * 1000.0 / hostFrames;
        };
        const auto perFrameCount = [&](uint64_t now, uint64_t before) {
          return static_cast<double>(now - before) / hostFrames;
        };
        perf << " untimed: frame_head=" << perFrame(untimed.FrameHead, previousUntimed.FrameHead)
             << " refresh_prep=" << perFrame(untimed.RefreshPrep, previousUntimed.RefreshPrep)
             << " ui_submit=" << perFrame(untimed.UiSubmit, previousUntimed.UiSubmit)
             << " kernel_reacquire="
             << perFrame(untimed.KernelReacquire, previousUntimed.KernelReacquire)
             // Part of `input`: PollNativeA32Input alone (SDL pad, bindings).
             << " input_poll=" << perFrame(untimed.InputPoll, previousUntimed.InputPoll)
             // frame_head split: frame-rate policy, area + pack scene note
             // (with the pacer's presentation submit), cadence report +
             // refresh plan, UI/config head.
             << " head_policy=" << perFrame(untimed.HeadPolicy, previousUntimed.HeadPolicy)
             << " head_area=" << perFrame(untimed.HeadArea, previousUntimed.HeadArea)
             << " head_cadence=" << perFrame(untimed.HeadCadence, previousUntimed.HeadCadence)
             << " head_ui=" << perFrame(untimed.HeadUi, previousUntimed.HeadUi)
             // ui_submit split: textures ahead, HUD build, PrepareTextures,
             // render-stream submit; then its kernel-lock acquisitions (per
             // host frame), those that waited and their ms
             // (config/kernel_ui_lock: 0 takes when on).
             << " ui_ahead=" << perFrame(untimed.UiAhead, previousUntimed.UiAhead)
             << " ui_build=" << perFrame(untimed.UiBuild, previousUntimed.UiBuild)
             << " ui_prepare=" << perFrame(untimed.UiPrepare, previousUntimed.UiPrepare)
             << " ui_queue=" << perFrame(untimed.UiQueue, previousUntimed.UiQueue)
             << " ui_lock=" << (uiSubmitHoldsKernel ? 1 : 0)
             << " ui_lock_takes=" << perFrameCount(untimed.UiLockTakes, previousUntimed.UiLockTakes)
             << " ui_lock_waits=" << perFrameCount(untimed.UiLockWaits, previousUntimed.UiLockWaits)
             << " ui_lock_wait=" << perFrame(untimed.UiLockWait, previousUntimed.UiLockWait)
             // The same for the frame start's released region (host frame
             // start, input, savestate requests).
             << " start_lock_takes="
             << perFrameCount(untimed.StartLockTakes, previousUntimed.StartLockTakes)
             << " start_lock_waits="
             << perFrameCount(untimed.StartLockWaits, previousUntimed.StartLockWaits)
             << " start_lock_wait=" << perFrame(untimed.StartLockWait, previousUntimed.StartLockWait);
        previousUntimed = untimed;
      }
      // Totals of a Mesa built with the Horizon shader cache (absent from
      // the stock devkitPro library, hence weak).
      if (mesa_switch_get_stats != nullptr) {
        static constexpr std::array<const char*, 25> kMesaNames{
            "glsl_compiles", "glsl_compile_ms", "glsl_compiles_skipped", "links", "link_ms",
            "link_glsl_ms", "link_st_ms", "links_from_cache", "nvc0_translates", "nvc0_translate_ms",
            "nvc0_cache_hits", "binary_loads", "binary_load_fails", "binary_load_ms", "binary_saves",
            "binary_save_ms", "cache_gets", "cache_get_hits", "cache_get_ms", "cache_corrupt",
            "cache_puts", "cache_put_bytes", "cache_put_ms", "cache_puts_dropped", "cache_has_key_hits"};
        std::array<uint64_t, 32> mesa{};
        const unsigned count = std::min<unsigned>(
            mesa_switch_get_stats(mesa.data(), static_cast<unsigned>(mesa.size())), kMesaNames.size());
        perf << " mesa:";
        for (unsigned index = 0; index < count; ++index) {
          const bool nanoseconds = std::string_view(kMesaNames[index]).ends_with("_ms");
          perf << ' ' << kMesaNames[index] << '='
               << (nanoseconds ? mesa[index] / 1000000U : mesa[index]);
        }
        if (mesa_switch_shader_cache_status != nullptr) {
          perf << " cache_status=\"" << mesa_switch_shader_cache_status() << '"';
        }
      }
      perf << '\n';
      // Host ms per host frame each guest thread held the game thread
      // (dispatch + its SVCs' host services) and thread switches per frame
      // (its own line; NativeA32GuestThreadProfile), then the same time split
      // into guest code, SVCs, kernel-lock retakes and scheduling, with the
      // game thread's costliest SVCs (guest_kernel:).
      {
        static Oot3dNativeGame::NativeA32GuestThreadProfile previousThreads{};
        perf << Oot3dNativeGame::FormatNativeA32GuestThreadsLine(
                    process, previousThreads, runFrameCount, hostFrames)
             << '\n'
             << Oot3dNativeGame::FormatNativeA32GuestKernelLine(process, previousThreads,
                                                                hostFrames)
             << '\n';
        previousThreads = process.GuestThreadProfile();
      }
      // Mirror guest replacements: executions/declines per window (its own line).
      perf << Oot3dNativeGame::FormatOot3dGuestOverridesPerfLine();
      // config/second_core: how the DSP frames went out, the second core's
      // lock waits and preemption, and how long the game thread waited for
      // it, per host frame (its own line).
      if (process.SecondCoreEnabled()) {
        const auto now = process.GuestThreadProfile();
        perf << Oot3dNativeGame::FormatNativeA32SecondCoreLine(
                    now, secondCoreProfileWindowStart,
                    {secondCoreDsp.Pumped - secondCoreDspWindowStart.Pumped,
                     secondCoreDsp.Flushed - secondCoreDspWindowStart.Flushed,
                     secondCoreDsp.Late - secondCoreDspWindowStart.Late,
                     secondCoreDsp.WaitSeconds - secondCoreDspWindowStart.WaitSeconds},
                    hostFrames)
             << '\n';
        secondCoreDspWindowStart = secondCoreDsp;
        secondCoreProfileWindowStart = now;
      }
      // config/actor_profile: actor costs over the same window (its own line).
      if (actorProfile.has_value()) perf << actorProfile->TakeLine(areaText);
      // Draw distance: extended and denied draws over the same window.
      perf << Oot3dNativeGame::TakeNativeActorVisibilityPerfLine(widescreenProjection.ActorVisibility);
      // config/indirect_profile: hottest indirect call pairs (its own line).
      if (indirectProfile.has_value()) perf << indirectProfile->Profile().TakeLine();
      // config/gpu_timing: GPU/CPU per render-thread stage and GL call
      // counts over the same window (empty when off).
      if (Fast::GlPerf::Enabled()) {
        perf << Fast::GlPerf::TakeReport(runFrameCount) << '\n';
      }
      // deko3d: GPU time per frame over the same window (empty with GL).
      const std::string dkperf = Fast::Deko3dPerfReport(runFrameCount);
      if (!dkperf.empty()) {
        perf << dkperf << '\n';
      }
      // Late lists, HUD-phase lists with world spans and holes inside one
      // CMB owner since the last window (at most 64 in all; the per-pixel
      // blink, SWITCH_PORT.md).
      for (const auto& note : picaFrontend.TakeCompositionNotes()) {
        perf << "composition note: window_end=" << runFrameCount << ' '
             << Oot3dNativeGame::FormatOot3dPicaCompositionNote(note) << '\n';
      }
      // The last windows are repeated in a guest fault report
      // (oot3d_native_guest_fault_report.h): guest = kNames[0], present = [13].
      Oot3dNativeGame::GuestFaultPerf().Record(Oot3dNativeGame::GuestFaultPerfWindow{
          runFrameCount, hostFrames / seconds,
          (values[0] - windowValues[0]) * 1000.0 / hostFrames,
          (values[13] - windowValues[13]) * 1000.0 / hostFrames,
          seconds * 1000.0 / hostFrames,
          Oot3dNativeGame::PerfLineValue(dkperf, "busy_ms"),
          Oot3dNativeGame::PerfLineValue(dkperf, "fence_cpu_ms")});
      if (perfWriter.has_value()) {
        (void)perfWriter->Push(std::move(perf).str());
      } else {
        // The SD write (open, write, close) as before, on this thread.
        Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::PerfFileClose);
        std::ofstream file("sdmc:/switch/oot3dre/logs/perf.txt", std::ios::app);
        file << perf.str();
      }
      perfLineGameMs = SecondsSince(perfLineStart) * 1000.0;
      windowStart = now;
      windowValues = values;
      windowHostFrames = runFrameCount;
      windowGuestFrames = frameCount;
      windowGuestInPica = phaseTiming.GuestInsidePicaSubmitSeconds;
      Oot3dNativeGame::HangWatch::Mark(Oot3dNativeGame::HangWatch::Stage::FrameEnd);
    }
  }
  // Write once at the first frame and at most once more when a persistent
  // geometry is first observed. Periodic receipt writes would put
  // synchronous SD filesystem work directly in the presentation loop.
  if (runFrameCount == 1U || !gSwitchPicaGeometryCacheObserved) {
    const auto wholeAotStats =
        Oot3dNativeGame::GetOot3dCompiledFunctionStats();
    gSwitchWholeAotBlockLimitExits =
        wholeAotStats.WholeAotBlockLimitExits;
    const auto picaBackend = api.GetNativePicaBackendStats();
    gSwitchPicaGeometryCacheEnabled =
        picaBackend.GeometryCacheEnabled;
    gSwitchPicaGeometryPersistentDraws =
        picaBackend.GeometryPersistentDraws;
    gSwitchPicaGeometryPersistentUploads =
        picaBackend.GeometryPersistentUploads;
    gSwitchPicaGeometryRegistryEntries =
        picaBackend.GeometryRegistryEntries;
    gSwitchPicaGeometryCacheObserved =
        gSwitchPicaGeometryPersistentDraws != 0U &&
        gSwitchPicaGeometryPersistentUploads != 0U &&
        gSwitchPicaGeometryRegistryEntries != 0U;
    // Also every 600 host frames: without the geometry cache (deko3d) the
    // file stayed at host_frames=1/guest_frames=1, which read as a stalled
    // guest (24/10, p5 on the console). config/async_status_writes: the SD
    // replace (30-80 ms here, every present 600k-3 in hitches.txt since
    // test.11) runs on a service thread; this thread only formats the text.
    if (runFrameCount == 1U || gSwitchPicaGeometryCacheObserved ||
        runFrameCount % 600U == 0U) {
      const auto statusStart = std::chrono::steady_clock::now();
      if (asyncStatusWrites) {
        PostSwitchBootStage("runtime_running");
      } else {
        WriteSwitchBootStage("runtime_running");
      }
      statusGameNanoseconds += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - statusStart)
              .count());
      ++statusUpdates;
    }
  }
#endif
  const auto completedFrameTime = std::chrono::steady_clock::now();
  if (!preparationScreen.Done()) {
    preparationScreen.ObserveFrame(
        std::chrono::duration<double, std::milli>(completedFrameTime - lastCompletedFrameTime).count(),
        completedFrameTime);
    if (preparationScreen.Done()) {
      std::fprintf(stderr,
                   "TRIAEVUM_INITIAL_PREPARATION parts=%zu startup_seconds=%.3f wait_seconds=%.3f "
                   "first_frames=%u first_frames_seconds=%.3f\n",
                   preparationScreen.Parts(), preparationScreen.StartupSeconds(),
                   preparationScreen.WaitSeconds(),
                   preparationScreen.FirstFrames(), preparationScreen.FirstFramesSeconds());
    }
  }
  lastCompletedFrameTime = completedFrameTime;
  if (!benchmarkMeasurementStarted && preparationScreen.Done() &&
      runFrameCount >= hostArgs.BenchmarkWarmupFrames) {
    benchmark.MeasurementStart = completedFrameTime;
    benchmark.MeasurementEnd = completedFrameTime;
    benchmarkMeasurementStarted = true;
    if (cpuProbeRequested) CpuProbe::Thread.StartAt(CpuProbe::Read());
  } else if (benchmarkMeasurementStarted &&
             runFrameCount > hostArgs.BenchmarkWarmupFrames) {
    const auto currentPhases = phaseTiming.Values();
    std::array<double, 15> delta{};
    for (size_t i=0; i<delta.size(); ++i) {
      delta[i]=currentPhases[i]-phaseBeforeFrame[i];
      benchmark.PhaseSeconds[i]+=delta[i];
    }
    const double nestedGuest=phaseTiming.GuestInsidePicaSubmitSeconds-nestedGuestBeforeFrame;
    benchmark.NestedGuestSeconds+=nestedGuest;
    benchmark.GuestClockSeconds+=phaseTiming.GuestClockSeconds-guestClockBeforeFrame;
    const Oot3dNativeGame::PreBackendBudget budget{
      delta[0],delta[7],nestedGuest,delta[8],delta[2],delta[3],delta[5],delta[6]};
    if(budget.Valid()) benchmark.PreBackendTimes.RecordMilliseconds(budget.Total()*1000.0);
    else ++benchmark.InvalidPreBackendIntervals;
    // Present includes pacing. Visual presentation includes backend/replay.
    // Remove each inclusive scope once; keep unclassified host/UI work visible.
    const double envelope=std::chrono::duration<double>(completedFrameTime-benchmark.MeasurementEnd).count()
        -delta[4]-delta[11]-delta[13];
    benchmark.PreBackendEnvelopeTimes.RecordMilliseconds(envelope*1000.0);
    benchmark.UnattributedSeconds+=envelope-budget.Total();
    if (benchmarkFrameTimes) {
      auto phases = phaseTiming.Values();
      for (size_t i = 0; i < phases.size(); ++i)
        phases[i] = (phases[i] - phaseBeforeFrame[i]) * 1000.0;
      benchmark.SlowFrames.Record(runFrameCount, std::chrono::duration<double, std::milli>(
          completedFrameTime - benchmark.MeasurementEnd).count(), phases);
      benchmarkFrameTimes->RecordMilliseconds(
          std::chrono::duration<double, std::milli>(
              completedFrameTime - benchmark.MeasurementEnd).count());
    }
    benchmark.MeasurementEnd = completedFrameTime;
    ++benchmark.MeasuredFrames;
    if (cpuProbeRequested) benchmarkCpuPhases = CpuProbe::Thread.Snapshot();
  }
  for (const auto &action : launch.SavestateActions) {
    if (runFrameCount == static_cast<uint64_t>(action.RunFrame) + 1U) {
      savestateLibrary->Request(
          {action.Load ? Oot3dNativeGame::SavestateRequestKind::Load
                       : Oot3dNativeGame::SavestateRequestKind::Save,
           action.WithSettings, action.Path});
    }
  }
  ServiceSavestateRequests();
  {
    if (Oot3dNativeGame::ShouldCaptureNativeA32AutomatedState(
            launch.SaveStateFrameAvailable,
            automatedStateSaved || automatedStateSaveFailed, runFrameCount,
            launch.SaveStateFrame)) {
      const auto outcome =
          SaveState(launch.SaveStatePath, launch.SaveStateWithSettings);
      if (outcome == SaveStateOutcome::Saved) {
        automatedStateSaved = true;
        automatedStateSavedAtRunFrame = runFrameCount - 1U;
        automatedStateSavedAtGuestFrame = frameCount;
      } else if (outcome == SaveStateOutcome::Failed) {
        automatedStateSaveFailed = true;
      }
    }
  }
  if (hostArgs.FrameLimit > 0 && runFrameCount >= hostArgs.FrameLimit)
    scriptedExit = true;  // before the close the render thread may observe
  ApplyWindowDemoFrameLimit(hostArgs, window, runFrameCount);
  return true;
}

// The guest input of one refresh: touch and TopScreen routing state, the input timeline or the polled
// frame. False when the input asked to close the window.
bool NativeA32WindowSession::SampleGuestRefreshInput(
    const uint32_t &guestRefreshIndex,
    bool &nativeFrontendTouchEnabled,
    uint32_t &topScreenNativeGameMode,
    bool &topScreenStartRoutingEligible,
    Oot3dNativeGame::NativeA32InputFrame &physicalInputFrame,
    Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  if (guestRefreshIndex != 0U) {
    nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
        process.Memory(), uiLifecycleBridge,
        nativeCandidateDispatch.TopScreenUiProfile);
    topScreenNativeGameMode = UINT32_MAX;
    const bool refreshedTopScreenGameplayInputMode =
        process.Memory().Read32(kNativeGameMode,
                                &topScreenNativeGameMode) &&
        Oot3dNativeGame::IsTopScreenGameplayInputMode(
            topScreenNativeGameMode);
    topScreenStartRoutingEligible =
        uiLifecycleBridge.NativeGameplayPresentationActive() &&
        refreshedTopScreenGameplayInputMode &&
        Oot3dNativeGame::IsTopScreenStartRoutingMode(topScreenNativeGameMode);
    picaCompositionDomain =
        refreshedTopScreenGameplayInputMode
            ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
            : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
    picaRefreshCompositionDomain = picaCompositionDomain;
  }
  // Re-arm after any state restore: RestoreState replaces the memory
  // image, and the watch is diagnostic state that must not disappear with
  // it. Cheap when the trapped page set is unchanged.
  if (!rawCopyWatchRanges.empty() &&
      !process.Memory().RangeWatchActive()) {
    process.Memory().SetRangeWatch(rawCopyWatchRanges);
  }
  inputFrame = ResolveNativeA32GuestInput(
      physicalInputFrame, inputTimeline, frameCount, runFrameCount,
      nativeFrontendTouchEnabled, topScreenStartRoutingEligible,
      nativeCandidateDispatch.TopScreenUiProfile,
      nativeCandidateDispatch.StartButtonLatch,
      nativeCandidateDispatch.TopScreenStartRouting);
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      Oot3dNativeGame::ApplyTopScreenVersionChoiceReveal(
          inputFrame, uiLifecycleBridge.NativeVersionChoiceAwaitingReveal(),
          uiLifecycleBridge.NativeVersionChoicePrompting(),
          nativeCandidateDispatch.TopScreenVersionChoiceReveal)) {
    uiLifecycleBridge.RevealNativeVersionChoice();
  }
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  UpdateInputBench(inputFrame);
  if (devLabStickX != 0 || devLabStickY != 0) {
    inputFrame.Hid.CirclePadX = devLabStickX;
    inputFrame.Hid.CirclePadY = devLabStickY;
  }
  inputFrame.Hid.Buttons |= devLabButtons;
#endif
  inputDiagnostics.Observe(inputFrame);
  if (inputFrame.Exit) {
    window.Close();
    return false;
  }
  return true;
}

// Before the guest runs a refresh: TopScreen input routing, the structural scenario, the guest clock to the
// next vblank, HID and DSP audio. False when the scenario closed the window.
bool NativeA32WindowSession::UpdateGuestServicesForRefresh(
    bool &topScreenStartRoutingEligible,
    Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  auto &dspHle = *dspHleStorage;
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  UpdateDevFreeCamera(inputFrame);
#endif
  const uint32_t topScreenButtons = inputFrame.Hid.Buttons;
  const uint32_t topScreenPressed =
      topScreenButtons &
      ~nativeCandidateDispatch.PreviousTopScreenButtons;
  const auto topScreenButton =
      [](Oot3dNativeGame::NativeA32HidButton value) {
        return Oot3dNativeGame::NativeA32HidButtonMask(value);
      };
  Oot3dNativeGame::TopScreenDpadActionState topScreenDpadActions;
  Oot3dNativeGame::TopScreenOcarinaState ocarinaOwner;
  const bool ocarinaOwnsDpad = nativeCandidateDispatch.TopScreenUiProfile &&
      Oot3dNativeGame::ReadTopScreenOcarinaState(process.Memory(), &ocarinaOwner) &&
      ocarinaOwner.Active;
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      topScreenStartRoutingEligible && !ocarinaOwnsDpad) {
    bool childLink = false;
    std::string childLinkError;
    if (!Oot3dNativeGame::ReadTopScreenChildLink(
            process.Memory(), &childLink, &childLinkError)) {
      throw std::runtime_error("TopScreen Link age decode failed: " +
                               childLinkError);
    }
    Oot3dNativeGame::TopScreenDpadPhysicalState dpadPhysical;
    dpadPhysical.ChildLink = childLink;
    constexpr std::array<Oot3dNativeGame::NativeA32HidButton, 4>
        kDpadButtons{Oot3dNativeGame::NativeA32HidButton::DpadUp,
                     Oot3dNativeGame::NativeA32HidButton::DpadDown,
                     Oot3dNativeGame::NativeA32HidButton::DpadLeft,
                     Oot3dNativeGame::NativeA32HidButton::DpadRight};
    for (std::size_t direction = 0U;
         direction < kDpadButtons.size(); ++direction) {
      const auto mask = topScreenButton(kDpadButtons[direction]);
      dpadPhysical.Pressed[direction] =
          (topScreenPressed & mask) != 0U;
      dpadPhysical.Held[direction] =
          (topScreenButtons & mask) != 0U;
    }
    topScreenDpadActions = Oot3dNativeGame::ResolveTopScreenDpadActions(
        activeTopScreenConfig, dpadPhysical);
  }
  const bool zrHeld = ThreeDsRecomp::Input::IsButtonHeld(
      inputFrame, ThreeDsRecomp::Input::Button::Zr);
  const bool zlHeld = ThreeDsRecomp::Input::IsButtonHeld(
      inputFrame, ThreeDsRecomp::Input::Button::Zl);
  Oot3dNativeGame::TopScreenExtendedInputFrame sampledTopScreenInput = {
      .ZrHeld =
          zrHeld ||
          topScreenDpadActions.IsHeld(
              Oot3dNativeGame::TopScreenDpadAction::ItemZr),
      .ZlHeld =
          zlHeld ||
          topScreenDpadActions.IsHeld(
              Oot3dNativeGame::TopScreenDpadAction::ItemZl),
      .DpadLeftHeld =
          (inputFrame.Hid.Buttons &
           Oot3dNativeGame::NativeA32HidButtonMask(
               Oot3dNativeGame::NativeA32HidButton::DpadLeft)) != 0U,
      .DpadRightHeld =
          (inputFrame.Hid.Buttons &
           Oot3dNativeGame::NativeA32HidButtonMask(
               Oot3dNativeGame::NativeA32HidButton::DpadRight)) != 0U};
  sampledTopScreenInput.DpadUpHeld =
      (inputFrame.Hid.Buttons & topScreenButton(
          Oot3dNativeGame::NativeA32HidButton::DpadUp)) != 0U;
  if (ocarinaOwnsDpad) {
    inputFrame.Hid.Buttons &= ~(topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadLeft) |
                               (ocarinaOwner.Page == 12 ? topScreenButton(
                                   Oot3dNativeGame::NativeA32HidButton::DpadUp) : 0U) |
                               topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadRight));
  }
  sampledTopScreenInput.GameplayDpadActionsOwned = true;
  nativeCandidateDispatch.TopScreenInputClock.ObserveGuest(sampledTopScreenInput);
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      activeTopScreenConfig.FreeCameraEnabled &&
      nativeControlPollingState.GameplayMouseOwned) {
    nativeCandidateDispatch.TopScreenCameraInput.Observe(
        inputFrame.CStick);
  } else if (inputFrame.CStick.Kind ==
             Oot3dNativeGame::NativeFreeCameraInputKind::Absolute) {
    nativeCandidateDispatch.TopScreenCameraInput.Observe(
        inputFrame.CStick);
  } else {
    nativeCandidateDispatch.TopScreenCameraInput.Reset();
  }
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    const uint32_t buttons = inputFrame.Hid.Buttons;
    const uint32_t pressed =
        buttons & ~nativeCandidateDispatch.PreviousTopScreenButtons;
    const auto button = [](Oot3dNativeGame::NativeA32HidButton value) {
      return Oot3dNativeGame::NativeA32HidButtonMask(value);
    };
    const bool gameplayViewPressed = topScreenDpadActions.WasPressed(
        Oot3dNativeGame::TopScreenDpadAction::View);
    const bool gameplayOcarinaPressed =
        topScreenDpadActions.WasPressed(
            Oot3dNativeGame::TopScreenDpadAction::Ocarina);
    const bool leftShoulderHeld =
        (buttons &
         button(Oot3dNativeGame::NativeA32HidButton::L)) != 0U;
    const bool rightShoulderHeld =
        (buttons &
         button(Oot3dNativeGame::NativeA32HidButton::R)) != 0U;
    bool selectPressed =
        (pressed &
         button(Oot3dNativeGame::NativeA32HidButton::Select)) != 0U;
    // R6 (2.1.1 0x005D5730..5BC0): on an open Items/Gear page SELECT toggles
    // the item description; it reaches the gameplay routes (minimap, Save)
    // only while the pause controller is in its gameplay state.
    if (Oot3dNativeGame::TopScreenItemDescriptionInputs description;
        Oot3dNativeGame::ReadTopScreenItemDescriptionInputs(process.Memory(), &description)) {
      auto &descriptionRuntime = *nativeCandidateDispatch.ItemDescription;
      if (Oot3dNativeGame::UpdateTopScreenItemDescription(
              description, selectPressed, descriptionRuntime.VisibleState())) {
        descriptionRuntime.CountToggle();
        std::string soundError;
        if (!Oot3dNativeGame::PlayTopScreenUiSound(process, 0x01000498U,
                                                   kTopScreenDirectCallReturn, &soundError))
          throw std::runtime_error("TopScreen description sound failed: " + soundError);
      }
      if (selectPressed && Oot3dNativeGame::TopScreenItemDescriptionOwnsSelect(description)) {
        selectPressed = false;
        inputFrame.Hid.Buttons &= ~button(Oot3dNativeGame::NativeA32HidButton::Select);
      }
    }
    if (topScreenDpadActions.WasPressed(
            Oot3dNativeGame::TopScreenDpadAction::MinimapToggle) ||
        (selectPressed &&
         activeTopScreenConfig.SelectAction ==
             Oot3dNativeGame::TopScreenSelectAction::MinimapToggle)) {
      SetTopScreenSessionMinimapVisible(
          !activeTopScreenConfig.MinimapVisible);
    }
    if (selectPressed &&
        activeTopScreenConfig.SelectAction ==
            Oot3dNativeGame::TopScreenSelectAction::MinimapToggle) {
      inputFrame.Hid.Buttons &=
          ~button(Oot3dNativeGame::NativeA32HidButton::Select);
    }
    // 2.1.1 0x005D5BC0..0x005D5C70 (issue 80): the source port disables the
    // native Save route at 0x0042EC2C, so SELECT opens Save/Quit through the
    // pause transaction, delivered at the native UI input boundary.
    if (selectPressed &&
        activeTopScreenConfig.SelectAction ==
            Oot3dNativeGame::TopScreenSelectAction::SaveScreen) {
      nativeCandidateDispatch.PauseTransaction->RequestSelectSave();
      nativeCandidateDispatch.ItemDescription->Hide();
      inputFrame.Hid.Buttons &=
          ~button(Oot3dNativeGame::NativeA32HidButton::Select);
    }

    if (gameplayViewPressed || gameplayOcarinaPressed) {
      ++nativeCandidateDispatch.TopScreenGameplayDpadAttempts;
      Oot3dNativeGame::TopScreenGameplayDpadResult dpadResult;
      std::string dpadError;
      if (!Oot3dNativeGame::PrepareTopScreenGameplayDpadGuest(
              process.Memory(),
              {.ViewPressed = gameplayViewPressed,
               .OcarinaPressed = gameplayOcarinaPressed,
               .LeftShoulderHeld = leftShoulderHeld,
               .RightShoulderHeld = rightShoulderHeld},
              &dpadResult, &dpadError)) {
        throw std::runtime_error(
            "TopScreen gameplay D-pad prepare failed: " + dpadError);
      }
      nativeCandidateDispatch.TopScreenNaviViewActivations +=
          dpadResult.NaviViewActivated ? 1U : 0U;
      if (dpadResult.OcarinaQueryRequired) {
        ++nativeCandidateDispatch.TopScreenOcarinaQueries;
        uint32_t nativeEligibility = 0U;
        constexpr std::array<std::uint32_t, 0> kNoArguments{};
        if (!process.InvokeFunctionWithResult(
                kTopScreenOcarinaEligibility, kNoArguments,
                kTopScreenDirectCallReturn, &nativeEligibility,
                &dpadError) ||
            !Oot3dNativeGame::CommitTopScreenGameplayOcarinaGuest(
                process.Memory(), nativeEligibility, &dpadResult,
                &dpadError)) {
          throw std::runtime_error(
              "TopScreen ocarina D-pad route failed: " + dpadError);
        }
        nativeCandidateDispatch.TopScreenOcarinaActivations +=
            dpadResult.OcarinaActivated ? 1U : 0U;
      }
    }
    std::string gameplayActionError;
    if (const auto sound = uiLifecycleBridge.TakeOcarinaGuideSound(); sound != 0U) {
      if (!Oot3dNativeGame::PlayTopScreenUiSound(
              process, sound, kTopScreenDirectCallReturn, &gameplayActionError)) {
        throw std::runtime_error("TopScreen ocarina guide sound failed: " + gameplayActionError);
      }
    }
    if (!Oot3dNativeGame::ConsumeTopScreenGameplayActions(
            process, topScreenDpadActions,
            nativeCandidateDispatch.TopScreenInput.ZrPressed ||
                nativeCandidateDispatch.TopScreenInput.ZlPressed,
            kTopScreenDirectCallReturn,
            &nativeCandidateDispatch.TopScreenGameplayActionRuntime,
            &nativeCandidateDispatch.TopScreenGameplayActions,
            &gameplayActionError)) {
      throw std::runtime_error(
          "TopScreen gameplay action failed: " +
          gameplayActionError);
    }
    nativeCandidateDispatch.PreviousTopScreenButtons = buttons;
    nativeCandidateDispatch.TopScreenItems.DirectItemId =
        nativeCandidateDispatch.TopScreenGameplayActionRuntime.DirectItem.ActiveItemId;
    nativeCandidateDispatch.PauseTransaction->AgeUndelivered();
    Oot3dNativeGame::TopScreenPauseDrawInputs pauseInputs;
    if (!Oot3dNativeGame::ReadTopScreenPauseDrawInputs(process.Memory(),
                                                       &pauseInputs)) {
      nativeCandidateDispatch.TopScreenPauseAotBarrier = true;
    } else {
      // ChildStates: [0] Gear 0x00504484, [1] Items 0x0050672C,
      // [2] Dungeon/World Map 0x00506CE8.
      Oot3dNativeGame::ObserveTopScreenStartRoutingPage(
          pauseInputs.ChildStates[1] != 0U, pauseInputs.ChildStates[0] != 0U,
          pauseInputs.ChildStates[2] != 0U,
          nativeCandidateDispatch.TopScreenStartRouting);
      const auto closePlan =
          Oot3dNativeGame::ResolveTopScreenPauseStartClose(
              pauseInputs,
              (pressed &
               button(Oot3dNativeGame::NativeA32HidButton::Start)) != 0U);
      // The transactions run inside this refresh's native UI update
      // (oot3d_top_screen_pause_transaction.h), not here: the guest may be
      // suspended inside a routine whose tail would undo them.
      if (closePlan.Page !=
          Oot3dNativeGame::TopScreenPauseClosePage::None) {
        nativeCandidateDispatch.PauseTransaction->RequestStartClose();
        Oot3dNativeGame::MarkTopScreenStartCloseGestureConsumed(
            nativeCandidateDispatch.TopScreenStartRouting);
        inputFrame.Hid.Buttons &=
            ~button(Oot3dNativeGame::NativeA32HidButton::Start);
        // START wins: a pending Save/Quit of the same page is dropped.
        nativeCandidateDispatch.TopScreenPauseSystemOpen = {};
      } else if (nativeCandidateDispatch.TopScreenStartRouting.OpenRequested) {
        nativeCandidateDispatch.PauseTransaction->RequestOpenLastPage(
            nativeCandidateDispatch.TopScreenStartRouting.LastPage);
      } else if (activeTopScreenConfig.ExitItemsToSaveScreen) {
        const auto systemOpenPlan =
            Oot3dNativeGame::ResolveTopScreenPauseBSystemOpen(
                pauseInputs,
                (pressed &
                 button(Oot3dNativeGame::NativeA32HidButton::B)) != 0U,
                nativeCandidateDispatch.TopScreenPauseSystemOpen);
        if (systemOpenPlan.Page !=
            Oot3dNativeGame::TopScreenPauseClosePage::None) {
          nativeCandidateDispatch.PauseTransaction->RequestSystemOpen(
              systemOpenPlan);
          // 0x005D5478..5558: B-to-Save clears the description.
          nativeCandidateDispatch.ItemDescription->Hide();
        }
      }
      const bool sceneRuntimeRoute = pauseInputs.HasScene &&
                                     pauseInputs.SceneMode == 3U &&
                                     (pauseInputs.RuntimeMode == 1U ||
                                      pauseInputs.RuntimeMode == 2U);
      nativeCandidateDispatch.TopScreenPauseAotBarrier =
          sceneRuntimeRoute ||
          Oot3dNativeGame::ResolveTopScreenPauseRouteActive(
              pauseInputs,
              &nativeCandidateDispatch.TopScreenAotPauseRoute);
    }
  }
  nativeCandidateDispatch.TopScreenStartRouting.OpenRequested = false;
  if (scenarioBootstrap.has_value() && !scenarioBootstrap->Finished()) {
    std::string scenarioError;
    if (!scenarioBootstrap->Advance(
            process, sceneViewProbe.Stats().LastPlayStateAddress,
            frameCount, &scenarioError)) {
      std::cerr << "oot3d_native_game: structural scenario failed: "
                << scenarioError << '\n';
      if (launch.ScenarioStrict) {
        scriptedExit = true;
        window.Close();
        return false;
      }
    }
    if (launch.ScenarioAutoExit && scenarioBootstrap->Complete()) {
      scriptedExit = true;
      window.Close();
      return false;
    }
  }
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  // F1 > Dev Tools warps run at the same guest-frame boundary as the
  // structural scenarios, through the same native transition request.
  if (devWarp != nullptr) {
    if (devWarpGuest == nullptr) devWarpGuest = Oot3dNativeGame::MakeDevWarpProcessGuest(process);
    devWarp->Advance(*devWarpGuest, sceneViewProbe.Stats().LastPlayStateAddress, frameCount);
    UpdateDevFreeCameraTour(frameCount);
    if (devLab) UpdateDevLab(frameCount);
  }
#endif
  refreshTickRemainder += kCtrArm11TicksPerSecond;
  nextVblankTick += refreshTickRemainder / kGuestDisplayRefreshRate;
  refreshTickRemainder %= kGuestDisplayRefreshRate;
  const auto clockStart = std::chrono::steady_clock::now();
  const auto clockResolution = AdvanceGuestClockTo(
      process, hostServices, processResult, nextVblankTick,
      launch.WholeAotBlockBudget, SecondCorePump());
  const double clockSeconds = SecondsSince(clockStart);
  phaseTiming.GuestClockSeconds += clockSeconds;
  phaseTiming.GuestSeconds += clockSeconds;
  if (clockResolution.OvershootTicks != 0U) {
    ++lateVblankDeadlineCount;
    maximumVblankDeadlineOvershootTicks =
        std::max(maximumVblankDeadlineOvershootTicks,
                 clockResolution.OvershootTicks);
  }
  const auto hidUpdate = hostServices.AdvanceHidToCurrentTick(
      process.Memory(), inputFrame.Hid);
  if (hidUpdate.Status ==
      Oot3dNativeGame::NativeA32CtrHidUpdateStatus::Failed) {
    throw std::runtime_error(
        "native HID sample could not be written to shared memory");
  }
  if (hidUpdate.EventsSignaled &&
      processResult.Kind !=
          Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget, SecondCorePump());
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  const uint32_t dspAudioFrames =
      hostServices.TakePendingDspAudioFrames();
  // config/audio_deferred: the DSP frames of this refresh (HLE mix and the
  // guest sound thread they wake) run after the host frame is presented,
  // off the path to the render thread's handoff (DeliverDspAudioFrames).
  // With the second core on they are always deferred: the second core
  // delivers them itself as soon as its threads are parked
  // (DeliverDspAudioFrameOnSecondCore; the game thread never waits for it),
  // or, without the host audio stream, its pump during the guest runs
  // (PumpDspAudioFrame) and the deferred points.
  if (deferDspAudio || SecondCorePump() != nullptr || dspOnSecondCore) {
    deferredDspAudioFrames += dspAudioFrames;
    if (dspOnSecondCore && dspAudioFrames != 0U)
      Oot3dNativeGame::SecondCore::NotifyKernel();
  } else {
    DeliverDspAudioFrames(dspAudioFrames);
  }
  if (!hostServices.SignalVBlank(process.Memory())) {
    throw std::runtime_error(
        "native VBlank could not enter the GSP relay queue");
  }
  ++vblankCount;
  return true;
}

// One DSP audio frame at a time: HLE mix, then the DSP interrupt and the
// guest threads it wakes (the sound thread), then the host audio output.
// runGuest = false: the caller is the second core's pump, between two Run()
// calls of a guest run that goes on.
void NativeA32WindowSession::DeliverDspAudioFrames(uint32_t dspAudioFrames,
                                                   bool runGuest) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &dspHle = *dspHleStorage;
  const bool secondCore = process.SecondCoreEnabled();
  // Storage kept across refreshes (no allocation per refresh); the samples
  // are the same.
  std::vector<int16_t> &audioSamples = refreshAudioSamples;
  audioSamples.clear();
  audioSamples.reserve(static_cast<size_t>(dspAudioFrames) * 320U);
  for (uint32_t audioFrame = 0; audioFrame < dspAudioFrames;
       ++audioFrame) {
    if (secondCore && runGuest) {
      // The sound thread finishes the previous frame first (the guest's
      // interrupt protocol, as the real DSP's period leaves it), and stays
      // parked in its wait while the mix below updates the shared regions
      // in place. Bounded: a sound thread that never waits again cannot
      // stall the game thread (the frame then goes out as the DSP would).
      constexpr auto kSecondCoreDspWait = std::chrono::milliseconds(50);
      const auto waitStart = std::chrono::steady_clock::now();
      ++secondCoreDsp.Flushed;
      if (!process.WaitSecondCoreIdle(kSecondCoreDspWait)) ++secondCoreDsp.Late;
      secondCoreDsp.WaitSeconds += SecondsSince(waitStart);
    }
    const size_t frameSampleOffset = audioSamples.size();
    const auto dspMixStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuDsp(CpuProbe::Phase::Dsp);
    if (!dspHle.ProcessFrame(process.Memory(), audioSamples, &error)) {
      throw std::runtime_error("native DSP HLE mix failed: " + error);
    }
    phaseTiming.DspMixSeconds += SecondsSince(dspMixStart);
    cpuDsp.Stop();
    pcmContinuityDiagnostics.ObserveFrame(
        dspStats.FramesMixed, std::span<const int16_t>(audioSamples)
                            .subspan(frameSampleOffset));
    ++dspStats.FramesMixed;
    dspStats.FramesSuppressed += launch.DisableAudio ? 1U : 0U;
    if (!hostServices.SignalDspAudioFrame()) {
      break;
    }
    if (!runGuest) continue;
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  AccountDspSamples(audioSamples);
  if (hostAudioStream != nullptr && !audioSamples.empty()) {
    // Stretch, resampling and the device queue run on the audio worker.
    const auto audioOutputStart = std::chrono::steady_clock::now();
    hostAudioStream->Submit(audioSamples);
    phaseTiming.AudioOutputSeconds += SecondsSince(audioOutputStart);
  } else if (hostAudioStream == nullptr && !launch.DisableAudio &&
             audioPlayer != nullptr && audioPlayer->IsInitialized() &&
             !audioSamples.empty()) {
    const auto audioOutputStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuAudio(CpuProbe::Phase::AudioOutput);
    const int32_t bufferedBefore = audioPlayer->Buffered();
    const std::vector<int16_t> *outputSamples = &audioSamples;
    if (hostAudioResampler.has_value()) {
      // Keep the host queue at its target by nudging the conversion ratio
      // (at most 0.5%, a few cents) instead of letting rate mismatch end in
      // underruns or dropped chunks.
      const double target =
          std::max<double>(audioPlayer->GetDesiredBuffered(), 1.0);
      hostAudioQueueEstimate =
          hostAudioQueueEstimate < 0.0
              ? static_cast<double>(bufferedBefore)
              : hostAudioQueueEstimate +
                    0.05 * (static_cast<double>(bufferedBefore) -
                            hostAudioQueueEstimate);
      hostAudioResampler->SetRateAdjustment(std::clamp(
          (hostAudioQueueEstimate - target) / target * 0.01, -0.005, 0.005));
      hostAudioResampled.clear();
      hostAudioResampler->Process(audioSamples, hostAudioResampled);
      outputSamples = &hostAudioResampled;
    }
    if (!outputSamples->empty()) {
      audioPlayer->Play(
          reinterpret_cast<const uint8_t *>(outputSamples->data()),
          outputSamples->size() * sizeof(int16_t));
    }
    audioOutputDiagnostics.Observe(
        frameCount, bufferedBefore, audioPlayer->Buffered(),
        outputSamples->size() / 2U, audioPlayer->GetDesiredBuffered());
    phaseTiming.AudioOutputSeconds += SecondsSince(audioOutputStart);
  }
}

void NativeA32WindowSession::AccountDspSamples(
    const std::vector<int16_t> &samples) {
  for (const int16_t sample : samples) {
    const int32_t magnitude = std::abs(static_cast<int32_t>(sample));
    dspStats.PeakMagnitude = std::max(dspStats.PeakMagnitude, magnitude);
    dspStats.NonzeroSamples += sample != 0 ? 1U : 0U;
    const uint16_t bits = static_cast<uint16_t>(sample);
    dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits);
    dspStats.PcmFnv1a64 *= 1099511628211ULL;
    dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits >> 8U);
    dspStats.PcmFnv1a64 *= 1099511628211ULL;
  }
  dspStats.SamplesMixed += samples.size() / 2U;
  if (!launch.AudioPcmDumpPath.empty()) {
    pcmDumpSamples.insert(pcmDumpSamples.end(), samples.begin(), samples.end());
  }
}

// config/second_core with the host audio stream: one deferred DSP frame, as
// the second core's idle work (NativeA32Process::RunSecondCore), on its host
// thread. It runs only while every second-core thread (the sound thread) is
// parked in its wait and no thread above the main thread's priority is ready
// on the game thread, so the in-place mix never meets the sound thread's
// writes and each frame's interrupt is consumed before the next one. The
// session state below is the game thread's under the kernel lock. A mix
// failure becomes the game thread's next Run() fault.
// - config/kernel_retake 0: the kernel lock stays held for the whole frame
//   (mix, interrupt, statistics, host audio submit). The game thread's next
//   guest exit then waits for the rest of it (test.11: thread 0's retakes,
//   guest_kernel: lock=, ran at ~72 % of dsp_core1_ms, 0.5-1.4 ms per host
//   frame, while threads 4 and 7, which no frame overlaps, waited ~0.01 ms).
// - On: under the lock the frame is claimed, its shared regions resolved
//   (BeginFrame: the only failure, before anything the guest can see) and its
//   interrupt signaled first; the mix and the submit then run with the lock
//   released, unless the interrupt readied a game-thread thread or one waits
//   on it (RunSecondCoreIdleWorkAfterEffect keeps it then; dsp_held_woken,
//   dsp_held_waiter). Nothing could tell the interrupt's place in the held
//   frame (no kernel operation ran between the mix and it), and the thread it
//   wakes runs on this host thread, after the mix. The game's interrupt event
//   is sticky (svcCreateEvent reset type 1 at 0x002FA2C4): it stays signaled
//   until the sound thread clears it, after the mix, so a game-thread wait
//   listing it starts only once the mix is over (core0_svc_gated); the game
//   thread's other SVCs during the mix see the kernel as after the whole
//   frame, and its guest code overlapped the mix before as well. The mix's
//   memory paths (sample reads) take the lock themselves; the regions are
//   accounted (EndFrame) and the statistics updated under it.
void NativeA32WindowSession::DeliverDspAudioFrameOnSecondCore() {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &dspHle = *dspHleStorage;
  --deferredDspAudioFrames;
  std::vector<int16_t> &samples = secondCoreDspSamples;
  samples.clear();
  std::string mixError;
  if (!dspMixReleasesKernel) {
    if (!dspHle.ProcessFrame(process.Memory(), samples, &mixError)) {
      throw std::runtime_error("native DSP HLE mix failed: " + mixError);
    }
    pcmContinuityDiagnostics.ObserveFrame(dspStats.FramesMixed,
                                          std::span<const int16_t>(samples));
    ++dspStats.FramesMixed;
    dspStats.FramesSuppressed += launch.DisableAudio ? 1U : 0U;
    // As the game thread's batch: with the DSP stopped the rest is dropped.
    if (!hostServices.SignalDspAudioFrame()) deferredDspAudioFrames = 0U;
    AccountDspSamples(samples);
    if (!samples.empty()) hostAudioStream->Submit(samples);
    return;
  }
  const Oot3dNativeGame::NativeDspMemoryAccess memory =
      dspHle.MemoryAccess(process.Memory());
  Oot3dNativeGame::NativeA32DspFrame frame;
  if (!dspHle.BeginFrame(memory, frame, &mixError)) {
    throw std::runtime_error("native DSP HLE mix failed: " + mixError);
  }
  const uint64_t core0Wakes = process.Core0ThreadWakes();
  // As the game thread's batch: with the DSP stopped the rest is dropped.
  if (!hostServices.SignalDspAudioFrame()) deferredDspAudioFrames = 0U;
  const Oot3dNativeGame::NativeA32SvcFilter waitsOnInterrupt =
      [&](uint32_t immediate, const oot3d::recomp::a32::GuestState &state) {
        return hostServices.SvcWaitsOnDspAudioInterrupt(immediate, state,
                                                        process.Memory());
      };
  process.RunSecondCoreIdleWorkAfterEffect(
      core0Wakes, hostServices.DspAudioInterruptHasCore0Waiter(), waitsOnInterrupt,
      [&] {
        dspHle.MixFrame(memory, frame, samples);
        if (!samples.empty()) hostAudioStream->Submit(samples);
      });
  if (!dspHle.EndFrame(memory, frame, &mixError)) {
    throw std::runtime_error("native DSP HLE mix failed: " + mixError);
  }
  pcmContinuityDiagnostics.ObserveFrame(dspStats.FramesMixed,
                                        std::span<const int16_t>(samples));
  ++dspStats.FramesMixed;
  dspStats.FramesSuppressed += launch.DisableAudio ? 1U : 0U;
  AccountDspSamples(samples);
}

// Runs the guest until it waits and drains the PICA work it submitted during the refresh.
void NativeA32WindowSession::RunGuestRefresh() {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  const auto &picaMemoryView = *picaMemoryViewStorage;
  auto &submissionQueue = *submissionQueueStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  // PPF unblocks the current guest refresh, not the next presentation.
  // The dependency capture keeps its plans for the complete visual frame.
  (void)FlushNativeDependencies();
  phaseStart = std::chrono::steady_clock::now();
  processResult =
      RunUntilGuestWait(process, launch.WholeAotBlockBudget, SecondCorePump());
  phaseTiming.GuestSeconds += SecondsSince(phaseStart);
  RequireRunnableGuest(processResult);

  phaseStart = std::chrono::steady_clock::now();
  const auto picaSubmitStart = phaseStart;
  CpuProbe::Scope cpuQueue(CpuProbe::Phase::PicaQueue);
  size_t drainPassesThisRefresh = 0;
  bool refreshHadNativeDraws = false;
  while (drainPassesThisRefresh < kMaximumPicaDrainPassesPerRefresh) {
    PublishNativeSceneView();
    // config/capture_delta: the log of the draws' register files, uniforms
    // and default attributes comes along unreplayed (below: to the plan
    // builder, or replayed here for the serial build).
    std::shared_ptr<Oot3dNativeGame::Oot3dPicaCaptureDeltaLog> drainedDelta;
    submissionQueue.TakePendingDraws(drainedDraws, drainedDelta);
    auto &draws = drainedDraws;
    auto completions = submissionQueue.TakePendingCompletions();
    auto displayTransfers = submissionQueue.TakePendingDisplayTransfers();
    auto memoryFills = submissionQueue.TakePendingMemoryFills();
    if (draws.empty() && completions.empty() &&
        displayTransfers.empty() && memoryFills.empty()) {
      break;
    }
    ++drainPassesThisRefresh;
    ++picaStats.PicaDrainPassCount;
    refreshHadNativeDraws = refreshHadNativeDraws || !draws.empty();
    const bool suppressTopScreenFrontendBackdrop =
        Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
            launch.UiProfile,
            uiLifecycleBridge.NativeFrontendPresentationActive());
    // The native menu backdrops TopScreen removes: the shared
    // CommonBackground00 of File Select and Name Entry, and the version
    // choice's own background.
    std::array<std::optional<uint32_t>, 2> frontendBackdropPhysicalAddresses{};
    if (suppressTopScreenFrontendBackdrop) {
      const std::array<std::optional<uint32_t>, 2> frontendBackdropGuestAddresses{
          uiLifecycleBridge.NativePauseSharedTextureGuestAddress(
              oot3d::ui::UiPauseSharedTextureSlot::CommonBackground00),
          uiLifecycleBridge.NativeVersionChoiceBackdropGuestAddress()};
      for (size_t index = 0; index < frontendBackdropGuestAddresses.size();
           ++index) {
        if (frontendBackdropGuestAddresses[index].has_value()) {
          frontendBackdropPhysicalAddresses[index] =
              picaMemoryView.TranslateGuest(
                  *frontendBackdropGuestAddresses[index], 1U);
        }
      }
    }
    const auto usesFrontendBackdrop =
        [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
          return std::any_of(
              state.Textures.begin(), state.Textures.end(),
              [&](const auto &texture) {
                return texture.Enabled &&
                       std::any_of(frontendBackdropPhysicalAddresses.begin(),
                                   frontendBackdropPhysicalAddresses.end(),
                                   [&](const auto &address) {
                                     return address.has_value() &&
                                            texture.PhysicalAddress == *address;
                                   });
              });
        };
    std::vector<Oot3dNativeGame::TopScreenFrontendCanvas>
        frontendBackdropCanvases;
    const auto rememberFrontendBackdropCanvas =
        [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
          if (state.Framebuffer.ColorPhysicalAddress != 0U &&
              std::none_of(frontendBackdropCanvases.begin(),
                           frontendBackdropCanvases.end(),
                           [&](const auto& canvas) {
                             return canvas.Matches(state);
                           })) {
            frontendBackdropCanvases.push_back(
                {state.Framebuffer.ColorPhysicalAddress, state.Viewport});
          }
        };
    if (suppressTopScreenFrontendBackdrop) {
      for (const auto &draw : draws) {
        if (usesFrontendBackdrop(draw.State)) {
          rememberFrontendBackdropCanvas(draw.State);
        }
      }
    }
    bool signaledPicaCompletion = false;
    size_t completionIndex = 0;
    size_t memoryFillIndex = 0;
    size_t displayTransferIndex = 0;
    const auto submitMemoryFill =
        [&](const Oot3dNativeGame::Oot3dPicaMemoryFillSubmission &fill) {
          picaSemanticTrace.RecordMemoryFill(
              presentationFrameCount, frameCount, fill);
          picaPresentationScheduler.Capture(fill);
          if (fill.CompletionId != 0U) {
            if (!fill.Interrupt.has_value() ||
                !hostServices.SignalPicaInterrupt(process.Memory(),
                                                  *fill.Interrupt)) {
              throw std::runtime_error(
                  "native PICA memory fill completion "
                  "could not enter the GSP relay queue");
            }
            if (*fill.Interrupt ==
                Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
              ++picaStats.CompletedP3dCount;
            } else if (*fill.Interrupt ==
                       Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
              ++picaStats.CompletedPpfCount;
            }
            ++picaStats.RecordedCompletionCount;
            signaledPicaCompletion = true;
          }
        };
    const auto submitDisplayTransfer =
        [&](const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
                &transfer) {
          const auto topFramebuffer = hostServices.TopFramebuffer();
          const bool selectedForTop = topFramebuffer.has_value() &&
                                      (transfer.Transfer.OutputAddress ==
                                           topFramebuffer->AddressLeft ||
                                       transfer.Transfer.OutputAddress ==
                                           topFramebuffer->AddressRight);
          picaSemanticTrace.RecordDisplayTransfer(
              presentationFrameCount, frameCount, transfer,
              selectedForTop, suppressTopScreenFrontendBackdrop);
          if (launch.ExtendedDiagnostics) {
            if (displayTransferEvents.size() == 64U) {
              displayTransferEvents.erase(displayTransferEvents.begin());
            }
            displayTransferEvents.push_back({
                {"input_address", transfer.Transfer.InputAddress},
                {"output_address", transfer.Transfer.OutputAddress},
                {"input_physical_address", transfer.InputPhysicalAddress},
                {"output_physical_address",
                 transfer.OutputPhysicalAddress},
                {"input_size", transfer.Transfer.InputSize},
                {"output_size", transfer.Transfer.OutputSize},
                {"flags", transfer.Transfer.Flags},
                {"texture_copy_bytes", transfer.Transfer.TextureCopyBytes},
                {"after_draw_submission_id",
                 transfer.AfterDrawSubmissionId},
                {"signals_guest_interrupt", transfer.SignalInterrupt},
                {"selected_for_top_at_submission", selectedForTop},
                {"topscreen_frontend_backdrop_suppression_active",
                 suppressTopScreenFrontendBackdrop},
            });
            if (transfer.Transfer.TextureCopyBytes != 0U) {
              // Same span the backend writes: byte count plus the per-row
              // gap, so every byte the copy materializes is inside the
              // watch. For a raw copy OutputSize carries the native
              // width/height pair in 16-byte units (see the bridge).
              const uint32_t copySize =
                  transfer.Transfer.TextureCopyBytes & ~15U;
              const uint32_t copyGap =
                  (transfer.Transfer.OutputSize >> 16U) * 16U;
              const uint32_t copyWidth =
                  copyGap == 0U
                      ? copySize
                      : (transfer.Transfer.OutputSize & 0xFFFFU) * 16U;
              if (copySize != 0U && copyWidth != 0U) {
                const uint64_t copyEnd =
                    uint64_t{transfer.OutputPhysicalAddress} + copySize +
                    uint64_t{(copySize - 1U) / copyWidth} * copyGap;
                const auto copySpan = static_cast<size_t>(
                    std::min<uint64_t>(copyEnd, uint64_t{1} << 32) -
                    transfer.OutputPhysicalAddress);
                // The memory object is indexed by guest addresses: the
                // transfer names a physical destination, so it has to be
                // translated before anything can be trapped. A span that
                // does not translate is skipped rather than watched at the
                // wrong address.
                const auto watchGuest = picaMemoryView.Translate(
                    transfer.OutputPhysicalAddress, copySpan);
                if (watchGuest.has_value()) {
                  const std::pair<uint32_t, uint32_t> watched{
                      *watchGuest,
                      *watchGuest + static_cast<uint32_t>(copySpan)};
                  if (std::find(rawCopyWatchRanges.begin(),
                                rawCopyWatchRanges.end(),
                                watched) == rawCopyWatchRanges.end()) {
                    rawCopyWatchRanges.push_back(watched);
                    process.Memory().SetRangeWatch(rawCopyWatchRanges);
                  } else if (!process.Memory().RangeWatchActive()) {
                    process.Memory().SetRangeWatch(rawCopyWatchRanges);
                  }
                }
              }
            }
          }
          picaPresentationScheduler.Capture(transfer);
          if (transfer.SignalInterrupt && transfer.Transfer.TextureCopyBytes == 0U) {
            if (!hostServices.SignalPicaInterrupt(
                    process.Memory(),
                    Oot3dNativeGame::Oot3dPicaInterruptId::Ppf)) {
              throw std::runtime_error(
                  "native PICA display transfer completion could not "
                  "enter the GSP relay queue");
            }
            ++picaStats.CompletedPpfCount;
            ++picaStats.RecordedCompletionCount;
            signaledPicaCompletion = true;
          }
          displayTransfersByOutput[transfer.Transfer.OutputAddress] =
              transfer;
          ++picaStats.DisplayTransferCount;
        };
    while (displayTransferIndex < displayTransfers.size() &&
           displayTransfers[displayTransferIndex].AfterDrawSubmissionId <=
               lastSubmittedDrawId) {
      submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
    }
    // Deferred planning: each draw enters the accumulator as a placeholder
    // (identity and decoded state only) and its plan is built off this
    // thread from the same immutable submission snapshot. Diagnostics that
    // read the plan here (semantic trace, extended diagnostics) keep the
    // serial build.
    std::shared_ptr<Oot3dNativeGame::Oot3dPicaDeferredPlanBatch> deferredPlans;
    if (picaPlanBuilder.Deferred() && !draws.empty() &&
        !launch.ExtendedDiagnostics && !picaSemanticTrace.Enabled()) {
      // Swaps vectors with a recycled batch: `draws` comes back empty with
      // capacity for the submission queue to fill next; nothing is copied
      // or allocated per pass.
      deferredPlans = picaPlanBuilder.BeginBatch(draws, std::move(drainedDelta));
    } else if (drainedDelta != nullptr) {
      drainedDelta->MaterializeAll(draws);
    }
    auto &drawList =
        deferredPlans != nullptr ? deferredPlans->Submissions : draws;
    for (size_t drawIndex = 0; drawIndex < drawList.size(); ++drawIndex) {
      auto &draw = drawList[drawIndex];
      while (memoryFillIndex < memoryFills.size() &&
             memoryFills[memoryFillIndex].BeforeDrawSubmissionId <=
                 draw.Id) {
        submitMemoryFill(memoryFills[memoryFillIndex++]);
      }
      const bool usesCommonBackground =
          suppressTopScreenFrontendBackdrop && usesFrontendBackdrop(draw.State);
      const bool targetsFrontendBackdrop =
          std::any_of(frontendBackdropCanvases.begin(),
                      frontendBackdropCanvases.end(),
                      [&](const auto& canvas) {
                        return canvas.Matches(draw.State);
                      });
      const bool isOpaqueTargetInitialization =
          IsNativeFrontendOpaqueTargetInitializationDraw(draw);
      // The menu's own backdrop stays (config/topscreen_menu_backdrop); only
      // its canvas clear is removed so the upper sky stays beside the menu.
      const bool suppressNativeFrontendBackdropDraw =
          Oot3dNativeGame::OmitTopScreenFrontendMenuDraw(
              suppressTopScreenFrontendBackdrop, targetsFrontendBackdrop,
              usesCommonBackground, isOpaqueTargetInitialization,
              Oot3dNativeGame::TopScreenMenuBackdropEnabled());
      if (deferredPlans != nullptr && offload.Plan) {
        // config/offload_plan: the placeholder is written where the
        // accumulator keeps it; no plan is built, moved or copied here.
        const auto placeholderStart = std::chrono::steady_clock::now();
        if (!suppressNativeFrontendBackdropDraw) {
          picaPlanBuilder.FillDeferredPlan(
              deferredPlans, drawIndex,
              picaPresentationScheduler.CaptureInPlace());
        }
        phaseTiming.PicaPlanSeconds += SecondsSince(placeholderStart);
        if (suppressNativeFrontendBackdropDraw) {
          picaStats.NativeMenuBackdropDrawOmissionCount +=
              usesCommonBackground ? 1U : 0U;
          picaStats.NativeMenuClearDrawOmissionCount +=
              isOpaqueTargetInitialization ? 1U : 0U;
        }
      } else {
      Oot3dNativeGame::Oot3dPicaVulkanDrawPlan plan;
      auto drawPhaseStart = std::chrono::steady_clock::now();
      if (deferredPlans != nullptr) {
        if (!suppressNativeFrontendBackdropDraw) {
          plan = picaPlanBuilder.MakeDeferredPlan(deferredPlans, drawIndex);
        }
      } else if (!picaPlanBuilder.Build(draw, plan, &error)) {
        throw std::runtime_error("native PICA Vulkan plan failed: " +
                                 error);
      }
      phaseTiming.PicaPlanSeconds += SecondsSince(drawPhaseStart);
      if (deferredPlans == nullptr) {
        picaSemanticTrace.RecordDraw(
            presentationFrameCount, frameCount, draw, plan,
            suppressNativeFrontendBackdropDraw, usesCommonBackground,
            isOpaqueTargetInitialization);
      }
      if (suppressNativeFrontendBackdropDraw) {
        picaStats.NativeMenuBackdropDrawOmissionCount +=
            usesCommonBackground ? 1U : 0U;
        picaStats.NativeMenuClearDrawOmissionCount +=
            isOpaqueTargetInitialization ? 1U : 0U;
      }
      if (launch.ExtendedDiagnostics) {
        drawPhaseStart = std::chrono::steady_clock::now();
        if (drawEvents.size() == 128U) {
          drawEvents.erase(drawEvents.begin());
        }
        nlohmann::json textures = nlohmann::json::array();
        for (size_t slot = 0; slot < draw.State.Textures.size();
             ++slot) {
          const auto &texture = draw.State.Textures[slot];
          if (!texture.Enabled) {
            continue;
          }
          const auto planTexture = std::find_if(
              plan.Textures.begin(), plan.Textures.end(),
              [slot](const auto &candidate) {
                return candidate.Slot == slot;
              });
          const std::span<const uint8_t> nativeBytes =
              planTexture != plan.Textures.end()
                  ? std::span<const uint8_t>(planTexture->NativeBytes)
                  : std::span<const uint8_t>();
          textures.push_back({
              {"slot", slot},
              {"width", texture.Width},
              {"height", texture.Height},
              {"physical_address", texture.PhysicalAddress},
              {"format", texture.Format},
              {"type", texture.Type},
              {"native_byte_count", nativeBytes.size()},
              {"native_byte_hash", HashDiagnosticBytes(nativeBytes)},
              {"native_nonzero_bytes",
               std::count_if(nativeBytes.begin(), nativeBytes.end(),
                             [](const uint8_t byte) {
                               return byte != 0U;
                             })},
          });
        }
        nlohmann::json vertexBindings = nlohmann::json::array();
        for (const auto &binding : plan.VertexBindings) {
          constexpr size_t kDiagnosticByteLimit = 256U;
          const auto bindingBytes = binding.ResolvedBytes();
          const size_t byteCount =
              std::min(bindingBytes.size(), kDiagnosticByteLimit);
          vertexBindings.push_back({
              {"binding", binding.Binding},
              {"byte_stride", binding.ByteStride},
              {"per_instance",
               binding.InputRate ==
                   Oot3dNativeGame::Oot3dPicaVertexInputRate::
                       PerInstance},
              {"byte_count", bindingBytes.size()},
              {"prefix_bytes",
               std::vector<uint8_t>(bindingBytes.begin(),
                                    bindingBytes.begin() + byteCount)},
          });
        }
        nlohmann::json defaultAttributes = nlohmann::json::array();
        for (size_t attributeIndex = 0;
             attributeIndex < draw.State.VertexInput.AttributeCount;
             ++attributeIndex) {
          if (!draw.State.VertexInput.Attributes[attributeIndex]
                   .Default) {
            continue;
          }
          defaultAttributes.push_back({
              {"attribute", attributeIndex},
              {"input_register",
               draw.State.ShaderInterface
                   .InputRegisterByAttribute[attributeIndex]},
              {"value", draw.Packet.DefaultAttributes[attributeIndex]},
          });
        }
        constexpr std::array<uint16_t, 6> kTevStageRegisters{
            0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
        nlohmann::json tevStages = nlohmann::json::array();
        for (const uint16_t base : kTevStageRegisters) {
          tevStages.push_back({
              {"base_register", base},
              {"sources", draw.Packet.Registers[base]},
              {"modifiers", draw.Packet.Registers[base + 1U]},
              {"operations", draw.Packet.Registers[base + 2U]},
              {"constant", draw.Packet.Registers[base + 3U]},
              {"scales", draw.Packet.Registers[base + 4U]},
          });
        }
        drawEvents.push_back({
            {"frame", frameCount},
            {"submission_id", draw.Id},
            {"command_list_address", draw.Packet.CommandListAddress},
            {"command_list_offset_words",
             draw.Packet.CommandListOffsetWords},
            {"topscreen_frontend_backdrop_suppressed",
             suppressNativeFrontendBackdropDraw},
            {"native_frontend_background_texture", usesCommonBackground},
            {"native_frontend_target_initialization",
             isOpaqueTargetInitialization},
            {"vertex_count", draw.State.VertexInput.VertexCount},
            {"vertex_offset", draw.State.VertexInput.VertexOffset},
            {"minimum_vertex_index", draw.MinimumVertexIndex},
            {"maximum_vertex_index", draw.MaximumVertexIndex},
            {"indexed", draw.State.VertexInput.Indexed},
            {"indices_are_16_bit",
             draw.State.VertexInput.IndicesAre16Bit},
            {"attribute_count", draw.State.VertexInput.AttributeCount},
            {"vulkan_base_vertex", plan.BaseVertex},
            {"vulkan_vertex_bindings", plan.VertexBindings.size()},
            {"vertex_shader_key", plan.VertexShader.StateKey},
            {"fragment_shader_key", plan.FragmentShader.StateKey},
            {"vertex_shader_source",
             std::string(plan.ResolvedVertexShaderSource())},
            {"fragment_shader_source",
             std::string(plan.ResolvedFragmentShaderSource())},
            {"vertex_uniform_floats", plan.VertexShader.Uniforms.Floats},
            {"shader_output_mask", draw.State.ShaderInterface.OutputMask},
            {"shader_output_total",
             draw.Packet.Registers[0x04FU] & 7U},
            {"geometry_shader_enabled",
             draw.State.ShaderInterface.GeometryShaderEnabled},
            {"geometry_shader_mode",
             draw.Packet.Registers[0x229U] & 3U},
            {"shader_output_maps",
             {draw.Packet.Registers[0x050U],
              draw.Packet.Registers[0x051U],
              draw.Packet.Registers[0x052U],
              draw.Packet.Registers[0x053U],
              draw.Packet.Registers[0x054U],
              draw.Packet.Registers[0x055U],
              draw.Packet.Registers[0x056U]}},
            {"framebuffer",
             {{"color_address",
               draw.State.Framebuffer.ColorPhysicalAddress},
              {"depth_address",
               draw.State.Framebuffer.DepthPhysicalAddress},
              {"width", draw.State.Framebuffer.Width},
              {"height", draw.State.Framebuffer.Height},
              {"color_format", draw.State.Framebuffer.ColorFormat},
              {"depth_format", draw.State.Framebuffer.DepthFormat},
              {"flipped", draw.State.Framebuffer.Flipped}}},
            {"viewport",
             {{"half_width", draw.State.Viewport.HalfWidth},
              {"half_height", draw.State.Viewport.HalfHeight},
              {"depth_range", draw.State.Viewport.DepthRange},
              {"near_plane", draw.State.Viewport.NearPlane},
              {"z_buffering", draw.State.Viewport.ZBuffering},
              {"corner_x", draw.State.Viewport.CornerX},
              {"corner_y", draw.State.Viewport.CornerY}}},
            {"scissor",
             {{"mode", static_cast<uint32_t>(draw.State.Scissor.Mode)},
              {"x1", draw.State.Scissor.X1},
              {"y1", draw.State.Scissor.Y1},
              {"x2", draw.State.Scissor.X2},
              {"y2", draw.State.Scissor.Y2}}},
            {"topology", static_cast<uint32_t>(draw.State.Topology)},
            {"cull_mode", static_cast<uint32_t>(draw.State.CullMode)},
            {"output_merger",
             {{"fragment_operation_mode",
               plan.State.OutputMerger.FragmentOperationMode},
              {"color_write_mask",
               plan.State.OutputMerger.ColorWriteMask},
              {"logic_operation",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.LogicOperation)},
              {"blend_enabled", plan.State.OutputMerger.Blend.Enabled},
              {"blend_color_equation",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.ColorEquation)},
              {"blend_source_color",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.SourceColor)},
              {"blend_destination_color",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.DestinationColor)},
              {"depth_test_enabled",
               plan.State.OutputMerger.Depth.TestEnabled},
              {"depth_write_enabled",
               plan.State.OutputMerger.Depth.WriteEnabled},
              {"depth_compare",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Depth.Compare)}}},
            {"textures", std::move(textures)},
            {"vertex_bindings", std::move(vertexBindings)},
            {"default_attributes", std::move(defaultAttributes)},
            {"tev_stages", std::move(tevStages)},
            {"combiner_buffer_color", draw.Packet.Registers[0x0FDU]},
        });
        phaseTiming.PicaDiagnosticsSeconds +=
            SecondsSince(drawPhaseStart);
      }
      if (!suppressNativeFrontendBackdropDraw) {
        picaPresentationScheduler.Capture(std::move(plan));
      }
      }
      while (completionIndex < completions.size() &&
             completions[completionIndex].AfterDrawSubmissionId ==
                 draw.Id) {
        const auto &completion = completions[completionIndex++];
        if (!hostServices.SignalPicaInterrupt(process.Memory(),
                                              completion.Interrupt)) {
          throw std::runtime_error(
              "native PICA draw completion could not "
              "enter the GSP relay queue");
        }
        if (completion.Interrupt ==
            Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
          ++picaStats.CompletedP3dCount;
        } else if (completion.Interrupt ==
                   Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
          ++picaStats.CompletedPpfCount;
        }
        ++picaStats.RecordedCompletionCount;
        signaledPicaCompletion = true;
      }
      lastSubmittedDrawId = draw.Id;
      while (
          displayTransferIndex < displayTransfers.size() &&
          displayTransfers[displayTransferIndex].AfterDrawSubmissionId ==
              draw.Id) {
        submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
      }
      ++picaStats.DrawCount;
    }
    // From here on the submissions belong to the plan builder.
    picaPlanBuilder.Publish(std::move(deferredPlans));
    if (completionIndex != completions.size()) {
      throw std::runtime_error(
          "native PICA completion did not match a submitted draw");
    }
    while (memoryFillIndex < memoryFills.size()) {
      submitMemoryFill(memoryFills[memoryFillIndex++]);
    }
    if (displayTransferIndex != displayTransfers.size()) {
      throw std::runtime_error("native PICA display transfer did not "
                               "match a submitted draw");
    }
    signaledPicaCompletion =
        FlushNativeDependencies() || signaledPicaCompletion;
    if (!signaledPicaCompletion) {
      break;
    }
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget, SecondCorePump());
    const double resumedGuestSeconds = SecondsSince(phaseStart);
    phaseTiming.GuestSeconds += resumedGuestSeconds;
    phaseTiming.GuestInsidePicaSubmitSeconds += resumedGuestSeconds;
    RequireRunnableGuest(processResult);
  }
  if (drainPassesThisRefresh == kMaximumPicaDrainPassesPerRefresh &&
      (!submissionQueue.PendingDraws().empty() ||
       !submissionQueue.PendingCompletions().empty() ||
       !submissionQueue.PendingDisplayTransfers().empty() ||
       !submissionQueue.PendingMemoryFills().empty())) {
    throw std::runtime_error("native PICA completion chain exceeded "
                             "the per-refresh drain limit");
  }
  picaStats.MaximumPicaDrainPassesPerRefresh = std::max<uint64_t>(
      picaStats.MaximumPicaDrainPassesPerRefresh, drainPassesThisRefresh);
  picaStats.RefreshesWithNativeDraws += refreshHadNativeDraws ? 1U : 0U;
  phaseTiming.PicaSubmitSeconds += SecondsSince(picaSubmitStart);
}

// Presents the host frame: selects the top-screen transfer and replays the visual frame or its samples.
void NativeA32WindowSession::PresentVisualFrame(
    const double &presentationElapsedSeconds,
    const float &visualInterpolationAlpha) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &api = *apiPointer;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  CpuProbe::Scope cpuVisual(CpuProbe::Phase::VisualPresentation);
  const auto visualPresentationStart = std::chrono::steady_clock::now();
  constexpr auto presentationRenderTargetNamespace =
      kVisualInterpolationRenderTargetNamespace;
  const bool lcdForceBlack = hostServices.LcdForceBlack();
  const auto topFramebuffer = hostServices.TopFramebuffer();
  const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
      *traceSelectedTopTransfer = nullptr;
  if (topFramebuffer.has_value()) {
    auto traceSelected =
        displayTransfersByOutput.find(topFramebuffer->AddressLeft);
    if (traceSelected == displayTransfersByOutput.end() &&
        topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
      traceSelected =
          displayTransfersByOutput.find(topFramebuffer->AddressRight);
    }
    if (traceSelected != displayTransfersByOutput.end()) {
      traceSelectedTopTransfer = &traceSelected->second;
    }
  }
  picaSemanticTrace.RecordPresentationSelection(
      presentationFrameCount, frameCount, lcdForceBlack,
      topFramebuffer.has_value()
          ? std::optional<uint32_t>(topFramebuffer->AddressLeft)
          : std::nullopt,
      topFramebuffer.has_value()
          ? std::optional<uint32_t>(topFramebuffer->AddressRight)
          : std::nullopt,
      traceSelectedTopTransfer);
  if (!lcdForceBlack) {
    if (topFramebuffer.has_value()) {
      auto selected =
          displayTransfersByOutput.find(topFramebuffer->AddressLeft);
      if (selected == displayTransfersByOutput.end() &&
          topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
        selected =
            displayTransfersByOutput.find(topFramebuffer->AddressRight);
      }
      if (selected != displayTransfersByOutput.end()) {
        // Guest side: transfer selection, frame completion and the
        // guest-memory reads the renderer needs. The rest runs as one
        // render command, in renderer order.
        const auto topSelection = selected->second;
        std::shared_ptr<Oot3dNativeGame::Oot3dPicaVisualFrame>
            completedFrame;
        // Guest frames completed before the selected one while the host
        // fell behind: canonical work and interpolation history only.
        std::shared_ptr<std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>
            supersededFrames;
        std::vector<Fast::Oot3d::GrassInteractor> frameActors;
        bool suppressTopScreenBackdrop = false;
        if (topSelection.CompletionId !=
            lastSelectedTopTransferCompletionId) {
          lastSelectedTopTransferCompletionId =
              topSelection.CompletionId;
          ++picaStats.SelectedTopTransferCount;
          // TopScreen draws the relocated lower menu onto the upper target
          // after the upper snapshot: that work completes the same frame.
          const bool relocatedFrontend =
              Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
                  launch.UiProfile,
                  uiLifecycleBridge.NativeFrontendPresentationActive());
          const auto frontendMenuCanvases =
              Oot3dNativeGame::TopScreenFrontendMenuCanvases();
          auto finishedFrames = picaPresentationScheduler.FinishFrames(
              topSelection, topDisplayOutputs,
              relocatedFrontend
                  ? std::span<const Oot3dNativeGame::Oot3dPicaViewportState>(
                        frontendMenuCanvases)
                  : std::span<const Oot3dNativeGame::Oot3dPicaViewportState>());
          if (std::find(topDisplayOutputs.begin(), topDisplayOutputs.end(),
                        topSelection.OutputPhysicalAddress) == topDisplayOutputs.end()) {
            if (topDisplayOutputs.size() == 4U)
              topDisplayOutputs.erase(topDisplayOutputs.begin());
            topDisplayOutputs.push_back(topSelection.OutputPhysicalAddress);
          }
          std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame> finished;
          if (!finishedFrames.empty()) {
            finished = std::move(finishedFrames.back());
            finishedFrames.pop_back();
            if (!finishedFrames.empty())
              supersededFrames = std::make_shared<
                  std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>(
                  std::move(finishedFrames));
          }
          if (finished.has_value()) {
            // The grass colliders are read only by the Vulkan/NRI grass;
            // config/offload_actors skips the walk on the GL backend.
            const bool actorsRead =
                !offload.Actors || api.PicaNativeNamespaceObservable();
            if (actorsRead) {
              frameActors = Oot3dNativeGame::CollectNativeActorInteractions(
                  process.Memory(),
                  sceneViewProbe.Stats().LastPlayStateAddress,
                  finished->Sequence);
            } else if (Fast::Deko3dGrassWantsColliders()) {
              // deko3d grass (config/grass = draw): Link and Epona only.
              frameActors = Oot3dNativeGame::CollectNativeGrassColliders(
                  process.Memory(),
                  sceneViewProbe.Stats().LastPlayStateAddress,
                  finished->Sequence);
            }
            ++visualStats.SnapshotCount;
            suppressTopScreenBackdrop = relocatedFrontend;
            completedFrame =
                std::make_shared<Oot3dNativeGame::Oot3dPicaVisualFrame>(
                    std::move(*finished));
            hostFrameCompletedStep = true;
          }
        }
        renderStream.Submit([&, topSelection, completedFrame, supersededFrames,
                             frameActors, suppressTopScreenBackdrop,
                             presentationElapsedSeconds,
                             visualInterpolationAlpha, frameCount = frameCount,
                             guestReferences =
                                 CollectGuestCopyReferences()] {
        if (rendererFrameSkipped) return;
        struct RenderCost {
          double &Total;
          std::chrono::steady_clock::time_point Start =
              std::chrono::steady_clock::now();
          ~RenderCost() { Total += SecondsSince(Start) * 1000.0; }
        } renderCost{renderFrameCpuMs};
        GpuFrameTimer().BeginFrame();
        EnsureInterpolationPacing();
        // Deferred plans of the closed frames are built (or taken from the
        // plan worker) here, on the render thread, before any use.
        if (completedFrame != nullptr) {
          Fast::GlPerf::SectionScope perfResolve(
              Fast::GlPerf::Section::ResolvePlans);
          Oot3dNativeGame::HangWatch::StageScope hangResolve(
              Oot3dNativeGame::HangWatch::Stage::RenderResolvePlans);
          if (supersededFrames != nullptr) {
            for (auto &superseded : *supersededFrames) {
              if (!Oot3dNativeGame::ResolveOot3dPicaDeferredVisualFrame(
                      superseded, &renderError))
                throw std::runtime_error(renderError);
            }
          }
          if (!Oot3dNativeGame::ResolveOot3dPicaDeferredVisualFrame(
                  *completedFrame, &renderError))
            throw std::runtime_error(renderError);
        }
        guestCopyReferences = guestReferences;
        uint64_t nativeReplaysSkippedThisFrame = 0U;
        uint64_t reducedRefusedThisFrame = 0U;
        uint64_t repeatFallbacksThisFrame = 0U;
        bool presentedVisualSample = false;
        bool selectedNewVisualFrame = false;
        {
          if (completedFrame != nullptr && supersededFrames != nullptr) {
            for (auto &superseded : *supersededFrames) {
              auto &sample = visualFrameSampleScratch;
              (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                  superseded, sample, true);
              const auto nativeTemporalSample =
                  Fast::Oot3d::BuildNativeFrameTemporalSample(
                      frameCompositionPolicy,
                      Fast::Oot3d::NativeFrameTemporalSampleKind::
                          Authoritative,
                      superseded.Sequence, superseded.Sequence,
                      visualContinuityEpoch, 1.0F,
                      static_cast<float>(presentationElapsedSeconds));
              if (skipNativeReplay) {
                ++nativeReplaysSkippedThisFrame;
              } else {
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                        NativeFrame,
                    false, nativeTemporalSample,
                    kNativeDependencyRenderTargetNamespace);
              }
              if (visualInterpolationEnabled) {
                Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                    superseded, suppressTopScreenBackdrop);
                preparedVisualTransition.Reset();
                if (latestVisualFrame.has_value())
                  previousVisualFrame = std::move(latestVisualFrame);
                latestVisualFrame = std::move(superseded);
              }
            }
          }
          if (completedFrame != nullptr) {
            std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame>
                currentVisualFrame(std::move(*completedFrame));
            Fast::Oot3d::GrassInteractionBridge::Instance().PublishFrame(
                currentVisualFrame->Sequence, frameActors);
            selectedNewVisualFrame = true;
            renderFrameKind =
                static_cast<uint8_t>(Oot3dNativeGame::RenderFrameKind::Step);
            // Dynamic resolution moves with the step frame: the backend
            // applies it at the top-screen target's next colour clear (this
            // step's), and the in-between frame that follows keeps it.
            Fast::PicaGlDynamicScale::gState.Requested =
                dynamicResolutionActive ? dynamicResolution.Scale() : 0.0F;
            // tour19: the scale applies at this frame's colour clear or, if
            // none comes, at its first draw into the target.
            ++Fast::PicaGlDynamicScale::gState.FrameSerial;
            auto &sample = visualFrameSampleScratch;
            // Keep the canonical target complete independently of the
            // delayed visual target. Raw-copy prefixes have already
            // executed; only the native suffix remains here.
            (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                *currentVisualFrame, sample, true);
            const auto nativeTemporalSample =
                Fast::Oot3d::BuildNativeFrameTemporalSample(
                    frameCompositionPolicy,
                    Fast::Oot3d::NativeFrameTemporalSampleKind::
                        Authoritative,
                    currentVisualFrame->Sequence,
                    currentVisualFrame->Sequence, visualContinuityEpoch,
                    1.0F, static_cast<float>(presentationElapsedSeconds));
            // The native namespace exists for read-backs into guest memory;
            // a backend that never reads it (GL) skips the replay, a whole
            // scene drawn for nothing (config/native_replay=1 keeps it).
            if (skipNativeReplay) {
              ++nativeReplaysSkippedThisFrame;
            } else {
              ExecuteVisualSample(
                  sample,
                  Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                      NativeFrame,
                  false, nativeTemporalSample,
                  kNativeDependencyRenderTargetNamespace);
            }
            Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                *currentVisualFrame, suppressTopScreenBackdrop);
            if (!visualInterpolationEnabled) {
              if (Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample)) {
                const auto temporalSample =
                    Fast::Oot3d::BuildNativeFrameTemporalSample(
                        frameCompositionPolicy,
                        Fast::Oot3d::NativeFrameTemporalSampleKind::
                            Authoritative,
                        currentVisualFrame->Sequence,
                        currentVisualFrame->Sequence,
                        visualContinuityEpoch, 1.0F,
                        static_cast<float>(
                            presentationElapsedSeconds));
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::
                        Oot3dPicaPresentationExecutionKind::CurrentFrame,
                    true, temporalSample);
                ++visualStats.DirectFramesPresented;
                ++visualStats.SamplesPresented;
                visualStats.SampleDrawCount += sample.Draws.size();
                presentedVisualSample = true;
              }
              // Direct gameplay modes retain no cross-frame matching
              // history. The scheduler keeps only the executed scanout
              // snapshot needed if presentation temporarily outruns
              // simulation.
              preparedVisualTransition.Reset();
              previousVisualFrame.reset();
              latestVisualFrame.reset();
              latestVisualTransitionContinuous = false;
            } else {
              bool resynchronizeVisualNamespace =
                  !latestVisualFrame.has_value();
              if (latestVisualFrame.has_value()) {
                const bool transitionPrepared =
                    preparedVisualTransition.Prepare(
                        *latestVisualFrame, *currentVisualFrame,
                        &visualInterpolationTiming);
                const auto &transition =
                    preparedVisualTransition.Stats();
                const float sampleAlpha =
                    visualSampleCadence.Resolve(
                        true, visualInterpolationAlpha);
                const bool visualSampleReady =
                    transitionPrepared &&
                    preparedVisualTransition.Sample(
                        *latestVisualFrame, *currentVisualFrame,
                        sampleAlpha, sample,
                        &visualInterpolationTiming);
                ++visualStats.TransitionCount;
                visualStats.MatchedDrawCount += transition.MatchedDraws;
                visualStats.StrictUniqueMatchedDrawCount +=
                    transition.StrictUniqueMatchedDraws;
                visualStats.StrictOrdinalMatchedDrawCount +=
                    transition.StrictOrdinalMatchedDraws;
                visualStats.StructuralOrdinalMatchedDrawCount +=
                    transition.StructuralOrdinalMatchedDraws;
                visualStats.PipelineOrdinalMatchedDrawCount +=
                    transition.PipelineOrdinalMatchedDraws;
                visualStats.ChangedContinuousDrawCount +=
                    transition.ChangedContinuousDraws;
                visualStats.ChangedPerInstanceVertexDrawCount +=
                    transition.ChangedPerInstanceVertexDraws;
                visualStats.ChangedPerVertexDrawCount +=
                    transition.ChangedPerVertexDraws;
                visualStats.AmbiguousPreviousDrawCount +=
                    transition.AmbiguousPreviousDraws;
                visualStats.AmbiguousCurrentDrawCount +=
                    transition.AmbiguousCurrentDraws;
                visualStats.UnmatchedPreviousDrawCount +=
                    transition.UnmatchedPreviousDraws;
                visualStats.UnmatchedCurrentDrawCount +=
                    transition.UnmatchedCurrentDraws;
                const double continuityThreshold =
                    visualContinuityTracker.CurrentThreshold();
                const bool continuousTransition =
                    transitionPrepared &&
                    visualContinuityTracker.Accept(transition);
                latestVisualTransitionContinuous = continuousTransition;
                visualStats.DiscontinuityCount +=
                    continuousTransition ? 0U : 1U;
                {
                  const auto &unmatched =
                      preparedVisualTransition.UnmatchedStats();
                  auto &totals = visualUnmatchedTotals;
                  ++totals.Transitions;
                  totals.WithUnmatched +=
                      transition.UnmatchedPreviousDraws +
                                  transition.UnmatchedCurrentDraws !=
                              0U
                          ? 1U
                          : 0U;
                  totals.UnmatchedPrevious += transition.UnmatchedPreviousDraws;
                  totals.UnmatchedCurrent += transition.UnmatchedCurrentDraws;
                  totals.CarriedCurrent += unmatched.CarriedCurrent;
                  totals.InsertedPrevious += unmatched.InsertedPrevious;
                  totals.WithoutCamera += unmatched.WithoutCamera;
                  totals.CameraMeasured += unmatched.CameraMeasured ? 1U : 0U;
                  totals.Discontinuities += continuousTransition ? 0U : 1U;
                }
                if (launch.ExtendedDiagnostics) {
                  if (visualTransitionEvents.size() == 1024U) {
                    visualTransitionEvents.erase(
                        visualTransitionEvents.begin());
                  }
                  visualTransitionEvents.push_back({
                      {"host_frame", frameCount},
                      {"previous_sequence",
                       latestVisualFrame->Sequence},
                      {"current_sequence",
                       currentVisualFrame->Sequence},
                      {"matched_draws", transition.MatchedDraws},
                      {"strict_unique_matched_draws",
                       transition.StrictUniqueMatchedDraws},
                      {"strict_ordinal_matched_draws",
                       transition.StrictOrdinalMatchedDraws},
                      {"structural_ordinal_matched_draws",
                       transition.StructuralOrdinalMatchedDraws},
                      {"pipeline_ordinal_matched_draws",
                       transition.PipelineOrdinalMatchedDraws},
                      {"changed_per_instance_vertex_draws",
                       transition.ChangedPerInstanceVertexDraws},
                      {"changed_per_vertex_draws",
                       transition.ChangedPerVertexDraws},
                      {"unmatched_previous_draws",
                       transition.UnmatchedPreviousDraws},
                      {"unmatched_current_draws",
                       transition.UnmatchedCurrentDraws},
                      {"top_target_matched_draws",
                       transition.TopTargetMatchedDraws},
                      {"matched_draw_coverage",
                       transition.MatchedDrawCoverage},
                      {"median_normalized_continuous_delta",
                       transition.MedianNormalizedContinuousDelta},
                      {"p90_normalized_continuous_delta",
                       transition.P90NormalizedContinuousDelta},
                      {"continuity_threshold", continuityThreshold},
                      {"continuous", continuousTransition},
                  });
                }
                if (continuousTransition && visualSampleReady) {
                  const auto temporalSample =
                      Fast::Oot3d::BuildNativeFrameTemporalSample(
                          frameCompositionPolicy,
                          Fast::Oot3d::NativeFrameTemporalSampleKind::
                              Transition,
                          latestVisualFrame->Sequence,
                          currentVisualFrame->Sequence,
                          visualContinuityEpoch, sampleAlpha,
                          static_cast<float>(
                              presentationElapsedSeconds));
                  ExecuteVisualSample(
                      sample,
                      Oot3dNativeGame::
                          Oot3dPicaPresentationExecutionKind::
                              InterpolatedFrame,
                      true, temporalSample);
                  ++visualStats.SamplesPresented;
                  visualStats.SampleDrawCount += sample.Draws.size();
                  visualStats.InterpolatedDrawCount +=
                      sample.InterpolatedDraws;
                  visualStats.InterpolatedPerInstanceVertexDrawCount +=
                      sample.InterpolatedPerInstanceVertexDraws;
                  visualStats.InterpolatedPerVertexDrawCount +=
                      sample.InterpolatedPerVertexDraws;
                  presentedVisualSample = true;
                } else {
                  resynchronizeVisualNamespace = true;
                }
              }
              if (resynchronizeVisualNamespace &&
                  Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample)) {
                preparedVisualTransition.Reset();
                ++visualContinuityEpoch;
                const auto temporalSample =
                    Fast::Oot3d::BuildNativeFrameTemporalSample(
                        frameCompositionPolicy,
                        Fast::Oot3d::NativeFrameTemporalSampleKind::
                            Resynchronized,
                        currentVisualFrame->Sequence,
                        currentVisualFrame->Sequence,
                        visualContinuityEpoch, 1.0F,
                        static_cast<float>(
                            presentationElapsedSeconds));
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::
                        Oot3dPicaPresentationExecutionKind::CurrentFrame,
                    true, temporalSample);
                ++visualStats.NamespaceResyncCount;
                ++visualStats.SamplesPresented;
                visualStats.SampleDrawCount += sample.Draws.size();
                presentedVisualSample = true;
              }
              if (latestVisualFrame.has_value()) {
                previousVisualFrame = std::move(latestVisualFrame);
              } else {
                latestVisualTransitionContinuous = false;
              }
              latestVisualFrame = std::move(currentVisualFrame);
            }
          }
        }
        if (!selectedNewVisualFrame && !presentedVisualSample &&
            visualInterpolationEnabled &&
            latestVisualTransitionContinuous &&
            previousVisualFrame.has_value() &&
            latestVisualFrame.has_value() &&
            preparedVisualTransition.Ready()) {
          auto &sample = visualFrameSampleScratch;
          const float sampleAlpha =
              visualSampleCadence.Resolve(
                  false, visualInterpolationAlpha);
          // Paced: the in-between frame at full or reduced resolution, or
          // dropped (the step image again) when the pair would not fit.
          // config/fps60_mode: at the locked 30 every in-between frame is
          // dropped (the step image again); otherwise the per-frame
          // governor (which drops single frames only in legacy).
          auto level =
              cadenceGovernor.DropsInBetween()
                  ? Oot3dNativeGame::InterpolatedSampleLevel::Dropped
                  : interpolationPacing.Decide();
          if (level == Oot3dNativeGame::InterpolatedSampleLevel::Dropped) {
            if (lastPresentedVisual.has_value() &&
                picaPresentationScheduler.HasSnapshot(
                    lastPresentedVisual->first,
                    lastPresentedVisual->second)) {
              const auto started = std::chrono::steady_clock::now();
              if (!picaPresentationScheduler.PresentExisting(
                      api, lastPresentedVisual->first,
                      lastPresentedVisual->second, &renderError)) {
                throw std::runtime_error(
                    "native top-screen dropped in-between frame failed: " +
                    renderError);
              }
              rendererTiming->VisualReplaySeconds += SecondsSince(started);
              renderFrameKind = static_cast<uint8_t>(
                  Oot3dNativeGame::RenderFrameKind::Dropped);
              presentedVisualSample = true;
            } else {
              level = Oot3dNativeGame::InterpolatedSampleLevel::Full;
            }
          }
          if (!presentedVisualSample &&
              preparedVisualTransition.Sample(
                  *previousVisualFrame, *latestVisualFrame, sampleAlpha,
                  sample, &visualInterpolationTiming)) {
            uint64_t sampleNamespace = kVisualInterpolationRenderTargetNamespace;
            if (level == Oot3dNativeGame::InterpolatedSampleLevel::Reduced) {
              if (Oot3dNativeGame::SampleSuitsReducedReplay(
                      sample, reducedReplayScratch)) {
                sampleNamespace = kReducedVisualRenderTargetNamespace;
              } else {
                level = Oot3dNativeGame::InterpolatedSampleLevel::Full;
                ++reducedRefusedThisFrame;
              }
            }
            const auto temporalSample =
                Fast::Oot3d::BuildNativeFrameTemporalSample(
                    frameCompositionPolicy,
                    Fast::Oot3d::NativeFrameTemporalSampleKind::
                        Transition,
                    previousVisualFrame->Sequence,
                    latestVisualFrame->Sequence,
                    visualContinuityEpoch, sampleAlpha,
                    static_cast<float>(presentationElapsedSeconds));
            ExecuteVisualSample(
                sample,
                Oot3dNativeGame::
                    Oot3dPicaPresentationExecutionKind::RepeatedFrame,
                true, temporalSample, sampleNamespace);
            renderFrameKind = static_cast<uint8_t>(
                Oot3dNativeGame::RenderFrameKindOf(level));
            ++visualStats.SamplesPresented;
            visualStats.SampleDrawCount += sample.Draws.size();
            visualStats.InterpolatedDrawCount += sample.InterpolatedDraws;
            visualStats.InterpolatedPerInstanceVertexDrawCount +=
                sample.InterpolatedPerInstanceVertexDraws;
            visualStats.InterpolatedPerVertexDrawCount +=
                sample.InterpolatedPerVertexDraws;
            presentedVisualSample = true;
          }
        }
        if (!presentedVisualSample &&
            picaPresentationScheduler.HasSnapshot(
                topSelection,
                presentationRenderTargetNamespace)) {
          const auto started = std::chrono::steady_clock::now();
          if (!picaPresentationScheduler.PresentExisting(
                  api, topSelection,
                  presentationRenderTargetNamespace, &renderError)) {
            throw std::runtime_error(
                "native top-screen scheduled scanout failed: " +
                renderError);
          }
          rendererTiming->VisualReplaySeconds += SecondsSince(started);
          ++visualStats.ReusedSnapshotPresentations;
        } else if (!presentedVisualSample && visualInterpolationEnabled &&
                   lastPresentedVisual.has_value() &&
                   picaPresentationScheduler.HasSnapshot(
                       lastPresentedVisual->first,
                       lastPresentedVisual->second)) {
          // The delayed visual namespace holds the previous guest frame,
          // not the one just selected: show the last presented sample
          // again rather than nothing.
          const auto started = std::chrono::steady_clock::now();
          if (!picaPresentationScheduler.PresentExisting(
                  api, lastPresentedVisual->first,
                  lastPresentedVisual->second, &renderError)) {
            throw std::runtime_error(
                "native top-screen repeated scanout failed: " + renderError);
          }
          rendererTiming->VisualReplaySeconds += SecondsSince(started);
          ++visualStats.ReusedSnapshotPresentations;
          ++repeatFallbacksThisFrame;
        }
        const uint64_t supersededThisFrame =
            supersededFrames != nullptr ? supersededFrames->size() : 0U;
        if (nativeReplaysSkippedThisFrame != 0U ||
            reducedRefusedThisFrame != 0U || repeatFallbacksThisFrame != 0U ||
            supersededThisFrame != 0U) {
          std::lock_guard lock(interpolationPerfMutex);
          interpolationPerf.SupersededFrames += supersededThisFrame;
          interpolationPerf.NativeReplaysSkipped += nativeReplaysSkippedThisFrame;
          interpolationPerf.ReducedRefused += reducedRefusedThisFrame;
          interpolationPerf.RepeatFallbacks += repeatFallbacksThisFrame;
        }
        {
          std::lock_guard lock(interpolationPerfMutex);
          interpolationPerf.VisualTiming = visualInterpolationTiming;
          interpolationPerf.Unmatched = visualUnmatchedTotals;
        }
        });
      }
    }

    const bool presentNativeBottomFrontend =
        Oot3dNativeGame::ShouldPresentNativeBottomFrontend(
            launch.UiProfile,
            uiLifecycleBridge.NativeFrontendPresentationActive());
    const auto bottomFramebuffer = hostServices.BottomFramebuffer();
    if (presentNativeBottomFrontend &&
        bottomFramebuffer.has_value()) {
      ++picaStats.NativeFrontendPresentationActiveCount;
      auto selectedBottom =
          displayTransfersByOutput.find(bottomFramebuffer->AddressLeft);
      if (selectedBottom == displayTransfersByOutput.end() &&
          bottomFramebuffer->AddressRight != 0U &&
          bottomFramebuffer->AddressRight !=
              bottomFramebuffer->AddressLeft) {
        selectedBottom = displayTransfersByOutput.find(
            bottomFramebuffer->AddressRight);
      }
      picaStats.NativeFrontendBottomTransferHitCount +=
          selectedBottom != displayTransfersByOutput.end() ? 1U : 0U;
      if (selectedBottom != displayTransfersByOutput.end()) {
        renderStream.Submit(
            [&, bottomSelection = selectedBottom->second] {
              if (rendererFrameSkipped ||
                  !picaPresentationScheduler.HasSnapshot(
                      bottomSelection,
                      presentationRenderTargetNamespace)) {
                return;
              }
              if (!picaPresentationScheduler.PresentExisting(
                      api, bottomSelection,
                      presentationRenderTargetNamespace, &renderError)) {
                throw std::runtime_error(
                    "native bottom-screen frontend presentation "
                    "failed: " + renderError);
              }
              ++picaStats.NativeBottomFrontendPresentationCount;
            });
      }
    }
  }
  phaseTiming.VisualPresentationSeconds +=
      SecondsSince(visualPresentationStart);
}

#if defined(TRIAEVUM_DEVELOPER_TOOLS)
void NativeA32WindowSession::UpdateDevFreeCameraTour(uint64_t guestFrame) {
  if (devTourPoses.empty() || devTourIndex >= static_cast<int>(devTourPoses.size()) || devWarp == nullptr) return;
  constexpr uint32_t kStartFrames = 90U;  // the scene's title card fades
  // Held for 45 frames in which the game took the free view (an intro or
  // other cutscene camera does not count), at most 900 frames.
  constexpr uint32_t kHoldFrames = 45U;
  constexpr uint32_t kGiveUpFrames = 900U;
  constexpr float kRadians = 3.14159265F / 180.0F;
  const auto status = devWarp->Status();
  const auto pose = [&](int index) {
    const auto &p = devTourPoses[static_cast<size_t>(index)];
    const float yaw = devTourAnchorYaw * kRadians;
    const std::array<float, 3> forward{std::sin(yaw), 0.0F, std::cos(yaw)};
    const std::array<float, 3> right{std::cos(yaw), 0.0F, -std::sin(yaw)};
    std::array<float, 3> eye{p.Forward, p.Right, p.Up};
    if (!p.Absolute)
      for (size_t axis = 0; axis < 3; ++axis)
        eye[axis] = devTourAnchor[axis] + forward[axis] * p.Forward + right[axis] * p.Right + (axis == 1 ? p.Up : 0.0F);
    const float viewYaw = p.Absolute ? p.Yaw : devTourAnchorYaw + p.Yaw;
    nativeCandidateDispatch.DevFreeCamera->SetPose(eye, viewYaw, p.Pitch);
    devTourAppliedAtPose = nativeCandidateDispatch.DevFreeCamera->AppliedCount();
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR pose=%d eye=%.1f,%.1f,%.1f yaw=%.1f pitch=%.1f frame=%llu\n", index,
                 eye[0], eye[1], eye[2], viewYaw, p.Pitch, static_cast<unsigned long long>(guestFrame));
  };
  if (devTourIndex < 0) {
    // A boot warp point is requested once gameplay starts: wait for it too.
    if (status.Phase != Oot3dNativeGame::DevWarpPhase::Idle) devTourAfterWarp = true;
    const bool ready = devTourAfterWarp ? status.Phase == Oot3dNativeGame::DevWarpPhase::Arrived
                                        : status.Observation.Gameplay && status.Observation.Player;
    if (!ready) {
      devTourFrames = 0;
      return;
    }
    if (++devTourFrames < kStartFrames) return;
    devTourAnchor = {status.Observation.Position[0], status.Observation.Position[1], status.Observation.Position[2]};
    devTourAnchorYaw = static_cast<float>(status.Observation.RotY) * (180.0F / 32768.0F);
    devTourIndex = 0;
    devTourFrames = 0;
    pose(0);
    return;
  }
  ++devTourFrames;
  // Only frames drawn from the main camera count: PlayState +0xA64 is the
  // active camera's index (cameras at +0xA54), 0 the main camera.
  int16_t activeCamera = -1;
  const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
  if (playState != 0U) (void)processStorage->Memory().Read16(playState + 0xA64U, reinterpret_cast<uint16_t *>(&activeCamera));
  const uint64_t appliedNow = nativeCandidateDispatch.DevFreeCamera->AppliedCount();
  if (activeCamera != 0) devTourAppliedAtPose = appliedNow;
  const uint64_t applied = appliedNow - devTourAppliedAtPose;
  if (applied < kHoldFrames && devTourFrames < kGiveUpFrames) return;
  char name[32];
  std::snprintf(name, sizeof(name), "tour-%02d.bmp", devTourIndex);
  pendingForcedScreenshot = devTourDirectory / name;
  std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR capture=%s applied=%llu%s\n",
               pendingForcedScreenshot.string().c_str(), static_cast<unsigned long long>(applied),
               applied < kHoldFrames ? " not-applied" : "");
  devTourFrames = 0;
  if (++devTourIndex < static_cast<int>(devTourPoses.size()))
    pose(devTourIndex);
  else
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR done\n");
}

void NativeA32WindowSession::UpdateDevLab(uint64_t guestFrame) {
  using Json = nlohmann::json;
  auto &camera = *nativeCandidateDispatch.DevFreeCamera;
  const auto status = devWarp->Status();
  const auto &seen = status.Observation;
  const auto place = [&] {
    return Json{{"frame", guestFrame},
                {"scene", seen.Scene},
                {"room", seen.Room},
                {"position", {seen.Position[0], seen.Position[1], seen.Position[2]}},
                {"yaw", static_cast<float>(seen.RotY) * (180.0F / 32768.0F)},
                {"gameplay", seen.Gameplay}};
  };
  const auto finish = [&](std::string_view result, Json detail) {
    devLab->Answer(*devLabCommand, result, detail);
    devLabCommand.reset();
  };
  const auto vector3 = [](const Json &value) {
    return std::array<float, 3>{value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
  };
  if (!devLabCommand) {
    devLabCommand = devLab->Next();
    if (!devLabCommand) return;
    devLabFrames = 0;
    devLabSawArrival = false;
    const auto &command = *devLabCommand;
    const std::string op = command["op"].get<std::string>();
    try {
      if (op == "warp" && command.contains("better")) {
        // An entrance of the Debug Warp Screen's Better list ([scene, entrance]
        // indices), with age ("child"/"adult") and time ("day"/"night").
        const auto scenes = Oot3dNativeGame::DevWarpBetterScenes();
        const auto scene = command["better"].at(0).get<size_t>();
        const auto entrance = command["better"].at(1).get<size_t>();
        if (scene >= scenes.size() || entrance >= scenes[scene].Count)
          throw std::out_of_range("no such Better scene/entrance");
        devWarp->WarpTo(Oot3dNativeGame::DevWarpBetterEntrances()[scenes[scene].First + entrance],
                        command.value("age", std::string("child")) == "adult" ? Oot3dNativeGame::DevWarpAge::Adult
                                                                               : Oot3dNativeGame::DevWarpAge::Child,
                        command.value("time", std::string("day")) == "night");
      } else if (op == "warp") {
        // Link respawns at the position in the room (SoH's warp point), facing yaw.
        Oot3dNativeGame::DevWarpPoint point;
        point.EntranceId = command.value("entrance", 0);
        point.RoomNum = static_cast<int8_t>(command.value("room", 0));
        const auto position = vector3(command.at("position"));
        std::copy(position.begin(), position.end(), point.Position);
        point.RotY = static_cast<int16_t>(std::lround(command.value("yaw", 0.0) * 32768.0 / 180.0));
        devWarp->WarpToPlace("lab " + command.value("id", Json()).dump(), point);
      } else if (op == "camera") {
        camera.SetPose(vector3(command.at("eye")), command.value("yaw", 0.0F), command.value("pitch", 0.0F));
        devLabAppliedAt = camera.AppliedCount();
      } else if (op == "camera_off") {
        camera.SetActive(false);
        finish("ok", place());
      } else if (op == "stick") {
        // Held on the circle pad from the next input frame until another
        // "stick" (x/y in -1..1; y=1 walks forward, 0/0 releases).
        devLabStickX = static_cast<int16_t>(std::lround(std::clamp(command.value("x", 0.0), -1.0, 1.0) * 154.0));
        devLabStickY = static_cast<int16_t>(std::lround(std::clamp(command.value("y", 0.0), -1.0, 1.0) * 154.0));
        // "buttons": 3DS buttons held with it (["R"] raises the shield).
        devLabButtons = 0U;
        static constexpr std::pair<const char *, ThreeDsRecomp::Input::Button> kNames[] = {
            {"A", ThreeDsRecomp::Input::Button::A}, {"B", ThreeDsRecomp::Input::Button::B},
            {"X", ThreeDsRecomp::Input::Button::X}, {"Y", ThreeDsRecomp::Input::Button::Y},
            {"L", ThreeDsRecomp::Input::Button::L}, {"R", ThreeDsRecomp::Input::Button::R},
            {"ZL", ThreeDsRecomp::Input::Button::Zl}, {"ZR", ThreeDsRecomp::Input::Button::Zr}};
        for (const auto &name : command.value("buttons", Json::array())) {
          for (const auto &[label, button] : kNames)
            if (name == label) devLabButtons |= ThreeDsRecomp::Input::ButtonMask(button);
        }
        finish("ok", {{"frame", guestFrame}, {"x", devLabStickX}, {"y", devLabStickY}, {"buttons", devLabButtons}});
      } else if (op == "capture") {
        // Written by the render stream with this frame; the file appears shortly after.
        pendingForcedScreenshot = command.at("path").get<std::string>();
        finish("ok", {{"frame", guestFrame}, {"path", pendingForcedScreenshot.string()}});
      } else if (op == "status") {
        const auto environment = sceneEnvironment->Snapshot();
        auto detail = place();
        detail["playState"] = sceneViewProbe.Stats().LastPlayStateAddress;
        detail["warp"] = status.Message;
        detail["profile"] = {{"file", environment.FileExists ? environment.Path.string() : std::string()},
                             {"enabled", environment.Profile.Enabled},
                             {"revision", environment.Revision},
                             {"error", environment.Error}};
        finish("ok", detail);
      } else if (op == "graphics") {
        // A partial "Graphics" block, applied like an F1 commit (not saved),
        // as the qualification schedule does.
        auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
        const auto current = graphicsSettings.Snapshot();
        const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
            Oot3dNativeGame::MergeScheduledGraphics(Fast::Oot3d::SerializeGraphicsSettings(current),
                                                    command.at("graphics")),
            current);
        const auto applied = graphicsSettings.Apply(decoded.Value, false);
        const auto issueDetails = [](const auto& issues, const char* phase) {
          auto details = Json::array();
          for (const auto& issue : issues) {
            details.push_back({{"field", issue.Field}, {"message", issue.Message}, {"corrected", issue.Corrected}});
            std::fprintf(stderr, "TRIAEVUM_DEV_LAB_GRAPHICS_ISSUE phase=%s field=\"%s\" corrected=%d message=\"%s\"\n",
                         phase, issue.Field.c_str(), issue.Corrected, issue.Message.c_str());
          }
          return details;
        };
        const auto effective = graphicsSettings.SnapshotForRendering();
        const auto effectiveJson = Fast::Oot3d::SerializeGraphicsSettings(effective.Value);
        finish(applied.Accepted() && !decoded.UnsupportedFutureVersion ? "ok" : "failed",
               {{"frame", guestFrame}, {"decodeIssues", decoded.Issues.size()},
                {"applyIssues", applied.Issues.size()},
                {"decodeIssueDetails", issueDetails(decoded.Issues, "decode")},
                {"applyIssueDetails", issueDetails(applied.Issues, "apply")},
                {"effectiveSurfaceLighting", effectiveJson["Effects"]["SurfaceLighting"]},
                {"effectiveDirectionalShadows", effectiveJson["Effects"]["DirectionalShadows"]["Mode"]},
                {"effectiveCasterFaces", static_cast<unsigned>(effective.Environment.CasterFaces)}});
      } else if (op == "surface_trace") {
        // The surface-lighting candidates of the next frames, one JSON line each.
        Fast::Renderer3ds::PicaSurfaceDebug::Instance().RequestTrace(command.at("path").get<std::string>(),
                                                                     command.value("frames", 1U),
                                                                     command.value("census", false));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "surface_exempt") {
        // Candidates first..last keep their native appearance ({} clears).
        Fast::Renderer3ds::PicaSurfaceDebug::Instance().SetExemptRange(command.value("first", 1U),
                                                                       command.value("last", 0U));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "peek") {
        // Guest memory as hex (at most 4 KiB): finding actor fields.
        const uint32_t address = command.at("address").get<uint32_t>();
        const uint32_t size = std::min(command.value("size", 256U), 4096U);
        std::string hex;
        hex.reserve(size * 2U);
        static constexpr char kDigits[] = "0123456789abcdef";
        bool readable = true;
        for (uint32_t i = 0; i < size && readable; ++i) {
          uint8_t byte = 0;
          readable = processStorage->Memory().Read8(address + i, &byte);
          hex += kDigits[byte >> 4U];
          hex += kDigits[byte & 15U];
        }
        finish(readable ? "ok" : "failed", {{"frame", guestFrame}, {"address", address}, {"hex", hex}});
      } else if (op == "poke16") {
        // One halfword written into guest memory (developer experiments only).
        const uint32_t address = command.at("address").get<uint32_t>();
        const uint16_t value = static_cast<uint16_t>(command.at("value").get<int>());
        const bool written = processStorage->Memory().Write16(address, value);
        finish(written ? "ok" : "failed", {{"frame", guestFrame}, {"address", address}});
      } else if (op == "water") {
        // The scene's water boxes as the renderer receives them.
        const auto water = Oot3dNativeGame::CaptureNativeWaterBodies(sceneViewProbe.Stats().LastPlayStateAddress, processStorage->Memory());
        Json bodies = Json::array();
        for (uint32_t i = 0; i < water.Count; ++i) {
          const auto &body = water.Bodies[i];
          bodies.push_back({{"min", {body.MinimumXZ[0], body.MinimumXZ[1]}},
                            {"max", {body.MaximumXZ[0], body.MaximumXZ[1]}},
                            {"y", body.SurfaceY},
                            {"properties", body.Properties}});
        }
        auto detail = place();
        detail["water"] = std::move(bodies);
        finish("ok", detail);
      } else if (op == "actors") {
        // The live actors (optionally only some ids): id, params, instance,
        // room, world position and scale.
        Json actors = Json::array();
        std::vector<uint16_t> only;
        if (command.contains("ids"))
          for (const auto &id : command["ids"]) only.push_back(id.get<uint16_t>());
        for (const auto &a : Oot3dNativeGame::CaptureNativeActors(sceneViewProbe.Stats().LastPlayStateAddress,
                                                                  processStorage->Memory())) {
          if (!only.empty() && std::find(only.begin(), only.end(), a.Id) == only.end()) continue;
          actors.push_back({{"id", a.Id},
                            {"params", a.Params},
                            {"category", a.Category},
                            {"room", a.Room},
                            {"address", a.Address},
                            {"position", {a.Position[0], a.Position[1], a.Position[2]}},
                            {"scale", {a.Scale[0], a.Scale[1], a.Scale[2]}}});
        }
        auto detail = place();
        detail["actors"] = std::move(actors);
        finish("ok", detail);
      } else if (op == "emitters") {
        // The native lights linked now (LightContext), with their owners.
        Json lights = Json::array();
        const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
        for (const auto &e : Oot3dNativeGame::CaptureNativeEmitters(playState, processStorage->Memory())) {
          Json light = {{"type", e.Type},
                        {"position", {e.Position[0], e.Position[1], e.Position[2]}},
                        {"direction", {e.Direction[0], e.Direction[1], e.Direction[2]}},
                        {"color", {e.Color[0], e.Color[1], e.Color[2]}},
                        {"radius", e.Radius}};
          if (e.Owner && e.Owner->Kind == "actor")
            light["owner"] = {{"kind", "actor"},
                              {"actorId", e.Owner->ActorId},
                              {"actor", e.Owner->ActorAddress},
                              {"params", e.Owner->Params},
                              {"offset", e.Owner->Offset}};
          else if (e.Owner)
            light["owner"] = {{"kind", e.Owner->Kind}, {"offset", e.Owner->Offset}};
          lights.push_back(std::move(light));
        }
        auto detail = place();
        detail["lights"] = std::move(lights);
        finish("ok", detail);
      } else if (op == "gizmos") {
        sceneEnvironment->ShowLightGizmos(command.value("shown", true));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "quit") {
        finish("ok", place());
        devLabQuit = true;
      } else if (op != "wait" && op != "gameplay") {
        finish("rejected", {{"reason", "unknown op"}});
      }
    } catch (const std::exception &error) {
      finish("rejected", {{"reason", error.what()}});
    }
    return;
  }
  ++devLabFrames;
  const auto &command = *devLabCommand;
  const std::string op = command["op"].get<std::string>();
  if (op == "warp") {
    // Settled: the given frames after arriving (title cards and fades).
    if (status.Phase == Oot3dNativeGame::DevWarpPhase::Failed) {
      finish("failed", {{"reason", status.Message}});
    } else if (status.Phase == Oot3dNativeGame::DevWarpPhase::Arrived) {
      if (!devLabSawArrival) {
        devLabSawArrival = true;
        devLabFrames = 0;
      } else if (devLabFrames >= command.value("settleFrames", 60U)) {
        finish("ok", place());
      }
    } else if (devLabFrames >= 3600U) {
      finish("timeout", {{"reason", status.Message}});
    }
  } else if (op == "camera") {
    // Held until the game took the free view for the given frames from the
    // main camera (PlayState +0xA64, 0): a cutscene camera does not count.
    int16_t activeCamera = -1;
    const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
    if (playState != 0U)
      (void)processStorage->Memory().Read16(playState + 0xA64U, reinterpret_cast<uint16_t *>(&activeCamera));
    if (activeCamera != 0) devLabAppliedAt = camera.AppliedCount();
    const uint64_t applied = camera.AppliedCount() - devLabAppliedAt;
    if (applied >= command.value("holdFrames", 45U))
      finish("ok", {{"frame", guestFrame}, {"applied", applied}});
    else if (devLabFrames >= command.value("timeoutFrames", 900U))
      finish("timeout", {{"frame", guestFrame}, {"applied", applied}, {"activeCamera", activeCamera}});
  } else if (op == "wait") {
    if (devLabFrames >= command.value("frames", 30U)) finish("ok", place());
  } else if (op == "gameplay") {
    // Until the player is in control (a script started at boot waits here
    // through the title screen and the file select).
    if (seen.Gameplay) finish("ok", place());
  }
}

void NativeA32WindowSession::UpdateInputBench(Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  using Bench = Oot3dNativeGame::InputBench;
  if (inputBench.GetMode() == Bench::Mode::Off || devWarp == nullptr) return;
  const auto warpStatus = devWarp->Status();
  const bool arrived = warpStatus.Phase == Oot3dNativeGame::DevWarpPhase::Arrived;
  const bool gameplay = warpStatus.Observation.Gameplay;
#if defined(__SWITCH__)
  const bool stop = Fast::Oot3d::DebugMenuOpen();
  const std::filesystem::path root = "sdmc:/switch/oot3dre";
#else
  const bool stop = false;
  const std::filesystem::path root = ".";
#endif
  const auto event = inputBench.Step(frameCount, gameplay, arrived, stop, inputFrame);
  if (event == Bench::Event::None) return;
  const bool recording = inputBench.GetMode() == Bench::Mode::Record;
  std::ofstream status(root / "logs" / "bench_status.txt", std::ios::trunc);
  if (event == Bench::Event::Started) {
    inputBenchStarted = std::chrono::steady_clock::now();
    status << (recording ? "recording" : "replaying") << " guest_frame=" << frameCount << '\n';
    return;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - inputBenchStarted).count();
  const uint32_t frames = inputBench.WalkFrame();
  if (recording) {
    std::error_code ignored;
    std::filesystem::create_directories(root / "bench", ignored);
    std::ofstream(root / "bench" / "input.json", std::ios::trunc) << inputBench.Recording().ToJson();
    status << "recorded frames=" << inputBench.Recording().EndFrame()
           << " segments=" << inputBench.Recording().SegmentCount() << " seconds=" << seconds << '\n';
  } else {
    status << "done frames=" << frames << " seconds=" << seconds
           << " guest_fps=" << (seconds > 0.0 ? frames / seconds : 0.0) << '\n';
  }
}

void NativeA32WindowSession::UpdateDevFreeCamera(Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &freeCamera = *nativeCandidateDispatch.DevFreeCamera;
  const Uint8 *keys = SDL_GetKeyboardState(nullptr);
  const auto &io = ImGui::GetIO();
#if defined(__SWITCH__)
  // Console build: controller only (Fast3dWindow ignores the keyboard too).
  const bool keyboard = false;
  (void)io;
#else
  const bool keyboard = keys != nullptr && !io.WantCaptureKeyboard;
#endif
  const bool toggle = keyboard && keys[SDL_SCANCODE_F6] != 0U;
  if (toggle && !devFreeCameraToggleHeld) freeCamera.SetActive(!freeCamera.Active());
  devFreeCameraToggleHeld = toggle;
  const auto now = std::chrono::steady_clock::now();
  const float seconds = devFreeCameraLastRefresh.time_since_epoch().count() == 0
      ? 0.0F
      : std::min(std::chrono::duration<float>(now - devFreeCameraLastRefresh).count(), 0.1F);
  devFreeCameraLastRefresh = now;
  // Physical sources, not the game's bindings (its R is the right mouse
  // button): the mouse motion since the last refresh, the pad as it is.
  auto &sources = nativeControlPollingState;
  const float mouseX = static_cast<float>(sources.DevMouseDeltaX);
  const float mouseY = static_cast<float>(sources.DevMouseDeltaY);
  sources.DevMouseDeltaX = sources.DevMouseDeltaY = 0;
  if (!freeCamera.Active()) return;
  // Pad: left stick moves, right stick looks (120 degrees per second), L/R
  // shoulders down/up.
  float forward = sources.DevPadLeftStick[1];
  float right = sources.DevPadLeftStick[0];
  float up = (sources.DevPadRightShoulder ? 1.0F : 0.0F) - (sources.DevPadLeftShoulder ? 1.0F : 0.0F);
  float yaw = -sources.DevPadRightStick[0] * 120.0F * seconds;
  float pitch = sources.DevPadRightStick[1] * 120.0F * seconds;
  float multiplier = 1.0F;
  if (keyboard) {
    // WASD moves, Q/E down/up, Shift faster, Ctrl slower.
    forward += (keys[SDL_SCANCODE_W] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_S] ? 1.0F : 0.0F);
    right += (keys[SDL_SCANCODE_D] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_A] ? 1.0F : 0.0F);
    up += (keys[SDL_SCANCODE_E] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_Q] ? 1.0F : 0.0F);
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) multiplier = 4.0F;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) multiplier = 0.25F;
  }
  // Right mouse button held: look (0.15 degrees per pixel).
  if (sources.DevMouseRightHeld && !io.WantCaptureMouse) {
    yaw -= mouseX * 0.15F;
    pitch -= mouseY * 0.15F;
  }
  const auto clampAxis = [](float value) { return std::clamp(value, -1.0F, 1.0F); };
  freeCamera.Move(clampAxis(forward), clampAxis(right), clampAxis(up), yaw, pitch, seconds, multiplier);
  // Link stays still: the game sees no pad, sticks or touch.
  inputFrame.Hid.Buttons = 0U;
  inputFrame.Hid.CirclePadX = inputFrame.Hid.CirclePadY = 0;
  inputFrame.Hid.TouchPressed = false;
  inputFrame.CStick = {};
}
#endif

namespace {
// deko3d config/lighting perpixel reads the scene's environment profile; the
// release defaults are in resources.o2r (environments/, staged by
// scripts/switch/stage-product.sh), not in a folder on the card. Installed
// only while the Modern model is on; render stream only.
std::optional<std::string> ReadArchivedEnvironmentDefault(const std::string &fileName) {
  auto *context = Ship::Context::GetRawInstance();
  auto resources = context != nullptr ? context->GetResourceManager() : nullptr;
  auto archives = resources != nullptr ? resources->GetArchiveManager() : nullptr;
  const std::string path = "environments/" + fileName;
  // HasFile first: LoadFile(path) adds an index entry on a miss.
  if (archives == nullptr || !archives->HasFile(path)) return std::nullopt;
  const auto file = archives->LoadFile(path);
  if (file == nullptr || file->Buffer == nullptr || file->BufferOffset > file->Buffer->size()) return std::nullopt;
  return std::string(file->Buffer->begin() + static_cast<std::ptrdiff_t>(file->BufferOffset), file->Buffer->end());
}

void ApplyLightingEnvironmentDefaults(Oot3dNativeGame::SceneEnvironmentRuntime &environment, bool modernModel) {
  static bool installed = false;
  // Only the deko3d renderer's defaults live in the archive.
  const bool perPixel = modernModel && Fast::Deko3dRendererSelected();
  if (perPixel == installed) return;
  installed = perPixel;
  environment.SetDefaultsReader(perPixel ? ReadArchivedEnvironmentDefault
                                         : Oot3dNativeGame::SceneEnvironmentRuntime::DefaultsReader{});
}
} // namespace

void NativeA32WindowSession::PublishNativeSceneView() {
  auto &process = *processStorage;
  // Guest memory is read here; the scene environment, the duplicate check
  // and the publish belong to the renderer and run in its order.
  auto snapshot = sceneViewProbe.Capture(process.Memory());
  auto environmentTime = snapshot
      ? Oot3dNativeGame::CaptureSceneEnvironmentTime(snapshot->PlayStateAddress, process.Memory())
      : std::nullopt;
  if (environmentTime)
    environmentTime->Group = Oot3dNativeGame::CaptureNativeEnvironmentGroup(
        snapshot->PlayStateAddress, process.Memory(), &environmentTime->AuthoredScene);
  // Where Link is (world position +0x28, shape yaw +0xBE: 0 faces +Z,
  // 0x4000 +X), for lights placed from F1.
  std::optional<Oot3dNativeGame::ScenePlayerPlacement> player;
  if (const uint32_t actor = playerTimingProbe.Stats().LastPlayerAddress; actor != 0U) {
    Oot3dNativeGame::ScenePlayerPlacement placement;
    uint32_t bits[3]{};
    uint16_t yaw = 0;
    if (process.Memory().Read32(actor + 0x28U, &bits[0]) &&
        process.Memory().Read32(actor + 0x2CU, &bits[1]) &&
        process.Memory().Read32(actor + 0x30U, &bits[2]) &&
        process.Memory().Read16(actor + 0xBEU, &yaw)) {
      for (size_t axis = 0; axis < 3; ++axis)
        placement.Position[axis] = std::bit_cast<float>(bits[axis]);
      placement.Yaw = static_cast<float>(static_cast<int16_t>(yaw)) * (180.0F / 32768.0F);
      if (std::isfinite(placement.Position[0]) && std::isfinite(placement.Position[1]) &&
          std::isfinite(placement.Position[2]))
        player = placement;
    }
  }
  // Light gizmos also show while the developer free camera looks around.
  bool freeCamera = false;
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  freeCamera = nativeCandidateDispatch.DevFreeCamera->Active();
  if (devEmitterLog && snapshot)
    devEmitterLog->Observe(++devEmitterFrames, snapshot->PlayStateAddress, snapshot->SceneId, process.Memory());
#endif
  renderStream.Submit([&, snapshot, environmentTime, player, freeCamera] {
  Fast::GlPerf::SectionScope perfScene(Fast::GlPerf::Section::SceneEnv);
  if (frameSpikesEnabled && snapshot && snapshot->SceneId.has_value()) {
    // Scene change (warp, door, loading): logs/spikes.txt ctx=load.
    const uint32_t scene = *snapshot->SceneId;
    if (spikeLastScene.has_value() && *spikeLastScene != scene)
      spikeSceneChangePresent = spikePresents;
    spikeLastScene = scene;
  }
  sceneEnvironment->ObserveScene(snapshot ? snapshot->SceneId : std::nullopt,
      environmentTime.value_or(Oot3dNativeGame::SceneEnvironmentTime{}), player);
  sceneEnvironment->PollProfileFiles();
  const auto shadowSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance().SnapshotForRendering();
  ApplyLightingEnvironmentDefaults(*sceneEnvironment,
                                   Fast::Oot3d::UsesFullSceneLighting(shadowSettings.Value.Effects.SurfaceLighting));
  picaCompositionTracker.SetRoomShadowShellsEnabled(
      shadowSettings.Environment.CasterFaces == Fast::Renderer3ds::ShadowCasterFaces::InteriorShells &&
      shadowSettings.Value.Effects.DirectionalShadows.Mode == Fast::Oot3d::DirectionalShadowMode::VirtualShadowMaps);
  sceneEnvironment->PublishLightGizmo(
      snapshot && snapshot->CameraAvailable ? std::optional(snapshot->Eye) : std::nullopt, freeCamera);
  if (!snapshot.has_value() ||
      snapshot->Serial == lastPublishedSceneViewSerial) {
    return;
  }
  const Fast::Oot3d::TitleSceneViewSubmission view{
      snapshot->GuestFunction,
      snapshot->GuestReturnAddress,
      snapshot->Left,
      snapshot->Right,
      snapshot->Bottom,
      snapshot->Top,
      snapshot->NearPlane,
      snapshot->FarPlane,
      snapshot->Eye,
      snapshot->At,
      snapshot->CameraAvailable,
      // Rooms share their scene's coordinates; another scene is another world.
      snapshot->SceneId.has_value() ? uint64_t{*snapshot->SceneId} + 1U : 0U,
  };
  if (titleRenderBackend != nullptr &&
      titleRenderBackend->PublishSceneView(view)) {
    lastPublishedSceneViewSerial = snapshot->Serial;
    ++publishedSceneViewCount;
  }
  });
}

// Guest memory and guest interrupts belong to the guest. The renderer
// reaches them only from a command the guest waits for (inline or Run);
// anything else would race the running guest, so it fails instead.
void NativeA32WindowSession::RequireStoppedGuest(const char *access) {
  if (renderStream.InCommand() && !renderStream.InSynchronousCommand()) {
    throw std::runtime_error(
        std::string("renderer reached guest state while the guest was running (") + access + ")");
  }
}

std::vector<Fast::Renderer3ds::PicaTextureCopyReference> NativeA32WindowSession::CollectGuestCopyReferences() {
  auto &submissionQueue = *submissionQueueStorage;
  std::vector<Fast::Renderer3ds::PicaTextureCopyReference> references;
  submissionQueue.CollectLiveCopyReferences(references);
  for (const auto& draw : submissionQueue.PendingDraws()) {
    for (const auto& resource : draw.Resources) {
      references.insert(references.end(), resource.CopyReferences.begin(),
                        resource.CopyReferences.end());
    }
  }
  for (const auto& plan : picaPresentationScheduler.PendingDraws()) {
    Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(plan, references);
  }
  return references;
}

void NativeA32WindowSession::PinGpuCopyResources(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample& sample) {
  Fast::GlPerf::SectionScope perfPin(Fast::GlPerf::Section::PinCopy);
  auto &api = *apiPointer;
  pinnedCopyReferences = guestCopyReferences;
  if (previousVisualFrame) {
    Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
        *previousVisualFrame, pinnedCopyReferences);
  }
  if (latestVisualFrame) {
    Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
        *latestVisualFrame, pinnedCopyReferences);
  }
  // The active sample may already have left the accumulator, or may be
  // the new frame before it replaces latestVisualFrame.
  for (const auto& draw : sample.Draws) {
    if (draw.BasePlan != nullptr) {
      Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(
          *draw.BasePlan, pinnedCopyReferences);
    }
  }
  if (!api.SetPinnedPicaTextureCopies(pinnedCopyReferences, &renderError)) {
    throw std::runtime_error("native PICA copy retention failed: " + renderError);
  }
}

bool NativeA32WindowSession::FlushNativeDependencies() {
  auto &api = *apiPointer;
  // The accumulator is guest-owned, so whether there is dependency work is
  // known here; only real work waits for the renderer.
  if (!picaPresentationScheduler.ViewDependencyWork(dependencySampleScratch))
    return false;
  // Synchronous: raw copies read and write guest memory and signal PPF
  // while the guest waits.
  bool flushed = false;
  renderStream.Run([&] {
    // Without an acquired image the flush waits; PPF arrives later, as it
    // may on hardware.
    if (rendererFrameSkipped) return;
    // Deferred placeholders this flush executes are built now (the guest
    // waits here anyway); the render worker is idle inside Run.
    uint64_t lastDependencyDraw = 0U;
    for (const auto& draw : dependencySampleScratch.Draws) {
      if (draw.BasePlan != nullptr)
        lastDependencyDraw = std::max(lastDependencyDraw, draw.BasePlan->SubmissionId);
    }
    if (!picaPresentationScheduler.ResolveDeferredDraws(
            lastDependencyDraw,
            [&](Oot3dNativeGame::Oot3dPicaVulkanDrawPlan& plan) {
              return Oot3dNativeGame::ResolveOot3dPicaDeferredDrawPlan(plan, &renderError);
            })) {
      throw std::runtime_error(renderError);
    }
    guestCopyReferences = CollectGuestCopyReferences();
    PinGpuCopyResources(dependencySampleScratch);
    if (!picaPresentationScheduler.Execute(
            api, dependencySampleScratch, kNativeDependencyRenderTargetNamespace,
            Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::DependencyFlush,
            false, &renderError)) {
      throw std::runtime_error("native PICA dependency flush failed: " + renderError);
    }
    flushed = true;
  });
  return flushed;
}

void NativeA32WindowSession::ExecuteVisualSample(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample &sample, Oot3dNativeGame::Oot3dPicaPresentationExecutionKind kind, bool present, const Fast::Oot3d::NativeFrameTemporalSample& temporalSample, uint64_t renderTargetNamespace) {
  auto &api = *apiPointer;
  // Leaving the game: a command already running (a new frame followed by its
  // interpolated sample, a repeated sample of the previous frame) draws
  // nothing more once the exit has begun.
  if (renderStream.ShutdownRequested()) return;
  Fast::GlPerf::SectionScope perfVisual(Fast::GlPerf::Section::VisualSample);
  const auto started = std::chrono::steady_clock::now();
  (void)api.PublishPicaFrameTemporalSample(temporalSample);
  // Only displayed samples advance the grass colliders: the authoritative
  // replay of a new frame is not shown, and taking it as the previous position
  // gave the next displayed sample a backward sweep.
  if (present) Fast::Oot3d::GrassInteractionBridge::Instance().SelectSample(temporalSample);
  PinGpuCopyResources(sample);
  // config/gpu_timing: step/interp frame and canonical (hidden) draws.
  // A new guest frame is every kind but the repeated (in-between) sample:
  // with interpolation the step frame presents an InterpolatedFrame (the
  // delayed sample at alpha 0), and the native replay may be skipped, so
  // counting only NativeFrame/CurrentFrame classified every step as interp.
  Fast::GlPerf::SampleScope perfSample(
      kind != Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::RepeatedFrame,
      present);
  bool executed;
  {
    Fast::GlPerf::SectionScope perfExecute(Fast::GlPerf::Section::Execute);
    executed = picaPresentationScheduler.Execute(
        api, sample, renderTargetNamespace, kind, present, &renderError);
  }
  if (!executed) {
    throw std::runtime_error(
        "native PICA scheduled presentation failed: " + renderError);
  }
  const double elapsed = SecondsSince(started);
  if (present) {
    capturedTemporalSample = temporalSample;
    lastPresentedVisual.emplace(sample.TopTransfer, renderTargetNamespace);
  }
  rendererTiming->PicaBackendSeconds += elapsed;
  rendererTiming->VisualReplaySeconds += elapsed;
}

} // namespace Oot3dA32Window
