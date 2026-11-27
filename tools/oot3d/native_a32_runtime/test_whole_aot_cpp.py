from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from whole_aot_cpp import (
    HEADER_NAME,
    PROGRAM_FORMAT,
    SELECTION_FORMAT,
    SOURCE_NAME,
    Instruction,
    LoweringError,
    _emit_vfp,
    generate,
)


def words(*values: int) -> bytes:
    return b"".join(value.to_bytes(4, "little") for value in values)


class WholeAotCppTests(unittest.TestCase):
    def test_callback_free_region_is_opt_in_and_cache_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(root, words(0xE2800002, 0xE12FFF1E))
            output = root / "output"
            generate(*inputs, output)
            original = (output / SOURCE_NAME).read_bytes()
            header = (output / HEADER_NAME).read_bytes()
            manifest = generate(*inputs, output, region_entries=frozenset({0x1000}))
            self.assertEqual(manifest["callback_free_regions"], [0x1000])
            self.assertEqual(header, (output / HEADER_NAME).read_bytes())
            generate(*inputs, output)
            self.assertEqual(original, (output / SOURCE_NAME).read_bytes())

    def test_register_promotion_is_opt_in_exact_shape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(
                0xE3A04005,  # MOV R4, #5
                0xEB000001,  # BL 0x1010
                0xE2800001,  # ADD R0, R0, #1
                0xE12FFF1E,  # BX LR
                0xE3A01007,  # MOV R1, #7 (callee)
                0xE12FFF1E,  # BX LR
            )
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(json.dumps({
                "format": PROGRAM_FORMAT,
                "base": 0x1000,
                "code_sha256": hashlib.sha256(code).hexdigest(),
                "blocks": [
                    {"id": 0, "pc": 0x1000, "end_pc": 0x1008, "successors": [
                        {"site": 0x1004, "kind": "call", "target": 0x1010},
                        {"site": 0x1004, "kind": "fallthrough", "target": 0x1008}]},
                    {"id": 1, "pc": 0x1008, "end_pc": 0x1010, "successors": [
                        {"site": 0x100C, "kind": "return"}]},
                    {"id": 2, "pc": 0x1010, "end_pc": 0x1018, "successors": [
                        {"site": 0x1014, "kind": "return"}]},
                ],
                "functions": [
                    {"entry": 0x1000, "name": "Caller", "blocks": [0, 1],
                     "closed_static_cfg": True,
                     "direct_calls": [{"site": 0x1004, "target": 0x1010}],
                     "tail_calls": [], "indirect_sites": []},
                    {"entry": 0x1010, "name": "Callee", "blocks": [2],
                     "closed_static_cfg": True, "direct_calls": [],
                     "tail_calls": [], "indirect_sites": []},
                ],
            }), encoding="utf-8")
            selection_path.write_text(json.dumps({
                "format": SELECTION_FORMAT,
                "functions": [{"entry": 0x1000}, {"entry": 0x1010}],
            }), encoding="utf-8")
            inputs = (program_path, selection_path, code_path)
            output = root / "output"
            generate(*inputs, output)
            reference = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("uint32_t& r4 = state.R[4];", reference)
            manifest = generate(*inputs, output, promote_registers=True)
            self.assertTrue(manifest["register_promotion"])
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            caller = source[source.index("Execute_Caller_00001000(\n    Oot3dWholeAotFrame& frame"):]
            caller = caller[:caller.index("\n}\n")]
            self.assertIn("uint32_t r4 = state.R[4];", caller)
            self.assertIn("uint32_t blocksRemaining = context.BlocksRemaining;", caller)
            # No lambda passed along: the locals do not escape.
            self.assertNotIn("commitState, reloadState", caller)
            self.assertIn("Oot3dAotRunBlockEntryHook(context, frame, state, 0x00001000U);", caller)
            # Before the call, only the dirty set (r4 and lr) and the budget.
            call = caller.index("Execute_Callee_00001010(frame, context, state")
            before = caller[caller.rindex("OOT3D_AOT_STAT(context.Stats.DirectCalls);", 0, call):call]
            self.assertIn("state.R[4] = r4;", before)
            self.assertIn("state.R[14] = r14;", before)
            self.assertNotIn("state.R[0] = r0;", before)
            self.assertIn("context.BlocksRemaining = blocksRemaining;", before)
            self.assertIn("reloadState();", caller[call:])
            # Returns and exits via shared labels that write back the state.
            self.assertIn("{ exitFlow = Oot3dAotReturned(r14); goto exit_", caller)
            self.assertRegex(caller, r"exit_\d+:\n    state\.R\[0\] = r0;")
            self.assertNotIn("\n        return ", caller)
            # Turned off again: the C++ is exactly the same as before.
            generate(*inputs, output)
            self.assertEqual(reference, (output / SOURCE_NAME).read_text(encoding="utf-8"))

    def test_giant_cold_attribute_keeps_shard_lines(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(root, words(0xE2800002, 0xE12FFF1E))
            plain, marked, hot = root / "plain", root / "marked", root / "hot"
            generate(*inputs, plain, shard_count=2, shard_strategy="affinity")
            manifest = generate(
                *inputs, marked, shard_count=2, shard_strategy="affinity",
                giant_function_instructions=1,
            )
            self.assertEqual(manifest["giant_cold_functions"], [0x1000])
            generate(
                *inputs, hot, shard_count=2, shard_strategy="affinity",
                giant_function_instructions=1, hot_entries=frozenset({0x1000}),
            )
            for path in plain.glob("oot3d_whole_aot_shard_*.cpp"):
                before = path.read_text(encoding="utf-8").splitlines()
                after = (marked / path.name).read_text(
                    encoding="utf-8").splitlines()
                self.assertEqual(len(before), len(after))
                self.assertEqual(
                    path.read_bytes(), (hot / path.name).read_bytes()
                )
                for old, new in zip(before, after):
                    if old != new:
                        self.assertEqual(
                            new, '[[gnu::optimize("no-schedule-insns", '
                            '"no-gcse-after-reload")]] ' + old,
                        )

    def test_callback_free_region_rejects_unknown_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(root, words(0xE12FFF1E))
            with self.assertRaisesRegex(ValueError, "selected function"):
                generate(*inputs, root / "out", region_entries=frozenset({0x2000}))

    def test_callback_free_region_rejects_external_and_observable_edges(self) -> None:
        for edge in ({"kind": "branch", "target": 0x9000},
                     {"kind": "indirect_call"}, {"kind": "resume"}):
            with self.subTest(edge=edge), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                inputs = self.write_inputs(root, words(0xE12FFF1E))
                program = json.loads(inputs[0].read_text())
                program["blocks"][0]["successors"] = [edge]
                inputs[0].write_text(json.dumps(program))
                with self.assertRaisesRegex(LoweringError, "closed call-free"):
                    generate(*inputs, root / "out", region_entries=frozenset({0x1000}))

    def test_callback_free_region_rejects_observable_instructions(self) -> None:
        for instruction in (0xEF000001, 0xEB000000, 0xE12FFF33):
            with self.subTest(instruction=instruction), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                inputs = self.write_inputs(root, words(instruction, 0xE12FFF1E))
                with self.assertRaisesRegex(LoweringError, "closed call-free"):
                    generate(*inputs, root / "out", region_entries=frozenset({0x1000}))

    def write_inputs(
        self, root: Path, code: bytes, *, direct_calls: list[dict] | None = None
    ) -> tuple[Path, Path, Path]:
        code_path = root / "code.bin"
        program_path = root / "program.json"
        selection_path = root / "selection.json"
        code_path.write_bytes(code)
        program_path.write_text(
            json.dumps(
                {
                    "format": PROGRAM_FORMAT,
                    "base": 0x1000,
                    "code_sha256": hashlib.sha256(code).hexdigest(),
                    "blocks": [
                        {
                            "id": 0,
                            "pc": 0x1000,
                            "end_pc": 0x1000 + len(code),
                            "successors": [
                                {"site": 0x1000 + len(code) - 4, "kind": "return"}
                            ],
                        }
                    ],
                    "functions": [
                        {
                            "entry": 0x1000,
                            "name": "SyntheticFunction",
                            "blocks": [0],
                            "closed_static_cfg": True,
                            "direct_calls": direct_calls or [],
                            "tail_calls": [],
                            "indirect_sites": [],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        selection_path.write_text(
            json.dumps(
                {
                    "format": SELECTION_FORMAT,
                    "functions": [{"entry": 0x1000}],
                }
            ),
            encoding="utf-8",
        )
        return program_path, selection_path, code_path

    def test_emits_direct_cfg_and_named_registers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xE3A00001, 0xE2800002, 0xE12FFF1E),
            )
            output = root / "output"
            manifest = generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertEqual(manifest["functions"][0]["instructions"], 3)
            self.assertIn("r0 = 0x00000001U", source)
            self.assertIn(
                "r0 = r0 + operand", source
            )
            self.assertIn(
                "return Oot3dAotReturned(r14)", source
            )
            self.assertNotIn("PackedOp", source)
            self.assertNotIn("ExecuteBlock", source)
            self.assertNotIn("ExecuteVfpScalar", source)
            self.assertIn("case Oot3dWholeAotFlowKind::Unsupported:", source)
            self.assertIn("a32::ExitKind::Unsupported, flow.Pc", source)
            self.assertIn("a32::FallbackReason::Unsupported", source)
            self.assertTrue((output / HEADER_NAME).is_file())

    def test_consumes_per_flag_liveness_in_promoted_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xE3500000, 0xE3510000, 0xE12FFF1E),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn(
                "Oot3dAotSetSubtractFlags(flags, r0, 0x00000000U, "
                "static_cast<Oot3dAotFlagMask>(0x0U));",
                source,
            )
            self.assertIn(
                "Oot3dAotSetSubtractFlags(flags, r1, 0x00000000U, "
                "static_cast<Oot3dAotFlagMask>(0xFU));",
                source,
            )
            self.assertNotIn("frame.Guest.cpsr", source)

    def test_emits_nonvirtual_typed_memory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xE5901000, 0xE5801004, 0xE12FFF1E),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("ReadFast<uint32_t>", source)
            self.assertIn("WriteFast<uint32_t>", source)
            self.assertNotIn("memory.Read32", source)

    def test_emits_logical_shifter_carry_and_nop(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE320F000,  # NOP
                    0xE1310082,  # TEQ r1, r2, LSL #1
                    0xE1810002,  # ORR r0, r1, r2
                    0xE12FFF1E,  # BX LR
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("Oot3dAotShiftImmediate", source)
            self.assertIn("Oot3dAotSetLogicalFlags", source)
            self.assertIn(
                "r0 = r1 | r2",
                source,
            )
            self.assertNotIn("0xE320F000", source)

    def test_non_flag_register_shift_preserves_zero_amount_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE1A0E03B,  # MOV lr, r11, LSR r0
                    0xE12FFF1E,  # BX lr
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn(
                "Oot3dAotShiftRegister(r11, 1U, r0, false).Value", source
            )
            self.assertNotIn("Oot3dAotLsr(r11, r0 & 0xFFU)", source)

    def test_emits_count_leading_zeros(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xE16FCF11, 0xE12FFF1E),  # CLZ r12, r1; BX LR
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("uint32_t value = r1", source)
            self.assertIn("while (count < 32U", source)
            self.assertIn("r12 = count", source)
            self.assertNotIn("Oot3dAotSetAddFlags", source)

    def test_emits_wide_immediates_and_saturating_arithmetic(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE3010234,  # MOVW r0, #0x1234
                    0xE3450678,  # MOVT r0, #0x5678
                    0xE1020051,  # QADD r0, r1, r2
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("r0 = 0x00001234U", source)
            self.assertIn("0x56780000U", source)
            self.assertIn("Oot3dAotSaturateSigned32", source)
            self.assertIn("Oot3dAotSetSaturationFlag", source)

    def test_emits_special_signed_multiply_forms(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE1003281,  # SMLABB r0, r1, r2, r3
                    0xE1443281,  # SMLALBB r3, r4, r1, r2
                    0xE0456392,  # UMAAL r6, r5, r2, r3
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("const int64_t value", source)
            self.assertIn("const int64_t product", source)
            self.assertIn("uint64_t value", source)

    def test_emits_media_reversal_packing_and_saturation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE6BF2F32,  # REV r2, r2
                    0xE6800012,  # PKHBT r0, r0, r2
                    0xE6E80010,  # USAT r0, #8, r0
                    0xE6BF0070,  # SXTH r0, r0
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("source & 0x000000FFU", source)
            self.assertIn("shifted.Value", source)
            self.assertIn("const int64_t clamped", source)
            self.assertIn("static_cast<int16_t>(source)", source)

    def test_emits_exact_vfp_float_to_signed_conversion(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xEEBD0AC0, 0xE12FFF1E),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("VfpBinary32ToSigned", source)

    def test_emits_inline_vfp_fast_paths_with_exact_fallbacks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                # VADD, VMLA, VDIV, VCVT.F32.S32 and VCVT.F32.U32 (all s0).
                words(0xEE300A00, 0xEE000A00, 0xEE800A00, 0xEEB80AC0,
                      0xEEB80A40, 0xE12FFF1E),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("Oot3dVfpAdd(", source)
            self.assertIn("Oot3dVfpMultiplyAccumulate(", source)
            self.assertIn("Oot3dVfpFromSigned(", source)
            # No exact host-float form: stay on the software implementation.
            self.assertIn("a32::VfpBinary32Divide(", source)
            self.assertIn("a32::VfpBinary32FromUnsigned(", source)
            self.assertNotIn("a32::VfpBinary32Add(", source)

    def test_native_vfp_aarch64_is_opt_in_and_cache_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                # VADD, VMLA, VNMLS, VDIV, VSQRT, VCVT.S32.F32, VCVT.F32.U32,
                # VCMP, VCMPE, VABS, VMRS r0, VMSR, VMRS APSR_nzcv.
                words(0xEE300A00, 0xEE000A00, 0xEE100A00, 0xEE800A00,
                      0xEEB10AC0, 0xEEBD0AC0, 0xEEB80A40, 0xEEB40A40,
                      0xEEB40AC0, 0xEEB00AC0, 0xEEF10A10, 0xEEE10A10,
                      0xEEF1FA10, 0xE12FFF1E),
            )
            output = root / "output"
            generate(*inputs, output)
            original = (output / SOURCE_NAME).read_bytes()
            self.assertNotIn(b"Oot3dVfpNative", original)
            self.assertNotIn(b"Oot3dGuestFp", original)
            manifest = generate(*inputs, output, native_vfp_aarch64=True)
            self.assertTrue(manifest["native_vfp_aarch64"])
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            for operation in (
                "Oot3dVfpNativeAdd(", "Oot3dVfpNativeMultiplyAccumulate(",
                "Oot3dVfpNativeNegativeMultiplySubtract(", "Oot3dVfpNativeDivide(",
                "Oot3dVfpNativeSquareRoot(", "Oot3dVfpNativeToSigned(",
                "Oot3dVfpNativeFromUnsigned(",
            ):
                self.assertIn(operation, source)
            self.assertEqual(source.count("Oot3dVfpNativeCompare("), 2)
            self.assertIn("    true, frame.Guest.fpscr);", source)
            self.assertIn("Oot3dGuestFpScope guestFpScope(state.fpscr);", source)
            self.assertIn("Oot3dGuestFpFoldFlags(frame.Guest.fpscr);", source)
            self.assertIn("Oot3dGuestFpLoadControl(frame.Guest.fpscr);", source)
            # Only the software lowering's helpers would appear otherwise.
            self.assertNotIn("Oot3dVfpAdd(", source)
            self.assertNotIn("a32::VfpBinary32", source)
            self.assertNotIn("Oot3dAotCommitVfp", source)
            # VABS stays a bit operation; mode checks stay in front of each op.
            self.assertIn("& 0x7FFFFFFFU;", source)
            self.assertIn("(frame.Guest.fpscr & 0x00379F00U) != 0U", source)
            generate(*inputs, output)
            self.assertEqual(original, (output / SOURCE_NAME).read_bytes())

    def test_vfp_zero_aware_is_opt_in_and_cache_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                # VADD, VSUB, VMUL, VNMUL, VMLA, VMLS, VNMLA, VNMLS, VDIV,
                # VSQRT, VCVT.S32.F32, VCMP.
                words(0xEE300A00, 0xEE300A40, 0xEE200A00, 0xEE200A40,
                      0xEE000A00, 0xEE000A40, 0xEE100A40, 0xEE100A00,
                      0xEE800A00, 0xEEB10AC0, 0xEEBD0AC0, 0xEEB40A40,
                      0xE12FFF1E),
            )
            output = root / "output"
            with self.assertRaises(ValueError):
                generate(*inputs, output, vfp_zero_aware=True)
            generate(*inputs, output, native_vfp_aarch64=True)
            native = (output / SOURCE_NAME).read_bytes()
            self.assertNotIn(b"ZeroAware", native)
            manifest = generate(*inputs, output, native_vfp_aarch64=True,
                                vfp_zero_aware=True)
            self.assertTrue(manifest["vfp_zero_aware"])
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            for operation in (
                "Add", "Subtract", "Multiply", "NegativeMultiply",
                "MultiplyAccumulate", "MultiplySubtract",
                "NegativeMultiplyAccumulate", "NegativeMultiplySubtract",
                "Divide", "SquareRoot",
            ):
                self.assertIn(f"Oot3dVfpZeroAware{operation}(", source)
                self.assertNotIn(f"Oot3dVfpNative{operation}(", source)
            # Conversions and compares have no zero-aware variant.
            self.assertIn("Oot3dVfpNativeToSigned(", source)
            self.assertIn("Oot3dVfpNativeCompare(", source)
            manifest = generate(*inputs, output, native_vfp_aarch64=True)
            self.assertNotIn("vfp_zero_aware", manifest)
            self.assertEqual(native, (output / SOURCE_NAME).read_bytes())

    def test_lean_stack_reads_and_multiple_loads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE5910004,  # LDR r0, [r1, #4]
                    0xE1D120B2,  # LDRH r2, [r1, #2]
                    0xE1C120D0,  # LDRD r2, r3, [r1]
                    0xE8BD0030,  # POP {r4, r5}
                    0xED920A00,  # VLDR s0, [r2]
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            original = (output / SOURCE_NAME).read_bytes()
            text = original.decode("utf-8")
            self.assertIn("uint32_t value = 0;", text)
            self.assertIn("&spanWords[1]", text)
            manifest = generate(*inputs, output, lean_stack=True)
            self.assertTrue(manifest["lean_stack"])
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            # Single reads: no dead initializer, same ReadFast call.
            self.assertIn("uint32_t value;", source)
            self.assertIn("uint16_t value;", source)
            self.assertNotIn("uint32_t value = 0;", source)
            self.assertNotIn("uint16_t value = 0;", source)
            # LDRD keeps its initialized fault address (read by the fault exit).
            self.assertIn("uint32_t faultAddress = address;", source)
            # LDM: the single-page path still fills spanWords; the fallback
            # reads into its own temporaries, in the same order, and faults at
            # the same addresses.
            self.assertNotIn("&spanWords[", source)
            self.assertIn("context.Memory.ReadWordsFast(firstAddress, spanWords, 2U)", source)
            self.assertIn("firstAddress + 4U, &word)) {", source)
            self.assertIn("spanWords[1] = word;", source)
            self.assertIn("r5 = spanWords[1];", source)
            self.assertLess(source.index("firstAddress + 0U, &word"),
                            source.index("firstAddress + 4U, &word"))
            manifest = generate(*inputs, output)
            self.assertNotIn("lean_stack", manifest)
            self.assertEqual(original, (output / SOURCE_NAME).read_bytes())

    def write_program(
        self, root: Path, code: bytes, blocks: list[dict], *,
        executable_size: int | None = None,
    ) -> tuple[Path, Path, Path]:
        """One function at 0x1000 made of `blocks` (pc, end_pc, successors)."""
        code_path = root / "code.bin"
        program_path = root / "program.json"
        selection_path = root / "selection.json"
        code_path.write_bytes(code)
        program = {
            "format": PROGRAM_FORMAT,
            "base": 0x1000,
            "code_sha256": hashlib.sha256(code).hexdigest(),
            "blocks": [{"id": index, **block} for index, block in enumerate(blocks)],
            "functions": [{
                "entry": 0x1000, "name": "Synthetic", "blocks": list(range(len(blocks))),
                "closed_static_cfg": True, "direct_calls": [], "tail_calls": [],
                "indirect_sites": [],
            }],
        }
        if executable_size is not None:
            program["executable_size"] = executable_size
        program_path.write_text(json.dumps(program), encoding="utf-8")
        selection_path.write_text(json.dumps({
            "format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}],
        }), encoding="utf-8")
        return program_path, selection_path, code_path

    @staticmethod
    def block_text(source: str, pc: int) -> str:
        start = source.index(f"block_{pc:08X}:\n")
        end = source.index("\n    }\n", start)
        return source[start:end]

    def test_sparse_block_entries_budget_at_entry_loops_and_gates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_program(
                root,
                words(
                    0xE3A00000,  # 0x1000 MOV r0, #0
                    0xE2800001,  # 0x1004 ADD r0, r0, #1   (loop header)
                    0xE350000A,  # 0x1008 CMP r0, #10
                    0x1AFFFFFC,  # 0x100C BNE 0x1004
                    0xE3A01002,  # 0x1010 MOV r1, #2      (gated)
                    0xE12FFF1E,  # 0x1014 BX LR           (plain)
                ),
                [
                    {"pc": 0x1000, "end_pc": 0x1004, "successors": [
                        {"site": 0x1000, "kind": "fallthrough", "target": 0x1004}]},
                    {"pc": 0x1004, "end_pc": 0x1010, "successors": [
                        {"site": 0x100C, "kind": "branch", "target": 0x1004, "condition": "ne"},
                        {"site": 0x100C, "kind": "fallthrough", "target": 0x1010}]},
                    {"pc": 0x1010, "end_pc": 0x1014, "successors": [
                        {"site": 0x1010, "kind": "fallthrough", "target": 0x1014}]},
                    {"pc": 0x1014, "end_pc": 0x1018, "successors": [
                        {"site": 0x1014, "kind": "return"}]},
                ],
            )
            output = root / "output"
            generate(*inputs, output)
            original = (output / SOURCE_NAME).read_bytes()
            header = (output / HEADER_NAME).read_bytes()
            text = original.decode("utf-8")
            self.assertEqual(text.count("Oot3dAotEnterBlock(context, frame, state, 0x"), 4)
            self.assertIn("Oot3dWholeAotHookGateSet Oot3dWholeAotHookGates() noexcept {\n"
                          "    return {};", text)
            with self.assertRaisesRegex(ValueError, "hook gate list"):
                generate(*inputs, output, sparse_block_entries=True)
            manifest = generate(*inputs, output, sparse_block_entries=True,
                                hook_gates=frozenset({0x1010, 0x2000}))
            self.assertTrue(manifest["sparse_block_entries"])
            self.assertEqual(manifest["hook_gates"], [0x1010, 0x2000])
            self.assertEqual(manifest["hook_gates_without_block"], [0x2000])
            self.assertEqual(manifest["sparse_block_entry_counts"], {
                "blocks": 4, "gated_blocks": 1, "budget_only_blocks": 2,
                "blocks_without_entry_code": 1,
            })
            # Same dispatch entries (resumable at every block).
            self.assertEqual(manifest["functions"][0]["dispatch_entries"], [0x1004, 0x1010, 0x1014])
            self.assertEqual(header, (output / HEADER_NAME).read_bytes())
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            # Entry and loop header: budget only; gated block: budget and gate;
            # the plain block: nothing.
            for pc in (0x1000, 0x1004):
                block = self.block_text(source, pc)
                self.assertIn("if (!Oot3dAotConsumeBlock(context)) {", block)
                self.assertIn(f"return Oot3dAotBlockLimit(context, 0x{pc:08X}U);", block)
            self.assertIn("Oot3dAotEnterBlock(context, frame, state, 0x00001010U, commitState, reloadState)",
                          self.block_text(source, 0x1010))
            plain = self.block_text(source, 0x1014)
            self.assertNotIn("Oot3dAotConsumeBlock", plain)
            self.assertNotIn("Oot3dAotEnterBlock", plain)
            self.assertNotIn("BlockLimit", plain)
            # Dispatch: self-charging blocks jump directly; the plain block is
            # charged by the dispatch, which may stop there with BlockLimit.
            function = source[source.index("Oot3dWholeAotFlow Execute_Synthetic_00001000(\n"):]
            dispatch = function[function.index("    switch (entryPc) {"):function.index("block_00001000:")]
            self.assertIn("    case 0x00001004U:\n        goto block_00001004;", dispatch)
            self.assertIn("    case 0x00001010U:\n        goto block_00001010;", dispatch)
            self.assertIn("    case 0x00001014U:\n        break;", dispatch)
            charge = dispatch.index("    if (!Oot3dAotConsumeBlock(context)) {")
            self.assertLess(dispatch.index("case 0x00001014U:\n        break;"), charge)
            self.assertIn("return Oot3dAotBlockLimit(context, entryPc);", dispatch[charge:])
            self.assertIn("    case 0x00001014U:\n        goto block_00001014;", dispatch[charge:])
            self.assertEqual(dispatch.count("goto block_00001014;"), 1)
            self.assertIn("static constexpr std::array<uint32_t, 2> kHookGatePcs{0x00001010U, 0x00002000U};", source)
            self.assertIn("return {false, kHookGatePcs};", source)
            # Turned off again: the C++ is exactly the same as before.
            manifest = generate(*inputs, output)
            self.assertNotIn("sparse_block_entries", manifest)
            self.assertNotIn("hook_gates", manifest)
            self.assertEqual(original, (output / SOURCE_NAME).read_bytes())

    def test_sparse_block_entries_charge_irreducible_loops(self) -> None:
        # A enters the B <-> C cycle at both B and C (no single header).
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_program(
                root,
                words(
                    0xE3500000,  # 0x1000 CMP r0, #0
                    0x0A000000,  # 0x1004 BEQ 0x100C  (into C)
                    0xE2800001,  # 0x1008 ADD r0, r0, #1  (B)
                    0xE3500005,  # 0x100C CMP r0, #5      (C)
                    0x1AFFFFFC,  # 0x1010 BNE 0x1008  (back into B)
                    0xE12FFF1E,  # 0x1014 BX LR
                ),
                [
                    {"pc": 0x1000, "end_pc": 0x1008, "successors": [
                        {"site": 0x1004, "kind": "branch", "target": 0x100C, "condition": "eq"},
                        {"site": 0x1004, "kind": "fallthrough", "target": 0x1008}]},
                    {"pc": 0x1008, "end_pc": 0x100C, "successors": [
                        {"site": 0x1008, "kind": "fallthrough", "target": 0x100C}]},
                    {"pc": 0x100C, "end_pc": 0x1014, "successors": [
                        {"site": 0x1010, "kind": "branch", "target": 0x1008, "condition": "ne"},
                        {"site": 0x1010, "kind": "fallthrough", "target": 0x1014}]},
                    {"pc": 0x1014, "end_pc": 0x1018, "successors": [
                        {"site": 0x1014, "kind": "return"}]},
                ],
            )
            output = root / "output"
            manifest = generate(*inputs, output, sparse_block_entries=True, hook_gates=frozenset())
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            charged = {
                pc for pc in (0x1000, 0x1008, 0x100C, 0x1014)
                if "Oot3dAotConsumeBlock(context)" in self.block_text(source, pc)
            }
            self.assertIn(0x1000, charged)
            # The cycle B <-> C passes a decrement whichever way it is entered.
            self.assertTrue(charged & {0x1008, 0x100C})
            self.assertNotIn(0x1014, charged)
            self.assertEqual(manifest["sparse_block_entry_counts"]["gated_blocks"], 0)
            self.assertIn("static constexpr std::array<uint32_t, 0> kHookGatePcs{};", source)

    def test_sparse_block_entries_reject_other_lowerings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(root, words(0xE2800002, 0xE12FFF1E))
            for options in ({"region_entries": frozenset({0x1000})},
                            {"promote_registers": True}):
                with self.subTest(options=options):
                    with self.assertRaisesRegex(ValueError, "does not support"):
                        generate(*inputs, root / "out", sparse_block_entries=True,
                                 hook_gates=frozenset(), **options)

    def test_fold_vfp_literals_is_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_program(
                root,
                words(
                    0xED9F0A01,  # 0x1000 VLDR s0, [pc, #4] -> 0x100C
                    0xED9F1B01,  # 0x1004 VLDR d1, [pc, #4] -> 0x1010
                    0xE12FFF1E,  # 0x1008 BX LR
                    0x3F800000,  # 0x100C 1.0f
                    0x00000000, 0x3FF00000,  # 0x1010 1.0
                ),
                [{"pc": 0x1000, "end_pc": 0x100C, "successors": [
                    {"site": 0x1008, "kind": "return"}]}],
                executable_size=0x18,
            )
            output = root / "output"
            generate(*inputs, output)
            original = (output / SOURCE_NAME).read_bytes()
            self.assertEqual(original.decode("utf-8").count("context.Memory.ReadFast<"), 2)
            manifest = generate(*inputs, output, fold_vfp_literals=True)
            self.assertTrue(manifest["fold_vfp_literals"])
            self.assertEqual(manifest["folded_vfp_literal_addresses"], 2)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertNotIn("ReadFast<", source)
            self.assertIn("// VFP literal at 0x0000100C", source)
            self.assertIn("frame.Guest.vfp[0] = 0x3F800000U;", source)
            self.assertIn("frame.Guest.vfp[2] = 0x00000000U;", source)
            self.assertIn("frame.Guest.vfp[3] = 0x3FF00000U;", source)
            manifest = generate(*inputs, output)
            self.assertNotIn("fold_vfp_literals", manifest)
            self.assertEqual(original, (output / SOURCE_NAME).read_bytes())

    def test_emits_exact_vfp_square_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xEEB10AC0, 0xE12FFF1E),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("VfpBinary32SquareRoot", source)

    def test_emits_vfp_multiple_load_and_store(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xEC920A0C,  # VLDMIA r2, {s0-s11}
                    0xECA00A0C,  # VSTMIA r0!, {s0-s11}
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn(
                "const uint32_t firstAddress = r2", source
            )
            self.assertIn("frame.Guest.vfp[0] = value0", source)
            self.assertIn("frame.Guest.vfp[11] = value11", source)
            self.assertIn("firstAddress + 44U, frame.Guest.vfp[11]", source)
            self.assertIn(
                "r0 = r0 + 48U", source
            )

    def test_vfp_double_memory_transfers_preserve_both_words(self) -> None:
        for register in range(16):
            for add in (False, True):
                with self.subTest(register=register, add=add):
                    raw = 0xED120B03 | (register << 12) | (int(add) << 23)
                    load = "\n".join(_emit_vfp(Instruction(0x1000, raw)))
                    store = "\n".join(_emit_vfp(Instruction(0x1004, raw & ~(1 << 20))))
                    self.assertIn("ReadFast<uint64_t>", load)
                    self.assertIn(f"frame.Guest.vfp[{register * 2}] = static_cast<uint32_t>(value)", load)
                    self.assertIn(f"frame.Guest.vfp[{register * 2 + 1}] = static_cast<uint32_t>(value >> 32U)", load)
                    self.assertIn("WriteFast<uint64_t>", store)
                    self.assertIn(f"static_cast<uint64_t>(frame.Guest.vfp[{register * 2 + 1}]) << 32U", store)
                    self.assertIn(f"{'+' if add else '-'} 12U", load)

    def test_vfp_double_literal_and_conditional_memory(self) -> None:
        source = "\n".join(_emit_vfp(Instruction(0x1000, 0x1D9F0B03)))
        self.assertIn("0x00001008U + 12U", source)
        self.assertIn("ReadFast<uint64_t>", source)
        self.assertIn("if (!(state.Flags.Z()))", source)

    def test_vfp_single_memory_retains_odd_lanes(self) -> None:
        source = "\n".join(_emit_vfp(Instruction(0x1000, 0xEDD2FA03)))
        self.assertIn("ReadFast<uint32_t>", source)
        self.assertIn("frame.Guest.vfp[31] = value", source)
        self.assertNotIn("uint64_t", source)

    def test_vfp_double_memory_rejects_unmodelled_registers_and_pc_store(self) -> None:
        for raw in (0xEDD20B03, 0xEDC20B03, 0xED8F0B03):
            with self.subTest(raw=hex(raw)), self.assertRaises(LoweringError):
                _emit_vfp(Instruction(0x1000, raw))

    def test_emits_vfp_decrement_before_double_store(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xED2D8B06,  # VPUSH {d8-d10}
                    0xECBD8B06,  # VPOP {d8-d10}
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("r13 -= 24U", source)
            self.assertIn("frame.Guest.vfp[16]", source)
            self.assertIn("frame.Guest.vfp[21]", source)
            self.assertIn("r13 += 24U", source)

    def test_emits_doubleword_load_and_store(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE1CD84D0,  # LDRD r8, r9, [sp, #0x40]
                    0xE1CD20F0,  # STRD r2, r3, [sp]
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("ReadFast<uint64_t>", source)
            self.assertIn(
                "r8 = static_cast<uint32_t>(value)", source
            )
            self.assertIn(
                "r9 = static_cast<uint32_t>(value >> 32U)",
                source,
            )
            self.assertIn("WriteFast<uint64_t>", source)

    def test_emits_signed_long_multiply_accumulate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xE0E1C193, 0xE12FFF1E),  # SMLAL r12, r1, r3, r1
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("const int64_t product", source)
            self.assertIn(
                "r12 = static_cast<uint32_t>(value)", source
            )
            self.assertIn(
                "r1 = static_cast<uint32_t>(value >> 32U)",
                source,
            )

    def test_emits_thread_pointer_barrier_and_clear_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xEE1D0F70,  # MRC TPIDRURW, r0
                    0xEE075F9A,  # legacy DSB
                    0xF57FF01F,  # CLREX
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn(
                "r0 = frame.Guest.thread_pointer", source
            )
            self.assertIn("std::atomic_thread_fence", source)
            self.assertIn("frame.Guest.exclusive_valid = false", source)

    def test_emits_word_exclusive_load_and_store(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE1940F9F,  # LDREX r0, [r4]
                    0xE1842F90,  # STREX r2, r0, [r4]
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("context.Memory.LoadExclusive", source)
            self.assertIn("context.Memory.StoreExclusive", source)
            self.assertIn("r2 = success ? 0U : 1U", source)

    def test_emits_parallel_unsigned_saturating_subtract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xE66ECFF2, 0xE12FFF1E),  # UQSUB8 r12, lr, r2
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("const unsigned a = (left >> shift) & 0xFFU", source)
            self.assertIn("value |= (a > b ? a - b : 0U) << shift", source)
            self.assertIn("r12 = value", source)

    def test_incremental_sharding_preserves_unchanged_placement(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(
                0xEB000000,  # BL 0x1008
                0xE12FFF1E,  # BX LR
                0xE3A00007,  # MOV R0, #7
                0xE12FFF1E,  # BX LR
            )
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "call", "target": 0x1008},
                                    {"site": 0x1000, "kind": "fallthrough", "target": 0x1004},
                                ],
                            },
                            {
                                "id": 1,
                                "pc": 0x1004,
                                "end_pc": 0x1008,
                                "successors": [{"site": 0x1004, "kind": "return"}],
                            },
                            {
                                "id": 2,
                                "pc": 0x1008,
                                "end_pc": 0x1010,
                                "successors": [{"site": 0x100C, "kind": "return"}],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "Caller",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [{"site": 0x1000, "target": 0x1008}],
                                "tail_calls": [],
                                "indirect_sites": [],
                            },
                            {
                                "entry": 0x1008,
                                "name": "Callee",
                                "blocks": [2],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [],
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {
                        "format": SELECTION_FORMAT,
                        "functions": [{"entry": 0x1000}, {"entry": 0x1008}],
                    }
                ),
                encoding="utf-8",
            )
            balanced_output = root / "balanced_output"
            generate(
                program_path,
                selection_path,
                code_path,
                balanced_output,
                shard_count=2,
                shard_strategy="balanced",
            )
            balanced_shards = {
                path.name: path.read_bytes()
                for path in balanced_output.glob("oot3d_whole_aot_shard_*.cpp")
            }
            incremental_manifest = generate(
                program_path,
                selection_path,
                code_path,
                balanced_output,
                shard_count=2,
                shard_strategy="incremental",
            )
            self.assertEqual(
                incremental_manifest["shard_strategy"], "incremental"
            )
            self.assertEqual(
                balanced_shards,
                {
                    path.name: path.read_bytes()
                    for path in balanced_output.glob(
                        "oot3d_whole_aot_shard_*.cpp"
                    )
                },
            )

            selection = json.loads(selection_path.read_text(encoding="utf-8"))
            selection["functions"] = selection["functions"][:-1]
            selection["external_functions"] = [
                {
                    "entry": 0x1008,
                    "name": "Callee",
                    "reason": "typed test boundary",
                }
            ]
            selection_path.write_text(
                json.dumps(selection), encoding="utf-8"
            )
            caller_shard_before = next(
                index
                for index, shard in enumerate(incremental_manifest["shards"])
                if 0x1000 in shard["entries"]
            )
            removal_manifest = generate(
                program_path,
                selection_path,
                code_path,
                balanced_output,
                shard_count=2,
                shard_strategy="incremental",
            )
            caller_shard_after = next(
                index
                for index, shard in enumerate(removal_manifest["shards"])
                if 0x1000 in shard["entries"]
            )
            self.assertEqual(caller_shard_before, caller_shard_after)

    def test_external_call_continuation_is_a_dispatch_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(0xEB000000, 0xE12FFF1E, 0xE12FFF1E)
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "call", "target": 0x1008},
                                    {"site": 0x1000, "kind": "fallthrough", "target": 0x1004},
                                ],
                            },
                            {
                                "id": 1,
                                "pc": 0x1004,
                                "end_pc": 0x1008,
                                "successors": [{"site": 0x1004, "kind": "return"}],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "Caller",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [{"site": 0x1000, "target": 0x1008}],
                                "tail_calls": [],
                                "indirect_sites": [],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {
                        "format": SELECTION_FORMAT,
                        "functions": [{"entry": 0x1000}],
                        "external_functions": [{"entry": 0x1008}],
                    }
                ),
                encoding="utf-8",
            )
            output = root / "output"
            manifest = generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertEqual(manifest["functions"][0]["resume_entries"], [0x1004])
            self.assertIn("case 0x00001004U:", source)
            self.assertIn("goto block_00001004;", source)
            self.assertIn("Oot3dAotCallExternal(context, 0x00001008U", source)
            self.assertIn("std::array<uint32_t, 2>", source)

    def test_indirect_call_exits_and_reenters_aot(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(0xE12FFF33, 0xE12FFF1E)
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "indirect_call"},
                                    {"site": 0x1000, "kind": "resume", "target": 0x1004},
                                ],
                            },
                            {
                                "id": 1,
                                "pc": 0x1004,
                                "end_pc": 0x1008,
                                "successors": [{"site": 0x1004, "kind": "return"}],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "IndirectCaller",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [
                                    {"site": 0x1000, "kind": "indirect_call"}
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {"format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}]}
                ),
                encoding="utf-8",
            )
            output = root / "output"
            manifest = generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertEqual(manifest["functions"][0]["resume_entries"], [0x1004])
            self.assertIn("r14 = 0x00001004U", source)
            self.assertIn("ExecuteOot3dWholeAotIndirect", source)
            self.assertIn("r3, frame, context, state", source)
            self.assertIn("case 0x00001004U:", source)

    def test_flat_dispatch_table_finds_exactly_the_entries(self) -> None:
        compiler = (
            os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        )
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        # Entries on both sides of a 4 KiB page boundary, adjacent words and
        # a gap of several pages.
        entries = [0x1000, 0x1004, 0x1FFC, 0x2000, 0x2010, 0x5008]
        code = bytearray(0x5010 - 0x1000)
        for entry in entries:
            code[entry - 0x1000:entry - 0x1000 + 4] = (0xE12FFF1E).to_bytes(4, "little")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(bytes(code))
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(bytes(code)).hexdigest(),
                        "blocks": [
                            {
                                "id": index,
                                "pc": entry,
                                "end_pc": entry + 4,
                                "successors": [{"site": entry, "kind": "return"}],
                            }
                            for index, entry in enumerate(entries)
                        ],
                        "functions": [
                            {
                                "entry": entry,
                                "name": f"Leaf{index}",
                                "blocks": [index],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [],
                            }
                            for index, entry in enumerate(entries)
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {
                        "format": SELECTION_FORMAT,
                        "functions": [{"entry": entry} for entry in entries],
                    }
                ),
                encoding="utf-8",
            )
            output = root / "output"
            generate(program_path, selection_path, code_path, output, shard_count=2)
            registry = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("constexpr uint32_t kDispatchCodeBase = 0x00001000U;", registry)
            self.assertIn("constexpr uint32_t kDispatchSlotCount = 4099U;", registry)
            self.assertNotIn("lower_bound", registry)
            self.assertIn("profile->Record(state.R[14], pc);", registry)
            # The table and the lookup, compiled alone against stub types:
            # every pc around the range, aligned or not, finds exactly the
            # entry at that pc or nothing.
            begin = registry.index("using Function = ")
            end = registry.index("Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(", begin)
            symbols = sorted(set(re.findall(r"Execute_(\w+)\}", registry[begin:end])))
            self.assertEqual(len(symbols), len(entries))
            stubs = "\n".join(
                f"Oot3dWholeAotFlow Execute_{symbol}(Oot3dWholeAotFrame&, "
                "Oot3dWholeAotContext&, Oot3dAotArchitecturalState&, uint32_t) "
                "{ return {}; }"
                for symbol in symbols
            )
            expected = ", ".join(f"0x{entry:08X}U" for entry in entries)
            program = (
                "#include <array>\n#include <bit>\n#include <cstddef>\n"
                "#include <cstdint>\n#include <cstdio>\n#define OOT3D_AOT_HOT inline\n"
                "struct Oot3dWholeAotFlow {}; struct Oot3dWholeAotFrame {};\n"
                "struct Oot3dWholeAotContext {}; struct Oot3dAotArchitecturalState {};\n"
                f"{stubs}\n"
                f"{registry[begin:end]}\n"
                f"constexpr std::array<uint32_t, {len(entries)}> kExpected{{{expected}}};\n"
                "int main() {\n"
                "    const uint32_t probes[] = {0U, 0xFFFFFFFFU, 0xFFFFFFFCU, 0x80001000U,\n"
                "                               0x40001000U, 0xC0001000U};\n"
                "    for (uint32_t pc : probes)\n"
                "        if (FindDispatchEntry(pc) != nullptr) return 1;\n"
                "    for (uint32_t pc = 0x0; pc < 0x9000U; ++pc) {\n"
                "        bool want = false;\n"
                "        for (uint32_t entry : kExpected) want |= entry == pc;\n"
                "        const DispatchEntry* found = FindDispatchEntry(pc);\n"
                "        if ((found != nullptr) != want) return 2;\n"
                "        if (found != nullptr && found->Pc != pc) return 3;\n"
                "    }\n"
                "    std::puts(\"dispatch ok\");\n"
                "    return 0;\n"
                "}\n"
            )
            source = root / "dispatch.cpp"
            source.write_text(program, encoding="utf-8")
            executable = root / "dispatch"
            subprocess.run(
                [compiler, "-std=c++20", "-O2", str(source), "-o", str(executable)],
                check=True, capture_output=True, timeout=120,
            )
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=30
            )
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def _alias_inputs(self, root: Path, aliases: list[dict[str, int]] | None):
        entries = [0x1000, 0x1008]
        code = words(0xE12FFF1E, 0, 0xE12FFF1E)
        code_path = root / "code.bin"
        program_path = root / "program.json"
        selection_path = root / "selection.json"
        code_path.write_bytes(code)
        program_path.write_text(json.dumps({
            "format": PROGRAM_FORMAT, "base": 0x1000,
            "code_sha256": hashlib.sha256(code).hexdigest(),
            "blocks": [{"id": i, "pc": e, "end_pc": e + 4,
                        "successors": [{"site": e, "kind": "return"}]}
                       for i, e in enumerate(entries)],
            "functions": [{"entry": e, "name": f"Leaf{i}", "blocks": [i],
                           "closed_static_cfg": True, "direct_calls": [],
                           "tail_calls": [], "indirect_sites": []}
                          for i, e in enumerate(entries)],
        }), encoding="utf-8")
        selection = {"format": SELECTION_FORMAT,
                     "functions": [{"entry": e} for e in entries]}
        if aliases is not None:
            selection["dispatch_aliases"] = aliases
        selection_path.write_text(json.dumps(selection), encoding="utf-8")
        return program_path, selection_path, code_path

    def test_dispatch_aliases_resolve_thumb_entries_to_their_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self._alias_inputs(root, [{"alias": 0x2001, "target": 0x1008},
                                               {"alias": 0x0FFF, "target": 0x1000}])
            generate(*inputs, root / "out", shard_count=2)
            registry = (root / "out" / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("ExecuteAlias_00002001(", registry)
            self.assertIn("return Execute_Leaf1_00001008(frame, context, state, 0x00001008U);",
                          registry)
            self.assertIn("{0x00000FFFU, ExecuteAlias_00000FFF},", registry)
            self.assertIn("return FindDispatchAlias(pc);", registry)
            self.assertIn("kEntryPoints{0x00000FFFU, 0x00001000U, 0x00001008U, 0x00002001U}",
                          registry)
            compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
            if not compiler:
                return
            begin = registry.index("using Function = ")
            end = registry.index("Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(", begin)
            thunks = registry[registry.index("Oot3dWholeAotFlow ExecuteAlias_"):begin]
            program = (
                "#include <array>\n#include <bit>\n#include <cstddef>\n#include <cstdint>\n"
                "#define OOT3D_AOT_HOT inline\n#define OOT3D_AOT_COLD\n"
                "struct Oot3dWholeAotFlow { uint32_t Pc; }; struct Oot3dWholeAotFrame {};\n"
                "struct Oot3dWholeAotContext {}; struct Oot3dAotArchitecturalState {};\n"
                "Oot3dWholeAotFlow Execute_Leaf0_00001000(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,"
                " Oot3dAotArchitecturalState&, uint32_t pc) { return {pc}; }\n"
                "Oot3dWholeAotFlow Execute_Leaf1_00001008(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,"
                " Oot3dAotArchitecturalState&, uint32_t pc) { return {pc}; }\n"
                f"{thunks}\n{registry[begin:end]}\n"
                "int main() {\n"
                "    Oot3dWholeAotFrame f; Oot3dWholeAotContext c; Oot3dAotArchitecturalState s;\n"
                "    const struct { uint32_t pc, target; } cases[] = {\n"
                "        {0x1000U, 0x1000U}, {0x1008U, 0x1008U}, {0x2001U, 0x1008U},\n"
                "        {0x0FFFU, 0x1000U}, {0x1001U, 0U}, {0x2000U, 0U}, {0x1004U, 0U},\n"
                "        {0xFFFFFFFFU, 0U}, {0U, 0U}};\n"
                "    for (const auto& item : cases) {\n"
                "        const DispatchEntry* e = FindDispatchEntry(item.pc);\n"
                "        if ((e != nullptr) != (item.target != 0U)) return 1;\n"
                "        if (e != nullptr && e->Execute(f, c, s, item.pc).Pc != item.target) return 2;\n"
                "    }\n"
                "    return 0;\n"
                "}\n"
            )
            source = root / "alias.cpp"
            source.write_text(program, encoding="utf-8")
            executable = root / "alias"
            subprocess.run([compiler, "-std=c++20", "-O2", str(source), "-o", str(executable)],
                           check=True, capture_output=True, timeout=120)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_dispatch_alias_colliding_with_an_entry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self._alias_inputs(root, [{"alias": 0x1008, "target": 0x1000}])
            with self.assertRaisesRegex(Exception, "collides with an entry"):
                generate(*inputs, root / "out", shard_count=2)
            inputs = self._alias_inputs(root, [{"alias": 0x3001, "target": 0x1004}])
            with self.assertRaisesRegex(Exception, "is not selected"):
                generate(*inputs, root / "out2", shard_count=2)

    def test_single_file_registry_rejects_dispatch_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self._alias_inputs(root, [{"alias": 0x2001, "target": 0x1008}])
            with self.assertRaisesRegex(Exception, "sharded registry"):
                generate(*inputs, root / "out", shard_count=1)

    def test_registry_without_aliases_is_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generate(*self._alias_inputs(root, None), root / "a", shard_count=2)
            generate(*self._alias_inputs(root, []), root / "b", shard_count=2)
            a = (root / "a" / SOURCE_NAME).read_text(encoding="utf-8")
            b = (root / "b" / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertEqual(a, b)
            self.assertNotIn("FindDispatchAlias", a)
            self.assertNotIn("ExecuteAlias_", a)

    def test_computed_add_pc_call_uses_existing_link_register(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(0xE088F003, 0xE12FFF1E)  # ADD pc,r8,r3; BX lr
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "indirect_call"},
                                    {"site": 0x1000, "kind": "resume", "target": 0x1004},
                                ],
                            },
                            {
                                "id": 1,
                                "pc": 0x1004,
                                "end_pc": 0x1008,
                                "successors": [{"site": 0x1004, "kind": "return"}],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "ComputedCaller",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [
                                    {"site": 0x1000, "kind": "indirect_call"}
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {"format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}]}
                ),
                encoding="utf-8",
            )
            output = root / "output"
            generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("(r8 + r3), frame, context, state", source)
            self.assertNotIn("r14 = 0x00001004U", source)

            # A manually established LR need not name the next instruction.
            # A failed predicate still falls through, not to that return site.
            code = words(0x1088F003, 0xE12FFF1E, 0xE12FFF1E)
            code_path.write_bytes(code)
            program = json.loads(program_path.read_text())
            program["code_sha256"] = hashlib.sha256(code).hexdigest()
            program["blocks"][0]["successors"][1]["target"] = 0x1008
            program["blocks"][0]["successors"].append(
                {"site": 0x1000, "kind": "fallthrough", "target": 0x1004})
            program["blocks"].append({"id": 2, "pc": 0x1008, "end_pc": 0x100C,
                                      "successors": [{"site": 0x1008, "kind": "return"}]})
            program["functions"][0]["blocks"].append(2)
            program_path.write_text(json.dumps(program))
            generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text()
            self.assertIn("callFlow.Pc != 0x00001008U", source)
            self.assertRegex(source, r"goto block_00001008;\s*}\s*goto block_00001004;")

    def test_emits_program_status_vmsr_and_binary64_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(
                    0xE10F2000,  # MRS r2,CPSR
                    0xE128F002,  # MSR CPSR_f,r2
                    0xEEE10A10,  # VMSR FPSCR,r0
                    0xEC410B10,  # VMOV d0,r0,r1
                    0xEEB03B40,  # VMOV.F64 d3,d0
                    0xEEB72AC2,  # VCVT.F64.F32 d2,s4
                    0xE12FFF1E,
                ),
            )
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("Materialize(state.PreservedCpsr())", source)
            self.assertIn("SetPreservedCpsrBits(0xFF000000U, value)", source)
            self.assertIn("fpscr = r0", source)
            self.assertIn("frame.Guest.vfp[1] = r1", source)
            self.assertIn("frame.Guest.vfp[6] = low", source)
            self.assertIn("a32::ExecuteVfpBinary64", source)

    def test_terminal_prefetch_is_not_a_pc_load(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(0xF5D1F040, 0xE12FFF1E)  # PLD [r1,#64]; BX lr
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "fallthrough", "target": 0x1004}
                                ],
                            },
                            {
                                "id": 1,
                                "pc": 0x1004,
                                "end_pc": 0x1008,
                                "successors": [{"site": 0x1004, "kind": "return"}],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "PrefetchThenReturn",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {"format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}]}
                ),
                encoding="utf-8",
            )
            output = root / "output"
            manifest = generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertNotIn("ReadFast<uint32_t>", source)
            self.assertIn("goto block_00001004", source)
            self.assertEqual(manifest["functions"][0]["dispatch_entries"], [0x1004])
            self.assertIn("std::array<uint32_t, 2>", source)

    def test_shared_resume_alias_keeps_both_callable_bodies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(0xEF000022, 0xE12FFF1E, 0xEF000022)
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            shared_resume = {
                "id": 1,
                "pc": 0x1004,
                "end_pc": 0x1008,
                "successors": [{"site": 0x1004, "kind": "return"}],
            }
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "svc", "value": 0x22},
                                    {"site": 0x1000, "kind": "resume", "target": 0x1004},
                                ],
                            },
                            shared_resume,
                            {
                                "id": 2,
                                "pc": 0x1008,
                                "end_pc": 0x100C,
                                "successors": [
                                    {"site": 0x1008, "kind": "svc", "value": 0x22},
                                    {"site": 0x1008, "kind": "resume", "target": 0x1004},
                                ],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "FirstCaller",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [],
                            },
                            {
                                "entry": 0x1008,
                                "name": "SecondCaller",
                                "blocks": [2, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [],
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {
                        "format": SELECTION_FORMAT,
                        "functions": [{"entry": 0x1000}, {"entry": 0x1008}],
                    }
                ),
                encoding="utf-8",
            )

            output = root / "output"
            manifest = generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")

            self.assertEqual(len(manifest["functions"]), 2)
            self.assertEqual(
                [item["resume_entries"] for item in manifest["functions"]],
                [[0x1004], [0x1004]],
            )
            self.assertIn("std::array<uint32_t, 3>", source)
            self.assertIn("case 0x00001000U:", source)
            self.assertIn("case 0x00001008U:", source)

    def test_pc_load_is_an_indirect_branch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(root, words(0xE49DF004))
            output = root / "output"
            generate(*inputs, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("ReadFast<uint32_t>", source)
            self.assertIn("r13 = updatedAddress", source)
            self.assertIn("switch (value)", source)
            self.assertIn("goto block_00001000", source)
            self.assertIn("return Oot3dAotBranch(value)", source)

    def test_svc_exits_to_runtime_and_reenters_at_resume(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            code = words(0xEF000022, 0xE12FFF1E)
            code_path = root / "code.bin"
            program_path = root / "program.json"
            selection_path = root / "selection.json"
            code_path.write_bytes(code)
            program_path.write_text(
                json.dumps(
                    {
                        "format": PROGRAM_FORMAT,
                        "base": 0x1000,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "blocks": [
                            {
                                "id": 0,
                                "pc": 0x1000,
                                "end_pc": 0x1004,
                                "successors": [
                                    {"site": 0x1000, "kind": "svc", "value": 0x22},
                                    {"site": 0x1000, "kind": "resume", "target": 0x1004},
                                ],
                            },
                            {
                                "id": 1,
                                "pc": 0x1004,
                                "end_pc": 0x1008,
                                "successors": [{"site": 0x1004, "kind": "return"}],
                            },
                        ],
                        "functions": [
                            {
                                "entry": 0x1000,
                                "name": "SvcCaller",
                                "blocks": [0, 1],
                                "closed_static_cfg": True,
                                "direct_calls": [],
                                "tail_calls": [],
                                "indirect_sites": [],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            selection_path.write_text(
                json.dumps(
                    {"format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}]}
                ),
                encoding="utf-8",
            )
            output = root / "output"
            manifest = generate(program_path, selection_path, code_path, output)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertEqual(manifest["functions"][0]["resume_entries"], [0x1004])
            self.assertIn(
                "Oot3dAotSvc(context, 0x00001000U, 0x000022U)", source
            )
            self.assertIn("case Oot3dWholeAotFlowKind::Svc:", source)
            self.assertIn("a32::ExitKind::Svc, flow.Pc", source)
            self.assertIn("case 0x00001004U:", source)

    def test_rejects_unselected_direct_call_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(
                root,
                words(0xEB000000, 0xE12FFF1E),
                direct_calls=[{"site": 0x1000, "target": 0x1008}],
            )
            with self.assertRaisesRegex(ValueError, "unselected function"):
                generate(*inputs, root / "output")


class AffinityIncrementalShardingTests(unittest.TestCase):
    """Stable distribution: only the touched shards change (ccache hits)."""

    @staticmethod
    def chain(count: int, size: int = 4) -> tuple:
        from whole_aot_cpp import Block, Function

        functions = []
        for index in range(count):
            entry = 0x1000 + index * 0x100
            instructions = tuple(
                Instruction(entry + offset * 4, 0xE1A00000)
                for offset in range(size + index % 3)
            )
            calls = (
                ((entry, entry + 0x100),) if index + 1 < count else ()
            )
            functions.append(
                Function(
                    entry,
                    f"F{index}",
                    (Block(0, entry, entry + len(instructions) * 4,
                           instructions, ()),),
                    calls,
                    (),
                )
            )
        return tuple(functions)

    @staticmethod
    def entry_sets(buckets) -> list:
        return [frozenset(f.entry for f in bucket) for bucket in buckets]

    def test_without_previous_assignment_matches_affinity(self) -> None:
        from whole_aot_cpp import _shard_buckets

        functions = self.chain(40)
        self.assertEqual(
            self.entry_sets(_shard_buckets(functions, 8, "affinity")),
            self.entry_sets(
                _shard_buckets(functions, 8, "affinity-incremental", {})
            ),
        )

    def test_removal_and_addition_touch_only_their_shards(self) -> None:
        from whole_aot_cpp import _shard_buckets

        functions = self.chain(40)
        base = _shard_buckets(functions, 8, "affinity")
        assignment = {
            f.entry: index for index, bucket in enumerate(base) for f in bucket
        }
        before = self.entry_sets(base)
        victim = functions[17]
        reduced = tuple(f for f in functions if f.entry != victim.entry)
        after = self.entry_sets(
            _shard_buckets(reduced, 8, "affinity-incremental", assignment)
        )
        changed = [
            index for index, (old, new) in enumerate(zip(before, after))
            if old - {victim.entry} != new
        ]
        self.assertEqual(changed, [])
        # Re-adding the removed function: it goes back next to a neighbor.
        reduced_assignment = {
            entry: index for entry, index in assignment.items()
            if entry != victim.entry
        }
        readded = _shard_buckets(
            functions, 8, "affinity-incremental", reduced_assignment
        )
        home = next(
            index for index, bucket in enumerate(readded)
            if victim.entry in {f.entry for f in bucket}
        )
        neighbors = {victim.entry - 0x100, victim.entry + 0x100}
        self.assertTrue(
            neighbors & {f.entry for f in readded[home]}
        )
        self.assertEqual(
            sum(
                1 for old, new in zip(before, self.entry_sets(readded))
                if old != new
            ),
            0 if home == assignment[victim.entry] else 2,
        )


if __name__ == "__main__":
    unittest.main()
