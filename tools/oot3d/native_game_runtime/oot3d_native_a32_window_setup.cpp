// Native A32 window session: setup (process image, host services, renderer, UI, initial state).
#include "fast/oot3d/gl_widescreen_menu_policy.h"
#include "oot3d_native_a32_window_session.h"
#include "oot3d_gameplay_settings_panel.h"
#include "oot3d_draw_distance_config.h"
#include "oot3d_landing_roll.h"
#include "oot3d_native_block_entry_hooks.h"
#include "oot3d_native_guest_overrides.h"
#include "oot3d_native_a32_memory_peek.h"
#include "oot3d_native_kernel_options.h"
#include "oot3d_switch_threads.h"
#include "oot3d_switch_guest_window.h"
#if defined(__SWITCH__)
#include <EGL/egl.h>
#include <glad/glad.h>
#include <switch.h>
#include "fast/backends/gfx_sdl.h"
#include "fast/backends/gfx_deko3d.h"
#include <algorithm>
#include <fstream>
#include "oot3d_build_version.h"
#include "oot3d_update_panel.h"
#include "oot3d_switch_code_pages.h"
#include "oot3d_whole_aot_gcov.h"
#include "oot3d_game_log.h"
#include <spdlog/spdlog.h>
#endif
// Portables: se usan sin condicion mas abajo (en PC fallaba la compilacion).
#include "fast/backends/gfx_deko3d.h"
#include "oot3d_game_log.h"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "oot3d_native_direct_aot.h"
#include "oot3d_title_grass_rules.h"
#include "oot3d/renderer/azahar_texture_pack.h"
#include <stb_image.h>
#include "fast/oot3d/graphics_settings_persistence.h"
#include "fast/oot3d/gl_widescreen_policy.h"

