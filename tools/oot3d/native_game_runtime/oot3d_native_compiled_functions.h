#pragma once

#include "recomp/a32_runtime.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

class NativeA32Memory;
struct Oot3dWholeAotHookGateSet;

struct Oot3dCompiledFunctionStats {
    uint64_t Calls = 0;
    uint64_t BytesProcessed = 0;
    uint64_t RetainedArmFallbacks = 0;
    uint64_t MemcpyOverlapFallbacks = 0;
    uint64_t MemcpyOverlapBytes = 0;
    uint64_t MemcpyDestinationAfterSourceFallbacks = 0;
    uint64_t MemcpyDestinationBeforeSourceFallbacks = 0;
    uint64_t Mtx3x4FastCalls = 0;
    uint64_t Mtx3x4SoftCalls = 0;
    uint64_t Mtx3x4ValidationCalls = 0;
    uint64_t Mtx3x4ValueMismatches = 0;
    uint64_t Mtx3x4FlagMismatches = 0;
    uint64_t WholeAotCalls = 0;
    uint64_t WholeAotDirectCalls = 0;
    uint64_t WholeAotIndirectCalls = 0;
    uint64_t WholeAotResolvedIndirectCalls = 0;
    uint64_t WholeAotExternalCalls = 0;
    uint64_t WholeAotSvcExits = 0;
    uint64_t WholeAotBlockLimitExits = 0;
    uint64_t WholeAotMemoryFaults = 0;
    uint64_t WholeAotUnsupportedExits = 0;
};

struct Oot3dWholeAotExternalTarget {
    uint32_t Entry = 0;
    uint64_t Calls = 0;
    uint64_t ManualCompiledCalls = 0;
    uint64_t TimingSamples = 0;
    uint64_t TimingSampleNanoseconds = 0;
};

bool Oot3dWholeAotAvailable() noexcept;
// The block-entry gates of the linked whole-AOT code (Oot3dWholeAotHookGates):
// every block for the default lowering and builds without whole-AOT; the
// gate list for code generated with --sparse-block-entries (N4). A
// block-entry callback whose list the gates do not cover is refused
// (Oot3dWholeAotHookGatesError in oot3d_native_whole_aot_runtime.h).
Oot3dWholeAotHookGateSet Oot3dLinkedWholeAotHookGates() noexcept;

// OoT3D game semantics (manual compiled functions, typed gameplay): on by default; off for another title
// (TitleProfile::Oot3dGameSemantics), whose code and data sit at other addresses.
void SetOot3dGameSemanticsEnabled(bool enabled) noexcept;
bool Oot3dGameSemanticsEnabled() noexcept;

// Native overrides of guest functions (oot3d_native_guest_overrides).
// No effect when the build doesn't compile them (precompiled whole-AOT module).
void SetOot3dGuestOverridesActive(bool enabled) noexcept;
// Connects the guest profile to the native overrides (Oot3dGuestOverrideProfileHook
// in oot3d_native_guest_overrides.h); without overrides in the build, does nothing.
void RegisterOot3dGuestOverrideProfileHook(
    oot3d::recomp::a32::BlockEntryCallback callback,
    bool (*forwards)(const void* user, uint32_t first, uint32_t last) noexcept,
    void (*mark)(void* user, uint32_t pc) noexcept) noexcept;
// One line per override: entry, name, native executions and refusals.
std::string DescribeOot3dGuestOverrides();
// config/guest_overrides_disable: per-replacement kill switches (hex entries
// and/or "mirror"; see DisableOot3dGuestOverrides). Returns how many it
// disabled (0 when the overrides are not compiled in).
size_t DisableOot3dGuestOverridesFromList(std::string_view list);
// logs/perf.txt "natives:" line: per window, executions and declines of
// the mirror replacements (Profile18 round), each and in total.
std::string FormatOot3dGuestOverridesPerfLine();
std::span<const uint32_t> Oot3dCompiledFunctionEntryPoints(
    bool includeWholeAot = true,
    bool includeManualCompiledFunctions = true) noexcept;
void ResetOot3dCompiledFunctionStats() noexcept;
Oot3dCompiledFunctionStats GetOot3dCompiledFunctionStats() noexcept;
void SetOot3dCompiledFunctionProfilingEnabled(bool enabled);
std::vector<Oot3dWholeAotExternalTarget>
GetOot3dWholeAotExternalTargets(size_t maximumTargets = 64U);

bool ExecuteOot3dCompiledFunction(
    uint32_t pc,
    oot3d::recomp::a32::GuestState& state,
    NativeA32Memory& memory,
    oot3d::recomp::a32::ExecutionResult* result,
    uint32_t blockBudget = 1'000'000U,
    uint32_t* blocksConsumed = nullptr,
    oot3d::recomp::a32::BlockEntryCallback blockEntry = nullptr,
    void* blockEntryUser = nullptr,
    const uint32_t* blockEntryPcs = nullptr,
    size_t blockEntryPcCount = 0U,
    bool skipFirstBlockEntry = false,
    bool enableManualCompiledFunctions = true,
    bool enableWholeAot = true,
    uint32_t stopPc = 0U);

bool ExecuteOot3dCompiledFunction(
    uint32_t pc,
    oot3d::recomp::a32::GuestState& state,
    oot3d::recomp::a32::MemoryBus& memory,
    oot3d::recomp::a32::ExecutionResult* result,
    void* user);

} // namespace Oot3dNativeGame
