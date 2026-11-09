# Native runtime of the OoT3D executables: the application host and audio libraries, the A32 runtime and its
# whole-function, mass and true AOT variants, typed gameplay, the A32 process, and their tests.
# Included from the root CMakeLists.txt, in its scope.

add_library(oot3d_native_application_host STATIC EXCLUDE_FROM_ALL
    tools/oot3d/native_demo_host/oot3d_demo_host_context.cpp
    tools/oot3d/native_demo_host/oot3d_demo_host_io.cpp
    tools/oot3d/native_demo_host/oot3d_demo_host_screenshot.cpp
    tools/oot3d/native_demo_host/oot3d_demo_host_window_timing.cpp
)
target_include_directories(oot3d_native_application_host PUBLIC
    tools/oot3d/native_demo_host
    tools/oot3d/decomp_support/include
)
target_link_libraries(oot3d_native_application_host PUBLIC three_ds_recomp_runtime)
target_compile_options(oot3d_native_application_host PRIVATE
    "${WARNING_OVERRIDE}")
oot3d_target_defaults(oot3d_native_application_host NO_WARNING_OVERRIDE FOLDER Tools)

add_library(oot3d_native_audio STATIC EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_cseq.cpp
    tools/oot3d/native_game_runtime/oot3d_native_audio.cpp
    tools/oot3d/native_game_runtime/oot3d_native_audio_profiles.cpp
    tools/oot3d/native_game_runtime/oot3d_native_audio_mixer.cpp
    tools/oot3d/native_game_runtime/oot3d_native_audio_dsp.cpp
    tools/oot3d/native_game_runtime/oot3d_native_audio_resampler.cpp
    tools/oot3d/native_game_runtime/oot3d_host_audio_resampler.cpp
    tools/oot3d/native_game_runtime/oot3d_host_audio_stream.cpp
    tools/oot3d/native_game_runtime/oot3d_native_audio_service.cpp
)
target_include_directories(oot3d_native_audio PUBLIC
    tools/oot3d/native_game_runtime
)
oot3d_target_defaults(oot3d_native_audio FOLDER Tools)

