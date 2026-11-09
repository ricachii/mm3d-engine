#!/usr/bin/env python3
"""Medición L3: transferencia de identidad entre dos code.bin de 3DS por firma de función (estilo FLIRT).

Para cada función (entrada+tamaño, de 3dsrecomp) se calcula un hash de sus instrucciones ARM **enmascarando** lo que cambia
al reubicar el código: el desplazamiento de B/BL/BLX, los LDR/VLDR relativos al PC, los ADR y el contenido de los literal
pools. Luego se empareja por hash exacto (únicos en ambos lados) y se propaga por el grafo de llamadas.

Uso:
  python3 -I l3_signatures.py A.code.bin A.funcs.csv B.code.bin B.funcs.csv [--names inventario.csv] [--out matches.csv] [--selftest]

CSV de funciones: columnas entry,thumb,size,instructions,source (salida de mm3d-inventory). Solo se usan las ARM.
Solo lee los binarios; no escribe contenido del juego (la salida son direcciones y hashes).
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import hashlib
import random
import struct
import sys

BASE = 0x00100000
MIN_INSTR = 8  # por debajo, demasiadas colisiones triviales
LOOSE = False  # --loose: ignora tambien los campos de registro (Rn, Rd, Rm) para una cota superior


def load_funcs(path: str, code_len: int) -> dict[int, int]:
    """Funciones ARM con su extension = distancia hasta la siguiente entrada de funcion (ARM o Thumb).

    El campo `size` del CSV (ultima instruccion - entrada) no sirve: una "funcion" de 3dsrecomp incluye codigo alcanzado
    por saltos lejanos y puede abarcar cientos de miles de instrucciones."""
    entries, arm = [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            e = int(r["entry"], 0) & ~1
            entries.append(e)
            if r["thumb"] == "0":
                arm.append(e)
    entries = sorted(set(entries))
    end = BASE + code_len
    out = {}
    for e in arm:
        i = bisect.bisect_right(entries, e)
        nxt = entries[i] if i < len(entries) else end
        s = min(nxt, end) - e
        if s >= 4 and e % 4 == 0:
            out[e] = s - (s % 4)
    return out


def analyse(code: bytes, entry: int, size: int, cmap: bytes | None = None):
    """Devuelve (hash_enmascarado, n_instrucciones, [destinos de BL en orden])."""
    n = size // 4
    w = struct.unpack_from(f"<{n}I", code, entry - BASE)
    pool: set[int] = set()
    for i, x in enumerate(w):
        if (x >> 28) == 0xF:
            continue
        pc = entry + 4 * i
        if (x & 0x0E000000) == 0x04000000 and ((x >> 16) & 0xF) == 15 and (x & (1 << 20)):  # LDR/LDRB literal
            imm, up = x & 0xFFF, (x >> 23) & 1
            t = pc + 8 + (imm if up else -imm)
            if entry <= t < entry + size:
                pool.add((t - entry) // 4)
        elif (x & 0x0F300E00) == 0x0D100A00 and ((x >> 16) & 0xF) == 15:  # VLDR literal
            imm, up = (x & 0xFF) * 4, (x >> 23) & 1
            t = ((pc + 8) & ~3) + (imm if up else -imm)
            if entry <= t < entry + size:
                pool.add((t - entry) // 4)
    h = hashlib.sha1()
    calls: list[int] = []
    count = 0
    for i, x in enumerate(w):
        if i in pool:
            continue
        if cmap is not None:
            j = (entry - BASE) // 4 + i
            if j >= len(cmap) or not cmap[j]:
                continue  # palabra que 3dsrecomp no clasifica como codigo (tabla, pool, dato)
        pc = entry + 4 * i
        cond = x >> 28
        if cond != 0xF and (x & 0x0E000000) == 0x0A000000:  # B / BL
            if x & 0x01000000:  # BL
                off = x & 0xFFFFFF
                off = off - (1 << 24) if off & 0x800000 else off
                calls.append((pc + 8 + (off << 2)) & 0xFFFFFFFF)
            x &= 0xFF000000
        elif cond == 0xF and (x & 0x0E000000) == 0x0A000000:  # BLX imm
            x &= 0xFE000000
        elif cond != 0xF and (x & 0x0E000000) == 0x04000000 and ((x >> 16) & 0xF) == 15:  # LDR/STR [pc, #imm]
            x &= ~0x00800FFF
        elif cond != 0xF and (x & 0x0F300E00) == 0x0D100A00 and ((x >> 16) & 0xF) == 15:  # VLDR [pc, #imm]
            x &= ~0x008000FF
        elif cond != 0xF and (x & 0x0FEF0000) in (0x028F0000, 0x024F0000):  # ADR
            x &= ~0xFFF
        if LOOSE:
            x &= ~0x000FF00F
        h.update(struct.pack("<I", x & 0xFFFFFFFF))
        count += 1
    return h.hexdigest(), count, calls


def signatures(code: bytes, funcs: dict[int, int], cmap: bytes | None = None):
    return {e: analyse(code, e, s, cmap) for e, s in funcs.items()}


def exact_pairs(sa, sb):
    ga, gb = collections.defaultdict(list), collections.defaultdict(list)
    for e, (h, n, _c) in sa.items():
        if n >= MIN_INSTR:
            ga[h].append(e)
    for e, (h, n, _c) in sb.items():
        if n >= MIN_INSTR:
            gb[h].append(e)
    uniq, amb_b = {}, 0
    for h, lb in gb.items():
        la = ga.get(h)
        if not la:
            continue
        if len(la) == 1 and len(lb) == 1:
            uniq[la[0]] = lb[0]
        else:
            amb_b += len(lb)
    return uniq, amb_b


def propagate(sa, sb, pairs: dict[int, int], hidden_ok=None):
    """Propaga por el grafo de llamadas: si f↔g coinciden y llaman a la misma cantidad de funciones, empareja posicionalmente
    los destinos aún sin emparejar. Exige que las dos listas de destinos tengan igual longitud."""
    pairs = dict(pairs)
    used_b = set(pairs.values())
    added: dict[int, int] = {}
    changed = True
    while changed:
        changed = False
        for a, b in list(pairs.items()):
            ca, cb = sa[a][2], sb[b][2]
            if not ca or len(ca) != len(cb):
                continue
            for ta, tb in zip(ca, cb):
                if ta in pairs or tb in used_b or ta not in sa or tb not in sb:
                    continue
                pairs[ta] = tb
                used_b.add(tb)
                added[ta] = tb
                changed = True
    return pairs, added


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a_code"); ap.add_argument("a_funcs"); ap.add_argument("b_code"); ap.add_argument("b_funcs")
    ap.add_argument("--names"); ap.add_argument("--out"); ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--loose", action="store_true", help="ignorar campos de registro (cota superior de un metodo difuso)")
    ap.add_argument("--no-map", action="store_true", help="no usar los mapas codigo/dato (A.funcs.csv.map, B.funcs.csv.map)")
    args = ap.parse_args()
    global LOOSE
    LOOSE = args.loose
    ca, cb = open(args.a_code, "rb").read(), open(args.b_code, "rb").read()
    fa, fb = load_funcs(args.a_funcs, len(ca)), load_funcs(args.b_funcs, len(cb))
    ma = mb = None
    if not args.no_map:
        ma, mb = open(args.a_funcs + ".map", "rb").read(), open(args.b_funcs + ".map", "rb").read()
    sa, sb = signatures(ca, fa, ma), signatures(cb, fb, mb)
    names = {}
    if args.names:
        for r in csv.DictReader(open(args.names, newline="", encoding="utf-8-sig")):
            names[int(r["entry"], 0)] = r["name"]
    real = lambda n: bool(n) and not n.startswith(("FUN_", "sub_", "caseD", "thunk", "LAB_", "switchD"))

    big_b = {e for e, (h, n, c) in sb.items() if n >= MIN_INSTR}
    wb_total = sum(sb[e][1] for e in sb)
    uniq, amb = exact_pairs(sa, sb)
    full, added = propagate(sa, sb, uniq)

    def share(pairs):
        wb = sum(sb[b][1] for b in pairs.values())
        return 100 * len(pairs) / len(sb), 100 * wb / wb_total
    print(f"A: {len(sa)} funciones ARM | B: {len(sb)} funciones ARM ({len(big_b)} con >= {MIN_INSTR} instrucciones)")
    pe, we = share(uniq)
    print(f"Coincidencias exactas ÚNICAS : {len(uniq):>6}  = {pe:5.1f}% de las funciones de B, {we:5.1f}% de sus instrucciones")
    print(f"  (hashes ambiguos, no emparejados: {amb} funciones de B)")
    pp, wp = share(full)
    print(f"Con propagación por llamadas : {len(full):>6}  = {pp:5.1f}% de las funciones de B, {wp:5.1f}% de sus instrucciones  (+{len(added)} propagadas)")
    if names:
        named_e = [b for a, b in uniq.items() if real(names.get(a, ""))]
        named_p = [b for a, b in full.items() if real(names.get(a, ""))]
        print(f"Con nombre real de A (exactas): {len(named_e)} = {100*len(named_e)/len(sb):.1f}% de B | (con propagación): {len(named_p)} = {100*len(named_p)/len(sb):.1f}%")
    if args.out:
        with open(args.out, "w", newline="") as f:
            w = csv.writer(f); w.writerow(["b_entry", "a_entry", "a_name", "kind"])
            for a, b in sorted(full.items(), key=lambda kv: kv[1]):
                w.writerow([f"0x{b:08X}", f"0x{a:08X}", names.get(a, ""), "propagada" if a in added else "exacta"])
    if args.selftest:
        # Precisión de la propagación: se esconde el 30 % de las exactas, se propaga con el resto y se compara con lo escondido.
        rnd = random.Random(1)
        keys = sorted(uniq)
        hidden = set(rnd.sample(keys, len(keys) * 3 // 10))
        kept = {a: b for a, b in uniq.items() if a not in hidden}
        _full, add = propagate(sa, sb, kept)
        eval_ = {a: b for a, b in add.items() if a in uniq}  # propagadas que tienen verdad conocida (exacta)
        ok = sum(1 for a, b in eval_.items() if uniq[a] == b)
        print(f"AUTOPRUEBA de propagación: de {len(add)} propagadas, {len(eval_)} tienen verdad conocida; correctas {ok} "
              f"= {100*ok/max(1,len(eval_)):.1f}% de precisión; recuperadas {len(eval_)} de {len(hidden)} escondidas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
