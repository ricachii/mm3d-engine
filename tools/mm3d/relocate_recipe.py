#!/usr/bin/env python3
"""Relocate an mm3d-pc translation recipe to another build of the same game code.

mm3d-pc's recipe (tools/mm3d/package/recipe) is a set of ADDRESS lists made for one
code.bin: function entries and sizes, Thumb functions, runtime-discovered entries,
constructor addresses and direct call sites. A different build of the same game
(for example the eShop 1.1.0 update, whose .text is ~500 bytes larger and whose
rodata/data start one page later) has the same code with small insertions, so
every function only moves by a small delta.

This tool finds that delta per function using the player's own code.bin and the
recipe's call sites, and writes a relocated recipe. It is address-only: the recipe
contains no instruction bytes or data of the game; code.bin is only read here, on
the player's machine, to look at its BL/BLX instructions.

Method (all of it checked against the new code):
  1. A call (site -> target) is the pair BL/BLX imm in code.bin. For a call whose
     two ends sit in the same shifted block the relative offset is unchanged, so
     the new site is found by matching relative offsets. A sliding-window vote over
     those candidates gives the dominant delta of each region.
  2. Per function, the delta that makes the most calls (outgoing and incoming)
     land on an existing BL/BLX of the new code with the other end's delta is
     kept; this is iterated until nothing changes.
  3. A relocated call site is kept in the recipe only if the new code really has a
     BL/BLX at the new site to the new target.

Usage: relocate_recipe.py --recipe <old recipe dir> --code <new code.bin>
                          --output <new recipe dir> [--old-text-end 0x51cfb4]
                          [--data-shift 0x1000] [--report report.json]

Part of the MM3D adaptation built on Volpe34's mm3d-pc (GPL-3.0-or-later).
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import lzma
import shutil
import struct
import sys
from pathlib import Path

BASE = 0x00100000
OLD_RODATA = 0x0061D000   # first page after the old build's .text (mm3d-pc "USA v1.1")


def bl_table(code: bytes) -> tuple[dict[int, int], dict[int, list[int]]]:
    """Every BL / BLX imm of the A32 code: site -> target, target -> [sites]."""
    bl: dict[int, int] = {}
    by_target: dict[int, list[int]] = collections.defaultdict(list)
    for offset in range(0, len(code) - 3, 4):
        word = struct.unpack_from("<I", code, offset)[0]
        top = word >> 24
        if top not in (0xEB, 0xFA, 0xFB):
            continue
        imm = word & 0xFFFFFF
        if imm & 0x800000:
            imm -= 1 << 24
        site = BASE + offset
        target = site + 8 + imm * 4 + (2 if top == 0xFB else 0)
        bl[site] = target
        by_target[target].append(site)
    return bl, by_target


class Relocator:
    def __init__(self, recipe: Path, code: bytes, window: int = 40, reach: int = 0x400):
        calls = json.load(lzma.open(recipe / "call_sites.json.xz", "rt"))["functions"][0]["direct_calls"]
        self.calls = sorted((c["site"], c["target"]) for c in calls)
        entries = {int(r["entry"], 16) for r in csv.DictReader(open(recipe / "ghidra_arm_inventory.csv"))}
        for function in json.load(open(recipe / "thumb_inventory.json"))["thumb_functions"]:
            entries.add(function["entry"] & ~1)
        for _, target in self.calls:
            entries.add(target & ~1)
        self.entries = sorted(entries)
        self.bl, self.by_target = bl_table(code)
        self.window, self.reach = window, reach
        self.delta = self._initial_profile()
        self._refine()

    # -- step 1 ---------------------------------------------------------------
    def _initial_profile(self) -> list[int]:
        by_rel: dict[int, list[int]] = collections.defaultdict(list)
        for site, target in self.bl.items():
            by_rel[target - site].append(site)
        candidates = [{c - s for c in by_rel.get(t - s, ()) if abs(c - s) <= 0x80000} for s, t in self.calls]
        voted: list[tuple[int, int]] = []
        for i, (site, _) in enumerate(self.calls):
            votes: collections.Counter = collections.Counter()
            for j in range(max(0, i - self.window), min(len(self.calls), i + self.window + 1)):
                votes.update(candidates[j])
            if votes:
                delta, count = votes.most_common(1)[0]
                if count >= 5 and delta in candidates[i]:
                    voted.append((site, delta))
        starts = [v[0] for v in voted]
        result = []
        for entry in self.entries:
            k = bisect.bisect_right(starts, entry) - 1
            result.append(voted[k][1] if k >= 0 else 0)
        return result

    # -- step 2 ---------------------------------------------------------------
    def _function(self, address: int) -> int:
        return bisect.bisect_right(self.entries, address & ~1) - 1

    def _refine(self) -> None:
        outgoing: dict[int, list] = collections.defaultdict(list)
        incoming: dict[int, list] = collections.defaultdict(list)
        for site, target in self.calls:
            f, g = self._function(site), self._function(target)
            outgoing[f].append((site, target, g))
            incoming[g].append((site, target, f))
        delta = self.delta
        for _ in range(6):
            changed = 0
            for f in range(len(self.entries)):
                if not outgoing[f] and not incoming[f]:
                    continue
                guesses: collections.Counter = collections.Counter()
                for site, target, g in outgoing[f]:
                    for new_site in self.by_target.get(target + delta[g], ()):
                        x = new_site - site
                        if abs(x - delta[f]) <= self.reach:
                            guesses[x] += 1
                for site, target, g in incoming[f]:
                    new_target = self.bl.get(site + delta[g])
                    if new_target is not None and abs(new_target - target - delta[f]) <= self.reach:
                        guesses[new_target - target] += 1
                if not guesses:
                    continue

                def score(x: int) -> int:
                    n = sum(1 for s, t, g in outgoing[f] if self.bl.get(s + x) == t + delta[g])
                    return n + sum(1 for s, t, g in incoming[f] if self.bl.get(s + delta[g]) == t + x)
                best = max(guesses, key=lambda x: (score(x), -abs(x - delta[f])))
                if score(best) > score(delta[f]):
                    delta[f] = best
                    changed += 1
            if not changed:
                break
        self.supported = [bool(outgoing[f] or incoming[f]) for f in range(len(self.entries))]

        def strength(f: int) -> int:
            n = sum(1 for s, t, g in outgoing[f] if self.bl.get(s + delta[f]) == t + delta[g])
            return n + sum(1 for s, t, g in incoming[f] if self.bl.get(s + delta[g]) == t + delta[f])
        power = [strength(f) for f in range(len(self.entries))]
        # A function backed by at most one call can match by coincidence (a Thumb helper
        # ended up in the middle of ARM code that way): unless its delta is the one of a
        # strongly backed neighbour, it takes the delta of the previous strong function.
        self.smoothed = 0
        for f in range(len(self.entries)):
            if power[f] > 1:
                continue
            previous = next((delta[k] for k in range(f - 1, -1, -1) if power[k] >= 3), None)
            following = next((delta[k] for k in range(f + 1, len(self.entries)) if power[k] >= 3), None)
            if delta[f] not in (previous, following) and (previous is not None or following is not None):
                delta[f] = previous if previous is not None else following
                self.smoothed += 1
        self._anchors()

    # -- step 2b: anchors -----------------------------------------------------
    def _anchors(self) -> None:
        """Per-call anchors (old address -> delta), to see changes INSIDE a function.

        A function that grew or shrank has one delta before the change and another
        after it. Every call that lands on a real BL/BLX gives an anchor for its site
        and for its target; an address whose neighbouring anchors disagree is flagged
        uncertain by map()."""
        anchors: dict[int, int] = {}
        for site, target in self.calls:
            f, g = self._function(site), self._function(target)
            ds, dt = self.delta[f], self.delta[g]
            if self.bl.get(site + ds) == target + dt:
                anchors[site] = ds
                anchors[target & ~1] = dt
                continue
            near = [s2 for s2 in self.by_target.get(target + dt, ()) if abs(s2 - (site + ds)) <= 0x100]
            if near:
                anchors.setdefault(site, min(near, key=lambda s2: abs(s2 - site - ds)) - site)
                continue
            s2 = site + ds
            for k in range(-0x100, 0x101, 4):
                t2 = self.bl.get(site + ds + k)
                if t2 is not None and abs(t2 - (target + dt)) <= 0x100:
                    anchors.setdefault(site, ds + k)
                    anchors.setdefault(target & ~1, t2 - target)
                    break
        self.anchor_addresses = sorted(anchors)
        self.anchor_delta = [anchors[a] for a in self.anchor_addresses]

    # -- public mapping -------------------------------------------------------
    def locate(self, address: int, data_shift: int) -> tuple[int, bool]:
        """(new address, certain) of an old one; keeps the Thumb bit.

        Uncertain: the anchors on both sides of the address disagree (code was added or
        removed in between) or there is none."""
        thumb = address & 1
        base = address & ~1
        if base >= OLD_RODATA:
            return (base + data_shift) | thumb, True
        f = self._function(base)
        i = bisect.bisect_right(self.anchor_addresses, base) - 1
        j = i + 1
        if 0 <= i and j < len(self.anchor_addresses) and self.anchor_delta[i] == self.anchor_delta[j]:
            return (base + self.anchor_delta[i]) | thumb, True
        return (base + (self.delta[f] if f >= 0 else 0)) | thumb, False

    def map(self, address: int, data_shift: int) -> int:
        """New address of an old one (keeps the Thumb bit)."""
        return self.locate(address, data_shift)[0]

    # -- uncertain addresses --------------------------------------------------
    def set_oracle(self, code: bytes, arm_entries: set[int], thumb_entries: set[int]) -> None:
        self.code, self.arm_entries, self.thumb_entries = code, arm_entries, thumb_entries

    def _entry_score(self, address: int, thumb: bool) -> int:
        """How much the new code says `address` is a function entry: 2 = the independent
        inventory (3dsrecomp) has it, 1 = it starts like a function, 0 = neither."""
        offset = address - BASE
        if offset < 0 or offset + 4 > len(self.code) or address & (1 if not thumb else 0):
            return 0
        if thumb:
            if address in self.thumb_entries:
                return 2
            half = struct.unpack_from("<H", self.code, offset)[0]
            return 1 if (half & 0xFF00) in (0xB500, 0xB400) else 0   # push {..., lr}
        if address in self.arm_entries:
            return 2
        word = struct.unpack_from("<I", self.code, offset)[0]
        return 1 if (word & 0xFFFF0000) == 0xE92D0000 and word & 0x4000 else 0  # stmdb sp!, {..., lr}

    def resolve(self, address: int, data_shift: int, thumb: bool | None = None) -> int | None:
        """New address of an old function entry, or None if it cannot be told.

        Certain addresses map directly. An uncertain one (code was added or removed
        around it) is tried with the deltas of the anchors on both sides and of its
        function; it is accepted only if exactly one candidate looks like a function
        entry in the new code."""
        new, certain = self.locate(address, data_shift)
        thumb = bool(address & 1) if thumb is None else thumb
        if certain:
            return new
        base = address & ~1
        i = bisect.bisect_right(self.anchor_addresses, base) - 1
        deltas = {self.delta[max(0, self._function(base))]}
        for k in (i, i + 1):
            if 0 <= k < len(self.anchor_delta):
                deltas.add(self.anchor_delta[k])
        scored = {}
        for d in deltas:
            candidate = base + d
            score = self._entry_score(candidate, thumb)
            if score:
                scored[candidate] = score
        if not scored:
            return None
        best = max(scored.values())
        winners = [a for a, v in scored.items() if v == best]
        return (winners[0] | (address & 1)) if len(winners) == 1 else None

    def consistent_calls(self, data_shift: int) -> list[tuple[int, int]]:
        kept = []
        for site, target in self.calls:
            s2, t2 = self.map(site, data_shift), self.map(target, data_shift) & ~1
            if self.bl.get(s2, None) is not None and (self.bl[s2] & ~1) == t2:
                kept.append((s2, t2 | (target & 1)))
        return kept


def load_inventory(path: Path | None) -> tuple[set[int], set[int], set[int]]:
    """Function entries of an independent inventory (mm3d-inventory CSV: entry,thumb,...):
    ARM entries, Thumb entries, and the Thumb entries that are the target of a call."""
    arm: set[int] = set()
    thumb: set[int] = set()
    called: set[int] = set()
    if path is not None:
        for row in csv.DictReader(open(path, newline="")):
            entry = int(row["entry"], 16) & ~1
            if row["thumb"] == "1":
                thumb.add(entry)
                if row.get("source") in ("Call", "Entry"):
                    called.add(entry)
            else:
                arm.add(entry)
    return arm, thumb, called


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--code", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--data-shift", type=lambda x: int(x, 0), default=0x1000)
    parser.add_argument("--our-inventory", type=Path,
                        help="funcs.csv of an independent function inventory of the new code (3dsrecomp)")
    parser.add_argument("--exheader", type=Path, help="exheader.bin of the new code (its .text size bounds the Thumb discovery)")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    code = args.code.read_bytes()
    r = Relocator(args.recipe, code)
    arm_entries, thumb_entries, called_thumb = load_inventory(args.our_inventory)
    r.set_oracle(code, arm_entries, called_thumb)
    shift = args.data_shift
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    report: dict = {}

    n_fun = len(r.entries)
    n_ev = sum(r.supported)
    report["function_entries"] = {"total": n_fun, "with_call_evidence": n_ev,
                                  "inferred_from_neighbours": n_fun - n_ev,
                                  "corrected_to_neighbour_delta": r.smoothed}
    old_calls = len(r.calls)
    kept = r.consistent_calls(shift)
    report["call_sites"] = {"old": old_calls, "relocated_and_verified": len(kept),
                            "dropped": old_calls - len(kept)}
    with lzma.open(out / "call_sites.json.xz", "wt", preset=9) as target:
        json.dump({"format": "mm3d-call-sites-v1", "functions": [
            {"direct_calls": [{"site": s, "target": t} for s, t in sorted(set(kept))]}]},
            target, separators=(",", ":"))

    def tally(name: str, total: int, certain: int, resolved: int) -> dict:
        return {"total": total, "certain": certain, "resolved_by_inspection": resolved,
                "dropped": total - certain - resolved}

    # ARM inventory (Ghidra function entries)
    rows = list(csv.DictReader(open(args.recipe / "ghidra_arm_inventory.csv")))
    certain = resolved = 0
    with open(out / "ghidra_arm_inventory.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["entry", "size", "name"])
        w.writeheader()
        for row in rows:
            old = int(row["entry"], 16)
            ok = r.locate(old, shift)[1]
            entry = r.resolve(old, shift, thumb=False)
            if entry is None:
                continue
            certain += ok
            resolved += not ok
            name = row["name"]
            if name.startswith("FUN_") or name.startswith("thunk_FUN_"):
                name = name.split("FUN_")[0] + f"FUN_{entry:08x}"
            w.writerow({"entry": f"0x{entry:08X}", "size": row["size"], "name": name})
    report["arm_inventory"] = tally("arm", len(rows), certain, resolved)

    # Thumb inventory. A relocated function is kept only if the independent inventory also
    # sees a Thumb entry there (a mis-relocated one sends the converter into ARM code);
    # Thumb entries that are call targets in the new code and are not covered yet are
    # added, their ranges found with the converter's own recursive descent.
    thumb = json.load(open(args.recipe / "thumb_inventory.json"))
    kept_thumb = []
    certain = resolved = 0
    covered: set[int] = set()
    for function in thumb["thumb_functions"]:
        old_start = int(function["ranges"].split(";")[0].split("-")[0], 16)
        ok = r.locate(old_start, shift)[1]
        new_start = r.resolve(old_start, shift, thumb=True)
        if new_start is None or (new_start & ~1) not in called_thumb:
            continue
        certain += ok
        resolved += not ok
        parts = []
        function_delta = (new_start & ~1) - old_start        # one delta for the whole function
        for part in function["ranges"].split(";"):
            a, b = (int(x, 16) for x in part.split("-"))
            a2 = a + function_delta
            parts.append(f"{a2:x}-{a2 + (b - a):x}")
            covered.update(range(a2, a2 + (b - a), 2))
        function["entry"] = (function["entry"] & ~1) + function_delta | (function["entry"] & 1)
        function["ranges"] = ";".join(parts)
        function["name"] = f"FUN_{function['entry'] & ~1:08x}"
        kept_thumb.append(function)
    added = 0
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import thumb_to_a32
    if args.exheader:
        thumb_to_a32.TEXT_LIMIT = BASE + (struct.unpack_from("<I", args.exheader.read_bytes(), 0x18)[0] & ~0xFFF)

    def descend(entry: int, known: set[int]):
        try:
            return thumb_to_a32.discover_thumb_function(code, entry, known)
        except (thumb_to_a32.ConversionError, struct.error):
            return None

    # Close the Thumb set under calls: every BL target of a seeded function is a function too.
    pending = set(called_thumb)
    range_mismatch = 0
    for function in kept_thumb:
        found = descend(function["entry"] & ~1, set())
        if found:
            pending.update(found[1])
            if found[0] != function["ranges"]:
                # The code of this function differs from the old build's: trust the new code.
                range_mismatch += 1
                function["ranges"] = found[0]
    while pending:
        entry = pending.pop() & ~1
        if entry in covered:
            continue
        found = descend(entry, covered)
        if found is None:
            continue
        ranges, calls = found
        for part in ranges.split(";"):
            a, b = (int(x, 16) for x in part.split("-"))
            covered.update(range(a, b, 2))
        kept_thumb.append({"entry": entry, "name": f"FUN_{entry:08x}", "ranges": ranges})
        added += 1
        pending.update(c & ~1 for c in calls if c & ~1 not in covered)
    kept_thumb.sort(key=lambda f: f["entry"])
    report["thumb_functions"] = {**tally("thumb", len(thumb["thumb_functions"]), certain, resolved),
                                 "added_from_independent_inventory": added, "ranges_redone_from_new_code": range_mismatch,
                                 "total_after": len(kept_thumb)}
    thumb["thumb_functions"] = kept_thumb
    (out / "thumb_inventory.json").write_text(json.dumps(thumb, indent=0))

    # address lists (entries observed at run time / constructors)
    for name, fmt in (("runtime_misses.txt", "{:08x}"), ("dynamic_entries.txt", "{:08x}"),
                      ("init_array.txt", "0x{:x}")):
        lines = [x.strip() for x in (args.recipe / name).read_text().splitlines() if x.strip()]
        new_lines = []
        certain = resolved = 0
        for x in lines:
            value = int(x, 16)
            ok = r.locate(value, shift)[1]
            new = r.resolve(value, shift)
            if new is None:
                continue
            certain += ok
            resolved += not ok
            new_lines.append(fmt.format(new))
        (out / name).write_text("".join(v + chr(10) for v in new_lines))
        report[name] = tally(name, len(lines), certain, resolved)
    for name in ("empty_boundary_audit.csv", "empty_selection.json"):
        shutil.copy2(args.recipe / name, out / name)
    # selection.json.xz is NOT relocated: its fingerprint belongs to the old program. Setup
    # computes the selection for the new program (mm3d_select) and `make_recipe.py` stores it.
    if args.report:
        args.report.write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
