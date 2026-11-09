"""BLX <imm> (ARM -> Thumb call) is lowered as a call to an external function whose entry has bit 0 set."""

from __future__ import annotations

import hashlib
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path

from whole_aot_cpp import (
    LoweringError,
    PROGRAM_FORMAT,
    SELECTION_FORMAT,
    SOURCE_NAME,
    generate,
)


def words(*items: int) -> bytes:
    return b"".join(struct.pack("<I", item) for item in items)


class BlxImmediateTests(unittest.TestCase):
    def build(self, root: Path, blx: int, target: int, external: list[int]) -> tuple[Path, Path, Path]:
        code = words(blx, 0xE12FFF1E, 0xE12FFF1E)
        code_path, program_path, selection_path = root / "code.bin", root / "program.json", root / "selection.json"
        code_path.write_bytes(code)
        program_path.write_text(json.dumps({
            "format": PROGRAM_FORMAT, "base": 0x1000, "code_sha256": hashlib.sha256(code).hexdigest(),
            "blocks": [
                {"id": 0, "pc": 0x1000, "end_pc": 0x1004,
                 "successors": [{"site": 0x1000, "kind": "direct_call", "target": target},
                                {"site": 0x1000, "kind": "resume", "target": 0x1004}]},
                {"id": 1, "pc": 0x1004, "end_pc": 0x1008, "successors": [{"site": 0x1004, "kind": "return"}]},
            ],
            "functions": [{"entry": 0x1000, "name": "Caller", "blocks": [0, 1], "closed_static_cfg": True,
                           "direct_calls": [{"site": 0x1000, "target": target}], "tail_calls": [], "indirect_sites": []}],
        }), encoding="utf-8")
        selection_path.write_text(json.dumps({
            "format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}],
            "external_functions": [{"entry": e} for e in external]}), encoding="utf-8")
        return program_path, selection_path, code_path

    def test_even_thumb_target_is_called_as_external_with_bit_zero_set(self) -> None:
        for blx, target in ((0xFA000000, 0x1008), (0xFB000000, 0x100A)):  # H = 0 / H = 1
            with self.subTest(blx=f"{blx:08X}"), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                inputs = self.build(root, blx, target, [target])
                output = root / "out"
                generate(*inputs, output)
                source = (output / SOURCE_NAME).read_text(encoding="utf-8")
                self.assertIn(f"Oot3dAotCallExternal(context, 0x{target | 1:08X}U", source)
                self.assertRegex(source, r"(r14|state\.R\[14\]) = 0x00001004U;")
                self.assertIn("callFlow.Pc != 0x00001004U", source)

    def test_target_not_declared_external_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.build(root, 0xFA000000, 0x1008, [])
            with self.assertRaises((LoweringError, ValueError)):
                generate(*inputs, root / "out")


if __name__ == "__main__":
    unittest.main()
