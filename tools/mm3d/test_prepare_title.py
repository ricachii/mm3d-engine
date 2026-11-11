"""prepare_title.py: llamadas a ayudantes 'salta-palabra' (bl + dato en linea) y su reparacion."""

from __future__ import annotations

import struct
import unittest

import prepare_title as pt


def words(*items: int) -> bytes:
    return b"".join(struct.pack("<I", item) for item in items)


def bl(site: int, target: int) -> int:
    return 0xEB000000 | (((target - site - 8) >> 2) & 0x00FFFFFF)


BASE = 0x1000
HELPER = BASE + 0x20


def image() -> bytes:
    code = [0xE1A00000] * 16  # nop
    code[0] = 0xE92D4010                 # 0x1000 push {r4, lr}
    code[1] = bl(BASE + 4, HELPER)       # 0x1004 bl helper (salta-palabra)
    code[2] = 0x806DB6DB                 # 0x1008 dato en linea (no decodifica)
    code[3] = 0xE3A00000                 # 0x100C mov r0, #0
    code[4] = bl(BASE + 0x10, BASE + 0x30)  # 0x1010 bl a una funcion normal
    code[5] = 0xE12FFF1E                 # 0x1014 bx lr
    code[8] = 0xE49E4004                 # 0x1020 ldr r4, [lr], #4   <- el ayudante
    code[9] = 0xE12FFF1E
    code[12] = 0xE12FFF1E                # 0x1030 funcion normal
    return words(*code)


class SkipWordTests(unittest.TestCase):
    def test_finds_only_calls_to_the_helper(self) -> None:
        self.assertEqual(pt.skip_word_call_sites(image(), BASE), [BASE + 4])

    def test_conditional_call_is_not_a_skip_word_site(self) -> None:
        code = bytearray(image())
        struct.pack_into("<I", code, 4, (bl(BASE + 4, HELPER) & 0x0FFFFFFF) | 0x10000000)  # blne
        self.assertEqual(pt.skip_word_call_sites(bytes(code), BASE), [])

    def program(self, edge_site: int, seeded: bool, kind: str = "unknown") -> dict:
        blocks = [{"pc": BASE}] + ([{"pc": BASE + 0xC}] if seeded else [])
        function = {"entry": BASE, "closed_static_cfg": False, "direct_calls": [],
                    "unresolved_static_edges": [{"kind": kind, "site": edge_site, "condition": "hi"}]}
        return {"blocks": blocks, "functions": [function]}

    def test_repair_closes_function_open_only_by_the_inline_word(self) -> None:
        program = self.program(BASE + 8, seeded=True)
        self.assertEqual(pt.repair_program(program, image(), BASE), 1)
        function = program["functions"][0]
        self.assertTrue(function["closed_static_cfg"])
        self.assertEqual(function["repaired"], "inline-data-after-skip-word-call")

    def test_repair_leaves_other_unknown_edges_open(self) -> None:
        # una arista desconocida que NO es el dato tras un bl salta-palabra
        self.assertEqual(pt.repair_program(self.program(BASE + 0x14, seeded=True), image(), BASE), 0)
        # el codigo tras el dato no esta sembrado: no se puede cerrar
        self.assertEqual(pt.repair_program(self.program(BASE + 8, seeded=False), image(), BASE), 0)
        # tipo de arista distinto
        self.assertEqual(pt.repair_program(self.program(BASE + 8, seeded=True, kind="indirect"), image(), BASE), 0)


class FlowTests(unittest.TestCase):
    """El recorrido de flujo siembra `site + 8` solo con el interruptor activo (por defecto no cambia nada)."""

    def reachable(self, enabled: bool) -> set[int]:
        from oot3d_pack import a32_cpp_aot as flow_module  # el sys.path lo prepara prepare_title

        code = image()
        slots = set(range(BASE, BASE + len(code), 4))
        previous = flow_module.SKIP_WORD_CALLS
        flow_module.SKIP_WORD_CALLS = enabled
        try:
            decoder = flow_module._Decoder(code, BASE)
            flow, _, _, _ = flow_module._address_aware_flow(decoder, slots, [BASE], slots)
        finally:
            flow_module.SKIP_WORD_CALLS = previous
        return set(flow.decoded_reachable)

    def test_code_after_the_inline_word_is_reached_only_when_enabled(self) -> None:
        self.assertNotIn(BASE + 0xC, self.reachable(False))
        self.assertIn(BASE + 0xC, self.reachable(True))


if __name__ == "__main__":
    unittest.main()
