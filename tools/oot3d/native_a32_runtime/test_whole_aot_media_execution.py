"""Execute generated ARMv6 media instructions against an independent reference model.

Each instruction word is lowered with the real emitter, compiled with a host C++ compiler into a function over a
mock guest state, and run on random and edge-case operands. The expected results come from a separate model written
from the ARM architecture description (not from the emitted code), so a wrong lowering fails here instead of only
changing the spelling of the generated source.
"""

from __future__ import annotations

import os
import random
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from whole_aot_cpp import Instruction, LoweringError, _emit_non_control

Q_BIT = 1 << 27
EDGES = [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x7FFF7FFF, 0x80008000, 0xFFFFFFFF, 0x00010001,
         0x7F, 0x80, 0xFF, 0x7F7F7F7F, 0x80808080, 0x80000000, 0x7FFFFFFF, 0x12345678, 0xFFFF0000, 0x0000FFFF]


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def s8(x):
    x &= 0xFF
    return x - 0x100 if x & 0x80 else x


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


# ---- reference model (from the ARM architecture description) -------------------------------------------------

def ref_parallel(prefix, op, a, b):
    """prefix: 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH; op: 0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 7 SUB8."""
    signed = prefix in (1, 2, 3)
    width = 8 if op in (4, 7) else 16
    mask = (1 << width) - 1
    count = 32 // width

    def lane(value, index):
        raw = (value >> (index * width)) & mask
        if signed:
            return raw - (1 << width) if raw & (1 << (width - 1)) else raw
        return raw

    if op in (0, 4):
        plan = [(i, i, i, 1) for i in range(count)]
    elif op in (3, 7):
        plan = [(i, i, i, -1) for i in range(count)]
    elif op == 1:   # ASX
        plan = [(0, 0, 1, -1), (1, 1, 0, 1)]
    else:           # SAX
        plan = [(0, 0, 1, 1), (1, 1, 0, -1)]
    result = 0
    for dest, ia, ib, sign in plan:
        total = lane(a, ia) + sign * lane(b, ib)
        if prefix in (1, 5):                       # modulo
            out = total & mask
        elif prefix in (2, 6):                     # saturate
            low, high = (-(1 << (width - 1)), (1 << (width - 1)) - 1) if signed else (0, mask)
            out = max(low, min(high, total)) & mask
        else:                                      # halving: bits [width:1] of the (width+1)-bit result
            out = ((total & ((1 << (width + 1)) - 1)) >> 1) & mask
        result |= out << (dest * width)
    return result


def ref_sat16(signed, field, source):
    result, saturated = 0, False
    for index in range(2):
        v = s16(source >> (16 * index))
        if signed:
            low, high = -(1 << field), (1 << field) - 1      # field = sat_imm, saturate to field+1 bits
        else:
            low, high = 0, (1 << field) - 1
        clamped = max(low, min(high, v))
        saturated |= clamped != v
        result |= (clamped & 0xFFFF) << (16 * index)
    return result, saturated


def ref_extend_add16(unsigned, rotate, rn, rm):
    rotated = ((rm >> rotate) | (rm << (32 - rotate))) & 0xFFFFFFFF if rotate else rm
    ext = (lambda v: v & 0xFF) if unsigned else (lambda v: s8(v) & 0xFFFF)
    low = (rn + ext(rotated)) & 0xFFFF
    high = ((rn >> 16) + ext(rotated >> 16)) & 0xFFFF
    return low | (high << 16)


def ref_dual(subtract, exchange, accumulate, rn, rm, ra):
    p_low = s16(rn) * s16(rm >> 16 if exchange else rm)
    p_high = s16(rn >> 16) * s16(rm if exchange else rm >> 16)
    result = p_low - p_high if subtract else p_low + p_high
    if accumulate:
        result += s32(ra)
    return result & 0xFFFFFFFF, result != s32(result)


# ---- cases -----------------------------------------------------------------------------------------------------

