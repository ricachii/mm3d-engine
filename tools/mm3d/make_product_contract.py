#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive a whole-AOT product contract (hashes and counts only) for a title.

The audit's structural checks (closed CFGs, no residual A32, dispatcher
coverage) stay meaningful; the identity and count fields pin these inputs.
Usage: make_product_contract.py --program P --selection S --code C
       --exheader E --generated-manifest M --output contract.json
"""
import argparse, hashlib, json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("program", "selection", "code", "exheader", "generated-manifest", "output"):
        ap.add_argument("--" + name, type=Path, required=True)
    a = ap.parse_args()
    program = json.loads(a.program.read_text(encoding="utf-8"))
    selection = json.loads(a.selection.read_text(encoding="utf-8"))
    generated = json.loads(a.generated_manifest.read_text(encoding="utf-8"))
    exheader = a.exheader.read_bytes()
    functions, blocks = program["functions"], program["blocks"]
    by_entry = {int(f["entry"]): f for f in functions}
    selected = [int(f["entry"]) for f in selection["functions"]]
    external = selection.get("external_functions", [])
    dispatch = set(selected)
    for entry in selected:
        dispatch.update(int(blocks[i]["pc"]) for i in by_entry[entry]["blocks"])
    all_blocks = {int(b["pc"]) for b in blocks}
    inputs = program.get("inputs", {})
    contract = {
        "format": "oot3d_whole_aot_product_contract_v1",
        "source_image": {
            "code": {"operational_id": "oot3d_code_bin", "bytes": a.code.stat().st_size,
                     "sha256": sha(a.code)},
            "exheader": {"operational_id": "oot3d_exheader", "bytes": len(exheader),
                         "sha256": sha(a.exheader)},
            "base": program["base"],
            "entrypoint": int.from_bytes(exheader[0x10:0x14], "little"),
            "executable_size": int.from_bytes(exheader[0x14:0x18], "little") * 0x1000,
        },
        "frontend": {"program_format": program["format"],
                     "inventory_sha256": inputs.get("inventory_sha256"),
                     "boundary_audit_sha256": inputs.get("boundary_audit_sha256")},
        "selection": {"path": a.selection.resolve().as_posix(), "format": selection["format"],
                      "mode": selection.get("selection_mode"), "sha256": sha(a.selection)},
        "closure": {
            "counts": {
                "functions": len(functions), "selected_functions": len(selected),
                "host_boundaries": len(external), "blocks": len(blocks),
                "unique_instructions": program["counts"]["instructions"],
                "emitted_instructions": sum(int(blocks[i]["instruction_count"])
                                            for e in selected for i in by_entry[e]["blocks"]),
                "dispatch_aliases": len(selection.get("selection_conflicts", [])),
                "dispatcher_entries": len(dispatch),
                "atomic_host_blocks": len(all_blocks - dispatch),
            },
            "host_boundaries": [{"entry": int(x["entry"]), "name": x.get("name", "")} for x in external],
            "residual_a32_entries": 0,
        },
        "codegen": {"format": generated["format"], "shard_count": generated["shard_count"],
                    "shard_strategy": generated["shard_strategy"]},
    }
    a.output.write_text(json.dumps(contract, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