namespace Oot3dA32Window {

namespace {

std::vector<Oot3dNativeGame::Oot3dPicaPhysicalMemoryRegion>
BuildPicaMemoryRegions(
    const Oot3dNativeGame::NativeA32ProcessImageManifest &manifest) {
  std::vector<Oot3dNativeGame::Oot3dPicaPhysicalMemoryRegion> regions{
      {0x20000000U, manifest.LinearHeapBaseAddress, manifest.LinearHeapSize}};
  for (const auto &region : manifest.SystemRegions) {
    if (region.Name == "ctr_vram") {
      regions.push_back({0x18000000U, region.Address, region.MappedSize});
      break;
    }
  }
  return regions;
}

#if defined(__SWITCH__)
constexpr const char *kSwitchAudioSampleRatePath =
    "sdmc:/switch/oot3dre/config/audio_sample_rate";
constexpr const char *kSwitchAudioDumpFlagPath =
    "sdmc:/switch/oot3dre/config/audio_dump";
constexpr const char *kSwitchAudioDumpPath =
    "sdmc:/switch/oot3dre/logs/audio_dump.wav";
constexpr uint32_t kSwitchAudioDumpSeconds = 60U;
// Horizon's renderer runs at 48 kHz: handing it 48 kHz PCM leaves no rate
// conversion outside the game's own resampler.
constexpr uint32_t kSwitchDefaultAudioSampleRate = 48000U;
// Frames per SDL device buffer. devkitPro's SDL (audren backend) keeps two
// such buffers and queues at most one per 5 ms renderer frame, so a buffer must
// last well over 5 ms: the former 160 frames (4.9 ms at 32728 Hz) delivered
// 32000 frames/s for a 32728 Hz voice and starved it continuously.
// SWITCHAUDIO_PlayDevice queues buffer i behind the playing one and polls, one
// renderer frame at a time, until i plays: the thread then has one buffer
// minus up to two 5 ms frames (polling and the renderer's lead) to queue the
// next. 512 frames (10.7 ms) left ~1 ms; the console's host queue grew as if
// the device drained 5-10% below 48 kHz, i.e. the voice ran dry between
// buffers (heard as grain). 1024 frames (21.3 ms) leave ~11 ms.
constexpr uint32_t kSwitchAudioDeviceFrames = 1024U;

// config/audio_sample_rate: a number in Hz (8000-192000); absent or invalid
// selects 48000. "32728" restores the former output at the DSP's own rate.
uint32_t ReadSwitchAudioSampleRate() {
  std::ifstream input(kSwitchAudioSampleRatePath);
  if (!input) {
    return kSwitchDefaultAudioSampleRate;
  }
  long long value = 0;
  if (!(input >> value) || value < 8000 || value > 192000) {
    std::fprintf(stderr, "audio: ignoring invalid %s\n",
                 kSwitchAudioSampleRatePath);
    return kSwitchDefaultAudioSampleRate;
  }
  return static_cast<uint32_t>(value);
}

// config/audio_device_frames: frames per SDL device buffer (256-4096). The
// audren backend keeps only two such buffers, so this is the slack the SDL
// audio thread has before the voice runs dry.
uint32_t ReadSwitchAudioDeviceFrames() {
  std::ifstream input("sdmc:/switch/oot3dre/config/audio_device_frames");
  long long value = 0;
  if (!input || !(input >> value) || value < 256 || value > 4096) {
    return kSwitchAudioDeviceFrames;
  }
  return static_cast<uint32_t>(value);
}
#endif

// Host audio stream switches: on the Switch config/audio_thread and
// config/audio_stretch ("0" turns each off; absent = on); on the desktop
// TRIAEVUM_AUDIO_THREAD / TRIAEVUM_AUDIO_STRETCH ("1" turns each on).
bool ReadHostAudioSwitch(const char *configName, const char *environmentName) {
#if defined(__SWITCH__)
  (void)environmentName;
  std::ifstream input(std::string("sdmc:/switch/oot3dre/config/") + configName);
  if (!input) {
    return true;
  }
  std::string value;
  input >> value;
  return value != "0";
#else
  (void)configName;
  const char *value = std::getenv(environmentName);
  return value != nullptr && std::string_view(value) == "1";
#endif
}

} // namespace

void NativeA32WindowSession::Setup() {
  // Before any settings load: configurations older than schema 15 take the
  // title's shipped grass source and mask rules.
  Fast::Oot3d::InstallTitleGrassRuleDefaults(Oot3dNativeGame::Oot3dTitleGrassRuleDefaults());
  if (launch.Host.Renderer != "opengl" &&
      launch.Host.Renderer != "nri" &&
      launch.Host.Renderer != "vulkan") {
    throw std::runtime_error(
        "the native process requires the OpenGL or NRI/Vulkan backend");
  }

  topScreenConfigRuntime =
      std::make_shared<Oot3dNativeGame::TopScreenUiConfigRuntime>(
          launch.TopScreenConfigPath, launch.TopScreenConfig);
  controlConfigRuntime =
      std::make_shared<Oot3dNativeGame::NativeControlConfigRuntime>(
          launch.ControlConfigPath, launch.ControlConfig);
  activeTopScreenConfig =
      topScreenConfigRuntime->Snapshot().Config;
#if defined(__SWITCH__)
  gSwitchUiProfile = Oot3dNativeGame::Oot3dUiProfileName(launch.UiProfile);
  gSwitchGameplayTiming =
      Oot3dNativeGame::GameplayTimingModeName(launch.GameplayTiming);
  gSwitchControlProfile = Oot3dNativeGame::NativeControlProfileName(
      controlConfigRuntime->Snapshot().Config.Profile);
  gSwitchPresentationRateHz = launch.PresentationRateHz;
  gSwitchWholeAotBlockBudget = launch.WholeAotBlockBudget;
  gSwitchPicaGeometryCacheEnabled =
      !launch.DisableOpenGlPicaGeometryCache;
#endif

  manifest = Oot3dNativeGame::LoadNativeA32ProcessImageManifest(
      launch.A32ProcessManifestPath, &error);
  if (!manifest.has_value()) {
    throw std::runtime_error(error);
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("manifest_loaded");
#endif

  if (!launch.ScenarioId.empty()) {
    scenarioCatalog =
        Oot3dNativeGame::LoadNativeScenarioCatalog(launch.ScenarioCatalogPath);
    const auto *recipe = Oot3dNativeGame::FindNativeScenarioRecipe(
        *scenarioCatalog, launch.ScenarioId);
    if (recipe == nullptr) {
      throw std::runtime_error("unknown structural scenario: " +
                               launch.ScenarioId);
    }
    scenarioBootstrap.emplace(scenarioCatalog->Runtime, *recipe);
    std::cout << "oot3d_native_game: structural scenario " << recipe->Id
              << " selects " << recipe->ScenePath << " entrance "
              << recipe->EntranceIndex << '\n';
  }

  if (!launch.TopScreenTextureOverridePackPath.empty()) {
    const auto fontPath = launch.TopScreenTextureOverridePackPath.parent_path() / "font_coverage.zip";
    if (std::filesystem::is_regular_file(fontPath)) {
      hdFontsLoaded = hdFonts.Load(fontPath, &error);
      if (!hdFontsLoaded) std::cerr << "HD font coverage unavailable; re-run Forge: " << error << '\n';
    }
  }
  if (!launch.TopScreenTextureOverridePackPath.empty()) {
    if (!topScreenTextureOverridePack.Load(
            launch.TopScreenTextureOverridePackPath, &error)) {
      throw std::runtime_error("cannot load TopScreen texture overrides: " +
                               error);
    }
    topScreenTextureOverridePackPointer = &topScreenTextureOverridePack;
#if defined(__SWITCH__)
    gSwitchTopScreen211Assets =
        topScreenTextureOverridePack.FindProfileTexture(
            Oot3dNativeGame::kTopScreen211MenuAtlasSemantic) != nullptr &&
        topScreenTextureOverridePack.FindProfileTexture(
            "oot3d/topscreen/2.1.1/font_atlas") != nullptr;
#endif
  }

  picaFrontend.SetDiagnosticHistoryEnabled(false);
  picaCompositionDomain =
      Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  picaRefreshCompositionDomain = picaCompositionDomain;
  {
    // A list the GPU queue kicks after the next frame's HUD phase began is
    // attributed as on time; config/late_lists_as_ui restores the HUD-phase
    // attribution (A/B for the per-pixel blink, SWITCH_PORT.md).
    std::error_code ignored;
#if defined(__SWITCH__)
    const std::filesystem::path configDirectory = "sdmc:/switch/oot3dre/config";
#else
    const std::filesystem::path configDirectory =
        std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "config";
#endif
    picaFrontend.SetLateCommandListsOnTime(
        !std::filesystem::exists(configDirectory / "late_lists_as_ui", ignored));
    // A scene draw written inside a CMB pass but outside its mesh packets
    // takes that pass's composition; config/no_pass_span_fallback leaves it
    // without one (A/B, SWITCH_PORT.md "Per-pixel blink in test.10").
    picaFrontend.SetPassSpanFallback(
        !std::filesystem::exists(configDirectory / "no_pass_span_fallback", ignored));
  }
  // Another title (MM3D) has none of OoT3D's menu/message files: degrade to English instead of
  // aborting (docs/decisiones.md D-016). Any other language-detection error still propagates.
  std::vector<Oot3dNativeGame::GameLanguage> detectedLanguages;
  try {
    detectedLanguages = Oot3dNativeGame::DetectGameLanguages(manifest->RomFsImagePath,
        manifest->RomFsImageOffset, manifest->RomFsImageSize);
  } catch (const std::runtime_error& error) {
    if (std::string_view(error.what()).find("No complete OOT3D menu/message language") ==
        std::string_view::npos) {
      throw;
    }
    std::fprintf(stderr, "game language: %s; using English\n", error.what());
    detectedLanguages = {{"en", "English", 1}};
  }
  languageSettings = std::make_shared<Oot3dNativeGame::GameLanguageSettings>(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "game_language.json",
      std::move(detectedLanguages));
  hostConfig.SystemLanguage = languageSettings->SystemId();
  hostConfig.ResourceLimitValues = manifest->ResourceLimitValues;
  hostConfig.ResourceCurrentValues = manifest->ResourceCurrentValues;
  hostConfig.LinearHeapBaseAddress = manifest->LinearHeapBaseAddress;
  hostConfig.LinearHeapSize = manifest->LinearHeapSize;
  hostConfig.HeapBaseAddress = manifest->HeapBaseAddress;
  hostConfig.HeapSize = manifest->HeapSize;
  hostConfig.RomFsImagePath = manifest->RomFsImagePath;
  hostConfig.RomFsImageOffset = manifest->RomFsImageOffset;
  hostConfig.RomFsImageSize = manifest->RomFsImageSize;
  hostConfig.PicaFrontend = &picaFrontend;
  hostConfig.PicaCompositionDomain = &picaCompositionDomain;
  hostConfig.PicaRefreshCompositionDomain = &picaRefreshCompositionDomain;
  hostConfig.PicaCompositionProvider = &picaCompositionTracker;
  hostConfig.SaveDataDirectory =
      launch.SaveDataDirectory.empty()
          ? launch.A32ProcessManifestPath.parent_path() /
                "oot3d_native_savedata"
          : launch.SaveDataDirectory;
  saveDataDirectory = hostConfig.SaveDataDirectory;
  // Save states default to a folder beside the executable on Windows and beside
  // the save data elsewhere; --quick-state pins one file for scripted runs.
#if defined(_WIN32)
  constexpr bool kSavestatesBesideExecutable = true;
#else
  constexpr bool kSavestatesBesideExecutable = false;
#endif
  savestateLibrary = std::make_shared<Oot3dNativeGame::SavestateLibrary>(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "savestates.json",
      Oot3dNativeGame::DefaultSavestateDirectory(
          kSavestatesBesideExecutable,
          std::filesystem::path(std::u8string(
              reinterpret_cast<const char8_t *>(Ship::Context::GetAppBundlePath().c_str()))),
          hostConfig.SaveDataDirectory),
      launch.QuickStatePath);
  quickStatePath = launch.QuickStatePath.empty() ? savestateLibrary->Directory()
                                                 : launch.QuickStatePath;
  savestateCompatibility = Oot3dNativeGame::NativeA32SavestateCompatibility{
      manifest->CodeBinSha256,
      manifest->ProcessName,
      std::string(oot3d::recomp::kA32SourceSnapshotId),
  };
  hostConfig.DetailedSvcDiagnostics = launch.ExtendedDiagnostics;
  hostConfig.ProfileRuntime = launch.ProfileA32Runtime;
  // Guest kernel fast paths (config/kernel_*, oot3d_native_kernel_options.h).
  const Oot3dNativeGame::NativeKernelOptions kernelOptions =
      Oot3dNativeGame::ReadNativeKernelOptions();
  hostConfig.FastSvc = kernelOptions.Svc;
  Oot3dNativeGame::SecondCore::gQuietWakes = kernelOptions.Wake;
  uiSubmitHoldsKernel = kernelOptions.UiLock;
  dspMixReleasesKernel = kernelOptions.Retake;
  {
    const std::string line = std::string("kernel_fast: wake=") +
                             (kernelOptions.Wake ? "1" : "0") +
                             " svc=" + (kernelOptions.Svc ? "1" : "0") +
                             " ui_lock=" + (kernelOptions.UiLock ? "1" : "0") +
                             " retake=" + (kernelOptions.Retake ? "1" : "0");
#if defined(__SWITCH__)
    // Before InitContextForDemo: kept until the game log exists (oot3d_game_log.h).
    Oot3dNativeGame::WriteGameLog(line);
#else
    std::fprintf(stderr, "%s\n", line.c_str());
#endif
  }
  auto &hostServices = hostServicesStorage.emplace(std::move(hostConfig));
  frameRateContract =
      Oot3dNativeGame::ResolveNativeFrameRateContract(
          launch.GameplayTiming, launch.PresentationRateHz);
  savestateCompatibility.GameplayTiming = frameRateContract.Mode;
#if defined(__SWITCH__)
  // The homebrew arguments always name the timing and presentation rate
  // (config/fps60); they, not the graphics settings, set the pacing here.
  // L3+R3 > Video > FPS replaces it while the game runs (RunFrame,
  // TakeLiveFrameRateMode).
  launchFrameRateMode = Oot3dNativeGame::ResolveLaunchFrameRateMode(
      launch.GameplayTiming, launch.PresentationRateHz);
#endif
  auto &frameRatePolicy = frameRatePolicyStorage.emplace(frameRateContract);
  widescreenProjection.HdFonts = hdFontsLoaded ? &hdFonts : nullptr;
  widescreenProjection.FrameRatePolicy = &frameRatePolicy;
  widescreenProjection.SceneViewProbe = &sceneViewProbe;
  widescreenProjection.PlayerTimingProbe = &playerTimingProbe;
  widescreenProjection.TemporalEventLedger = &temporalEventLedger;
  if (frameRateContract.Mode ==
      Oot3dNativeGame::GameplayTimingMode::Enhanced60) {
    widescreenProjection.PlayerTemporalBridge = &playerTemporalBridge;
  }
  widescreenProjection.CollectExtendedDiagnostics = launch.ExtendedDiagnostics;
  widescreenProjection.ProfileA32Blocks = launch.ProfileA32Blocks;
  widescreenProjection.TraceA32Blocks = !launch.A32BlockTracePath.empty();
  {
    // Per-function profile of the guest code and switch for the native overrides.
    // Both only read config files; without them nothing changes.
    std::filesystem::path guestProfileConfig;
#if defined(__SWITCH__)
    guestProfileConfig = "sdmc:/switch/oot3dre/config/guest_profile";
    const std::filesystem::path guestProfileOutput =
        "sdmc:/switch/oot3dre/logs/guest_profile.txt";
    const std::filesystem::path guestFunctionMap =
        "sdmc:/switch/oot3dre/config/guest_functions.txt";
    std::error_code overridesError;
    Oot3dNativeGame::SetOot3dGuestOverridesActive(!std::filesystem::exists(
        "sdmc:/switch/oot3dre/config/guest_overrides_off", overridesError));
#else
    if (const char *path = std::getenv("TRIAEVUM_GUEST_PROFILE")) {
      guestProfileConfig = path;
    }
    const std::filesystem::path guestProfileOutput = "guest_profile.txt";
    const std::filesystem::path guestFunctionMap = "guest_functions.txt";
    if (const char *overrides = std::getenv("TRIAEVUM_GUEST_OVERRIDES")) {
      Oot3dNativeGame::SetOot3dGuestOverridesActive(
          std::string_view(overrides) != "0");
    }
#endif
    // Spike profile (config/guest_spike_ms; off by default): turns the
    // profile on for every frame even without config/guest_profile (then without the
    // periodic report) and writes logs/guest_spikes.txt and
    // logs/guest_spikes_summary.txt (oot3d_guest_spike_profile.h).
    std::filesystem::path guestSpikeConfig;
#if defined(__SWITCH__)
    guestSpikeConfig = "sdmc:/switch/oot3dre/config/guest_spike_ms";
    const std::filesystem::path guestSpikeLog =
        "sdmc:/switch/oot3dre/logs/guest_spikes.txt";
    const std::filesystem::path guestSpikeSummary =
        "sdmc:/switch/oot3dre/logs/guest_spikes_summary.txt";
#else
    if (const char *path = std::getenv("TRIAEVUM_GUEST_SPIKE_MS"))
      guestSpikeConfig = path;
    const std::filesystem::path guestSpikeLog = "guest_spikes.txt";
    const std::filesystem::path guestSpikeSummary = "guest_spikes_summary.txt";
#endif
    std::error_code spikeError;
    const bool guestSpikes =
        !guestSpikeConfig.empty() &&
        std::filesystem::exists(guestSpikeConfig, spikeError);
    Oot3dNativeGame::GuestProfileOptions guestProfileOptions;
    bool guestProfileOn = Oot3dNativeGame::GuestBlockProfiler::LoadConfig(
        guestProfileConfig, guestProfileOutput, guestFunctionMap,
        &guestProfileOptions);
    if (!guestProfileOn && guestSpikes) {
      guestProfileOptions = {};
      guestProfileOptions.DumpEveryHostFrames = 0U; // spike profile only
      guestProfileOptions.OutputPath = guestProfileOutput;
      guestProfileOptions.FunctionMapPath = guestFunctionMap;
      guestProfileOn = true;
    }
    // The profile takes the block-entry callback on every block, which sparse
    // whole-AOT code (--sparse-block-entries) never calls outside its gates:
    // it would see nothing and the runtime refuses the request. The game runs
    // on without it.
    if (guestProfileOn &&
        !Oot3dNativeGame::Oot3dLinkedWholeAotHookGates().EveryBlock) {
      std::fprintf(stderr,
                   "TRIAEVUM_GUEST_PROFILE refused: the whole-AOT code was "
                   "generated with --sparse-block-entries, which calls the "
                   "block-entry callback only at its hook gates; the guest "
                   "profile (config/guest_profile, config/guest_spike_ms) needs "
                   "a build without that flag\n");
      guestProfileOn = false;
    }
    if (guestProfileOn) {
      // OoT3D text segment (ExHeader): 0x00100000..0x004CA000.
      guestProfiler.Start(guestProfileOptions, 0x00100000U, 0x003CA000U);
      guestProfiler.SetAppendix([](std::ostream &out) {
        out << Oot3dNativeGame::DescribeOot3dGuestOverrides();
      });
    }
    if (guestSpikes && guestProfiler.Active()) {
      Oot3dNativeGame::GuestSpikeConfig spikeConfig;
      std::string text;
      {
        std::ifstream file(guestSpikeConfig);
        std::getline(file, text, '\0');
      }
      Oot3dNativeGame::ParseGuestSpikeConfig(text, &spikeConfig);
      const bool on = guestProfiler.EnableSpikes(spikeConfig, guestSpikeLog,
                                                 guestSpikeSummary);
      std::fprintf(stderr,
                   "TRIAEVUM_GUEST_SPIKES enabled=%d threshold_ms=%.2f "
                   "load_ms=%.2f\n",
                   on ? 1 : 0, spikeConfig.ThresholdMs, spikeConfig.LoadMs);
    }
  }
  auto &blockTraceWriter = blockTraceWriterStorage.emplace(
      launch.A32BlockTracePath, widescreenProjection);
  picaCompositionHooks =
      Oot3dNativeGame::Oot3dNativePicaCompositionHookPcs();
  typedGameplayObservableExitHooks =
      Oot3dNativeGame::Oot3dTypedGameplayObservableExitPoints();
  // The block-entry hook list comes only from Oot3dNativeBlockEntryHookPcs
  // (oot3d_native_block_entry_hooks.h): sparse whole-AOT code
  // (--sparse-block-entries) gates exactly the union it reports, through
  // tools/oot3d/native_a32_runtime/whole_aot_hook_gates.json.
  Oot3dNativeGame::Oot3dBlockEntryHookOptions blockEntryHookOptions;
  blockEntryHookOptions.PlayerTemporal =
      widescreenProjection.PlayerTemporalBridge != nullptr;
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  blockEntryHookOptions.TypedGameplayObservableExits = true;
#endif
  blockEntryHookOptions.TopScreen =
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
  blockEntryHookOptions.ExtendedDiagnostics = launch.ExtendedDiagnostics;
  blockEntryHookOptions.HdFonts = hdFontsLoaded;
#if defined(__SWITCH__)
  // Actor cost profile (config/actor_profile; off by default): its hooks
  // exist only while it is on (oot3d_native_actor_profile.h).
  if (std::error_code profileError;
      std::filesystem::exists("sdmc:/switch/oot3dre/config/actor_profile", profileError)) {
    actorProfile.emplace(Oot3dNativeGame::NativeProfileTicksPerMs());
    widescreenProjection.ActorProfile = &*actorProfile;
    blockEntryHookOptions.ActorProfile = true;
    std::fprintf(stderr, "TRIAEVUM_ACTOR_PROFILE enabled=1 hooks=%zu\n",
                 Oot3dNativeGame::NativeActorProfile::HookPcs().size());
  }
  // Indirect call profile (config/indirect_profile; off by default): the
  // dispatcher counts call site and target pairs while it is installed
  // (oot3d_native_indirect_profile.h). A PGO generate build has installed its
  // session profile already (oot3d_whole_aot_pgo.cpp): the perf line reuses it.
  if (std::error_code profileError;
      std::filesystem::exists("sdmc:/switch/oot3dre/config/indirect_profile", profileError)) {
    indirectProfile.emplace();
    std::fprintf(stderr, "TRIAEVUM_INDIRECT_PROFILE enabled=1 shared=%d\n",
                 indirectProfile->Shared() ? 1 : 0);
  }
#endif
  blockEntryHooks =
      Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs(blockEntryHookOptions);
#if defined(__SWITCH__)
  gSwitchBlockHookCount = blockEntryHooks.size();
  gSwitchBlockHooksSorted =
      std::is_sorted(blockEntryHooks.begin(), blockEntryHooks.end());
  gSwitchBlockHooksContainProcessEntry = std::binary_search(
      blockEntryHooks.begin(), blockEntryHooks.end(), 0x00100000U);
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  enableCompiledFunctions = true;
#else
  enableCompiledFunctions = !launch.DisableCompiledFunctions;
#endif
  wholeAotAvailable =
      Oot3dNativeGame::Oot3dWholeAotAvailable();
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  if (!wholeAotAvailable) {
    throw std::runtime_error(
        "whole-AOT product mode requires the audited generated backend");
  }
  enableWholeAot = true;
#else
  enableWholeAot = enableCompiledFunctions &&
                              !launch.DisableWholeAot &&
                              wholeAotAvailable;
#endif
  if (enableCompiledFunctions && !launch.DisableWholeAot &&
      !wholeAotAvailable) {
    std::cerr
        << "Warning: whole-AOT was requested but is not linked. "
           "Guest execution will use substantially slower fallback paths.\n";
  }
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  enableMassAot = false;
#else
  enableMassAot = enableCompiledFunctions &&
                             !launch.DisableMassAot &&
                             Oot3dNativeGame::Oot3dMassAotAvailable();
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  enableTrueAot = false;
#else
  enableTrueAot =
      enableCompiledFunctions && !launch.DisableTrueAotBlocks;
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  enableManualCompiledFunctions = true;
#else
  enableManualCompiledFunctions =
      !launch.DisableManualCompiledFunctions;
#endif
  trueAotEntries = Oot3dNativeGame::Oot3dTrueAotBlockEntryPoints();
  compiledFunctionEntries =
      Oot3dNativeGame::Oot3dCompiledFunctionEntryPoints(
          enableWholeAot,
          enableCompiledFunctions && enableManualCompiledFunctions);
  typedGameplayEntries =
      Oot3dNativeGame::Oot3dTypedGameplayEntryPoints();
  if (enableMassAot) {
    const auto massAotEntries = Oot3dNativeGame::Oot3dMassAotBlockEntryPoints();
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  massAotEntries.begin(), massAotEntries.end());
    Oot3dNativeGame::ResetOot3dMassAotStats();
  }
  if (enableCompiledFunctions) {
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  typedGameplayEntries.begin(),
                                  typedGameplayEntries.end());
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  compiledFunctionEntries.begin(),
                                  compiledFunctionEntries.end());
    Oot3dNativeGame::ResetOot3dTypedGameplayStats();
    Oot3dNativeGame::ResetOot3dCompiledFunctionStats();
    Oot3dNativeGame::SetOot3dCompiledFunctionProfilingEnabled(
        launch.ProfileA32Runtime);
  }
  if (enableTrueAot) {
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  trueAotEntries.begin(), trueAotEntries.end());
    Oot3dNativeGame::ResetOot3dTrueAotBlockStats();
  }
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  wholeAotObservableExitBlocks.push_back(
      Oot3dNativeGame::kOot3dMeshCommandPacketSubmitEntry);
  wholeAotObservableExitBlocks.insert(
      wholeAotObservableExitBlocks.end(), picaCompositionHooks.begin(),
      picaCompositionHooks.end());
#endif
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  if (enableCompiledFunctions && !launch.DisableTypedGameplay) {
    wholeAotObservableExitBlocks.insert(
        wholeAotObservableExitBlocks.end(),
        typedGameplayObservableExitHooks.begin(),
        typedGameplayObservableExitHooks.end());
  }