set(OOT3D_PINNED_A32_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/upstream")
set(OOT3D_A32_AOT_GENERATED_DIR
    "${CMAKE_BINARY_DIR}/oot3d_a32_generated"
    CACHE PATH "Generated revision-pinned OoT3D A32 AOT shards")
set(OOT3D_WHOLE_AOT_GENERATED_DIR
    "${CMAKE_BINARY_DIR}/oot3d_whole_aot_cpp"
    CACHE PATH "Generated whole-function OoT3D A32 C++ AOT shards")
set(OOT3D_WHOLE_AOT_PROGRAM
    "${CMAKE_BINARY_DIR}/oot3d_whole_aot/aot_program.json"
    CACHE FILEPATH "Audited structural program consumed by whole-AOT codegen")
set(OOT3D_WHOLE_AOT_SELECTION
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/whole_aot_functions.json"
    CACHE FILEPATH "Byte-exact whole-AOT function selection")
set(OOT3D_WHOLE_AOT_CODE_BIN "" CACHE FILEPATH
    "Legally extracted code.bin used by the whole-AOT product audit")
set(OOT3D_WHOLE_AOT_EXHEADER "" CACHE FILEPATH
    "Legally extracted ExHeader used by the whole-AOT product audit")
set(OOT3D_WHOLE_AOT_PRODUCT_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/whole_aot_product_manifest.json"
    CACHE FILEPATH "Immutable whole-AOT product closure contract")
set(OOT3D_WHOLE_AOT_PREBUILT_LIBRARY
    "${CMAKE_BINARY_DIR}/oot3d_native_whole_aot.lib"
    CACHE FILEPATH "Prebuilt whole-function OoT3D A32 AOT archive")
set(OOT3D_WHOLE_AOT_THINLTO_CACHE_DIR "" CACHE PATH
    "Persistent lld ThinLTO cache for whole-AOT consumer links")
option(OOT3D_REBUILD_WHOLE_AOT
    "Compile all generated whole-function AOT shards (expensive)" OFF)
option(OOT3D_REGENERATE_WHOLE_AOT
    "Regenerate whole-AOT C++ before compiling the shards" ON)
option(OOT3D_REQUIRE_WHOLE_AOT
    "Fail configuration when the whole-function AOT backend is unavailable" OFF)
option(OOT3D_WHOLE_AOT_PRODUCT_MODE
    "Require audited zero-residual whole-AOT execution" OFF)
option(OOT3D_DIRECT_AOT_PLUGIN
    "Consume title code from the stable TriAevum direct-AOT plugin ABI" OFF)
set(OOT3D_WHOLE_AOT_MAX_PARALLEL_COMPILES "2" CACHE STRING
    "Maximum concurrent whole-AOT shard compilations")
set(OOT3D_WHOLE_AOT_SHARD_COUNT "256" CACHE STRING
    "Generated whole-AOT C++ shard count")
# affinity: distribution by call affinity, recomputed from scratch on every
# generation (one function more or less in the selection reshuffles ~30% of the shards).
# affinity-incremental: same distribution on the first generation; afterwards
# keeps the shard of each existing function (see _affinity_incremental_buckets),
# so ccache hits the untouched shards. The PGO .gcda files match the
# generated C++, so only switch the strategy before a profile training run.
set(OOT3D_WHOLE_AOT_SHARD_STRATEGY "affinity" CACHE STRING
    "Whole-AOT C++ shard strategy (affinity or affinity-incremental)")
set_property(CACHE OOT3D_WHOLE_AOT_SHARD_STRATEGY PROPERTY STRINGS
    affinity affinity-incremental)
# Shard compile time (measurements at the commit that created these options): the giant
# guest functions spend 50-65% of the time in sched1 and 15-45% in the RTL PRE/gcse.
# 0 / empty = previous behavior (identical C++ and flags).
# PGO (GCC): the attribute changes neither the profile_id (mangled name), nor the
# line checksum nor the CFG one of any function (compared in the .gcno/__gcov* of
# shards 003 and 007), so the .gcda files are valid with or without it. In the
# "generate" build all the giants may be marked (doesn't affect the final binary). In
# "use", -fprofile-partial-training optimizes for speed the functions the
# collection didn't execute, so mark only the collection's cold ones: generate the list with
# whole_aot_hot_from_gcda.py from the .gcda files and pass it in
# OOT3D_WHOLE_AOT_HOT_FUNCTIONS (the executed functions come out byte-for-byte identical);
# left empty, a GCC "use" configure derives it from OOT3D_WHOLE_AOT_PROFILE_FILE.
set(OOT3D_WHOLE_AOT_GIANT_FUNCTION_INSTRUCTIONS "0" CACHE STRING
    "Marks functions with >= N guest instructions (except the hot ones) without sched1/gcse-after-reload; 0 turns it off")
set(OOT3D_WHOLE_AOT_HOT_FUNCTIONS "" CACHE FILEPATH
    "List of hot functions (whole_aot_hot_functions.py or whole_aot_hot_from_gcda.py) that never lose optimizations")
set(OOT3D_WHOLE_AOT_GCSE_MEMORY_KB "" CACHE STRING
    "--param=max-gcse-memory for the shards (GCC); only giant functions skip the RTL PRE; empty = GCC default")
option(OOT3D_WHOLE_AOT_THINLTO
    "Compile and link the whole-AOT archive with LLVM ThinLTO" OFF)
set(OOT3D_WHOLE_AOT_PROFILE_MODE "none" CACHE STRING
    "Whole-AOT profile mode: none, generate, use, or sample-use (LLVM); none, generate or use (GCC)")
set_property(CACHE OOT3D_WHOLE_AOT_PROFILE_MODE PROPERTY STRINGS
    none generate use sample-use)
set(OOT3D_WHOLE_AOT_PROFILE_FILE "" CACHE FILEPATH
    "LLVM indexed or sample profile consumed by whole-AOT profile-use; with GCC, the directory holding the .gcda files")
set(OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR "sdmc:/switch/oot3dre/pgo" CACHE STRING
    "GCC profile-generate: directory the instrumented program writes its .gcda files to")
# Compiles out the per-call statistics of the generated code (OOT3D_AOT_STAT:
# direct/indirect/resolved-indirect call counters, reported only in the host
# summary JSON). Guest state, scheduling, block budget and the boot-status
# block-limit counter are unaffected.
if (OOT3D_WHOLE_AOT_PRODUCT_MODE)
    set(_oot3d_whole_aot_lean_stats_default ON)
else()
    set(_oot3d_whole_aot_lean_stats_default OFF)
endif()
option(OOT3D_WHOLE_AOT_LEAN_STATS
    "Compile out whole-AOT per-call statistics (summary-only counters)"
    ${_oot3d_whole_aot_lean_stats_default})
option(OOT3D_WHOLE_AOT_NATIVE_VFP_AARCH64
    "Regenerate whole-AOT C++ with native AArch64 VFP (generate_whole_aot_cpp.py --native-vfp-aarch64)" OFF)
set(OOT3D_WHOLE_AOT_ORDER_FILE "" CACHE FILEPATH
    "Optional linker symbol order file for the final whole-AOT consumer")
if (OOT3D_WHOLE_AOT_PRODUCT_MODE AND NOT OOT3D_DIRECT_AOT_PLUGIN)
    set(OOT3D_REQUIRE_WHOLE_AOT ON CACHE BOOL
        "Fail configuration when the whole-function AOT backend is unavailable"
        FORCE)
endif()
set(OOT3D_MASS_AOT_GENERATED_DIR "" CACHE PATH
    "Generated mass-direct OoT3D C++ metadata directory")
set(OOT3D_MASS_AOT_PREBUILT_LIBRARY "" CACHE FILEPATH
    "Prebuilt mass-direct OoT3D C++ ThinLTO archive")
add_library(oot3d_native_a32_runtime STATIC EXCLUDE_FROM_ALL
    ${OOT3D_PINNED_A32_ROOT}/recomp/a32_core.cpp
    ${OOT3D_PINNED_A32_ROOT}/recomp/a32_core_alu.cpp
    ${OOT3D_PINNED_A32_ROOT}/recomp/a32_core_memory.cpp
    ${OOT3D_PINNED_A32_ROOT}/recomp/a32_runtime.cpp
    ${OOT3D_PINNED_A32_ROOT}/recomp/a32_vfp_binary64.cpp
    tools/oot3d/native_a32_runtime/mass_cpp/recomp/a32_direct_runtime.cpp
    tools/oot3d/native_a32_runtime/oot3d_native_a32_vfp_ops.cpp
    ${OOT3D_PINNED_A32_ROOT}/recomp/a32_vfp_transport.cpp
)
target_include_directories(oot3d_native_a32_runtime PUBLIC
    ${OOT3D_PINNED_A32_ROOT}
    ${OOT3D_PINNED_A32_ROOT}/recomp
    tools/oot3d/native_a32_runtime
    tools/oot3d/native_a32_runtime/mass_cpp
)
target_compile_options(oot3d_native_a32_runtime PRIVATE "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_options(oot3d_native_a32_runtime PRIVATE
        $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    target_compile_definitions(oot3d_native_a32_runtime PRIVATE NOMINMAX)
    set_property(TARGET oot3d_native_a32_runtime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_native_a32_runtime PROPERTY FOLDER Tools)

if(OOT3D_WHOLE_AOT_PRODUCT_MODE)
    # The generated product still uses the guest-state and exact VFP ABI as
    # data contracts.  Keep those helpers separate so the decoded A32 core,
    # direct runtime, and interpreter cannot enter a whole-AOT product link.
    add_library(oot3d_whole_aot_architecture_support STATIC EXCLUDE_FROM_ALL
        ${OOT3D_PINNED_A32_ROOT}/recomp/a32_vfp_binary64.cpp
        tools/oot3d/native_a32_runtime/oot3d_native_a32_vfp_ops.cpp
        ${OOT3D_PINNED_A32_ROOT}/recomp/a32_vfp_transport.cpp)
    target_include_directories(oot3d_whole_aot_architecture_support PUBLIC
        ${OOT3D_PINNED_A32_ROOT}
        ${OOT3D_PINNED_A32_ROOT}/recomp
        tools/oot3d/native_a32_runtime
        tools/oot3d/native_a32_runtime/mass_cpp)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(oot3d_whole_aot_architecture_support PRIVATE
            -O2 -fno-fast-math -ffp-contract=off -fno-rtti)
    endif()
    set_property(TARGET oot3d_whole_aot_architecture_support PROPERTY
        FOLDER Tools)

    # Title-neutral support linked into the private whole-AOT plugin produced
    # by Forge.  Keep this boundary narrow: generated title code needs only
    # the exact guest-memory and VFP semantics, never the host/game runtime.
    include(tools/triaevum_release/cmake/TitleSupport.cmake)
    triaevum_add_title_support("${CMAKE_CURRENT_SOURCE_DIR}" "${OOT3D_PINNED_A32_ROOT}")
    target_compile_options(triaevum_title_whole_aot_support PRIVATE
        "${WARNING_OVERRIDE}")
endif()

set(_oot3d_a32_generated_header
    "${OOT3D_A32_AOT_GENERATED_DIR}/oot3d_a32_generated.h")
set(_oot3d_a32_generated_registry
    "${OOT3D_A32_AOT_GENERATED_DIR}/registry.cpp")
set(_oot3d_a32_generated_true_aot_header
    "${OOT3D_A32_AOT_GENERATED_DIR}/oot3d_a32_true_aot_generated.h")
set(_oot3d_a32_generated_true_aot_source
    "${OOT3D_A32_AOT_GENERATED_DIR}/oot3d_a32_true_aot_generated.cpp")
if (EXISTS "${_oot3d_a32_generated_header}" AND
    EXISTS "${_oot3d_a32_generated_registry}" AND
    EXISTS "${_oot3d_a32_generated_true_aot_header}" AND
    EXISTS "${_oot3d_a32_generated_true_aot_source}")
    file(GLOB _oot3d_a32_generated_shards CONFIGURE_DEPENDS
        "${OOT3D_A32_AOT_GENERATED_DIR}/shard_*.cpp")
    add_library(oot3d_native_a32_aot STATIC EXCLUDE_FROM_ALL
        ${_oot3d_a32_generated_header}
        ${_oot3d_a32_generated_registry}
        ${_oot3d_a32_generated_true_aot_header}
        ${_oot3d_a32_generated_true_aot_source}
        ${_oot3d_a32_generated_shards}
    )
    target_include_directories(oot3d_native_a32_aot PUBLIC
        ${OOT3D_PINNED_A32_ROOT}
        ${OOT3D_A32_AOT_GENERATED_DIR}
    )
    target_link_libraries(oot3d_native_a32_aot PUBLIC
        oot3d_native_a32_runtime)
    oot3d_target_defaults(oot3d_native_a32_aot FOLDER Tools)
endif()

add_executable(oot3d_native_a32_runtime_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_a32_runtime/oot3d_native_a32_runtime_tests.cpp
)
target_link_libraries(oot3d_native_a32_runtime_tests PRIVATE
    oot3d_native_a32_runtime)
oot3d_target_defaults(oot3d_native_a32_runtime_tests FOLDER Tests)

add_executable(oot3d_native_a32_vfp_fast_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_a32_runtime/oot3d_native_a32_vfp_fast_tests.cpp
)
target_link_libraries(oot3d_native_a32_vfp_fast_tests PRIVATE
    oot3d_native_a32_runtime)
target_compile_options(oot3d_native_a32_vfp_fast_tests PRIVATE "${WARNING_OVERRIDE}"
    $<IF:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>,/fp:strict,-ffp-model=strict>)
oot3d_target_defaults(oot3d_native_a32_vfp_fast_tests NO_WARNING_OVERRIDE FOLDER Tests)

# Native AArch64 VFP (whole_aot_cpp.py --native-vfp-aarch64) against the
# software implementation. Meaningful on AArch64 hosts (or qemu-aarch64);
# elsewhere it checks the software fallbacks.
add_executable(oot3d_native_a32_vfp_aarch64_tests EXCLUDE_FROM_ALL
    tools/oot3d/native_a32_runtime/oot3d_native_a32_vfp_aarch64_tests.cpp
)
target_link_libraries(oot3d_native_a32_vfp_aarch64_tests PRIVATE
    oot3d_native_a32_runtime)
target_compile_options(oot3d_native_a32_vfp_aarch64_tests PRIVATE "${WARNING_OVERRIDE}"
    $<IF:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>,/fp:strict,-ffp-contract=off>)
oot3d_target_defaults(oot3d_native_a32_vfp_aarch64_tests NO_WARNING_OVERRIDE FOLDER Tests)

add_executable(pica_gi_geometry_tests EXCLUDE_FROM_ALL
    runtime/three_ds_recomp/tests/pica_gi_geometry_tests.cpp
    runtime/three_ds_recomp/src/fast/renderer3ds/pica_gi_geometry.cpp
)
target_include_directories(pica_gi_geometry_tests PRIVATE
    runtime/three_ds_recomp/include)
target_compile_features(pica_gi_geometry_tests PRIVATE cxx_std_20)
oot3d_target_defaults(pica_gi_geometry_tests NO_WARNING_OVERRIDE FOLDER Tests)

add_executable(pica_fragment_lighting_memo_tests EXCLUDE_FROM_ALL
    runtime/three_ds_recomp/tests/pica_fragment_lighting_memo_tests.cpp
    runtime/three_ds_recomp/src/fast/oot3d/pica_fragment_lighting.cpp
)
target_include_directories(pica_fragment_lighting_memo_tests PRIVATE
    runtime/three_ds_recomp/include)
target_compile_features(pica_fragment_lighting_memo_tests PRIVATE cxx_std_20)
oot3d_target_defaults(pica_fragment_lighting_memo_tests NO_WARNING_OVERRIDE FOLDER Tests)

if (NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json REQUIRED)
endif()

add_library(oot3d_typed_gameplay STATIC EXCLUDE_FROM_ALL
    tools/oot3d/typed_gameplay/oot3d_gameplay_actor.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_animation.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_camera.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_camera_animation.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_cutscene.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_math.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_player.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_quake.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_skel_anime.cpp
    tools/oot3d/typed_gameplay/oot3d_gameplay_time.cpp
)
target_include_directories(oot3d_typed_gameplay PUBLIC
    tools/oot3d/typed_gameplay
)
oot3d_target_defaults(oot3d_typed_gameplay FOLDER Tools)

add_executable(oot3d_gameplay_time_tests EXCLUDE_FROM_ALL
    tools/oot3d/typed_gameplay/oot3d_gameplay_time_tests.cpp
)
target_link_libraries(oot3d_gameplay_time_tests PRIVATE
    oot3d_typed_gameplay)
target_compile_options(oot3d_gameplay_time_tests PRIVATE
    "${WARNING_OVERRIDE}")
oot3d_target_defaults(oot3d_gameplay_time_tests NO_WARNING_OVERRIDE FOLDER Tests)

add_library(oot3d_native_a32_process STATIC EXCLUDE_FROM_ALL
    tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp
    # fastmem phase 1: guest regions at window base + address
    # (config/fastmem_window; SWITCH_PORT.md "fastmem window").
    tools/oot3d/native_game_runtime/oot3d_switch_guest_window.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp
    tools/oot3d/native_game_runtime/oot3d_native_a32_registry.cpp
    tools/oot3d/native_game_runtime/oot3d_native_owner_call_adapter.cpp
    tools/oot3d/native_game_runtime/oot3d_native_compiled_functions.cpp
    tools/oot3d/native_game_runtime/oot3d_native_mass_aot.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_actor_lifecycle.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_actor_update_records.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_audio_request_callback.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_dyna_interaction_reset.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_game_state.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_pause_ui_alpha.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_player_lock_on.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_record_initializer.cpp
    tools/oot3d/native_game_runtime/oot3d_typed_gameplay_bridge.cpp
    tools/oot3d/native_game_runtime/oot3d_native_true_aot_blocks.cpp
    tools/oot3d/native_game_runtime/oot3d_native_whole_aot_runtime.cpp
)
target_include_directories(oot3d_native_a32_process PUBLIC
    tools/oot3d/native_game_runtime
    tools/mm3d/runtime
)
target_link_libraries(oot3d_native_a32_process PUBLIC
    oot3d_typed_gameplay
    nlohmann_json::nlohmann_json)

# The fastmem window's allocator hook on the host (replaces the global
# operator new/delete): only in test binaries that run with the window
# (TRIAEVUM_FASTMEM_WINDOW); the console's hook is in the newlib wrappers.
function(oot3d_guest_window_host_alloc target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" AND UNIX)
        target_sources(${target} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tools/oot3d/native_game_runtime/oot3d_switch_guest_window_host_alloc.cpp)
    endif()
endfunction()
if(OOT3D_WHOLE_AOT_PRODUCT_MODE)
    if(OOT3D_DIRECT_AOT_PLUGIN)
        target_sources(oot3d_native_a32_process PRIVATE
            tools/oot3d/native_game_runtime/oot3d_native_direct_aot.cpp)
        target_link_libraries(oot3d_native_a32_process PUBLIC
            oot3d_native_a32_runtime ${CMAKE_DL_LIBS})
        target_compile_definitions(oot3d_native_a32_process PRIVATE
            OOT3D_NATIVE_DIRECT_AOT_PLUGIN=1
            OOT3D_NATIVE_GENERATED_WHOLE_AOT=1
            OOT3D_WHOLE_AOT_PRODUCT_MODE=1)
    else()
        target_sources(oot3d_native_a32_process PRIVATE
            tools/oot3d/native_game_runtime/oot3d_memory_bus_abi.cpp)
        target_link_libraries(oot3d_native_a32_process PUBLIC
            oot3d_whole_aot_architecture_support)
    endif()
else()
    target_link_libraries(oot3d_native_a32_process PUBLIC
        oot3d_native_a32_runtime)
endif()
if (TARGET oot3d_native_a32_aot)
    target_include_directories(oot3d_native_a32_process PRIVATE
        ${OOT3D_A32_AOT_GENERATED_DIR})
    target_compile_definitions(oot3d_native_a32_process PRIVATE
        OOT3D_NATIVE_GENERATED_TRUE_AOT=1)
endif()
set(_oot3d_whole_aot_generated_header
    "${OOT3D_WHOLE_AOT_GENERATED_DIR}/oot3d_whole_aot_generated.h")
set(_oot3d_whole_aot_generated_source
    "${OOT3D_WHOLE_AOT_GENERATED_DIR}/oot3d_whole_aot_generated.cpp")
set(_oot3d_whole_aot_generated_manifest
    "${OOT3D_WHOLE_AOT_GENERATED_DIR}/whole_aot_cpp_manifest.json")
if (OOT3D_WHOLE_AOT_PRODUCT_MODE AND NOT OOT3D_DIRECT_AOT_PLUGIN)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(_oot3d_product_input IN ITEMS
            "${OOT3D_WHOLE_AOT_PROGRAM}"
            "${OOT3D_WHOLE_AOT_SELECTION}"
            "${OOT3D_WHOLE_AOT_CODE_BIN}"
            "${OOT3D_WHOLE_AOT_EXHEADER}"
            "${OOT3D_WHOLE_AOT_PRODUCT_MANIFEST}"
            "${_oot3d_whole_aot_generated_manifest}")
        if (NOT EXISTS "${_oot3d_product_input}")
            message(FATAL_ERROR
                "Whole-AOT product input does not exist: ${_oot3d_product_input}. "
                "Run prepare_whole_aot_product.py before configuring.")
        endif()
    endforeach()
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/audit_whole_aot_product.py"
            --manifest "${OOT3D_WHOLE_AOT_PRODUCT_MANIFEST}"
            --program "${OOT3D_WHOLE_AOT_PROGRAM}"
            --selection "${OOT3D_WHOLE_AOT_SELECTION}"
            --code "${OOT3D_WHOLE_AOT_CODE_BIN}"
            --exheader "${OOT3D_WHOLE_AOT_EXHEADER}"
            --generated-manifest "${_oot3d_whole_aot_generated_manifest}"
            --skip-generated-file-hashes
        RESULT_VARIABLE _oot3d_product_audit_result
        OUTPUT_VARIABLE _oot3d_product_audit_output
        ERROR_VARIABLE _oot3d_product_audit_error)
    if (NOT _oot3d_product_audit_result EQUAL 0)
        message(FATAL_ERROR
            "Whole-AOT product audit failed during configure:\n"
            "${_oot3d_product_audit_output}${_oot3d_product_audit_error}")
    endif()
endif()
if (EXISTS "${_oot3d_whole_aot_generated_header}" AND
    EXISTS "${_oot3d_whole_aot_generated_source}")
    if (OOT3D_REBUILD_WHOLE_AOT)
        file(GLOB _oot3d_whole_aot_generated_shards CONFIGURE_DEPENDS
            "${OOT3D_WHOLE_AOT_GENERATED_DIR}/oot3d_whole_aot_shard_*.cpp")
        # The hand-written native overrides (guest_overrides.json,
        # linked into the shards by apply_guest_overrides.py) stay in the same
        # file as the shards that call them.
        add_library(oot3d_native_whole_aot STATIC EXCLUDE_FROM_ALL
            ${_oot3d_whole_aot_generated_header}
            ${_oot3d_whole_aot_generated_source}
            ${_oot3d_whole_aot_generated_shards}
            tools/oot3d/native_game_runtime/oot3d_native_guest_overrides.cpp)
        target_include_directories(oot3d_native_whole_aot PRIVATE
            ${OOT3D_WHOLE_AOT_GENERATED_DIR}
            tools/oot3d/native_game_runtime
            ${OOT3D_PINNED_A32_ROOT}
            tools/oot3d/native_a32_runtime)
        if (OOT3D_WHOLE_AOT_PRODUCT_MODE)
            # Product builds consume only generated native functions plus the
            # architectural data/VFP ABI.  The decoded A32 runtime is a legacy
            # development dependency and must not enter the product closure.
            target_link_libraries(oot3d_native_whole_aot PUBLIC
                oot3d_whole_aot_architecture_support
                nlohmann_json::nlohmann_json)
        else()
            target_link_libraries(oot3d_native_whole_aot PUBLIC
                oot3d_native_a32_runtime
                nlohmann_json::nlohmann_json)
        endif()
        target_compile_options(oot3d_native_whole_aot PRIVATE
            "${WARNING_OVERRIDE}")
        if (OOT3D_WHOLE_AOT_LEAN_STATS)
            target_compile_definitions(oot3d_native_whole_aot PRIVATE
                OOT3D_WHOLE_AOT_LEAN_STATS=1)
        endif()
        if (NOT MSVC)
            # Affinity-sharded guest callees are deliberately colocated so
            # normal release builds can remove their translated call ABI.
            target_compile_options(oot3d_native_whole_aot PRIVATE
                -finline-functions)
        endif()
        if (OOT3D_WHOLE_AOT_GCSE_MEMORY_KB AND
            CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # Functions whose RTL PRE/cprop/gcse-after-reload would exceed this
            # budget (blocks x pseudos) skip those passes; the hot ones are
            # small and come out byte-for-byte identical (measured with 32768).
            target_compile_options(oot3d_native_whole_aot PRIVATE
                "--param=max-gcse-memory=${OOT3D_WHOLE_AOT_GCSE_MEMORY_KB}")
        endif()
        if (CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
            # The Switch release flags carry -ffast-math, which breaks the
            # exact VFP emulation of the generated code (cumulative IXC flags
            # diverge from the reference tests). Gameplay code stays exact.
            # devkitA64's GCC keeps a frame pointer by default (mov x29, sp in
            # every non-leaf function); the shards need none (guest faults
            # report guest state; the guard allocator's frame walk is bounded
            # and stops at a shard frame), and x29 becomes a free register.
            # sa2-nx: frame setups 24,659 -> 24; changes no profile checksum.
            target_compile_options(oot3d_native_whole_aot PRIVATE
                -fno-fast-math -ffp-contract=off -fomit-frame-pointer)
        endif()
        set(_oot3d_whole_aot_gcc_profile OFF)
        if (OOT3D_WHOLE_AOT_THINLTO OR
            NOT OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "none")
            if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
                NOT OOT3D_WHOLE_AOT_THINLTO AND
                OOT3D_WHOLE_AOT_PROFILE_MODE MATCHES "^(generate|use)$")
                set(_oot3d_whole_aot_gcc_profile ON)
            elseif (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                message(FATAL_ERROR
                    "Whole-AOT ThinLTO/profile modes require clang-cl/LLVM; "
                    "GCC supports OOT3D_WHOLE_AOT_PROFILE_MODE generate/use only")
            endif()
        endif()
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" /clang:--print-resource-dir
                OUTPUT_VARIABLE _oot3d_clang_resource_dir
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
            set(_oot3d_clang_builtins
                "${_oot3d_clang_resource_dir}/lib/windows/clang_rt.builtins-x86_64.lib")
            if (NOT EXISTS "${_oot3d_clang_builtins}")
                message(FATAL_ERROR
                    "LLVM builtins library does not exist: ${_oot3d_clang_builtins}")
            endif()
            target_link_libraries(oot3d_native_whole_aot INTERFACE
                "${_oot3d_clang_builtins}")
        endif()
        if (OOT3D_WHOLE_AOT_THINLTO)
            target_compile_options(oot3d_native_whole_aot PRIVATE
                -flto=thin)
        endif()
        unset(_oot3d_whole_aot_hot_list)
        if (OOT3D_WHOLE_AOT_HOT_FUNCTIONS)
            set(_oot3d_whole_aot_hot_list "${OOT3D_WHOLE_AOT_HOT_FUNCTIONS}")
        endif()
        if (_oot3d_whole_aot_gcc_profile)
            # GCC instrumentation (gcov) of the generated shards only. Both
            # builds name each .gcda by its object path relative to their own
            # build tree (-fprofile-prefix-path), so the instrumented and the
            # optimized build may live in different build directories, but
            # they must compile the same generated C++ from the same
            # OOT3D_WHOLE_AOT_GENERATED_DIR path. Names are flat, e.g.
            # CMakeFiles#oot3d_native_whole_aot.dir#home#...#oot3d_whole_aot_shard_000.cpp.gcda
            # Both modes: -fno-profile-values. libgcov's value profilers keep
            # shared lists that need atomic updates, and its indirect-call
            # profiler reads tpidr_el0 (crashed before main until
            # oot3d_switch_threads.cpp mirrored it); nfsmw-nx and sa2-nx turn
            # it off too. Branch and function counts are what PGO uses.
            if (OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "generate")
                # -fprofile-info-section: no libgcov constructor/exit write;
                # each object's counters are listed in the gcov_info section
                # and oot3d_whole_aot_pgo.cpp streams the whole session to
                # the SD card (periodically, on focus loss, at exit). The
                # runtime directory only names the files inside the stream
                # (whole_aot_pgo_merge.py --runtime-dir).
                target_compile_options(oot3d_native_whole_aot PRIVATE
                    "-fprofile-generate=${OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR}"
                    "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                    -fprofile-update=single
                    -fno-profile-values
                    -fprofile-info-section=gcov_info)
                # Links libgcov (__gcov_info_to_gcda) into the final executable.
                target_link_options(oot3d_native_whole_aot INTERFACE
                    -fprofile-generate)
                set(OOT3D_WHOLE_AOT_GCOV_GENERATE ON)
                target_compile_definitions(oot3d_native_whole_aot INTERFACE
                    OOT3D_WHOLE_AOT_GCOV_GENERATE=1
                    "OOT3D_WHOLE_AOT_GCOV_DIR=\"${OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR}\"")
            else()
                if (NOT IS_DIRECTORY "${OOT3D_WHOLE_AOT_PROFILE_FILE}")
                    message(FATAL_ERROR
                        "With GCC, OOT3D_WHOLE_AOT_PROFILE_FILE must be the "
                        "directory holding the collected .gcda files")
                endif()
                # Functions the training never reached keep their normal
                # optimization instead of being optimized for size; shards
                # without a .gcda are compiled as without a profile.
                # -fprofile-correction: the counters were updated without
                # atomics (-fprofile-update=single) while the guest and host
                # threads ran the same code and were read while running, so
                # some blocks' counts do not add up ("not flow-consistent");
                # GCC smooths them instead of stopping (nfsmw-nx, sa2-nx).
                target_compile_options(oot3d_native_whole_aot PRIVATE
                    "-fprofile-use=${OOT3D_WHOLE_AOT_PROFILE_FILE}"
                    "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                    -fprofile-partial-training
                    -fprofile-correction
                    -fno-profile-values
                    -Wno-missing-profile
                    # A function whose CFG changed since the collection run
                    # (edited runtime header) just loses its profile.
                    -Wno-error=coverage-mismatch
                    # Recompiled code marks no function inline, so GCC's
                    # default limit (30) decides every call; with a profile
                    # the inliner grows hot call sites first. sa2-nx's
                    # measured choice: 64, with unit growth capped at 12 %
                    # (executed calls -41 %, NRO +2 %). Inlining runs after
                    # the profile is read: no new collection needed.
                    --param=max-inline-insns-auto=64
                    --param=inline-unit-growth=12)
                # The functions the collection ran keep sched1 and
                # gcse-after-reload even when giant
                # (OOT3D_WHOLE_AOT_GIANT_FUNCTION_INSTRUCTIONS): without an
                # explicit OOT3D_WHOLE_AOT_HOT_FUNCTIONS the list comes from
                # the profile itself.
                if (NOT OOT3D_WHOLE_AOT_HOT_FUNCTIONS)
                    find_package(Python3 REQUIRED COMPONENTS Interpreter)
                    set(_oot3d_whole_aot_hot_list
                        "${CMAKE_BINARY_DIR}/whole_aot_hot_from_gcda.txt")
                    execute_process(
                        COMMAND ${Python3_EXECUTABLE}
                            "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/whole_aot_hot_from_gcda.py"
                            --gcda-dir "${OOT3D_WHOLE_AOT_PROFILE_FILE}"
                            --generated-dir "${OOT3D_WHOLE_AOT_GENERATED_DIR}"
                            --mode gcda
                            --output "${_oot3d_whole_aot_hot_list}"
                        RESULT_VARIABLE _oot3d_whole_aot_hot_result
                        OUTPUT_VARIABLE _oot3d_whole_aot_hot_output
                        ERROR_VARIABLE _oot3d_whole_aot_hot_output)
                    if (NOT _oot3d_whole_aot_hot_result EQUAL 0)
                        message(FATAL_ERROR
                            "whole_aot_hot_from_gcda.py failed on "
                            "${OOT3D_WHOLE_AOT_PROFILE_FILE}:\n${_oot3d_whole_aot_hot_output}")
                    endif()
                    message(STATUS "Whole-AOT hot functions from the profile: "
                        "${_oot3d_whole_aot_hot_list}")
                endif()
                # Pre-generated C++ (OOT3D_REGENERATE_WHOLE_AOT=OFF, as
                # scripts/build-fork.sh builds) only follows the list if it
                # was generated with it (build-fork.sh PGO=use does).
                set(_oot3d_whole_aot_manifest
                    "${OOT3D_WHOLE_AOT_GENERATED_DIR}/whole_aot_cpp_manifest.json")
                if (NOT OOT3D_REGENERATE_WHOLE_AOT AND
                    EXISTS "${_oot3d_whole_aot_manifest}")
                    file(READ "${_oot3d_whole_aot_manifest}" _oot3d_whole_aot_manifest_text)
                    string(JSON _oot3d_whole_aot_giant ERROR_VARIABLE _oot3d_json_error
                        GET "${_oot3d_whole_aot_manifest_text}" giant_cold_instructions)
                    if (_oot3d_json_error)
                        set(_oot3d_whole_aot_giant 0)
                    endif()
                    string(JSON _oot3d_whole_aot_generated_hot ERROR_VARIABLE _oot3d_json_error
                        LENGTH "${_oot3d_whole_aot_manifest_text}" giant_cold_hot_entries)
                    file(STRINGS "${_oot3d_whole_aot_hot_list}" _oot3d_whole_aot_hot_lines
                        REGEX "^[ \t]*[0-9A-Fa-fx]")
                    list(LENGTH _oot3d_whole_aot_hot_lines _oot3d_whole_aot_hot_count)
                    if (_oot3d_whole_aot_giant GREATER 0 AND (_oot3d_json_error OR
                        NOT _oot3d_whole_aot_generated_hot EQUAL _oot3d_whole_aot_hot_count))
                        message(WARNING
                            "The generated whole-AOT C++ was not generated with the "
                            "${_oot3d_whole_aot_hot_count} hot functions of "
                            "${_oot3d_whole_aot_hot_list} (manifest: "
                            "${_oot3d_whole_aot_generated_hot}): giant functions the "
                            "collection ran stay without sched1/gcse-after-reload. "
                            "Regenerate with --hot-functions (scripts/build-fork.sh PGO=use).")
                    endif()
                endif()
            endif()
        elseif (OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "generate")
            target_compile_options(oot3d_native_whole_aot PRIVATE
                -fprofile-instr-generate)
            set(_oot3d_clang_profile_runtime
                "${_oot3d_clang_resource_dir}/lib/windows/clang_rt.profile-x86_64.lib")
            if (NOT EXISTS "${_oot3d_clang_profile_runtime}")
                message(FATAL_ERROR
                    "LLVM profile runtime does not exist: "
                    "${_oot3d_clang_profile_runtime}")
            endif()
            target_link_libraries(oot3d_native_whole_aot INTERFACE
                "${_oot3d_clang_profile_runtime}")
        elseif (OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "use" OR
                OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "sample-use")
            if (NOT EXISTS "${OOT3D_WHOLE_AOT_PROFILE_FILE}")
                message(FATAL_ERROR
                    "OOT3D_WHOLE_AOT_PROFILE_FILE does not exist")
            endif()
            if (OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "use")
                set(_oot3d_whole_aot_profile_option
                    "-fprofile-instr-use=${OOT3D_WHOLE_AOT_PROFILE_FILE}")
            else()
                set(_oot3d_whole_aot_profile_option
                    "-fprofile-sample-use=${OOT3D_WHOLE_AOT_PROFILE_FILE}")
            endif()
            target_compile_options(oot3d_native_whole_aot PRIVATE
                "${_oot3d_whole_aot_profile_option}")
        elseif (NOT OOT3D_WHOLE_AOT_PROFILE_MODE STREQUAL "none")
            message(FATAL_ERROR
                "Invalid OOT3D_WHOLE_AOT_PROFILE_MODE: "
                "${OOT3D_WHOLE_AOT_PROFILE_MODE}")
        endif()
        if (OOT3D_WHOLE_AOT_ORDER_FILE)
            if (NOT EXISTS "${OOT3D_WHOLE_AOT_ORDER_FILE}")
                message(FATAL_ERROR
                    "OOT3D_WHOLE_AOT_ORDER_FILE does not exist")
            endif()
            target_link_options(oot3d_native_whole_aot INTERFACE
                "/ORDER:@${OOT3D_WHOLE_AOT_ORDER_FILE}")
        endif()
        set_property(GLOBAL APPEND PROPERTY JOB_POOLS
            oot3d_whole_aot_compile=${OOT3D_WHOLE_AOT_MAX_PARALLEL_COMPILES})
        set_property(TARGET oot3d_native_whole_aot PROPERTY
            JOB_POOL_COMPILE oot3d_whole_aot_compile)
        if (MSVC)
            # Generated shards do not share a compiler PDB. /Zi plus /FS
            # serialized otherwise independent translation units.
            target_compile_options(oot3d_native_whole_aot PRIVATE /Z7)
            set_property(TARGET oot3d_native_whole_aot PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        if (OOT3D_REGENERATE_WHOLE_AOT)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(_oot3d_whole_aot_code_arguments)
            set(_oot3d_whole_aot_code_dependencies)
            if (OOT3D_WHOLE_AOT_CODE_BIN)
                list(APPEND _oot3d_whole_aot_code_arguments
                    --code "${OOT3D_WHOLE_AOT_CODE_BIN}")
                list(APPEND _oot3d_whole_aot_code_dependencies
                    "${OOT3D_WHOLE_AOT_CODE_BIN}")
            endif()
            set(_oot3d_whole_aot_hot_arguments)
            if (_oot3d_whole_aot_hot_list)
                set(_oot3d_whole_aot_hot_arguments
                    --hot-functions "${_oot3d_whole_aot_hot_list}")
                list(APPEND _oot3d_whole_aot_code_dependencies
                    "${_oot3d_whole_aot_hot_list}")
            endif()
            set(_oot3d_whole_aot_generation_stamp
                "${OOT3D_WHOLE_AOT_GENERATED_DIR}/generation.stamp")
            add_custom_command(
                OUTPUT "${_oot3d_whole_aot_generation_stamp}"
                COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/generate_whole_aot_cpp.py"
                    --program "${OOT3D_WHOLE_AOT_PROGRAM}"
                    --selection "${OOT3D_WHOLE_AOT_SELECTION}"
                    ${_oot3d_whole_aot_code_arguments}
                    --output "${OOT3D_WHOLE_AOT_GENERATED_DIR}"
                    --shards "${OOT3D_WHOLE_AOT_SHARD_COUNT}"
                    --shard-strategy "${OOT3D_WHOLE_AOT_SHARD_STRATEGY}"
                    --giant-function-instructions "${OOT3D_WHOLE_AOT_GIANT_FUNCTION_INSTRUCTIONS}"
                    ${_oot3d_whole_aot_hot_arguments}
                    $<$<BOOL:${OOT3D_WHOLE_AOT_NATIVE_VFP_AARCH64}>:--native-vfp-aarch64>
                COMMAND ${CMAKE_COMMAND} -E touch
                    "${_oot3d_whole_aot_generation_stamp}"
                DEPENDS
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/generate_whole_aot_cpp.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/generate_whole_aot.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/generate_aot.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/whole_aot_cpp.py"
                    "${OOT3D_WHOLE_AOT_SELECTION}"
                    "${OOT3D_WHOLE_AOT_PROGRAM}"
                    ${_oot3d_whole_aot_code_dependencies}
                COMMENT "Regenerating ${OOT3D_WHOLE_AOT_SHARD_STRATEGY} whole-AOT C++ shards"
                VERBATIM)
            add_custom_target(oot3d_generate_whole_aot_cpp
                DEPENDS "${_oot3d_whole_aot_generation_stamp}")
            add_dependencies(oot3d_native_whole_aot
                oot3d_generate_whole_aot_cpp)
        endif()
        set_property(TARGET oot3d_native_whole_aot PROPERTY FOLDER Tools)
    elseif (EXISTS "${OOT3D_WHOLE_AOT_PREBUILT_LIBRARY}")
        add_library(oot3d_native_whole_aot STATIC IMPORTED GLOBAL)
        set_target_properties(oot3d_native_whole_aot PROPERTIES
            IMPORTED_LOCATION "${OOT3D_WHOLE_AOT_PREBUILT_LIBRARY}")
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" /clang:--print-resource-dir
                OUTPUT_VARIABLE _oot3d_clang_resource_dir
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
            set(_oot3d_clang_builtins
                "${_oot3d_clang_resource_dir}/lib/windows/clang_rt.builtins-x86_64.lib")
            if (NOT EXISTS "${_oot3d_clang_builtins}")
                message(FATAL_ERROR
                    "LLVM builtins library does not exist: ${_oot3d_clang_builtins}")
            endif()
            target_link_libraries(oot3d_native_whole_aot INTERFACE
                "${_oot3d_clang_builtins}")
        endif()
        # Compile the small registry in the consumer so runtime dispatch
        # contracts can evolve without rebuilding every prebuilt AOT shard.
        # The archive registry member is then left unselected by the linker.
        target_sources(oot3d_native_a32_process PRIVATE
            "${_oot3d_whole_aot_generated_source}")
    endif()
    if (TARGET oot3d_native_whole_aot)
        if (OOT3D_WHOLE_AOT_PRODUCT_MODE)
            add_custom_target(oot3d_audit_whole_aot_product
                COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/audit_whole_aot_product.py"
                    --manifest "${OOT3D_WHOLE_AOT_PRODUCT_MANIFEST}"
                    --program "${OOT3D_WHOLE_AOT_PROGRAM}"
                    --selection "${OOT3D_WHOLE_AOT_SELECTION}"
                    --code "${OOT3D_WHOLE_AOT_CODE_BIN}"
                    --exheader "${OOT3D_WHOLE_AOT_EXHEADER}"
                    --generated-manifest
                    "${_oot3d_whole_aot_generated_manifest}"
                DEPENDS
                    "${OOT3D_WHOLE_AOT_PROGRAM}"
                    "${OOT3D_WHOLE_AOT_SELECTION}"
                    "${OOT3D_WHOLE_AOT_CODE_BIN}"
                    "${OOT3D_WHOLE_AOT_EXHEADER}"
                    "${OOT3D_WHOLE_AOT_PRODUCT_MANIFEST}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/oot3d/native_a32_runtime/whole_aot_functions.json"
                    "${_oot3d_whole_aot_generated_manifest}"
                COMMENT "Auditing zero-residual whole-AOT product closure"
                VERBATIM)
            if (TARGET oot3d_generate_whole_aot_cpp)
                add_dependencies(oot3d_audit_whole_aot_product
                    oot3d_generate_whole_aot_cpp)
            endif()
            add_dependencies(oot3d_native_a32_process
                oot3d_audit_whole_aot_product)
            target_compile_definitions(oot3d_native_a32_process PRIVATE
                OOT3D_WHOLE_AOT_PRODUCT_MODE=1)
        endif()
        if (OOT3D_WHOLE_AOT_THINLTO_CACHE_DIR)
            file(MAKE_DIRECTORY "${OOT3D_WHOLE_AOT_THINLTO_CACHE_DIR}")
            target_link_options(oot3d_native_whole_aot INTERFACE
                "/lldltocache:${OOT3D_WHOLE_AOT_THINLTO_CACHE_DIR}")
        endif()
        target_link_libraries(oot3d_native_a32_process PUBLIC
            oot3d_native_whole_aot)
        target_include_directories(oot3d_native_a32_process PRIVATE
            ${OOT3D_WHOLE_AOT_GENERATED_DIR})
        target_compile_definitions(oot3d_native_a32_process PRIVATE
            OOT3D_NATIVE_GENERATED_WHOLE_AOT=1)
        if (OOT3D_REBUILD_WHOLE_AOT)
            target_compile_definitions(oot3d_native_whole_aot PRIVATE
                OOT3D_NATIVE_GENERATED_WHOLE_AOT=1)
            target_compile_definitions(oot3d_native_a32_process PRIVATE
                OOT3D_GUEST_OVERRIDES=1)
        endif()
    endif()
endif()
if (OOT3D_REQUIRE_WHOLE_AOT AND NOT TARGET oot3d_native_whole_aot AND
    NOT OOT3D_DIRECT_AOT_PLUGIN)
    message(FATAL_ERROR
        "OOT3D_REQUIRE_WHOLE_AOT is ON, but no whole-AOT backend was "
        "configured. Generated metadata must exist in "
        "${OOT3D_WHOLE_AOT_GENERATED_DIR} and either "
        "OOT3D_REBUILD_WHOLE_AOT must be ON or "
        "OOT3D_WHOLE_AOT_PREBUILT_LIBRARY must name an existing archive "
        "(configured value: ${OOT3D_WHOLE_AOT_PREBUILT_LIBRARY}).")
endif()
if (EXISTS "${OOT3D_MASS_AOT_GENERATED_DIR}/oot3d_mass_generated.h" AND
    EXISTS "${OOT3D_MASS_AOT_GENERATED_DIR}/oot3d_mass_function_boundaries.h" AND
    EXISTS "${OOT3D_MASS_AOT_PREBUILT_LIBRARY}")
    add_library(oot3d_native_mass_aot STATIC IMPORTED GLOBAL)
    set_target_properties(oot3d_native_mass_aot PROPERTIES
        IMPORTED_LOCATION "${OOT3D_MASS_AOT_PREBUILT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OOT3D_MASS_AOT_GENERATED_DIR}")
    target_link_libraries(oot3d_native_a32_process PUBLIC
        oot3d_native_mass_aot)
    target_include_directories(oot3d_native_a32_process PRIVATE
        ${OOT3D_MASS_AOT_GENERATED_DIR}
        tools/oot3d/native_a32_runtime/mass_cpp)
    target_compile_definitions(oot3d_native_a32_process PRIVATE
        OOT3D_NATIVE_GENERATED_MASS_AOT=1)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" /clang:--print-resource-dir
            OUTPUT_VARIABLE _oot3d_mass_clang_resource_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        set(_oot3d_mass_clang_builtins
            "${_oot3d_mass_clang_resource_dir}/lib/windows/clang_rt.builtins-x86_64.lib")
        if (NOT EXISTS "${_oot3d_mass_clang_builtins}")
            message(FATAL_ERROR
                "LLVM builtins library does not exist: ${_oot3d_mass_clang_builtins}")
        endif()
        target_link_libraries(oot3d_native_mass_aot INTERFACE
            "${_oot3d_mass_clang_builtins}")
    endif()
endif()

if (TARGET oot3d_native_a32_aot)
    add_executable(oot3d_typed_gameplay_tests EXCLUDE_FROM_ALL
        tools/oot3d/native_game_runtime/oot3d_typed_gameplay_tests.cpp
    )
    target_include_directories(oot3d_typed_gameplay_tests PRIVATE
        ${OOT3D_A32_AOT_GENERATED_DIR}
    )
    target_link_libraries(oot3d_typed_gameplay_tests PRIVATE
        oot3d_native_a32_process
        oot3d_native_a32_aot
    )
    target_compile_options(oot3d_typed_gameplay_tests PRIVATE
        "${WARNING_OVERRIDE}")
    oot3d_target_defaults(oot3d_typed_gameplay_tests NO_WARNING_OVERRIDE FOLDER Tests)

    add_executable(oot3d_typed_player_gameplay_tests EXCLUDE_FROM_ALL
        tools/oot3d/native_game_runtime/oot3d_typed_player_gameplay_tests.cpp
    )
    target_include_directories(oot3d_typed_player_gameplay_tests PRIVATE
        ${OOT3D_A32_AOT_GENERATED_DIR}
    )
    target_link_libraries(oot3d_typed_player_gameplay_tests PRIVATE
        oot3d_native_a32_process
        oot3d_native_a32_aot
    )
    target_compile_options(oot3d_typed_player_gameplay_tests PRIVATE
        "${WARNING_OVERRIDE}")
    oot3d_target_defaults(oot3d_typed_player_gameplay_tests NO_WARNING_OVERRIDE FOLDER Tests)
endif()
target_compile_options(oot3d_native_a32_process PRIVATE "${WARNING_OVERRIDE}")
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_options(oot3d_native_a32_process PRIVATE
        $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    target_compile_definitions(oot3d_native_a32_process PRIVATE NOMINMAX)
    set_property(TARGET oot3d_native_a32_process PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_property(TARGET oot3d_native_a32_process PROPERTY FOLDER Tools)

# The mass-AOT tests compile against the generated A32 AOT header, so they exist
# only in a tree configured with those generated inputs.
if (TARGET oot3d_native_a32_aot)
    add_executable(oot3d_native_mass_aot_tests EXCLUDE_FROM_ALL
        tools/oot3d/native_game_runtime/oot3d_native_mass_aot_tests.cpp)
    target_link_libraries(oot3d_native_mass_aot_tests PRIVATE
        oot3d_native_a32_process
        oot3d_native_a32_aot)
    target_include_directories(oot3d_native_mass_aot_tests PRIVATE
        ${OOT3D_A32_AOT_GENERATED_DIR})
    if (TARGET oot3d_native_mass_aot)
        target_include_directories(oot3d_native_mass_aot_tests PRIVATE
            ${OOT3D_MASS_AOT_GENERATED_DIR}
            tools/oot3d/native_a32_runtime/mass_cpp)
    endif()
    target_compile_options(oot3d_native_mass_aot_tests PRIVATE
        "${WARNING_OVERRIDE}")
    oot3d_target_defaults(oot3d_native_mass_aot_tests NO_WARNING_OVERRIDE FOLDER Tests)
endif()
