// Native A32 window: the session driver (Run, Finish) and the guest-run helpers the session files share.
#include "oot3d_native_a32_process_image.h"
#include "oot3d_native_title_profile.h"
#include "oot3d_native_a32_window.h"
#include "oot3d_native_a32_window_session.h"
#include "oot3d_native_a32_window_summary.h"
#include "oot3d_hang_watchdog.h"
#include "oot3d_switch_guard_alloc.h"
#include "oot3d_switch_threads.h"
#include "fast/backends/gfx_deko3d.h"
#include <sstream>

namespace Oot3dA32Window {

#if defined(__SWITCH__)
namespace {
std::string FormatSwitchBootStage(const char* stage) {
  std::ostringstream stream;
  stream << "status=launching\n"
         << "stage=" << stage << '\n'
         << "whole_aot_functions=12419\n"
         << "host_boundaries=3\n"
         << "residual_a32=0\n"
         << "block_hook_count=" << gSwitchBlockHookCount << '\n'
         << "block_hooks_sorted=" << (gSwitchBlockHooksSorted ? 1 : 0)
         << '\n'
         << "block_hooks_contain_process_entry="
         << (gSwitchBlockHooksContainProcessEntry ? 1 : 0) << '\n'
         << "host_frames=" << gSwitchRuntimeHostFrames << '\n'
         << "guest_frames=" << gSwitchRuntimeGuestFrames << '\n'
         << "ui_profile=" << gSwitchUiProfile << '\n'
         << "gameplay_timing=" << gSwitchGameplayTiming << '\n'
         << "presentation_rate_hz=" << gSwitchPresentationRateHz << '\n'
         << "visual_interpolation=" << (gSwitchVisualInterpolation ? 1 : 0)
         << '\n'
         << "control_profile=" << gSwitchControlProfile << '\n'
         << "topscreen_211_assets=" << (gSwitchTopScreen211Assets ? 1 : 0)
         << '\n'
         << "whole_aot_block_budget=" << gSwitchWholeAotBlockBudget << '\n'
         << "whole_aot_block_limit_exits="
         << gSwitchWholeAotBlockLimitExits << '\n'
         << "pica_geometry_cache_enabled="
         << (gSwitchPicaGeometryCacheEnabled ? 1 : 0) << '\n'
         << "pica_geometry_persistent_draws="
         << gSwitchPicaGeometryPersistentDraws << '\n'
         << "pica_geometry_persistent_uploads="
         << gSwitchPicaGeometryPersistentUploads << '\n'
         << "pica_geometry_registry_entries="
         << gSwitchPicaGeometryRegistryEntries << '\n';
  return std::move(stream).str();
}
} // namespace

void WriteSwitchBootStage(const char* stage) {
  (void)Oot3dNativeGame::SwitchBootStatusFile().WriteNow(FormatSwitchBootStage(stage));
}

void PostSwitchBootStage(const char* stage) {
  Oot3dNativeGame::SwitchBootStatusFile().Post(FormatSwitchBootStage(stage));
}
#endif

void WriteA32BlockTrace(const std::filesystem::path &path,
                        const NativeWidescreenProjectionState &runtime) {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot open A32 block trace: " + path.string());
  }
  stream << nlohmann::json({
                               {"format", "oot3d_a32_block_trace_v2"},
                               {"records", runtime.BlockTrace.size()},
                               {"records_observed",
                                runtime.BlockTraceRecordsObserved},
                               {"truncated", runtime.BlockTraceTruncated},
                           })
                .dump()
         << '\n';
  for (const auto &record : runtime.BlockTrace) {
    stream
        << nlohmann::json(
               {
                   {"host_frame", record.HostFrame},
                   {"pc", record.Pc},
                   {"memory_write_generation", record.MemoryWriteGeneration},
                   {"memory_write_fingerprint", record.MemoryWriteFingerprint},
                   {"fast_read_mismatch_count", record.FastReadMismatchCount},
                   {"last_fast_read_mismatch_address",
                    record.LastFastReadMismatchAddress},
                   {"last_fast_read_mismatch_value",
                    record.LastFastReadMismatchValue},
                   {"last_checked_read_mismatch_value",
                    record.LastCheckedReadMismatchValue},
                   {"r", record.Registers},
                   {"cpsr", record.Cpsr},
                   {"fpscr", record.Fpscr},
                   {"thread_pointer", record.ThreadPointer},
                   {"vfp", record.Vfp},
                   {"exclusive_address", record.ExclusiveAddress},
                   {"exclusive_token", record.ExclusiveToken},
                   {"exclusive_size", record.ExclusiveSize},
                   {"exclusive_valid", record.ExclusiveValid},
               })
               .dump()
        << '\n';
  }
  if (!stream) {
    throw std::runtime_error("cannot write A32 block trace: " + path.string());
  }
}

