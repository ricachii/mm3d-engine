"""Generate the selected whole-function C++ AOT shards for OOT3D."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Sequence

from generate_aot import operational_path
from generate_whole_aot import main as generate_program
from whole_aot_cpp import HOOK_GATES_NAME, generate, load_hook_gates


ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[2]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--program",
        type=Path,
        default=REPO_ROOT / "build-codex/oot3d_whole_aot/aot_program.json",
    )
    parser.add_argument(
        "--selection",
        type=Path,
        default=ROOT / "whole_aot_functions.json",
    )
    parser.add_argument(
        "--code",
        type=Path,
        default=operational_path("oot3d_code_bin"),
        help="decrypted code.bin matched by the whole-AOT program",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / "build-codex/oot3d_whole_aot_cpp",
    )
    parser.add_argument(
        "--shards",
        type=int,
        default=32,
        help="stable generated C++ function shard count",
    )
    parser.add_argument(
        "--shard-strategy",
        choices=(
            "stable", "balanced", "incremental", "affinity",
            "affinity-incremental",
        ),
        default="stable",
        help=(
            "stable hash placement, weight-balanced placement, preservation "
            "of existing assignments, direct-call affinity placement, or "
            "affinity placement that preserves the previous manifest's "
            "assignments (ccache-friendly)"
        ),
    )
    parser.add_argument(
        "--callback-free-region", action="append", type=lambda value: int(value, 0),
        default=[], help="experimental closed call-free function entry (repeatable)",
    )
    parser.add_argument(
        "--memory-region", action="append", type=lambda value: int(value, 0),
        default=[], help="experimental memory-region function entry (repeatable)",
    )
    parser.add_argument(
        "--local-state", action="store_true",
        help="experimental: keep promoted GPRs/flags in locals in every function",
    )
    parser.add_argument(
        "--promote-registers", action="store_true",
        help="GPRs, NZCV and block budget in local variables in all ordinary "
             "functions, written to the state only at calls (dirty set), "
             "exits and hooks (exact; changes the C++ and invalidates PGO)",
    )
    parser.add_argument(
        "--no-literal-folding", action="store_true",
        help="keep PC-relative literal loads from the read-only text segment "
             "as memory reads instead of folding them to constants",
    )
    parser.add_argument(
        "--guest-overrides", type=Path, nargs="?", const=Path(__file__).resolve().with_name("guest_overrides.json"),
        default=None,
        help="enables the listed native overrides (default: guest_overrides.json) "
             "in the generated shards; see apply_guest_overrides.py",
    )
    parser.add_argument(
        "--native-vfp-aarch64", action="store_true",
        help="execute scalar binary32 VFP with native AArch64 instructions, "
             "the guest FPSCR mode loaded in FPCR and flags accumulated in "
             "FPSR (exact; software operations on other hosts)",
    )
    parser.add_argument(
        "--vfp-zero-aware", action="store_true",
        help="with --native-vfp-aarch64: arithmetic operations use "
             "Oot3dVfpZeroAware*, which keep exact zeros (x*0, x-x, exact "
             "cancellation) on the native path instead of recomputing them "
             "in software (same value and same flags)",
    )
    parser.add_argument(
        "--lean-stack", action="store_true",
        help="guest reads without initializing the destination and LDM without the "
             "address of the spanWords array on the fast path: takes each "
             "load's hot path off the stack (same effect)",
    )
    parser.add_argument(
        "--giant-function-instructions", type=int, default=0,
        help="marks functions with at least N guest instructions with "
             "[[gnu::optimize]] without sched1 or gcse-after-reload (GCC); "
             "0 disables (default)",
    )
    parser.add_argument(
        "--hot-functions", type=Path, default=None,
        help="hot functions that never get that attribute: one "
             "hex entry per line (the rest of the line and '#...' is ignored)",
    )
    parser.add_argument(
        "--sparse-block-entries", action="store_true",
        help="N4: the block budget only at function entries, loop headers "
             "and dispatches, the block-entry hook gate only at the PCs of "
             "--hook-gates (same guest state; BlockLimit exits, the "
             "cooperative thread switches, move; the runtime refuses hooks "
             "elsewhere)",
    )
    parser.add_argument(
        "--hook-gates", type=Path, default=ROOT / HOOK_GATES_NAME,
        help="with --sparse-block-entries: every PC a runtime block-entry "
             "hook can use (default: whole_aot_hook_gates.json, written by "
             "oot3d_block_entry_hook_census)",
    )
    parser.add_argument(
        "--fold-vfp-literals", action="store_true",
        help="VLDR from the read-only text segment ([pc, #imm]) becomes a "
             "constant, like LDR literals: no page-flag check, load or fault "
             "path (same values)",
    )
    parser.add_argument(
        "--trap-unsupported", action="store_true",
        help="an instruction that cannot be lowered becomes a runtime trap (Unsupported flow) instead of "
             "aborting its function; listed in the manifest as trapped_instructions (use for data decoded as code)",
    )
    args = parser.parse_args(argv)
    if args.shards <= 0:
        parser.error("--shards must be positive")
    if not args.program.is_file():
        result = generate_program(["--output", str(args.program)])
        if result != 0:
            return result
    manifest = generate(
        args.program,
        args.selection,
        args.code,
        args.output,
        shard_count=args.shards,
        shard_strategy=args.shard_strategy,
        region_entries=frozenset(args.callback_free_region),
        memory_entries=frozenset(args.memory_region),
        local_state=args.local_state,
        fold_literals=not args.no_literal_folding,
        native_vfp_aarch64=args.native_vfp_aarch64,
        giant_function_instructions=args.giant_function_instructions,
        hot_entries=_read_hot_entries(args.hot_functions),
        promote_registers=args.promote_registers,
        vfp_zero_aware=args.vfp_zero_aware,
        lean_stack=args.lean_stack,
        sparse_block_entries=args.sparse_block_entries,
        hook_gates=load_hook_gates(args.hook_gates) if args.sparse_block_entries else None,
        fold_vfp_literals=args.fold_vfp_literals,
        trap_unsupported=args.trap_unsupported,
    )
    if args.guest_overrides is not None:
        from apply_guest_overrides import apply as apply_overrides, load_manifest
        located = apply_overrides(
            args.output,
            [item["entry"] for item in load_manifest(args.guest_overrides)])
        print(f"Native overrides enabled: {len(located)} function(s)")
    functions = manifest["functions"]
    print(
        f"Whole-function C++ AOT: {len(functions)} function(s), "
        f"{sum(item['instructions'] for item in functions)} instructions, "
        f"{manifest['shard_count']} shard(s)"
    )
    return 0


def _read_hot_entries(path: Path | None) -> frozenset[int]:
    if path is None:
        return frozenset()
    entries = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        token = line.split("#", 1)[0].strip()
        if token:
            entries.add(int(token.split()[0], 16))
    return frozenset(entries)


if __name__ == "__main__":
    raise SystemExit(main())
