// Interprete Thumb-1 (ARMv6K, sin Thumb-2) del host para el codigo Thumb de MM3D.
//
// Se usa como implementacion de las funciones `external` que el generador AOT no compila (docs/decisiones.md D-005/D-006):
// la entrada es la direccion de una funcion Thumb; se interpreta hasta que el control pasa a codigo ARM (compilado) y se
// devuelve el tipo de salida y el PC para que el despachador continue.
//
// Plantilla sobre el estado del guest (`r[16]`, `cpsr`) y el bus de memoria (Read8/16/32, Write8/16/32 -> bool) para poder
// probarlo en el host sin el resto del runtime. No depende de nada de OoT3D.
//
// Cubre: desplazamientos, suma/resta, MOV/CMP/ADD/SUB inmediato, las 16 operaciones ALU, registros altos, BX/BLX, LDR relativo
// a PC, carga/almacenamiento (registro, inmediato, media palabra, SP), ADD PC/SP, ADD/SUB SP, PUSH/POP, LDMIA/STMIA,
// ramas condicionales e incondicionales, BL/BLX en par, NOP. Lo demas (SVC, extensiones, REV, CPS, BKPT, instrucciones de
// 32 bits distintas de BL/BLX) termina con `Unsupported`.
#pragma once

#include <cstdint>