Oot3dNativeGame::NativeA32ProcessRunResult
RunUntilGuestWait(Oot3dNativeGame::NativeA32Process &process,
                  uint32_t wholeAotBlockBudget,
                  const std::function<void()> *betweenRuns) {
  static const bool diagnoseWait = std::getenv("TRIAEVUM_DIAGNOSE_GUEST_WAIT") != nullptr;
  const auto waitStart = diagnoseWait ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
  // A block-limit exit unwinds the native whole-AOT call chain. The caller
  // resumes it immediately: this is a stack-safety boundary, not an SDL event
  // pump or guest scheduling boundary.
  auto result = process.Run(wholeAotBlockBudget);
  while (result.Kind == Oot3dNativeGame::NativeA32ProcessRunKind::Yielded) {
    // Low-priority workers may poll indefinitely. Once the foreground is
    // waiting, let the host deliver its timers, HID and GPU completions.
    // The worker stays Ready and resumes after those events, not next boot.
    if (process.OnlyBackgroundThreadsReady()) return result;
    if (diagnoseWait && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(20)) {
      for (uint32_t id = 0; id < process.ThreadCount(); ++id) {
        const auto* thread = process.ThreadState(id);
        if (thread != nullptr)
          std::fprintf(stderr, "GUEST_WAIT_TIMEOUT thread=%u pc=%08x lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x\n",
                       id, thread->r[15], thread->r[14], thread->r[13],
                       thread->r[0], thread->r[1], thread->r[2]);
      }
      throw std::runtime_error("guest did not reach a wait boundary within diagnostic deadline");
    }
    if (betweenRuns != nullptr) (*betweenRuns)();
    result = process.Run(wholeAotBlockBudget);
  }
  return result;
}

void RequireRunnableGuest(
    const Oot3dNativeGame::NativeA32ProcessRunResult &result) {
  if (result.Kind == Oot3dNativeGame::NativeA32ProcessRunKind::Faulted) {
    // The report (registers, backtrace, memory, area, last perf windows) goes
    // to the game log only; boot-status.txt and the error screen keep the
    // one-line error of the exception.
    if (!result.GuestReport.empty()) {
      LogFatalErrorForDemo(
          (result.GuestReport + Oot3dNativeGame::GuestFaultPerf().Format()).c_str());
    }
    throw std::runtime_error("native A32 process faulted: " + result.Error);
  }
}

// config/second_core: guest threads that asked for processor 1 (the sound
// thread) run on their own host thread, pinned to core 1
// (SwitchThreadRole::GuestCore1). From here on the game thread holds the
// kernel lock for all its host work; NativeA32Process::Run releases it
// around guest code (oot3d_native_a32_second_core.h).
void NativeA32WindowSession::StartSecondCore() {
  auto &process = *processStorage;
  process.EnableSecondCore();
  Oot3dNativeGame::SecondCore::LockKernel();
  // With the host audio stream the DSP frames run on the second core's host
  // thread (DeliverDspAudioFrameOnSecondCore); without it (SDL player and
  // its resampler on the game thread) the game thread's pump delivers them.
  dspOnSecondCore = hostAudioStream != nullptr;
  if (dspOnSecondCore) {
    process.SetSecondCoreIdleWork(
        {[this] { return deferredDspAudioFrames != 0U; },
         [this] { DeliverDspAudioFrameOnSecondCore(); }});
  } else {
    secondCorePump = [this] { PumpDspAudioFrame(); };
  }
  const uint32_t budget = launch.WholeAotBlockBudget;
  secondCoreThread = std::thread([&process, budget] {
    Oot3dNativeGame::PlaceSwitchThread(Oot3dNativeGame::SwitchThreadRole::GuestCore1);
    process.RunSecondCore(budget);
  });
}

void NativeA32WindowSession::StopSecondCore() noexcept {
  if (!secondCoreThread.joinable()) return;
  auto &process = *processStorage;
  process.RequestSecondCoreStop();
  {
    // It may be waiting for the lock to see the request.
    const Oot3dNativeGame::SecondCore::KernelRelease release;
    secondCoreThread.join();
  }
  process.SetSecondCoreIdleWork({});
  dspOnSecondCore = false;
  process.DisableSecondCore();
  Oot3dNativeGame::SecondCore::UnlockKernel();
}

