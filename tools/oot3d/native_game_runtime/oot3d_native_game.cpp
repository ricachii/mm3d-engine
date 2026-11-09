#include "oot3d_interior_sun_prebuild.h"
#include <cstdio>
#include <cstdlib>
#include "oot3d_game_language.h"
#include <array>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__SWITCH__) || defined(__ANDROID__)
#include <SDL.h>
#endif

#include "oot3d_demo_host_context.h"
#if defined(__ANDROID__)
#include "android_host.h"
#endif
#include "oot3d_native_game_bootstrap.h"
#include "oot3d_native_game_launch_profile.h"
#include "triaevum_product_info.h"
#include "oot3d_native_a32_window.h"
#if defined(OOT3D_REQUIRE_WHOLE_AOT_PLUGIN_V2)
#include "oot3d_native_direct_aot.h"
#include "triaevum_title_plugin_loader.h"
#endif

#if defined(__SWITCH__)
#include "fast/oot3d/debug_menu.h"
#include "oot3d_update_panel.h"
#include "oot3d_whole_aot_gcov.h"
#include "oot3d_switch_console_screen.h"
#include "oot3d_switch_rom_import.h"
#include "oot3d_switch_topscreen_import.h"
#include "oot3d_native_status_file.h"
#include "oot3d_switch_performance.h"
#include <switch.h>
// libnx (applet.c, weak, default 0): 1 makes libnx's exit end the application
// through AM instead of returning to the homebrew loader.
extern "C" u32 __nx_applet_exit_mode;
void PlaceSwitchMainThread(); // oot3d_switch_threads.cpp
#endif

namespace {

#if defined(__SWITCH__)
// Through the process's boot-status writer (oot3d_native_status_file.h): a
// heartbeat still queued on its service thread is dropped, never written
// over completed/failed.
void WriteSwitchBootStatus(const char* status, const std::string& detail = {}) {
    std::string text = std::string("status=") + status + '\n' +
                       "whole_aot_functions=12419\n"
                       "host_boundaries=3\n"
                       "residual_a32=0\n";
    if (!detail.empty()) {
        text += "detail=" + detail + '\n';
    }
    (void)Oot3dNativeGame::SwitchBootStatusFile().WriteNow(text);
}

// A fatal error shows the system's error dialog before the game closes, so the
// player learns why instead of getting a crash report from the homebrew loader.
void ShowSwitchFatalError(const char* what) {
    ErrorApplicationConfig config;
    const std::string details = std::string(what) +
                                "\n\nThe log is in switch/oot3dre/logs/oot3d_native_game.log.";
    if (R_SUCCEEDED(errorApplicationCreate(
            &config, "Ocarina of Time 3D stopped because of an error.", details.c_str())))
        (void)errorApplicationShow(&config);
}
#endif

void SetRendererEnvironment(const char* name,
                            const std::filesystem::path& value) {
    if (value.empty())
        return;
#ifdef _WIN32
    if (_wputenv_s(std::filesystem::path(name).c_str(), value.c_str()) != 0)
#else
    if (setenv(name, value.string().c_str(), 1) != 0)
#endif
        throw std::runtime_error(std::string("could not set ") + name);
}

void ConfigurePicaAotShaders(const Oot3dNativeGameLaunch& launch) {
    SetRendererEnvironment("TRIAEVUM_RENDERER_CACHE_DIR", launch.RendererCacheDirectory);
    SetRendererEnvironment("TRIAEVUM_WATER_TEXTURE_CATALOG", launch.WaterTextureCatalogPath);
    SetRendererEnvironment("TRIAEVUM_SCENE_LIGHT_CATALOG", launch.SceneLightCatalogPath);
    SetRendererEnvironment("OOT3D_PICA_AOT_SHADER_PACK",
                           launch.PicaAotShaderPackPath);
    SetRendererEnvironment("OOT3D_PICA_EFFECTIVE_SHADER_INVENTORY",
                           launch.PicaEffectiveShaderInventoryPath);
    SetRendererEnvironment("OOT3D_PICA_PIPELINE_INVENTORY",
                           launch.PicaPipelineInventoryPath);
    SetRendererEnvironment("OOT3D_PICA_PIPELINE_MANIFEST",
                           launch.PicaPipelineManifestPath);
    if (launch.PicaAotShaderStrict) {
#ifdef _WIN32
        if (_putenv_s("OOT3D_PICA_AOT_SHADER_STRICT", "1") != 0)
#else
        if (setenv("OOT3D_PICA_AOT_SHADER_STRICT", "1", 1) != 0)
#endif
            throw std::runtime_error(
                "could not enable strict PICA AOT shaders");
    }
    if (launch.PicaPipelinePrewarm) {
#ifdef _WIN32
        if (_putenv_s("OOT3D_PICA_PIPELINE_PREWARM", "1") != 0)
#else
        if (setenv("OOT3D_PICA_PIPELINE_PREWARM", "1", 1) != 0)
#endif
            throw std::runtime_error(
                "could not enable PICA pipeline prewarm");
    }
}

struct NativeGameArguments {
    std::vector<std::string> Storage;
    std::vector<char*> Pointers;
};

NativeGameArguments PrepareNativeGameArguments(int argc, char** argv) {
    NativeGameArguments prepared;
    std::filesystem::path profilePath;
    int commandLineStart = argc;

    if (argc <= 1) {
        profilePath = Oot3dNativeGame::DefaultNativeGameLaunchProfilePath(
            argc > 0 ? argv[0] : "oot3d_native_game");
        commandLineStart = argc;
    } else if (std::string_view(argv[1]) == "--launch-profile") {
        if (argc <= 2) {
            throw std::runtime_error("--launch-profile requires a path");
        }
        profilePath = argv[2];
        commandLineStart = 3;
    } else {
        return prepared;
    }

    prepared.Storage.emplace_back(
        argc > 0 ? argv[0] : "oot3d_native_game");
    auto profileArguments =
        Oot3dNativeGame::LoadNativeGameLaunchProfile(profilePath);
    prepared.Storage.insert(prepared.Storage.end(),
                            std::make_move_iterator(profileArguments.begin()),
                            std::make_move_iterator(profileArguments.end()));
    for (int index = commandLineStart; index < argc; ++index) {
        prepared.Storage.emplace_back(argv[index]);
    }
    prepared.Pointers.reserve(prepared.Storage.size());
    for (auto& argument : prepared.Storage) {
        prepared.Pointers.push_back(argument.data());
    }
    std::cout << "Using native game launch profile: "
              << std::filesystem::absolute(profilePath).lexically_normal()
              << '\n';
    return prepared;
}

} // namespace

