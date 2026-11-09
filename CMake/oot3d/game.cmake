# The game: the native game runtime, the oot3d_native_game executable (TriAevum) and its link options,
# crash diagnostics, the CPU phase probe option and the Vulkan runtime deployment.
# Included from the root CMakeLists.txt, in its scope.

add_library(oot3d_native_game_runtime STATIC EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_a32_scene_view.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_timing_probe.cpp
    tools/oot3d/native_game_runtime/oot3d_native_scenario_bootstrap.cpp
    tools/oot3d/native_game_runtime/oot3d_native_frame_rate.cpp
    tools/oot3d/native_game_runtime/oot3d_native_presentation_pacer.cpp
    # Con stubs para no-Switch; el destructor lo exige el enlace de PC.
    tools/oot3d/native_game_runtime/oot3d_switch_vsync_clock.cpp
    tools/oot3d/native_game_runtime/oot3d_native_temporal_event_ledger.cpp
    tools/oot3d/native_game_runtime/oot3d_native_player_temporal_bridge.cpp
    tools/oot3d/native_game_runtime/oot3d_native_root_motion.cpp
    tools/oot3d/native_game_runtime/oot3d_native_skel_anime.cpp
)
target_include_directories(oot3d_native_game_runtime PUBLIC
    tools/oot3d/decomp_support/include
    tools/oot3d/native_demo_host
    tools/oot3d/native_game_runtime
)
target_compile_options(oot3d_native_game_runtime PRIVATE "${WARNING_OVERRIDE}")
target_link_libraries(oot3d_native_game_runtime PUBLIC
    oot3d_native_audio
    oot3d_native_a32_process
)
if (TARGET oot3d_native_a32_aot)
    target_link_libraries(oot3d_native_game_runtime PUBLIC oot3d_native_a32_aot)
    target_compile_definitions(oot3d_native_game_runtime PRIVATE
        OOT3D_NATIVE_A32_AOT_AVAILABLE=1)
endif()
oot3d_target_defaults(oot3d_native_game_runtime NO_WARNING_OVERRIDE FOLDER Tools)

add_executable(oot3d_native_game_runtime_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_frame_rate_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_temporal_event_ledger_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_player_temporal_bridge_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_scene_view_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_timing_probe_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_root_motion_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_skel_anime_tests.cpp
)
target_link_libraries(oot3d_native_game_runtime_tests PRIVATE oot3d_native_game_runtime)
oot3d_target_defaults(oot3d_native_game_runtime_tests FOLDER Tests)

# Clock verification must also work without the optional legacy cutscene host.
add_executable(triaevum_presentation_clock_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_frame_rate_tests.cpp
    tools/triaevum_release/tests/presentation_clock_main.cpp)