#endif
  // Sorted TopScreen entries the product hook owns (product builds only).
  std::vector<uint32_t> productTopScreenBlocks;
  if (launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    topScreenSourcePortBlocks = {
        kPauseTouchButtonsDrawCall,       kPauseTouchButtonsTouchBranch,
        kPauseTouchButtonsHeldDownQuery,
        kPauseLowerScreenBlock,           kPauseUiNativeUpdateCall,
        kPauseProjectionPrepareCall,
        kPauseAlternateViewportDrawCall,  kPauseConditionalDrawCall,
        kPauseRoutedCommandCallA,         kPauseRoutedCommandCallB,
        kPauseOverlayViewportDrawCall,    kPauseAlternateRendererUpdateCall,
        kPauseSceneSevenDrawCall,         kPauseSceneViewportDrawCall,
        kPauseSceneFiveDrawCall,          kPauseUiDrawReturn,
        Oot3dNativeGame::kTopScreenSystemPageBoundary,
        kPauseTouchCoordinateUpdateCall,  kPauseRendererVisibilityCall,
        kPauseControllerDrawCall,         kTopScreenCameraNormal1Scalar,
        kTopScreenCameraFovScalar,
        kTopScreenCameraUpdateEntry,      kTopScreenCameraUpdatePatchSite,
        kTopScreenTitleLogoFadeHoldPrefix,
        Oot3dNativeGame::kTopScreenPageProjectionUploadCall,
        Oot3dNativeGame::kTopScreenItemsPageDrawCall,
        Oot3dNativeGame::kTopScreenItemsCursorDrawCallA,
        Oot3dNativeGame::kTopScreenItemsCursorDrawCallB};
    for (const auto &contract :
         Oot3dNativeGame::TopScreenVerifiedControlFlowContracts()) {
      topScreenSourcePortBlocks.push_back(contract.Entry);
    }
    for (const auto &contract :
         Oot3dNativeGame::TopScreenVerifiedFloatLoadContracts()) {
      topScreenSourcePortBlocks.push_back(contract.Entry);
    }
    const auto mapIconSites =
        Oot3dNativeGame::TopScreenDungeonMapIconCoordinateSites();
    topScreenSourcePortBlocks.insert(topScreenSourcePortBlocks.end(),
                                    mapIconSites.begin(), mapIconSites.end());
    const auto itemEntries = Oot3dNativeGame::TopScreenItemDispatchEntries();
    topScreenSourcePortBlocks.insert(topScreenSourcePortBlocks.end(),
                                    itemEntries.begin(), itemEntries.end());
    topScreenSourcePortBlocks.push_back(kTopScreenGameplayCompositionPrefix);
    topScreenSourcePortBlocks.push_back(Oot3dNativeGame::kTopScreenInputUpdateBoundary);
    // These source ports are reached from inside large AOT regions. Make
    // their native entries observable so execution returns to the dispatcher
    // before applying the typed replacement. Keep this list sorted because
    // the whole-AOT block-entry callback uses binary_search.
    const std::array topScreenObservableExitBlocks{
        kTopScreenCameraNormal1Scalar,
        kTopScreenCameraUpdateEntry,
        kTopScreenGameplayCompositionPrefix,
        kTopScreenTitleLogoFadeHoldPrefix,
    };
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
    wholeAotObservableExitBlocks.insert(
        wholeAotObservableExitBlocks.end(),
        topScreenObservableExitBlocks.begin(),
        topScreenObservableExitBlocks.end());
#else
    // The product runs the same TopScreen source port as developer builds:
    // every block is an observable exit of the whole-AOT graph, because the
    // product keeps whole-AOT execution through pause routes.
    wholeAotObservableExitBlocks.insert(wholeAotObservableExitBlocks.end(),
                                       topScreenObservableExitBlocks.begin(),
                                       topScreenObservableExitBlocks.end());
    wholeAotObservableExitBlocks.insert(wholeAotObservableExitBlocks.end(),
                                       topScreenSourcePortBlocks.begin(),
                                       topScreenSourcePortBlocks.end());
    productTopScreenBlocks = topScreenSourcePortBlocks;
    productTopScreenBlocks.push_back(kPauseRendererVisibilityNativeDraw);
    productTopScreenBlocks.insert(productTopScreenBlocks.end(), itemEntries.begin(),
                                  itemEntries.end());
    std::sort(productTopScreenBlocks.begin(), productTopScreenBlocks.end());
    // The whole-AOT graph reports only its block starts: a hook or resume pc
    // that is not an entry of the title module would silently never run.
    // tools/oot3d/native_a32_runtime/whole_aot_observed_sites.json makes the
    // generator start a block at each of them.
    {
      std::vector<uint32_t> required = productTopScreenBlocks;
      for (const auto &contract :
           Oot3dNativeGame::TopScreenVerifiedControlFlowContracts())
        required.push_back(contract.Target);
      for (const auto &contract :
           Oot3dNativeGame::TopScreenVerifiedFloatLoadContracts())
        required.push_back(contract.Entry + 4U);
      for (const auto pc : mapIconSites)
        required.push_back(pc + 4U);
      required.push_back(kTopScreenCameraFovScalar + 4U);
      const auto entries = Oot3dNativeGame::Oot3dWholeAotEntryPoints();
      std::vector<uint32_t> sortedEntries(entries.begin(), entries.end());
      std::sort(sortedEntries.begin(), sortedEntries.end());
      std::string missing;
      for (const uint32_t pc : required) {
        if (!std::binary_search(sortedEntries.begin(), sortedEntries.end(), pc)) {
          char text[16];
          std::snprintf(text, sizeof(text), " 0x%08X", pc);
          missing += text;
        }
      }
      if (!missing.empty())
        throw std::runtime_error(
            "the title module does not start a block at the TopScreen sites" + missing +
            "; rebuild it with whole_aot_observed_sites.json");
    }
#endif
    wholeAotObservableExitBlocks.push_back(Oot3dNativeGame::kTopScreenInputUpdateBoundary);
    wholeAotObservableExitBlocks.push_back(kPauseRendererVisibilityNativeDraw);
    wholeAotObservableExitBlocks.insert(wholeAotObservableExitBlocks.end(),
                                       itemEntries.begin(), itemEntries.end());
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  topScreenSourcePortBlocks.begin(),
                                  topScreenSourcePortBlocks.end());
  }
  std::sort(wholeAotObservableExitBlocks.begin(),
            wholeAotObservableExitBlocks.end());
  wholeAotObservableExitBlocks.erase(
      std::unique(wholeAotObservableExitBlocks.begin(),
                  wholeAotObservableExitBlocks.end()),
      wholeAotObservableExitBlocks.end());
  std::sort(nativeCandidateEntries.begin(), nativeCandidateEntries.end());
  nativeCandidateEntries.erase(
      std::unique(nativeCandidateEntries.begin(), nativeCandidateEntries.end()),
      nativeCandidateEntries.end());
  Oot3dNativeGame::ConfigureOot3dNativeA32Candidates(
      nativeCandidateEntries);
  auto &process = processStorage.emplace(
      Oot3dNativeGame::Oot3dNativeA32Registry(), hostServices);
  widescreenProjection.TraceMemory = &process.Memory();
  widescreenProjection.ActorVisibility.HostMemory = &process.Memory();
  process.Memory().EnableWriteTraceFingerprint(
      widescreenProjection.TraceA32Blocks);
  auto &uiLifecycleBridge = uiLifecycleBridgeStorage.emplace(
      process.Memory());
  uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
  uiLifecycleBridge.SetTopScreenUiAtlasAvailable(
      topScreenTextureOverridePackPointer != nullptr &&
      topScreenTextureOverridePack.FindProfileTexture(Oot3dNativeGame::kTopScreen30UiAtlasSemantic) != nullptr);
  uiLifecycleBridge.SetVersionChoiceRevealRequired(
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen);
  widescreenProjection.UiLifecycleBridge = &uiLifecycleBridge;
  widescreenProjection.PicaCompositionDomain = &picaCompositionDomain;
  widescreenProjection.PicaCompositionTracker = &picaCompositionTracker;
  widescreenProjection.TopScreenUiProfile =
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
  widescreenProjection.TopScreenConfig = activeTopScreenConfig;
  const bool blockEntryForAll =
      launch.ProfileA32Blocks || widescreenProjection.TraceA32Blocks;
  if (guestProfiler.Active()) {
    // The profile sees every block and forwards to the policy only its hooks.
    guestProfiler.Chain(&ApplyNativeWidescreenProjectionPolicy,
                        &widescreenProjection, blockEntryHooks,
                        blockEntryForAll);
  }
  nativeBlockEntryPcs =
      blockEntryForAll || guestProfiler.Active()
          ? std::span<const uint32_t>{}
          : std::span<const uint32_t>{blockEntryHooks};
  // Sparse whole-AOT code (--sparse-block-entries) calls the callback only at
  // the PCs of whole_aot_hook_gates.json: refuse a hook outside them, and the
  // every-block callback of --trace-a32-blocks/--profile-a32-blocks, here
  // rather than let them silently never run (ExecuteOot3dCompiledFunction
  // refuses the same at run time).
  if (enableWholeAot) {
    if (const std::string error = Oot3dNativeGame::Oot3dWholeAotHookGatesError(
            Oot3dNativeGame::Oot3dLinkedWholeAotHookGates(), nativeBlockEntryPcs);
        !error.empty())
      throw std::runtime_error(error);
  }
  nativeCandidateDispatch = NativeCandidateDispatchState{
      guestProfiler.Active() ? &Oot3dNativeGame::GuestBlockProfiler::OnBlockEntry
                             : &ApplyNativeWidescreenProjectionPolicy,
      guestProfiler.Active() ? static_cast<void *>(&guestProfiler)
                             : static_cast<void *>(&widescreenProjection),
      nativeBlockEntryPcs,
      Oot3dNativeGame::BuildOot3dMassAotPcFilter(nativeBlockEntryPcs,
                                                 nativeBlockEntryPcs.empty()),
      Oot3dNativeGame::BuildOot3dMassAotPcFilter(
          wholeAotObservableExitBlocks, false),
      &process.Memory(),
      {static_cast<float>(frameRateContract.NativeUpdateRate),
       &frameRatePolicy.GameplayClock().Context()},
      enableCompiledFunctions && !launch.DisableTypedGameplay,
      enableMassAot,
      enableCompiledFunctions,
      enableWholeAot,
      enableManualCompiledFunctions,
      enableTrueAot,
      launch.ProfileA32Runtime,
  };
  nativeCandidateDispatch.TopScreenUiProfile =
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
  // After the rebuild above, which would discard an earlier assignment.
  nativeCandidateDispatch.ProductTopScreenBlocks = std::move(productTopScreenBlocks);
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    // Debugging: forces Game Over after N seconds of gameplay (the file's
    // text is N; empty = 10). Without the file/variable, nothing changes.
#if defined(__SWITCH__)
    std::ifstream debugGameOver("sdmc:/switch/oot3dre/config/debug_force_game_over");
    if (debugGameOver) {
      std::string text;
      std::getline(debugGameOver, text);
      nativeCandidateDispatch.TopScreenDebugGameOver.RequiredCalls =
          Oot3dNativeGame::ParseTopScreenDebugGameOverCalls(text);
    }
#else
    if (const char *text = std::getenv("TRIAEVUM_DEBUG_FORCE_GAME_OVER"))
      nativeCandidateDispatch.TopScreenDebugGameOver.RequiredCalls =
          Oot3dNativeGame::ParseTopScreenDebugGameOverCalls(text);
#endif
  }
  widescreenProjection.TopScreenInput =
      &nativeCandidateDispatch.TopScreenInput;
  widescreenProjection.TopScreenInputClock =
      &nativeCandidateDispatch.TopScreenInputClock;
  widescreenProjection.ObservedExitPc = &nativeCandidateDispatch.ObservedExitPc;
  nativeCandidateDispatch.TopScreenPausePageRedrawActive =
      &widescreenProjection.TopScreenPausePageRedrawActive;
  widescreenProjection.TopScreenItems = &nativeCandidateDispatch.TopScreenItems;
  // After the dispatch state is rebuilt: an earlier pointer would name the
  // free camera that assignment destroyed.
  widescreenProjection.DevFreeCamera = nativeCandidateDispatch.DevFreeCamera.get();
  nativeCandidateDispatch.TopScreenItems.TraceEnabled = launch.ExtendedDiagnostics;
  nativeCandidateDispatch.UiLifecycleBridge = &uiLifecycleBridge;
  nativeCandidateDispatch.PicaCompositionDomain = &picaCompositionDomain;
  nativeCandidateDispatch.TopScreenPauseProjection =
      &uiLifecycleBridge.TopScreenPauseProjection();
  nativeCandidateDispatch.TopScreenConfig = &activeTopScreenConfig;
  Oot3dNativeGame::ApplyTopScreenFreeCameraConfig(
      nativeCandidateDispatch.TopScreenCamera.Camera, activeTopScreenConfig);
  appliedTopScreenConfigRevision =
      topScreenConfigRuntime->Snapshot().Revision;
  widescreenProjection.TopScreenPauseTargetRoute =
      &nativeCandidateDispatch.TopScreenPauseRoute;
  widescreenProjection.TopScreenTouchCoordinateRoute =
      &nativeCandidateDispatch.TopScreenTouchCoordinateRoute;
  widescreenProjection.WholeAotObservableExitPcs =
      wholeAotObservableExitBlocks;
  process.SetTimingEnabled(launch.ProfileA32Runtime);
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  // Native candidates are experimental reconstruction aids.  The product
  // executes the audited whole-AOT graph and never substitutes source-native
  // or typed gameplay implementations for guest functions.
  if (!nativeCandidateEntries.empty()) {
    process.SetNativeBlockCallback(&ExecuteNativeCandidate,
                                   &nativeCandidateDispatch);
  }
#else
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    process.SetNativeBlockCallback(&ExecuteProductTopScreenHook,
                                   &nativeCandidateDispatch);
  }
