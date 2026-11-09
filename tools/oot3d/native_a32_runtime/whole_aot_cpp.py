"""Emit direct function-level C++ from the structural OOT3D AOT program."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from whole_aot_optimization_ir import Flag, analyze_function
from whole_aot_memory_regions import carried_pair_copy, region_memory_safe


PROGRAM_FORMAT = "oot3d_whole_aot_program_v1"
SELECTION_FORMAT = "oot3d_whole_aot_function_selection_v1"
OUTPUT_FORMAT = "oot3d_whole_aot_cpp_v1"
HEADER_NAME = "oot3d_whole_aot_generated.h"
SOURCE_NAME = "oot3d_whole_aot_generated.cpp"
INTERNAL_HEADER_NAME = "oot3d_whole_aot_generated_internal.h"
SHARD_PREFIX = "oot3d_whole_aot_shard_"
MANIFEST_NAME = "whole_aot_cpp_manifest.json"
HOOK_GATES_FORMAT = "oot3d_whole_aot_hook_gates_v1"
HOOK_GATES_NAME = "whole_aot_hook_gates.json"


def load_hook_gates(path: Path) -> frozenset[int]:
    """PCs of whole_aot_hook_gates.json ("pcs": hex strings or integers), the
    union of every runtime block-entry hook list, written by the host tool
    oot3d_block_entry_hook_census (oot3d_native_block_entry_hooks.h)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("format") != HOOK_GATES_FORMAT:
        raise ValueError(f"{path}: not an {HOOK_GATES_FORMAT} file")
    pcs = frozenset(int(pc, 0) if isinstance(pc, str) else int(pc) for pc in data["pcs"])
    misaligned = sorted(pc for pc in pcs if pc & 3)
    if misaligned:
        raise ValueError(f"{path}: hook gate 0x{misaligned[0]:08X} is not word aligned")
    return pcs


class LoweringError(ValueError):
    pass


# Text-segment words that host code rewrites at run time, so their loads must
# stay real memory reads. Only WriteHostBytes can modify the read-only text
# segment (GetWritePointer, WriteBytes-style guest paths and Fill refuse
# non-writable regions).
# - 0x003CC430: Obj_Mure retirement margin, restored by
#   AssertObjMureDistances (oot3d_native_a32_actor_visibility.cpp).
# Text words the host rewrites at run time, never folded into constants:
# actor visibility (AssertObjMureDistances) and the TopScreen layout patches
# inside text (kVerifiedLayoutPatches: gear texture filters, dungeon map).
HOST_WRITTEN_TEXT_RANGES: tuple[tuple[int, int], ...] = (
    (0x003CC430, 4),
    (0x004673BC, 4),
    (0x00467688, 4),
    (0x002F0404, 4),
    (0x002F0410, 4),
    (0x0044262C, 4),
    (0x0044264C, 4),
    (0x0042D9F0, 4),
)


@dataclass(frozen=True)
class LiteralImage:
    """The immutable text segment used to fold PC-relative literal loads.

    `base`/`size` are the structural program's text range, which the product
    audit ties to the ExHeader text segment; the process manifest maps that
    segment read-only, so guest stores to it fault and never change bytes.
    """

    base: int
    data: bytes
    excluded: tuple[tuple[int, int], ...] = HOST_WRITTEN_TEXT_RANGES

    def word(self, address: int, size: int) -> int | None:
        if address < self.base or address + size > self.base + len(self.data):
            return None
        if any(address < start + length and start < address + size
               for start, length in self.excluded):
            return None
        offset = address - self.base
        return int.from_bytes(self.data[offset:offset + size], "little")


# Set by generate() while rendering; None disables literal folding (unit
# tests that call the emitters directly keep the original memory reads).
_LITERAL_IMAGE: LiteralImage | None = None
_FOLDED_LITERALS: set[int] = set()
_FOLDED_VFP_LITERALS: set[int] = set()
# Set by generate(): scalar binary32 VFP through native AArch64 instructions
# with the guest FPSCR mode in FPCR (oot3d_native_a32_vfp_aarch64.h). Off by
# default; the emitted code still compiles to the software operations on
# other hosts.
_NATIVE_VFP_AARCH64 = False
# Register promotion (generate(promote_registers=True)): GPRs, NZCV and
# the block budget live in local variables in every ordinary function,
# synchronized with the architectural state only where it is observable (see
# _promote_function_body). Off by default: the generated C++ is the same as before.
#
# Measured (2026-10-17, GCC 16.1 devkitA64 -O3, Switch shard flags):
# worse. Shard 243: 114.2k -> 154.8k instructions, loads +77%, stores
# +89% (stack 13.5k/15.8k -> 33.3k/32.7k); FUN_0046c204: 81k -> 119k.
# With references, state itself is the "home" of each register: GCC
# stores on write and reloads only what it uses after a call (including
# the cold memory slow paths). With locals, (1) every value live across
# a cold call (ReadFastSlow/WriteFastSlow) needs a callee-saved register
# or a stack spill; (2) the entry loads all promoted registers at once,
# because the exits store the "maybe dirty" set (static);
# (3) the prologue saves x19-x28. Without the slow paths
# (experiment, not exact) it is still 19% worse. Hence it stays off.
_PROMOTE_REGISTERS = False
# Set by generate() (--vfp-zero-aware, only with the native VFP): arithmetic
# goes through Oot3dVfpZeroAware*, which keep exact zero results (x * 0,
# x - x, exact cancellation in VMLA/VMLS) on the native path instead of the
# software operation; same value and flags (oot3d_native_a32_vfp_aarch64.h).
_VFP_ZERO_AWARE = False
# Set by generate() (--lean-stack): no guest value lives on the stack in the
# hot path of a load. A read destination is not zero-initialized (ReadFast
# writes it whenever it returns true, and it is dead when it returns false),
# and the word-by-word LDM fallback reads into its own temporaries, so the
# spanWords array of the single-page path never has its address taken and is
# kept in registers. Same values, faults and order.
_LEAN_STACK = False
# Set by generate() (--sparse-block-entries, "N4"): the block budget and the
# block-entry gate only where they are needed, instead of in every block.
# Credit: ReXGlue/XenonRecomp-style recompilers emit loop barriers only at
# loop headers (no per-block check); sa2-nx dropped the provably useless ones.
# - Budget: kept at the function entry and at every target of a DFS back edge
#   of the function's goto graph (each directed cycle contains such an edge,
#   so every loop, irreducible ones included, passes a decrement), and charged
#   once by the dispatch switch for every other block. Every nested call
#   enters a callee at its entry, and every resume, indirect call into a block
#   and re-dispatch of the registry loop goes through a dispatch switch, so
#   the number of blocks a run executes stays bounded by the budget times the
#   largest function, and the C++ call depth by the budget.
# - Gate: only blocks whose PC is in _HOOK_GATES (whole_aot_hook_gates.json,
#   the union of the runtime hook lists) call Oot3dAotEnterBlock; they charge
#   the budget too. A run's first block is always charged (by itself or by the
#   dispatch), so "first block" stays BlocksConsumed() == 1 after the gated
#   block's own charge, as in the default lowering.
# - Guest state is the same; BlockLimit exits (cooperative thread switches)
#   happen only at charged blocks and dispatches, so run lengths change.
_SPARSE_BLOCK_ENTRIES = False
_HOOK_GATES: frozenset[int] = frozenset()
# Block counts of the last sparse generation, for the manifest.
_SPARSE_STATS: dict[str, int] = {}
# Set by generate() (--fold-vfp-literals): VLDR Sd/Dd, [pc, #imm] from the
# immutable text segment becomes a constant, like the LDR literal folding
# above (same image, same host-written exclusions): no page-flag check, no
# load and no fault path. Text is mapped read-only for the whole process, so
# the read always yields the code.bin bytes and can never fault.
_FOLD_VFP_LITERALS = False
# Giant cold functions marked with GIANT_COLD_ATTRIBUTE (see generate).
_GIANT_COLD_ENTRIES: frozenset[int] = frozenset()
# The pre-allocation scheduler (sched1, which on AArch64 GCC enables only at -O3)
# and post-allocation load CSE (-fgcse-after-reload, only at -O3) cost 50-65% of
# the compile time of giant functions (thousands of guest instructions). The
# attribute disables them only there and keeps forced inlining of the hot helpers
# (measured: same external symbols, .text +0.02%). It goes on the same line as
# the signature, with no macro or new lines: the shard line numbers (and with
# them the PGO .gcda checksums) do not change. Other compilers ignore
# an unknown gnu:: attribute (just a warning, and the shards use -w).
GIANT_COLD_ATTRIBUTE = (
    '[[gnu::optimize("no-schedule-insns", "no-gcse-after-reload")]] '
)


def _cpp_flag_mask(mask: Flag) -> str:
    return f"static_cast<Oot3dAotFlagMask>(0x{int(mask):X}U)"


@dataclass(frozen=True)
class Instruction:
    pc: int
    raw: int


@dataclass(frozen=True)
class Block:
    block_id: int
    pc: int
    end_pc: int
    instructions: tuple[Instruction, ...]
    successors: tuple[dict[str, object], ...]


@dataclass(frozen=True)
class Function:
    entry: int
    name: str
    blocks: tuple[Block, ...]
    direct_calls: tuple[tuple[int, int], ...]
    resume_entries: tuple[int, ...]
    dispatch_entries: tuple[int, ...] = ()


def _function_dispatch_entries(function: Function) -> tuple[int, ...]:
    return function.dispatch_entries or function.resume_entries


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _write_if_different(path: Path, data: bytes) -> None:
    if path.is_file() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def _ror(value: int, amount: int) -> int:
    amount &= 31
    if amount == 0:
        return value & 0xFFFFFFFF
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF


def _reg(index: int, pc: int | None = None) -> str:
    if index == 15:
        if pc is None:
            raise LoweringError("PC register requires an instruction address")
        return f"0x{pc + 8:08X}U"
    if not 0 <= index < 15:
        raise LoweringError(f"invalid core register {index}")
    return f"state.R[{index}]"


def _lane(index: int) -> str:
    if not 0 <= index < 32:
        raise LoweringError(f"invalid VFP lane {index}")
    return f"frame.Guest.vfp[{index}]"


def _condition(raw: int) -> str:
    condition = (raw >> 28) & 0xF
    n = "state.Flags.N()"
    z = "state.Flags.Z()"
    c = "state.Flags.C()"
    v = "state.Flags.V()"
    expressions = {
        0x0: z,
        0x1: f"!({z})",
        0x2: c,
        0x3: f"!({c})",
        0x4: n,
        0x5: f"!({n})",
        0x6: v,
        0x7: f"!({v})",
        0x8: f"({c}) && !({z})",
        0x9: f"!({c}) || ({z})",
        0xA: f"({n}) == ({v})",
        0xB: f"({n}) != ({v})",
        0xC: f"!({z}) && (({n}) == ({v}))",
        0xD: f"({z}) || (({n}) != ({v}))",
        0xE: "true",
        0xF: "false",
    }
    return expressions[condition]


def _conditional(raw: int, body: Iterable[str]) -> list[str]:
    lines = list(body)
    if ((raw >> 28) & 0xF) == 0xE:
        return ["{", *(f"    {line}" for line in lines), "}"]
    return [
        f"if ({_condition(raw)}) {{",
        *(f"    {line}" for line in lines),
        "}",
    ]


def _shifted_register(raw: int, pc: int) -> str:
    rm = raw & 0xF
    value = _reg(rm, pc)
    shift_type = (raw >> 5) & 0x3
    if raw & (1 << 4):
        rs = (raw >> 8) & 0xF
        # Register-specified shifts treat an amount of zero as a no-op. The
        # immediate LSR/ASR helpers intentionally treat encoded zero as 32,
        # so they cannot represent this ARM form.
        return (
            f"Oot3dAotShiftRegister({value}, {shift_type}U, "
            f"{_reg(rs, pc)}, false).Value"
        )
    methods = ("Oot3dAotLsl", "Oot3dAotLsr", "Oot3dAotAsr", "Oot3dAotRor")
    amount = (raw >> 7) & 0x1F
    if amount == 0 and shift_type == 0:
        return value
    if amount == 0 and shift_type == 3:
        return (
            f"((state.Flags.C() ? 0x80000000U : 0U) | "
            f"({value} >> 1U))"
        )
    return f"{methods[shift_type]}({value}, {amount}U)"


def _operand2(raw: int, pc: int) -> str:
    if raw & (1 << 25):
        immediate = raw & 0xFF
        rotate = ((raw >> 8) & 0xF) * 2
        return f"0x{_ror(immediate, rotate):08X}U"
    return _shifted_register(raw, pc)


def _logical_operand2(raw: int, pc: int) -> tuple[list[str], str, str]:
    old_carry = "state.Flags.C()"
    if raw & (1 << 25):
        immediate = raw & 0xFF
        rotate = ((raw >> 8) & 0xF) * 2
        value = _ror(immediate, rotate)
        carry = old_carry if rotate == 0 else f"(0x{value:08X}U & 0x80000000U) != 0U"
        return [], f"0x{value:08X}U", carry
    rm = raw & 0xF
    shift_type = (raw >> 5) & 0x3
    if raw & (1 << 4):
        rs = (raw >> 8) & 0xF
        setup = [
            "const auto shifted = Oot3dAotShiftRegister(",
            f"    {_reg(rm, pc)}, {shift_type}U, {_reg(rs, pc)}, {old_carry});",
        ]
    else:
        amount = (raw >> 7) & 0x1F
        setup = [
            "const auto shifted = Oot3dAotShiftImmediate(",
            f"    {_reg(rm, pc)}, {shift_type}U, {amount}U, {old_carry});",
        ]
    return setup, "shifted.Value", "shifted.Carry"


def _emit_data_processing(
    item: Instruction, live_flags: Flag = Flag.NZCV
) -> list[str]:
    raw = item.raw
    pc = item.pc
    opcode = (raw >> 21) & 0xF
    setflags = bool(raw & (1 << 20))
    immediate = (raw & 0x0E000000) == 0x02000000
    immediate_shift = (raw & 0x0E000010) == 0x00000000
    register_shift = (raw & 0x0E000090) == 0x00000010
    if not (immediate or immediate_shift or register_shift):
        raise LoweringError(
            f"invalid data-processing encoding 0x{raw:08X} at 0x{pc:08X}"
        )
    if 0x8 <= opcode <= 0xB and not setflags:
        raise LoweringError(
            f"test data-processing opcode without S bit at 0x{pc:08X}"
        )
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    operand = _operand2(raw, pc)
    body: list[str] = []
    live = _cpp_flag_mask(live_flags)
    if opcode == 0x0:  # AND
        if rd == 15:
            raise LoweringError(f"AND writes PC at 0x{pc:08X}")
        if setflags:
            setup, operand, carry = _logical_operand2(raw, pc)
            body.extend(setup)
        body.append(f"{_reg(rd)} = {_reg(rn, pc)} & {operand};")
        if setflags:
            body.append(f"Oot3dAotSetLogicalFlags(state, {_reg(rd)}, {carry}, {live});")
    elif opcode == 0x1:  # EOR
        if rd == 15:
            raise LoweringError(f"EOR writes PC at 0x{pc:08X}")
        if setflags:
            setup, operand, carry = _logical_operand2(raw, pc)
            body.extend(setup)
        body.append(f"{_reg(rd)} = {_reg(rn, pc)} ^ {operand};")
        if setflags:
            body.append(f"Oot3dAotSetLogicalFlags(state, {_reg(rd)}, {carry}, {live});")
    elif opcode == 0x2:  # SUB
        if rd == 15:
            raise LoweringError(f"SUB writes PC at 0x{pc:08X}")
        body.append(f"const uint32_t operand = {operand};")
        if setflags:
            body.append(f"Oot3dAotSetSubtractFlags(state, {_reg(rn, pc)}, operand, {live});")
        body.append(f"{_reg(rd)} = {_reg(rn, pc)} - operand;")
    elif opcode == 0x4:  # ADD
        if rd == 15:
            raise LoweringError(f"ADD writes PC at 0x{pc:08X}")
        body.append(f"const uint32_t operand = {operand};")
        if setflags:
            body.append(f"Oot3dAotSetAddFlags(state, {_reg(rn, pc)}, operand, {live});")
        body.append(f"{_reg(rd)} = {_reg(rn, pc)} + operand;")
    elif opcode == 0x5:  # ADC
        if rd == 15:
            raise LoweringError(f"ADC writes PC at 0x{pc:08X}")
        body.append(
            f"{_reg(rd)} = Oot3dAotAddWithCarry(state, {_reg(rn, pc)}, "
            f"{operand}, state.Flags.C(), "
            f"{'true' if setflags else 'false'}, {live});"
        )
    elif opcode == 0x6:  # SBC
        if rd == 15:
            raise LoweringError(f"SBC writes PC at 0x{pc:08X}")
        body.append(
            f"{_reg(rd)} = Oot3dAotAddWithCarry(state, {_reg(rn, pc)}, "
            f"~({operand}), state.Flags.C(), "
            f"{'true' if setflags else 'false'}, {live});"
        )
    elif opcode == 0x7:  # RSC
        if rd == 15:
            raise LoweringError(f"RSC writes PC at 0x{pc:08X}")
        body.append(
            f"{_reg(rd)} = Oot3dAotAddWithCarry(state, {operand}, "
            f"~({_reg(rn, pc)}), state.Flags.C(), "
            f"{'true' if setflags else 'false'}, {live});"
        )
    elif opcode == 0x3:  # RSB
        if rd == 15:
            raise LoweringError(f"RSB writes PC at 0x{pc:08X}")
        body.append(f"const uint32_t operand = {operand};")
        if setflags:
            body.append(f"Oot3dAotSetSubtractFlags(state, operand, {_reg(rn, pc)}, {live});")
        body.append(f"{_reg(rd)} = operand - {_reg(rn, pc)};")
    elif opcode == 0x8:  # TST
        if not setflags:
            raise LoweringError(f"TST without S bit at 0x{pc:08X}")
        setup, operand, carry = _logical_operand2(raw, pc)
        body.extend(setup)
        body.append(f"const uint32_t value = {_reg(rn, pc)} & {operand};")
        body.append("Oot3dAotSetLogicalFlags(state, value, " + carry + f", {live});")
    elif opcode == 0x9:  # TEQ
        if not setflags:
            raise LoweringError(f"TEQ without S bit at 0x{pc:08X}")
        setup, operand, carry = _logical_operand2(raw, pc)
        body.extend(setup)
        body.append(f"const uint32_t value = {_reg(rn, pc)} ^ {operand};")
        body.append("Oot3dAotSetLogicalFlags(state, value, " + carry + f", {live});")
    elif opcode == 0xA:  # CMP
        body.append(f"Oot3dAotSetSubtractFlags(state, {_reg(rn, pc)}, {operand}, {live});")
    elif opcode == 0xB:  # CMN
        body.append(f"Oot3dAotSetAddFlags(state, {_reg(rn, pc)}, {operand}, {live});")
    elif opcode == 0xC:  # ORR
        if rd == 15:
            raise LoweringError(f"ORR writes PC at 0x{pc:08X}")
        if setflags:
            setup, operand, carry = _logical_operand2(raw, pc)
            body.extend(setup)
        body.append(f"{_reg(rd)} = {_reg(rn, pc)} | {operand};")
        if setflags:
            body.append(f"Oot3dAotSetLogicalFlags(state, {_reg(rd)}, {carry}, {live});")
    elif opcode == 0xD:  # MOV
        if rd == 15:
            raise LoweringError(f"MOV writes PC at 0x{pc:08X}")
        if setflags:
            setup, operand, carry = _logical_operand2(raw, pc)
            body.extend(setup)
        body.append(f"{_reg(rd)} = {operand};")
        if setflags:
            body.append(f"Oot3dAotSetLogicalFlags(state, {_reg(rd)}, {carry}, {live});")
    elif opcode == 0xE:  # BIC
        if rd == 15:
            raise LoweringError(f"BIC writes PC at 0x{pc:08X}")
        if setflags:
            setup, operand, carry = _logical_operand2(raw, pc)
            body.extend(setup)
        body.append(f"{_reg(rd)} = {_reg(rn, pc)} & ~({operand});")
        if setflags:
            body.append(f"Oot3dAotSetLogicalFlags(state, {_reg(rd)}, {carry}, {live});")
    elif opcode == 0xF:  # MVN
        if rd == 15:
            raise LoweringError(f"MVN writes PC at 0x{pc:08X}")
        if setflags:
            setup, operand, carry = _logical_operand2(raw, pc)
            body.extend(setup)
        body.append(f"{_reg(rd)} = ~({operand});")
        if setflags:
            body.append(f"Oot3dAotSetLogicalFlags(state, {_reg(rd)}, {carry}, {live});")
    else:
        raise LoweringError(
            f"data-processing opcode {opcode} at 0x{pc:08X} is unsupported"
        )
    return _conditional(raw, body)


def _emit_count_leading_zeros(item: Instruction) -> list[str]:
    raw = item.raw
    rd = (raw >> 12) & 0xF
    rm = raw & 0xF
    if rd == 15:
        raise LoweringError(f"CLZ writes PC at 0x{item.pc:08X}")
    return _conditional(
        raw,
        [
            f"uint32_t value = {_reg(rm, item.pc)};",
            "uint32_t count = 0U;",
            "while (count < 32U && (value & 0x80000000U) == 0U) {",
            "    ++count;",
            "    value <<= 1U;",
            "}",
            f"{_reg(rd)} = count;",
        ],
    )