#if defined(__linux__) && !defined(__SWITCH__) && !defined(__ANDROID__)
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace {
// Diagnostic (opt-in): with TRIAEVUM_BACKTRACE_SIGNAL=1, SIGUSR1 makes the receiving thread print its backtrace.
// Send it to each thread with tgkill to see where a hung process is waiting (no debugger needed).
void DumpThreadBacktrace(int) {
    void* frames[48];
    const int count = backtrace(frames, 48);
    char header[64];
    const int length = std::snprintf(header, sizeof(header), "BT tid=%ld frames=%d\n",
                                     static_cast<long>(syscall(SYS_gettid)), count);
    if (length > 0) (void)!write(2, header, static_cast<size_t>(length));
    backtrace_symbols_fd(frames, count, 2);
}
}  // namespace
#endif

int main(int argc, char** argv) {
#if defined(__linux__) && !defined(__SWITCH__) && !defined(__ANDROID__)
    if (std::getenv("TRIAEVUM_BACKTRACE_SIGNAL") != nullptr) {
        struct sigaction action{};
        action.sa_handler = DumpThreadBacktrace;
        sigemptyset(&action.sa_mask);
        sigaction(SIGUSR1, &action, nullptr);
    }
#endif
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--prebuild-interior-sun")
            return Oot3dNativeGame::InteriorSunToolMain(argc, argv);
#if defined(__ANDROID__)
        InitializeAndroidGameHost();
#endif
#if defined(OOT3D_REQUIRE_WHOLE_AOT_PLUGIN_V2)
        if (argc == 3 && std::string_view(argv[1]) == "--verify-title-plugin") {
            Oot3dNativeGame::ConfigureTitlePlugin(argv[2]);
            if (!Oot3dNativeGame::Oot3dWholeAotPluginV2Available())
                throw std::runtime_error("Selected title plugin does not expose a valid whole-AOT ABI v2");
            WriteTriAevumProductInfo(std::cout);
            return 0;
        }
#endif
        if (argc == 3 && std::string_view(argv[1]) == "--game-language-info") {
            std::cout << Oot3dNativeGame::GameLanguageDocument(
                Oot3dNativeGame::DetectGameLanguages(argv[2])).dump() << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--product-info") {
            WriteTriAevumProductInfo(std::cout);
            return 0;
        }
        Oot3dNativeGameLaunch launch;
        NativeGameArguments preparedArguments;
#if defined(__SWITCH__)
        PlaceSwitchMainThread();
        // Instrumented (PGO generate) build only: stream the counts to
        // sdmc:/switch/oot3dre/pgo (oot3d_whole_aot_gcov.h).
        Oot3dNativeGame::Oot3dWholeAotPgoStart();
        // First launch: prepare game/ from the player's decrypted dump
        // before anything reads the manifest or the RomFS.
        if (!Oot3dNativeGame::RunSwitchRomImport())
            return 0;
        // The TopScreen HUD textures come from a mod the release cannot ship:
        // offer to download it and build config/atlas_overrides.o3tu from
        // game/romfs.bin before the launch arguments look for the pack, on
        // the same console as the import.
        if (!Oot3dNativeGame::RunSwitchTopScreenImport())
            return 0;
        // The start-up console gives the window back to SDL/deko3d.
        Oot3dNativeGame::CloseStartupScreen();
        // A Mesa built with its Horizon disk cache (see docs) stores compiled
        // GLSL and nvc0 code here; the stock devkitPro Mesa ignores these.
        setenv("MESA_SHADER_CACHE_DIR", "sdmc:/switch/oot3dre/mesacache", 0);
        setenv("MESA_GLSL_CACHE_MAX_SIZE", "128M", 0);
        // An update installed by the previous run swaps its remaining files
        // now, before anything opens them (oot3d_update_release.h).
        Oot3dNativeGame::FinishPendingUpdate("sdmc:/switch/oot3dre");
        // SDL's positional face-button convention is useful for portable PC
        // layouts, but the Switch build should follow the labels printed on
        // Joy-Con and Pro Controllers: A is A, B is B, and so on.
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
        launch.ControlConfig = Oot3dNativeGame::NativeControlPreset(
            Oot3dNativeGame::NativeControlProfile::Controller);
        std::array<char*, 21> homebrewArguments{
            argc > 0 ? argv[0] : const_cast<char*>("oot3d_native_game.nro"),
            const_cast<char*>("--a32-process-manifest"),
            const_cast<char*>(
                "sdmc:/switch/oot3dre/game/oot3d_native_process_manifest.json"),
            const_cast<char*>("--resource-root"),
            const_cast<char*>("sdmc:/switch/oot3dre/resources.o2r"),
            const_cast<char*>("--renderer"),
            const_cast<char*>("opengl"),
            const_cast<char*>("--save-data"),
            const_cast<char*>("sdmc:/switch/oot3dre/savedata"),
            const_cast<char*>("--ui-profile"),
            const_cast<char*>("topscreen"),
            const_cast<char*>("--topscreen-config"),
            const_cast<char*>(
                "sdmc:/switch/oot3dre/config/topscreen_ui.json"),
            const_cast<char*>("--topscreen-texture-overrides"),
            const_cast<char*>(
                "sdmc:/switch/oot3dre/config/atlas_overrides.o3tu"),
            const_cast<char*>("--controls-config"),
            const_cast<char*>(
                "sdmc:/switch/oot3dre/config/oot3d_controls.json"),
            const_cast<char*>("--gameplay-timing"),
            const_cast<char*>("native30_interpolated"),
            const_cast<char*>("--presentation-rate"),
            const_cast<char*>("30"),
        };
        // config/fps60: game logic stays at 30 steps per second and the
        // runtime interpolates an extra frame between steps (60 Hz output).
        // Doubles the GPU and render-thread work; pair it with a lower
        // config/resolution_scale. Both rates launch native30_interpolated
        // (at 30 it presents Original30, nothing interpolated): the timing
        // contract must allow interpolation so L3+R3 > Video > FPS can
        // switch to 60 while the game runs (oot3d_native_launch_frame_rate.h).
        if (std::ifstream("sdmc:/switch/oot3dre/config/fps60")) {
            homebrewArguments[20] = const_cast<char*>("60");
        }
        // config/atlas_overrides.o3tu is built on the console from the
        // third-party TopScreen mod (RunSwitchTopScreenImport), which a release
        // cannot ship: without it the TopScreen UI runs without texture
        // overrides instead of refusing to start.
        std::vector<char*> homebrewArgv(homebrewArguments.begin(), homebrewArguments.end());
        if (!std::filesystem::is_regular_file(homebrewArguments[14])) {
            homebrewArgv.erase(homebrewArgv.begin() + 13, homebrewArgv.begin() + 15);
        } else {
            if (std::filesystem::is_regular_file(
                    "sdmc:/switch/oot3dre/config/topscreen_ui_atlas_hd.png")) {
                // The HD HUD art beside the pack (built by the same import); a
                // file the game cannot use falls back to the native atlas.
                homebrewArgv.push_back(const_cast<char*>("--topscreen-ui-atlas-hd"));
                homebrewArgv.push_back(
                    const_cast<char*>("sdmc:/switch/oot3dre/config/topscreen_ui_atlas_hd.png"));
            }
            // The mod's HD images of the game's own HUD textures (hearts, B
            // label, cursor), from the same import.
            if (std::filesystem::is_directory("sdmc:/switch/oot3dre/config/topscreen_hd")) {
                homebrewArgv.push_back(const_cast<char*>("--topscreen-hd-textures"));
                homebrewArgv.push_back(const_cast<char*>("sdmc:/switch/oot3dre/config/topscreen_hd"));
            }
        }
        if (argc <= 1) {
            argc = static_cast<int>(homebrewArgv.size());
            argv = homebrewArgv.data();
        }
#else
        preparedArguments = PrepareNativeGameArguments(argc, argv);
        if (!preparedArguments.Pointers.empty()) {
            argc = static_cast<int>(preparedArguments.Pointers.size());
            argv = preparedArguments.Pointers.data();
        }
#endif
#if defined(OOT3D_REQUIRE_WHOLE_AOT_PLUGIN_V2)
        std::vector<char*> runtimeArguments;
        bool pluginSelected = false;
        for (int index = 0; index < argc; ++index) {
            if (std::string_view(argv[index]) == "--title-plugin") {
                if (pluginSelected || index + 1 == argc)
                    throw std::runtime_error("--title-plugin requires exactly one path");
                Oot3dNativeGame::ConfigureTitlePlugin(argv[++index]);
                pluginSelected = true;
            } else {
                runtimeArguments.push_back(argv[index]);
            }
        }
        argc = static_cast<int>(runtimeArguments.size());
        argv = runtimeArguments.data();
#endif
        if (!ParseOot3dNativeGameArgs(argc, argv, launch)) {
            PrintOot3dNativeGameUsage();
            return 2;
        }

#if defined(OOT3D_REQUIRE_WHOLE_AOT_PLUGIN_V2)
        if (!Oot3dNativeGame::Oot3dWholeAotPluginV2Available()) {
            throw std::runtime_error(
                "the packaged whole-AOT ABI-v2 title module is missing or incompatible "
                "(memory visibility ABI 2 required); install a matching runtime/title package");
        }
#endif
#if defined(__SWITCH__)
        WriteSwitchBootStatus("launching");
#endif
        ConfigurePicaAotShaders(launch);
#ifdef OOT3D_NATIVE_A32_WINDOW_AVAILABLE
        RunOot3dNativeA32Window(launch);
        ClearLaunchMarkerForDemo();
#if defined(__SWITCH__)
        // CPU High (L3+R3 > Video > Performance mode): the earlier rate back
        // while the game log still exists (its logger goes with the context).
        Oot3dNativeGame::RestorePerformanceForExit();
#endif
        // The A32 window owns objects that refer back to the host context.
        // Destroy those locals on return before tearing the context down.
        DestroyContextForDemo();
#if defined(__SWITCH__)
        WriteSwitchBootStatus("completed");
        // Exit game (L3+R3) ends the application instead of returning to the
        // homebrew loader: on return hbl maps the next NRO into memory this
        // process's services still hold and aborts (hbl + 0x3810, 2001-0106
        // InvalidCurrentMemory). svcExitProcess (1.0.0-test.2) ended the process
        // behind AM's back and the console still reported a crash on exit. With
        // exit mode 1, libnx's exit sends AM ISelfController::Exit
        // (_appletSelfExit), the way an application closes itself.
        if (Fast::Oot3d::ExitGameRequested())
            __nx_applet_exit_mode = 1;
#endif
        return 0;
#else
        throw std::runtime_error(
            "native A32 window runtime is unavailable in this build");
#endif
    } catch (const std::exception& ex) {
        std::cerr << "oot3d_native_game: " << ex.what() << '\n';
#if defined(__SWITCH__)
        WriteSwitchBootStatus("failed", ex.what());
#endif
        LogFatalErrorForDemo(ex.what());
#if defined(__SWITCH__)
        Oot3dNativeGame::RestorePerformanceForExit();
#endif
        DestroyContextForDemo();
#if defined(__SWITCH__)
        ShowSwitchFatalError(ex.what());
        // Like Exit game: returning to the homebrew loader makes it abort
        // (hbl + 0x3810, 2001-0106), so the application closes through AM.
        __nx_applet_exit_mode = 1;
#endif
        return 1;
    }
}
