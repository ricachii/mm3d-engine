#pragma once

// Shared declarations of the A32 window host (oot3d_native_a32_window.cpp) and its topic files:
// title addresses, the dispatch and projection state, and the functions the window calls.

#include <cstdio>
#include <unordered_set>
#include "oot3d_native_a32_window.h"
#include "oot3d_dev_free_camera.h"
#include "three_ds_sdl_controller.h"
#include "oot3d_prebackend_budget.h"
#include "oot3d_cpu_phase_probe.h"
#if defined(__SWITCH__)
#include "fast/oot3d/debug_menu.h"
#endif
#include "fast/renderer/frame_time_distribution.h"
#include "fast/renderer/slow_frame_samples.h"
#include "fast/renderer3ds/pica_program_preparation.h"
#include "fast/renderer3ds/slow_step_trace.h"
#include "oot3d_native_preparation_screen.h"
#include "oot3d_game_language_panel.h"
#include "oot3d_video_settings_panel.h"
#include "oot3d_graphics_settings_schedule.h"
#include "oot3d_scene_environment.h"
#ifdef OOT3D_WHOLE_AOT_PRODUCT_MODE
#include "oot3d_native_crash_diagnostics.h"
#endif

#include "oot3d_a32_provenance.h"
#include "oot3d_demo_host_context.h"
#include "oot3d_demo_host_io.h"
#include "oot3d_demo_host_screenshot.h"
#include "oot3d_demo_host_window_timing.h"
#include "oot3d_legacy_ui_adapters.h"
#include "oot3d_n64_ui_renderer.h"
#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_dsp_hle.h"
#include "oot3d_native_a32_dsp_memory_adapter.h"
#include "oot3d_native_a32_process_image.h"
#include "oot3d_native_a32_registry.h"
#include "oot3d_native_a32_savestate.h"
#include "oot3d_native_a32_scene_view.h"
#include "oot3d_native_actor_interactions.h"
#include "oot3d_native_a32_actor_visibility.h"
#include "oot3d_native_actor_profile.h"
#include "oot3d_native_indirect_profile.h"
#include "oot3d_native_area.h"
#include "fast/oot3d/grass_interaction_bridge.h"
#include "oot3d_native_a32_timing_probe.h"
#include "oot3d_native_compiled_functions.h"
#include "oot3d_native_controls_settings_panel.h"
#include "oot3d_native_frame_rate.h"
#include "oot3d_native_game_bootstrap.h"
#include "oot3d_native_mass_aot.h"
#include "oot3d_native_pcm_diagnostics.h"
#include "oot3d_native_presentation_pacer.h"
#include "oot3d_native_pica_frontend.h"
#include "oot3d_native_pica_composition.h"
#include "oot3d_native_pica_presentation_scheduler.h"
#include "oot3d_native_render_command_stream.h"
#include "oot3d_native_pica_semantic_trace.h"
#include "oot3d_native_pica_submission.h"
#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_native_pica_visual_savestate.h"
#include "oot3d_native_pica_vulkan_bridge.h"
#include "oot3d_native_pica_vulkan_plan.h"
#include "oot3d_native_player_temporal_bridge.h"
#include "oot3d_native_scenario_bootstrap.h"
#include "oot3d_native_temporal_event_ledger.h"
#include "oot3d_native_true_aot_blocks.h"
#include "oot3d_native_ui_lifecycle_bridge.h"
#include "oot3d_native_ui_texture_provider.h"
#include "oot3d_top_screen_ocarina_text_runtime.h"
#include "oot3d_top_screen_pause_transaction.h"
#include "oot3d_top_screen_item_description_runtime.h"
#include "oot3d_top_screen_wide_items_runtime.h"
#include "oot3d_native_a32_window_guest_pcs.h"
#include "oot3d_native_whole_aot_runtime.h"
#ifdef OOT3D_NATIVE_DIRECT_AOT_PLUGIN
#include "oot3d_native_direct_aot.h"
#endif
#include "oot3d_top_screen_camera_guest.h"
#include "oot3d_top_screen_controls.h"
#include "oot3d_top_screen_frontend_composition.h"
#include "oot3d_top_screen_gameplay_actions.h"
#include "oot3d_top_screen_gameplay_action_consumer.h"
#include "oot3d_top_screen_item_guest.h"
#include "oot3d_top_screen_item_dispatch.h"
#include "oot3d_top_screen_input_cadence.h"
#include "oot3d_top_screen_items_hint.h"
#include "oot3d_top_screen_settings_panel.h"
#include "oot3d_top_screen_texture_overrides.h"
#include "oot3d_native_hd_font_runtime.h"
#include "oot3d_top_screen_texture_runtime.h"
#include "oot3d_typed_gameplay_bridge.h"