#endif
  if (!Oot3dNativeGame::MountNativeA32ProcessImage(
          process, *manifest, std::filesystem::path{}, &error)) {
    throw std::runtime_error(error);
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("process_image_mounted");
#endif

  picaMemoryRegions = BuildPicaMemoryRegions(*manifest);
  const auto &picaMemoryView = picaMemoryViewStorage.emplace(
      process.Memory(), picaMemoryRegions);
  if (topScreenTextureOverridePackPointer != nullptr) {
    topScreenTextureOverrideRuntime.emplace(topScreenTextureOverridePack,
                                            picaMemoryView);
    widescreenProjection.TopScreenTextureOverrides =
        &*topScreenTextureOverrideRuntime;
  }
  auto &submissionQueue = submissionQueueStorage.emplace(
      picaMemoryView, true);
  // Generated title code links its own checked memory helpers. Only a module
  // advertising the observer contract can preserve GPU/CPU byte ownership.
#if defined(OOT3D_NATIVE_DIRECT_AOT_PLUGIN)
  copyWritesObservable =
      Oot3dNativeGame::Oot3dWholeAotPluginObservesMemoryWrites();
#else
  copyWritesObservable = true;
#endif
  if (copyWritesObservable) {
    process.Memory().SetWriteObserver(
        [&](uint32_t address, size_t size) {
          if (!picaMemoryView.SynchronizeGuestWrite(process.Memory(), address, size))
            throw std::runtime_error("cannot make a guest write visible through its physical aliases");
          submissionQueue.NotifyGuestWrite(address, size);
        });
    submissionQueue.BindGuestWriteObservation(
        [&](uint32_t address, size_t size) {
          return process.Memory().ObserveWrites(address, size);
        });
  }
  copyWriteObserverLifetime.emplace(process.Memory());
  if (hdFontsLoaded) submissionQueue.SetTextureSnapshotTransform(
      [&](const Oot3dNativeGame::Oot3dPicaTextureState& texture,
          Oot3dNativeGame::Oot3dPicaResourceSnapshot& resource) {
        hdFonts.Resolve(picaMemoryView, texture, resource);
      });
  if (topScreenTextureOverrideRuntime.has_value()) {
    submissionQueue.SetTexturePayloadTransform(
        [&](const Oot3dNativeGame::Oot3dPicaTextureState &texture,
            std::span<std::uint8_t> payload) {
          topScreenTextureOverrideRuntime->Transform(texture, payload);
        });
  }
  submissionQueue.SetRuntimeProfilingEnabled(launch.ProfileA32Runtime);
  picaFrontend.SetPacketSink(&submissionQueue);
  // Work off the game thread (config/offload_*, oot3d_native_offload.h).
  offload = Oot3dNativeGame::ReadNativeOffloadOptions();
  submissionQueue.SetLeanCapture(offload.Capture);
  picaFrontend.SetInPlaceDrawPackets(offload.Capture);
  // config/capture_delta: only what changed per draw on this thread.
  {
    const bool captureDelta = Oot3dNativeGame::ReadNativeCaptureDelta();
    submissionQueue.SetCaptureDelta(captureDelta);
    picaFrontend.SetCaptureDelta(captureDelta);
  }
  // config/list_walk_fast: the command-list walk's dispatch table and batches.
  picaFrontend.SetListWalkFast(Oot3dNativeGame::ReadNativeListWalkFast());
  // config/vertex_snapshot_fast: index/vertex snapshots viewed and certified
  // directly, compared page by page; certificates peek at clean cards
  // (before the second core starts).
  {
    const bool vertexSnapshotFast = Oot3dNativeGame::ReadNativeVertexSnapshotFast();
    submissionQueue.SetVertexSnapshotFast(vertexSnapshotFast);
    Oot3dNativeGame::SetRangeWriteGenerationCardPeek(vertexSnapshotFast);
  }
  // config/texture_snapshot_fast: textures viewed and certified directly,
  // found through a front table, compared page by page; the next packet
  // prefetched.
  submissionQueue.SetTextureSnapshotFast(Oot3dNativeGame::ReadNativeTextureSnapshotFast());
  picaPresentationScheduler.SetMoveFreeFinish(offload.Plan);
#if defined(__SWITCH__)
  if (offload.Perf) {
    perfWriter.emplace("sdmc:/switch/oot3dre/logs/perf.txt");
    hitchWriter.emplace("sdmc:/switch/oot3dre/logs/hitches.txt", 64U);
    (void)hitchWriter->Push("session_start\n");
  }
  // config/async_status_writes: boot-status.txt's heartbeat through a
  // service thread (oot3d_native_status_file.h).
  asyncStatusWrites = Oot3dNativeGame::ReadNativeAsyncStatusWrites();
#endif

  hostArgs = launch.Host;
  hostArgs.AudioSampleRate = Oot3dNativeGame::NativeA32DspHle::NativeSampleRate;
  hostArgs.AudioSampleLength =
      Oot3dNativeGame::NativeA32DspHle::SamplesPerFrame;
#if defined(__SWITCH__)
  hostArgs.AudioSampleRate = ReadSwitchAudioSampleRate();
  hostArgs.AudioSampleLength = ReadSwitchAudioDeviceFrames();
#endif
  hostArgs.AudioDesiredBuffered = std::max<int32_t>(
      hostArgs.AudioDesiredBuffered,
      static_cast<int32_t>((static_cast<uint64_t>(hostArgs.AudioSampleRate) *
                                kMinimumHostAudioPrebufferMilliseconds +
                            999U) /
                           1000U));
#if defined(__SWITCH__)
  // ~125 ms of queued audio: the console crackled whenever a frame ran long.
  hostArgs.AudioDesiredBuffered = std::max<int32_t>(hostArgs.AudioDesiredBuffered, 4096);
  {
    // A/B switch for the hand-written guest functions (no rebuild needed).
    std::error_code overridesError;
    if (std::filesystem::exists("sdmc:/switch/oot3dre/config/no_guest_overrides", overridesError)) {
      Oot3dNativeGame::SetOot3dGuestOverridesEnabled(false);
    }
    // Per-replacement A/B: hex entries and/or "mirror" (the Profile18
    // round's replacements); logs/perf.txt "natives:" shows the effect.
    std::ifstream disableList("sdmc:/switch/oot3dre/config/guest_overrides_disable");
    if (disableList) {
      const std::string list((std::istreambuf_iterator<char>(disableList)),
                             std::istreambuf_iterator<char>());
      Oot3dNativeGame::DisableOot3dGuestOverridesFromList(list);
    }
  }
#endif
  // The user's profiles beside the configuration; the release's defaults in
  // the runtime resources.
  sceneEnvironment = std::make_shared<Oot3dNativeGame::SceneEnvironmentRuntime>(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "environments",
      [](const Fast::Renderer3ds::PicaEnvironmentLighting& environment) {
        Fast::Oot3d::GraphicsSettingsRuntime::Instance().PublishSceneEnvironment(environment);
      },
      launch.Host.ResourceRoot.empty() ? std::filesystem::path{}
                                       : std::filesystem::path(launch.Host.ResourceRoot) / "environments");
  // F1 order: the scene lights join Lighting > Lights & Shadows; then
  // Controls, HUD and Game (Save states, Language) after the graphics tabs.
  applicationSettingsTabs.push_back(Oot3dNativeGame::CreateSceneEnvironmentPanel(sceneEnvironment));
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  if (const char *tour = std::getenv("TRIAEVUM_DEV_FREECAM_TOUR"); tour != nullptr && *tour != '\0') {
    std::string text = tour;
    size_t start = 0;
    while (start < text.size()) {
      const size_t end = std::min(text.find(';', start), text.size());
      DevFreeCameraTourPose pose;
      std::string entry = text.substr(start, end - start);
      if (!entry.empty() && entry.front() == '@') {
        pose.Absolute = true;
        entry.erase(0, 1);
      }
      if (std::sscanf(entry.c_str(), "%f,%f,%f,%f,%f", &pose.Forward, &pose.Right, &pose.Up, &pose.Yaw,
                      &pose.Pitch) == 5)
        devTourPoses.push_back(pose);
      start = end + 1;
    }
    const char *out = std::getenv("TRIAEVUM_DEV_FREECAM_TOUR_OUT");
    devTourDirectory = out != nullptr ? std::filesystem::path(out) : std::filesystem::current_path();
    std::filesystem::create_directories(devTourDirectory);
    // A boot warp point is noticed when it is requested (UpdateDevFreeCameraTour).
    devTourAfterWarp = std::getenv("TRIAEVUM_DEV_WARP") != nullptr;
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR poses=%zu out=%s\n", devTourPoses.size(),
                 devTourDirectory.string().c_str());
  }
  if (const char *lab = std::getenv("TRIAEVUM_DEV_LAB"); lab != nullptr && *lab != '\0') {
    devLab = std::make_unique<Oot3dNativeGame::DevLabChannel>(lab);
    std::fprintf(stderr, "TRIAEVUM_DEV_LAB %s\n", lab);
  }
#if defined(__SWITCH__)
  // No environment on the console/emulator: config/dev_lab turns the channel
  // on in sdmc:/switch/oot3dre/lab (warps in one session, e.g. a shader sweep).
  else if (std::filesystem::exists("sdmc:/switch/oot3dre/config/dev_lab")) {
    devLab = std::make_unique<Oot3dNativeGame::DevLabChannel>("sdmc:/switch/oot3dre/lab");
  }
#endif
  if (const char *emitters = std::getenv("TRIAEVUM_DEV_EMITTERS"); emitters != nullptr && *emitters != '\0') {
    devEmitterLog = std::make_unique<Oot3dNativeGame::NativeEmitterInventoryLog>(emitters);
    std::fprintf(stderr, "TRIAEVUM_DEV_EMITTERS %s\n", emitters);
  }
  // Agents edit scene profiles on disk while the game runs.
  if (const char *reload = std::getenv("TRIAEVUM_ENVIRONMENT_HOT_RELOAD"); reload != nullptr && *reload != '0') {
    sceneEnvironment->SetHotReload(true);
    std::fprintf(stderr, "TRIAEVUM_ENVIRONMENT_HOT_RELOAD enabled\n");
  }
#endif
  applicationSettingsTabs.push_back(
      Oot3dNativeGame::CreateNativeControlsSettingsPanel(
          controlConfigRuntime,
          launch.UiProfile ==
                  Oot3dNativeGame::Oot3dUiProfile::TopScreen
              ? topScreenConfigRuntime
              : nullptr));
  if (launch.UiProfile ==
      Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    applicationSettingsTabs.push_back(
        Oot3dNativeGame::CreateTopScreenSettingsPanel(
            topScreenConfigRuntime));
  }
  applicationSettingsTabs.push_back(
      Oot3dNativeGame::CreateSavestatePanel(savestateLibrary));
  applicationSettingsTabs.push_back(Oot3dNativeGame::CreateGameLanguagePanel(languageSettings));
#if defined(__SWITCH__)
  applicationSettingsTabs.push_back(
      Oot3dNativeGame::CreateGameplaySettingsPanel("sdmc:/switch/oot3dre/config"));
#else
  applicationSettingsTabs.push_back(Oot3dNativeGame::CreateGameplaySettingsPanel(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "config"));
#endif
#if defined(__SWITCH__)
  applicationSettingsTabs.push_back(
      Oot3dNativeGame::CreateVideoSettingsPanel("sdmc:/switch/oot3dre/config"));
#endif
#if defined(__SWITCH__)
  // OTA updates from GitHub Releases, and the version at the top of the menu.
  applicationSettingsTabs.push_back(Oot3dNativeGame::CreateUpdatePanel("sdmc:/switch/oot3dre"));
  Fast::Oot3d::DebugMenuTitleText().store(Oot3dNativeGame::BuildVersion(),
                                          std::memory_order_relaxed);
#endif
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  devWarp = std::make_shared<Oot3dNativeGame::DevWarpRuntime>(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "developer-tools.json");
#if defined(__SWITCH__)
  {
    // The input bench (oot3d_input_bench.h): replay wins over record.
    using Bench = Oot3dNativeGame::InputBench;
    std::error_code ignored;
    const std::filesystem::path root = "sdmc:/switch/oot3dre";
    if (std::filesystem::exists(root / "config" / "bench_replay", ignored)) {
      try {
        inputBench = Bench(Bench::Mode::Replay,
                           Oot3dNativeGame::NativeA32InputTimeline::LoadFile(root / "bench" / "input.json"));
      } catch (const std::exception &exception) {
        std::ofstream(root / "logs" / "bench_status.txt", std::ios::trunc)
            << "failed " << exception.what() << '\n';
      }
    } else if (std::filesystem::exists(root / "config" / "bench_record", ignored)) {
      inputBench = Bench(Bench::Mode::Record);
    }
  }
#endif
  applicationSettingsTabs.push_back(
      Oot3dNativeGame::CreateDevToolsPanel(devWarp, nativeCandidateDispatch.DevFreeCamera,
                                           languageSettings->BootLanguage()));
  // Scripted check of the Better list without the menu:
  // TRIAEVUM_DEV_WARP=<scene>:<entrance>:<child|adult>:<day|night> (list
  // indices), requested when gameplay first starts.
  // TRIAEVUM_DEV_FREECAM=<x>,<y>,<z>,<yaw>,<pitch>: the free camera at a fixed
  // pose from the start (degrees; yaw 0 faces +Z, pitch positive up).
  if (const char *pose = std::getenv("TRIAEVUM_DEV_FREECAM")) {
    std::array<float, 3> eye{};
    float yaw = 0.0F, pitch = 0.0F;
    if (std::sscanf(pose, "%f,%f,%f,%f,%f", &eye[0], &eye[1], &eye[2], &yaw, &pitch) != 5)
      throw std::runtime_error("TRIAEVUM_DEV_FREECAM must be <x>,<y>,<z>,<yaw>,<pitch>");
    nativeCandidateDispatch.DevFreeCamera->SetPose(eye, yaw, pitch);
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM pose=%.1f,%.1f,%.1f yaw=%.1f pitch=%.1f\n", eye[0], eye[1], eye[2], yaw, pitch);
  }
  if (const char *script = std::getenv("TRIAEVUM_DEV_WARP")) {
    int scene = -1, entrance = -1;
    char age[8]{}, time[8]{};
    const auto scenes = Oot3dNativeGame::DevWarpBetterScenes();
    if (std::sscanf(script, "%d:%d:%7[a-z]:%7[a-z]", &scene, &entrance, age, time) != 4 || scene < 0 ||
        scene >= static_cast<int>(scenes.size()) || entrance < 0 ||
        entrance >= static_cast<int>(scenes[static_cast<size_t>(scene)].Count)) {
      throw std::runtime_error("TRIAEVUM_DEV_WARP must be <scene>:<entrance>:<child|adult>:<day|night>");
    }
    devWarp->WarpTo(Oot3dNativeGame::DevWarpBetterEntrances()[scenes[static_cast<size_t>(scene)].First +
                                                               static_cast<size_t>(entrance)],
                    std::string_view(age) == "adult" ? Oot3dNativeGame::DevWarpAge::Adult
                                                     : Oot3dNativeGame::DevWarpAge::Child,
                    std::string_view(time) == "night");
  }