// Second core: the deferred DSP frames of the host frame go out while the
// game thread runs guest code, one at a time, each once the work the previous
// one woke is done: the sound thread back in its wait on the second core and
// no interrupt-level thread ready on the game thread (as the real DSP's
// 4.9 ms period leaves it). It is parked while the HLE mix updates the shared
// regions in place, so the mix never meets its writes; its processing then
// overlaps the game thread's guest code.
void NativeA32WindowSession::PumpDspAudioFrame() {
  if (deferredDspAudioFrames == 0U) return;
  if (processStorage->AuxiliaryWorkPending()) return;
  --deferredDspAudioFrames;
  ++secondCoreDsp.Pumped;
  DeliverDspAudioFrames(1U, false);
}

void NativeA32WindowSession::Run() {
  Setup();
  if (!launch.ConvertStatePath.empty()) {
    // The loaded moment, re-saved before any guest frame runs.
    const std::string *settings =
        initialStateHostSettings ? &*initialStateHostSettings : nullptr;
    const auto outcome = SaveState(launch.ConvertStatePath, false, settings);
    Finish();
    if (outcome != SaveStateOutcome::Saved) {
      throw std::runtime_error("savestate conversion failed: " +
                               (lastSavestateMessage.empty()
                                    ? std::string("a host continuation is pending")
                                    : lastSavestateMessage));
    }
    return;
  }
  auto &window = *windowPointer;
#if defined(__SWITCH__)
  // config/grass (mod): the grass source rules of the product preset (the
  // PC's default Grass object) for the deko3d backend; off without the file.
  // The budgets follow the FPS: the start's here, a change in L3+R3 > Video
  // at once (Deko3dSetGrassFrameRate, RunFrame).
  if (Fast::Deko3dRendererSelected()) {
    static constexpr std::string_view kProductGraphics =
#include "triaevum_product_graphics.inc"
        ;
    Fast::Deko3dConfigureGrass(kProductGraphics, visualInterpolationEnabled);
  }
  // Guard pages (config/guard_alloc) from here on: the boot is left out (it
  // ran out of memory with every small block guarded).
  Oot3dNativeGame::GuardAlloc::Arm();
  // Hang watchdog (oot3d_hang_watchdog.h): logs/hang.txt when the game or
  // the render thread stops for > 3 s. The extra section reads the stream's
  // sync state with try_lock only (it runs while something may be stuck).
  Oot3dNativeGame::HangWatch::Start(
      [](void *user, Oot3dNativeGame::HangWatch::Text &text) {
        auto &session = *static_cast<NativeA32WindowSession *>(user);
        Oot3dNativeGame::NativeRenderCommandStream::SyncSnapshot sync;
        text.Str("stream: shutdown=")
            .Dec(session.renderStream.ShutdownRequested() ? 1U : 0U);
        if (session.renderStream.TryGetSyncSnapshot(sync)) {
          text.Str(" queued=").Dec(sync.Queued)
              .Str(" frames_queued=").Dec(sync.FramesQueued)
              .Str(" busy=").Dec(sync.Busy ? 1U : 0U)
              .Str(" worker_waiting=").Dec(sync.WorkerWaiting ? 1U : 0U)
              .Str(" progress_waiters=").Dec(sync.ProgressWaiters)
              .Str(" stop=").Dec(sync.Stop ? 1U : 0U)
              .Str(" failed=").Dec(sync.Failed ? 1U : 0U)
              .Str(" submitted=").Dec(sync.Submitted)
              .Str(" executed=").Dec(sync.Executed)
              .Str(" submit_waits=").Dec(sync.Waits.SubmitWaits)
              .Str(" drain_waits=").Dec(sync.Waits.DrainWaits)
              .Str(" worker_idle_waits=").Dec(sync.Waits.WorkerIdleWaits)
              .Str(" signals_skipped=").Dec(sync.Waits.SkippedNotifies);
        } else {
          text.Str(" mutex=BUSY");
        }
        if (session.realtimePacerStorage.has_value()) {
          const auto tail = session.realtimePacerStorage->TailCounters();
          text.Str("\npacer: waits=").Dec(tail.Waits)
              .Str(" oversleep_max_us=")
              .Dec(tail.MaximumOversleepNanoseconds / 1000U);
        }
      },
      this);
#endif
  while (window.IsRunning() &&
         processResult.Kind !=
             Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    if (!RunFrame()) break;
  }
  Finish();
}

