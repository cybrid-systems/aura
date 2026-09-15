#!/usr/bin/env python3
"""Issue #3813: steal checkpoint probe dual-track (C-bridge vs table)."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    br = _read("src/compiler/aura_jit_bridge.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_aot_bridge_checkpoint_version_steal.cpp")
    build = _read("build.py")

    # AC1: probe does not conflate C-bridge with table
    must("Issue #3813", "AC1 probe cite", br)
    must("aura_get_current_bridge_epoch()", "AC1 C-bridge sample", br)
    must("cur_c_bridge", "AC1 C-bridge local", br)
    must("table_mismatch", "AC1 independent table", br)
    must("c_bridge_mismatch", "AC1 C-bridge mismatch", br)
    # Forbid the old conflated compare shape near the probe.
    probe = br.find("aura_aot_probe_checkpoint_version")
    if probe == -1:
        fails.append("AC1: probe not found")
    else:
        win = br[probe : probe + 1200]
        if "bridge_epoch != table_epoch" in win or "bridge_epoch != cur_table" in win:
            fails.append("AC1: still compares bridge_epoch to table epoch")
        if "aura_get_current_bridge_epoch()" not in win:
            fails.append("AC1: probe window missing C-bridge load")
        if "table_mismatch" not in win:
            fails.append("AC1: probe window missing table_mismatch")

    must("table_epoch = 0", "AC1 header default arg", hdr)
    must("Issue #3813", "AC1 header cite", hdr)

    # AC2: callers pass independent table sample
    must("Issue #3813", "AC2 fiber cite", fib)
    must("aura_aot_func_table_epoch()", "AC2 table sample at callers", fib)
    if fib.count("aura_aot_probe_checkpoint_version(") < 4:
        fails.append("AC2: expected >=4 probe call sites")

    must("table_epoch", "AC2 stub 3-arg", stub)

    # AC3: aura_is_jit_closure_fresh semantics unchanged (dual-fresh retained)
    fresh = br.find("extern \"C\" bool aura_is_jit_closure_fresh")
    if fresh == -1:
        fails.append("AC3: aura_is_jit_closure_fresh missing")
    else:
        # Comment block with #3447 sits immediately above the definition.
        fwin = br[max(0, fresh - 1600) : fresh + 400]
        must("Issue #3447", "AC3 dual-fresh cite retained", fwin)
        must("domain_ok", "AC3 domain_ok retained", fwin + br[fresh : fresh + 1200])

    # AC4: tests
    for tag in ("ac3813_1", "ac3813_2", "ac3813_3", "ac3813_4"):
        must(tag, "AC4 tests", test)
    must("owner-scoped", "AC4 owner-scoped", test)
    must("table-domain stale", "AC4 table stale", test)
    # Forbid conflated oracle (table_epoch+99 as sole 2-arg "bridge" mismatch)
    if "aura_aot_probe_checkpoint_version(0, table_epoch + 99)" in test:
        fails.append("AC4: conflated 2-arg table_epoch+99 oracle still present")

    must("check_steal_checkpoint_dual_track_3813", "AC4 build", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3813.cpp").is_file():
        fails.append("AC4: invent test_issue_3813.cpp present")
    if (ROOT / "docs" / "design" / "3813-steal-checkpoint-dual-track.md").is_file():
        fails.append("AC4: invent docs/design present")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK  AC1_probe_dual_track_no_conflation")
    print("OK  AC2_callers_independent_table_sample")
    print("OK  AC3_jit_closure_fresh_unchanged")
    print("OK  AC4_tests_and_wiring")
    print(
        "\nOK: Issue #3813 steal checkpoint probe dual-track "
        "-- C-bridge vs C-bridge AND table vs table; no C!=table false steal deopt"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