#endif
  Fast::Oot3d::InstallGraphicsSettingsPanelTabs(
      std::move(applicationSettingsTabs));
  InitContextForDemo(hostArgs);
#if defined(__SWITCH__)
  // The game log exists from here: the lines kept before it (kernel_fast:,
  // the Video panel's TRIAEVUM_PERF_*), then every later WriteGameLog line,
  // queued for the logger's thread at warning level (the release logger
  // drops info).
  Oot3dNativeGame::GameLog().Attach([](const std::string &line) { SPDLOG_WARN("{}", line); });
  WriteSwitchBootStage("host_context_initialized");
  // What the start-up constructor did with the code (oot3d_switch_code_pages.cpp):
  // remapped (size, time), off or why it stayed on 4 KB pages. Warning level:
  // the release logger drops info.
  SPDLOG_WARN("{}", Oot3dNativeGame::FormatCodePagesStatus(
                        Oot3dNativeGame::CurrentCodePagesStatus()));
  // Instrumented (PGO generate) builds: where the counts go.
  if (const std::string pgo = Oot3dNativeGame::Oot3dWholeAotPgoDescribe(); !pgo.empty())
    SPDLOG_WARN("{}", pgo);
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE) && defined(_WIN32)
  std::string crashDiagnosticsError;
  if (!Oot3dNativeGame::InstallWholeAotCrashDiagnostics(
          saveDataDirectory / "crashes", &crashDiagnosticsError)) {
    throw std::runtime_error("whole-AOT crash diagnostics failed: " +
                             crashDiagnosticsError);
  }