NativeA32WindowSession::~NativeA32WindowSession() {
#if defined(__SWITCH__)
  // A run that left by an exception never reached Finish.
  Oot3dNativeGame::HangWatch::Stop();
#endif
  StopSecondCore();
  StopRendering();
}

void NativeA32WindowSession::StopRendering() noexcept {
  if (renderingStopped) return;
  renderingStopped = true;
  // No queued command starts any more; the one running finishes (and draws
  // no further sample: ExecuteVisualSample checks the stream); the worker is
  // joined. The GL context stays where it was, as before: teardown after
  // this point issues no rendering.
  const size_t discarded = renderStream.Shutdown();
  if (discarded != 0U) {
    std::fprintf(stderr, "TRIAEVUM_RENDER_SHUTDOWN discarded_commands=%zu\n",
                 discarded);
  }
  // The overlay no longer routes into the closed stream.
  guiRendererRoutingReset.reset();
  // After the render worker: resolving a placeholder takes the builder's
  // lock, so the plan worker stops once nobody can resolve any more.
  picaPlanBuilder.StopWorker();
  if (rendererTiming != &phaseTiming && rendererTiming != nullptr) {
    phaseTiming.PicaBackendSeconds += workerPhaseTiming.PicaBackendSeconds;
    phaseTiming.VisualReplaySeconds += workerPhaseTiming.VisualReplaySeconds;
    phaseTiming.PacingWaitSeconds += workerPhaseTiming.PacingWaitSeconds;
    rendererTiming = &phaseTiming;
  }
}

