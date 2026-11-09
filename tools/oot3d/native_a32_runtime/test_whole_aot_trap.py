"""--trap-unsupported: an unlowerable instruction becomes a runtime trap instead of aborting the function."""

from __future__ import annotations

import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from whole_aot_cpp import LoweringError, PROGRAM_FORMAT, SELECTION_FORMAT, SOURCE_NAME, generate


def words(*items: int) -> bytes:
    return b"".join(struct.pack("<I", item) for item in items)


class TrapUnsupportedTests(unittest.TestCase):
    def inputs(self, root: Path, code: bytes, direct_calls: list[dict], selected: list[int], external: list[int]):
        code_path, program_path, selection_path = root / "code.bin", root / "program.json", root / "selection.json"
        code_path.write_bytes(code)
        # block 0: [BL?/data...] up to the data word; block 1: the data word as its own block; block 2: BX LR
        program_path.write_text(json.dumps({
            "format": PROGRAM_FORMAT, "base": 0x1000, "code_sha256": hashlib.sha256(code).hexdigest(),
            "blocks": [
                {"id": 0, "pc": 0x1000, "end_pc": 0x1008,
                 "successors": [{"site": 0x1004, "kind": "fallthrough", "target": 0x1008}]},
                {"id": 1, "pc": 0x1008, "end_pc": 0x100C,
                 "successors": [{"site": 0x1008, "kind": "fallthrough", "target": 0x100C}]},
                {"id": 2, "pc": 0x100C, "end_pc": 0x1010, "successors": [{"site": 0x100C, "kind": "return"}]},
            ],
            "functions": [{"entry": 0x1000, "name": "Func", "blocks": [0, 1, 2], "closed_static_cfg": True,
                           "direct_calls": direct_calls, "tail_calls": [], "indirect_sites": []}],
        }), encoding="utf-8")
        selection_path.write_text(json.dumps({
            "format": SELECTION_FORMAT, "functions": [{"entry": e} for e in selected],
            "external_functions": [{"entry": e} for e in external]}), encoding="utf-8")
        return program_path, selection_path, code_path

    # mov r0,#1 ; mov r1,#2 ; <0x3EFFFFED data word> ; bx lr
    CODE = words(0xE3A00001, 0xE3A01002, 0x3EFFFFED, 0xE12FFF1E)

    def test_without_the_flag_the_function_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(LoweringError):
                generate(*self.inputs(root, self.CODE, [], [0x1000], []), root / "out")

    def test_with_the_flag_the_instruction_becomes_a_trap_and_is_listed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out"
            manifest = generate(*self.inputs(root, self.CODE, [], [0x1000], []), output, trap_unsupported=True)
            source = (output / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("Oot3dWholeAotFlowKind::Unsupported, 0x00001008U, 0x3EFFFFEDU", source)
            self.assertEqual([t["pc"] for t in manifest["trapped_instructions"]], [0x1008])
            self.assertEqual(manifest["trapped_instructions"][0]["raw"], "0x3EFFFFED")
            # the other instructions of the function are still lowered
            self.assertIn("block_0000100C", source)

    def test_call_resolution_errors_are_never_trapped(self) -> None:
        # BL to an unselected, non-external target must keep failing even with the flag.
        code = words(0xEB000003, 0xE3A01002, 0xE3A00001, 0xE12FFF1E)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.inputs(root, code, [{"site": 0x1000, "target": 0x1010}], [0x1000], [])
            with self.assertRaises((LoweringError, ValueError)):
                generate(*inputs, root / "out", trap_unsupported=True)


if __name__ == "__main__":
    unittest.main()