target_link_libraries(triaevum_presentation_clock_tests PRIVATE oot3d_native_game_runtime)
# oot3d_native_launch_frame_rate.h reads the FrameRate modes of the graphics settings.
target_include_directories(triaevum_presentation_clock_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/three_ds_recomp/include)
oot3d_target_defaults(triaevum_presentation_clock_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Paced 60 Hz interpolation (header-only governor).
add_executable(oot3d_native_interpolation_pacing_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_interpolation_pacing_tests.cpp
)
target_compile_features(oot3d_native_interpolation_pacing_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_native_interpolation_pacing_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Dynamic resolution of the top-screen scene (config/dynamic_resolution;
# header-only controller).
add_executable(oot3d_native_dynamic_resolution_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_dynamic_resolution_tests.cpp
)
target_compile_features(oot3d_native_dynamic_resolution_tests PRIVATE cxx_std_20)
target_include_directories(oot3d_native_dynamic_resolution_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/three_ds_recomp/include)
oot3d_target_defaults(oot3d_native_dynamic_resolution_tests NO_WARNING_OVERRIDE FOLDER Tests)

# deko3d backend, pure parts (config/renderer selection; header-only).
add_executable(oot3d_deko3d_backend_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_deko3d_backend_tests.cpp
)
target_compile_features(oot3d_deko3d_backend_tests PRIVATE cxx_std_20)
target_include_directories(oot3d_deko3d_backend_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/three_ds_recomp/include)
oot3d_target_defaults(oot3d_deko3d_backend_tests NO_WARNING_OVERRIDE FOLDER Tests)

# config/deko3d_clear_draw: the depth reset's proofs (header-only;
# tests/switch/test_clear_draw_tev.py runs its --emit output through glsl_eval).
add_executable(oot3d_deko3d_clear_draw_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_deko3d_clear_draw_tests.cpp
)
target_compile_features(oot3d_deko3d_clear_draw_tests PRIVATE cxx_std_20)
target_include_directories(oot3d_deko3d_clear_draw_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/three_ds_recomp/include
    ${CMAKE_SOURCE_DIR}/tools/oot3d/native_pica_frontend)
oot3d_target_defaults(oot3d_deko3d_clear_draw_tests NO_WARNING_OVERRIDE FOLDER Tests)

# deko3d toon / per-pixel lighting variants (config/lighting; pure parts).
add_executable(oot3d_deko3d_lighting_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_deko3d_lighting_tests.cpp
    runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting.cpp
    runtime/three_ds_recomp/src/fast/oot3d/pica_toon_shader.cpp
)
target_compile_features(oot3d_deko3d_lighting_tests PRIVATE cxx_std_20)
target_link_libraries(oot3d_deko3d_lighting_tests PRIVATE ThreeDsRecomp::Renderer3dsPicaCore)
oot3d_target_defaults(oot3d_deko3d_lighting_tests NO_WARNING_OVERRIDE FOLDER Tests)

# On-screen FPS counter (config/fps_counter; header-only).
add_executable(oot3d_fps_counter_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_fps_counter_tests.cpp
)
target_compile_features(oot3d_fps_counter_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_fps_counter_tests NO_WARNING_OVERRIDE FOLDER Tests)

# L3+R3 > Video > Performance mode: files, apm configurations, restore rules
# and the CPU clock keeper (oot3d_switch_performance.h; header-only).
add_executable(oot3d_switch_performance_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_switch_performance_tests.cpp
)
target_compile_features(oot3d_switch_performance_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_switch_performance_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Game log lines kept until the logger exists (oot3d_game_log.h; header-only).
add_executable(oot3d_game_log_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_game_log_tests.cpp
)
target_compile_features(oot3d_game_log_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_game_log_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Landing roll (N64) at the fall action's landing (config/no_landing_roll; header-only).
add_executable(oot3d_landing_roll_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_landing_roll_tests.cpp
)
target_compile_features(oot3d_landing_roll_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_landing_roll_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Texture pack size cap, mipmaps and the HD textures setting (pure parts).
add_executable(oot3d_texture_pack_scale_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_texture_pack_scale_tests.cpp
    runtime/three_ds_recomp/src/fast/oot3d/azahar_texture_scale.cpp
)
target_compile_features(oot3d_texture_pack_scale_tests PRIVATE cxx_std_20)
target_include_directories(oot3d_texture_pack_scale_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/three_ds_recomp/include)
# ConfigWriter (fast/oot3d/config_writer.h) runs a thread.
target_link_libraries(oot3d_texture_pack_scale_tests PRIVATE Threads::Threads)
oot3d_target_defaults(oot3d_texture_pack_scale_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Late-presentation log (logs/hitches.txt; header-only).
add_executable(oot3d_hitch_recorder_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_hitch_recorder_tests.cpp
)
target_compile_features(oot3d_hitch_recorder_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_hitch_recorder_tests NO_WARNING_OVERRIDE FOLDER Tests)

# boot-status.txt's heartbeat through a service thread
# (config/async_status_writes; header-only).
add_executable(oot3d_native_status_file_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_status_file_tests.cpp
)
target_compile_features(oot3d_native_status_file_tests PRIVATE cxx_std_20)
target_link_libraries(oot3d_native_status_file_tests PRIVATE Threads::Threads)
oot3d_target_defaults(oot3d_native_status_file_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Console log diagnostics: area ids (hitches.txt, perf.txt), the actor cost
# profile (config/actor_profile) and the indirect call profile
# (config/indirect_profile, the PGO census; recorded from several threads);
# header-only. The name tables are checked against the decomp's actor ids and
# scene_lights.py's scene table.
add_executable(oot3d_native_perf_diagnostics_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_perf_diagnostics_tests.cpp
)
target_compile_features(oot3d_native_perf_diagnostics_tests PRIVATE cxx_std_20)
target_compile_definitions(oot3d_native_perf_diagnostics_tests PRIVATE
    OOT3D_ACTOR_SEMANTICS_HEADER="${CMAKE_SOURCE_DIR}/tools/oot3d/decomp_support/include/oot3d/actor_object_semantics.h"
    OOT3D_SCENE_LIGHTS_SCRIPT="${CMAKE_SOURCE_DIR}/tools/triaevum_release/scene_lights.py")
target_link_libraries(oot3d_native_perf_diagnostics_tests PRIVATE Threads::Threads)
oot3d_target_defaults(oot3d_native_perf_diagnostics_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Guest fault report in the game log (registers, backtrace, memory around
# pointers, area ids, last perf windows); header-only.
add_executable(oot3d_native_guest_fault_report_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_guest_fault_report_tests.cpp
)
target_compile_features(oot3d_native_guest_fault_report_tests PRIVATE cxx_std_20)
target_include_directories(oot3d_native_guest_fault_report_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/oot3d/native_a32_runtime/upstream)
oot3d_target_defaults(oot3d_native_guest_fault_report_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Guest spike profile (config/guest_spike_ms, logs/guest_spikes.txt;
# header-only).
add_executable(oot3d_guest_spike_profile_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_guest_spike_profile_tests.cpp
)
target_compile_features(oot3d_guest_spike_profile_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_guest_spike_profile_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Guest block profile (config/guest_profile): reports from its writer thread,
# never holding the game thread.
add_executable(oot3d_native_guest_profile_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_guest_profile_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_guest_profile.cpp
)
target_compile_features(oot3d_native_guest_profile_tests PRIVATE cxx_std_20)
target_include_directories(oot3d_native_guest_profile_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/oot3d/native_a32_runtime/upstream)
target_link_libraries(oot3d_native_guest_profile_tests PRIVATE Threads::Threads)
oot3d_target_defaults(oot3d_native_guest_profile_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Frame spikes (logs/spikes.txt), vsync clock and the vsync-anchored pacer
# (config/frame_spikes, config/pacer_vsync; --report prints the drift table).
add_executable(oot3d_frame_spike_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_frame_spike_tests.cpp
    tools/oot3d/native_game_runtime/oot3d_native_presentation_pacer.cpp
    tools/oot3d/native_game_runtime/oot3d_switch_vsync_clock.cpp
)
target_compile_features(oot3d_frame_spike_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_frame_spike_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Presentation cadence of fps60 (header-only governor and pipeline simulation;
# --report prints the before/after table).
add_executable(oot3d_native_cadence_governor_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_cadence_governor_tests.cpp
)
target_compile_features(oot3d_native_cadence_governor_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_native_cadence_governor_tests NO_WARNING_OVERRIDE FOLDER Tests)

# GPU frame time without the window acquire's idle gap (header-only).
add_executable(oot3d_native_gpu_frame_timer_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_gpu_frame_timer_tests.cpp
)
target_compile_features(oot3d_native_gpu_frame_timer_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_native_gpu_frame_timer_tests NO_WARNING_OVERRIDE FOLDER Tests)

# config/skip_bottom_screen policy (header-only).
add_executable(oot3d_native_bottom_screen_skip_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_bottom_screen_skip_tests.cpp
)
target_compile_features(oot3d_native_bottom_screen_skip_tests PRIVATE cxx_std_20)
oot3d_target_defaults(oot3d_native_bottom_screen_skip_tests NO_WARNING_OVERRIDE FOLDER Tests)

add_executable(oot3d_native_audio_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_audio_tests.cpp
)
target_link_libraries(oot3d_native_audio_tests PRIVATE oot3d_native_audio)
oot3d_target_defaults(oot3d_native_audio_tests FOLDER Tests)

if(ANDROID)
    add_library(oot3d_native_game SHARED EXCLUDE_FROM_ALL)
else()
    add_executable(oot3d_native_game EXCLUDE_FROM_ALL)
endif()
target_sources(oot3d_native_game PRIVATE
    tools/oot3d/native_game_runtime/oot3d_native_game.cpp
    tools/oot3d/native_game_runtime/triaevum_product_info.cpp
    tools/oot3d/native_game_runtime/oot3d_native_game_bootstrap.cpp
    tools/oot3d/native_game_runtime/oot3d_native_game_launch_profile.cpp
)
if(WIN32)
    target_sources(oot3d_native_game PRIVATE
        tools/oot3d/native_game_runtime/triaevum_windows.manifest)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Relocated packages must resolve the bootstrap library beside the runtime,
    # never accidentally fall back to the publisher's build directory.
    set_target_properties(oot3d_native_game PROPERTIES
        BUILD_RPATH_USE_ORIGIN TRUE
        BUILD_RPATH "$ORIGIN;$ORIGIN/lib"
        INSTALL_RPATH "$ORIGIN;$ORIGIN/lib")
endif()
set(TRIAEVUM_PRODUCT_PLUGIN 0)
if(OOT3D_DIRECT_AOT_PLUGIN)
    set(TRIAEVUM_PRODUCT_PLUGIN 1)
    set_property(TARGET oot3d_native_game PROPERTY OUTPUT_NAME TriAevum)
    add_custom_target(triaevum_public_runtime
        DEPENDS oot3d_native_game triaevum_title_aot_stub)
    # The separately licensed iMMERSE module, a library of its own beside the
    # runtime (runtime/three_ds_recomp/src/fast/immerse/LICENSE.md).
    set(_triaevum_immerse_target "")
    if(TARGET triaevum_immerse)
        add_dependencies(triaevum_public_runtime triaevum_immerse)
        set(_triaevum_immerse_target ",\n  \"immerse\": \"$<TARGET_FILE:triaevum_immerse>\"")
    endif()
    # The separately licensed DLSS module with NVIDIA's DLSS runtime and the
    # NGX SDK licence (runtime/three_ds_recomp/src/fast/dlss/LICENSE.md), and
    # FidelityFX's FSR runtime (MIT), each beside the runtime.
    if(TARGET triaevum_dlss AND THREE_DS_RECOMP_DLSS_VK_DLL)
        add_dependencies(triaevum_public_runtime triaevum_dlss)
        string(APPEND _triaevum_immerse_target
            ",\n  \"dlss\": \"$<TARGET_FILE:triaevum_dlss>\""
            ",\n  \"dlss_runtime\": \"${THREE_DS_RECOMP_DLSS_VK_DLL}\""
            ",\n  \"dlss_license\": \"${THREE_DS_RECOMP_NGX_SOURCE_DIR}/LICENSE.txt\"")
    endif()
    if(THREE_DS_RECOMP_FFX_VK_DLL)
        string(APPEND _triaevum_immerse_target
            ",\n  \"fidelityfx_runtime\": \"${THREE_DS_RECOMP_FFX_VK_DLL}\"")
    endif()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/triaevum-runtime-targets-$<CONFIG>.json"
        CONTENT "{\n  \"format\": \"triaevum_runtime_targets_v1\",\n  \"source_commit\": \"${GIT_COMMIT_HASH}\",\n  \"runtime\": \"$<TARGET_FILE:oot3d_native_game>\",\n  \"stub\": \"$<TARGET_FILE:triaevum_title_aot_stub>\"${_triaevum_immerse_target}\n}\n")
endif()
set(TRIAEVUM_PRODUCT_NRI 0)
if(THREE_DS_RECOMP_ENABLE_NRI AND OOT3D_ENABLE_VULKAN_RENDERER)
    set(TRIAEVUM_PRODUCT_NRI 1)
endif()
# Reported so the release workflow can refuse a runtime that offers the
# experimental global illumination modes.
set(TRIAEVUM_PRODUCT_EXPERIMENTAL_GI 0)
if(THREE_DS_RECOMP_OFFER_EXPERIMENTAL_GI)
    set(TRIAEVUM_PRODUCT_EXPERIMENTAL_GI 1)
endif()
# Reported so the release workflow can refuse a runtime with developer tools.
set(TRIAEVUM_PRODUCT_DEVELOPER_TOOLS 0)
if(TRIAEVUM_DEVELOPER_TOOLS)
    set(TRIAEVUM_PRODUCT_DEVELOPER_TOOLS 1)
endif()
configure_file(
    tools/oot3d/native_game_runtime/triaevum_product_build.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/triaevum-product/triaevum_product_build.h" @ONLY)
target_include_directories(oot3d_native_game PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/triaevum-product")
get_filename_component(OOT3D_NATIVE_GAME_WORKSPACE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
configure_file(
    config/oot3d_native_game.launch.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/oot3d_native_game.launch.json"
    @ONLY)
unset(OOT3D_NATIVE_GAME_WORKSPACE_ROOT)

add_executable(oot3d_native_game_launch_profile_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_game_launch_profile.cpp
    tools/oot3d/native_game_runtime/oot3d_native_game_launch_profile_tests.cpp
)
target_include_directories(oot3d_native_game_launch_profile_tests PRIVATE
    tools/oot3d/native_game_runtime)
target_link_libraries(oot3d_native_game_launch_profile_tests PRIVATE
    nlohmann_json::nlohmann_json)
target_compile_options(oot3d_native_game_launch_profile_tests PRIVATE
    "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(oot3d_native_game_launch_profile_tests PRIVATE
        NOMINMAX)
    set_property(TARGET oot3d_native_game_launch_profile_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_native_game_launch_profile_tests PROPERTY
    FOLDER Tests)

add_executable(oot3d_graphics_settings_schedule_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_graphics_settings_schedule.cpp
    tools/oot3d/native_game_runtime/oot3d_graphics_settings_schedule_tests.cpp
)
target_include_directories(oot3d_graphics_settings_schedule_tests PRIVATE
    tools/oot3d/native_game_runtime)
target_link_libraries(oot3d_graphics_settings_schedule_tests PRIVATE
    nlohmann_json::nlohmann_json)
target_compile_options(oot3d_graphics_settings_schedule_tests PRIVATE
    "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_property(TARGET oot3d_graphics_settings_schedule_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_graphics_settings_schedule_tests PROPERTY
    FOLDER Tests)

# Grass on the Switch (SWITCH_PORT.md, "Grass on deko3d"): density thinning,
# the profile's budgets and config/grass, culling and the instance stream,
# and the preset's rules on twin grounds from the first frame.
set(_oot3d_grass_runtime "${CMAKE_CURRENT_SOURCE_DIR}/runtime/three_ds_recomp")
add_executable(oot3d_grass_switch_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_grass_switch_tests.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_async_placement_builder.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_instance_stream.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_persistent_placement_cache.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_placement_cache.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_shadow_refinement.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_world_placement_cache.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_overlay_mask.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_surface_extractor.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_switch_profile.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_texture_source_cache.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_texture_twin_table.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_texture_twins.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/grass_visibility.cpp
    ${_oot3d_grass_runtime}/src/fast/oot3d/texture_preview_artifact.cpp
)
target_include_directories(oot3d_grass_switch_tests PRIVATE
    tools/oot3d/native_game_runtime ${_oot3d_grass_runtime}/include)
target_compile_features(oot3d_grass_switch_tests PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(oot3d_grass_switch_tests PRIVATE Threads::Threads
    nlohmann_json::nlohmann_json)
set_property(TARGET oot3d_grass_switch_tests PROPERTY FOLDER Tests)

add_executable(oot3d_actor_visibility_policy_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_actor_visibility_policy.cpp
    tools/oot3d/native_game_runtime/oot3d_actor_visibility_policy_tests.cpp
)
target_compile_options(oot3d_actor_visibility_policy_tests PRIVATE
    "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_property(TARGET oot3d_actor_visibility_policy_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_actor_visibility_policy_tests PROPERTY
    FOLDER Tests)

add_executable(oot3d_native_a32_actor_visibility_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_actor_visibility_policy.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_actor_visibility.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_actor_visibility_tests.cpp
    runtime/three_ds_recomp/src/fast/oot3d/perspective_fov_policy.cpp
)
target_include_directories(oot3d_native_a32_actor_visibility_tests PRIVATE
    runtime/three_ds_recomp/include)
target_link_libraries(oot3d_native_a32_actor_visibility_tests PRIVATE
    oot3d_native_a32_process)
target_compile_options(oot3d_native_a32_actor_visibility_tests PRIVATE
    "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_property(TARGET oot3d_native_a32_actor_visibility_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_native_a32_actor_visibility_tests PROPERTY FOLDER Tests)

# The block-entry hook list (oot3d_native_block_entry_hooks.h) with the
# sources that define its PCs: the census tool, which writes and checks
# tools/oot3d/native_a32_runtime/whole_aot_hook_gates.json (the gate list of
# --sparse-block-entries; tests/switch/run.sh runs --check), and its tests.
set(OOT3D_BLOCK_ENTRY_HOOK_SOURCES
    tools/oot3d/native_game_runtime/oot3d_native_block_entry_hooks.cpp
    tools/oot3d/native_game_runtime/oot3d_actor_visibility_policy.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_actor_visibility.cpp
    tools/oot3d/native_game_runtime/oot3d_top_screen_wide_items_runtime.cpp
    runtime/three_ds_recomp/src/fast/oot3d/perspective_fov_policy.cpp
)
add_executable(oot3d_block_entry_hook_census EXCLUDE_FROM_ALL
    ${OOT3D_BLOCK_ENTRY_HOOK_SOURCES}
    tools/oot3d/native_game_runtime/oot3d_block_entry_hook_census.cpp)
add_executable(oot3d_native_block_entry_hooks_tests EXCLUDE_FROM_ALL
    ${OOT3D_BLOCK_ENTRY_HOOK_SOURCES}
    tools/oot3d/native_game_runtime/oot3d_native_block_entry_hooks_tests.cpp)
foreach(target oot3d_block_entry_hook_census oot3d_native_block_entry_hooks_tests)
    target_include_directories(${target} PRIVATE
        runtime/three_ds_recomp/include
        tools/oot3d/native_game_runtime
        tools/oot3d/ui_topscreen)
    target_link_libraries(${target} PRIVATE
        oot3d_native_a32_process
        oot3d_native_game_runtime
        oot3d_native_pica_composition
        oot3d_native_ui_lifecycle_bridge
        oot3d_ui_topscreen)
    target_compile_options(${target} PRIVATE "${WARNING_OVERRIDE}")
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(${target} PRIVATE NOMINMAX)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    set_property(TARGET ${target} PROPERTY FOLDER Tests)
endforeach()

add_library(oot3d_native_crash_diagnostics STATIC EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_crash_diagnostics.cpp
)
target_include_directories(oot3d_native_crash_diagnostics PUBLIC
    tools/oot3d/native_game_runtime
)
target_compile_options(oot3d_native_crash_diagnostics PRIVATE
    "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(oot3d_native_crash_diagnostics PRIVATE NOMINMAX)
    target_link_libraries(oot3d_native_crash_diagnostics PUBLIC dbghelp.lib)
    set_property(TARGET oot3d_native_crash_diagnostics PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_native_crash_diagnostics PROPERTY FOLDER Tools)

add_executable(oot3d_native_crash_diagnostics_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_crash_diagnostics_tests.cpp
)
target_link_libraries(oot3d_native_crash_diagnostics_tests PRIVATE
    oot3d_native_crash_diagnostics)
target_compile_options(oot3d_native_crash_diagnostics_tests PRIVATE
    "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(oot3d_native_crash_diagnostics_tests PRIVATE
        NOMINMAX)
    set_property(TARGET oot3d_native_crash_diagnostics_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_native_crash_diagnostics_tests PROPERTY FOLDER Tools)
target_include_directories(oot3d_native_game PRIVATE
    tools/oot3d/native_demo_host
    tools/oot3d/native_game_runtime
    tools/oot3d/ui_topscreen
)
target_link_libraries(oot3d_native_game PRIVATE
    oot3d_native_application_host
    oot3d_native_game_runtime
    oot3d_native_a32_savestate
    oot3d_native_control_config
    oot3d_ui_topscreen)
target_sources(oot3d_native_game PRIVATE tools/oot3d/native_game_runtime/oot3d_game_language.cpp)
if (OOT3D_WHOLE_AOT_GCOV_GENERATE)
    # GCC profile-generate build of the whole-AOT shards (native_runtime.cmake):
    # create the profile directory and flush counters while playing.
    target_compile_definitions(oot3d_native_game PRIVATE
        OOT3D_WHOLE_AOT_GCOV_GENERATE=1
        "OOT3D_WHOLE_AOT_GCOV_DIR=\"${OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR}\"")
endif()
if (TARGET oot3d_native_a32_aot OR TARGET oot3d_native_whole_aot OR
    OOT3D_DIRECT_AOT_PLUGIN)
    target_sources(oot3d_native_game PRIVATE
        tools/oot3d/native_game_runtime/oot3d_native_a32_window.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp
        tools/oot3d/native_game_runtime/oot3d_native_block_entry_hooks.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp
        tools/oot3d/native_game_runtime/oot3d_native_guest_profile.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_window_savestate.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_candidate_dispatch.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_projection_policy.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_window_input.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_window_summary.cpp
        tools/oot3d/native_game_runtime/oot3d_native_preparation_screen.cpp
        tools/oot3d/native_game_runtime/oot3d_native_render_command_stream.cpp
        tools/oot3d/native_game_runtime/oot3d_hang_watchdog.cpp
        tools/oot3d/native_game_runtime/oot3d_heap_bin_check.cpp
        tools/oot3d/native_game_runtime/oot3d_top_screen_ocarina_text_runtime.cpp
        tools/oot3d/native_game_runtime/oot3d_top_screen_pause_transaction.cpp
        tools/oot3d/native_game_runtime/oot3d_top_screen_item_description_runtime.cpp
        tools/oot3d/native_game_runtime/oot3d_top_screen_wide_items_runtime.cpp
        tools/oot3d/native_game_runtime/oot3d_native_actor_interactions.cpp
        tools/oot3d/native_game_runtime/oot3d_actor_visibility_policy.cpp
        tools/oot3d/native_game_runtime/oot3d_native_a32_actor_visibility.cpp
        tools/oot3d/native_game_runtime/oot3d_native_pcm_diagnostics.cpp
        tools/oot3d/native_game_runtime/oot3d_native_controls_settings_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_game_language_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_gameplay_settings_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_video_settings_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_savestate_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_savestate_settings.cpp
        tools/oot3d/native_game_runtime/oot3d_scene_environment.cpp
        tools/oot3d/native_game_runtime/oot3d_scene_environment_blend.cpp
        tools/oot3d/native_game_runtime/oot3d_light_gizmo.cpp
        tools/oot3d/native_game_runtime/oot3d_scene_environment_codec.cpp
        tools/oot3d/native_game_runtime/oot3d_scene_environment_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_top_screen_settings_panel.cpp
        tools/oot3d/native_game_runtime/oot3d_graphics_settings_schedule.cpp)
    set_source_files_properties(tools/oot3d/native_game_runtime/oot3d_native_actor_interactions.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/decomp_support/include")
    # Developer tools (F1 > Dev Tools: SoH's Debug Warp Screen and Warp
    # Points). Never in a public build: --product-info reports the capability
    # and the release workflow refuses a runtime that has it.
    if(TRIAEVUM_DEVELOPER_TOOLS)
        target_sources(oot3d_native_game PRIVATE
            tools/oot3d/native_game_runtime/oot3d_dev_warp.cpp
            tools/oot3d/native_game_runtime/oot3d_dev_tools_panel.cpp
            tools/oot3d/native_game_runtime/oot3d_dev_warp_process.cpp
            tools/oot3d/native_game_runtime/oot3d_dev_emitter_inventory.cpp
            tools/oot3d/native_game_runtime/oot3d_dev_lab.cpp)
        set_source_files_properties(tools/oot3d/native_game_runtime/oot3d_dev_warp.cpp
            tools/oot3d/native_game_runtime/oot3d_dev_emitter_inventory.cpp
            PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/decomp_support/include")
        target_compile_definitions(oot3d_native_game PRIVATE TRIAEVUM_DEVELOPER_TOOLS=1)
        add_executable(oot3d_dev_warp_tests EXCLUDE_FROM_ALL
            tools/oot3d/native_game_runtime/oot3d_dev_warp.cpp
            tools/oot3d/native_game_runtime/oot3d_dev_warp_tests.cpp)
        target_include_directories(oot3d_dev_warp_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_game_runtime"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/decomp_support/include")
        target_link_libraries(oot3d_dev_warp_tests PRIVATE oot3d_native_game_runtime)
        oot3d_target_defaults(oot3d_dev_warp_tests FOLDER Tests)
        add_executable(oot3d_video_settings_tests EXCLUDE_FROM_ALL
            tools/oot3d/native_game_runtime/oot3d_video_settings_tests.cpp)
        oot3d_target_defaults(oot3d_video_settings_tests FOLDER Tests)
        # Scene environment profiles for agents and scripts (validate, format,
        # explain), without starting the game.
        add_executable(triaevum_environment_tool EXCLUDE_FROM_ALL
            tools/oot3d/native_game_runtime/oot3d_environment_tool.cpp
            tools/oot3d/native_game_runtime/oot3d_interior_sun_prebuild.cpp
            tools/oot3d/native_game_runtime/oot3d_scene_environment_codec.cpp)
        target_include_directories(triaevum_environment_tool PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_game_runtime")
        target_link_libraries(triaevum_environment_tool PRIVATE three_ds_recomp_runtime oot3d_native_pica_composition)
        oot3d_target_defaults(triaevum_environment_tool FOLDER Tools)
        # The scene environment runtime's developer hot reload.
        add_executable(triaevum_environment_tests EXCLUDE_FROM_ALL
            tools/oot3d/native_game_runtime/oot3d_scene_environment_tests.cpp
            tools/oot3d/native_game_runtime/oot3d_scene_environment.cpp
            tools/oot3d/native_game_runtime/oot3d_scene_environment_blend.cpp
            tools/oot3d/native_game_runtime/oot3d_scene_environment_codec.cpp
            tools/oot3d/native_game_runtime/oot3d_light_gizmo.cpp)
        target_include_directories(triaevum_environment_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_game_runtime")
        target_compile_definitions(triaevum_environment_tests PRIVATE TRIAEVUM_DEVELOPER_TOOLS=1)
        target_link_libraries(triaevum_environment_tests PRIVATE three_ds_recomp_runtime)
        oot3d_target_defaults(triaevum_environment_tests FOLDER Tests)
    endif()
    target_link_libraries(oot3d_native_game PRIVATE
        oot3d_native_a32_input
        oot3d_native_a32_process_image
        oot3d_native_a32_ctr_host
        oot3d_native_ui_lifecycle_bridge
        oot3d_native_ui_texture_provider
        oot3d_ui_legacy_adapters
        oot3d_top_screen_texture_runtime
        oot3d_top_screen_runtime_consumer
        oot3d_native_pica_composition
        oot3d_native_pica_submission
        oot3d_native_pica_semantic_trace
        oot3d_native_pica_vulkan_bridge)
    if (TARGET oot3d_native_a32_aot)
        target_link_libraries(oot3d_native_game PRIVATE
            oot3d_native_a32_aot)
    endif()
    if (OOT3D_DIRECT_AOT_PLUGIN)
        target_link_libraries(oot3d_native_game PRIVATE
            triaevum_title_aot_stub)
        target_compile_definitions(oot3d_native_game PRIVATE
            OOT3D_REQUIRE_WHOLE_AOT_PLUGIN_V2=1
            OOT3D_NATIVE_DIRECT_AOT_PLUGIN=1)
    endif()
    target_compile_definitions(oot3d_native_game PRIVATE
        OOT3D_NATIVE_A32_WINDOW_AVAILABLE=1)
endif()
target_compile_options(oot3d_native_game PRIVATE "${WARNING_OVERRIDE}")
if (OOT3D_WHOLE_AOT_PRODUCT_MODE)
    target_link_libraries(oot3d_native_game PRIVATE
        oot3d_native_crash_diagnostics)
    target_compile_definitions(oot3d_native_game PRIVATE
        OOT3D_WHOLE_AOT_PRODUCT_MODE=1)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT OOT3D_FAST_DEV_LINK)
        set(_oot3d_whole_aot_symbol_dir "${CMAKE_CURRENT_BINARY_DIR}/symbols")
        file(MAKE_DIRECTORY "${_oot3d_whole_aot_symbol_dir}")
        # /DEBUG keeps unreferenced functions and data unless /OPT:REF is explicit. Identical-code folding
        # (/OPT:ICF) stays off: it can merge functions whose addresses the code compares.
        target_link_options(oot3d_native_game PRIVATE
            "/DEBUG:FULL"
            "/OPT:REF"
            "/PDB:${_oot3d_whole_aot_symbol_dir}/oot3d_native_game.pdb"
            "/MAP:${_oot3d_whole_aot_symbol_dir}/oot3d_native_game.map")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_options(oot3d_native_game PRIVATE
            "/DEBUG:NONE"
            "/INCREMENTAL:NO"
            "/OPT:NOREF"
            "/OPT:NOICF")
    endif()
endif()
option(TRIAEVUM_ENABLE_CPU_PHASE_PROBE
    "Developer-only Windows main-thread CPU/wall accounting (not clean performance timing)" OFF)
if(TRIAEVUM_ENABLE_CPU_PHASE_PROBE)
    foreach(_cpu_probe_target IN ITEMS oot3d_native_a32_process
            oot3d_native_a32_ctr_host oot3d_native_pica_submission
            oot3d_native_pica_vulkan_plan oot3d_native_game)
        target_compile_definitions(${_cpu_probe_target} PRIVATE TRIAEVUM_ENABLE_CPU_PHASE_PROBE=1)
    endforeach()
    unset(_cpu_probe_target)
endif()

set(_oot3d_vulkan_hosts
    triaevum_oot3d_module_host
    oot3d_native_game)
if(OOT3D_ENABLE_VULKAN_RENDERER AND TARGET three_ds_recomp_shaderc)
    if(TARGET oot3d_native_pica_default_pack)
        add_dependencies(oot3d_native_game
            oot3d_native_pica_default_pack)
    endif()
    foreach(oot3d_vulkan_host IN LISTS _oot3d_vulkan_hosts)
        add_custom_command(TARGET ${oot3d_vulkan_host} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:three_ds_recomp_shaderc>
                    $<TARGET_FILE_DIR:${oot3d_vulkan_host}>
            COMMENT "Deploying shaderc for ${oot3d_vulkan_host}"
        )
    endforeach()
endif()
if(COMMAND three_ds_recomp_deploy_nri_runtime)
    foreach(oot3d_vulkan_host IN LISTS _oot3d_vulkan_hosts)
        three_ds_recomp_deploy_nri_runtime(${oot3d_vulkan_host})
    endforeach()
endif()
unset(_oot3d_vulkan_hosts)
oot3d_target_defaults(oot3d_native_game NO_WARNING_OVERRIDE)
# Release identity (set by the release build, -DOOT3D_RELEASE_VERSION/BUILD): the
# version the NACP and the L3+R3 menu show and the build number the OTA
# updater compares (higher = newer, across channels).
set(OOT3D_RELEASE_VERSION "0.1.0-dev" CACHE STRING
    "Release version (semver; at most 15 characters, the NACP's limit)")
set(OOT3D_RELEASE_BUILD "0" CACHE STRING
    "Monotonic release build number (0 for development builds)")
string(LENGTH "${OOT3D_RELEASE_VERSION}" _oot3d_release_version_length)
if (NOT OOT3D_RELEASE_VERSION MATCHES
        "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$"
    OR _oot3d_release_version_length GREATER 15)
    message(FATAL_ERROR "OOT3D_RELEASE_VERSION '${OOT3D_RELEASE_VERSION}' is not a semver of at most 15 characters")
endif()
if (NOT OOT3D_RELEASE_BUILD MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "OOT3D_RELEASE_BUILD '${OOT3D_RELEASE_BUILD}' is not a build number")
endif()
unset(_oot3d_release_version_length)
if (CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # Core and priority placement for every pthread (see the source).
    target_sources(oot3d_native_game PRIVATE
        # The NRO's code and read-only data moved to 2 MB blocks by the first
        # constructor (config/no_code_hugepages turns it off).
        tools/oot3d/native_game_runtime/oot3d_switch_code_pages.cpp
        # PGO generate builds: streams the shards' counters to the SD card
        # (oot3d_whole_aot_gcov.h); empty functions otherwise.
        tools/oot3d/native_game_runtime/oot3d_whole_aot_pgo.cpp
        tools/oot3d/native_game_runtime/oot3d_switch_threads.cpp
        # Clocks for logs/perf.txt (sys-clk IPC, read only).
        tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp
        # L3+R3 > Video > Performance mode: GPU/memory through apm, CPU High
        # through clkrst (config/cpu_performance, gpu_performance,
        # memory_performance).
        tools/oot3d/native_game_runtime/oot3d_switch_performance.cpp
        # Display vsync clock (config/pacer_vsync, config/frame_spikes).
        tools/oot3d/native_game_runtime/oot3d_switch_vsync_clock.cpp
        # The console's six-axis sensors as controller motion (gyro aiming).
        tools/oot3d/native_game_runtime/oot3d_switch_motion.cpp
        # Guard pages for one malloc size range (config/guard_alloc) and the
        # libnx exception handler that reports faults to logs/hang.txt.
        tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp
        # Release identity, the OTA updater (L3+R3 > Updates) and the
        # start-up step that finishes an install.
        tools/oot3d/native_game_runtime/oot3d_build_version.cpp
        tools/oot3d/native_game_runtime/oot3d_update_release.cpp
        tools/oot3d/native_game_runtime/oot3d_switch_network.cpp
        tools/oot3d/native_game_runtime/oot3d_switch_updater.cpp
        tools/oot3d/native_game_runtime/oot3d_update_panel.cpp)
    # First-launch import of the player's decrypted dump into game/.
    target_sources(oot3d_native_game PRIVATE
        tools/oot3d/native_game_runtime/oot3d_rom_import.cpp
        tools/oot3d/native_game_runtime/oot3d_switch_rom_import.cpp)
    # TopScreen HUD textures: the mod downloaded from GameBanana and built
    # into config/atlas_overrides.o3tu at start-up (zlib reads the archive).
    target_sources(oot3d_native_game PRIVATE
        tools/oot3d/native_game_runtime/oot3d_topscreen_import.cpp
        tools/oot3d/native_game_runtime/oot3d_switch_topscreen_import.cpp)
    # Only this file sees the version: a new one recompiles one source.
    set_source_files_properties(tools/oot3d/native_game_runtime/oot3d_build_version.cpp
        PROPERTIES COMPILE_DEFINITIONS
        "OOT3D_RELEASE_VERSION=\"${OOT3D_RELEASE_VERSION}\";OOT3D_RELEASE_BUILD=${OOT3D_RELEASE_BUILD}")
    # libcurl (devkitPro's, on libnx's TLS backend) for the updater's and the
    # HUD texture download's HTTPS, mbedcrypto for the updater's SHA-256
    # checks and zlib for the mod archive.
    find_library(OOT3D_SWITCH_CURL_LIBRARY curl HINTS "${DEVKITPRO}/portlibs/switch/lib" REQUIRED)
    find_library(OOT3D_SWITCH_MBEDCRYPTO_LIBRARY mbedcrypto HINTS "${DEVKITPRO}/portlibs/switch/lib" REQUIRED)
    find_library(OOT3D_SWITCH_Z_LIBRARY z HINTS "${DEVKITPRO}/portlibs/switch/lib" REQUIRED)
    target_link_libraries(oot3d_native_game PRIVATE
        ${OOT3D_SWITCH_CURL_LIBRARY} ${OOT3D_SWITCH_MBEDCRYPTO_LIBRARY} ${OOT3D_SWITCH_Z_LIBRARY})
    target_link_options(oot3d_native_game PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=threadCreate
        -Wl,--wrap=SDL_SetThreadPriority
        -Wl,--wrap=_malloc_r -Wl,--wrap=_memalign_r -Wl,--wrap=_free_r -Wl,--wrap=_realloc_r
        -Wl,--wrap=_calloc_r -Wl,--wrap=_malloc_usable_size_r)
    nx_generate_nacp(oot3d_native_game.nacp
        NAME "The Legend of Zelda: Ocarina of Time 3D Recompiled"
        AUTHOR "OoT3D Recompiled contributors"
        VERSION "${OOT3D_RELEASE_VERSION}")
    nx_create_nro(oot3d_native_game
        NACP oot3d_native_game.nacp
        ICON ${CMAKE_CURRENT_SOURCE_DIR}/res/switch/icon.jpg)
endif()
set_property(TARGET oot3d_native_game PROPERTY FOLDER Tools)
