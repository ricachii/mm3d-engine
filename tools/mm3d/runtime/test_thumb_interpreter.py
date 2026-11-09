"""Prueba del interprete Thumb-1 (mm3d_thumb_interpreter.h) contra un modelo de referencia independiente.

El modelo esta escrito aparte (tablas de decodificacion explicitas, banderas con enteros sin acotar) y se ejecuta sobre los mismos
vectores aleatorios que el C++ compilado. Cada vector es UNA instruccion (o un par BL/BLX) con presupuesto 1.
"""

from __future__ import annotations

import os
import random
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

M = 0xFFFFFFFF
CODE = 0x1000
WINDOW = 0x2000
SIZE = 0x4000
N, Z, C, V = 1 << 31, 1 << 30, 1 << 29, 1 << 28


def s32(x): x &= M; return x - (1 << 32) if x & (1 << 31) else x
def s8(x): x &= 0xFF; return x - 256 if x & 128 else x
def s16(x): x &= 0xFFFF; return x - 65536 if x & 32768 else x
def sext(x, bits): x &= (1 << bits) - 1; return x - (1 << bits) if x & (1 << (bits - 1)) else x


class Mem:
    def __init__(self, data): self.b = bytearray(data)
    def ok(self, a, n): return 0 <= a and a + n <= len(self.b)
    def read(self, a, n): return int.from_bytes(self.b[a:a + n], "little") if self.ok(a, n) else None
    def write(self, a, n, v):
        if not self.ok(a, n): return False
        self.b[a:a + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little"); return True


def add(x, y, c):
    u = (x & M) + (y & M) + c
    res = u & M
    return res, u > M, s32(x) + s32(y) + c != s32(res)


def flags(cpsr, res, c=None, v=None):
    out = cpsr & ~(N | Z)
    if res & (1 << 31): out |= N
    if res == 0: out |= Z
    if c is not None: out = (out & ~C) | (C if c else 0)
    if v is not None: out = (out & ~V) | (V if v else 0)
    return out


def cond_ok(cond, f):
    n, z, c, v = bool(f & N), bool(f & Z), bool(f & C), bool(f & V)
    return [z, not z, c, not c, n, not n, v, not v, c and not z, (not c) or z, n == v, n != v, (not z) and n == v, z or n != v][cond]


def ref_run(regs, cpsr, mem, x, second):
    """Una instruccion en CODE. Devuelve (kind, pc, detail, regs, cpsr)."""
    r = list(regs)
    pcv = CODE + 4
    nxt = CODE + 2

    def finish(kind="limit", pc=None, detail=0):
        return (kind, (nxt | 1) if pc is None else pc, detail, r, cpsr)

    def fault(addr): return ("fault", CODE, addr, regs, cpsr)

    t = x >> 11
    if t <= 2:  # desplazamientos con inmediato
        rd, rm, imm = x & 7, (x >> 3) & 7, (x >> 6) & 31
        v = r[rm]
        if t == 0:
            res = (v << imm) & M if imm else v
            if imm: cpsr = (cpsr & ~C) | (C if (v >> (32 - imm)) & 1 else 0)
        elif t == 1:
            n = imm or 32
            res = v >> n if n < 32 else 0
            cpsr = (cpsr & ~C) | (C if (v >> (n - 1)) & 1 else 0)
        else:
            n = imm or 32
            res = (s32(v) >> min(n, 31)) & M if n < 32 else (M if v >> 31 else 0)
            cpsr = (cpsr & ~C) | (C if (v >> (n - 1)) & 1 else 0) if n < 32 else (cpsr & ~C) | (C if v >> 31 else 0)
        r[rd] = res; cpsr = flags(cpsr, res)
    elif t == 3:  # ADD/SUB
        rd, rn, f = x & 7, (x >> 3) & 7, (x >> 6) & 7
        imm, sub = (x >> 10) & 1, (x >> 9) & 1
        b = f if imm else r[f]
        res, c, v = add(r[rn], (~b) & M if sub else b, 1 if sub else 0)
        r[rd] = res; cpsr = flags(cpsr, res, c, v)
    elif x >> 13 == 1:  # imm8
        op, rd, imm = (x >> 11) & 3, (x >> 8) & 7, x & 255
        if op == 0: r[rd] = imm; cpsr = flags(cpsr, imm)
        elif op == 1: res, c, v = add(r[rd], (~imm) & M, 1); cpsr = flags(cpsr, res, c, v)
        else:
            res, c, v = add(r[rd], imm, 0) if op == 2 else add(r[rd], (~imm) & M, 1)
            r[rd] = res; cpsr = flags(cpsr, res, c, v)
    elif x >> 10 == 0b010000:  # ALU
        op, rd, rm = (x >> 6) & 15, x & 7, (x >> 3) & 7
        a, b = r[rd], r[rm]
        res, write = 0, True
        cf = bool(cpsr & C)
        def shift(kind):
            nonlocal cpsr
            n = b & 255
            if n == 0: return a
            if kind == 0:
                if n < 32: cpsr = (cpsr & ~C) | (C if (a >> (32 - n)) & 1 else 0); return (a << n) & M
                cpsr = (cpsr & ~C) | (C if n == 32 and a & 1 else 0); return 0
            if kind == 1:
                if n < 32: cpsr = (cpsr & ~C) | (C if (a >> (n - 1)) & 1 else 0); return a >> n
                cpsr = (cpsr & ~C) | (C if n == 32 and a >> 31 else 0); return 0
            if kind == 2:
                if n < 32: cpsr = (cpsr & ~C) | (C if (a >> (n - 1)) & 1 else 0); return (s32(a) >> n) & M
                cpsr = (cpsr & ~C) | (C if a >> 31 else 0); return M if a >> 31 else 0
            k = n % 32
            if k == 0: cpsr = (cpsr & ~C) | (C if a >> 31 else 0); return a
            res_ = ((a >> k) | (a << (32 - k))) & M
            cpsr = (cpsr & ~C) | (C if res_ >> 31 else 0); return res_
        if op == 0: res = a & b; cpsr = flags(cpsr, res)
        elif op == 1: res = a ^ b; cpsr = flags(cpsr, res)
        elif op in (2, 3, 4, 7):
            res = shift({2: 0, 3: 1, 4: 2, 7: 3}[op]); cpsr = flags(cpsr, res)
        elif op == 5: res, c, v = add(a, b, 1 if cf else 0); cpsr = flags(cpsr, res, c, v)
        elif op == 6: res, c, v = add(a, (~b) & M, 1 if cf else 0); cpsr = flags(cpsr, res, c, v)
        elif op == 8: res = a & b; cpsr = flags(cpsr, res); write = False
        elif op == 9: res, c, v = add((~b) & M, 0, 1); cpsr = flags(cpsr, res, c, v)
        elif op == 10: t_, c, v = add(a, (~b) & M, 1); cpsr = flags(cpsr, t_, c, v); write = False
        elif op == 11: t_, c, v = add(a, b, 0); cpsr = flags(cpsr, t_, c, v); write = False
        elif op == 12: res = a | b; cpsr = flags(cpsr, res)
        elif op == 13: res = (a * b) & M; cpsr = flags(cpsr, res)
        elif op == 14: res = a & (~b & M); cpsr = flags(cpsr, res)
        else: res = (~b) & M; cpsr = flags(cpsr, res)
        if write: r[rd] = res
    elif x >> 10 == 0b010001:  # registros altos
        op, rm, rd = (x >> 8) & 3, (x >> 3) & 15, ((x >> 4) & 8) | (x & 7)
        get = lambda i: pcv if i == 15 else r[i]
        if op == 3:
            tgt = get(rm)
            link = (x >> 7) & 1
            if link: r[14] = nxt | 1
            if tgt & 1: nxt = tgt & ~1
            else: return ("return" if (not link and rm == 14) else "branch", tgt & ~3, 0, r, cpsr)
        elif op == 1: res, c, v = add(get(rd), (~get(rm)) & M, 1); cpsr = flags(cpsr, res, c, v)
        else:
            val = (get(rd) + get(rm)) & M if op == 0 else get(rm)
            if rd == 15: nxt = val & ~1
            else: r[rd] = val
    elif t == 9:  # LDR PC
        rd = (x >> 8) & 7; a = ((pcv & ~3) + (x & 255) * 4)
        v = mem.read(a, 4)
        if v is None: return fault(a)
        r[rd] = v
    elif x >> 12 == 5:  # reg offset
        op, rd, rn, rm = (x >> 9) & 7, x & 7, (x >> 3) & 7, (x >> 6) & 7
        a = (r[rn] + r[rm]) & M
        sizes = {0: 4, 1: 2, 2: 1, 3: 1, 4: 4, 5: 2, 6: 1, 7: 2}
        if op <= 2:
            if not mem.write(a, sizes[op], r[rd]): return fault(a)
        else:
            v = mem.read(a, sizes[op])
            if v is None: return fault(a)
            r[rd] = {3: s8(v) & M, 4: v, 5: v, 6: v, 7: s16(v) & M}[op]
    elif x >> 13 == 3:  # imm offset
        rd, rn, imm = x & 7, (x >> 3) & 7, (x >> 6) & 31
        byte, load = (x >> 12) & 1, (x >> 11) & 1
        a = (r[rn] + (imm if byte else imm * 4)) & M
        n = 1 if byte else 4
        if load:
            v = mem.read(a, n)
            if v is None: return fault(a)
            r[rd] = v
        elif not mem.write(a, n, r[rd]): return fault(a)
    elif x >> 12 == 8:  # halfword imm
        rd, rn, imm = x & 7, (x >> 3) & 7, (x >> 6) & 31
        a = (r[rn] + imm * 2) & M
        if (x >> 11) & 1:
            v = mem.read(a, 2)
            if v is None: return fault(a)
            r[rd] = v
        elif not mem.write(a, 2, r[rd]): return fault(a)
    elif x >> 12 == 9:  # SP relative
        rd = (x >> 8) & 7; a = (r[13] + (x & 255) * 4) & M
        if (x >> 11) & 1:
            v = mem.read(a, 4)
            if v is None: return fault(a)
            r[rd] = v
        elif not mem.write(a, 4, r[rd]): return fault(a)
    elif x >> 12 == 10:  # ADD Rd, PC/SP
        rd = (x >> 8) & 7
        r[rd] = (((r[13]) if (x >> 11) & 1 else (pcv & ~3)) + (x & 255) * 4) & M
    elif x >> 8 == 0xB0:  # ADD/SP
        imm = (x & 127) * 4
        r[13] = (r[13] - imm) & M if x & 128 else (r[13] + imm) & M
    elif x >> 12 == 11 and (x >> 9) & 3 == 2:  # PUSH/POP
        pop, extra, lst = (x >> 11) & 1, (x >> 8) & 1, x & 255
        regs_ = [i for i in range(8) if lst >> i & 1]
        n = len(regs_) + extra
        if not pop:
            base = (r[13] - 4 * n) & M
            a = base
            for i in regs_ + ([14] if extra else []):
                if not mem.write(a, 4, r[i]): return fault(a)
                a += 4
            r[13] = base
        else:
            a = r[13]; vals = {}
            for i in regs_ + ([15] if extra else []):
                v = mem.read(a, 4)
                if v is None: return fault(a)
                vals[i] = v; a += 4
            for i in regs_: r[i] = vals[i]
            r[13] = (r[13] + 4 * n) & M
            if extra:
                tgt = vals[15]
                if tgt & 1: nxt = tgt & ~1
                else: return ("return", tgt & ~3, 0, r, cpsr)
    elif x >> 12 == 12:  # LDMIA/STMIA
        load, rn, lst = (x >> 11) & 1, (x >> 8) & 7, x & 255
        a = r[rn] & ~3; n = 0
        for i in range(8):
            if not lst >> i & 1: continue
            n += 1
            if load:
                v = mem.read(a, 4)
                if v is None: return fault(a)
                r[i] = v
            elif not mem.write(a, 4, r[i]): return fault(a)
            a += 4
        if not (load and lst >> rn & 1): r[rn] = ((regs[rn] & ~3) + 4 * n) & M if False else ((r[rn] & ~3) + 4 * n) & M
    elif x >> 12 == 13:  # Bcond / SVC
        cond = (x >> 8) & 15
        if cond >= 14: return ("unsupported", CODE | 1, x, regs, cpsr)
        if cond_ok(cond, cpsr): nxt = (pcv + sext(x & 255, 8) * 2) & M
    elif t == 28:
        nxt = (pcv + sext(x & 0x7FF, 11) * 2) & M
    elif t == 30:
        s = second
        lr = (pcv + sext(x & 0x7FF, 11) * 4096) & M
        tgt = (lr + ((s & 0x7FF) << 1)) & M
        r[14] = (CODE + 4) | 1
        if s >> 11 == 31: nxt = tgt
        elif s >> 11 == 29 and not s & 1: return ("branch", tgt & ~3, 0, r, cpsr)
        else: return ("unsupported", CODE | 1, x, regs, cpsr)
    elif x == 0xBF00:
        pass
    else:
        return ("unsupported", CODE | 1, x, regs, cpsr)
    return ("limit", nxt | 1, 0, r, cpsr)


# ---- generacion de vectores -------------------------------------------------------------------------------------------

def rnd_reg(rng):
    p = rng.random()
    if p < 0.35: return WINDOW + rng.randrange(0, 0x800)
    if p < 0.55: return rng.choice([0, 1, 2, 31, 32, 33, 255, 0x7FFFFFFF, 0x80000000, M, 0xFFFF, 0x8000, 0x100])
    if p < 0.7: return rng.choice([0x80000000, 0xFFFFFFFF, 0x80000001, 0x7FFFFFFF, 1])
    return rng.getrandbits(32)


def instructions(rng, count):
    out = []
    def add_(x, second=0): out.append((x, second))
    for _ in range(count):
        # una por familia
        for fam in range(26):
            rd, rn, rm = rng.randrange(8), rng.randrange(8), rng.randrange(8)
            if fam == 0: add_((rng.randrange(3) << 11) | (rng.choice([0, 0, 1, 31, 2, rng.randrange(32)]) << 6) | (rm << 3) | rd)
            elif fam == 1: add_(0x1800 | (rng.randrange(8) << 6) | (rn << 3) | rd | (rng.randrange(4) << 9))
            elif fam == 2: add_(0x2000 | (rng.randrange(4) << 11) | (rd << 8) | rng.randrange(256))
            elif fam == 3: add_(0x4000 | (rng.randrange(16) << 6) | (rm << 3) | rd)
            elif fam == 4:
                op = rng.randrange(4); hd = ((rng.randrange(16)) & 15); hm = rng.randrange(16)
                add_(0x4400 | (op << 8) | ((hd >> 3) << 7) | (hm << 3) | (hd & 7) if op != 3 else 0x4700 | (rng.randrange(2) << 7) | (rng.choice([14, 0, 1, 2, 3, 7, 8, 12]) << 3))
            elif fam == 5: add_(0x4800 | (rd << 8) | rng.randrange(256))
            elif fam == 6: add_(0x5000 | (rng.randrange(8) << 9) | (rm << 6) | (rn << 3) | rd)
            elif fam == 7: add_(0x6000 | (rng.randrange(4) << 11) | (rng.randrange(32) << 6) | (rn << 3) | rd)
            elif fam == 8: add_(0x8000 | (rng.randrange(2) << 11) | (rng.randrange(32) << 6) | (rn << 3) | rd)
            elif fam == 9: add_(0x9000 | (rng.randrange(2) << 11) | (rd << 8) | rng.randrange(256))
            elif fam == 10: add_(0xA000 | (rng.randrange(2) << 11) | (rd << 8) | rng.randrange(256))
            elif fam == 11: add_(0xB000 | (rng.randrange(2) << 7) | rng.randrange(128))
            elif fam == 12: add_(0xB400 | (rng.randrange(2) << 11) | (rng.randrange(2) << 8) | rng.randrange(256))
            elif fam == 13: add_(0xC000 | (rng.randrange(2) << 11) | (rd << 8) | rng.randrange(1, 256))
            elif fam == 14: add_(0xD000 | (rng.randrange(14) << 8) | rng.randrange(256))
            elif fam == 15: add_(0xE000 | rng.randrange(0x800))
            elif fam == 16: add_(0xF000 | rng.randrange(0x800), 0xF800 | rng.randrange(0x800))
            elif fam == 17: add_(0xF000 | rng.randrange(0x800), 0xE800 | (rng.randrange(0x400) << 1))
            elif fam == 18: add_(0xBF00)
            elif fam == 19: add_(0xDF00 | rng.randrange(256))   # SVC -> no soportado
            elif fam == 20: add_(0xB200 | rng.randrange(256))   # extensiones -> no soportado
            else: add_(rng.choice([0x4600, 0x46C0, 0x4770, 0x4670]))
    return out


def build_cases(rng, count):
    cases = []
    for x, second in instructions(rng, count):
        regs = [rnd_reg(rng) for _ in range(16)]
        regs[15] = 0
        if rng.random() < 0.7: regs[13] = WINDOW + 0x400 + (rng.randrange(0, 0x100) & ~3)
        data = bytes(rng.getrandbits(8) for _ in range(SIZE))
        cases.append((x, second, regs, rng.getrandbits(32) & (N | Z | C | V), data))
    return cases


SOURCE = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "mm3d_thumb_interpreter.h"
struct Guest { std::array<uint32_t, 16> r{}; uint32_t cpsr = 0; };
struct Bus {
    std::array<uint8_t, %(size)d> m{};
    template <class T> bool rd(uint32_t a, T* v) { if (a > m.size() || sizeof(T) > m.size() - a) return false; std::memcpy(v, &m[a], sizeof(T)); return true; }
    template <class T> bool wr(uint32_t a, T v) { if (a > m.size() || sizeof(T) > m.size() - a) return false; std::memcpy(&m[a], &v, sizeof(T)); return true; }
    bool Read8(uint32_t a, uint8_t* v) { return rd(a, v); } bool Read16(uint32_t a, uint16_t* v) { return rd(a, v); } bool Read32(uint32_t a, uint32_t* v) { return rd(a, v); }
    bool Write8(uint32_t a, uint8_t v) { return wr(a, v); } bool Write16(uint32_t a, uint16_t v) { return wr(a, v); } bool Write32(uint32_t a, uint32_t v) { return wr(a, v); }
};
int main(int argc, char** argv) {
    FILE* f = std::fopen(argv[1], "rb"); unsigned bad = 0, total = 0;
    uint32_t head[4];
    while (std::fread(head, 4, 4, f) == 4) {
        Guest g; Bus b; uint32_t regs[16], cpsr;
        std::fread(regs, 4, 16, f); std::fread(&cpsr, 4, 1, f); std::fread(b.m.data(), 1, b.m.size(), f);
        uint32_t want_regs[16], want_cpsr, want[3]; std::fread(want, 4, 3, f); std::fread(want_regs, 4, 16, f); std::fread(&want_cpsr, 4, 1, f);
        std::vector<uint8_t> want_mem(b.m.size()); std::fread(want_mem.data(), 1, want_mem.size(), f);
        for (int i = 0; i < 16; ++i) g.r[i] = regs[i]; g.cpsr = cpsr;
        uint16_t h1 = (uint16_t)head[0], h2 = (uint16_t)head[1];
        std::memcpy(&b.m[0x1000], &h1, 2); std::memcpy(&b.m[0x1002], &h2, 2);
        want_mem[0x1000] = (uint8_t)(h1 & 0xFF); want_mem[0x1001] = (uint8_t)(h1 >> 8); want_mem[0x1002] = (uint8_t)(h2 & 0xFF); want_mem[0x1003] = (uint8_t)(h2 >> 8);
        auto res = mm3d::thumb::Run(g, b, 0x1000, 1);
        ++total;
        bool ok = (uint32_t)res.kind == want[0] && res.pc == want[1] && res.detail == want[2] && g.cpsr == want_cpsr;
        for (int i = 0; i < 15; ++i) ok = ok && g.r[i] == want_regs[i];
        ok = ok && std::memcmp(b.m.data(), want_mem.data(), b.m.size()) == 0;
        if (!ok && bad++ < 10) {
            std::printf("FAIL instr=%%04X/%%04X kind=%%u/%%u pc=%%08X/%%08X detail=%%08X/%%08X cpsr=%%08X/%%08X\n", head[0], head[1], (uint32_t)res.kind, want[0], res.pc, want[1], res.detail, want[2], g.cpsr, want_cpsr);
            for (int i = 0; i < 15; ++i) if (g.r[i] != want_regs[i]) std::printf("   r%%d got=%%08X want=%%08X (in=%%08X)\n", i, g.r[i], want_regs[i], regs[i]);
        }
    }
    std::printf("vectors=%%u failures=%%u\n", total, bad);
    return bad == 0 ? 0 : 1;
}
""" % {"size": SIZE}

KIND = {"return": 0, "branch": 1, "fault": 2, "limit": 3, "unsupported": 4}


class ThumbInterpreterTests(unittest.TestCase):
    def test_matches_reference(self) -> None:
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("hace falta un compilador C++")
        rng = random.Random(20261109)
        cases = build_cases(rng, 250)
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            (d / "t.cpp").write_text("#include <vector>\n" + SOURCE)
            blob = bytearray()
            for x, second, regs, cpsr, data in cases:
                mem = Mem(data)
                mem.b[0x1000:0x1002] = x.to_bytes(2, "little"); mem.b[0x1002:0x1004] = second.to_bytes(2, "little")
                kind, pc, detail, out_regs, out_cpsr = ref_run(regs, cpsr, mem, x, second)
                blob += b"".join(v.to_bytes(4, "little") for v in (x, second, 0, 0))
                blob += b"".join(v.to_bytes(4, "little") for v in regs) + cpsr.to_bytes(4, "little") + data
                blob += b"".join(v.to_bytes(4, "little") for v in (KIND[kind], pc, detail))
                blob += b"".join(v.to_bytes(4, "little") for v in out_regs) + out_cpsr.to_bytes(4, "little") + bytes(mem.b)
            (d / "v.bin").write_bytes(bytes(blob))
            build = subprocess.run([compiler, "-std=c++20", "-O1", "-w", "-I", str(Path(__file__).parent), str(d / "t.cpp"), "-o", str(d / "t")],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[:3000])
            run = subprocess.run([str(d / "t"), str(d / "v.bin")], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout[:3500])


if __name__ == "__main__":
    unittest.main()