#include "fast/Fast3dGui.h"
#include "fast/Fast3dWindow.h"
#include "fast/oot3d/graphics_settings_persistence.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/perspective_fov_policy.h"
#include "fast/oot3d/presentation_pacing_policy.h"
#include "fast/oot3d/title_render_backend.h"
#include "ship/Context.h"
#include "ship/audio/Audio.h"
#include "ship/audio/AudioPlayer.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h"
#include "ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h"
#include "ship/controller/physicaldevice/GlobalSDLDeviceSettings.h"
#include "ship/window/gui/Gui.h"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif


namespace Oot3dA32Window {

inline constexpr uint64_t kCtrArm11TicksPerSecond = 268111856ULL;
inline constexpr uint64_t kGuestDisplayRefreshRate =
    Oot3dNativeGame::kOot3dNativeTimeUnitsPerSecond;
inline constexpr uint32_t kMinimumHostAudioPrebufferMilliseconds = 100U;
inline constexpr uint64_t kA32RuntimeSampleDenominator = 64ULL;
inline constexpr uint64_t kNativeDependencyRenderTargetNamespace = 1ULL;
inline constexpr uint64_t kVisualInterpolationRenderTargetNamespace = 2ULL;
// Reduced-resolution in-between frames (config/interp_scale): images of
// their own, at the lower scale, so the full-scale ones stay intact.
inline constexpr uint64_t kReducedVisualRenderTargetNamespace = 3ULL;
inline constexpr size_t kMaximumPicaDrainPassesPerRefresh = 64U;
inline constexpr size_t kMaximumA32BlockTraceRecords = 65'536U;

namespace CpuProbe = Oot3dNativeGame::CpuPhaseProbe;

struct NativeFramePhaseTiming {
  double GuestSeconds = 0.0;
  double GuestInsidePicaSubmitSeconds = 0.0;
  double GuestClockSeconds = 0.0;
  double FrameStartSeconds = 0.0;
  double HostFrameStartSeconds = 0.0;
  double InputPollSeconds = 0.0;
  double RendererFrameStartSeconds = 0.0;
  double DspMixSeconds = 0.0;
  double AudioOutputSeconds = 0.0;
  double PicaSubmitSeconds = 0.0;
  double PicaPlanSeconds = 0.0;
  double PicaBackendSeconds = 0.0;
  double PicaDiagnosticsSeconds = 0.0;
  double VisualPresentationSeconds = 0.0;
  double VisualReplaySeconds = 0.0;
  double PresentSeconds = 0.0;
  double PacingWaitSeconds = 0.0;
  std::array<double, 15> Values() const {
    return {GuestSeconds, FrameStartSeconds, HostFrameStartSeconds, InputPollSeconds,
      RendererFrameStartSeconds, DspMixSeconds, AudioOutputSeconds, PicaSubmitSeconds,
      PicaPlanSeconds, PicaBackendSeconds, PicaDiagnosticsSeconds,
      VisualPresentationSeconds, VisualReplaySeconds, PresentSeconds, PacingWaitSeconds};
  }
};

struct NativeAudioOutputDiagnostics {
  void Observe(uint32_t hostFrame, int32_t before, int32_t after,
               uint64_t requestedFrames, int32_t desiredBuffered) {
    const auto now = std::chrono::steady_clock::now();
    if (HasSubmitTime) {
      const double gapSeconds =
          std::chrono::duration<double>(now - LastSubmitTime).count();
      MaximumSubmitGapSeconds = std::max(MaximumSubmitGapSeconds, gapSeconds);
      if (gapSeconds >= 0.05 && SubmitGapEvents.size() < 64U) {
        SubmitGapEvents.push_back({
            {"host_frame", hostFrame},
            {"gap_seconds", gapSeconds},
            {"buffered_before", before},
        });
      }
    }
    LastSubmitTime = now;
    HasSubmitTime = true;
    ++SubmitCalls;
    RequestedFrames += requestedFrames;
    if (!HasBufferObservation) {
      MinimumBufferedBefore = before;
      MaximumBufferedBefore = before;
      MinimumBufferedAfter = after;
      MaximumBufferedAfter = after;
      HasBufferObservation = true;
    } else {
      MinimumBufferedBefore = std::min(MinimumBufferedBefore, before);
      MaximumBufferedBefore = std::max(MaximumBufferedBefore, before);
      MinimumBufferedAfter = std::min(MinimumBufferedAfter, after);
      MaximumBufferedAfter = std::max(MaximumBufferedAfter, after);
    }
    const bool wasPrimed = Primed;
    if (after >= desiredBuffered) {
      Primed = true;
    }
    if (wasPrimed && before == 0) {
      ++StarvationObservations;
      StarvationEvents.push_back({
          {"host_frame", hostFrame},
          {"buffered_before", before},
          {"buffered_after", after},
          {"requested_frames", requestedFrames},
      });
    }
    const uint64_t growth =
        after > before ? static_cast<uint64_t>(after - before) : 0U;
    if (growth < requestedFrames) {
      EstimatedShortfallFrames += requestedFrames - growth;
      ++ShortGrowthSubmissions;
    }
  }