namespace mm3d::thumb {

enum class Exit : uint8_t {
    Return,       // salto a codigo ARM como retorno (BX LR / POP {PC} con destino par)
    Branch,       // salto o llamada a codigo ARM (LR ya actualizado si era una llamada)
    MemoryFault,  // pc = instruccion que fallo, detail = direccion
    BlockLimit,   // presupuesto agotado; pc = siguiente instruccion Thumb (bit 0 activo)
    Unsupported,  // pc = instruccion (bit 0 activo), detail = instruccion
};

struct Result {
    Exit kind;
    uint32_t pc;
    uint32_t detail;
};

constexpr uint32_t kN = 1U << 31, kZ = 1U << 30, kC = 1U << 29, kV = 1U << 28;

namespace detail {

constexpr uint32_t Ror(uint32_t v, unsigned n) { n &= 31U; return n == 0U ? v : (v >> n) | (v << (32U - n)); }

struct AddResult { uint32_t value; bool carry; bool overflow; };

constexpr AddResult AddWithCarry(uint32_t x, uint32_t y, bool carry_in) {
    const uint64_t unsigned_sum = static_cast<uint64_t>(x) + y + (carry_in ? 1U : 0U);
    const int64_t signed_sum = static_cast<int64_t>(static_cast<int32_t>(x)) + static_cast<int32_t>(y) + (carry_in ? 1 : 0);
    const uint32_t value = static_cast<uint32_t>(unsigned_sum);
    return {value, unsigned_sum != value, signed_sum != static_cast<int32_t>(value)};
}

constexpr int32_t SignExtend(uint32_t value, unsigned bits) {
    const uint32_t sign = 1U << (bits - 1U);
    return static_cast<int32_t>((value ^ sign) - sign);
}

}  // namespace detail

template <class Guest, class Bus>
Result Run(Guest& g, Bus& bus, uint32_t start, uint32_t budget) {
    using namespace detail;
    uint32_t pc = start & ~1U;
    auto setNZ = [&](uint32_t v) {
        g.cpsr = (g.cpsr & ~(kN | kZ)) | ((v & 0x80000000U) ? kN : 0U) | (v == 0U ? kZ : 0U);
    };
    auto setNZCV = [&](uint32_t v, bool c, bool o) {
        g.cpsr = (g.cpsr & ~(kN | kZ | kC | kV)) | ((v & 0x80000000U) ? kN : 0U) | (v == 0U ? kZ : 0U) |
                 (c ? kC : 0U) | (o ? kV : 0U);
    };
    auto setC = [&](bool c) { g.cpsr = (g.cpsr & ~kC) | (c ? kC : 0U); };
    auto carry = [&] { return (g.cpsr & kC) != 0U; };
    auto condition = [&](unsigned cond) {
        const bool n = (g.cpsr & kN) != 0U, z = (g.cpsr & kZ) != 0U, c = (g.cpsr & kC) != 0U, v = (g.cpsr & kV) != 0U;
        switch (cond) {
            case 0: return z;
            case 1: return !z;
            case 2: return c;
            case 3: return !c;
            case 4: return n;
            case 5: return !n;
            case 6: return v;
            case 7: return !v;
            case 8: return c && !z;
            case 9: return !c || z;
            case 10: return n == v;
            case 11: return n != v;
            case 12: return !z && n == v;
            case 13: return z || n != v;
            default: return true;
        }
    };
    auto fault = [&](uint32_t at, uint32_t address) { return Result{Exit::MemoryFault, at, address}; };

    for (;;) {
        if (budget == 0U) return {Exit::BlockLimit, pc | 1U, 0U};
        --budget;
        uint16_t half;
        if (!bus.Read16(pc, &half)) return fault(pc, pc);
        const uint32_t x = half;
        const uint32_t here = pc;
        uint32_t next = pc + 2U;
        const uint32_t pcv = here + 4U;  // valor de PC leido por una instruccion Thumb

        const unsigned top5 = x >> 11;
        if (top5 <= 2U) {  // LSL/LSR/ASR Rd, Rm, #imm5
            const unsigned rd = x & 7U, rm = (x >> 3) & 7U;
            unsigned imm = (x >> 6) & 31U;
            const uint32_t v = g.r[rm];
            uint32_t out;
            if (top5 == 0U) {
                if (imm == 0U) out = v; else { out = v << imm; setC(((v >> (32U - imm)) & 1U) != 0U); }
            } else if (top5 == 1U) {
                if (imm == 0U) imm = 32U;
                if (imm == 32U) { out = 0U; setC((v >> 31) != 0U); } else { out = v >> imm; setC(((v >> (imm - 1U)) & 1U) != 0U); }
            } else {
                if (imm == 0U) imm = 32U;
                if (imm == 32U) { out = static_cast<uint32_t>(static_cast<int32_t>(v) >> 31); setC((v >> 31) != 0U); }
                else { out = static_cast<uint32_t>(static_cast<int32_t>(v) >> imm); setC(((v >> (imm - 1U)) & 1U) != 0U); }
            }
            g.r[rd] = out; setNZ(out);
        } else if (top5 == 3U) {  // ADD/SUB Rd, Rn, Rm|#imm3
            const unsigned rd = x & 7U, rn = (x >> 3) & 7U, field = (x >> 6) & 7U;
            const bool immediate = (x >> 10) & 1U, subtract = (x >> 9) & 1U;
            const uint32_t operand = immediate ? field : g.r[field];
            const auto r = subtract ? AddWithCarry(g.r[rn], ~operand, true) : AddWithCarry(g.r[rn], operand, false);
            g.r[rd] = r.value; setNZCV(r.value, r.carry, r.overflow);
        } else if ((x >> 13) == 1U) {  // MOV/CMP/ADD/SUB Rd, #imm8
            const unsigned op = (x >> 11) & 3U, rd = (x >> 8) & 7U;
            const uint32_t imm = x & 0xFFU;
            if (op == 0U) { g.r[rd] = imm; setNZ(imm); }
            else if (op == 1U) { const auto r = AddWithCarry(g.r[rd], ~imm, true); setNZCV(r.value, r.carry, r.overflow); }
            else {
                const auto r = op == 2U ? AddWithCarry(g.r[rd], imm, false) : AddWithCarry(g.r[rd], ~imm, true);
                g.r[rd] = r.value; setNZCV(r.value, r.carry, r.overflow);
            }
        } else if ((x >> 10) == 0x10U) {  // 010000: operaciones ALU
            const unsigned op = (x >> 6) & 15U, rd = x & 7U, rm = (x >> 3) & 7U;
            const uint32_t a = g.r[rd], b = g.r[rm];
            uint32_t out = 0U;
            bool write = true;
            auto shiftAmount = [&](unsigned kind) {  // kind: 0 LSL, 1 LSR, 2 ASR, 3 ROR
                const unsigned amount = b & 0xFFU;
                if (amount == 0U) return a;  // el acarreo no cambia
                if (kind == 0U) {
                    if (amount < 32U) { setC(((a >> (32U - amount)) & 1U) != 0U); return a << amount; }
                    setC(amount == 32U && (a & 1U) != 0U); return 0U;
                }
                if (kind == 1U) {
                    if (amount < 32U) { setC(((a >> (amount - 1U)) & 1U) != 0U); return a >> amount; }
                    setC(amount == 32U && (a >> 31) != 0U); return 0U;
                }
                if (kind == 2U) {
                    if (amount < 32U) { setC(((a >> (amount - 1U)) & 1U) != 0U); return static_cast<uint32_t>(static_cast<int32_t>(a) >> amount); }
                    setC((a >> 31) != 0U); return static_cast<uint32_t>(static_cast<int32_t>(a) >> 31);
                }
                const unsigned rot = amount & 31U;
                if (rot == 0U) { setC((a >> 31) != 0U); return a; }
                const uint32_t r = Ror(a, rot); setC((r >> 31) != 0U); return r;
            };
            switch (op) {
                case 0: out = a & b; setNZ(out); break;
                case 1: out = a ^ b; setNZ(out); break;
                case 2: out = shiftAmount(0); setNZ(out); break;
                case 3: out = shiftAmount(1); setNZ(out); break;
                case 4: out = shiftAmount(2); setNZ(out); break;
                case 5: { const auto r = AddWithCarry(a, b, carry()); out = r.value; setNZCV(out, r.carry, r.overflow); break; }
                case 6: { const auto r = AddWithCarry(a, ~b, carry()); out = r.value; setNZCV(out, r.carry, r.overflow); break; }
                case 7: out = shiftAmount(3); setNZ(out); break;
                case 8: out = a & b; setNZ(out); write = false; break;
                case 9: { const auto r = AddWithCarry(~b, 0U, true); out = r.value; setNZCV(out, r.carry, r.overflow); break; }
                case 10: { const auto r = AddWithCarry(a, ~b, true); setNZCV(r.value, r.carry, r.overflow); write = false; break; }
                case 11: { const auto r = AddWithCarry(a, b, false); setNZCV(r.value, r.carry, r.overflow); write = false; break; }
                case 12: out = a | b; setNZ(out); break;
                case 13: out = a * b; setNZ(out); break;
                case 14: out = a & ~b; setNZ(out); break;
                default: out = ~b; setNZ(out); break;
            }
            if (write) g.r[rd] = out;
        } else if ((x >> 10) == 0x11U) {  // 010001: registros altos, BX, BLX
            const unsigned op = (x >> 8) & 3U;
            const unsigned rm = (x >> 3) & 15U;
            const unsigned rd = ((x >> 4) & 8U) | (x & 7U);
            auto reg = [&](unsigned i) { return i == 15U ? pcv : g.r[i]; };
            if (op == 3U) {
                const uint32_t target = reg(rm);
                const bool link = (x >> 7) & 1U;
                if (link) g.r[14] = next | 1U;
                if (target & 1U) { next = target & ~1U; }
                else return {(!link && rm == 14U) ? Exit::Return : Exit::Branch, target & ~3U, 0U};
            } else if (op == 1U) {
                const auto r = AddWithCarry(reg(rd), ~reg(rm), true); setNZCV(r.value, r.carry, r.overflow);
            } else {
                const uint32_t value = op == 0U ? reg(rd) + reg(rm) : reg(rm);
                if (rd == 15U) next = value & ~1U; else g.r[rd] = value;
            }
        } else if (top5 == 9U) {  // LDR Rd, [PC, #imm8*4]
            const unsigned rd = (x >> 8) & 7U;
            const uint32_t address = (pcv & ~3U) + (x & 0xFFU) * 4U;
            uint32_t v;
            if (!bus.Read32(address, &v)) return fault(here, address);
            g.r[rd] = v;
        } else if ((x >> 12) == 5U) {  // carga/almacenamiento con registro
            const unsigned op = (x >> 9) & 7U, rd = x & 7U, rn = (x >> 3) & 7U, rm = (x >> 6) & 7U;
            const uint32_t address = g.r[rn] + g.r[rm];
            bool ok = true;
            switch (op) {
                case 0: ok = bus.Write32(address, g.r[rd]); break;
                case 1: ok = bus.Write16(address, static_cast<uint16_t>(g.r[rd])); break;
                case 2: ok = bus.Write8(address, static_cast<uint8_t>(g.r[rd])); break;
                case 3: { uint8_t v; ok = bus.Read8(address, &v); if (ok) g.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))); break; }
                case 4: { uint32_t v; ok = bus.Read32(address, &v); if (ok) g.r[rd] = v; break; }
                case 5: { uint16_t v; ok = bus.Read16(address, &v); if (ok) g.r[rd] = v; break; }
                case 6: { uint8_t v; ok = bus.Read8(address, &v); if (ok) g.r[rd] = v; break; }
                default: { uint16_t v; ok = bus.Read16(address, &v); if (ok) g.r[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); break; }
            }
            if (!ok) return fault(here, address);
        } else if ((x >> 13) == 3U) {  // carga/almacenamiento con inmediato (palabra / byte)
            const unsigned rd = x & 7U, rn = (x >> 3) & 7U, imm = (x >> 6) & 31U;
            const bool byte = (x >> 12) & 1U, load = (x >> 11) & 1U;
            const uint32_t address = g.r[rn] + (byte ? imm : imm * 4U);
            bool ok;
            if (load) {
                if (byte) { uint8_t v; ok = bus.Read8(address, &v); if (ok) g.r[rd] = v; }
                else { uint32_t v; ok = bus.Read32(address, &v); if (ok) g.r[rd] = v; }
            } else {
                ok = byte ? bus.Write8(address, static_cast<uint8_t>(g.r[rd])) : bus.Write32(address, g.r[rd]);
            }
            if (!ok) return fault(here, address);
        } else if ((x >> 12) == 8U) {  // LDRH/STRH con inmediato
            const unsigned rd = x & 7U, rn = (x >> 3) & 7U, imm = (x >> 6) & 31U;
            const uint32_t address = g.r[rn] + imm * 2U;
            bool ok;
            if ((x >> 11) & 1U) { uint16_t v; ok = bus.Read16(address, &v); if (ok) g.r[rd] = v; }
            else ok = bus.Write16(address, static_cast<uint16_t>(g.r[rd]));
            if (!ok) return fault(here, address);
        } else if ((x >> 12) == 9U) {  // LDR/STR Rd, [SP, #imm8*4]
            const unsigned rd = (x >> 8) & 7U;
            const uint32_t address = g.r[13] + (x & 0xFFU) * 4U;
            bool ok;
            if ((x >> 11) & 1U) { uint32_t v; ok = bus.Read32(address, &v); if (ok) g.r[rd] = v; }
            else ok = bus.Write32(address, g.r[rd]);
            if (!ok) return fault(here, address);
        } else if ((x >> 12) == 10U) {  // ADD Rd, PC|SP, #imm8*4
            const unsigned rd = (x >> 8) & 7U;
            g.r[rd] = (((x >> 11) & 1U) ? g.r[13] : (pcv & ~3U)) + (x & 0xFFU) * 4U;
        } else if ((x >> 8) == 0xB0U) {  // ADD/SUB SP, #imm7*4
            const uint32_t imm = (x & 0x7FU) * 4U;
            g.r[13] = (x & 0x80U) ? g.r[13] - imm : g.r[13] + imm;
        } else if ((x >> 12) == 11U && ((x >> 9) & 3U) == 2U) {  // PUSH/POP
            const bool pop = (x >> 11) & 1U, extra = (x >> 8) & 1U;
            const uint32_t list = x & 0xFFU;
            unsigned count = 0U;
            for (unsigned i = 0; i < 8U; ++i) count += (list >> i) & 1U;
            count += extra ? 1U : 0U;
            if (!pop) {
                const uint32_t base = g.r[13] - 4U * count;
                uint32_t address = base;
                for (unsigned i = 0; i < 8U; ++i) {
                    if (!((list >> i) & 1U)) continue;
                    if (!bus.Write32(address, g.r[i])) return fault(here, address);
                    address += 4U;
                }
                if (extra && !bus.Write32(address, g.r[14])) return fault(here, address);
                g.r[13] = base;
            } else {
                uint32_t address = g.r[13];
                uint32_t loaded[8]{};
                for (unsigned i = 0; i < 8U; ++i) {
                    if (!((list >> i) & 1U)) continue;
                    if (!bus.Read32(address, &loaded[i])) return fault(here, address);
                    address += 4U;
                }
                uint32_t target = 0U;
                if (extra && !bus.Read32(address, &target)) return fault(here, address);
                for (unsigned i = 0; i < 8U; ++i) if ((list >> i) & 1U) g.r[i] = loaded[i];
                g.r[13] += 4U * count;
                if (extra) {
                    if (target & 1U) next = target & ~1U;
                    else return {Exit::Return, target & ~3U, 0U};
                }
            }
        } else if ((x >> 12) == 12U) {  // STMIA/LDMIA Rn!, {rlist}
            const bool load = (x >> 11) & 1U;
            const unsigned rn = (x >> 8) & 7U;
            const uint32_t list = x & 0xFFU;
            uint32_t address = g.r[rn] & ~3U;
            unsigned count = 0U;
            for (unsigned i = 0; i < 8U; ++i) {
                if (!((list >> i) & 1U)) continue;
                ++count;
                if (load) { uint32_t v; if (!bus.Read32(address, &v)) return fault(here, address); g.r[i] = v; }
                else if (!bus.Write32(address, g.r[i])) return fault(here, address);
                address += 4U;
            }
            if (!(load && ((list >> rn) & 1U))) g.r[rn] = (g.r[rn] & ~3U) + 4U * count;
        } else if ((x >> 12) == 13U) {  // rama condicional (cond 0xE indefinida, 0xF SVC)
            const unsigned cond = (x >> 8) & 15U;
            if (cond >= 14U) return {Exit::Unsupported, here | 1U, x};
            if (condition(cond)) next = pcv + static_cast<uint32_t>(SignExtend(x & 0xFFU, 8U) * 2);
        } else if (top5 == 28U) {  // B
            next = pcv + static_cast<uint32_t>(SignExtend(x & 0x7FFU, 11U) * 2);
        } else if (top5 == 30U) {  // BL / BLX en par: primera mitad
            uint16_t second;
            if (!bus.Read16(here + 2U, &second)) return fault(here, here + 2U);
            const uint32_t after = here + 4U;
            if ((second >> 11) == 31U) {  // BL
                g.r[14] = after | 1U;
                next = pcv + static_cast<uint32_t>(SignExtend(x & 0x7FFU, 11U) * 4096) + (static_cast<uint32_t>(second & 0x7FFU) << 1);
            } else if ((second >> 11) == 29U && !(second & 1U)) {  // BLX (a ARM)
                g.r[14] = after | 1U;
                const uint32_t target = (pcv + static_cast<uint32_t>(SignExtend(x & 0x7FFU, 11U) * 4096) + (static_cast<uint32_t>(second & 0x7FFU) << 1)) & ~3U;
                return {Exit::Branch, target, 0U};
            } else {
                return {Exit::Unsupported, here | 1U, x};
            }
            next = next & ~1U;
            pc = next;
            continue;
        } else if (x == 0xBF00U) {  // NOP
        } else {
            return {Exit::Unsupported, here | 1U, x};
        }
        pc = next;
    }
}

}  // namespace mm3d::thumb
