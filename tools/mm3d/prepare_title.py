#!/usr/bin/env python3
"""prepare_title.py: de un code.bin + exheader.bin descomprimidos al programa AOT, la seleccion y el informe.

Pasos (cada uno reanudable: si el archivo de salida existe se reutiliza, salvo con --force):
  1. mm3d-inventory (3dsrecomp)  -> funcs.csv (+ .map codigo/dato, + .modes ARM/Thumb por media-palabra)
  2. inventario sintetico ARM    -> inventory.csv (entry,size,name=sub_XXXXXXXX), sin hints, sin las entradas Thumb
  3. whole_aot_program.py        -> aot_program.json
  4. seleccion con cierre        -> selection.json: cada funcion se intenta bajar con el propio generador; las que fallan
                                    (instruccion sin soporte, datos decodificados como codigo, llamada a algo no compilable)
                                    se descartan y se repite con sus llamadores. Las llamadas ARM->Thumb (BLX inmediato)
                                    se declaran `external` (las implementa el host).
  5. report.json                 -> que se descarto y por que; descartes alcanzables desde la entrada por llamadas directas.

Uso: python3 prepare_title.py --code code.bin --exheader exheader.bin --workdir DIR --tool mm3d-inventory [--base 0x100000]
Solo lee binarios y escribe direcciones, tamanos y contadores.
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
A32 = HERE.parent / "oot3d/native_a32_runtime"
sys.path.insert(0, str(A32))
sys.path.insert(0, str(A32 / "upstream/src"))

import whole_aot_cpp as G  # noqa: E402


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def step_inventory(args, work: Path):
    funcs = work / "funcs.csv"
    if args.force or not funcs.exists():
        log("1/5 descubrimiento de funciones (3dsrecomp)")
        subprocess.run([args.tool, args.code, args.exheader, str(funcs), "--no-hints"], check=True)
    rows = list(csv.DictReader(funcs.open(newline="")))
    modes = (work / "funcs.csv.modes").read_bytes()
    return rows, modes


def thumb_only(modes: bytes, base: int, address: int) -> bool:
    index = (address - base) // 2
    return 0 <= index < len(modes) and modes[index] == 2


def skip_word_call_sites(code: bytes, base: int) -> list[int]:
    """Sitios de `bl` (condicion siempre) a un ayudante cuya primera instruccion es `ldr rX, [lr], #4`.

    Ese ayudante lee la palabra en linea que sigue al `bl` y devuelve a `site + 8`: la palabra es un dato (en MM3D,
    los comodines de excepcion de coma flotante de la libreria matematica), no una instruccion. whole_aot_program.py
    supone que la llamada vuelve a `site + 4`, decodifica el dato como instruccion y, si no decodifica, deja la
    funcion abierta (`0x1DAFC0` y `0x61CBF4`). Con OOT3D_SKIP_WORD_CALLS=1 el flujo siembra `site + 8` como bloque
    (ver a32_cpp_aot.py) y `repair_program` cierra la funcion.
    """
    sites = []
    count = len(code) // 4
    for index in range(count):
        raw = struct.unpack_from("<I", code, index * 4)[0]
        if (raw & 0xFF000000) != 0xEB000000:
            continue
        offset = raw & 0x00FFFFFF
        if offset & 0x00800000:
            offset -= 0x01000000
        target = base + index * 4 + 8 + offset * 4
        slot = (target - base) // 4
        if target % 4 or not 0 <= slot < count:
            continue
        # LDR Rd, [LR], #4  (post-indexado, suma, sin escritura en PC): 0xE49E?004
        if (struct.unpack_from("<I", code, slot * 4)[0] & 0xFFFF0FFF) == 0xE49E0004:
            sites.append(base + index * 4)
    return sites


def step_synthetic_inventory(args, work: Path, rows, modes, code_len: int):
    base = args.base
    end = base + code_len
    thumb_entries, kept, dropped_thumb_overlap = [], [], 0
    for row in rows:
        entry = int(row["entry"], 0)
        if row["thumb"] == "1":
            thumb_entries.append((entry & ~1, row["source"]))
            continue
        size = int(row["size"])
        if size <= 0 or entry % 4 or entry + size > end:
            continue
        if thumb_only(modes, base, entry):
            dropped_thumb_overlap += 1
            continue
        kept.append((entry, size))
    inventory = work / "inventory.csv"
    with inventory.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["entry", "size", "name"])
        for entry, size in kept:
            writer.writerow([f"0x{entry:08X}", size, f"sub_{entry:08X}"])
    (work / "audit_empty.csv").write_text("entry\n")
    with (work / "thumb_entries.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["entry", "source"])
        for entry, source in sorted(thumb_entries):
            writer.writerow([f"0x{entry:08X}", source])
    log(f"2/5 inventario sintetico ARM: {len(kept)} funciones; Thumb: {len(thumb_entries)} "
        f"({sum(1 for _, s in thumb_entries if s == 'Call')} por llamada); "
        f"ARM descartadas por entrar en Thumb: {dropped_thumb_overlap}")
    return inventory, thumb_entries


def step_program(args, work: Path, inventory: Path, executable_size: int) -> dict:
    program = work / "aot_program.json"
    if args.force or not program.exists():
        log("3/5 programa AOT (whole_aot_program.py), unos 3 minutos")
        subprocess.run(
            [sys.executable, str(A32 / "whole_aot_program.py"), "--code", args.code, "--inventory", str(inventory),
             "--boundary-audit", str(work / "audit_empty.csv"), "--base", hex(args.base),
             "--executable-size", hex(executable_size), "--output", str(program), "--force"],
            check=True, env={**os.environ, "OOT3D_SKIP_WORD_CALLS": "1"})
    return json.loads(program.read_text())


def is_blx_immediate(code: bytes, base: int, site: int) -> bool:
    raw = struct.unpack_from("<I", code, site - base)[0]
    return (raw & 0xFE000000) == 0xFA000000


def repair_program(program: dict, code: bytes, base: int) -> int:
    """Cierra las funciones que solo estan 'abiertas' por llamadas BLX <imm> con destino Thumb.

    whole_aot_program.py no puede crear un bloque en un destino Thumb no alineado a 4 (bit H del BLX) y deja la arista en
    `unresolved_static_edges`, aunque la llamada ya esta en `direct_calls` y el bloque tiene sus aristas direct_call y
    resume (identico a un BLX ya cerrado). Si TODAS las aristas sin resolver son de ese tipo, la funcion se marca cerrada.
    """
    repaired = 0
    skip_resumes = {site + 4 for site in skip_word_call_sites(code, base)}
    block_pcs = {int(block["pc"]) for block in program.get("blocks", [])}
    for function in program["functions"]:
        if function.get("closed_static_cfg"):
            continue
        edges = function.get("unresolved_static_edges", [])
        # Dato en linea tras un `bl` salta-palabra: la arista "desconocida" es el dato, y el codigo que le sigue
        # (site + 4) ya es un bloque sembrado. Solo se cierra si TODAS las aristas abiertas son de ese tipo.
        if edges and all(
            edge.get("kind") == "unknown" and edge.get("site") is not None
            and int(edge["site"]) in skip_resumes and int(edge["site"]) + 4 in block_pcs
            for edge in edges
        ):
            function["closed_static_cfg"] = True
            function["unresolved_static_edges"] = []
            function["repaired"] = "inline-data-after-skip-word-call"
            repaired += 1
            continue
        calls = {(int(c["site"]), int(c["target"])) for c in function.get("direct_calls", [])}
        if not edges:
            continue
        ok = all(
            edge.get("kind") == "direct_call" and edge.get("site") is not None
            and is_blx_immediate(code, base, int(edge["site"]))
            and any(site == int(edge["site"]) for site, _ in calls)
            for edge in edges
        )
        if ok:
            function["closed_static_cfg"] = True
            function["unresolved_static_edges"] = []
            function["repaired"] = "blx-immediate-thumb-target"
            repaired += 1
    return repaired


def step_selection(args, work: Path, program: dict, code: bytes, thumb_entries):
    repaired = repair_program(program, code, args.base)
    (work / "aot_program.repaired.json").write_text(json.dumps(program))
    log(f"4/5 seleccion con cierre (funciones reparadas por BLX Thumb: {repaired})")
    thumb_even = {entry for entry, _ in thumb_entries}
    all_closed = [f for f in program["functions"] if f.get("closed_static_cfg")]
    closed_entries = {int(f["entry"]) for f in all_closed}
    targets = {int(c["target"]) for f in all_closed for c in f.get("direct_calls", [])}
    relaxed = {
        "format": G.SELECTION_FORMAT,
        "functions": [{"entry": int(f["entry"])} for f in all_closed],
        "external_functions": [{"entry": t} for t in sorted(targets - closed_entries)],
    }
    functions, _ = G._load_functions(program, relaxed, code)
    by_entry = {f.entry: f for f in functions}
    callers: dict[int, set[int]] = collections.defaultdict(set)
    for f in functions:
        for _, target in f.direct_calls:
            callers[target].add(f.entry)
    modes = (work / "funcs.csv.modes").read_bytes()
    selected = {f.entry for f in functions if f.entry not in thumb_even and not thumb_only(modes, args.base, f.entry)}
    reason: dict[int, str] = {}
    for entry in set(by_entry) - selected:
        reason[entry] = "thumb: la entrada es codigo Thumb" if entry in thumb_even else "thumb: entrada en una zona Thumb"
    # Externos: entradas Thumb a las que llama codigo ARM seleccionado + destinos de llamadas que no son funciones del programa.
    external = set(thumb_even) | (targets - closed_entries)
    symbols = {e: G._symbol(by_entry[e].name, e) for e in selected}
    ext = frozenset(external)
    G._TRAP_UNSUPPORTED = True  # los datos decodificados como codigo no deben descartar la funcion entera
    G._TRAPPED = []
    queue = list(selected)
    rounds = 0
    while queue:
        rounds += 1
        retry: set[int] = set()
        for entry in queue:
            if entry not in selected:
                continue
            try:
                G._render_function(by_entry[entry], symbols, ext)
            except G.LoweringError as error:
                selected.discard(entry)
                symbols.pop(entry, None)
                reason[entry] = f"lowering: {error}"
                retry |= {c for c in callers.get(entry, ()) if c in selected}
            except Exception as error:  # noqa: BLE001 - se informa, no se oculta
                selected.discard(entry)
                symbols.pop(entry, None)
                reason[entry] = f"excepcion: {type(error).__name__}: {error}"
                retry |= {c for c in callers.get(entry, ()) if c in selected}
        log(f"   ronda {rounds}: {len(queue)} funciones intentadas, {len(retry)} llamadores a reintentar, "
            f"{len(selected)} seleccionadas")
        queue = sorted(retry)
    # externos realmente usados por codigo seleccionado
    used_external = sorted({t for e in selected for _, t in by_entry[e].direct_calls if t not in selected})
    blx_targets = {t for e in selected for site, t in by_entry[e].direct_calls
                   if t not in selected and is_blx_immediate(code, args.base, site)}
    unresolved = [t for t in used_external if t not in ext]
    selection = {
        "format": G.SELECTION_FORMAT,
        "source_profile": "prepare_title.py: todas las funciones compilables",
        "source_profile_sha256": "0" * 64,
        "functions": [{"entry": e, "name": by_entry[e].name, "reason": "closed-CFG lowering"} for e in sorted(selected)],
        "external_functions": [{"entry": t, "name": f"external_{t:08X}",
                                "reason": ("host: Thumb (BLX inmediato)" if t in blx_targets or t in thumb_even
                                           else "host: ARM no compilable")} for t in used_external],
        "performance_exclusions": [], "selection_conflicts": [], "closure_groups": [],
        "selection_mode": "all_lowerable",
    }
    (work / "selection.json").write_text(json.dumps(selection, indent=1))
    trapped = {t["pc"]: t for t in G._TRAPPED}
    (work / "trapped.json").write_text(json.dumps(sorted(trapped.values(), key=lambda t: t["pc"]), indent=0))
    (work / "reasons.json").write_text(json.dumps({f"0x{e:08X}": r for e, r in sorted(reason.items()) if e not in selected}, indent=0))
    return selection, by_entry, selected, reason, callers, thumb_even, unresolved


def step_report(args, work: Path, program, selection, by_entry, selected, reason, callers, thumb_even, unresolved):
    entry_point = args.base
    # alcanzables desde la entrada por llamadas directas (subaproximacion: ignora saltos indirectos)
    seen, stack = set(), [entry_point]
    while stack:
        e = stack.pop()
        if e in seen or e not in by_entry:
            continue
        seen.add(e)
        stack.extend(t for _, t in by_entry[e].direct_calls)
    dropped = {e: r for e, r in reason.items() if e not in selected}
    kinds = collections.Counter(r.split(":")[0] for r in dropped.values())
    cascade = {e for e, r in dropped.items() if "not selected or external" in r}
    reach_dropped = sorted(e for e in dropped if e in seen and not dropped[e].startswith("thumb"))
    import re
    def normalise(text):
        text = re.sub(r"0x[0-9A-Fa-f]{8}", "0x*", text)
        return re.sub(r"\d+", "#", text)[:100]
    messages = collections.Counter(normalise(r) for e, r in dropped.items() if e not in cascade)
    report = {
        "funciones_programa": len(program["functions"]),
        "seleccionadas": len(selected),
        "externas_usadas": len(selection["external_functions"]),
        "externas_thumb_usadas": sum(1 for x in selection["external_functions"] if x["reason"].startswith("host: Thumb")),
        "externas_arm_no_compilables": [f"0x{x['entry']:08X}" for x in selection["external_functions"]
                                        if x["reason"].startswith("host: ARM")],
        "instrucciones_trampeadas": len(json.loads((work / "trapped.json").read_text())),
        "descartadas_por_tipo": dict(kinds),
        "descartadas_por_cascada": len(cascade),
        "alcanzables_desde_la_entrada_por_llamadas_directas": len(seen),
        "descartadas_alcanzables_no_thumb": len(reach_dropped),
        "descartadas_alcanzables_no_thumb_lista": [f"0x{e:08X}" for e in reach_dropped[:60]],
        "primeros_errores_no_cascada": messages.most_common(25),
        "externos_sin_declarar": [f"0x{t:08X}" for t in unresolved],
    }
    (work / "report.json").write_text(json.dumps(report, indent=1, ensure_ascii=False))
    log("5/5 informe")
    print(json.dumps(report, indent=1, ensure_ascii=False))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--code", required=True)
    ap.add_argument("--exheader", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--tool", required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x00100000)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    code = Path(args.code).read_bytes()
    exheader = Path(args.exheader).read_bytes()
    executable_size = struct.unpack_from("<I", exheader, 0x14)[0] * 0x1000
    rows, modes = step_inventory(args, work)
    inventory, thumb_entries = step_synthetic_inventory(args, work, rows, modes, len(code))
    program = step_program(args, work, inventory, executable_size)
    selection, by_entry, selected, reason, callers, thumb_even, unresolved = step_selection(
        args, work, program, code, thumb_entries)
    step_report(args, work, program, selection, by_entry, selected, reason, callers, thumb_even, unresolved)
    return 0


if __name__ == "__main__":
    sys.exit(main())