  bool HasBufferObservation = false;
  bool HasSubmitTime = false;
  bool Primed = false;
  std::chrono::steady_clock::time_point LastSubmitTime{};
  double MaximumSubmitGapSeconds = 0.0;
  uint64_t SubmitCalls = 0;
  uint64_t RequestedFrames = 0;
  uint64_t ShortGrowthSubmissions = 0;
  uint64_t EstimatedShortfallFrames = 0;
  uint64_t StarvationObservations = 0;
  int32_t MinimumBufferedBefore = 0;
  int32_t MaximumBufferedBefore = 0;
  int32_t MinimumBufferedAfter = 0;
  int32_t MaximumBufferedAfter = 0;
  nlohmann::json StarvationEvents = nlohmann::json::array();
  nlohmann::json SubmitGapEvents = nlohmann::json::array();
};

struct NativeInputConsumerDiagnostics {
  uint64_t Samples = 0;
  uint64_t ReadFailures = 0;
  uint64_t HeldFrames = 0;
  uint64_t PressedFrames = 0;
  uint64_t ReleasedFrames = 0;
  uint32_t LastHeld = 0;
  uint32_t LastPressed = 0;
  uint32_t LastReleased = 0;
  uint32_t LastRawCurrent = 0;
  uint32_t LastRawPrevious = 0;
  bool HasPrevious = false;
  nlohmann::json Transitions = nlohmann::json::array();

  void Observe(Oot3dNativeGame::NativeA32Memory &memory, uint32_t hostFrame) {
    uint32_t held = 0;
    uint32_t pressed = 0;
    uint32_t released = 0;
    uint32_t rawCurrent = 0;
    uint32_t rawPrevious = 0;
    if (!memory.Read32(kInputGlobalState + kInputHeldOffset, &held) ||
        !memory.Read32(kInputGlobalState + kInputPressedOffset, &pressed) ||
        !memory.Read32(kInputGlobalState + kInputReleasedOffset, &released) ||
        !memory.Read32(kInputGlobalState + kInputRawCurrentOffset,
                       &rawCurrent) ||
        !memory.Read32(kInputGlobalState + kInputRawPreviousOffset,
                       &rawPrevious)) {
      ++ReadFailures;
      return;
    }
    ++Samples;
    HeldFrames += held != 0U ? 1U : 0U;
    PressedFrames += pressed != 0U ? 1U : 0U;
    ReleasedFrames += released != 0U ? 1U : 0U;
    const bool changed = !HasPrevious || held != LastHeld ||
                         pressed != LastPressed || released != LastReleased ||
                         rawCurrent != LastRawCurrent ||
                         rawPrevious != LastRawPrevious;
    if (changed && Transitions.size() < 64U) {
      Transitions.push_back({
          {"host_frame", hostFrame},
          {"held", held},
          {"pressed", pressed},
          {"released", released},
          {"raw_current", rawCurrent},
          {"raw_previous", rawPrevious},
      });
    }
    LastHeld = held;
    LastPressed = pressed;
    LastReleased = released;
    LastRawCurrent = rawCurrent;
    LastRawPrevious = rawPrevious;
    HasPrevious = true;
  }
};

struct NativeSceneProjectionState {
  struct CallsiteStats {
    uint64_t Calls = 0;
    float FirstLeft = 0.0F;
    float FirstRight = 0.0F;
    // The latest perspective frustum this callsite received, before the
    // presentation policy widens it (left, right, bottom, top, near, far).
    std::array<float, 6> LastFrustum{};
  };