void NativeA32WindowSession::Finish() {
#if defined(__SWITCH__)
  // Before anything the watchdog's extra section reads goes away.
  Oot3dNativeGame::HangWatch::Stop();
#endif
  // The second core's host thread joins first: everything below reads guest
  // state single-threaded.
  StopSecondCore();
  auto &hostServices = *hostServicesStorage;
  auto &frameRatePolicy = *frameRatePolicyStorage;
  auto &blockTraceWriter = *blockTraceWriterStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &submissionQueue = *submissionQueueStorage;
  auto &window = *windowPointer;
  auto &api = *apiPointer;
  auto &preparationScreen = *preparationScreenStorage;
  auto &nativeUiTextureProvider = *nativeUiTextureProviderStorage;
  auto &ocarinaText = *ocarinaTextStorage;
  auto &n64UiRenderer = *n64UiRendererStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  auto &presentationScheduler = *presentationSchedulerStorage;
  auto &realtimePacer = *realtimePacerStorage;
  // Before anything is released: frames still queued behind the exit are
  // discarded, not drained (they would run against state the exit tears
  // down), the render worker is joined and the plan worker stopped. Every
  // renderer-owned value below is read after the last command ran, and the
  // shader-source cache with no build still running. A run that closed
  // itself first completes what it submitted (nothing is released yet).
  if (scriptedExit.load() && !renderStream.ShutdownRequested()) renderStream.Drain();
  StopRendering();
  CpuProbe::Thread.Active = false;
  // The audio worker stops before anything below can tear the device down.
  hostAudioStream.reset();

#if defined(__SWITCH__)
  WriteSwitchBootStage(
      processResult.Kind ==
              Oot3dNativeGame::NativeA32ProcessRunKind::Terminated
          ? "runtime_guest_terminated"
          : "runtime_window_closed");
#endif

  if (!launch.AudioPcmDumpPath.empty()) {
    Oot3dNativeGame::WriteStereoPcm16Wave(
        launch.AudioPcmDumpPath,
        Oot3dNativeGame::NativeA32DspHle::NativeSampleRate, pcmDumpSamples);
  }

  blockTraceWriter.Write();
  picaSemanticTrace.Finish();

  WriteNativeA32WindowRuntimeSummary({launch,
      controlConfigRuntime, activeTopScreenConfig, scenarioBootstrap, hdFonts,
      hdFontsLoaded, quickStatePath, hostServices, frameRatePolicy,
      sceneViewProbe, playerTimingProbe, temporalEventLedger, playerTemporalBridge,
      widescreenProjection, enableCompiledFunctions, wholeAotAvailable, enableWholeAot,
      enableManualCompiledFunctions, process, uiLifecycleBridge, nativeCandidateDispatch,
      submissionQueue, hostArgs, window, api,
      nativeUiTextureProvider, ocarinaText, n64UiRenderer, inputTimeline,
      phaseTiming, inputDiagnostics, inputConsumerDiagnostics, nativeControlPollingState,
      shaderSourceCache, audioPlayer, processResult, frameCount,
      runFrameCount, presentationFrameCount, nextVblankTick, lateVblankDeadlineCount,
      maximumVblankDeadlineOvershootTicks, savestateStats, presentationScheduler, visualSampleCadence,
      visualContinuityEpoch, picaStats, visualStats, visualInterpolationTiming,
      vblankCount, dspStats, displayTransferEvents, drawEvents,
      visualTransitionEvents, timing, frameCompositionPolicy, realtimePacer,
      visualInterpolationEnabled, audioOutputDiagnostics, pcmContinuityDiagnostics, pcmDumpSamples,
      screenshotState, publishedSceneViewCount, automatedStateSavedAtRunFrame, automatedStateSavedAtGuestFrame,
      benchmark, benchmarkFrameTimes, cpuProbeRequested, benchmarkCpuPhases,
      picaCompositionTracker, topScreenTextureOverridePackPointer, topScreenTextureOverrideRuntime, preparationScreen,
      lastTopScreenPrimitivesBySubsystem, picaPresentationScheduler, parallelPresentation});

  if (launch.ExtendedDiagnostics &&
      (process.Memory().RangeWatchActive() || watchReportRequested)) {
    const auto& watched = process.Memory().WatchCounters();
    std::fprintf(
        stderr,
        "TRIAEVUM_RAW_COPY_HOST_ACCESS active=%d ranges=%zu typed_reads=%llu typed_writes=%llu "
        "byte_reads=%llu pointer_reads=%llu pointer_writes=%llu byte_writes=%llu "
        "host_byte_writes=%llu fills=%llu suppressed_byte_writes=%llu first_read=0x%08X "
        "first_size=%u first_write=0x%08X first_write_size=%u\n",
        process.Memory().RangeWatchActive() ? 1 : 0,
        process.Memory().WatchedRangeCount(),
        static_cast<unsigned long long>(watched.TypedReads),
        static_cast<unsigned long long>(watched.TypedWrites),
        static_cast<unsigned long long>(watched.ByteReads),
        static_cast<unsigned long long>(watched.PointerReads),
        static_cast<unsigned long long>(watched.PointerWrites),
        static_cast<unsigned long long>(watched.ByteWrites),
        static_cast<unsigned long long>(watched.HostByteWrites),
        static_cast<unsigned long long>(watched.Fills),
        static_cast<unsigned long long>(watched.SuppressedByteWrites),
        watched.FirstReadAddress,
        static_cast<unsigned>(watched.FirstReadSize),
        watched.FirstWriteAddress,
        static_cast<unsigned>(watched.FirstWriteSize));
  }

  if (launch.ScenarioStrict &&
      (!scenarioBootstrap.has_value() || !scenarioBootstrap->Complete())) {
    const std::string detail =
        scenarioBootstrap.has_value()
            ? scenarioBootstrap->Stats().Error
            : "structural scenario bootstrap is unavailable";
    throw std::runtime_error("structural scenario did not complete capture" +
                             (detail.empty() ? std::string() : ": " + detail));
  }
}

} // namespace Oot3dA32Window

void RunOot3dNativeA32Window(const Oot3dNativeGameLaunch &launch) {
  // The TopScreen UI is a port of OoT3D's interface (OoT3D addresses): another
  // title (TitleProfile::Oot3dGameSemantics) runs the classic UI instead.
  if (launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    const auto manifest = Oot3dNativeGame::LoadNativeA32ProcessImageManifest(
        launch.A32ProcessManifestPath, nullptr);
    if (manifest.has_value() &&
        !Oot3dNativeGame::ResolveTitleProfile(
             Oot3dNativeGame::ReadProgramId(manifest->CodeBinPath))
             .Oot3dGameSemantics) {
      std::fprintf(stderr, "ui profile: topscreen is OoT3D-only; this title "
                           "runs the classic UI\n");
      Oot3dNativeGameLaunch classic = launch;
      classic.UiProfile = Oot3dNativeGame::Oot3dUiProfile::Oot3d;
      Oot3dA32Window::NativeA32WindowSession session(classic);
      session.Run();
      return;
    }
  }
  Oot3dA32Window::NativeA32WindowSession session(launch);
  session.Run();
}