def _emit_wide_immediate(item: Instruction) -> list[str]:
    raw = item.raw
    rd = (raw >> 12) & 0xF
    if rd == 15:
        raise LoweringError(f"wide immediate writes PC at 0x{item.pc:08X}")
    immediate = ((raw >> 4) & 0xF000) | (raw & 0xFFF)
    if raw & (1 << 22):  # MOVT
        value = f"({_reg(rd)} & 0x0000FFFFU) | 0x{immediate << 16:08X}U"
    else:  # MOVW
        value = f"0x{immediate:08X}U"
    return _conditional(raw, [f"{_reg(rd)} = {value};"])


def _emit_saturating_arithmetic(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    form = raw & 0x0FF000F0
    if form not in {0x01000050, 0x01200050, 0x01400050, 0x01600050}:
        raise LoweringError(
            f"saturating arithmetic 0x{raw:08X} at 0x{pc:08X} is unsupported"
        )
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    rm = raw & 0xF
    if 15 in {rn, rd, rm}:
        raise LoweringError(f"saturating arithmetic uses PC at 0x{pc:08X}")
    double_right = form in {0x01400050, 0x01600050}
    subtract = form in {0x01200050, 0x01600050}
    body = [
        "bool saturated = false;",
        f"const int64_t left = static_cast<int32_t>({_reg(rm)});",
        f"int64_t right = static_cast<int32_t>({_reg(rn)});",
    ]
    if double_right:
        body.extend(
            [
                "right = static_cast<int32_t>(",
                "    Oot3dAotSaturateSigned32(right * 2, saturated));",
            ]
        )
    operator = "-" if subtract else "+"
    body.extend(
        [
            f"{_reg(rd)} = Oot3dAotSaturateSigned32(",
            f"    left {operator} right, saturated);",
            "Oot3dAotSetSaturationFlag(frame, saturated);",
        ]
    )
    return _conditional(raw, body)


def _emit_signed_half_multiply(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    form = raw & 0x0FF00090
    if form not in {0x01000080, 0x01400080, 0x01600080}:
        raise LoweringError(f"multiply 0x{raw:08X} at 0x{pc:08X} is unsupported")
    destination_hi = (raw >> 16) & 0xF
    accumulator_or_lo = (raw >> 12) & 0xF
    right_register = (raw >> 8) & 0xF
    left_register = raw & 0xF
    used = {destination_hi, right_register, left_register}
    if form != 0x01600080:
        used.add(accumulator_or_lo)
    if 15 in used:
        raise LoweringError(f"signed half multiply uses PC at 0x{pc:08X}")
    left_shift = 16 if raw & (1 << 5) else 0
    right_shift = 16 if raw & (1 << 6) else 0
    body = [
        "const int32_t left = static_cast<int16_t>(",
        f"    {_reg(left_register, pc)} >> {left_shift}U);",
        "const int32_t right = static_cast<int16_t>(",
        f"    {_reg(right_register, pc)} >> {right_shift}U);",
    ]
    if form == 0x01600080:  # SMULxy
        body.append(
            f"{_reg(destination_hi)} = static_cast<uint32_t>(left * right);"
        )
    elif form == 0x01000080:  # SMLAxy
        body.extend(
            [
                "const int64_t value = static_cast<int64_t>(left) * right +",
                f"    static_cast<int32_t>({_reg(accumulator_or_lo)});",
                "const bool saturated =",
                "    value < -INT64_C(2147483648) || value > INT64_C(2147483647);",
                f"{_reg(destination_hi)} = static_cast<uint32_t>(value);",
                "Oot3dAotSetSaturationFlag(frame, saturated);",
            ]
        )
    else:  # SMLALxy
        body.extend(
            [
                "const int64_t product = static_cast<int64_t>(left) * right;",
                "uint64_t value =",
                f"    (static_cast<uint64_t>({_reg(destination_hi)}) << 32U) |",
                f"    {_reg(accumulator_or_lo)};",
                "value += static_cast<uint64_t>(product);",
                f"{_reg(accumulator_or_lo)} = static_cast<uint32_t>(value);",
                f"{_reg(destination_hi)} = static_cast<uint32_t>(value >> 32U);",
            ]
        )
    return _conditional(raw, body)


def _emit_special_multiply(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    form = raw & 0x0FF000F0
    rd_hi = (raw >> 16) & 0xF
    rd_lo_or_acc = (raw >> 12) & 0xF
    rs = (raw >> 8) & 0xF
    rm = raw & 0xF
    if form == 0x00600090:  # MLS
        if 15 in {rd_hi, rd_lo_or_acc, rs, rm}:
            raise LoweringError(f"MLS uses PC at 0x{pc:08X}")
        return _conditional(
            raw,
            [
                "const uint32_t product = static_cast<uint32_t>(",
                f"    static_cast<uint64_t>({_reg(rm)}) * {_reg(rs)});",
                f"{_reg(rd_hi)} = {_reg(rd_lo_or_acc)} - product;",
            ],
        )
    if form == 0x00400090:  # UMAAL
        if 15 in {rd_hi, rd_lo_or_acc, rs, rm}:
            raise LoweringError(f"UMAAL uses PC at 0x{pc:08X}")
        return _conditional(
            raw,
            [
                f"uint64_t value = static_cast<uint64_t>({_reg(rm)}) * {_reg(rs)} +",
                f"                 {_reg(rd_lo_or_acc)} + {_reg(rd_hi)};",
                f"{_reg(rd_lo_or_acc)} = static_cast<uint32_t>(value);",
                f"{_reg(rd_hi)} = static_cast<uint32_t>(value >> 32U);",
            ],
        )
    raise LoweringError(
        f"special multiply 0x{raw:08X} at 0x{pc:08X} is unsupported"
    )


def _emit_dual_signed_long_multiply(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    form = raw & 0x0FF000D0
    if form not in {0x07400010, 0x07400050}:
        raise LoweringError(
            f"dual signed multiply 0x{raw:08X} at 0x{pc:08X} is unsupported"
        )
    rd_hi = (raw >> 16) & 0xF
    rd_lo = (raw >> 12) & 0xF
    rs = (raw >> 8) & 0xF
    rm = raw & 0xF
    if 15 in {rd_hi, rd_lo, rs, rm}:
        raise LoweringError(f"dual signed multiply uses PC at 0x{pc:08X}")
    exchange = bool(raw & (1 << 5))
    low_right_shift = 16 if exchange else 0
    high_right_shift = 0 if exchange else 16
    operator = "-" if form == 0x07400050 else "+"
    return _conditional(
        raw,
        [
            f"const uint32_t left = {_reg(rm)};",
            f"const uint32_t right = {_reg(rs)};",
            "const int64_t lowProduct =",
            "    static_cast<int16_t>(left) *",
            f"    static_cast<int16_t>(right >> {low_right_shift}U);",
            "const int64_t highProduct =",
            "    static_cast<int16_t>(left >> 16U) *",
            f"    static_cast<int16_t>(right >> {high_right_shift}U);",
            "uint64_t value =",
            f"    (static_cast<uint64_t>({_reg(rd_hi)}) << 32U) | {_reg(rd_lo)};",
            f"value += static_cast<uint64_t>(lowProduct {operator} highProduct);",
            f"{_reg(rd_lo)} = static_cast<uint32_t>(value);",
            f"{_reg(rd_hi)} = static_cast<uint32_t>(value >> 32U);",
        ],
    )


def _emit_multiply(
    item: Instruction, live_flags: Flag = Flag.NZCV
) -> list[str]:
    raw = item.raw
    pc = item.pc
    accumulate = bool(raw & (1 << 21))
    setflags = bool(raw & (1 << 20))
    destination = (raw >> 16) & 0xF
    accumulator = (raw >> 12) & 0xF
    right = (raw >> 8) & 0xF
    left = raw & 0xF
    used = {destination, right, left}
    if accumulate:
        used.add(accumulator)
    if 15 in used:
        raise LoweringError(f"multiply uses PC at 0x{pc:08X}")
    expression = (
        f"static_cast<uint64_t>({_reg(left)}) * {_reg(right)}"
    )
    if accumulate:
        expression += f" + {_reg(accumulator)}"
    body = [f"{_reg(destination)} = static_cast<uint32_t>({expression});"]
    if setflags:
        body.append(
            f"Oot3dAotSetNz(state, {_reg(destination)}, "
            f"{_cpp_flag_mask(live_flags)});"
        )
    return _conditional(raw, body)


def _emit_long_multiply(
    item: Instruction, live_flags: Flag = Flag.NZCV
) -> list[str]:
    raw = item.raw
    pc = item.pc
    form = raw & 0x0FE000F0
    if form not in {0x00800090, 0x00A00090, 0x00C00090, 0x00E00090}:
        raise LoweringError(
            f"long multiply 0x{raw:08X} at 0x{pc:08X} is unsupported"
        )
    rd_hi = (raw >> 16) & 0xF
    rd_lo = (raw >> 12) & 0xF
    rs = (raw >> 8) & 0xF
    rm = raw & 0xF
    if 15 in {rd_hi, rd_lo, rs, rm}:
        raise LoweringError(f"long multiply uses PC at 0x{pc:08X}")
    signed_product = bool(raw & (1 << 22))
    accumulate = bool(raw & (1 << 21))
    setflags = bool(raw & (1 << 20))
    body = [
        f"const uint32_t leftValue = {_reg(rm, pc)};",
        f"const uint32_t rightValue = {_reg(rs, pc)};",
    ]
    if signed_product:
        body.extend(
            [
                "const int64_t product =",
                "    static_cast<int64_t>(static_cast<int32_t>(leftValue)) *",
                "    static_cast<int64_t>(static_cast<int32_t>(rightValue));",
                "uint64_t value = static_cast<uint64_t>(product);",
            ]
        )
    else:
        body.append(
            "uint64_t value = static_cast<uint64_t>(leftValue) * rightValue;"
        )
    if accumulate:
        body.extend(
            [
                f"value += (static_cast<uint64_t>({_reg(rd_hi, pc)}) << 32U) |",
                f"         {_reg(rd_lo, pc)};",
            ]
        )
    body.extend(
        [
            f"{_reg(rd_lo)} = static_cast<uint32_t>(value);",
            f"{_reg(rd_hi)} = static_cast<uint32_t>(value >> 32U);",
        ]
    )
    if setflags:
        body.append(
            f"Oot3dAotSetNz64(state, value, {_cpp_flag_mask(live_flags)});"
        )
    return _conditional(raw, body)


def _emit_system(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    rt = (raw >> 12) & 0xF
    if ((raw >> 28) & 0xF) == 0xF or rt == 15:
        raise LoweringError(f"system instruction uses invalid Rt at 0x{pc:08X}")
    form = raw & 0x0FFF0FFF
    if form == 0x0E1D0F70:  # MRC TPIDRURW
        return _conditional(
            raw, [f"{_reg(rt)} = frame.Guest.thread_pointer;"]
        )
    if form in {0x0E070F9A, 0x0E070FBA}:  # legacy DSB/DMB
        return _conditional(
            raw,
            ["std::atomic_thread_fence(std::memory_order_seq_cst);"],
        )
    raise LoweringError(
        f"system instruction 0x{raw:08X} at 0x{pc:08X} is unsupported"
    )


def _emit_exclusive(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    opcode = (raw >> 20) & 0xFF
    load = bool(opcode & 1)
    if ((raw >> 28) & 0xF) == 0xF or opcode not in range(0x18, 0x20):
        raise LoweringError(
            f"exclusive transfer 0x{raw:08X} at 0x{pc:08X} is unsupported"
        )
    if load:
        valid = (raw & 0x0FF00FFF) == ((opcode << 20) | 0xF9F)
    else:
        valid = (raw & 0x0FF00FF0) == ((opcode << 20) | 0xF90)
    if not valid:
        raise LoweringError(
            f"exclusive transfer 0x{raw:08X} at 0x{pc:08X} is malformed"
        )
    size = {0x18: 4, 0x19: 4, 0x1A: 8, 0x1B: 8,
            0x1C: 1, 0x1D: 1, 0x1E: 2, 0x1F: 2}[opcode]
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    rt = rd if load else raw & 0xF
    if size == 8:
        rt &= 0xE
    if 15 in {rn, rd, rt} or (size == 8 and rt >= 14):
        raise LoweringError(f"exclusive transfer uses PC at 0x{pc:08X}")
    body = [f"const uint32_t address = {_reg(rn, pc)};"]
    if load:
        body.extend(
            [
                "uint64_t value = 0U;",
                "uint64_t token = 0U;",
                "uint32_t faultAddress = address;",
                f"if (!context.Memory.LoadExclusive(address, {size}U, &value,",
                "                                  &token, &faultAddress)) {",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, faultAddress);",
                "}",
                f"{_reg(rt)} = static_cast<uint32_t>(value);",
            ]
        )
        if size == 8:
            body.append(
                f"{_reg(rt + 1)} = static_cast<uint32_t>(value >> 32U);"
            )
        body.extend(
            [
                "frame.Guest.exclusive_address = address;",
                "frame.Guest.exclusive_token = token;",
                f"frame.Guest.exclusive_size = {size}U;",
                "frame.Guest.exclusive_valid = true;",
            ]
        )
    else:
        body.extend(
            [
                "a32::ExclusiveStoreResult storeResult =",
                "    a32::ExclusiveStoreResult::ReservationLost;",
                "if (frame.Guest.exclusive_valid &&",
                "    frame.Guest.exclusive_address == address &&",
                f"    frame.Guest.exclusive_size == {size}U) {{",
                f"    uint64_t value = {_reg(rt, pc)};",
            ]
        )
        if size == 8:
            body.extend(
                [
                    f"    value |= static_cast<uint64_t>({_reg(rt + 1, pc)})",
                    "             << 32U;",
                ]
            )
        body.extend(
            [
                "    uint32_t faultAddress = address;",
                "    storeResult = context.Memory.StoreExclusive(",
                f"        address, {size}U, value, frame.Guest.exclusive_token,",
                "        &faultAddress);",
                "    if (storeResult == a32::ExclusiveStoreResult::MemoryFault) {",
                f"        return Oot3dAotMemoryFault(context, 0x{pc:08X}U, faultAddress);",
                "    }",
                "}",
                "const bool success =",
                "    storeResult == a32::ExclusiveStoreResult::Success;",
                f"{_reg(rd)} = success ? 0U : 1U;",
                "frame.Guest.exclusive_address = 0U;",
                "frame.Guest.exclusive_token = 0U;",
                "frame.Guest.exclusive_size = 0U;",
                "frame.Guest.exclusive_valid = false;",
            ]
        )
    return _conditional(raw, body)


def _emit_media_dual(item: Instruction) -> list[str]:
    """SMLAD/SMLADX/SMLSD/SMLSDX and SMUAD/SMUADX/SMUSD/SMUSDX (32-bit dual 16x16 multiplies)."""
    raw = item.raw
    pc = item.pc
    form = raw & 0x0FF000D0
    if form not in {0x07000010, 0x07000050}:
        raise LoweringError(f"media instruction 0x{raw:08X} at 0x{pc:08X} is unsupported")
    rd = (raw >> 16) & 0xF
    ra = (raw >> 12) & 0xF  # 15 selects SMUAD/SMUSD (no accumulate)
    rm = (raw >> 8) & 0xF
    rn = raw & 0xF
    if 15 in {rd, rm, rn}:
        raise LoweringError(f"dual multiply uses PC at 0x{pc:08X}")
    exchange = bool(raw & (1 << 5))
    operator = "-" if form == 0x07000050 else "+"
    low_shift = 16 if exchange else 0
    high_shift = 0 if exchange else 16
    body = [
        f"const uint32_t left = {_reg(rn)};",
        f"const uint32_t right = {_reg(rm)};",
        "const int64_t lowProduct = static_cast<int64_t>(static_cast<int16_t>(left)) *",
        f"    static_cast<int16_t>(right >> {low_shift}U);",
        "const int64_t highProduct = static_cast<int64_t>(static_cast<int16_t>(left >> 16U)) *",
        f"    static_cast<int16_t>(right >> {high_shift}U);",
        f"int64_t result = lowProduct {operator} highProduct;",
    ]
    if ra != 15:
        body.append(f"result += static_cast<int32_t>({_reg(ra)});")
    body.extend(
        [
            "const uint32_t value = static_cast<uint32_t>(result);",
            f"{_reg(rd)} = value;",
            "Oot3dAotSetSaturationFlag(frame, result != static_cast<int32_t>(value));",
        ]
    )
    return _conditional(raw, body)


_PARALLEL_LANES = {
    0: ((0, 0, 0, "+"), (1, 1, 1, "+")),  # ADD16
    1: ((0, 0, 1, "-"), (1, 1, 0, "+")),  # ASX
    2: ((0, 0, 1, "+"), (1, 1, 0, "-")),  # SAX
    3: ((0, 0, 0, "-"), (1, 1, 1, "-")),  # SUB16
    4: tuple((lane, lane, lane, "+") for lane in range(4)),  # ADD8
    7: tuple((lane, lane, lane, "-") for lane in range(4)),  # SUB8
}
# prefix (bits 22:20) -> (signed, mode). GE flags are not modelled: MM3D has no SEL.
_PARALLEL_PREFIX = {
    1: (True, "modular"), 2: (True, "saturate"), 3: (True, "halve"),
    5: (False, "modular"), 6: (False, "saturate"), 7: (False, "halve"),
}


def _emit_parallel_arithmetic(item: Instruction) -> list[str]:
    """ARMv6 parallel add/subtract: S/Q/SH/U/UQ/UH x ADD16/ASX/SAX/SUB16/ADD8/SUB8.

    The GE flags written by the S* and U* forms are deliberately not modelled (nothing
    reads them without SEL). Tracked in docs/decisiones.md."""
    raw = item.raw
    pc = item.pc
    prefix = (raw >> 20) & 7
    operation = (raw >> 5) & 7
    if prefix not in _PARALLEL_PREFIX or operation not in _PARALLEL_LANES:
        raise LoweringError(f"parallel arithmetic 0x{raw:08X} at 0x{pc:08X} is unsupported")
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    rm = raw & 0xF
    if 15 in {rn, rd, rm}:
        raise LoweringError(f"parallel arithmetic uses PC at 0x{pc:08X}")
    signed, mode = _PARALLEL_PREFIX[prefix]
    width = 8 if operation in (4, 7) else 16
    mask = (1 << width) - 1
    value_type = f"{'' if signed else 'u'}int{width}_t"
    body = [
        f"const uint32_t left = {_reg(rn)};",
        f"const uint32_t right = {_reg(rm)};",
        "uint32_t value = 0U;",
    ]
    for destination, left_lane, right_lane, sign in _PARALLEL_LANES[operation]:
        left_value = f"static_cast<int32_t>(static_cast<{value_type}>(left >> {left_lane * width}U))"
        right_value = f"static_cast<int32_t>(static_cast<{value_type}>(right >> {right_lane * width}U))"
        body.append("{")
        body.append(f"    const int32_t a = {left_value};")
        body.append(f"    const int32_t b = {right_value};")
        body.append(f"    int32_t r = a {sign} b;")
        if mode == "saturate":
            low = -(1 << (width - 1)) if signed else 0
            high = (1 << (width - 1)) - 1 if signed else mask
            body.append(f"    if (r < {low}) {{ r = {low}; }} else if (r > {high}) {{ r = {high}; }}")
        elif mode == "halve":
            body.append("    r >>= 1;")
        body.append(
            f"    value |= (static_cast<uint32_t>(r) & 0x{mask:X}U) << {destination * width}U;"
        )
        body.append("}")
    body.append(f"{_reg(rd)} = value;")
    return _conditional(raw, body)


def _emit_saturate16(item: Instruction) -> list[str]:
    """SSAT16 / USAT16: saturate both signed halfwords of Rn."""
    raw = item.raw
    pc = item.pc
    signed = (raw & 0x0FF00FF0) == 0x06A00F30
    rd = (raw >> 12) & 0xF
    rm = raw & 0xF
    if 15 in {rd, rm}:
        raise LoweringError(f"saturate16 uses PC at 0x{pc:08X}")
    field = (raw >> 16) & 0xF
    if signed:
        bits = field + 1
        low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    else:
        low, high = 0, (1 << field) - 1
    body = [f"const uint32_t source = {_reg(rm)};", "uint32_t value = 0U;", "bool saturated = false;"]
    for lane in range(2):
        body.extend(
            [
                "{",
                f"    int32_t v = static_cast<int16_t>(source >> {lane * 16}U);",
                f"    if (v < {low}) {{ v = {low}; saturated = true; }}",
                f"    else if (v > {high}) {{ v = {high}; saturated = true; }}",
                f"    value |= (static_cast<uint32_t>(v) & 0xFFFFU) << {lane * 16}U;",
                "}",
            ]
        )
    body.extend([f"{_reg(rd)} = value;", "Oot3dAotSetSaturationFlag(frame, saturated);"])
    return _conditional(raw, body)


def _emit_extend_add16(item: Instruction) -> list[str]:
    """SXTAB16 / UXTAB16: add the zero/sign-extended bytes 0 and 2 of the rotated Rm to the halfwords of Rn."""
    raw = item.raw
    pc = item.pc
    unsigned = (raw & 0x0FF000F0) == 0x06C00070
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    rm = raw & 0xF
    if 15 in {rn, rd, rm}:
        raise LoweringError(f"extend-and-add uses PC at 0x{pc:08X}")
    rotate = ((raw >> 10) & 0x3) * 8
    byte_type = "uint8_t" if unsigned else "int8_t"
    return _conditional(
        raw,
        [
            f"const uint32_t source = Oot3dAotRor({_reg(rm, pc)}, {rotate}U);",
            f"const uint32_t base = {_reg(rn, pc)};",
            f"const uint32_t low = (base + static_cast<uint16_t>(static_cast<{byte_type}>(source))) & 0xFFFFU;",
            f"const uint32_t high = ((base >> 16U) + static_cast<uint16_t>(static_cast<{byte_type}>(source >> 16U))) & 0xFFFFU;",
            f"{_reg(rd)} = low | (high << 16U);",
        ],
    )


def _emit_extension(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    rd = (raw >> 12) & 0xF
    rm = raw & 0xF
    reversal = raw & 0x0FFF0FF0
    if reversal in {0x06BF0F30, 0x06BF0FB0, 0x06FF0FB0}:
        if 15 in {rd, rm}:
            raise LoweringError(f"byte reversal uses PC at 0x{pc:08X}")
        body = [f"const uint32_t source = {_reg(rm)};"]
        if reversal == 0x06BF0F30:  # REV
            body.extend(
                [
                    "const uint32_t value =",
                    "    ((source & 0x000000FFU) << 24U) |",
                    "    ((source & 0x0000FF00U) << 8U) |",
                    "    ((source & 0x00FF0000U) >> 8U) |",
                    "    ((source & 0xFF000000U) >> 24U);",
                ]
            )
        elif reversal == 0x06BF0FB0:  # REV16
            body.extend(
                [
                    "const uint32_t value =",
                    "    ((source & 0x00FF00FFU) << 8U) |",
                    "    ((source & 0xFF00FF00U) >> 8U);",
                ]
            )
        else:  # REVSH
            body.extend(
                [
                    "const uint32_t half =",
                    "    ((source & 0xFFU) << 8U) | ((source >> 8U) & 0xFFU);",
                    "const uint32_t value =",
                    "    (half & 0x8000U) != 0U ? half | 0xFFFF0000U : half;",
                ]
            )
        body.append(f"{_reg(rd)} = value;")
        return _conditional(raw, body)

    packing = raw & 0x0FF00070
    if packing in {0x06800010, 0x06800050}:  # PKHBT / PKHTB
        rn = (raw >> 16) & 0xF
        if 15 in {rn, rd, rm}:
            raise LoweringError(f"packing instruction uses PC at 0x{pc:08X}")
        amount = (raw >> 7) & 0x1F
        shift_type = 0 if packing == 0x06800010 else 2
        low_mask = "0x0000FFFFU" if shift_type == 0 else "0xFFFF0000U"
        high_mask = "0xFFFF0000U" if shift_type == 0 else "0x0000FFFFU"
        return _conditional(
            raw,
            [
                "const auto shifted = Oot3dAotShiftImmediate(",
                f"    {_reg(rm)}, {shift_type}U, {amount}U,",
                "    (frame.Guest.cpsr & a32::kFlagC) != 0U);",
                f"{_reg(rd)} = ({_reg(rn)} & {low_mask}) |",
                f"    (shifted.Value & {high_mask});",
            ],
        )

    saturation = raw & 0x0FE00030
    if saturation in {0x06A00010, 0x06E00010}:  # SSAT / USAT
        signed = saturation == 0x06A00010
        if 15 in {rd, rm}:
            raise LoweringError(f"saturation instruction uses PC at 0x{pc:08X}")
        shift_type = 2 if raw & (1 << 6) else 0
        amount = (raw >> 7) & 0x1F
        bits = ((raw >> 16) & 0x1F) + (1 if signed else 0)
        body = [
            "const auto shifted = Oot3dAotShiftImmediate(",
            f"    {_reg(rm)}, {shift_type}U, {amount}U,",
            "    (frame.Guest.cpsr & a32::kFlagC) != 0U);",
            "const int64_t source = static_cast<int32_t>(shifted.Value);",
        ]
        if signed:
            if bits == 32:
                low, high = "-INT64_C(2147483648)", "INT64_C(2147483647)"
            else:
                low = f"-(INT64_C(1) << {bits - 1}U)"
                high = f"(INT64_C(1) << {bits - 1}U) - 1"
        else:
            low = "INT64_C(0)"
            high = "INT64_C(0)" if bits == 0 else f"(INT64_C(1) << {bits}U) - 1"
        body.extend(
            [
                f"constexpr int64_t low = {low};",
                f"constexpr int64_t high = {high};",
                "const int64_t clamped = source < low ? low :",
                "                        source > high ? high : source;",
                f"{_reg(rd)} = static_cast<uint32_t>(clamped);",
                "Oot3dAotSetSaturationFlag(frame, clamped != source);",
            ]
        )
        return _conditional(raw, body)

    if (raw & 0x0FF000F0) == 0x066000F0:  # UQSUB8
        rn = (raw >> 16) & 0xF
        if 15 in {rn, rd, rm}:
            raise LoweringError(f"UQSUB8 uses PC at 0x{pc:08X}")
        return _conditional(
            raw,
            [
                f"const uint32_t left = {_reg(rn, pc)};",
                f"const uint32_t right = {_reg(rm, pc)};",
                "uint32_t value = 0U;",
                "for (unsigned lane = 0U; lane < 4U; ++lane) {",
                "    const unsigned shift = lane * 8U;",
                "    const unsigned a = (left >> shift) & 0xFFU;",
                "    const unsigned b = (right >> shift) & 0xFFU;",
                "    value |= (a > b ? a - b : 0U) << shift;",
                "}",
                f"{_reg(rd)} = value;",
            ],
        )
    if (raw & 0x0F800F10) == 0x06000F10:  # parallel add/subtract family
        return _emit_parallel_arithmetic(item)
    if (raw & 0x0FA00FF0) == 0x06A00F30:  # SSAT16 (0x06A..) / USAT16 (0x06E..)
        return _emit_saturate16(item)
    if (raw & 0x0FB000F0) == 0x06800070 and ((raw >> 16) & 0xF) != 15:  # SXTAB16 / UXTAB16
        return _emit_extend_add16(item)
    extension = raw & 0x0FF000F0
    signed = extension in {0x06800070, 0x06A00070, 0x06B00070}
    unsigned = extension in {0x06C00070, 0x06E00070, 0x06F00070}
    if not (signed or unsigned):
        raise LoweringError(f"media extension 0x{raw:08X} at 0x{pc:08X} is unsupported")
    rn = (raw >> 16) & 0xF
    if rd == 15:
        raise LoweringError(f"extension writes PC at 0x{pc:08X}")
    rotate = ((raw >> 10) & 0x3) * 8
    byte16 = extension in {0x06800070, 0x06C00070}
    if byte16 and rn != 15:
        raise LoweringError(
            f"extension-and-add pair at 0x{pc:08X} is unsupported"
        )
    byte = byte16 or extension in {0x06A00070, 0x06E00070}
    value_type = "int8_t" if byte else "int16_t"
    if unsigned:
        value_type = "uint8_t" if byte else "uint16_t"
    body = [
        f"const uint32_t source = Oot3dAotRor({_reg(rm, pc)}, {rotate}U);",
    ]
    if byte16:
        body.extend(
            [
                f"const uint32_t low = static_cast<uint16_t>(static_cast<{value_type}>(source));",
                f"const uint32_t high = static_cast<uint16_t>(static_cast<{value_type}>(source >> 16U));",
                "uint32_t value = low | (high << 16U);",
            ]
        )
    else:
        body.append(
            f"uint32_t value = static_cast<uint32_t>(static_cast<{value_type}>(source));"
        )
    if rn != 15:
        body.append(f"value += {_reg(rn, pc)};")
    body.append(f"{_reg(rd)} = value;")
    return _conditional(raw, body)


def _emit_extra_memory(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    operation = (raw >> 5) & 0x3
    encoded_load = bool(raw & (1 << 20))
    if operation not in {1, 2, 3}:
        raise LoweringError(
            f"miscellaneous transfer 0x{raw:08X} at 0x{pc:08X} is unsupported"
        )
    immediate = bool(raw & (1 << 22))
    pre_index = bool(raw & (1 << 24))
    add_offset = bool(raw & (1 << 23))
    writeback = bool(raw & (1 << 21)) or not pre_index
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    double_transfer = operation in {2, 3} and not encoded_load
    load = encoded_load or (operation == 2 and double_transfer)
    if rd == 15 or (double_transfer and rd >= 14):
        raise LoweringError(f"signed transfer writes PC at 0x{pc:08X}")
    if double_transfer and not pre_index and bool(raw & (1 << 21)):
        raise LoweringError(f"invalid doubleword T transfer at 0x{pc:08X}")
    offset = (
        f"0x{(((raw >> 4) & 0xF0) | (raw & 0xF)):X}U"
        if immediate
        else _reg(raw & 0xF, pc)
    )
    base = _reg(rn, pc)
    operator = "+" if add_offset else "-"
    value_type = "uint8_t" if operation == 2 else "uint16_t"
    body = [
        f"const uint32_t updatedAddress = {base} {operator} {offset};",
        f"const uint32_t address = {'updatedAddress' if pre_index else base};",
    ]
    if double_transfer and load:
        body.extend(
            [
                "uint64_t value = 0;",
                "uint32_t faultAddress = address;",
                "if (!context.Memory.ReadFast<uint64_t>(",
                "        address, &value, &faultAddress)) {",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, faultAddress);",
                "}",
                f"{_reg(rd)} = static_cast<uint32_t>(value);",
                f"{_reg(rd + 1)} = static_cast<uint32_t>(value >> 32U);",
            ]
        )
    elif double_transfer:
        body.extend(
            [
                f"const uint64_t value = static_cast<uint64_t>({_reg(rd + 1, pc)}) << 32U |",
                f"                       {_reg(rd, pc)};",
                "uint32_t faultAddress = address;",
                "if (!context.Memory.WriteFast<uint64_t>(",
                "        address, value, &faultAddress)) {",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, faultAddress);",
                "}",
            ]
        )
    elif load:
        body.extend(
            [
                f"{'uint8_t' if operation == 2 else 'uint16_t'} value = 0;",
                f"if (!context.Memory.ReadFast<{'uint8_t' if operation == 2 else 'uint16_t'}>(address, &value)) {{",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                "}",
            ]
        )
        if operation == 1:
            body.append(f"{_reg(rd)} = value;")
        else:
            signed_type = "int8_t" if operation == 2 else "int16_t"
            body.extend(
                [
                    f"{_reg(rd)} = static_cast<uint32_t>(static_cast<int32_t>(",
                    f"    static_cast<{signed_type}>(value)));",
                ]
            )
    else:
        value_type = "uint16_t"
        body.extend(
            [
                f"if (!context.Memory.WriteFast<{value_type}>(",
                f"        address, static_cast<{value_type}>({_reg(rd, pc)}))) {{",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                "}",
            ]
        )
    if writeback:
        if rn == 15:
            raise LoweringError(f"signed transfer writes PC base at 0x{pc:08X}")
        body.append(f"{_reg(rn)} = updatedAddress;")
    return _conditional(raw, body)


def _emit_single_memory(
    item: Instruction,
    *,
    allow_pc_load: bool = False,
    internal_pc_targets: frozenset[int] = frozenset(),
) -> list[str]:
    raw = item.raw
    pc = item.pc
    register_offset = bool(raw & (1 << 25))
    pre_index = bool(raw & (1 << 24))
    add_offset = bool(raw & (1 << 23))
    byte_transfer = bool(raw & (1 << 22))
    writeback = bool(raw & (1 << 21)) or not pre_index
    load = bool(raw & (1 << 20))
    rn = (raw >> 16) & 0xF
    rd = (raw >> 12) & 0xF
    offset = _shifted_register(raw, pc) if register_offset else f"0x{raw & 0xFFF:X}U"
    base = _reg(rn, pc)
    operator = "+" if add_offset else "-"
    if (
        _LITERAL_IMAGE is not None
        and load
        and rn == 15
        and not register_offset
        and pre_index
        and not writeback
        and not (rd == 15 and (not allow_pc_load or byte_transfer))
    ):
        # LDR/LDRB rd, [pc, #imm]: a literal-pool read from the immutable text
        # segment always yields the code.bin bytes and can never fault.
        address = (pc + 8 + (raw & 0xFFF if add_offset else -(raw & 0xFFF))) & 0xFFFFFFFF
        size = 1 if byte_transfer else 4
        literal = _LITERAL_IMAGE.word(address, size)
        if literal is not None:
            _FOLDED_LITERALS.add(address)
            value_type = "uint8_t" if byte_transfer else "uint32_t"
            body = [
                f"const {value_type} value = 0x{literal:0{size * 2}X}U; "
                f"// literal at 0x{address:08X}",
            ]
            if rd != 15:
                body.append(f"{_reg(rd)} = value;")
            elif internal_pc_targets:
                body.append("switch (value) {")
                for target in sorted(internal_pc_targets):
                    body.extend([f"case 0x{target:08X}U:", f"    goto block_{target:08X};"])
                body.extend(["default:", "    return Oot3dAotBranch(value);", "}"])
            else:
                body.append("return Oot3dAotBranch(value);")
            return _conditional(raw, body)
    body = [
        f"const uint32_t updatedAddress = {base} {operator} {offset};",
        f"const uint32_t address = {'updatedAddress' if pre_index else base};",
    ]
    if rd == 15 and (not allow_pc_load or not load or byte_transfer):
        raise LoweringError(f"single transfer uses PC destination at 0x{pc:08X}")
    if load:
        value_type = "uint8_t" if byte_transfer else "uint32_t"
        body.extend(
            [
                f"{value_type} value = 0;",
                f"if (!context.Memory.ReadFast<{value_type}>(address, &value)) {{",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                "}",
            ]
        )
        if rd != 15:
            body.append(f"{_reg(rd)} = value;")
    else:
        value_type = "uint8_t" if byte_transfer else "uint32_t"
        body.extend(
            [
                f"if (!context.Memory.WriteFast<{value_type}>(",
                f"        address, static_cast<{value_type}>({_reg(rd, pc)}))) {{",
                f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                "}",
            ]
        )
    if writeback:
        if rn == 15:
            raise LoweringError(f"single transfer writes PC base at 0x{pc:08X}")
        body.append(f"{_reg(rn)} = updatedAddress;")
    if load and rd == 15:
        if internal_pc_targets:
            body.append("switch (value) {")
            for target in sorted(internal_pc_targets):
                body.extend(
                    [
                        f"case 0x{target:08X}U:",
                        f"    goto block_{target:08X};",
                    ]
                )
            body.extend(
                [
                    "default:",
                    "    return Oot3dAotBranch(value);",
                    "}",
                ]
            )
        else:
            body.append("return Oot3dAotBranch(value);")
    return _conditional(raw, body)


def _emit_block_transfer(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    pre_index = bool(raw & (1 << 24))
    add_offset = bool(raw & (1 << 23))
    user_registers = bool(raw & (1 << 22))
    writeback = bool(raw & (1 << 21))
    load = bool(raw & (1 << 20))
    rn = (raw >> 16) & 0xF
    registers = tuple(index for index in range(16) if raw & (1 << index))
    if user_registers or rn == 15 or not registers:
        raise LoweringError(f"unsupported block transfer at 0x{pc:08X}")
    byte_count = len(registers) * 4
    if add_offset:
        first = f"{_reg(rn)}" + (" + 4U" if pre_index else "")
        updated = f"{_reg(rn)} + {byte_count}U"
    else:
        adjustment = byte_count if pre_index else byte_count - 4
        first = f"{_reg(rn)} - {adjustment}U"
        updated = f"{_reg(rn)} - {byte_count}U"
    body = [f"const uint32_t firstAddress = {first};"]
    count = len(registers)
    if count >= 2:
        # One page check and one copy when the whole transfer lies on a single
        # untraced fast page; otherwise the exact word-by-word path below,
        # which keeps fault order and partial writes. Values are snapshotted
        # first: the transfer itself never changes a source register.
        if load:
            body.append(f"uint32_t spanWords[{count}];")
            head = (f"if (!context.Memory.ReadWordsFast("
                    f"firstAddress, spanWords, {count}U)) {{")
            fallback = [
                line
                for offset in range(count)
                for line in (
                    "    if (!context.Memory.ReadFast<uint32_t>(",
                    f"            firstAddress + {offset * 4}U, &spanWords[{offset}])) {{",
                    f"        return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                                   firstAddress + {offset * 4}U);",
                    "    }",
                )
            ] if not _LEAN_STACK else [
                line
                for offset in range(count)
                for line in (
                    "    {",
                    "        uint32_t word;",
                    "        if (!context.Memory.ReadFast<uint32_t>(",
                    f"                firstAddress + {offset * 4}U, &word)) {{",
                    f"            return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                                       firstAddress + {offset * 4}U);",
                    "        }",
                    f"        spanWords[{offset}] = word;",
                    "    }",
                )
            ]
        else:
            sources = ", ".join(_reg(register, pc) for register in registers)
            body.append(f"const uint32_t spanWords[{count}] = {{{sources}}};")
            head = (f"if (!context.Memory.WriteWordsFast("
                    f"firstAddress, spanWords, {count}U)) {{")
            fallback = [
                line
                for offset in range(count)
                for line in (
                    "    if (!context.Memory.WriteFast<uint32_t>(",
                    f"            firstAddress + {offset * 4}U, spanWords[{offset}])) {{",
                    f"        return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                                   firstAddress + {offset * 4}U);",
                    "    }",
                )
            ]
        body.extend([head, *fallback, "}"])
        if load:
            for offset, register in enumerate(registers):
                if register != 15:
                    body.append(f"{_reg(register)} = spanWords[{offset}];")
        if writeback:
            body.append(f"{_reg(rn)} = {updated};")
        if load and 15 in registers:
            body.append(
                f"return Oot3dAotReturned(spanWords[{registers.index(15)}]);")
        return _conditional(raw, body)
    if load:
        for offset, register in enumerate(registers):
            body.extend(
                [
                    f"uint32_t value{register} = 0;",
                    f"if (!context.Memory.ReadFast<uint32_t>(",
                    f"        firstAddress + {offset * 4}U, &value{register})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                               firstAddress + {offset * 4}U);",
                    "}",
                ]
            )
        for register in registers:
            if register != 15:
                body.append(f"{_reg(register)} = value{register};")
    else:
        for offset, register in enumerate(registers):
            body.extend(
                [
                    "if (!context.Memory.WriteFast<uint32_t>(",
                    f"        firstAddress + {offset * 4}U, {_reg(register, pc)})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                               firstAddress + {offset * 4}U);",
                    "}",
                ]
            )
    if writeback:
        body.append(f"{_reg(rn)} = {updated};")
    if load and 15 in registers:
        body.append("return Oot3dAotReturned(value15);")
    return _conditional(raw, body)


def _vfp_lane(raw: int, register_shift: int, extension_bit: int) -> int:
    return ((raw >> register_shift) & 0xF) * 2 + ((raw >> extension_bit) & 1)


def _vfp_binary64_supported(raw: int) -> bool:
    if (raw & 0xF0000000) == 0xF0000000:
        return False
    ternary = raw & 0x0FB00F50
    if ternary in {
        0x0E000B00,
        0x0E000B40,
        0x0E200B00,
        0x0E300B00,
        0x0E300B40,
        0x0E800B00,
    }:
        return (raw & ((1 << 22) | (1 << 7) | (1 << 5))) == 0
    unary = raw & 0x0FBF0FD0
    if unary in {0x0EB10B40, 0x0EB10BC0, 0x0EB40B40, 0x0EB40BC0}:
        return (raw & ((1 << 22) | (1 << 5))) == 0
    if unary in {0x0EB80BC0, 0x0EB70AC0}:
        return (raw & (1 << 22)) == 0
    if unary in {0x0EBD0BC0, 0x0EBC0BC0, 0x0EB70BC0}:
        return (raw & (1 << 5)) == 0
    return False


_NATIVE_VFP_TERNARY = {
    0x0E000A00: "Oot3dVfpNativeMultiplyAccumulate",
    0x0E000A40: "Oot3dVfpNativeMultiplySubtract",
    0x0E100A40: "Oot3dVfpNativeNegativeMultiplyAccumulate",
    0x0E100A00: "Oot3dVfpNativeNegativeMultiplySubtract",
}
_NATIVE_VFP_BINARY = {
    0x0E200A00: "Oot3dVfpNativeMultiply",
    0x0E200A40: "Oot3dVfpNativeNegativeMultiply",
    0x0E300A00: "Oot3dVfpNativeAdd",
    0x0E300A40: "Oot3dVfpNativeSubtract",
    0x0E800A00: "Oot3dVfpNativeDivide",
}
_NATIVE_VFP_UNARY = {
    0x0EB10AC0: "Oot3dVfpNativeSquareRoot",
    0x0EB80AC0: "Oot3dVfpNativeFromSigned",
    0x0EB80A40: "Oot3dVfpNativeFromUnsigned",
    0x0EBC0AC0: "Oot3dVfpNativeToUnsigned",
    0x0EBD0AC0: "Oot3dVfpNativeToSigned",
}


def _native_vfp_name(operation: str | None) -> str | None:
    """Arithmetic entry point for the current option: Oot3dVfpNative* or,
    with --vfp-zero-aware, Oot3dVfpZeroAware* (conversions and compares
    have no zero-aware variant)."""
    if operation is None or not _VFP_ZERO_AWARE:
        return operation
    return operation.replace("Oot3dVfpNative", "Oot3dVfpZeroAware", 1)


def _emit_native_vfp(
    raw: int, pc: int, destination: int, left: int, right: int
) -> list[str] | None:
    """Scalar binary32 data processing through oot3d_native_a32_vfp_aarch64.h.

    None leaves the instruction to the software lowering (VABS/VNEG stay
    integer bit operations there)."""
    del pc
    ternary = raw & 0x0FB00F50
    operation = _native_vfp_name(_NATIVE_VFP_TERNARY.get(ternary))
    if operation is not None:
        return [
            f"{operation}({_lane(destination)}, {_lane(destination)},",
            f"    {_lane(left)}, {_lane(right)}, frame.Guest.fpscr);",
        ]
    operation = _native_vfp_name(_NATIVE_VFP_BINARY.get(ternary))
    if operation is not None:
        return [
            f"{operation}({_lane(destination)}, {_lane(left)}, {_lane(right)},",
            "    frame.Guest.fpscr);",
        ]
    unary = raw & 0x0FBF0FD0
    operation = _NATIVE_VFP_UNARY.get(unary)
    if unary == 0x0EB10AC0:
        operation = _native_vfp_name(operation)
    if operation is not None:
        return [
            f"{operation}({_lane(destination)}, {_lane(right)}, frame.Guest.fpscr);",
        ]
    if unary in {0x0EB40A40, 0x0EB40AC0}:
        return [
            f"Oot3dVfpNativeCompare({_lane(destination)}, {_lane(right)},",
            f"    {'true' if unary == 0x0EB40AC0 else 'false'}, frame.Guest.fpscr);",
        ]
    return None


def _vfp_word_span(
    load: bool, first: int, words: int, per_word: list[str]
) -> list[str]:
    """VLDM/VSTM/VPUSH/VPOP of `words` S lanes from `first` at firstAddress.

    Two or more words take one page check and one copy when the whole range
    lies on a single untraced fast page (ReadWordsFast/WriteWordsFast: the
    same bytes, generations and page stamps as the word-by-word transfer).
    Otherwise `per_word`, the exact word-by-word lowering, runs unchanged in
    its own scope, so fault order, partial stores and lane updates stay
    identical. A store snapshots the lanes first; guest memory never aliases
    the VFP registers, so the values are the same."""
    if words < 2:
        return per_word
    if load:
        return [
            f"uint32_t spanWords[{words}];",
            f"if (context.Memory.ReadWordsFast(firstAddress, spanWords, {words}U)) {{",
            *(
                f"    {_lane(first + offset)} = spanWords[{offset}];"
                for offset in range(words)
            ),
            "} else {",
            *(f"    {line}" for line in per_word),
            "}",
        ]
    sources = ", ".join(_lane(first + offset) for offset in range(words))
    return [
        f"const uint32_t spanWords[{words}] = {{{sources}}};",
        f"if (!context.Memory.WriteWordsFast(firstAddress, spanWords, {words}U)) {{",
        *(f"    {line}" for line in per_word),
        "}",
    ]


def _emit_vfp(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    body: list[str] = []
    push = (raw & 0x0FBF0E00) == 0x0D2D0A00
    pop = (raw & 0x0FBF0E00) == 0x0CBD0A00
    if push or pop:
        words = raw & 0xFF
        double_registers = bool(raw & (1 << 8))
        high_bit = bool(raw & (1 << 22))
        first = ((raw >> 12) & 0xF) * 2
        if not double_registers:
            first += 1 if high_bit else 0
        if (
            words == 0
            or (double_registers and (high_bit or words & 1))
            or first + words > 32
        ):
            raise LoweringError(f"invalid VFP push/pop at 0x{pc:08X}")
        byte_count = words * 4
        if push:
            body.extend(
                [
                    f"frame.Guest.r[13] -= {byte_count}U;",
                    "const uint32_t firstAddress = frame.Guest.r[13];",
                ]
            )
            per_word = [
                line
                for offset in range(words)
                for line in (
                    "if (!context.Memory.WriteFast<uint32_t>(",
                    f"        firstAddress + {offset * 4}U, {_lane(first + offset)})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                               firstAddress + {offset * 4}U);",
                    "}",
                )
            ]
            body.extend(_vfp_word_span(False, first, words, per_word))
        else:
            body.append("const uint32_t firstAddress = frame.Guest.r[13];")
            per_word = [
                line
                for offset in range(words)
                for line in (
                    f"uint32_t value{offset} = 0;",
                    "if (!context.Memory.ReadFast<uint32_t>(",
                    f"        firstAddress + {offset * 4}U, &value{offset})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                               firstAddress + {offset * 4}U);",
                    "}",
                    f"{_lane(first + offset)} = value{offset};",
                )
            ]
            body.extend(_vfp_word_span(True, first, words, per_word))
            body.append(f"frame.Guest.r[13] += {byte_count}U;")
        return _conditional(raw, body)
    if (raw & 0x0FF00FD0) in {0x0C400B10, 0x0C500B10}:
        to_core = bool(raw & (1 << 20))
        first_core = (raw >> 12) & 0xF
        second_core = (raw >> 16) & 0xF
        double_register = (raw & 0xF) + (((raw >> 5) & 1) * 16)
        if first_core == 15 or second_core == 15 or double_register >= 16:
            raise LoweringError(
                f"invalid VFP core-pair transport at 0x{pc:08X}"
            )
        first_lane = double_register * 2
        if to_core:
            body.extend(
                [
                    f"{_reg(first_core)} = {_lane(first_lane)};",
                    f"{_reg(second_core)} = {_lane(first_lane + 1)};",
                ]
            )
        else:
            body.extend(
                [
                    f"{_lane(first_lane)} = {_reg(first_core, pc)};",
                    f"{_lane(first_lane + 1)} = {_reg(second_core, pc)};",
                ]
            )
        return _conditional(raw, body)
    if (raw & 0x0F300E00) in {0x0D000A00, 0x0D100A00}:
        load = bool(raw & (1 << 20))
        add_offset = bool(raw & (1 << 23))
        rn = (raw >> 16) & 0xF
        double_register = bool(raw & (1 << 8))
        if (double_register and raw & (1 << 22)) or (not load and rn == 15):
            raise LoweringError(f"invalid VFP scalar memory transfer at 0x{pc:08X}")
        lane = _vfp_lane(raw, 12, 22)
        offset = (raw & 0xFF) * 4
        base = f"0x{(pc + 8) & ~3:08X}U" if rn == 15 else _reg(rn)
        operator = "+" if add_offset else "-"
        if _FOLD_VFP_LITERALS and _LITERAL_IMAGE is not None and load and rn == 15:
            # VLDR [pc, #imm]: a literal-pool read from the immutable text
            # segment (see _FOLD_VFP_LITERALS), folded like LDR [pc, #imm].
            address = (((pc + 8) & ~3) + (offset if add_offset else -offset)) & 0xFFFFFFFF
            size = 8 if double_register else 4
            literal = _LITERAL_IMAGE.word(address, size)
            if literal is not None:
                _FOLDED_VFP_LITERALS.add(address)
                body.append(f"// VFP literal at 0x{address:08X}")
                body.append(f"{_lane(lane)} = 0x{literal & 0xFFFFFFFF:08X}U;")
                if double_register:
                    body.append(f"{_lane(lane + 1)} = 0x{literal >> 32:08X}U;")
                return _conditional(raw, body)
        body.append(f"const uint32_t address = {base} {operator} {offset}U;")
        if double_register:
            # D0-D15 alias consecutive pairs of S registers, including the high word.
            if load:
                body.extend(
                    [
                        "uint64_t value = 0;",
                        "if (!context.Memory.ReadFast<uint64_t>(address, &value)) {",
                        f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                        "}",
                        f"{_lane(lane)} = static_cast<uint32_t>(value);",
                        f"{_lane(lane + 1)} = static_cast<uint32_t>(value >> 32U);",
                    ]
                )
            else:
                body.extend(
                    [
                        f"const uint64_t value = static_cast<uint64_t>({_lane(lane)}) |",
                        f"    (static_cast<uint64_t>({_lane(lane + 1)}) << 32U);",
                        "if (!context.Memory.WriteFast<uint64_t>(address, value)) {",
                        f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                        "}",
                    ]
                )
        elif load:
            body.extend(
                [
                    "uint32_t value = 0;",
                    "if (!context.Memory.ReadFast<uint32_t>(address, &value)) {",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                    "}",
                    f"{_lane(lane)} = value;",
                ]
            )
        else:
            body.extend(
                [
                    "if (!context.Memory.WriteFast<uint32_t>(",
                    f"        address, {_lane(lane)})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U, address);",
                    "}",
                ]
            )
        return _conditional(raw, body)
    if (raw & 0x0E000E00) == 0x0C000A00:
        pre_index = bool(raw & (1 << 24))
        add_offset = bool(raw & (1 << 23))
        high_bit = bool(raw & (1 << 22))
        writeback = bool(raw & (1 << 21))
        load = bool(raw & (1 << 20))
        rn = (raw >> 16) & 0xF
        double_registers = bool(raw & (1 << 8))
        words = raw & 0xFF
        first = ((raw >> 12) & 0xF) * 2
        if double_registers:
            if high_bit or words & 1:
                raise LoweringError(
                    f"unsupported VFP double-register list at 0x{pc:08X}"
                )
        else:
            first += 1 if high_bit else 0
        if (
            rn == 15
            or words == 0
            or first + words > 32
            or not ((not pre_index and add_offset) or
                    (pre_index and not add_offset))
        ):
            raise LoweringError(f"invalid VFP block transfer at 0x{pc:08X}")
        byte_count = words * 4
        if add_offset:
            first_address = _reg(rn)
            updated = f"{_reg(rn)} + {byte_count}U"
        else:
            first_address = f"{_reg(rn)} - {byte_count}U"
            updated = first_address
        body.append(f"const uint32_t firstAddress = {first_address};")
        if load:
            per_word = [
                line
                for offset in range(words)
                for line in (
                    f"uint32_t value{offset} = 0;",
                    "if (!context.Memory.ReadFast<uint32_t>(",
                    f"        firstAddress + {offset * 4}U, &value{offset})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                               firstAddress + {offset * 4}U);",
                    "}",
                )
            ] + [
                f"{_lane(first + offset)} = value{offset};"
                for offset in range(words)
            ]
        else:
            per_word = [
                line
                for offset in range(words)
                for line in (
                    "if (!context.Memory.WriteFast<uint32_t>(",
                    f"        firstAddress + {offset * 4}U, {_lane(first + offset)})) {{",
                    f"    return Oot3dAotMemoryFault(context, 0x{pc:08X}U,",
                    f"                               firstAddress + {offset * 4}U);",
                    "}",
                )
            ]
        body.extend(_vfp_word_span(load, first, words, per_word))
        if writeback:
            body.append(f"{_reg(rn)} = {updated};")
        return _conditional(raw, body)
    if (raw & 0x0FBF0ED0) == 0x0EB00A40:
        if raw & (1 << 8):
            if raw & ((1 << 22) | (1 << 5)):
                raise LoweringError(
                    f"binary64 VMOV uses D16-D31 at 0x{pc:08X}"
                )
            destination = ((raw >> 12) & 0xF) * 2
            source = (raw & 0xF) * 2
            return _conditional(
                raw,
                [
                    f"const uint32_t low = {_lane(source)};",
                    f"const uint32_t high = {_lane(source + 1)};",
                    f"{_lane(destination)} = low;",
                    f"{_lane(destination + 1)} = high;",
                ],
            )
        destination = _vfp_lane(raw, 12, 22)
        source = _vfp_lane(raw, 0, 5)
        return _conditional(raw, [f"{_lane(destination)} = {_lane(source)};"])
    if (raw & 0x0FF00F7F) == 0x0E000A10:
        core = (raw >> 12) & 0xF
        destination = _vfp_lane(raw, 16, 7)
        return _conditional(raw, [f"{_lane(destination)} = {_reg(core, pc)};"])
    if (raw & 0x0FF00F7F) == 0x0E100A10:
        core = (raw >> 12) & 0xF
        source = _vfp_lane(raw, 16, 7)
        return _conditional(raw, [f"{_reg(core)} = {_lane(source)};"])
    if (raw & 0x0FFF0FFF) == 0x0EF10A10:
        core = (raw >> 12) & 0xF
        if core == 15:
            statement = (
                "state.Flags.AssignPackedNzcv(frame.Guest.fpscr);"
            )
            return _conditional(raw, [statement])
        statements = [f"{_reg(core)} = frame.Guest.fpscr;"]
        if _NATIVE_VFP_AARCH64:
            # The cumulative flags accumulate in FPSR until read.
            statements.insert(0, "Oot3dGuestFpFoldFlags(frame.Guest.fpscr);")
        return _conditional(raw, statements)
    if (raw & 0x0FFF0FFF) == 0x0EE10A10:
        core = (raw >> 12) & 0xF
        if core == 15:
            raise LoweringError(f"VMSR uses PC at 0x{pc:08X}")
        statements = [f"frame.Guest.fpscr = {_reg(core, pc)};"]
        if _NATIVE_VFP_AARCH64:
            statements.append("Oot3dGuestFpLoadControl(frame.Guest.fpscr);")
        return _conditional(raw, statements)
    if _vfp_binary64_supported(raw):
        return _conditional(
            raw,
            [
                "const auto vfpFlow = a32::ExecuteVfpBinary64(",
                f"    0x{raw:08X}U, 0x{pc:08X}U, frame.Guest);",
                "if (vfpFlow.kind != a32::ExitKind::Fallthrough ||",
                f"    vfpFlow.pc != 0x{pc + 4:08X}U) {{",
                "    return {Oot3dWholeAotFlowKind::Unsupported,",
                f"            vfpFlow.pc, 0x{raw:08X}U}};",
                "}",
            ],
        )

    destination = _vfp_lane(raw, 12, 22)
    left = _vfp_lane(raw, 16, 7)
    right = _vfp_lane(raw, 0, 5)
    ternary = raw & 0x0FB00F50
    # Oot3dVfp* are inline host-float fast paths with the software operation
    # as their exact fallback (oot3d_native_a32_vfp_fast.h).
    operation = {
        0x0E000A00: "Oot3dVfpMultiplyAccumulate",
        0x0E000A40: "Oot3dVfpMultiplySubtract",
        0x0E100A40: "Oot3dVfpNegativeMultiplyAccumulate",
        0x0E100A00: "Oot3dVfpNegativeMultiplySubtract",
        0x0E200A00: "Oot3dVfpMultiply",
        0x0E200A40: "Oot3dVfpNegativeMultiply",
        0x0E300A00: "Oot3dVfpAdd",
        0x0E300A40: "Oot3dVfpSubtract",
        0x0E800A00: "a32::VfpBinary32Divide",
    }.get(ternary)
    body.extend([
        "if ((frame.Guest.fpscr & 0x00379F00U) != 0U) {",
        f"    return {{Oot3dWholeAotFlowKind::Unsupported, 0x{pc:08X}U, 0x{raw:08X}U}};",
        "}",
    ])
    if _NATIVE_VFP_AARCH64:
        native = _emit_native_vfp(raw, pc, destination, left, right)
        if native is not None:
            body.extend(native)
            return _conditional(raw, body)
    if operation is not None:
        arguments = [
            _lane(destination),
            _lane(left),
            _lane(right),
            "frame.Guest.fpscr",
        ] if "Accumulate" in operation or "Subtract" in operation and "Multiply" in operation else [
            _lane(left), _lane(right), "frame.Guest.fpscr"
        ]
        body.append(
            f"Oot3dAotCommitVfp({_lane(destination)}, frame.Guest.fpscr,"
        )
        body.append(f"    {operation}({', '.join(arguments)}));")
        return _conditional(raw, body)

    unary = raw & 0x0FBF0FD0
    if unary == 0x0EB00AC0:
        body.append(f"{_lane(destination)} = {_lane(right)} & 0x7FFFFFFFU;")
    elif unary == 0x0EB10A40:
        body.append(f"{_lane(destination)} = {_lane(right)} ^ 0x80000000U;")
    elif unary == 0x0EB10AC0:
        body.extend(
            [
                f"Oot3dAotCommitVfp({_lane(destination)}, frame.Guest.fpscr,",
                f"    a32::VfpBinary32SquareRoot({_lane(right)}, frame.Guest.fpscr));",
            ]
        )
    elif unary in {0x0EB40A40, 0x0EB40AC0}:
        body.extend(
            [
                "const auto comparison = a32::VfpBinary32Compare(",
                f"    {_lane(destination)}, {_lane(right)},",
                f"    {'true' if unary == 0x0EB40AC0 else 'false'});",
                "frame.Guest.fpscr = (frame.Guest.fpscr & ~0xF0000000U) |",
                "              (comparison.value & 0xF0000000U);",
                "frame.Guest.fpscr |= comparison.exception_flags;",
            ]
        )
    elif unary in {0x0EB80AC0, 0x0EB80A40}:
        operation = (
            "Oot3dVfpFromSigned" if unary == 0x0EB80AC0
            else "a32::VfpBinary32FromUnsigned"
        )
        body.extend(
            [
                f"Oot3dAotCommitVfp({_lane(destination)}, frame.Guest.fpscr,",
                f"    {operation}({_lane(right)}, frame.Guest.fpscr));",
            ]
        )
    elif unary == 0x0EBC0AC0:
        body.extend(
            [
                f"Oot3dAotCommitVfp({_lane(destination)}, frame.Guest.fpscr,",
                f"    a32::VfpBinary32ToUnsigned({_lane(right)}, frame.Guest.fpscr));",
            ]
        )
    elif unary == 0x0EBD0AC0:
        body.extend(
            [
                f"Oot3dAotCommitVfp({_lane(destination)}, frame.Guest.fpscr,",
                f"    a32::VfpBinary32ToSigned({_lane(right)}, frame.Guest.fpscr));",
            ]
        )
    else:
        raise LoweringError(f"VFP instruction 0x{raw:08X} at 0x{pc:08X} is unsupported")
    return _conditional(raw, body)


def _is_prefetch(raw: int) -> bool:
    return (
        (raw & 0xFF30F000) == 0xF510F000
        or (raw & 0xFF30F010) == 0xF710F000
        or (raw & 0xFF70F000) == 0xF450F000
        or (raw & 0xFF70F010) == 0xF650F000
    )


def _emit_program_status(item: Instruction) -> list[str]:
    raw = item.raw
    pc = item.pc
    if (raw & 0x0FBF0FFF) == 0x010F0000:  # MRS Rd, CPSR/SPSR
        if raw & (1 << 22):
            raise LoweringError(f"MRS SPSR at 0x{pc:08X} is unsupported")
        destination = (raw >> 12) & 0xF
        if destination == 15:
            raise LoweringError(f"MRS uses PC destination at 0x{pc:08X}")
        value = "state.Flags.Materialize(state.PreservedCpsr())"
        return _conditional(raw, [f"{_reg(destination)} = {value};"])
    if (raw & 0x0FB0FFF0) == 0x0120F000:  # MSR CPSR/SPSR_fields, Rm
        if raw & (1 << 22):
            raise LoweringError(f"MSR SPSR at 0x{pc:08X} is unsupported")
        fields = (raw >> 16) & 0xF
        source = raw & 0xF
        if fields != 0x8:
            raise LoweringError(
                f"MSR CPSR fields 0x{fields:X} at 0x{pc:08X} are unsupported"
            )
        return _conditional(
            raw,
            [
                f"const uint32_t value = {_reg(source, pc)};",
                "state.SetPreservedCpsrBits(0xFF000000U, value);",
                "state.Flags.AssignPackedNzcv(value);",
            ],
        )
    raise LoweringError(
        f"program-status instruction 0x{raw:08X} at 0x{pc:08X} is unsupported"
    )


def _emit_non_control(
    item: Instruction, live_flags: Flag = Flag.NZCV
) -> list[str]:
    raw = item.raw
    if raw == 0xF57FF01F:  # CLREX
        return [
            "{",
            "    frame.Guest.exclusive_address = 0U;",
            "    frame.Guest.exclusive_token = 0U;",
            "    frame.Guest.exclusive_size = 0U;",
            "    frame.Guest.exclusive_valid = false;",
            "}",
        ]
    if _is_prefetch(raw):
        return ["{", "}"]
    barrier = raw & 0xFFFFFFF0
    if barrier in {0xF57FF040, 0xF57FF050, 0xF57FF060}:
        return ["{"] + ["}"]
    if (raw & 0x0FFFFFFF) == 0x0320F000:  # NOP architectural hint
        return _conditional(raw, [])
    if (raw & 0x0FF00000) in {0x03000000, 0x03400000}:
        return _emit_wide_immediate(item)
    if (raw & 0x0FFF0FFF) in {0x0E1D0F70, 0x0E070F9A, 0x0E070FBA}:
        return _emit_system(item)
    opcode = (raw >> 20) & 0xFF
    if (
        ((raw >> 28) & 0xF) != 0xF
        and 0x18 <= opcode <= 0x1F
        and (raw & 0x00000FF0) == 0x00000F90
    ):
        return _emit_exclusive(item)
    if (raw & 0x0FF000D0) in {0x07400010, 0x07400050}:
        return _emit_dual_signed_long_multiply(item)
    if (raw & 0x0E000000) == 0x08000000:
        return _emit_block_transfer(item)
    if (raw & 0x0F000010) == 0x06000010:
        return _emit_extension(item)
    if (raw & 0x0F000010) == 0x07000010:
        # Media space (bits 27:24 = 0111, bit 4 = 1): SMLAD/SMUAD, USAD8, ... Nothing
        # in it is a load/store. Without this guard _emit_single_memory accepted it as
        # a register-offset STR/LDR and silently produced wrong code (MM3D: SMLAD).
        return _emit_media_dual(item)
    if (raw & 0x0C000000) == 0x04000000:
        return _emit_single_memory(item)
    if (raw & 0x0C000000) == 0x0C000000:
        return _emit_vfp(item)
    if (raw & 0x0C000000) == 0:
        if (
            (raw & 0x0FBF0FFF) == 0x010F0000
            or (raw & 0x0FB0FFF0) == 0x0120F000
        ):
            return _emit_program_status(item)
        if (raw & 0x0FF000F0) in {
            0x01000050,
            0x01200050,
            0x01400050,
            0x01600050,
        }:
            return _emit_saturating_arithmetic(item)
        if (raw & 0x0FFF0FF0) == 0x016F0F10:  # CLZ
            return _emit_count_leading_zeros(item)
        if (raw & 0x0FF000F0) in {0x00400090, 0x00600090}:
            return _emit_special_multiply(item)
        if (raw & 0x0FE000F0) in {
            0x00800090,
            0x00A00090,
            0x00C00090,
            0x00E00090,
        }:
            return _emit_long_multiply(item, live_flags)
        if (raw & 0x0FC000F0) == 0x00000090:
            return _emit_multiply(item, live_flags)
        if (raw & 0x0FF00090) in {
            0x01000080,
            0x01400080,
            0x01600080,
        }:
            return _emit_signed_half_multiply(item)
        if (raw & 0x0E000090) == 0x00000090:
            return _emit_extra_memory(item)
        return _emit_data_processing(item, live_flags)
    raise LoweringError(f"instruction 0x{raw:08X} at 0x{item.pc:08X} is unsupported")


def _branch_target(pc: int, raw: int) -> int:
    displacement = raw & 0x00FFFFFF
    if displacement & 0x00800000:
        displacement -= 0x01000000
    return (pc + 8 + displacement * 4) & 0xFFFFFFFF


def _goto(target: int, block_pcs: set[int]) -> str:
    if target in block_pcs:
        return f"goto block_{target:08X};"
    return f"return Oot3dAotBranch(0x{target:08X}U);"


def _emit_terminal(
    block: Block,
    block_pcs: set[int],
    direct_calls: dict[int, int],
    function_symbols: dict[int, str],
    external_entries: frozenset[int],
    live_flags: Flag = Flag.NZCV,
    promote: bool = False,
) -> list[str]:
    item = block.instructions[-1]
    raw = item.raw
    pc = item.pc

    # Architecturally inert hints can use encodings that overlap ordinary
    # loads to r15. Route them before the control-transfer decoders.
    if raw == 0xF57FF01F or _is_prefetch(raw) or (
        raw & 0xFFFFFFF0
    ) in {0xF57FF040, 0xF57FF050, 0xF57FF060}:
        lines = _emit_non_control(item, live_flags)
        fallthrough_edges = [
            edge for edge in block.successors
            if edge.get("kind") == "fallthrough"
        ]
        if len(fallthrough_edges) != 1:
            raise LoweringError(
                f"hint block 0x{block.pc:08X} has no unique fallthrough"
            )
        return [*lines, _goto(int(fallthrough_edges[0]["target"]), block_pcs)]
    if (raw & 0x0E000000) == 0x0A000000 and ((raw >> 28) & 0xF) != 0xF:
        if raw & (1 << 24):
            target = _branch_target(pc, raw)
            recorded_target = direct_calls.get(pc)
            if recorded_target != target:
                raise LoweringError(
                    f"direct call at 0x{pc:08X} has no matching program edge"
                )
            if target in function_symbols:
                call = (
                    f"Execute_{function_symbols[target]}(frame, context, state, "
                    f"0x{target:08X}U)"
                )
                call_prefix = ["commitState();"]
                call_suffix = ["reloadState();"]
            elif target in external_entries and promote:
                # No lambdas as arguments: the locals cannot escape.
                call = (
                    f"Oot3dAotCallExternal(context, 0x{target:08X}U, frame, state)"
                )
                call_prefix = ["commitState();"]
                call_suffix = ["reloadState();"]
            elif target in external_entries:
                call = (
                    f"Oot3dAotCallExternal(context, 0x{target:08X}U, frame, state, "
                    "commitState, reloadState)"
                )
                call_prefix = []
                call_suffix = []
            else:
                raise LoweringError(
                    f"direct call target 0x{target:08X} is not selected or external"
                )
            return_pc = pc + 4
            body = [
                "OOT3D_AOT_STAT(context.Stats.DirectCalls);",
                f"state.R[14] = 0x{return_pc:08X}U;",
                *call_prefix,
                f"const Oot3dWholeAotFlow callFlow = {call};",
                *call_suffix,
                "if (callFlow.Kind != Oot3dWholeAotFlowKind::Returned ||",
                f"    callFlow.Pc != 0x{return_pc:08X}U) {{",
                "    return callFlow;",
                "}",
            ]
            return [
                *_conditional(raw, body),
                _goto(return_pc, block_pcs),
            ]
        target = _branch_target(pc, raw)
        branch = _goto(target, block_pcs)
        normalized_guaranteed = (
            any(
                edge.get("kind") == "branch"
                and int(edge.get("target", -1)) == target
                and edge.get("condition") is None
                for edge in block.successors
            )
            and not any(
                edge.get("kind") == "fallthrough" for edge in block.successors
            )
        )
        if ((raw >> 28) & 0xF) == 0xE or normalized_guaranteed:
            return [branch]
        fallthrough = pc + 4
        return [
            *_conditional(raw, [branch]),
            _goto(fallthrough, block_pcs),
        ]
    if (raw & 0x0FFFFFF0) == 0x012FFF10:
        register = raw & 0xF
        transfer = (
            f"return Oot3dAotReturned({_reg(register, pc)});"
            if register == 14
            else f"return Oot3dAotBranch({_reg(register, pc)});"
        )
        if ((raw >> 28) & 0xF) == 0xE:
            return [transfer]
        return [
            *_conditional(raw, [transfer]),
            _goto(pc + 4, block_pcs),
        ]
    if (raw & 0x0FFFFFF0) == 0x012FFF30:
        register = raw & 0xF
        body = [
            "OOT3D_AOT_STAT(context.Stats.IndirectCalls);",
            f"state.R[14] = 0x{pc + 4:08X}U;",
            "commitState();",
            "const Oot3dWholeAotFlow callFlow =",
            "    ExecuteOot3dWholeAotIndirect(",
            f"        {_reg(register, pc)}, frame, context, state);",
            "reloadState();",
            "if (callFlow.Kind != Oot3dWholeAotFlowKind::Returned ||",
            f"    callFlow.Pc != 0x{pc + 4:08X}U) {{",
            "    return callFlow;",
            "}",
        ]
        return [*_conditional(raw, body), _goto(pc + 4, block_pcs)]
    if (
        (raw & 0x0C000000) == 0
        and ((raw >> 21) & 0xF) == 0x4
        and ((raw >> 12) & 0xF) == 15
    ):
        if raw & (1 << 20):
            raise LoweringError(f"ADD pc with S bit at 0x{pc:08X} is unsupported")
        target = f"({_reg((raw >> 16) & 0xF, pc)} + {_operand2(raw, pc)})"
        indirect_call = any(
            edge.get("kind") == "indirect_call" for edge in block.successors
        )
        if indirect_call:
            resume_edges = [
                edge for edge in block.successors
                if edge.get("kind") == "resume"
            ]
            if len(resume_edges) != 1:
                raise LoweringError(
                    f"computed call at 0x{pc:08X} has no unique resume"
                )
            return_pc = int(resume_edges[0]["target"])
            body = [
                "OOT3D_AOT_STAT(context.Stats.IndirectCalls);",
                "commitState();",
                "const Oot3dWholeAotFlow callFlow =",
                "    ExecuteOot3dWholeAotIndirect(",
                f"        {target}, frame, context, state);",
                "reloadState();",
                "if (callFlow.Kind != Oot3dWholeAotFlowKind::Returned ||",
                f"    callFlow.Pc != 0x{return_pc:08X}U) {{",
                "    return callFlow;",
                "}",
            ]
            if ((raw >> 28) & 0xF) == 0xE:
                return [*body, _goto(return_pc, block_pcs)]
            return [*_conditional(raw, [*body, _goto(return_pc, block_pcs)]),
                    _goto(pc + 4, block_pcs)]
        transfer = f"return Oot3dAotBranch({target});"
        if ((raw >> 28) & 0xF) == 0xE:
            return [transfer]
        return [
            *_conditional(raw, [transfer]),
            _goto(pc + 4, block_pcs),
        ]
    if (
        (raw & 0x0C000000) == 0
        and ((raw >> 21) & 0xF) == 0xD
        and ((raw >> 12) & 0xF) == 15
    ):
        target = _operand2(raw, pc)
        is_link_return = (raw & 0x02000FFF) == 0x0000000E
        transfer = (
            f"return Oot3dAotReturned({target});"
            if is_link_return
            else f"return Oot3dAotBranch({target});"
        )
        if ((raw >> 28) & 0xF) == 0xE:
            return [transfer]
        return [
            *_conditional(raw, [transfer]),
            _goto(pc + 4, block_pcs),
        ]
    if (
        (raw & 0x0E000000) == 0x08000000
        and raw & (1 << 20)
        and raw & (1 << 15)
    ):
        lines = _emit_non_control(item, live_flags)
        if ((raw >> 28) & 0xF) == 0xE:
            return lines
        return [*lines, _goto(pc + 4, block_pcs)]
    if (
        (raw & 0x0C000000) == 0x04000000
        and ((raw >> 12) & 0xF) == 15
    ):
        lines = _emit_single_memory(
            item,
            allow_pc_load=True,
            internal_pc_targets=frozenset(block_pcs),
        )
        if ((raw >> 28) & 0xF) == 0xE:
            return lines
        return [*lines, _goto(pc + 4, block_pcs)]
    if (raw & 0x0F000000) == 0x0F000000:
        lines = _conditional(
            raw,
            [
                f"return Oot3dAotSvc(context, 0x{pc:08X}U, "
                f"0x{raw & 0x00FFFFFF:06X}U);",
            ],
        )
        if ((raw >> 28) & 0xF) == 0xE:
            return lines
        return [*lines, _goto(pc + 4, block_pcs)]
    lines = _emit_non_control(item, live_flags)
    fallthrough_edges = [
        edge for edge in block.successors
        if edge.get("kind") == "fallthrough"
    ]
    if len(fallthrough_edges) != 1:
        raise LoweringError(f"block 0x{block.pc:08X} has no unique fallthrough")
    target = int(fallthrough_edges[0]["target"])
    return [*lines, _goto(target, block_pcs)]


def _symbol(name: str, entry: int) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not sanitized or sanitized[0].isdigit():
        sanitized = "Function_" + sanitized
    return f"{sanitized}_{entry:08X}"


def _sparse_charged_blocks(
    function: Function, bodies: dict[int, list[str]]
) -> frozenset[int]:
    """--sparse-block-entries: the blocks that keep a budget decrement.

    The function entry, every gated block (_HOOK_GATES) and every target of a
    back edge of a depth-first search of the goto graph (the emitted
    `goto block_X;`, which covers branches, fallthroughs, call returns and
    internal PC tables). Every directed cycle contains a DFS back edge whose
    target lies on the cycle, so no loop, irreducible or not, avoids a
    decrement; the check below re-proves it on the result."""
    successors = {
        pc: sorted({int(target, 16) for line in body for target in _GOTO_BLOCK.findall(line)})
        for pc, body in bodies.items()
    }
    for pc, targets in successors.items():
        stray = [target for target in targets if target not in successors]
        if stray:
            raise LoweringError(
                f"block 0x{pc:08X} jumps to 0x{stray[0]:08X} outside function 0x{function.entry:08X}")
    charged = {function.entry} | {pc for pc in successors if pc in _HOOK_GATES}
    on_stack: set[int] = set()
    done: set[int] = set()
    roots = [function.entry, *(block.pc for block in function.blocks if block.pc != function.entry)]
    for root in roots:
        if root in done:
            continue
        on_stack.add(root)
        stack = [(root, iter(successors[root]))]
        while stack:
            node, pending = stack[-1]
            target = next(pending, None)
            if target is None:
                stack.pop()
                on_stack.discard(node)
                done.add(node)
            elif target in on_stack:
                charged.add(target)  # back edge: target is on the cycle
            elif target not in done:
                on_stack.add(target)
                stack.append((target, iter(successors[target])))
    # Proof check: the blocks without a decrement form an acyclic graph.
    free = {pc for pc in successors if pc not in charged}
    indegree = {pc: 0 for pc in free}
    for pc in free:
        for target in successors[pc]:
            if target in free:
                indegree[target] += 1
    ready = [pc for pc, count in indegree.items() if count == 0]
    removed = 0
    while ready:
        pc = ready.pop()
        removed += 1
        for target in successors[pc]:
            if target in free:
                indegree[target] -= 1
                if indegree[target] == 0:
                    ready.append(target)
    if removed != len(free):
        raise LoweringError(f"function 0x{function.entry:08X}: a loop has no budget decrement")
    return frozenset(charged)



def _render_function(
    function: Function,
    function_symbols: dict[int, str],
    external_entries: frozenset[int],
    *,
    callback_free: bool = False,
    symbol_suffix: str = "",
    memory_view: bool = False,
    local_state: bool = False,
    memory_copy: bool = False,
) -> list[str]:
    symbol = _symbol(function.name, function.entry) + symbol_suffix
    block_pcs = {block.pc for block in function.blocks}
    optimization = analyze_function(function)
    instruction_optimization = {
        item.pc: item
        for block in optimization.blocks
        for item in block.instructions
    }
    promoted_registers = tuple(
        index for index in range(15)
        if optimization.promoted_gprs & (1 << index)
    )
    # Broad residency regressed the matched full-title workload. Keep local
    # state only at the existing explicitly selected callback-free boundaries.
    local_state = local_state or callback_free or memory_view
    # Broad promotion (see _promote_function_body); the regions and copies above
    # keep their own mechanism.
    promote = (_PROMOTE_REGISTERS and not local_state and not memory_copy)
    lines = [
        f"Oot3dWholeAotFlow Execute_{symbol}(",
        "    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,",
        "    Oot3dAotArchitecturalState& state,",
        "    uint32_t entryPc) {",
    ]
    lines.extend(
        f"    uint32_t{' ' if local_state else '& '}r{index} = state.R[{index}];"
        for index in promoted_registers
        if not promote
    )
    if optimization.promoted_flags != Flag.NONE and not promote:
        lines.append("    Oot3dAotLazyFlags flags = state.Flags;" if local_state
                     else "    auto& flags = state.Flags;")
    if memory_view:
        lines.extend([
            "    NativeA32UnobservedMemory regionMemory(context.Memory);",
            "    uint32_t regionBlocksRemaining = context.BlocksRemaining;",
        ])
    # Calls and observers commit before they can unwind. Ordinary exits commit
    # explicitly below: no caller cleanup may overwrite newer callee state.
    if not promote:
        lines.append("    const auto commitState = [&]() noexcept {")
    if local_state:
        lines.extend(f"        state.R[{index}] = r{index};" for index in promoted_registers)
        if optimization.promoted_flags != Flag.NONE:
            lines.append("        state.Flags = flags;")
    if memory_view:
        lines.extend([
            # Region stores mark their cards as they go: nothing to publish.
            # Consumed blocks are derived (BlockBudget - BlocksRemaining).
            "        context.BlocksRemaining = regionBlocksRemaining;",
        ])
    if not promote:
        lines.extend([
            "    };",
            "    const auto reloadState = [&]() noexcept {",
        ])
    if local_state:
        lines.extend(f"        r{index} = state.R[{index}];" for index in promoted_registers)
        if optimization.promoted_flags != Flag.NONE:
            lines.append("        flags = state.Flags;")
    if not promote:
        lines.append("    };")
    # Instructions of each block (independent of its entry code).
    bodies: dict[int, list[str]] = {}
    for block in function.blocks:
        body: list[str] = []
        for item in block.instructions[:-1]:
            live_flags = instruction_optimization[item.pc].required_flag_writes
            for emitted in _emit_non_control(item, live_flags):
                body.append("        " + emitted)
        terminal_flags = instruction_optimization[
            block.instructions[-1].pc
        ].required_flag_writes
        for emitted in _emit_terminal(
            block,
            block_pcs,
            dict(function.direct_calls),
            function_symbols,
            external_entries,
            terminal_flags,
            promote=promote,
        ):
            body.append("        " + emitted)
        bodies[block.pc] = body
    dispatch = (function.entry, *_function_dispatch_entries(function))
    lines.append("    switch (entryPc) {")
    if _SPARSE_BLOCK_ENTRIES:
        charged = _sparse_charged_blocks(function, bodies)
        uncharged_dispatch = [entry for entry in dispatch if entry not in charged]
        gated_blocks = sum(1 for pc in block_pcs if pc in _HOOK_GATES)
        for key, value in (
            ("blocks", len(block_pcs)),
            ("gated_blocks", gated_blocks),
            ("budget_only_blocks", len(charged) - gated_blocks),
            ("blocks_without_entry_code", len(block_pcs) - len(charged)),
        ):
            _SPARSE_STATS[key] = _SPARSE_STATS.get(key, 0) + value
        for entry in dispatch:
            if entry in charged:
                lines.extend([f"    case 0x{entry:08X}U:", f"        goto block_{entry:08X};"])
        lines.extend(f"    case 0x{entry:08X}U:" for entry in uncharged_dispatch)
        if uncharged_dispatch:
            lines.append("        break;")
        lines.extend([
            "    default:",
            "        return {Oot3dWholeAotFlowKind::Unsupported, entryPc, 0U};",
            "    }",
        ])
        if uncharged_dispatch:
            # A block without its own decrement is charged by its dispatch
            # (resume, indirect call into the block, registry re-dispatch).
            lines.extend([
                "    if (!Oot3dAotConsumeBlock(context)) {",
                "        return Oot3dAotBlockLimit(context, entryPc);",
                "    }",
                "    switch (entryPc) {",
            ])
            for entry in uncharged_dispatch:
                lines.extend([f"    case 0x{entry:08X}U:", f"        goto block_{entry:08X};"])
            lines.extend([
                "    default:",
                "        return {Oot3dWholeAotFlowKind::Unsupported, entryPc, 0U};",
                "    }",
            ])
    else:
        charged = frozenset(block_pcs)
        for entry in dispatch:
            lines.extend([f"    case 0x{entry:08X}U:", f"        goto block_{entry:08X};"])
        lines.extend([
            "    default:",
            "        return {Oot3dWholeAotFlowKind::Unsupported, entryPc, 0U};",
            "    }",
        ])
    for block in function.blocks:
        lines.append(f"block_{block.pc:08X}:")
        lines.append("    {")
        copy = carried_pair_copy(block) if memory_copy else None
        if copy is not None:
            live = instruction_optimization[block.instructions[3].pc].required_flag_writes
            registers = ", ".join(f"state.R[{index}]" for index in (
                copy.source, copy.destination, copy.count, copy.carried, copy.scratch))
            lines.extend([
                "        {",
                "            NativeA32UnobservedMemory copyMemory(context.Memory);",
                f"            if (copyMemory.CopyPairs({registers},",
                f"                    state.Flags, context, 0x{block.pc:08X}U, {_cpp_flag_mask(live)})) {{",
                f"                if (state.R[{copy.count}] != 0U) goto block_{block.pc:08X};",
                "                " + _goto(block.instructions[-1].pc + 4, block_pcs),
                "            }",
                "        }",
            ])
        gated = not _SPARSE_BLOCK_ENTRIES or block.pc in _HOOK_GATES
        enter = (
            "(regionBlocksRemaining != 0U)" if memory_view else
            "Oot3dAotConsumeBlock(context)" if callback_free or not gated else
            f"Oot3dAotEnterBlock(context, frame, state, 0x{block.pc:08X}U, commitState, reloadState)"
        )
        if promote:
            # Local budget; the (rare) hook synchronizes everything around the
            # cold half, which receives no locals.
            lines.extend([
                "        if (blocksRemaining == 0U) [[unlikely]] {",
                f"            return Oot3dAotBlockLimit(context, 0x{block.pc:08X}U);",
                "        }",
                "        --blocksRemaining;",
                f"        if (context.BlockEntryGate->MayContain(0x{block.pc:08X}U)) [[unlikely]] {{",
                # Becomes the dirty-set stores (_promote_function_body).
                "            commitState();",
                f"            Oot3dAotRunBlockEntryHook(context, frame, state, 0x{block.pc:08X}U);",
                "            reloadState();",
                "        }",
            ])
        elif block.pc in charged:
            lines.extend(
                [
                    f"        if (!{enter}) {{",
                    f"            return Oot3dAotBlockLimit(context, 0x{block.pc:08X}U);",
                    "        }",
                ]
            )
        if memory_view:
            lines.append("        --regionBlocksRemaining;")
        lines.extend(bodies[block.pc])
        lines.append("    }")
    lines.extend(
        [
            "    return {Oot3dWholeAotFlowKind::Unsupported, 0U, 0U};",
            "}",
            "",
        ]
    )
    promoted_helpers = (
        "Oot3dAotSetNz", "Oot3dAotSetNz64",
        "Oot3dAotSetLogicalFlags", "Oot3dAotSetSubtractFlags",
        "Oot3dAotSetAddFlags", "Oot3dAotAddWithCarry",
    )
    body_start = lines.index("    switch (entryPc) {")
    for index in range(body_start, len(lines)):
        line = lines[index]
        line = re.sub(r"frame\.Guest\.r\[(\d+)\]", r"state.R[\1]", line)
        line = line.replace(
            "(frame.Guest.cpsr & a32::kFlagC) != 0U", "state.Flags.C()"
        )
        if memory_view:
            line = line.replace("context.Memory.ReadFast<", "regionMemory.ReadFast<")
            line = line.replace("context.Memory.WriteFast<", "regionMemory.WriteFast<")
            line = line.replace("context.Memory.ReadWordsFast(", "regionMemory.ReadWordsFast(")
            line = line.replace("context.Memory.WriteWordsFast(", "regionMemory.WriteWordsFast(")
        for helper in promoted_helpers:
            line = line.replace(f"{helper}(frame,", f"{helper}(state,")
        line = line.replace(
            "Oot3dAotSetSaturationFlag(frame,",
            "Oot3dAotSetSaturationFlag(state,",
        )
        if not promote:
            for register in promoted_registers:
                line = line.replace(f"state.R[{register}]", f"r{register}")
        if optimization.promoted_flags != Flag.NONE and not promote:
            line = line.replace("state.Flags", "flags")
            for helper in promoted_helpers:
                line = line.replace(f"{helper}(state,", f"{helper}(flags,")
        lines[index] = line
    if _LEAN_STACK and not memory_view:
        _drop_read_zero_initializers(lines, body_start)
    if promote:
        return _promote_function_body(function, lines, body_start, promoted_helpers)
    committed = lines[:body_start]
    for line in lines[body_start:]:
        if line.lstrip().startswith("return "):
            committed.append(line[:len(line) - len(line.lstrip())] + "commitState();")
        elif re.search(r"\breturn\b", line):
            raise LoweringError(
                f"function 0x{function.entry:08X}: return must be standalone: {line.strip()}"
            )
        committed.append(line)
    return committed


def _promoted_prologue(registers: tuple[int, ...], has_flags: bool) -> list[str]:
    """Locals and full synchronizations of a promoted function.

    The lambdas are always_inline and only called directly: a local whose
    address escaped into a non-inline function would have to live on the stack.
    """
    attribute = "__attribute__((always_inline))"
    lines = [f"    uint32_t r{index} = state.R[{index}];" for index in registers]
    if has_flags:
        lines.append("    Oot3dAotLazyFlags flags = state.Flags;")
    lines += [
        "    uint32_t blocksRemaining = context.BlocksRemaining;",
        "    Oot3dWholeAotFlow exitFlow{};",
        f"    const auto commitAllState = [&]() noexcept {attribute} {{",
        *(f"        state.R[{index}] = r{index};" for index in registers),
        *(["        state.Flags = flags;"] if has_flags else []),
        "        context.BlocksRemaining = blocksRemaining;",
        "    };",
        # Name used by the native override prologues.
        f"    const auto commitState = [&]() noexcept {attribute} {{",
        "        commitAllState();",
        "    };",
        f"    const auto reloadState = [&]() noexcept {attribute} {{",
        *(f"        r{index} = state.R[{index}];" for index in registers),
        *(["        flags = state.Flags;"] if has_flags else []),
        "        blocksRemaining = context.BlocksRemaining;",
        "    };",
    ]
    return lines


_BLOCK_LABEL = re.compile(r"^block_([0-9A-F]{8}):$")
_GOTO_BLOCK = re.compile(r"\bgoto block_([0-9A-F]{8});")
_REGISTER_WRITE = re.compile(
    r"(?<![\w.>])r(\d+)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)"
    r"|(?:\+\+|--)\s*r(\d+)\b|(?<![\w.>])r(\d+)\s*(?:\+\+|--)")

# Helpers that take registers only by value (oot3d_native_whole_aot_runtime.h,
# oot3d_aot_architectural_state.h); any other one receiving a promoted local
# is rejected by _promote_function_body.
_BY_VALUE_REGISTER_CALLEES = frozenset({
    "static_cast", "Oot3dAotLsl", "Oot3dAotLsr", "Oot3dAotAsr", "Oot3dAotRor",
    "Oot3dAotShiftRegister", "Oot3dAotShiftImmediate", "Oot3dAotReturned",
    "Oot3dAotBranch", "Oot3dAotSetNz", "Oot3dAotSetNz64", "Oot3dAotSetLogicalFlags",
    "Oot3dAotSetSubtractFlags", "Oot3dAotSetAddFlags", "Oot3dAotAddWithCarry",
    "Oot3dAotSaturateSigned32", "ExecuteOot3dWholeAotIndirect", "Oot3dAotMemoryFault",
    "Oot3dAotRegisterArgument",
})
_CALL_NAME = re.compile(r"([A-Za-z_][\w:]*)(?:<[^<>()]*>)?\s*\(")
_REGISTER_TOKEN = re.compile(r"(?<![\w.\[])r\d+\b")


def _calls_with_register_argument(line: str) -> set[str]:
    """Functions (and methods) whose argument parentheses contain an rN on the line."""
    found: set[str] = set()
    stack: list[str | None] = []
    position = 0
    while position < len(line):
        match = _CALL_NAME.match(line, position)
        if match and (position == 0 or not (line[position - 1].isalnum() or line[position - 1] in "_.>:")):
            name = match.group(1)
            stack.append(None if name in {"if", "while", "for", "switch", "return", "sizeof"} else name)
            position = match.end()
            continue
        char = line[position]
        if char == "(":
            stack.append(None)
        elif char == ")":
            if stack:
                stack.pop()
        else:
            token = _REGISTER_TOKEN.match(line, position)
            if token and (position == 0 or not (line[position - 1].isalnum() or line[position - 1] in "_.[")):
                found.update(name for name in stack if name is not None)
                position = token.end()
                continue
        position += 1
    return found


_FLAGS_READ = re.compile(r"\bflags\.(?:N|Z|C|V)\(\)|\bflags\.Materialize\(")


def _flags_written(line: str) -> bool:
    """Conservative: any use of flags that is not a known read."""
    stripped = _FLAGS_READ.sub("", line)
    return re.search(r"(?<![\w.])flags\b", stripped) is not None


def _is_unconditional_call(raw: int, block) -> bool:
    if ((raw >> 28) & 0xF) != 0xE:
        return False
    if (raw & 0x0F000000) == 0x0B000000:  # BL
        return True
    if (raw & 0x0FFFFFF0) == 0x012FFF30:  # BLX register
        return True
    return any(edge.get("kind") == "indirect_call" for edge in block.successors)


def _promote_function_body(
    function: Function,
    lines: list[str],
    body_start: int,
    flag_helpers: tuple[str, ...],
) -> list[str]:
    """Synchronizes the promoted locals only where the state is observable.

    Invariant: outside the static "dirty" set, each local equals its
    state field (loaded at entry and reloaded after every
    call, hook or override). Calls and the block hook store only
    the block's dirty set; returns and exits (memory fault, budget,
    SVC, non-returned flow) go to a label shared per set,
    which stores that set and the budget and returns. The sets are
    safe supersets: storing a clean local rewrites the same value.

    Writes and uses come from the emitted C++ itself (exact), not from the
    IR analysis, which is conservative for several encodings. For the scan to be
    complete, a local may only be written by assignment, compound
    assignment or ++/--: taking its address or passing it to a function outside
    the by-value helper list is rejected (LoweringError).
    """
    body = lines[body_start:]
    text = "\n".join(body)
    registers = tuple(sorted({int(index) for index in re.findall(r"\bstate\.R\[(\d+)\]", text)}))
    has_flags = "state.Flags" in text or any(f"{helper}(state," in text for helper in flag_helpers)
    for index, line in enumerate(body):
        for register in registers:
            line = line.replace(f"state.R[{register}]", f"r{register}")
        if has_flags:
            line = line.replace("state.Flags", "flags")
            for helper in flag_helpers:
                line = line.replace(f"{helper}(state,", f"{helper}(flags,")
        body[index] = line
    register_mask = sum(1 << index for index in registers)
    for line in body:
        # Unary & (address): after start, '(', ',', '=', '{', '?' or ':'.
        if re.search(r"(?:^|[(,={?:])\s*&\s*r\d+\b", line.strip()):
            raise LoweringError(
                f"function 0x{function.entry:08X}: address of a promoted register: {line.strip()}")
        for callee in _calls_with_register_argument(line):
            if callee not in _BY_VALUE_REGISTER_CALLEES:
                raise LoweringError(
                    f"function 0x{function.entry:08X}: promoted register passed to {callee}")
    lines = [*lines[:body_start], *body]
    blocks_by_pc = {block.pc: block for block in function.blocks}
    # Lines of each block (the stretch before the first label is the dispatch).
    owner: list[int | None] = []
    current: int | None = None
    for line in body:
        match = _BLOCK_LABEL.match(line)
        if match:
            current = int(match.group(1), 16)
        owner.append(current)
    writes: dict[int, int] = {pc: 0 for pc in blocks_by_pc}
    flag_writes: dict[int, bool] = {pc: False for pc in blocks_by_pc}
    successors: dict[int, set[int]] = {pc: set() for pc in blocks_by_pc}
    calls: set[int] = set()
    for line, pc in zip(body, owner):
        if pc is None:
            continue
        if "Oot3dWholeAotFlow callFlow" in line:
            calls.add(pc)
        for match in _REGISTER_WRITE.finditer(line):
            index = int(next(group for group in match.groups() if group is not None))
            if register_mask & (1 << index):
                writes[pc] |= 1 << index
        if has_flags and _flags_written(line):
            flag_writes[pc] = True
        for target in _GOTO_BLOCK.findall(line):
            successors[pc].add(int(target, 16))
    resets = {
        pc: pc in calls and _is_unconditional_call(block.instructions[-1].raw, block)
        for pc, block in blocks_by_pc.items()
    }
    dirty_in = {pc: (0, False) for pc in blocks_by_pc}
    dirty_out = {pc: (0, False) for pc in blocks_by_pc}
    changed = True
    while changed:
        changed = False
        for block in function.blocks:
            pc = block.pc
            incoming = dirty_in[pc]
            outgoing = (0, False) if resets[pc] else (
                incoming[0] | writes[pc], incoming[1] or flag_writes[pc])
            if outgoing != dirty_out[pc]:
                dirty_out[pc] = outgoing
                changed = True
                for target in successors[pc]:
                    if target in dirty_in:
                        merged = (dirty_in[target][0] | outgoing[0],
                                  dirty_in[target][1] or outgoing[1])
                        dirty_in[target] = merged
    # Set used across the whole block: entry plus everything the block writes.
    block_sets = {
        pc: (dirty_in[pc][0] | writes[pc], dirty_in[pc][1] or flag_writes[pc])
        for pc in blocks_by_pc
    }
    labels: dict[tuple[int, bool], str] = {}

    def label(dirty: tuple[int, bool]) -> str:
        if dirty not in labels:
            labels[dirty] = f"exit_{len(labels)}"
        return labels[dirty]

    def stores(dirty: tuple[int, bool], indent: str) -> list[str]:
        result = [f"{indent}state.R[{index}] = r{index};"
                  for index in registers if dirty[0] & (1 << index)]
        if has_flags and dirty[1]:
            result.append(f"{indent}state.Flags = flags;")
        result.append(f"{indent}context.BlocksRemaining = blocksRemaining;")
        return result

    output = [*lines[:body_start], *_promoted_prologue(registers, has_flags)]
    pending_exit: str | None = None
    depth = 0
    # After call + reload, state is the truth: the non-returned flow exit
    # need not store anything (except the budget).
    after_call: int | None = None
    for line, pc in zip(lines[body_start:], owner):
        if pc != after_call:
            after_call = None
        if "Oot3dWholeAotFlow callFlow" in line:
            after_call = pc
        elif after_call is not None and (
                _REGISTER_WRITE.search(line) or (has_flags and _flags_written(line))):
            raise LoweringError(
                f"function 0x{function.entry:08X}: state written after a call: {line.strip()}")
        dirty = (block_sets[pc] if pc is not None and after_call is None
                 else (0, False))
        indent = line[:len(line) - len(line.lstrip())]
        stripped = line.strip()
        if pending_exit is not None:
            depth += line.count("(") - line.count(")")
            if depth == 0 and stripped.endswith(";"):
                output.append(f"{line} goto {pending_exit}; }}")
                pending_exit = None
            else:
                output.append(line)
            continue
        if stripped.startswith("return "):
            target = label(dirty if pc is not None else (0, False))
            text = indent + "{ exitFlow = " + stripped[len("return "):]
            depth = text.count("(") - text.count(")")
            if depth == 0 and stripped.endswith(";"):
                output.append(f"{text} goto {target}; }}")
            else:
                output.append(text)
                pending_exit = target
            continue
        if re.search(r"\breturn\b", line):
            raise LoweringError(
                f"function 0x{function.entry:08X}: return must be standalone: {stripped}"
            )
        if stripped == "commitState();":
            # Before a call or the block hook: only what may be
            # dirty in this block (clean locals already equal state).
            output.extend(stores(dirty, indent))
            continue
        output.append(line)
    if pending_exit is not None:
        raise LoweringError(f"function 0x{function.entry:08X}: unterminated return")
    if output[-1] == "" and output[-2] == "}":
        output = output[:-2]
    elif output[-1] == "}":
        output = output[:-1]
    else:
        raise LoweringError(f"function 0x{function.entry:08X}: unexpected function end")
    for dirty, name in labels.items():
        output.append(f"{name}:")
        output.extend(stores(dirty, "    "))
        output.append("    return exitFlow;")
    output.extend(["}", ""])
    return output
_READ_DESTINATION = re.compile(r"^(\s*)(uint(?:8|16|32|64)_t) (value\d*) = 0;$")


def _drop_read_zero_initializers(lines: list[str], start: int) -> None:
    """--lean-stack: `T valueN = 0;` immediately followed (within three lines)
    by the ReadFast that writes it loses the dead initializer. The variable's
    address escapes only into the cold ReadFastSlow, so the initializer was a
    stack store on the hot path of every guest load."""
    for index in range(start, len(lines)):
        match = _READ_DESTINATION.match(lines[index])
        if match is None:
            continue
        name = match.group(3)
        window = " ".join(lines[index + 1:index + 4])
        if ("ReadFast<" in window and
                re.search(rf"&{name}\b(?!\[)", window) is not None):
            lines[index] = f"{match.group(1)}{match.group(2)} {name};"


def _render_region_function(
    function: Function,
    symbols: dict[int, str],
    external_entries: frozenset[int],
    region_entries: frozenset[int],
    memory_entries: frozenset[int] = frozenset(),
    local_state: bool = False,
) -> list[str]:
    memory = function.entry in memory_entries
    if memory and any(carried_pair_copy(block) for block in function.blocks):
        return _render_function(function, symbols, external_entries, memory_copy=True)
    if function.entry not in region_entries and not memory:
        return _render_function(function, symbols, external_entries,
                                local_state=local_state)
    pcs = {block.pc for block in function.blocks}
    # No guest call may change the callback configuration inside this region.
    if function.direct_calls or not function.blocks or any(
        edge.get("kind") not in {"return", "branch", "fallthrough"}
        or (edge.get("kind") != "return" and int(edge.get("target", -1)) not in pcs)
        for block in function.blocks for edge in block.successors
    ) or any(
        (item.raw & 0x0F000000) == 0x0F000000  # SVC
        or (item.raw & 0x0F000000) == 0x0B000000  # BL
        or (item.raw & 0x0FFFFFF0) == 0x012FFF30  # BLX register
        for block in function.blocks for item in block.instructions
    ):
        raise LoweringError(f"region 0x{function.entry:08X} is not a closed call-free CFG")
    if memory and not region_memory_safe(function):
        raise LoweringError(f"memory region 0x{function.entry:08X} contains an observable operation")
    symbol = _symbol(function.name, function.entry)
    lines = _render_function(function, symbols, external_entries,
                             symbol_suffix="_Observed")
    lines += _render_function(function, symbols, external_entries,
                              callback_free=True, symbol_suffix="_Unobserved", memory_view=memory)
    # A conservative address interval is sufficient: false positives use the
    # unchanged path. Full tracing (an empty hook list) always uses that path.
    begin = min(pcs)
    end = max(item.pc for block in function.blocks for item in block.instructions)
    lines += [
        f"Oot3dWholeAotFlow Execute_{symbol}(",
        "    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,",
        "    Oot3dAotArchitecturalState& state, uint32_t entryPc) {",
        "    bool observed = context.BlockEntry != nullptr;",
        "    if (observed && context.BlockEntryPcCount != 0U) {",
        "        const auto end = context.BlockEntryPcs + context.BlockEntryPcCount;",
        f"        const auto hook = std::lower_bound(context.BlockEntryPcs, end, 0x{begin:08X}U);",
        f"        observed = hook != end && *hook <= 0x{end:08X}U;",
        "    }",
    ]
    if memory:
        lines.append("    observed = observed || !NativeA32UnobservedMemory::Eligible(context.Memory);")
    lines += [
        f"    return observed ? Execute_{symbol}_Observed(frame, context, state, entryPc)",
        f"                    : Execute_{symbol}_Unobserved(frame, context, state, entryPc);",
        "}", "",
    ]
    return lines


def _hook_gates_definition() -> list[str]:
    """Oot3dWholeAotHookGates() of the registry: every block, or the sorted
    PCs that --sparse-block-entries gated (the runtime refuses any other hook,
    oot3d_native_compiled_functions.cpp)."""
    if not _SPARSE_BLOCK_ENTRIES:
        return [
            "Oot3dWholeAotHookGateSet Oot3dWholeAotHookGates() noexcept {",
            "    return {};",
            "}",
            "",
        ]
    gates = sorted(_HOOK_GATES)
    values = ", ".join(f"0x{pc:08X}U" for pc in gates)
    return [
        "Oot3dWholeAotHookGateSet Oot3dWholeAotHookGates() noexcept {",
        f"    static constexpr std::array<uint32_t, {len(gates)}> kHookGatePcs{{{values}}};",
        "    return {false, kHookGatePcs};",
        "}",
        "",
    ]


def _native_vfp_entry_lines() -> list[str]:
    # Guest FPSCR mode in FPCR for the whole call; FPSR folded back into the
    # guest FPSCR on every exit, including an observable-exit unwind.
    if not _NATIVE_VFP_AARCH64:
        return []
    return ["        Oot3dGuestFpScope guestFpScope(state.fpscr);"]


def _render_header() -> str:
    return """#pragma once

#include \"oot3d_native_whole_aot_runtime.h\"

#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

std::span<const uint32_t> Oot3dWholeAotEntryPoints() noexcept;
// Blocks that test the block-entry gate (every block, or the sorted PCs of
// --sparse-block-entries); see Oot3dWholeAotHookGateSet.
Oot3dWholeAotHookGateSet Oot3dWholeAotHookGates() noexcept;
bool ExecuteOot3dWholeAotFunction(
    uint32_t pc,
    oot3d::recomp::a32::GuestState& state,
    NativeA32Memory& memory,
    oot3d::recomp::a32::ExecutionResult* result,
    Oot3dWholeAotStats* stats,
    Oot3dWholeAotExternalCall externalCall,
    uint32_t blockBudget,
    uint32_t* blocksConsumed,
    oot3d::recomp::a32::BlockEntryCallback blockEntry,
    void* blockEntryUser,
    const uint32_t* blockEntryPcs,
    size_t blockEntryPcCount,
    const Oot3dAotBlockEntryFilter* blockEntryFilter,
    bool skipFirstBlockEntry,
    uint32_t stopPc);

} // namespace Oot3dNativeGame
"""


def _render_source(
    functions: tuple[Function, ...], external_entries: frozenset[int],
    region_entries: frozenset[int] = frozenset(),
    memory_entries: frozenset[int] = frozenset(),
    local_state: bool = False,
) -> str:
    functions_by_entry = {function.entry: function for function in functions}
    function_symbols = {
        function.entry: _symbol(function.name, function.entry)
        for function in functions
    }
    entry_owners = _entry_owners(functions)
    lines = [
        f'#include "{HEADER_NAME}"',
        '#include "oot3d_native_a32_memory.h"',
        "",
        "#include <array>",
        "#include <atomic>",
        "",
        "namespace Oot3dNativeGame {",
        "namespace {",
        "namespace a32 = oot3d::recomp::a32;",
        "",
        "Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(",
        "    uint32_t pc, Oot3dWholeAotFrame& frame,",
        "    Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state);",
        "",
    ]
    if memory_entries:
        lines[2:2] = ['#include "oot3d_native_a32_memory_region.h"']
    for function in functions:
        lines.extend(
            [
                f"Oot3dWholeAotFlow Execute_{function_symbols[function.entry]}(",
                "    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,",
                "    Oot3dAotArchitecturalState& state,",
                "    uint32_t entryPc);",
            ]
        )
    lines.append("")
    for function in functions:
        lines.extend(
            _render_region_function(function, function_symbols, external_entries, region_entries, memory_entries, local_state)
        )
    lines.extend(
        [
            "Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(",
            "    uint32_t pc, Oot3dWholeAotFrame& frame,",
            "    Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state) {",
            "    switch (pc) {",
        ]
    )
    for entry, owner in sorted(entry_owners.items()):
        function = functions_by_entry[owner]
        symbol = _symbol(function.name, function.entry)
        lines.extend(
            [
                f"    case 0x{entry:08X}U:",
                "        OOT3D_AOT_STAT(context.Stats.ResolvedIndirectCalls);",
                f"        return Execute_{symbol}(frame, context, state, pc);",
            ]
        )
    lines.extend(
        [
            "    default:",
            "        return Oot3dAotCallExternal(context, pc, frame, state);",
            "    }",
            "}",
            "",
        ]
    )
    entries = ", ".join(
        f"0x{entry:08X}U" for entry in sorted(entry_owners)
    )
    lines.extend(
        [
            f"constexpr std::array<uint32_t, {len(entry_owners)}> kEntryPoints{{{entries}}};",
            "",
            "} // namespace",
            "",
            "std::span<const uint32_t> Oot3dWholeAotEntryPoints() noexcept {",
            "    return kEntryPoints;",
            "}",
            "",
            *_hook_gates_definition(),
            "bool ExecuteOot3dWholeAotFunction(",
            "    uint32_t pc, a32::GuestState& state, NativeA32Memory& memory,",
            "    a32::ExecutionResult* result, Oot3dWholeAotStats* stats,",
            "    Oot3dWholeAotExternalCall externalCall, uint32_t blockBudget,",
            "    uint32_t* blocksConsumed, a32::BlockEntryCallback blockEntry,",
            "    void* blockEntryUser, const uint32_t* blockEntryPcs,",
            "    size_t blockEntryPcCount,",
            "    const Oot3dAotBlockEntryFilter* blockEntryFilter,",
            "    bool skipFirstBlockEntry, uint32_t stopPc) {",
            "    if (result == nullptr || stats == nullptr || blockBudget == 0U) return false;",
            "    if (blocksConsumed != nullptr) *blocksConsumed = 0U;",
            "    static_cast<void>(stopPc);",
            "    Oot3dWholeAotFrame frame(state);",
            "    Oot3dAotArchitecturalState promotedState(state);",
            "    Oot3dWholeAotContext context{",
            "        memory, memory, *stats, externalCall, blockEntry, blockEntryUser,",
            "        blockEntryPcs, blockEntryPcCount, blockEntryFilter,",
            "        skipFirstBlockEntry,",
            "        blockBudget};",
            "    Oot3dWholeAotFlow flow{};",
            "    a32::GuestState observableExitState{};",
            "    bool restoreObservableExitState = false;",
            "    try {",
            "        Oot3dWholeAotExecutionScope executionScope;",
            *_native_vfp_entry_lines(),
            "        switch (pc) {",
        ]
    )
    for entry, owner in sorted(entry_owners.items()):
        function = functions_by_entry[owner]
        symbol = _symbol(function.name, function.entry)
        lines.extend(
            [
                f"    case 0x{entry:08X}U:",
                f"            flow = Execute_{symbol}(frame, context, promotedState, pc);",
                "            break;",
            ]
        )
    lines.extend(
        [
            "        default:",
            "            return false;",
            "        }",
            "    } catch (const Oot3dWholeAotObservableExit& exit) {",
            "        restoreObservableExitState =",
            "            Oot3dWholeAotTakeObservableExitSnapshot(",
            "                exit.Pc, &observableExitState);",
            "        flow = Oot3dAotBranch(exit.Pc);",
            "    }",
            "    promotedState.Flush(0x7FFFU, true);",
            "    if (restoreObservableExitState) state = observableExitState;",
            "    ++stats->Calls;",
            "    state.r[15] = flow.Pc;",
            "    if (blocksConsumed != nullptr) {",
            "        *blocksConsumed = context.BlocksConsumed() != 0U",
            "                              ? context.BlocksConsumed() : 1U;",
            "    }",
            "    switch (flow.Kind) {",
            "    case Oot3dWholeAotFlowKind::Returned:",
            "    case Oot3dWholeAotFlowKind::Branch:",
            "        *result = {a32::ExitKind::Branch, flow.Pc,",
            "                   a32::FallbackReason::None, 0U};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::Svc:",
            "        *result = {a32::ExitKind::Svc, flow.Pc,",
            "                   a32::FallbackReason::None, flow.Detail};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::BlockLimit:",
            "        *result = {a32::ExitKind::BlockLimit, flow.Pc,",
            "                   a32::FallbackReason::None, context.BlocksConsumed()};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::MemoryFault:",
            "        *result = {a32::ExitKind::MemoryFault, flow.Pc,",
            "                   a32::FallbackReason::None, flow.Detail};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::Unsupported:",
            "        ++stats->UnsupportedExits;",
            "        *result = {a32::ExitKind::Unsupported, flow.Pc,",
            "                   a32::FallbackReason::Unsupported, flow.Detail};",
            "        return true;",
            "    }",
            "    return false;",
            "}",
            "",
            "} // namespace Oot3dNativeGame",
            "",
        ]
    )
    return "\n".join(lines)


def _entry_owners(functions: tuple[Function, ...]) -> dict[int, int]:
    # A callable entry always owns itself. Multiple recovered functions may
    # legitimately contain the same post-SVC or post-indirect-call resume
    # block; those aliases are architectural-state-only continuations, so one
    # deterministic owner is enough for dispatcher re-entry while every
    # callable body remains available to direct AOT calls.
    owners = {function.entry: function.entry for function in functions}
    for function in functions:
        for entry in _function_dispatch_entries(function):
            owners.setdefault(entry, function.entry)
    return owners


def _render_internal_header() -> str:
    lines = [
        "#pragma once",
        "",
        f'#include "{HEADER_NAME}"',
        "",
        "namespace Oot3dNativeGame::GeneratedWholeAot {",
        "",
        "Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(",
        "    uint32_t pc, Oot3dWholeAotFrame& frame,",
        "    Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state);",
        "",
    ]
    lines.extend(["} // namespace Oot3dNativeGame::GeneratedWholeAot", ""])
    return "\n".join(lines)


def _render_function_declarations(functions: Iterable[Function]) -> list[str]:
    lines: list[str] = []
    for function in functions:
        symbol = _symbol(function.name, function.entry)
        lines.extend(
            [
                f"Oot3dWholeAotFlow Execute_{symbol}(",
                "    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,",
                "    Oot3dAotArchitecturalState& state,",
                "    uint32_t entryPc);",
            ]
        )
    return lines


def _render_shard_source(
    functions: tuple[Function, ...],
    all_symbols: dict[int, str],
    external_entries: frozenset[int],
    declarations: tuple[Function, ...],
    region_entries: frozenset[int] = frozenset(),
    memory_entries: frozenset[int] = frozenset(),
    local_state: bool = False,
) -> str:
    lines = [
        f'#include "{INTERNAL_HEADER_NAME}"',
        '#include "oot3d_native_a32_memory.h"',
        "",
        "#include <atomic>",
        "",
        "namespace Oot3dNativeGame::GeneratedWholeAot {",
        "namespace a32 = oot3d::recomp::a32;",
        "",
    ]
    if any(function.entry in memory_entries for function in functions):
        lines[2:2] = ['#include "oot3d_native_a32_memory_region.h"']
    lines.extend(_render_function_declarations(declarations))
    lines.append("")
    for function in functions:
        rendered = _render_region_function(function, all_symbols, external_entries, region_entries, memory_entries, local_state)
        if function.entry in _GIANT_COLD_ENTRIES:
            rendered = [GIANT_COLD_ATTRIBUTE + rendered[0], *rendered[1:]]
        lines.extend(rendered)
    lines.extend(["} // namespace Oot3dNativeGame::GeneratedWholeAot", ""])
    return "\n".join(lines)


def _render_registry_source(functions: tuple[Function, ...]) -> str:
    function_symbols = {
        function.entry: _symbol(function.name, function.entry)
        for function in functions
    }
    entry_owners = _entry_owners(functions)
    dispatch_entries = sorted(entry_owners)
    # Flat dispatch: one slot per instruction word from the first to the last
    # entry. Every entry is an A32 instruction address, so a word index is
    # exact.
    misaligned = [entry for entry in dispatch_entries if entry & 3]
    if misaligned:
        raise LoweringError(
            f"dispatch entry 0x{misaligned[0]:08X} is not word aligned"
        )
    dispatch_code_base = dispatch_entries[0]
    dispatch_slot_count = ((dispatch_entries[-1] - dispatch_code_base) >> 2) + 1
    entries = ", ".join(
        f"0x{entry:08X}U" for entry in dispatch_entries
    )
    lines = [
        f'#include "{HEADER_NAME}"',
        f'#include "{INTERNAL_HEADER_NAME}"',
        '#include "oot3d_native_a32_memory.h"',
        '#include "oot3d_native_indirect_profile.h"',
        "",
        "#include <array>",
        "#include <bit>",
        "",
        "namespace Oot3dNativeGame {",
        "namespace a32 = oot3d::recomp::a32;",
        "",
        "namespace GeneratedWholeAot {",
        "",
    ]
    lines.extend(_render_function_declarations(functions))
    lines.extend(
        [
            "",
            "using Function = Oot3dWholeAotFlow (*)(",
            "    Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
            "    Oot3dAotArchitecturalState&, uint32_t);",
            "struct DispatchEntry {",
            "    uint32_t Pc;",
            "    Function Execute;",
            "};",
            "",
            f"constexpr std::array<DispatchEntry, {len(entry_owners)}> kDispatchEntries{{{{",
        ]
    )
    for entry in dispatch_entries:
        owner = entry_owners[entry]
        symbol = function_symbols[owner]
        lines.append(
            f"    {{0x{entry:08X}U, Execute_{symbol}}},"
        )
    lines.extend(
        [
            "}};",
            "",
            "// Flat dispatch table: slot (pc - kDispatchCodeBase) / 4 holds one",
            "// plus the index of the entry at that pc in kDispatchEntries, zero",
            "// where no entry exists (the same set and the same null result as a",
            "// search of kDispatchEntries). It is filled once during static",
            "// initialization, in .bss: no relocations and no image growth.",
            f"constexpr uint32_t kDispatchCodeBase = 0x{dispatch_code_base:08X}U;",
            f"constexpr uint32_t kDispatchSlotCount = {dispatch_slot_count}U;",
            "static uint32_t gDispatchSlots[kDispatchSlotCount];",
            "[[maybe_unused]] const bool gDispatchSlotsReady = [] {",
            "    for (uint32_t index = 0; index < kDispatchEntries.size(); ++index) {",
            "        gDispatchSlots[(kDispatchEntries[index].Pc - kDispatchCodeBase) >> 2U] =",
            "            index + 1U;",
            "    }",
            "    return true;",
            "}();",
            "",
            "// Inline into the indirect call and the dispatcher loop: out of line",
            "// it costs a call plus spills around it in ExecuteOot3dWholeAotIndirect.",
            "OOT3D_AOT_HOT const DispatchEntry* FindDispatchEntry(uint32_t pc) noexcept {",
            "    // A pc below the base wraps above the bound, and a pc that is not",
            "    // word aligned rotates its low bits to the top: both miss.",
            "    const uint32_t slot = std::rotr(pc - kDispatchCodeBase, 2);",
            "    if (slot >= kDispatchSlotCount) {",
            "        return nullptr;",
            "    }",
            "    const uint32_t index = gDispatchSlots[slot];",
            "    return index != 0U ? &kDispatchEntries[index - 1U] : nullptr;",
            "}",
            "",
            "Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(",
            "    uint32_t pc, Oot3dWholeAotFrame& frame,",
            "    Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state) {",
            "    const DispatchEntry* entry = FindDispatchEntry(pc);",
            "    if (Oot3dIndirectProfile* profile = gOot3dIndirectProfile) [[unlikely]] {",
            "        profile->Record(state.R[14], pc);",
            "    }",
            "    if (entry == nullptr) {",
            "        return Oot3dAotCallExternal(context, pc, frame, state);",
            "    }",
            "    OOT3D_AOT_STAT(context.Stats.ResolvedIndirectCalls);",
            "    return entry->Execute(frame, context, state, pc);",
            "}",
            "",
            "} // namespace GeneratedWholeAot",
            "",
            "namespace {",
            f"constexpr std::array<uint32_t, {len(entry_owners)}> kEntryPoints{{{entries}}};",
            "} // namespace",
            "",
            "std::span<const uint32_t> Oot3dWholeAotEntryPoints() noexcept {",
            "    return kEntryPoints;",
            "}",
            "",
            *_hook_gates_definition(),
            "bool ExecuteOot3dWholeAotFunction(",
            "    uint32_t pc, a32::GuestState& state, NativeA32Memory& memory,",
            "    a32::ExecutionResult* result, Oot3dWholeAotStats* stats,",
            "    Oot3dWholeAotExternalCall externalCall, uint32_t blockBudget,",
            "    uint32_t* blocksConsumed, a32::BlockEntryCallback blockEntry,",
            "    void* blockEntryUser, const uint32_t* blockEntryPcs,",
            "    size_t blockEntryPcCount,",
            "    const Oot3dAotBlockEntryFilter* blockEntryFilter,",
            "    bool skipFirstBlockEntry, uint32_t stopPc) {",
            "    if (result == nullptr || stats == nullptr || blockBudget == 0U) return false;",
            "    if (blocksConsumed != nullptr) *blocksConsumed = 0U;",
            "    const GeneratedWholeAot::DispatchEntry* entry =",
            "        GeneratedWholeAot::FindDispatchEntry(pc);",
            "    if (entry == nullptr) return false;",
            "    Oot3dWholeAotFrame frame(state);",
            "    Oot3dAotArchitecturalState promotedState(state);",
            "    Oot3dWholeAotContext context{",
            "        memory, memory, *stats, externalCall, blockEntry, blockEntryUser,",
            "        blockEntryPcs, blockEntryPcCount, blockEntryFilter,",
            "        skipFirstBlockEntry,",
            "        blockBudget};",
            "    Oot3dWholeAotFlow flow{};",
            "    a32::GuestState observableExitState{};",
            "    bool restoreObservableExitState = false;",
            "    try {",
            "        Oot3dWholeAotExecutionScope executionScope;",
            *_native_vfp_entry_lines(),
            "        flow = entry->Execute(frame, context, promotedState, pc);",
            "        while ((flow.Kind == Oot3dWholeAotFlowKind::Returned ||",
            "                flow.Kind == Oot3dWholeAotFlowKind::Branch) &&",
            "               flow.Pc != stopPc && context.BlocksRemaining != 0U) {",
            "            entry = GeneratedWholeAot::FindDispatchEntry(flow.Pc);",
            "            if (entry == nullptr) break;",
            "            flow = entry->Execute(",
            "                frame, context, promotedState, flow.Pc);",
            "        }",
            "    } catch (const Oot3dWholeAotObservableExit& exit) {",
            "        restoreObservableExitState =",
            "            Oot3dWholeAotTakeObservableExitSnapshot(",
            "                exit.Pc, &observableExitState);",
            "        flow = Oot3dAotBranch(exit.Pc);",
            "    }",
            "    promotedState.Flush(0x7FFFU, true);",
            "    if (restoreObservableExitState) state = observableExitState;",
            "    ++stats->Calls;",
            "    state.r[15] = flow.Pc;",
            "    if (blocksConsumed != nullptr) {",
            "        *blocksConsumed = context.BlocksConsumed() != 0U",
            "                              ? context.BlocksConsumed() : 1U;",
            "    }",
            "    switch (flow.Kind) {",
            "    case Oot3dWholeAotFlowKind::Returned:",
            "    case Oot3dWholeAotFlowKind::Branch:",
            "        *result = {a32::ExitKind::Branch, flow.Pc,",
            "                   a32::FallbackReason::None, 0U};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::Svc:",
            "        *result = {a32::ExitKind::Svc, flow.Pc,",
            "                   a32::FallbackReason::None, flow.Detail};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::BlockLimit:",
            "        *result = {a32::ExitKind::BlockLimit, flow.Pc,",
            "                   a32::FallbackReason::None, context.BlocksConsumed()};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::MemoryFault:",
            "        *result = {a32::ExitKind::MemoryFault, flow.Pc,",
            "                   a32::FallbackReason::None, flow.Detail};",
            "        return true;",
            "    case Oot3dWholeAotFlowKind::Unsupported:",
            "        ++stats->UnsupportedExits;",
            "        *result = {a32::ExitKind::Unsupported, flow.Pc,",
            "                   a32::FallbackReason::Unsupported, flow.Detail};",
            "        return true;",
            "    }",
            "    return false;",
            "}",
            "",
            "} // namespace Oot3dNativeGame",
            "",
        ]
    )
    return "\n".join(lines)


def _stable_shard_index(entry: int, shard_count: int) -> int:
    mixed = ((entry >> 2) * 0x9E3779B1) & 0xFFFFFFFF
    return (mixed * shard_count) >> 32


def _shard_buckets(
    functions: tuple[Function, ...], shard_count: int, strategy: str,
    existing_assignments: dict[int, int] | None = None,
) -> list[list[Function]]:
    buckets: list[list[Function]] = [[] for _ in range(shard_count)]
    if strategy == "stable":
        for function in functions:
            buckets[_stable_shard_index(function.entry, shard_count)].append(
                function
            )
        return buckets
    if strategy == "incremental":
        weights = [0] * shard_count
        unassigned: list[Function] = []
        for function in functions:
            index = (existing_assignments or {}).get(function.entry)
            if index is None or not 0 <= index < shard_count:
                unassigned.append(function)
                continue
            buckets[index].append(function)
            weights[index] += sum(
                len(block.instructions) for block in function.blocks
            )
        for function in sorted(
            unassigned,
            key=lambda item: (
                -sum(len(block.instructions) for block in item.blocks),
                item.entry,
            ),
        ):
            index = min(range(shard_count), key=lambda item: (weights[item], item))
            buckets[index].append(function)
            weights[index] += sum(
                len(block.instructions) for block in function.blocks
            )
        return buckets
    if strategy == "affinity-incremental":
        return _affinity_incremental_buckets(
            functions, shard_count, existing_assignments or {}
        )
    if strategy == "affinity":
        function_by_entry = {function.entry: function for function in functions}
        weights_by_entry = {
            function.entry: sum(
                len(block.instructions) for block in function.blocks
            )
            for function in functions
        }
        adjacency: dict[int, dict[int, int]] = {
            function.entry: {} for function in functions
        }
        for function in functions:
            for _, target in function.direct_calls:
                if target not in function_by_entry or target == function.entry:
                    continue
                adjacency[function.entry][target] = (
                    adjacency[function.entry].get(target, 0) + 1
                )
                adjacency[target][function.entry] = (
                    adjacency[target].get(function.entry, 0) + 1
                )

        unassigned = set(function_by_entry)
        remaining_weight = sum(weights_by_entry.values())
        weighted_degree = {
            entry: sum(neighbors.values())
            for entry, neighbors in adjacency.items()
        }
        for bucket_index, bucket in enumerate(buckets):
            if not unassigned:
                break
            buckets_remaining = shard_count - bucket_index
            target_weight = max(
                1, (remaining_weight + buckets_remaining - 1) // buckets_remaining
            )
            bucket_weight = 0
            affinity: dict[int, int] = {}

            def add(entry: int) -> None:
                nonlocal bucket_weight, remaining_weight
                unassigned.remove(entry)
                bucket.append(function_by_entry[entry])
                weight = weights_by_entry[entry]
                bucket_weight += weight
                remaining_weight -= weight
                affinity.pop(entry, None)
                for neighbor, edge_weight in adjacency[entry].items():
                    if neighbor in unassigned:
                        affinity[neighbor] = (
                            affinity.get(neighbor, 0) + edge_weight
                        )

            seed = max(
                unassigned,
                key=lambda entry: (
                    weighted_degree[entry],
                    weights_by_entry[entry],
                    -entry,
                ),
            )
            add(seed)
            while unassigned and (
                bucket_weight < target_weight or buckets_remaining == 1
            ):
                connected = [entry for entry in affinity if entry in unassigned]
                candidates = connected if connected else list(unassigned)
                candidate = max(
                    candidates,
                    key=lambda entry: (
                        affinity.get(entry, 0),
                        weighted_degree[entry],
                        -abs(
                            target_weight
                            - bucket_weight
                            - weights_by_entry[entry]
                        ),
                        -entry,
                    ),
                )
                candidate_weight = weights_by_entry[candidate]
                if (
                    buckets_remaining > 1
                    and bucket_weight * 4 >= target_weight * 3
                    and bucket_weight + candidate_weight > target_weight
                ):
                    break
                add(candidate)
        return buckets
    if strategy != "balanced":
        raise ValueError(f"unknown whole-AOT shard strategy: {strategy}")

    weights = [0] * shard_count
    weighted_functions = sorted(
        functions,
        key=lambda function: (
            -sum(len(block.instructions) for block in function.blocks),
            function.entry,
        ),
    )
    for function in weighted_functions:
        index = min(range(shard_count), key=lambda item: (weights[item], item))
        buckets[index].append(function)
        weights[index] += sum(
            len(block.instructions) for block in function.blocks
        )
    return buckets


# Weight cap (instructions) of a shard when receiving a new function in
# affinity-incremental mode, relative to the mean: keeps a shard from growing
# without bound by accumulating new neighbors (the parallel build tail).
AFFINITY_INCREMENTAL_WEIGHT_CAP = 1.25


def _affinity_incremental_buckets(
    functions: tuple[Function, ...], shard_count: int,
    existing_assignments: dict[int, int],
) -> list[list[Function]]:
    """Stable affinity: preserves the previous distribution.

    Without a previous distribution (first generation, different shard count),
    the result is identical to ``affinity``. With one, each function that already
    existed stays in the same shard, so a small change in the selection
    (a function that is new, removed or changed size) only alters the shards
    that actually contain or call it, and ccache hits the rest. New
    functions go to the shard with which they have the most direct calls (to keep
    inlining between caller and callee), as long as they fit under the weight cap;
    with no placed neighbors, they go to the lightest shard.
    """
    if not existing_assignments:
        return _shard_buckets(functions, shard_count, "affinity")
    buckets: list[list[Function]] = [[] for _ in range(shard_count)]
    weights = [0] * shard_count
    weight_of = {
        function.entry: sum(len(block.instructions) for block in function.blocks)
        for function in functions
    }
    placed: dict[int, int] = {}
    unassigned: list[Function] = []
    for function in functions:
        index = existing_assignments.get(function.entry)
        if index is None or not 0 <= index < shard_count:
            unassigned.append(function)
            continue
        buckets[index].append(function)
        weights[index] += weight_of[function.entry]
        placed[function.entry] = index
    if not unassigned:
        return buckets
    known = set(weight_of)
    adjacency: dict[int, dict[int, int]] = {entry: {} for entry in known}
    for function in functions:
        for _, target in function.direct_calls:
            if target not in known or target == function.entry:
                continue
            adjacency[function.entry][target] = (
                adjacency[function.entry].get(target, 0) + 1
            )
            adjacency[target][function.entry] = (
                adjacency[target].get(function.entry, 0) + 1
            )
    cap = AFFINITY_INCREMENTAL_WEIGHT_CAP * sum(weight_of.values()) / shard_count
    for function in sorted(
        unassigned, key=lambda item: (-weight_of[item.entry], item.entry)
    ):
        weight = weight_of[function.entry]
        score: dict[int, int] = {}
        for neighbor, edge_weight in adjacency[function.entry].items():
            if neighbor in placed:
                score[placed[neighbor]] = (
                    score.get(placed[neighbor], 0) + edge_weight
                )
        index = next(
            (
                candidate
                for candidate in sorted(
                    score, key=lambda item: (-score[item], weights[item], item)
                )
                if weights[candidate] + weight <= cap
            ),
            None,
        )
        if index is None:
            index = min(range(shard_count), key=lambda item: (weights[item], item))
        buckets[index].append(function)
        weights[index] += weight
        placed[function.entry] = index
    return buckets


def _manifest_shard_assignments(
    output: Path, shard_count: int
) -> dict[int, int]:
    """Previous distribution read only from the manifest and only with the same count."""
    manifest_path = output / MANIFEST_NAME
    if not manifest_path.is_file():
        return {}
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if manifest.get("shard_count") != shard_count:
        return {}
    assignments: dict[int, int] = {}
    shards = manifest.get("shards", [])
    if isinstance(shards, list):
        for index, shard in enumerate(shards):
            entries = shard.get("entries") if isinstance(shard, dict) else None
            if isinstance(entries, list):
                for entry in entries:
                    if isinstance(entry, int):
                        assignments[entry] = index
    return assignments


def _existing_shard_assignments(
    output: Path, shard_count: int
) -> dict[int, int]:
    assignments: dict[int, int] = {}
    manifest_path = output / MANIFEST_NAME
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            manifest = {}
        shards = manifest.get("shards", [])
        if isinstance(shards, list):
            for index, shard in enumerate(shards):
                if not isinstance(shard, dict):
                    continue
                entries = shard.get("entries")
                if not isinstance(entries, list):
                    continue
                for entry in entries:
                    if isinstance(entry, int):
                        assignments[entry] = index
        if assignments:
            return assignments

    width = max(2, len(str(shard_count - 1)))
    pattern = re.compile(
        r"^Oot3dWholeAotFlow Execute_.*_([0-9A-F]{8})\(\n"
        r"\s+Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,\n"
        r"(?:\s+Oot3dAotArchitecturalState& state,\n)?"
        r"\s+uint32_t entryPc\) \{$",
        re.MULTILINE,
    )
    for index in range(shard_count):
        path = output / f"{SHARD_PREFIX}{index:0{width}d}.cpp"
        if not path.is_file():
            continue
        source = path.read_text(encoding="utf-8")
        for match in pattern.finditer(source):
            assignments[int(match.group(1), 16)] = index
    return assignments


def _load_functions(
    program: dict[str, object], selection: dict[str, object], code: bytes
) -> tuple[tuple[Function, ...], frozenset[int]]:
    if program.get("format") != PROGRAM_FORMAT:
        raise ValueError("invalid whole-AOT program")
    if selection.get("format") != SELECTION_FORMAT:
        raise ValueError("invalid whole-AOT function selection")
    base = int(program["base"])
    blocks_by_id = {int(block["id"]): block for block in program["blocks"]}
    functions_by_entry = {
        int(function["entry"]): function for function in program["functions"]
    }
    selected_entries = {
        int(item["entry"]) for item in selection.get("functions", [])
    }
    external_entries = frozenset(
        int(item["entry"])
        for item in selection.get("external_functions", [])
    )
    duplicate_entries = selected_entries & external_entries
    if duplicate_entries:
        entry = min(duplicate_entries)
        raise ValueError(f"function 0x{entry:08X} is both selected and external")
    selected: list[Function] = []
    for item in selection.get("functions", []):
        entry = int(item["entry"])
        record = functions_by_entry.get(entry)
        if record is None:
            raise ValueError(f"selected function 0x{entry:08X} is absent")
        if not record.get("closed_static_cfg"):
            raise ValueError(f"selected function 0x{entry:08X} has an open CFG")
        direct_calls = tuple(
            (int(call["site"]), int(call["target"]))
            for call in record.get("direct_calls", [])
        )
        for site, target in direct_calls:
            if target not in selected_entries and target not in external_entries:
                raise ValueError(
                    f"direct call at 0x{site:08X} targets unselected "
                    f"function 0x{target:08X}"
                )
        blocks: list[Block] = []
        for block_id_value in record["blocks"]:
            block_id = int(block_id_value)
            block_record = blocks_by_id[block_id]
            pc = int(block_record["pc"])
            end_pc = int(block_record["end_pc"])
            start = pc - base
            end = end_pc - base
            if start < 0 or end > len(code) or (end - start) % 4:
                raise ValueError(f"block 0x{pc:08X} is outside code.bin")
            instructions = tuple(
                Instruction(
                    pc=pc + offset,
                    raw=int.from_bytes(code[start + offset:start + offset + 4], "little"),
                )
                for offset in range(0, end - start, 4)
            )
            blocks.append(
                Block(
                    block_id,
                    pc,
                    end_pc,
                    instructions,
                    tuple(block_record.get("successors", [])),
                )
            )
        block_pcs = {block.pc for block in blocks}
        indirect_resumes = {
            int(site["site"]) + 4
            for site in record.get("indirect_sites", [])
            if site.get("kind") == "indirect_call"
        }
        runtime_resumes = {
            int(edge["target"])
            for block in blocks
            for edge in block.successors
            if edge.get("kind") == "resume" and "target" in edge
        }
        selected.append(
            Function(
                entry,
                str(record["name"]),
                tuple(blocks),
                direct_calls,
                tuple(
                    sorted(
                        {
                            site + 4
                            for site, _ in direct_calls
                            if site + 4 in block_pcs
                            and site + 4 != entry
                        }
                        | {
                            resume
                            for resume in indirect_resumes
                            if resume in block_pcs and resume != entry
                        }
                        | {
                            resume
                            for resume in runtime_resumes
                            if resume in block_pcs and resume != entry
                        }
                    )
                ),
                tuple(sorted(block_pcs - {entry})),
            )
        )
    return (
        tuple(sorted(selected, key=lambda function: function.entry)),
        external_entries,
    )


def generate(
    program_path: Path,
    selection_path: Path,
    code_path: Path,
    output: Path,
    shard_count: int = 1,
    shard_strategy: str = "stable",
    region_entries: frozenset[int] = frozenset(),
    memory_entries: frozenset[int] = frozenset(),
    local_state: bool = False,
    fold_literals: bool = True,
    native_vfp_aarch64: bool = False,
    giant_function_instructions: int = 0,
    hot_entries: frozenset[int] = frozenset(),
    promote_registers: bool = False,
    vfp_zero_aware: bool = False,
    lean_stack: bool = False,
    sparse_block_entries: bool = False,
    hook_gates: frozenset[int] | None = None,
    fold_vfp_literals: bool = False,
) -> dict[str, object]:
    """``giant_function_instructions`` > 0 marks the functions with at least
    that many guest instructions, except those in ``hot_entries`` (profile), with
    GIANT_COLD_ATTRIBUTE. 0 (default) generates exactly the same C++ as before.

    ``sparse_block_entries`` (N4, see _SPARSE_BLOCK_ENTRIES) needs
    ``hook_gates``, the PCs that may carry a runtime block-entry hook
    (whole_aot_hook_gates.json). ``fold_vfp_literals``: see _FOLD_VFP_LITERALS."""
    global _LITERAL_IMAGE, _NATIVE_VFP_AARCH64, _GIANT_COLD_ENTRIES
    global _VFP_ZERO_AWARE, _LEAN_STACK, _PROMOTE_REGISTERS
    global _SPARSE_BLOCK_ENTRIES, _HOOK_GATES, _FOLD_VFP_LITERALS
    if vfp_zero_aware and not native_vfp_aarch64:
        raise ValueError("--vfp-zero-aware requires --native-vfp-aarch64")
    if sparse_block_entries:
        if hook_gates is None:
            raise ValueError("--sparse-block-entries requires the hook gate list")
        if region_entries or memory_entries or promote_registers:
            raise ValueError(
                "--sparse-block-entries does not support callback-free regions, "
                "memory regions or register promotion")
    # Absent from the manifest when off: without the options the output (and
    # the manifest) is byte for byte the previous one.
    option_contract: dict[str, object] = {
        **({"vfp_zero_aware": True} if vfp_zero_aware else {}),
        **({"lean_stack": True} if lean_stack else {}),
        **({"sparse_block_entries": True,
            "hook_gates": sorted(hook_gates or ())} if sparse_block_entries else {}),
        **({"fold_vfp_literals": True} if fold_vfp_literals else {}),
    }
    if giant_function_instructions < 0:
        raise ValueError("giant function threshold must not be negative")
    giant_contract: dict[str, object] = (
        {
            "giant_cold_instructions": giant_function_instructions,
            "giant_cold_hot_entries": sorted(hot_entries),
        }
        if giant_function_instructions
        else {}
    )
    if shard_count <= 0:
        raise ValueError("whole-AOT shard count must be positive")
    if shard_strategy not in {
        "stable", "balanced", "incremental", "affinity", "affinity-incremental"
    }:
        raise ValueError(
            "whole-AOT shard strategy must be stable, balanced, incremental, "
            "affinity or affinity-incremental"
        )
    program_bytes = program_path.read_bytes()
    selection_bytes = selection_path.read_bytes()
    code = code_path.read_bytes()
    program_sha256 = _sha256(program_bytes)
    selection_sha256 = _sha256(selection_bytes)
    code_sha256 = _sha256(code)
    generator_sha256 = _sha256(Path(__file__).read_bytes())
    memory_contract = ""
    if memory_entries:
        generator_sha256 = _sha256(
            Path(__file__).read_bytes() + Path(__file__).with_name("whole_aot_memory_regions.py").read_bytes())
        memory_contract = _sha256(
            (Path(__file__).parents[1] / "native_game_runtime/oot3d_native_a32_memory_region.h").read_bytes())
    manifest_path = output / MANIFEST_NAME
    if manifest_path.is_file():
        try:
            cached = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            cached = {}
        expected = {
            "format": OUTPUT_FORMAT,
            "program_sha256": program_sha256,
            "selection_sha256": selection_sha256,
            "code_sha256": code_sha256,
            "generator_sha256": generator_sha256,
            "shard_count": shard_count,
            "shard_strategy": shard_strategy,
            "callback_free_regions": sorted(region_entries),
            "memory_regions": sorted(memory_entries),
            "memory_region_contract_sha256": memory_contract,
            **({"local_state": True} if local_state else {}),
            "literal_folding": fold_literals,
            **({"native_vfp_aarch64": True} if native_vfp_aarch64 else {}),
            **({"register_promotion": True} if promote_registers else {}),
            **giant_contract,
            **option_contract,
        }
        files = cached.get("files", {})
        cached_shards = {
            name for name in files if name.startswith(SHARD_PREFIX)
        }
        disk_shards = {path.name for path in output.glob(f"{SHARD_PREFIX}*")}
        if all(cached.get(key) == value for key, value in expected.items()) and (
            # Absent in manifests of the default (software) lowering.
            cached.get("native_vfp_aarch64", False) == native_vfp_aarch64
        ) and (
            cached.get("register_promotion", False) == promote_registers
        ) and (
            # Absent in manifests generated without the giant-function marks.
            cached.get("giant_cold_instructions", 0)
            == giant_function_instructions
        ) and all(
            cached.get(key, False) == (key in option_contract)
            for key in ("vfp_zero_aware", "lean_stack", "sparse_block_entries",
                        "fold_vfp_literals")
        ) and all(
            (output / name).is_file() and
            _sha256((output / name).read_bytes()) == digest
            for name, digest in files.items()
        ) and {HEADER_NAME, SOURCE_NAME} <= set(files) and (
            cached_shards == disk_shards
        ):
            return cached
    program = json.loads(program_bytes)
    if code_sha256 != program.get("code_sha256"):
        raise ValueError("code.bin does not match the structural AOT program")
    selection = json.loads(selection_bytes)
    functions, external_entries = _load_functions(program, selection, code)
    _FOLDED_LITERALS.clear()
    _FOLDED_VFP_LITERALS.clear()
    _SPARSE_STATS.clear()
    _LITERAL_IMAGE = None
    # Without a declared text size (minimal fixtures) nothing is folded.
    if fold_literals and program.get("executable_size") is not None:
        text_size = int(program["executable_size"])
        if not 0 < text_size <= len(code):
            raise ValueError("program executable size does not fit code.bin")
        _LITERAL_IMAGE = LiteralImage(int(program["base"]), code[:text_size])
    _NATIVE_VFP_AARCH64 = native_vfp_aarch64
    _PROMOTE_REGISTERS = promote_registers
    _VFP_ZERO_AWARE = vfp_zero_aware
    _LEAN_STACK = lean_stack
    _SPARSE_BLOCK_ENTRIES = sparse_block_entries
    _HOOK_GATES = frozenset(hook_gates or ()) if sparse_block_entries else frozenset()
    _FOLD_VFP_LITERALS = fold_vfp_literals
    _GIANT_COLD_ENTRIES = frozenset(
        function.entry
        for function in functions
        if giant_function_instructions
        and function.entry not in hot_entries
        and sum(len(block.instructions) for block in function.blocks)
        >= giant_function_instructions
    )
    try:
        return _generate_files(
            output, functions, external_entries, shard_count, shard_strategy,
            region_entries, memory_entries, local_state, fold_literals,
            memory_contract, program_sha256, selection_sha256, code_sha256,
            generator_sha256, {**giant_contract, **option_contract})
    finally:
        _LITERAL_IMAGE = None
        _NATIVE_VFP_AARCH64 = False
        _PROMOTE_REGISTERS = False
        _VFP_ZERO_AWARE = False
        _LEAN_STACK = False
        _SPARSE_BLOCK_ENTRIES = False
        _HOOK_GATES = frozenset()
        _FOLD_VFP_LITERALS = False
        _GIANT_COLD_ENTRIES = frozenset()


def _generate_files(
    output: Path,
    functions: tuple[Function, ...],
    external_entries: frozenset[int],
    shard_count: int,
    shard_strategy: str,
    region_entries: frozenset[int],
    memory_entries: frozenset[int],
    local_state: bool,
    fold_literals: bool,
    memory_contract: str,
    program_sha256: str,
    selection_sha256: str,
    code_sha256: str,
    generator_sha256: str,
    giant_contract: dict[str, object] | None = None,
) -> dict[str, object]:
    if region_entries - {function.entry for function in functions}:
        raise ValueError("callback-free regions must be selected function entries")
    if memory_entries - {function.entry for function in functions}:
        raise ValueError("memory regions must be selected function entries")
    header = _render_header().encode("utf-8")
    generated_files: dict[str, bytes] = {HEADER_NAME: header}
    shard_records: list[dict[str, object]] = []
    if shard_count == 1:
        generated_files[SOURCE_NAME] = _render_source(
            functions, external_entries, region_entries, memory_entries,
            local_state,
        ).encode("utf-8")
    else:
        function_symbols = {
            function.entry: _symbol(function.name, function.entry)
            for function in functions
        }
        existing_assignments = (
            _existing_shard_assignments(output, shard_count)
            if shard_strategy == "incremental"
            else _manifest_shard_assignments(output, shard_count)
            if shard_strategy == "affinity-incremental"
            else None
        )
        buckets = _shard_buckets(
            functions, shard_count, shard_strategy, existing_assignments
        )
        generated_files[INTERNAL_HEADER_NAME] = _render_internal_header().encode(
            "utf-8"
        )
        width = max(2, len(str(shard_count - 1)))
        functions_by_entry = {
            function.entry: function for function in functions
        }
        for index, bucket in enumerate(buckets):
            name = f"{SHARD_PREFIX}{index:0{width}d}.cpp"
            shard_functions = tuple(sorted(bucket, key=lambda item: item.entry))
            declaration_entries = {
                function.entry for function in shard_functions
            } | {
                target
                for function in shard_functions
                for _, target in function.direct_calls
                if target in functions_by_entry
            }
            declarations = tuple(
                functions_by_entry[entry]
                for entry in sorted(declaration_entries)
            )
            generated_files[name] = _render_shard_source(
                shard_functions,
                function_symbols,
                external_entries,
                declarations,
                region_entries,
                memory_entries,
                local_state,
            ).encode("utf-8")
            shard_records.append(
                {
                    "file": name,
                    "functions": len(shard_functions),
                    "entries": [
                        function.entry for function in shard_functions
                    ],
                    "instructions": sum(
                        len(block.instructions)
                        for function in shard_functions
                        for block in function.blocks
                    ),
                }
            )
        generated_files[SOURCE_NAME] = _render_registry_source(functions).encode(
            "utf-8"
        )
    if memory_entries:
        for name, data in generated_files.items():
            if b'#include "oot3d_native_a32_memory_region.h"' in data:
                generated_files[name] = f"// memory-region contract: {memory_contract}\n".encode() + data
    manifest = {
        "format": OUTPUT_FORMAT,
        "program_sha256": program_sha256,
        "selection_sha256": selection_sha256,
        "code_sha256": code_sha256,
        "generator_sha256": generator_sha256,
        "shard_count": shard_count,
        "shard_strategy": shard_strategy,
        "callback_free_regions": sorted(region_entries),
        "memory_regions": sorted(memory_entries),
        "memory_region_contract_sha256": memory_contract,
        **({"local_state": True} if local_state else {}),
        "literal_folding": fold_literals,
        **({"native_vfp_aarch64": True} if _NATIVE_VFP_AARCH64 else {}),
        **({"register_promotion": True} if _PROMOTE_REGISTERS else {}),
        **(giant_contract or {}),
        **(
            {"giant_cold_functions": sorted(_GIANT_COLD_ENTRIES)}
            if "giant_cold_instructions" in (giant_contract or {})
            else {}
        ),
        "folded_literal_addresses": len(_FOLDED_LITERALS),
        **(
            {"folded_vfp_literal_addresses": len(_FOLDED_VFP_LITERALS)}
            if _FOLD_VFP_LITERALS
            else {}
        ),
        **(
            {
                "sparse_block_entry_counts": dict(sorted(_SPARSE_STATS.items())),
                # Listed gates that start no block: no hook can run there in
                # either lowering.
                "hook_gates_without_block": sorted(
                    _HOOK_GATES - {block.pc for function in functions for block in function.blocks}),
            }
            if _SPARSE_BLOCK_ENTRIES
            else {}
        ),
        "folding_excluded_ranges": [
            [start, length] for start, length in HOST_WRITTEN_TEXT_RANGES
        ] if fold_literals else [],
        "shards": shard_records,
        "functions": [
            {
                "entry": function.entry,
                "name": function.name,
                "blocks": len(function.blocks),
                "instructions": sum(
                    len(block.instructions) for block in function.blocks
                ),
                "direct_calls": len(function.direct_calls),
                "resume_entries": list(function.resume_entries),
                "dispatch_entries": list(_function_dispatch_entries(function)),
            }
            for function in functions
        ],
        "external_functions": sorted(external_entries),
        "files": {
            name: _sha256(data) for name, data in generated_files.items()
        },
    }
    output.mkdir(parents=True, exist_ok=True)
    for stale in output.glob(f"{SHARD_PREFIX}*"):
        if stale.name not in generated_files:
            stale.unlink()
    if INTERNAL_HEADER_NAME not in generated_files:
        (output / INTERNAL_HEADER_NAME).unlink(missing_ok=True)
    for name, data in generated_files.items():
        _write_if_different(output / name, data)
    _write_if_different(
        output / MANIFEST_NAME,
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    return manifest