def make_cases():
    """(label, raw, reference) where reference(regs) -> (dest_register, value, q_set)."""
    cases = []
    for prefix in (1, 2, 3, 5, 6, 7):
        for op in (0, 1, 2, 3, 4, 7):
            for rd, rn, rm in ((3, 1, 2), (1, 1, 2), (2, 1, 2), (3, 1, 1)):
                raw = 0xE6000F10 | (prefix << 20) | (rn << 16) | (rd << 12) | (op << 5) | rm
                cases.append((f"parallel p{prefix} op{op} rd{rd}", raw,
                              lambda r, p=prefix, o=op, n=rn, m=rm, d=rd: (d, ref_parallel(p, o, r[n], r[m]), False)))
    for signed in (True, False):
        for field in range(16):
            for rd, rm in ((3, 1), (1, 1)):
                raw = (0xE6A00F30 if signed else 0xE6E00F30) | (field << 16) | (rd << 12) | rm
                cases.append((f"{'ssat16' if signed else 'usat16'} #{field} rd{rd}", raw,
                              lambda r, sg=signed, f=field, m=rm, d=rd: (d, *ref_sat16(sg, f, r[m]))))
    for unsigned in (True, False):
        for rotate in (0, 8, 16, 24):
            for rd, rn, rm in ((3, 1, 2), (1, 1, 2), (2, 1, 2)):
                raw = (0xE6C00070 if unsigned else 0xE6800070) | (rn << 16) | (rd << 12) | ((rotate // 8) << 10) | rm
                cases.append((f"{'uxtab16' if unsigned else 'sxtab16'} ror{rotate} rd{rd}", raw,
                              lambda r, u=unsigned, ro=rotate, n=rn, m=rm, d=rd: (d, ref_extend_add16(u, ro, r[n], r[m]), False)))
    for subtract in (False, True):
        for exchange in (False, True):
            for accumulate in (True, False):
                for rd, ra, rm, rn in ((4, 3, 2, 1), (1, 3, 2, 1), (4, 3, 1, 1)):
                    raw = (0xE7000010 | (int(subtract) << 6) | (int(exchange) << 5) | (rd << 16)
                           | ((ra if accumulate else 15) << 12) | (rm << 8) | rn)
                    name = ("smls" if subtract else "smla") if accumulate else ("smus" if subtract else "smua")
                    cases.append((f"{name}d{'x' if exchange else ''} rd{rd}", raw,
                                  lambda r, s=subtract, e=exchange, a=accumulate, n=rn, m=rm, x=ra, d=rd:
                                  (d, *ref_dual(s, e, a, r[n], r[m], r[x]))))
    return cases


def vectors(rng, count):
    pool = EDGES + [rng.getrandbits(32) for _ in range(20)]
    out = []
    for _ in range(count):
        regs = [rng.choice(pool) if rng.random() < 0.5 else rng.getrandbits(32) for _ in range(16)]
        regs[15] = 0
        out.append(regs)
    return out


SOURCE_HEAD = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct Frame { struct { uint32_t cpsr = 0; } Guest; };
struct Context {};
struct State { uint32_t R[16]{}; struct { bool C() const { return false; } } Flags; };
static inline uint32_t Oot3dAotRor(uint32_t v, unsigned n) { n &= 31U; return n == 0U ? v : (v >> n) | (v << (32U - n)); }
static inline void Oot3dAotSetSaturationFlag(Frame& f, bool s) { if (s) f.Guest.cpsr |= 1U << 27U; }
"""


class MediaExecutionTests(unittest.TestCase):
    def test_new_instructions_match_the_reference_model(self) -> None:
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        rng = random.Random(20261109)
        cases = make_cases()
        functions, table = [], []
        for index, (label, raw, reference) in enumerate(cases):
            body = "\n".join(_emit_non_control(Instruction(0x1000, raw)))
            functions.append(f"static void Run{index}(Frame& frame, Context& context, State& state) {{\n(void)context;\n{body}\n}}")
            for regs in vectors(rng, 60):
                dest, value, q = reference(regs)
                after = list(regs)
                after[dest] = value
                table.append((index, regs, after, bool(q)))
        lines = [SOURCE_HEAD, *functions,
                 "struct Vec { int run; uint32_t in[16]; uint32_t out[16]; int q; };",
                 "static const Vec kVectors[] = {"]
        for index, before, after, q in table:
            lines.append("{%d,{%s},{%s},%d}," % (index, ",".join(f"{v}U" for v in before),
                                                 ",".join(f"{v}U" for v in after), int(q)))
        lines.append("};")
        runners = ",".join(f"Run{i}" for i in range(len(cases)))
        lines.append(f"using Fn = void (*)(Frame&, Context&, State&); static const Fn kRun[] = {{{runners}}};")
        names = ",".join('"%s"' % c[0] for c in cases)
        lines.append(f"static const char* kNames[] = {{{names}}};")
        lines.append(r"""
int main() {
    unsigned bad = 0;
    for (const Vec& v : kVectors) {
        Frame f; Context c; State s;
        for (int i = 0; i < 16; ++i) s.R[i] = v.in[i];
        kRun[v.run](f, c, s);
        bool ok = ((f.Guest.cpsr >> 27U) & 1U) == (unsigned)v.q;
        for (int i = 0; i < 15; ++i) ok = ok && s.R[i] == v.out[i];
        if (!ok && bad++ < 12) {
            std::printf("FAIL %s q=%u want=%d\n", kNames[v.run], (f.Guest.cpsr >> 27U) & 1U, v.q);
            for (int i = 0; i < 6; ++i) std::printf("   r%d in=%08X got=%08X want=%08X\n", i, v.in[i], s.R[i], v.out[i]);
        }
    }
    std::printf("vectors=%zu failures=%u\n", sizeof(kVectors) / sizeof(kVectors[0]), bad);
    return bad == 0 ? 0 : 1;
}
""")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "media.cpp"
            source.write_text("\n".join(lines), encoding="utf-8")
            binary = Path(directory) / "media"
            build = subprocess.run([compiler, "-std=c++20", "-O1", "-w", str(source), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[:3000])
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout[:3000])

    def test_media_space_is_never_lowered_as_a_memory_access(self) -> None:
        # SMLAD r3, r7, r8, r9 used to be emitted as a register-offset STR (silent miscompile).
        body = "\n".join(_emit_non_control(Instruction(0x14A4B8, 0xE7039817)))
        self.assertIn("Oot3dAotSetSaturationFlag", body)
        self.assertNotIn("WriteFast", body)
        self.assertNotIn("ReadFast", body)

    def test_unimplemented_media_forms_are_rejected_not_miscompiled(self) -> None:
        for raw in (0xE780F211,  # USAD8
                    0xE7800211,  # USADA8
                    0xE7F000F0,  # permanently undefined (UDF)
                    0xE7E00250):  # UBFX (ARMv6T2, not in the 3DS ARM11)
            with self.assertRaises(LoweringError, msg=f"{raw:08X}"):
                _emit_non_control(Instruction(0x1000, raw))


if __name__ == "__main__":
    unittest.main()