  Fast::Oot3d::ScenePresentationPolicy Presentation{};
  float FovMultiplier = 1.0F;
  uint64_t FrustumCalls = 0;
  uint64_t OrthographicCalls = 0;
  std::map<std::pair<uint32_t, uint32_t>, CallsiteStats> Callsites;
};

struct NativeWidescreenProjectionState {
  struct A32BlockTraceRecord {
    uint32_t HostFrame = 0;
    uint32_t Pc = 0;
    uint64_t MemoryWriteGeneration = 0;
    uint64_t MemoryWriteFingerprint = 0;
    uint64_t FastReadMismatchCount = 0;
    uint32_t LastFastReadMismatchAddress = 0;
    uint64_t LastFastReadMismatchValue = 0;
    uint64_t LastCheckedReadMismatchValue = 0;
    std::array<uint32_t, 16> Registers{};
    uint32_t Cpsr = 0;
    uint32_t Fpscr = 0;
    uint32_t ThreadPointer = 0;
    std::array<uint32_t, 32> Vfp{};
    uint32_t ExclusiveAddress = 0;
    uint64_t ExclusiveToken = 0;
    uint8_t ExclusiveSize = 0;
    bool ExclusiveValid = false;
  };

  NativeSceneProjectionState Projection;
  Oot3dNativeGame::NativeActorVisibilityState ActorVisibility;
  bool CollectExtendedDiagnostics = false;
  uint32_t CurrentHostFrame = 0;
  uint32_t EnMagInitCalls = 0;
  uint32_t EnMagUpdateCalls = 0;
  uint32_t EnMagDrawCalls = 0;
  nlohmann::json EnMagEvents = nlohmann::json::array();
  std::map<std::string, uint64_t> UiLifecycleCallCounts;
  nlohmann::json UiLifecycleEvents = nlohmann::json::array();
  nlohmann::json GameStateEvents = nlohmann::json::array();
  uint64_t GameStateUpdateCalls = 0;
  uint32_t LastGameStateAddress = 0;
  uint32_t LastGameStateMain = 0;
  uint32_t LastNextGameStateInit = 0;
  uint32_t LastNativeGameMode = 0;
  uint8_t LastGameStateRunning = 0;
  uint8_t LastTransitionTrigger = 0;
  uint8_t LastTransitionType = 0;
  uint8_t LastTransitionState = 0;
  bool HasGameStateSnapshot = false;
  nlohmann::json RendererQueueEvents = nlohmann::json::array();
  std::map<std::string, uint64_t> RendererQueueCallCounts;
  bool ProfileA32Blocks = false;
  uint64_t BlockEntries = 0;
  uint32_t BlockSampleState = 0x6D2B79F5U;
  std::unordered_map<uint32_t, uint64_t> BlockSamples;
  bool TraceA32Blocks = false;
  // TRIAEVUM_CALL_TRACE=<file>: first visit of every block per guest thread.
  bool CallTrace = false;
  std::FILE *CallTraceFile = nullptr;
  uint64_t CallTraceSequence = 0;
  std::unordered_set<uint64_t> CallTraceSeen;
  bool BlockTraceTruncated = false;
  uint64_t BlockTraceRecordsObserved = 0;
  Oot3dNativeGame::NativeA32Memory *TraceMemory = nullptr;
  Oot3dNativeGame::NativeHdFontRuntime *HdFonts = nullptr;
  std::deque<A32BlockTraceRecord> BlockTrace;
  Oot3dNativeGame::Oot3dNativeUiLifecycleBridge *UiLifecycleBridge = nullptr;
  Oot3dNativeGame::TopScreenOcarinaTextRuntime *OcarinaText = nullptr;
  Oot3dNativeGame::TopScreenPauseTransactionRuntime *PauseTransaction = nullptr;
  Oot3dNativeGame::TopScreenSystemPageRuntime *SystemPage = nullptr;
  Oot3dNativeGame::TopScreenItemDescriptionRuntime *ItemDescription = nullptr;
  Oot3dNativeGame::Oot3dPicaCompositionDomain *PicaCompositionDomain =
      nullptr;
  Oot3dNativeGame::Oot3dNativePicaCompositionTracker
      *PicaCompositionTracker = nullptr;
  Oot3dNativeGame::TopScreenTextureOverrideRuntime *TopScreenTextureOverrides =
      nullptr;
  bool TopScreenUiProfile = false;
  Oot3dNativeGame::TopScreenUiConfig TopScreenConfig;
  Oot3dNativeGame::TopScreenExtendedInputFrame *TopScreenInput = nullptr;
  Oot3dNativeGame::TopScreenInputCadence *TopScreenInputClock = nullptr;
  const Oot3dNativeGame::TopScreenItemDispatchRuntime *TopScreenItems = nullptr;
  std::span<const uint32_t> WholeAotObservableExitPcs;
  // Set to the pc of each observable exit this policy raises (see
  // NativeCandidateDispatchState::ObservedExitPc).
  std::optional<uint32_t> *ObservedExitPc = nullptr;
  std::optional<Oot3dNativeGame::TopScreenPauseTargetPlan>
      PendingTopScreenPauseTarget;
  uint32_t PendingTopScreenPauseRenderer = 0U;
  uint64_t TopScreenPauseTargetObservations = 0U;
  uint64_t TopScreenPauseTargetCommands = 0U;
  uint64_t TopScreenPauseTargetRewrites = 0U;
  uint64_t TopScreenPauseChildSuppressions = 0U;
  uint64_t TopScreenPauseChildNativeDraws = 0U;
  bool HasTopScreenPauseDrawInputs = false;
  bool LastTopScreenPauseRouteActive = false;
  bool LastTopScreenPauseChildrenSuppressed = false;
  Oot3dNativeGame::TopScreenPauseDrawInputs LastTopScreenPauseDrawInputs{};
  Oot3dNativeGame::TopScreenPausePageRedrawState TopScreenPausePageRedraw;
  Oot3dNativeGame::TopScreenTouchCoordinateRouteState
      *TopScreenTouchCoordinateRoute = nullptr;
  bool TopScreenPausePageRedrawActive = false;
  uint64_t TopScreenPausePageCompositionCalls = 0U;
  uint64_t TopScreenPausePageRedraws = 0U;
  uint64_t TopScreenPausePageRedrawSkips = 0U;
  uint64_t TopScreenPauseProjectionCalls = 0U;
  uint64_t TopScreenPauseProjectionFailures = 0U;
  nlohmann::json TopScreenPauseProjectionEvents = nlohmann::json::array();
  Oot3dNativeGame::TopScreenPauseRouteState *TopScreenPauseTargetRoute =
      nullptr;
  Oot3dNativeGame::TopScreenPauseDrawRoutingState TopScreenPauseDrawRouting;
  std::optional<Oot3dNativeGame::TopScreenPauseChildState>
      PendingTopScreenPauseChildRestore;
  Oot3dNativeGame::NativeFrameRatePolicy *FrameRatePolicy = nullptr;
  Oot3dNativeGame::NativeA32SceneViewProbe *SceneViewProbe = nullptr;
  // The developer free camera owns Camera_Update while it is active.
  const Oot3dNativeGame::DevFreeCamera *DevFreeCamera = nullptr;
  Oot3dNativeGame::NativeA32PlayerTimingProbe *PlayerTimingProbe = nullptr;
  Oot3dNativeGame::NativeTemporalEventLedger *TemporalEventLedger = nullptr;
  Oot3dNativeGame::NativeA32PlayerTemporalBridge *PlayerTemporalBridge =
      nullptr;
  // Actor cost profile (config/actor_profile); null when off.
  Oot3dNativeGame::NativeActorProfile *ActorProfile = nullptr;
  nlohmann::json PlayerTimingEvents = nlohmann::json::array();
  bool PlayerTimingEventsTruncated = false;
};

struct NativeCandidateDispatchState {
  oot3d::recomp::a32::BlockEntryCallback BlockEntry = nullptr;
  void *BlockEntryUser = nullptr;
  std::span<const uint32_t> BlockEntryPcs;
  Oot3dNativeGame::Oot3dMassAotPcFilter MassAotBlockEntryFilter;
  Oot3dNativeGame::Oot3dMassAotPcFilter MassAotObservableExitFilter;
  Oot3dNativeGame::NativeA32Memory *Memory = nullptr;
  Oot3dNativeGame::Oot3dTypedGameplayContext TypedGameplayContext;
  bool TypedGameplay = false;
  bool MassAot = false;
  bool CompiledFunctions = false;
  bool WholeAot = false;
  bool ManualCompiledFunctions = true;
  bool TrueAotBlocks = false;
  bool ProfileRuntime = false;
  bool TopScreenUiProfile = false;
  bool TopScreenPauseAotBarrier = false;
  bool TopScreenExtendedCameraActive = false;
  Oot3dNativeGame::TopScreenFreeCameraGuestRuntime TopScreenCamera;
  // Developer free camera (inactive unless F1 > Dev Tools turns it on).
  std::shared_ptr<Oot3dNativeGame::DevFreeCamera> DevFreeCamera =
      std::make_shared<Oot3dNativeGame::DevFreeCamera>();
  bool DevFreeCameraReported = false;
  Oot3dNativeGame::TopScreenCameraFovOwnership TopScreenCameraFov;
  Oot3dNativeGame::NativeFreeCameraInputAccumulator TopScreenCameraInput;
  Oot3dNativeGame::TopScreenExtendedInputFrame TopScreenInput;
  Oot3dNativeGame::TopScreenInputCadence TopScreenInputClock;
  // The pc whose block policy raised the last observable exit: the product
  // dispatcher resumes it without running that policy a second time.
  std::optional<uint32_t> ObservedExitPc;
  // The projection policy's page redraw to the upper screen.
  const bool *TopScreenPausePageRedrawActive = nullptr;
  uint32_t PreviousTopScreenButtons = 0U;
  Oot3dNativeGame::NativeA32PolledButtonLatch StartButtonLatch;
  Oot3dNativeGame::TopScreenStartRoutingState TopScreenStartRouting;
  Oot3dNativeGame::TopScreenVersionChoiceRevealState TopScreenVersionChoiceReveal;
  Oot3dNativeGame::TopScreenPauseSystemOpenState TopScreenPauseSystemOpen;
  Oot3dNativeGame::TopScreenPauseTransactionRuntime *PauseTransaction = nullptr;
  Oot3dNativeGame::TopScreenSystemPageRuntime *SystemPage = nullptr;
  Oot3dNativeGame::TopScreenItemDescriptionRuntime *ItemDescription = nullptr;
  Oot3dNativeGame::TopScreenWideItemsRuntime *WideItems = nullptr;
  // Sorted TopScreen entries the product hook owns (product builds only).
  std::vector<uint32_t> ProductTopScreenBlocks;
  uint64_t TopScreenAimProjectileCycleAttempts = 0U;
  uint64_t TopScreenAimProjectileCycleUpdates = 0U;
  uint64_t TopScreenAimProjectileCycleSoundCalls = 0U;
  uint64_t TopScreenGameplayDpadAttempts = 0U;
  uint64_t TopScreenNaviViewActivations = 0U;
  uint64_t TopScreenOcarinaQueries = 0U;
  uint64_t TopScreenOcarinaActivations = 0U;
  Oot3dNativeGame::TopScreenGameplayActionConsumerStats
      TopScreenGameplayActions;
  Oot3dNativeGame::TopScreenGameplayActionConsumerRuntime
      TopScreenGameplayActionRuntime;
  uint8_t TopScreenViewportDrawPhase = 0U;
  uint32_t TopScreenViewportDrawArgument = 0U;
  uint8_t TopScreenOverlayViewportDrawPhase = 0U;
  uint8_t TopScreenTouchCoordinateUpdatePhase = 0U;
  std::optional<Oot3dNativeGame::TopScreenAlternateRendererState>
      TopScreenAlternateRendererRestore;
  Oot3dNativeGame::TopScreenPauseRouteState TopScreenPauseRoute;
  Oot3dNativeGame::TopScreenPauseRouteState TopScreenAotPauseRoute;
  Oot3dNativeGame::TopScreenTouchCoordinateRouteState
      TopScreenTouchCoordinateRoute;
  Oot3dNativeGame::TopScreenRendererVisibilityRouteState
      TopScreenRendererVisibilityRoute;
  uint32_t TopScreenGameOverDrawPhase = 0U;
  std::array<uint32_t, 5> TopScreenGameOverDrawRegisters{};
  Oot3dNativeGame::TopScreenPauseChildState TopScreenGameOverChildRestore{};
  // config/debug_force_game_over (off without the file).
  Oot3dNativeGame::TopScreenDebugGameOverState TopScreenDebugGameOver{};
  Oot3dNativeGame::TopScreenPauseControllerState TopScreenPauseController;
  uint8_t TopScreenPauseControllerDrawPhase = 0U;
  std::array<uint32_t, 2> TopScreenPauseControllerVisibleRenderers{};
  Oot3dNativeGame::TopScreenItemDispatchRuntime TopScreenItems;
  uint32_t SampleState = 0xA32C0DE1U;
  uint64_t Calls = 0;
  uint64_t Samples = 0;
  uint64_t TypedGameplaySamples = 0;
  uint64_t TypedGameplaySampleNanoseconds = 0;
  uint64_t CompiledSamples = 0;
  uint64_t CompiledSampleNanoseconds = 0;
  uint64_t TrueAotSamples = 0;
  uint64_t TrueAotSampleNanoseconds = 0;
  uint64_t UnhandledSamples = 0;
  uint64_t UnhandledSampleNanoseconds = 0;
  uint64_t TopScreenSourcePortCalls = 0;
  uint64_t TopScreenTitleLogoFadeHoldCalls = 0;
  uint64_t TopScreenLowerCompositionSkips = 0;
  uint64_t TopScreenCameraNormal1Calls = 0;
  uint64_t TopScreenCameraUpdateCalls = 0;
  uint64_t TopScreenCameraActiveCalls = 0;
  Oot3dNativeGame::Oot3dNativeUiLifecycleBridge *UiLifecycleBridge = nullptr;
  Oot3dNativeGame::TopScreenOcarinaTextRuntime *OcarinaText = nullptr;
  Oot3dNativeGame::Oot3dPicaCompositionDomain *PicaCompositionDomain =
      nullptr;
  Oot3dNativeGame::TopScreenPauseProjectionState *TopScreenPauseProjection =
      nullptr;
  const Oot3dNativeGame::TopScreenUiConfig *TopScreenConfig = nullptr;
};

struct NativeControlPollingState {
  int32_t ControllerInstance = -1;
  std::array<bool, Oot3dNativeGame::kNativeControlActionCount>
      PhysicalActions{};
  std::array<bool, Oot3dNativeGame::kNativeControlActionCount>
      PendingPressedActions{};
  int64_t PendingMouseDeltaX = 0;
  int64_t PendingMouseDeltaY = 0;
  double PendingInputSeconds = 0.0;
  bool GameplayMouseOwned = false;
  uint64_t MouseEligiblePolls = 0;
  uint64_t MouseReleasedPolls = 0;
  uint64_t MouseHostUiPolls = 0;
  uint64_t MouseNativeUiPolls = 0;
  uint64_t MouseCaptureTransitions = 0;
  uint64_t MouseMovementPolls = 0;
  Oot3dNativeGame::NativeRightStickProfileState RightStickProfile;
  ThreeDsRecomp::Input::VirtualMotionState VirtualMotion;
  uint32_t PreviousControllerButtons = 0;
  uint32_t PendingControllerButtons = 0;
#if defined(__SWITCH__)
  Fast::Oot3d::DebugMenuCombo DebugMenuCombo;
#endif
  // The developer free camera's own physical sources, apart from the game's
  // bindings (the right mouse button is the game's R): mouse motion since it
  // last read it, the right mouse button, and the pad's sticks (-1..1, dead
  // zone applied; Y up) and shoulders.
  int64_t DevMouseDeltaX = 0;
  int64_t DevMouseDeltaY = 0;
  bool DevMouseRightHeld = false;
  std::array<float, 2> DevPadLeftStick{};
  std::array<float, 2> DevPadRightStick{};
  bool DevPadLeftShoulder = false;
  bool DevPadRightShoulder = false;
};

bool ExecuteProductTopScreenHook(uint32_t pc, oot3d::recomp::a32::GuestState &state,
                                   oot3d::recomp::a32::MemoryBus &,
                                   oot3d::recomp::a32::ExecutionResult *result,
                                   uint32_t blockBudget, uint32_t *blocksConsumed, void *user);

bool ExecuteNativeCandidate(uint32_t pc, oot3d::recomp::a32::GuestState &state,
                            oot3d::recomp::a32::MemoryBus &memory,
                            oot3d::recomp::a32::ExecutionResult *result,
                            uint32_t blockBudget, uint32_t *blocksConsumed,
                            void *user);

void ApplyNativeWidescreenProjectionPolicy(
    uint32_t pc, oot3d::recomp::a32::GuestState &state,
    oot3d::recomp::a32::MemoryBus &memory, void *user);

Oot3dNativeGame::NativeA32InputFrame
PollNativeA32Input(Fast::Fast3dWindow &window,
                   bool nativeFrontendTouchEnabled,
                   // Where the page that owns the touch is drawn.
                   Oot3dNativeGame::NativeA32TouchPresentation
                       nativeTouchPresentation,
                   bool hostGuiVisible,
                   bool topScreenUiProfile,
                   bool topScreenFreeCameraEnabled,
                   Oot3dNativeGame::NativeControlConfigRuntime &controls,
                   const Oot3dNativeGame::NativeAimProfileTransform
                       &aimTransform,
                   double samplePeriodSeconds,
                   bool guestRefreshWillConsume,
                   NativeControlPollingState &pollingState,
                   // The gameplay HUD drawn last frame (400x240 canvas):
                   // on Switch, taps on its controls press them.
                   std::span<const oot3d::ui::UiPrimitive>
                       topScreenHudPrimitives,
                   bool widescreenMenuCorners = false);

Oot3dNativeGame::NativeA32InputFrame ResolveNativeA32GuestInput(
    const Oot3dNativeGame::NativeA32InputFrame &physical,
    const Oot3dNativeGame::NativeA32InputTimeline &timeline,
    uint32_t guestFrameIndex, uint32_t runFrameIndex,
    bool nativeFrontendTouchEnabled,
    bool topScreenStartRoutingEligible, bool topScreenUiProfile,
    Oot3dNativeGame::NativeA32PolledButtonLatch &startButtonLatch,
    Oot3dNativeGame::TopScreenStartRoutingState &startRoutingState);

bool ResolveNativeA32TouchInputEnabled(
    Oot3dNativeGame::NativeA32Memory &memory,
    const Oot3dNativeGame::Oot3dNativeUiLifecycleBridge &uiLifecycleBridge,
    bool topScreenUiProfile);

// The default profile's lower screen, or the TopScreen page on the top
// screen: the widened Items page while Items is open (the predicate the
// wide-page draw uses, ReadTopScreenWidePageState), else a centred page.
Oot3dNativeGame::NativeA32TouchPresentation ResolveNativeA32TouchPresentation(
    Oot3dNativeGame::NativeA32Memory &memory, bool topScreenUiProfile);

} // namespace Oot3dA32Window