#endif
  windowPointer = &GetActiveFast3dWindowForDemo();
  auto &window = *windowPointer;
  // Interactive presentation is paced by NativeRealtimeRefreshPacer, not by a
  // second software limiter in SDL. Bounded probes retain their existing policy.
  if (hostArgs.ThroughputBenchmark || hostArgs.FrameLimit == 0U) {
    window.SetTargetFps(0);
  }
  apiPointer = &GetActiveRenderingApiForDemo(window);
  auto &api = *apiPointer;
  programPreparation =
      dynamic_cast<Fast::Renderer3ds::PicaProgramPreparationBackend *>(&api);
  alwaysAvailablePrograms =
      programPreparation != nullptr &&
      programPreparation->UsesAlwaysAvailablePrograms();
  if (programPreparation != nullptr)
    programPreparation->RegisterTitlePassPrograms(
        Oot3dNativeGame::NativePicaTitlePassPrograms());
  if (programPreparation != nullptr &&
      (launch.PicaParametricTev || alwaysAvailablePrograms)) {
    programPreparation->PrepareNativePicaPrograms(
        Oot3dNativeGame::NativePicaVertexProgramFamily());
  }
  // Initial preparation: from here until the renderer's startup work is ready
  // and the first frames run within budget, the window shows its progress.
  auto &preparationScreen = preparationScreenStorage.emplace(
      std::chrono::steady_clock::now());
  api.SetNativePicaGeometryCacheEnabled(
      !launch.DisableOpenGlPicaGeometryCache);
  titleRenderBackend =
      dynamic_cast<Fast::Oot3d::TitleRenderBackend *>(&api);
  gui =
      Ship::Context::GetRawInstance()->GetWindow()->GetGui();
  if (gui == nullptr) {
    throw std::runtime_error("native PICA host has no renderer GUI");
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("opengl_renderer_initialized");
#endif
  auto &nativeUiTextureProvider = nativeUiTextureProviderStorage.emplace(
      picaMemoryView, topScreenTextureOverridePackPointer);
  // TopScreen's HD UI atlas (on the Switch, config/topscreen_ui_atlas_hd.png
  // from the mod's 4096x2048 redraw reduced to 3x): it replaces the exact
  // native ui_atlas bytes, and a texture pack image of that atlas wins. A
  // missing or unusable file keeps the native atlas.
  // Ported from upstream TriAevum (coccofresco/TriAevum commit 14edc1eb,
  // GPL-3.0-or-later).
  std::shared_ptr<const Oot3dNativeGame::Oot3dUiHdAtlas> uiHdAtlas;
  if (!launch.TopScreenUiAtlasHdPath.empty() && topScreenTextureOverridePackPointer != nullptr) {
    const auto *native = topScreenTextureOverridePack.FindProfileTexture("oot3d/topscreen/3.0/ui_atlas");
    int width = 0, height = 0, channels = 0;
    stbi_uc *decoded = native == nullptr ? nullptr
                                         : stbi_load(launch.TopScreenUiAtlasHdPath.string().c_str(),
                                                     &width, &height, &channels, 4);
    if (native == nullptr) {
      std::fprintf(stderr, "topscreen: HD UI atlas unused: the texture pack has no 3.0 ui_atlas\n");
    } else if (decoded == nullptr) {
      std::fprintf(stderr, "topscreen: HD UI atlas unused: cannot decode %s (%s)\n",
                   launch.TopScreenUiAtlasHdPath.string().c_str(),
                   stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown");
    } else if (!Oot3dNativeGame::IsUiHdAtlasSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                                 native->Width, native->Height)) {
      std::fprintf(stderr, "topscreen: HD UI atlas unused: %dx%d is not a whole multiple of %ux%u\n", width,
                   height, static_cast<unsigned>(native->Width), static_cast<unsigned>(native->Height));
    } else {
      auto atlas = std::make_shared<Oot3dNativeGame::Oot3dUiHdAtlas>();
      atlas->rgba8 = std::make_shared<const std::vector<uint8_t>>(
          decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4U);
      atlas->width = static_cast<uint32_t>(width);
      atlas->height = static_cast<uint32_t>(height);
      atlas->native_payload = native->EncodedPayload;
      uiHdAtlas = std::move(atlas);
      std::fprintf(stderr, "topscreen: HD UI atlas %dx%d (%zu KiB)\n", width, height,
                   uiHdAtlas->rgba8->size() / 1024U);
    }
    if (decoded != nullptr) stbi_image_free(decoded);
  }
  // The mod's HD images of the game's own HUD textures (on the Switch,
  // config/topscreen_hd/, 3x): Azahar names them by its legacy hash of the
  // native texture, so each is matched the way a legacy-hash pack would be.
  struct UiHdImage {
    uint16_t NativeWidth = 0, NativeHeight = 0;
    uint8_t NativeFormat = 0;
    uint64_t LegacyHash = 0;
    Oot3dNativeGame::Oot3dUiHdAtlas Image;
  };
  auto uiHdImages = std::make_shared<std::vector<UiHdImage>>();
  if (!launch.TopScreenHdTexturesPath.empty() && topScreenTextureOverridePackPointer != nullptr) {
    std::error_code listError;
    for (const auto &entry : std::filesystem::directory_iterator(launch.TopScreenHdTexturesPath, listError)) {
      const std::string name = entry.path().filename().string();
      unsigned width = 0, height = 0, format = 0;
      unsigned long long hash = 0;
      int consumed = 0;
      if (std::sscanf(name.c_str(), "tex1_%ux%u_%16llX_%u_mip0.png%n", &width, &height, &hash, &format,
                      &consumed) != 4 ||
          static_cast<size_t>(consumed) != name.size() || width == 0U || height == 0U || width > 1024U ||
          height > 1024U || format > 13U) {
        continue;
      }
      int imageWidth = 0, imageHeight = 0, channels = 0;
      stbi_uc *decoded = stbi_load(entry.path().string().c_str(), &imageWidth, &imageHeight, &channels, 4);
      if (decoded == nullptr) {
        std::fprintf(stderr, "topscreen: HD image unused: cannot decode %s (%s)\n", name.c_str(),
                     stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown");
        continue;
      }
      if (!Oot3dNativeGame::IsUiHdAtlasSize(static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight),
                                            width, height)) {
        std::fprintf(stderr, "topscreen: HD image unused: %s is %dx%d, not a whole multiple of %ux%u\n",
                     name.c_str(), imageWidth, imageHeight, width, height);
      } else {
        UiHdImage image;
        image.NativeWidth = static_cast<uint16_t>(width);
        image.NativeHeight = static_cast<uint16_t>(height);
        image.NativeFormat = static_cast<uint8_t>(format);
        image.LegacyHash = hash;
        image.Image.rgba8 = std::make_shared<const std::vector<uint8_t>>(
            decoded, decoded + static_cast<size_t>(imageWidth) * static_cast<size_t>(imageHeight) * 4U);
        image.Image.width = static_cast<uint32_t>(imageWidth);
        image.Image.height = static_cast<uint32_t>(imageHeight);
        uiHdImages->push_back(std::move(image));
        std::fprintf(stderr, "topscreen: HD image %s %dx%d\n", name.c_str(), imageWidth, imageHeight);
      }
      stbi_image_free(decoded);
    }
  }
  // HUD textures resolve the player's texture pack exactly as native PICA
  // uploads do (same identity, same async decode) while HD textures are on,
  // reduced to 3x native (kUiTextureMaxScale) whatever "HD texture size"
  // says. The pack's generation changes when HD textures are turned on or
  // off, so the UI renderer resolves its textures again at once. A request
  // ahead of the first draw (N64UiFast3dRenderer::RequestAhead) only queues.
  // The legacy hash (Henriko's pack; the HD images) is computed once per
  // texture and returned for the provider's identity cache: a texture
  // resolved again (a pack change, a poll) comes with both hashes and is
  // neither decoded nor hashed again here.
  nativeUiTextureProvider.SetCustomTextureSource({
      [uiHdAtlas, uiHdImages](const Oot3dNativeGame::Oot3dNativeUiCustomTextureRequest &request) {
        Oot3d::Renderer::AzaharTextureRequest identity{
            .Width = request.width,
            .Height = request.height,
            .NativeFormat = request.native_pica_format,
            .MipLevel = 0U,
            .NativeBytes = request.encoded_base_level,
            .NativeRgba8 = request.rgba8,
            .UserInterface = true,
            .Ahead = request.ahead,
            .KnownModernHash = request.modern_hash,
            .KnownLegacyHash = request.legacy_hash,
        };
        if (!identity.KnownLegacyHash && !request.ahead) {
          std::string error;
          const uint64_t legacy = Oot3d::Renderer::ComputeAzaharTextureHash(identity, false, &error);
          if (error.empty()) identity.KnownLegacyHash = legacy;
        }
        const auto pack = Oot3d::Renderer::AzaharTexturePackRuntime::Instance().ResolveOrQueue(identity);
        const Oot3dNativeGame::Oot3dUiHdAtlas *hdImage = nullptr;
        for (const auto &image : *uiHdImages) {
          if (image.NativeWidth == request.width && image.NativeHeight == request.height &&
              image.NativeFormat == request.native_pica_format && identity.KnownLegacyHash == image.LegacyHash) {
            hdImage = &image.Image;
            break;
          }
        }
        auto custom = Oot3dNativeGame::ChooseUiCustomTexture(pack, request, uiHdAtlas.get(), hdImage);
        custom.legacy_hash = identity.KnownLegacyHash;
        return custom;
      },
      // Not bumped by a change of only "HD texture size" (HUD images are 3x
      // whatever it says): the HUD does nothing then.
      [] { return Oot3d::Renderer::AzaharTexturePackRuntime::Instance().UiRevision(); },
  });
  if (hdFontsLoaded) nativeUiTextureProvider.SetGeneratedTextureTransform(
      [&](const Oot3dNativeGame::Oot3dPicaTextureState& texture,
          Oot3dNativeGame::Oot3dPicaResourceSnapshot& resource) {
        hdFonts.Resolve(picaMemoryView, texture, resource);
      });
  auto &ocarinaText = ocarinaTextStorage.emplace(process.Memory(), nativeUiTextureProvider);
  nativeCandidateDispatch.OcarinaText = &ocarinaText;
  widescreenProjection.OcarinaText = &ocarinaText;
  auto &pauseTransaction = pauseTransactionStorage.emplace(process.Memory());
  nativeCandidateDispatch.PauseTransaction = &pauseTransaction;
  widescreenProjection.PauseTransaction = &pauseTransaction;
  auto &systemPage = systemPageStorage.emplace(process.Memory());
  nativeCandidateDispatch.SystemPage = &systemPage;
  widescreenProjection.SystemPage = &systemPage;
  auto &itemDescription = itemDescriptionStorage.emplace(process.Memory());
  nativeCandidateDispatch.ItemDescription = &itemDescription;
  widescreenProjection.ItemDescription = &itemDescription;
  auto &wideItems = wideItemsStorage.emplace(process.Memory());
  nativeCandidateDispatch.WideItems = &wideItems;
  auto &uiRenderBackend = uiRenderBackendStorage.emplace(api);
  auto &n64UiRenderer = n64UiRendererStorage.emplace(uiRenderBackend,
                                               &ocarinaText);
  window.SetTextureFilter(Fast::FILTER_LINEAR);
  inputTimeline = Oot3dNativeGame::NativeA32InputTimeline::LoadFile(
      launch.Host.InputTimelinePath);
  graphicsSettingsSchedule =
      Oot3dNativeGame::GraphicsSettingsSchedule::LoadFile(
          launch.Host.GraphicsSettingsSchedulePath);
#if defined(__SWITCH__)
  WriteSwitchBootStage("ui_runtime_initialized");
#endif

  UpdateScenePresentation();
  if (guestProfiler.Active()) {
    process.SetBlockEntryCallback(
        &Oot3dNativeGame::GuestBlockProfiler::OnBlockEntry, &guestProfiler,
        std::vector<uint32_t>{});
  } else {
    process.SetBlockEntryCallback(
        &ApplyNativeWidescreenProjectionPolicy, &widescreenProjection,
        launch.ProfileA32Blocks || widescreenProjection.TraceA32Blocks
            ? std::vector<uint32_t>{}
            : blockEntryHooks);
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("guest_callbacks_initialized");
#endif

  shaderSourceCache.TevMode = launch.PicaParametricTev
      ? Oot3dNativeGame::Oot3dPicaTevMode::Parametric
      : Oot3dNativeGame::Oot3dPicaTevMode::Specialized;
  shaderSourceCache.UberFragments = alwaysAvailablePrograms;
  auto &picaSemanticTrace = picaSemanticTraceStorage.emplace(
      launch.PicaSemanticTracePath);
  picaSemanticTrace.BeginSession(
      launch.Host.Renderer,
      Oot3dNativeGame::GameplayTimingModeName(launch.GameplayTiming),
      Oot3dNativeGame::Oot3dUiProfileName(launch.UiProfile));
  auto &dspHle = dspHleStorage.emplace(
      Oot3dNativeGame::BuildNativeA32DspPhysicalRegions(*manifest));
  context = Ship::Context::GetRawInstance();
  audio = context != nullptr ? context->GetAudio() : nullptr;
  audioPlayer = audio != nullptr ? audio->GetAudioPlayer() : nullptr;
  if (audioPlayer != nullptr && audioPlayer->IsInitialized() &&
      static_cast<uint32_t>(audioPlayer->GetSampleRate()) !=
          Oot3dNativeGame::NativeA32DspHle::NativeSampleRate) {
    hostAudioResampler.emplace(
        Oot3dNativeGame::NativeA32DspHle::NativeSampleRate,
        static_cast<uint32_t>(audioPlayer->GetSampleRate()));
  }
  {
    const bool audioThread = ReadHostAudioSwitch("audio_thread", "TRIAEVUM_AUDIO_THREAD");
    const bool audioStretch = ReadHostAudioSwitch("audio_stretch", "TRIAEVUM_AUDIO_STRETCH");
    if (audioPlayer != nullptr && audioPlayer->IsInitialized() && !launch.DisableAudio &&
        (audioThread || audioStretch)) {
      Oot3dNativeGame::HostAudioStream::Options options;
      options.InputRate = Oot3dNativeGame::NativeA32DspHle::NativeSampleRate;
      options.OutputRate = static_cast<uint32_t>(audioPlayer->GetSampleRate());
      options.TargetFrames = audioPlayer->GetDesiredBuffered();
      options.DeviceFrames = audioPlayer->GetSampleLength();
      options.Thread = audioThread;
      options.Stretch = audioStretch;
      std::shared_ptr<Ship::AudioPlayer> player = audioPlayer;
      Oot3dNativeGame::HostAudioStream::Sink sink;
      sink.Buffered = [player]() { return player->Buffered(); };
      sink.Play = [player](const int16_t *samples, size_t count) {
        player->Play(reinterpret_cast<const uint8_t *>(samples), count * sizeof(int16_t));
      };
      sink.Flush = [player]() { player->Flush(); };
      hostAudioStream = std::make_unique<Oot3dNativeGame::HostAudioStream>(options, std::move(sink));
    }
    std::fprintf(stderr, "audio: host stream %s (thread=%d stretch=%d)\n",
                 hostAudioStream != nullptr ? "on" : "off", audioThread ? 1 : 0,
                 audioStretch ? 1 : 0);
  }
#if defined(__SWITCH__)
  std::fprintf(stderr, "audio: DSP %u Hz -> device %d Hz (%s), %d frames per device buffer\n",
               Oot3dNativeGame::NativeA32DspHle::NativeSampleRate,
               audioPlayer != nullptr ? audioPlayer->GetSampleRate() : 0,
               hostAudioResampler.has_value() ? "game resampler" : "no conversion",
               audioPlayer != nullptr ? audioPlayer->GetSampleLength() : 0);
  {
    std::error_code dumpError;
    auto *sdlPlayer = dynamic_cast<Ship::SDLAudioPlayer *>(audioPlayer.get());
    if (sdlPlayer != nullptr && audioPlayer->IsInitialized() &&
        std::filesystem::exists(kSwitchAudioDumpFlagPath, dumpError)) {
      sdlPlayer->StartPcmCapture(kSwitchAudioDumpPath, kSwitchAudioDumpSeconds);
    }
  }
#endif
#if defined(__SWITCH__)
  WriteSwitchBootStage("audio_runtime_initialized");
#endif
  phaseStart = std::chrono::steady_clock::now();
  auto &presentationScheduler = presentationSchedulerStorage.emplace(
      frameRateContract.SimulationRateHz,
      frameRateContract.Mode ==
              Oot3dNativeGame::GameplayTimingMode::Enhanced60
          ? Oot3dNativeGame::NativeSimulationCatchUpPolicy::
                PreserveBoundedDebt
          : Oot3dNativeGame::NativeSimulationCatchUpPolicy::DropExcess);
  if (const char* watchRange = std::getenv("TRIAEVUM_WATCH_GUEST_RANGE")) {
    unsigned long long start = 0, size = 0;
    const int parsed = std::sscanf(watchRange, "%llx:%llx", &start, &size);
    std::fprintf(stderr,
                 "TRIAEVUM_WATCH_GUEST_RANGE raw='%s' parsed=%d start=0x%llX size=0x%llX\n",
                 watchRange, parsed, start, size);
    if (parsed == 2 && size != 0) {
      rawCopyWatchRanges.emplace_back(static_cast<uint32_t>(start),
                                      static_cast<uint32_t>(start + size));
      process.Memory().SetRangeWatch(rawCopyWatchRanges);
      watchReportRequested = true;
      std::fprintf(stderr, "TRIAEVUM_WATCH_GUEST_RANGE active=%d ranges=%zu\n",
                   process.Memory().RangeWatchActive() ? 1 : 0,
                   process.Memory().WatchedRangeCount());
    }
  }
  // fastmem window (config/fastmem_window; SWITCH_PORT.md "fastmem window"):
  // where the image's regions live once the image is mounted, or why they
  // stay on the heap, and whether the guest's typed accesses take the window
  // (access=window) or the slow path for every access (access=slow).
  // Warning level: the release logger drops info.
  {
    const std::string fastmemWindow =
        Oot3dNativeGame::GuestWindow::FormatStatus(Oot3dNativeGame::GuestWindow::CurrentStatus()) +
        (process.Memory().InGuestWindow() ? " access=window" : " access=slow");
#if defined(__SWITCH__)
    SPDLOG_WARN("{}", fastmemWindow);
#else
    std::fprintf(stderr, "%s\n", fastmemWindow.c_str());
#endif
  }
  // config/second_core (SWITCH_PORT.md "Second guest core"; desktop:
  // TRIAEVUM_SECOND_CORE=1): guest threads that ask for processor 1 run on
  // their own host thread on core 1. Absent = every guest thread on the game
  // thread, as before. Started before the first guest code and before an
  // initial savestate load, which then places its threads.
  {
    bool secondCore = false;
#if defined(__SWITCH__)
    std::error_code secondCoreError;
    secondCore = std::filesystem::exists(
        "sdmc:/switch/oot3dre/config/second_core", secondCoreError);
#else
    if (const char *value = std::getenv("TRIAEVUM_SECOND_CORE")) {
      secondCore = std::string_view(value) != "0";
    }
#endif
    std::fprintf(stderr, "TRIAEVUM_SECOND_CORE enabled=%d\n", secondCore ? 1 : 0);
    if (secondCore) StartSecondCore();
  }
  if (!launch.LoadStatePath.empty()) {
#if defined(__SWITCH__)
    WriteSwitchBootStage("initial_savestate_loading");
#endif
    Oot3dNativeGame::NativeA32SavestateRuntimeState restoredRuntime;
    Oot3dNativeGame::NativeA32SavestateIoResult ioResult;
    if (!Oot3dNativeGame::LoadNativeA32State(
            launch.LoadStatePath, savestateCompatibility, restoredRuntime,
            process, hostServices, picaFrontend, submissionQueue, dspHle,
            &ioResult, &error)) {
      throw std::runtime_error("native savestate load failed: " + error);
    }
    ApplyLoadedSavestateSettings(restoredRuntime, launch.LoadStateWithSettings);
    if (restoredRuntime.HostSettingsAvailable)
      initialStateHostSettings = restoredRuntime.HostSettingsJson;
    Oot3dNativeGame::ResetOot3dTypedGameplayTransientState();
    hdFonts.Reset();
    const auto restoredGravity = hostServices.HidRuntimeProfile().LastAccelerometer;
    nativeControlPollingState.VirtualMotion.RestoreGravity(
        {static_cast<float>(restoredGravity[0]), static_cast<float>(restoredGravity[1]),
         static_cast<float>(restoredGravity[2])});
    nativeControlPollingState.PendingMouseDeltaX = 0;
    nativeControlPollingState.PendingMouseDeltaY = 0;
    nativeControlPollingState.PendingInputSeconds = 0.0;
    picaCompositionTracker.Reset();
    frameCount = restoredRuntime.FrameCount;
    refreshTickRemainder = restoredRuntime.RefreshTickRemainder;
    nextVblankTick = restoredRuntime.NextVblankTick;
    processResult.Kind = static_cast<Oot3dNativeGame::NativeA32ProcessRunKind>(
        restoredRuntime.ProcessRunKind);
    RestoreSavestateTiming(restoredRuntime);
    ApplySelectedUiProfile();
    if (!api.ResetPicaState(&error)) {
      throw std::runtime_error(
          "native PICA reset before initial savestate restore failed: " +
          error);
    }
    if (titleRenderBackend != nullptr &&
        !titleRenderBackend->ResetTitleState(&error)) {
      throw std::runtime_error(
          "OOT3D renderer reset before initial savestate restore failed: " +
          error);
    }
    RestoreSavestatePicaTextureCache(restoredRuntime);
    RestoreSavestatePicaColorTargets(restoredRuntime);
    RestoreSavestatePicaPresentationState(restoredRuntime);
    RestoreSavestatePicaVisualReplayState(restoredRuntime);
    RestoreSavestateTopScreenTemporalState(restoredRuntime);
    ++savestateStats.LoadCount;
    savestateStats.LoadBytes += ioResult.FileBytes;
    savestateStats.LoadSeconds += SecondsSince(phaseStart);
    savestateStats.ReadSeconds += ioResult.IoSeconds;
    savestateStats.DecodeSeconds += ioResult.DecodeSeconds;
    savestateStats.RestoreSeconds += ioResult.RestoreSeconds;
    std::cout << "oot3d_native_game: loaded initial state at frame "
              << frameCount << " from " << launch.LoadStatePath.string()
              << '\n';
  } else {
    ApplySelectedUiProfile();
#if defined(__SWITCH__)
    WriteSwitchBootStage("initial_guest_dispatch");
#endif
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
#if defined(__SWITCH__)
    WriteSwitchBootStage("initial_guest_wait");
#endif
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  timing = WindowDemoTimingState{};  // its default members read the clock here
  presentationPacingPolicy =
      Fast::Oot3d::ResolvePresentationPacingPolicy(EffectiveFrameRateMode());
  frameCompositionPolicy =
      ResolveFrameCompositionPolicy(presentationPacingPolicy);
  visualSampleCadence.Configure(
      frameCompositionPolicy.FixedMultiplier()
          ? frameCompositionPolicy.FixedSampleMultiplier
          : 0U);
  auto &realtimePacer = realtimePacerStorage.emplace(
      hostArgs.FrameLimit == 0U && !hostArgs.ThroughputBenchmark,
      presentationPacingPolicy.TargetRateHz);
  realtimePacer.Configure(presentationPacingPolicy.Enabled &&
                              !hostArgs.ThroughputBenchmark,
                          presentationPacingPolicy.TargetRateHz);
#if defined(__SWITCH__)
  // No busy-wait tail on the render thread by default (config/thread_layout:
  // pacer_spin_us=N, or the legacy layout's 500 us, for A/B).
  realtimePacer.SetMaximumSpin(std::chrono::microseconds(
      Oot3dNativeGame::ActiveSwitchThreadLayout().PacerSpinMicroseconds));
  // config/frame_spikes (or config/gpu_timing): logs/spikes.txt, one line per
  // frame that lost its vsync, and the missed_* summary in perf.txt.
#if defined(__SWITCH__)
  // config/audio_deferred: absent = on, "0" = DSP frames before the guest
  // refresh as before (A/B).
  {
    std::ifstream audioDeferredFile("sdmc:/switch/oot3dre/config/audio_deferred");
    const bool present = audioDeferredFile.good();
    std::string text;
    if (present) std::getline(audioDeferredFile, text);
    deferDspAudio = !present || Oot3dNativeGame::ParseOffloadSwitch(text);
  }
#endif
  // config/pacer_vsync: the pacer follows the display's real vsync (phase
  // and period measured from viGetDisplayVsyncEvent) instead of the CPU's
  // 1/60 s; optional content = offset in ms after the vsync (0-15, default
  // 1). Off by default in this first version (A/B).
  {
    frameSpikesEnabled =
        std::ifstream("sdmc:/switch/oot3dre/config/frame_spikes").good() ||
        std::ifstream("sdmc:/switch/oot3dre/config/gpu_timing").good();
    std::ifstream pacerVsyncFile("sdmc:/switch/oot3dre/config/pacer_vsync");
    pacerVsyncConfigured = pacerVsyncFile.good();
    double offsetMs = 1.0;
    if (pacerVsyncConfigured && (pacerVsyncFile >> offsetMs) &&
        offsetMs >= 0.0 && offsetMs <= 15.0)
      pacerVsyncOffsetMs = offsetMs;
    bool vsyncStarted = false;
    if (frameSpikesEnabled || pacerVsyncConfigured) {
      vsyncStarted = vsyncClock.Start();
      // tour19 (rc 0x1272): no display event -> the window acquire's
      // returns (oot3d_vsync_clock.h). Valid after ~30 blocked acquires;
      // until then the pacer keeps the CPU clock.
      if (!vsyncStarted && vsyncClock.StartAcquireFallback()) {
        Oot3dNativeGame::gAcquireVsyncClock.store(&vsyncClock,
                                                   std::memory_order_relaxed);
        vsyncStarted = true;
      }
    }
    if (pacerVsyncConfigured && vsyncStarted) {
      realtimePacer.SetVsyncAnchor(
          [](const void *user, Oot3dNativeGame::VsyncClockSnapshot *snapshot) {
            *snapshot =
                static_cast<const Oot3dNativeGame::SwitchVsyncClock *>(user)->Read();
            return snapshot->Valid;
          },
          &vsyncClock,
          std::chrono::nanoseconds(
              static_cast<int64_t>(pacerVsyncOffsetMs * 1.0e6)));
    }
    if (frameSpikesEnabled) {
      spikeWriter.emplace("sdmc:/switch/oot3dre/logs/spikes.txt", 64U);
      char header[256];
      std::snprintf(header, sizeof(header),
                    "session_start vsync_clock=%d vsync_source=%s vsync_rc=0x%X "
                    "vsync_rc_module=%u vsync_rc_desc=%u vsync_stage=%u "
                    "pacer_vsync=%d offset_ms=%.2f\n",
                    vsyncStarted ? 1 : 0,
                    vsyncClock.Source() ==
                            Oot3dNativeGame::VsyncClockSource::DisplayEvent
                        ? "event"
                    : vsyncClock.Source() ==
                            Oot3dNativeGame::VsyncClockSource::Acquire
                        ? "acquire"
                        : "none",
                    vsyncClock.StartResult(), vsyncClock.StartResult() & 0x1FFU,
                    (vsyncClock.StartResult() >> 9U) & 0x1FFFU,
                    vsyncClock.StartStage(),
                    pacerVsyncConfigured && vsyncStarted ? 1 : 0,
                    pacerVsyncOffsetMs);
      (void)spikeWriter->Push(header);
    }
    std::fprintf(stderr,
                 "TRIAEVUM_FRAME_SPIKES enabled=%d vsync_clock=%d rc=0x%X "
                 "stage=%u source=%u pacer_vsync=%d offset_ms=%.2f\n",
                 frameSpikesEnabled ? 1 : 0, vsyncStarted ? 1 : 0,
                 vsyncClock.StartResult(), vsyncClock.StartStage(),
                 static_cast<unsigned>(vsyncClock.Source()),
                 pacerVsyncConfigured && vsyncStarted ? 1 : 0, pacerVsyncOffsetMs);
  }
#endif
  visualInterpolationEnabled =
      Oot3dNativeGame::ShouldUseNativeVisualInterpolation(
          frameRateContract,
          frameCompositionPolicy.InterpolationEnabled());
  // The two values above belong to the renderer. The guest detects policy
  // changes against its own copies.
  guestCompositionPolicy = frameCompositionPolicy;
  guestInterpolationEnabled = visualInterpolationEnabled;
#if defined(__SWITCH__)
  // The receipt's visual_interpolation: what the start presents (the launch
  // contract allows it at both rates since the FPS switches live).
  gSwitchVisualInterpolation = visualInterpolationEnabled;
#endif
  lastPresentationTime = std::chrono::steady_clock::now();
  picaPresentationScheduler.SetTimingEnabled(launch.ExtendedDiagnostics);
#if defined(__SWITCH__)
  // config/skip_bottom_screen (TopScreen only; absent = drawn as before):
  // the lower screen is never shown in that profile
  // (oot3d_native_bottom_screen_skip.h).
  {
    std::error_code skipBottomError;
    picaPresentationScheduler.SetSkipBottomScreen(
        launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen &&
        std::filesystem::exists("sdmc:/switch/oot3dre/config/skip_bottom_screen",
                                skipBottomError));
  }
#endif
  api.SetPicaPhysicalMemoryAccess(
      [&](uint32_t address, std::span<uint8_t> bytes) {
        RequireStoppedGuest("memory read");
        return picaMemoryView.Read(address, bytes);
      },
      [&](std::span<const Fast::Renderer3ds::PicaPhysicalMemoryWrite> writes) {
        RequireStoppedGuest("memory write");
        // The writeback is the renderer writing the copy itself, not a guest or
        // service access; keep it out of the guest-access counters. Scope, not
        // a pair of calls: an early return must not leave the watch suppressed.
        {
        const Oot3dNativeGame::NativeA32Memory::RangeWatchSuppression suppressed(
            process.Memory());
        for (const auto& write : writes) {
          if (!picaMemoryView.ForEachGuestAlias(write.Address, write.Bytes.size(),
              [&](uint32_t address, size_t size) { return process.Memory().IsWritable(address, size); }))
            return false;
        }
        for (const auto& write : writes) {
          if (!picaMemoryView.ForEachGuestAlias(write.Address, write.Bytes.size(),
              [&](uint32_t address, size_t) { return process.Memory().WriteBytes(address, write.Bytes); }))
            return false;
        }
        }
        return true;
      },
      [&](std::span<const Fast::Renderer3ds::PicaRawCopySpan> spans,
          Fast::Renderer3ds::PicaRenderBackend::PhysicalMemoryRead reader) {
        RequireStoppedGuest("raw copy");
        std::vector<std::pair<uint32_t, size_t>> ranges;
        for (const auto& span : spans) {
          if (!picaMemoryView.ForEachGuestAlias(span.DestinationAddress, span.ByteCount,
              [&](uint32_t address, size_t size) { ranges.emplace_back(address, size); return true; }))
            return false;
        }
        return process.Memory().DeferWrites(ranges,
            [&, reader = std::move(reader)](uint32_t address, std::span<uint8_t> bytes) {
              const auto physical = picaMemoryView.TranslateGuest(address, bytes.size());
              if (!physical) return false;
              // The bytes live in a renderer-owned GPU copy: a guest read
              // reaches it after every renderer command before it.
              if (renderStream.InCommand() ||
                  renderStream.CurrentMode() ==
                      Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline) {
                return reader(*physical, bytes);
              }
              bool read = false;
              renderStream.Run([&] { read = reader(*physical, bytes); });
              return read;
            });
      },
      [&](uint64_t) {
        RequireStoppedGuest("PPF interrupt");
        if (!hostServices.SignalPicaInterrupt(process.Memory(), Oot3dNativeGame::Oot3dPicaInterruptId::Ppf)) return false;
        ++picaStats.CompletedPpfCount;
        ++picaStats.RecordedCompletionCount;
        return true;
      },
      [&]() {
        RequireStoppedGuest("deferred writes");
        return process.Memory().MaterializeDeferredWrites();
      });
  physicalMemoryAccessLifetime.emplace(api);
  rendererTiming = &phaseTiming;
  // Declared after every renderer-owned local the commands reference: leaving
  // the loop (normally or by exception) stops the worker before they die.
  renderStreamStop.emplace(renderStream);

  benchmark = NativeA32BenchmarkWindow{};  // its default members read the clock here
  pacingTrace = std::getenv("TRIAEVUM_PACING_TRACE");
  measurePacing = pacingTrace && std::string_view(pacingTrace) == "1";
  benchmarkFrameTimes = (hostArgs.ThroughputBenchmark || measurePacing)
      ? std::make_unique<Fast::Renderer::FrameTimeDistribution>() : nullptr;
  lastCompletedFrameTime = std::chrono::steady_clock::now();
  cpuProbeEnvironment = std::getenv("TRIAEVUM_PROFILE_CPU_LEDGER");
  cpuProbeRequested = CpuProbe::BuildEnabled && cpuProbeEnvironment &&
      std::string_view(cpuProbeEnvironment) == "1";

  parallelPresentationEnvironment =
      std::getenv("TRIAEVUM_PARALLEL_PRESENTATION");
  // Harnesses override the launch choice: "0" serial, "1" parallel.
  parallelPresentation =
      parallelPresentationEnvironment != nullptr &&
              (std::string_view(parallelPresentationEnvironment) == "0" ||
               std::string_view(parallelPresentationEnvironment) == "1")
          ? std::string_view(parallelPresentationEnvironment) == "1"
          : launch.ParallelPresentation;
  // An OpenGL context is current only on the thread that created it; the
  // worker would issue GL calls without one (glCreateShader returns 0 and
  // glGetString null, as first seen on the Switch homebrew).
  // The Switch can opt back in: the GL context then moves to the worker.
  bool openGlWorker = false;
  if (hostArgs.Renderer == "opengl") {
#if defined(__SWITCH__)
    std::error_code optInError;
    // On by default since it moved ~10 ms of GL work off the game thread on
    // the console; config/serial_presentation turns it off.
    openGlWorker = parallelPresentation &&
        !std::filesystem::exists("sdmc:/switch/oot3dre/config/serial_presentation", optInError);
#endif
    parallelPresentation = openGlWorker;
  }
  fast3dGui = dynamic_cast<Fast::Fast3dGui*>(gui.get());
  if (parallelPresentation) {
    // Renderer timings accumulate apart and merge once the stream drains.
    rendererTiming = &workerPhaseTiming;
    renderStream.SetMode(
        Oot3dNativeGame::NativeRenderCommandStream::Mode::Worker);
    // deko3d (config/renderer): no GL context to move; its objects are not
    // bound to a thread.
    if (openGlWorker && !Fast::Deko3dRendererSelected()) {
      // Release the context here and make it current on the render worker,
      // which from now on issues every renderer command.
      SDL_Window* glWindow = SDL_GL_GetCurrentWindow();
      SDL_GLContext glContext = SDL_GL_GetCurrentContext();
#if defined(__SWITCH__)
      std::ofstream glTrace("sdmc:/switch/oot3dre/logs/glthread.txt", std::ios::trunc);
      glTrace << "main thread=" << threadGetCurHandle() << " sdl_window=" << glWindow
              << " sdl_ctx=" << glContext << " egl_ctx=" << eglGetCurrentContext()
              << " egl_draw=" << eglGetCurrentSurface(EGL_DRAW)
              << " gl_version=" << (glGetString(GL_VERSION) != nullptr) << '\n';
#endif
      const int releaseResult = SDL_GL_MakeCurrent(glWindow, nullptr);
#if defined(__SWITCH__)
      glTrace << "release=" << releaseResult << " err=" << SDL_GetError()
              << " egl_ctx_after=" << eglGetCurrentContext() << " egl_error=" << eglGetError() << '\n';
      glTrace.flush();
#else
      (void)releaseResult;
#endif
      // Run() executes on the calling thread once the queue drains; only
      // Submit() reaches the worker. Drain() then waits for it (and rethrows).
      renderStream.Submit([glWindow, glContext] {
        const int result = SDL_GL_MakeCurrent(glWindow, glContext);
#if defined(__SWITCH__)
        std::ofstream trace("sdmc:/switch/oot3dre/logs/glthread.txt", std::ios::app);
        trace << "worker thread=" << threadGetCurHandle() << " make_current=" << result
              << " err=" << SDL_GetError() << " egl_error=" << eglGetError()
              << " egl_ctx=" << eglGetCurrentContext() << " sdl_ctx=" << SDL_GL_GetCurrentContext()
              << " gl_version=" << (glGetString(GL_VERSION) != nullptr ? reinterpret_cast<const char*>(glGetString(GL_VERSION)) : "(null)")
              << '\n';
#endif
        if (result != 0) {
          throw std::runtime_error(std::string("render worker GL context: ") + SDL_GetError());
        }
#if defined(__SWITCH__)
        // config/gl_thread: Mesa's GL thread behind this worker (A/B).
        Fast::StartSwitchMesaGlThread();
#endif
      });
      renderStream.Drain();
    }
#if defined(__SWITCH__)
    // config/frames_in_flight=2 is ignored: on the console (fps60fix.nro,
    // tour3b) it froze at the first warp with the game and render threads
    // inside malloc (heap corrupted: state shared by the two threads assumes
    // the guest at most one frame ahead of the renderer). The stream supports
    // it (SetMaxFramesInFlight, tested); the session does not yet.
    {
      std::error_code framesInFlightError;
      if (std::filesystem::exists("sdmc:/switch/oot3dre/config/frames_in_flight",
                                  framesInFlightError)) {
        std::fprintf(stderr, "TRIAEVUM frames_in_flight ignored (1 frame in flight)\n");
      }
      renderStream.SetMaxFramesInFlight(1U);
    }
#endif
    // config/early_frame_slot: present = on, content "0" = off (A/B).
    {
      std::ifstream earlySlotFile(
#if defined(__SWITCH__)
          "sdmc:/switch/oot3dre/config/early_frame_slot"
#else
          "config/early_frame_slot"
#endif
      );
      if (earlySlotFile.is_open()) {
        std::string value;
        std::getline(earlySlotFile, value);
        earlyFrameSlot = value.empty() || value[0] != '0';
      }
      std::fprintf(stderr, "TRIAEVUM early_frame_slot=%d\n",
                   earlyFrameSlot ? 1 : 0);
    }
    // The ImGui overlay (and the display composition it triggers) renders
    // in renderer order, from a copy of each frame's draw data.
    if (fast3dGui != nullptr) {
      fast3dGui->SetRendererRouting(
          {[&](std::function<void()> work) { renderStream.Run(std::move(work)); },
           [&](std::function<void()> work) {
             renderStream.Submit(std::move(work));
           }});
    }
  }
  // The routing refers to this loop's stream: removed on every exit.
  guiRendererRoutingReset.emplace(parallelPresentation ? fast3dGui : nullptr);
  // Deferred PICA planning: the guest thread queues each drained draw as a
  // placeholder and its plan is built by a background worker or, at the
  // latest, by the render command that executes it. Only with the render
  // worker (the serial loop keeps the original order of work).
  // Switch: on by default; config/serial_pica turns it off for A/B, and
  // config/pica_plan_render_thread builds on the render thread without the
  // extra worker. Desktop: TRIAEVUM_DEFERRED_PICA_PLAN=1 (worker) or =render.
  {
    bool deferredPicaPlans = false;
    bool picaPlanWorker = true;
#if defined(__SWITCH__)
    std::error_code picaPlanError;
    deferredPicaPlans = !std::filesystem::exists(
        "sdmc:/switch/oot3dre/config/serial_pica", picaPlanError);
    picaPlanWorker = !std::filesystem::exists(
        "sdmc:/switch/oot3dre/config/pica_plan_render_thread", picaPlanError);
#else
    const char* deferredPicaPlanEnvironment =
        std::getenv("TRIAEVUM_DEFERRED_PICA_PLAN");
    if (deferredPicaPlanEnvironment != nullptr) {
      const std::string_view value(deferredPicaPlanEnvironment);
      deferredPicaPlans = value == "1" || value == "render";
      picaPlanWorker = value == "1";
    }
#endif
    // Texture decode moved off the render thread (the hitches entering a new
    // area). The OpenGL and deko3d backends take the decoded mips. Switch:
    // config/no_texture_predecode turns it off.
    bool predecodeTextures =
        std::string_view(apiPointer->GetName()) == "OpenGL" ||
        std::string_view(apiPointer->GetName()) == "deko3d";
#ifdef __SWITCH__
    predecodeTextures = predecodeTextures &&
        !std::filesystem::exists(
            "sdmc:/switch/oot3dre/config/no_texture_predecode", picaPlanError);
#endif
    picaPlanBuilder.SetPredecodeTextures(predecodeTextures);
    // Plan build cost per draw (SWITCH_PORT.md): the canonical identity is
    // left out where nothing reads it (deko3d without the semantic trace or
    // extended diagnostics, whose serial builds share these options), plans
    // are built in place, and the worker and the render thread build
    // different slots at once. Kill switches: config/plan_full_identity,
    // config/plan_copy_build, config/plan_one_lock. Desktop:
    // TRIAEVUM_PLAN_BUILD=legacy turns all three off.
    {
      bool fullIdentity = false;
      bool copyBuild = false;
      bool oneLock = false;
#if defined(__SWITCH__)
      fullIdentity = std::filesystem::exists(
          "sdmc:/switch/oot3dre/config/plan_full_identity", picaPlanError);
      copyBuild = std::filesystem::exists(
          "sdmc:/switch/oot3dre/config/plan_copy_build", picaPlanError);
      oneLock = std::filesystem::exists(
          "sdmc:/switch/oot3dre/config/plan_one_lock", picaPlanError);
#else
      const char* planBuildEnvironment = std::getenv("TRIAEVUM_PLAN_BUILD");
      if (planBuildEnvironment != nullptr &&
          std::string_view(planBuildEnvironment) == "legacy") {
        fullIdentity = copyBuild = oneLock = true;
      }
#endif
      const bool identityUnread =
          std::string_view(apiPointer->GetName()) == "deko3d" &&
          !picaSemanticTrace.Enabled() && !launch.ExtendedDiagnostics;
      picaPlanBuilder.SetBuildOptions(!identityUnread || fullIdentity,
                                      !copyBuild, !oneLock);
      std::fprintf(stderr,
                   "TRIAEVUM_PLAN_BUILD canonical_identity=%d in_place=%d "
                   "parallel=%d\n",
                   (!identityUnread || fullIdentity) ? 1 : 0, copyBuild ? 0 : 1,
                   oneLock ? 0 : 1);
    }
    picaPlanBuilder.Configure(deferredPicaPlans && parallelPresentation,
                              picaPlanWorker);
  }

  // Paced interpolation (config/fps60; oot3d_native_interpolation_pacing.h)
  // and the native replay. Switch: config/interp_pacing ("0": every
  // in-between frame drawn, as before), config/interp_scale (e.g. "1.5":
  // in-between frames that do not fit at full resolution are drawn at that
  // scale), config/interp_scale_mode ("always": every in-between frame
  // reduced), config/native_replay ("1": replay the native namespace even on
  // a backend that never reads it), config/fps60_mode (presentation
  // cadence). Desktop: TRIAEVUM_INTERP_PACING, TRIAEVUM_INTERP_SCALE,
  // TRIAEVUM_INTERP_SCALE_MODE, TRIAEVUM_NATIVE_REPLAY, TRIAEVUM_FPS60_MODE.
  {
    const auto readText = [](const char* file, const char* environment)
        -> std::optional<std::string> {
#if defined(__SWITCH__)
      (void)environment;
      std::ifstream input(std::string("sdmc:/switch/oot3dre/config/") + file);
      if (!input) return std::nullopt;
      std::string text;
      std::getline(input, text);
      return text;
#else
      (void)file;
      const char* value = std::getenv(environment);
      return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
    };
    const auto trimmed = [](std::string text) {
      while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                               text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
      return text;
    };
    auto& options = interpolationPacingOptions;
    if (const auto text = readText("interp_pacing", "TRIAEVUM_INTERP_PACING"))
      options.KeepGameSpeed = trimmed(*text) != "0";
    if (const auto text = readText("interp_scale", "TRIAEVUM_INTERP_SCALE"))
      options.ReducedScaleText = trimmed(*text);
    if (const auto text = readText("interp_margin_ms", "TRIAEVUM_INTERP_MARGIN_MS"))
      options.MarginText = trimmed(*text);
    if (const auto text =
            readText("interp_scale_mode", "TRIAEVUM_INTERP_SCALE_MODE"))
      options.ReducedAlways = trimmed(*text) == "always";
    if (const auto text = readText("native_replay", "TRIAEVUM_NATIVE_REPLAY"))
      options.ForceNativeReplay = trimmed(*text) == "1";
    // config/pacer_entry_recover (TRIAEVUM_PACER_ENTRY_RECOVER): on without
    // the file; "0" keeps the former probe schedule and counts the
    // render-thread holds as frame work (SWITCH_PORT.md "60 FPS back sooner
    // after entering an area").
    if (const auto text =
            readText("pacer_entry_recover", "TRIAEVUM_PACER_ENTRY_RECOVER"))
      options.PacerEntryRecover = trimmed(*text) != "0";
    // Draws that exist in only one of two guest frames (culling/LOD while
    // the camera turns fast; oot3d_native_pica_visual_frame.h): the step
    // frame shows the previous guest frame itself (config/interp_exact_step)
    // and the in-between frame moves unpartnered draws with the measured
    // camera (config/interp_carry_unmatched). Both on; "0" restores the old
    // sample (A/B of the flicker at 60 FPS). Desktop:
    // TRIAEVUM_INTERP_EXACT_STEP, TRIAEVUM_INTERP_CARRY_UNMATCHED.
    {
      Oot3dNativeGame::Oot3dPicaVisualTransitionOptions transitionOptions;
      transitionOptions.ExactStep = true;
      transitionOptions.CarryUnmatched = true;
      if (const auto text = readText("interp_exact_step", "TRIAEVUM_INTERP_EXACT_STEP"))
        transitionOptions.ExactStep = trimmed(*text) != "0";
      if (const auto text =
              readText("interp_carry_unmatched", "TRIAEVUM_INTERP_CARRY_UNMATCHED"))
        transitionOptions.CarryUnmatched = trimmed(*text) != "0";
      preparedVisualTransition.SetOptions(transitionOptions);
    }
    // config/dynamic_resolution (TRIAEVUM_DYNAMIC_RESOLUTION): present = on;
    // optional text = lowest scale (oot3d_native_dynamic_resolution.h).
    if (const auto text =
            readText("dynamic_resolution", "TRIAEVUM_DYNAMIC_RESOLUTION")) {
      options.DynamicResolution = true;
      options.DynamicResolutionText = trimmed(*text);
    }
    // config/drs_60 (TRIAEVUM_DRS_60): "full" holds the full scale at 60
    // FPS (L3+R3 > Video > Resolution at 60 FPS; the menu changes it live).
    if (const auto text = readText("drs_60", "TRIAEVUM_DRS_60"))
      options.DynamicResolutionSixty =
          Oot3dNativeGame::ParseDynamicResolutionSixtyMode(trimmed(*text));
    std::fprintf(stderr, "TRIAEVUM_DRS_60 mode=%s\n",
                 Oot3dNativeGame::DynamicResolutionSixtyModeName(
                     options.DynamicResolutionSixty).data());
    // config/fps60_mode: legacy (default), adaptive, strict60 (A/B of the
    // presentation cadence, oot3d_native_cadence_governor.h). Without it,
    // config/interp_pacing=0 keeps its meaning (every in-between frame
    // drawn): strict60.
    if (const auto text = readText("fps60_mode", "TRIAEVUM_FPS60_MODE")) {
      options.Fps60Mode = Oot3dNativeGame::ParseFps60Mode(trimmed(*text));
    } else if (!options.KeepGameSpeed) {
      options.Fps60Mode.Mode = Oot3dNativeGame::Fps60Mode::Strict60;
    }
  }

#if defined(__SWITCH__)
  WriteSwitchBootStage("runtime_loop");
#endif
}

void NativeA32WindowSession::ApplyTopScreenConfigSnapshot() {
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  const auto snapshot = topScreenConfigRuntime->Snapshot();
  if (snapshot.Revision == appliedTopScreenConfigRevision) {
    return;
  }
  activeTopScreenConfig = snapshot.Config;
  appliedTopScreenConfigRevision = snapshot.Revision;
  uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
  widescreenProjection.TopScreenConfig = activeTopScreenConfig;
  Oot3dNativeGame::ApplyTopScreenFreeCameraConfig(
      nativeCandidateDispatch.TopScreenCamera.Camera,
      activeTopScreenConfig);
  if (!activeTopScreenConfig.FreeCameraEnabled) {
    nativeCandidateDispatch.TopScreenCameraInput.Reset();
  }
}

void NativeA32WindowSession::SetTopScreenSessionMinimapVisible(bool visible) {
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  activeTopScreenConfig.MinimapVisible = visible;
  uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
  widescreenProjection.TopScreenConfig = activeTopScreenConfig;
}

void NativeA32WindowSession::ApplySelectedUiProfile() {
  auto &process = *processStorage;
  if (launch.UiProfile != Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    return;
  }
  Oot3dNativeGame::TopScreenLayoutApplyStats layoutStats;
  if (!Oot3dNativeGame::ApplyTopScreenVerifiedLayout(process.Memory(),
                                                     &layoutStats, &error)) {
    throw std::runtime_error(
        "TopScreen source-port layout rejected the process image: " + error);
  }
  Oot3dNativeGame::TopScreenRuntimeGeometryApplyStats geometryStats;
  if (layoutStats.WordsChanged != 0U &&
      !Oot3dNativeGame::RebindTopScreenVerifiedRuntimeGeometry(
          process.Memory(), &geometryStats, &error)) {
    throw std::runtime_error(
        "TopScreen source-port runtime geometry rejected the process "
        "image: " +
        error);
  }
  std::cout << "TopScreen source-port layout: contracts="
            << layoutStats.ContractsChecked
            << " words=" << layoutStats.WordsWritten
            << " changed_words=" << layoutStats.WordsChanged
            << " rebound_streams=" << geometryStats.StreamsRebound
            << " rebound_quads=" << geometryStats.QuadsRebound << '\n';
}

void NativeA32WindowSession::UpdateScenePresentation() {
  auto &window = *windowPointer;
  uint32_t sceneWidth = std::max<uint32_t>(1, window.GetWidth());
  const uint32_t sceneHeight = std::max<uint32_t>(1, window.GetHeight());
  // The Vulkan scene target takes the window's shape. The OpenGL one keeps
  // the native 5:3 (letterboxed) unless its widescreen option widens it by
  // GlWidescreenFactor, so the frustum must widen by exactly that much. The
  // deko3d targets follow the GL rule (gfx_deko3d_pica_targets.cpp).
  if (apiPointer != nullptr &&
      (std::string_view(apiPointer->GetName()) == "OpenGL" ||
       std::string_view(apiPointer->GetName()) == "deko3d")) {
    const float factor = Fast::Oot3d::GlWidescreenEnabled()
                             ? Fast::Oot3d::GlWidescreenFactor(sceneWidth, sceneHeight)
                             : 1.0F;
    sceneWidth = std::max<uint32_t>(
        1U, static_cast<uint32_t>(std::lround(
                static_cast<float>(sceneHeight) *
                Fast::Oot3d::kNativeTopScreenAspect * factor)));
  }
  widescreenProjection.Projection.Presentation =
      Fast::Oot3d::ResolveScenePresentationPolicy(sceneWidth, sceneHeight);
  const auto graphics = Fast::Oot3d::GraphicsSettingsRuntime::Instance().Snapshot();
  widescreenProjection.Projection.FovMultiplier = graphics.FovMultiplier;
  auto &visibility = widescreenProjection.ActorVisibility;
  // F1 > Display > Extended draw distance (PC) or L3+R3 > Video > Draw
  // distance (Switch, config/draw_distance): the farther one wins.
  const auto choice = Oot3dNativeGame::LiveActorDrawDistance().load(std::memory_order_relaxed);
  visibility.DistanceMultiplier = std::max(
      graphics.ExtendedDrawDistance ? Oot3dNativeGame::kExtendedActorDistanceMultiplier : 1.0F,
      Oot3dNativeGame::ActorDrawDistanceMultiplier(choice));
  visibility.ExtendGroups = choice != Oot3dNativeGame::ActorDrawDistance::Original &&
                            !Oot3dNativeGame::LiveDrawDistanceNativeGroups().load(std::memory_order_relaxed);
  visibility.NoKokiriFade = graphics.ExtendedDrawDistance;
  // Game log: the effective draw distance at start and on each change.
  if (auto line = Oot3dNativeGame::TakeNativeActorVisibilitySettingsLine(visibility, choice))
    Oot3dNativeGame::WriteGameLog(std::move(*line));
  visibility.Presentation = widescreenProjection.Projection.Presentation;
  visibility.FovMultiplier = graphics.FovMultiplier;
}

bool NativeA32WindowSession::WidescreenMenuCornersActive() {
  if (apiPointer == nullptr || windowPointer == nullptr ||
      std::string_view(apiPointer->GetName()) != "OpenGL" ||
      !nativeCandidateDispatch.TopScreenUiProfile ||
      !uiLifecycleBridgeStorage.has_value() ||
      !uiLifecycleBridgeStorage->NativeFrontendPresentationActive() ||
      !Fast::Oot3d::GlWidescreenEnabled() ||
      !Fast::Oot3d::GlWidescreenMenusEnabled() ||
      !Fast::Oot3d::GlWidescreenMenuCornersEnabled()) {
    return false;
  }
  auto &window = *windowPointer;
  return Fast::Oot3d::GlWidescreenFactor(window.GetWidth(), window.GetHeight()) > 1.0F;
}

Fast::Oot3d::FrameRateMode NativeA32WindowSession::EffectiveFrameRateMode() const {
  return launchFrameRateMode.has_value()
             ? *launchFrameRateMode
             : Fast::Oot3d::GraphicsSettingsRuntime::Instance()
                   .Snapshot()
                   .FrameRate;
}

Fast::Renderer3ds::PicaFrameCompositionPolicy NativeA32WindowSession::ResolveFrameCompositionPolicy(const Fast::Oot3d::PresentationPacingPolicy& pacing) {
  auto policy = pacing.Composition;
  policy.NativeRateHz = frameRateContract.SimulationRateHz;
  if (!frameRateContract.VisualInterpolationAllowed) {
    policy.Interpolation =
        Fast::Oot3d::NativeVisualInterpolationMode::Disabled;
    policy.FixedSampleMultiplier = 1U;
  }
  return policy;
}

} // namespace Oot3dA32Window
