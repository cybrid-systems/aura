#!/usr/bin/env python3
"""Issue #4159 source-cite gate: P0 chaos residual-zero hard gate not
mandatory on production concurrency Ready (machine-checkable continuous
deploy AND).

Residual: steal residual-zero + sticky fail-closed exist
(#3134/#3162/#3195 — steal_safety_production_residual_zero_v_read + the
sticky bit), but the soak that REQUIRES the residual arms to stay zero
under the hard gate was RELEASE-only / env-gated via
AURA_CHAOS_SOAK_HARD_GATE=1 (build.py cmd_chaos_soak_hard_gate_2722,
#2722/#2755). The coverage linter proved command PRESENCE, not a
continuous deploy AND — a deploy could ship a Soft-green soak while the
named residuals (rearm_race / lifetime_proof / envframe_lag) grew
between RELEASE runs: missing machine-checkable continuous proof of the
I3/I6 arms (not Soft-as-vuln).

Fix contract (existing mechanisms only — no new residual bus):
  - steal_safety.h: additive deploy-AND accessor
    steal_safety_production_concurrency_ready_gate_v_read() AND-s
    (1) the #3195 multi-worker Ready latch
    (aura_runtime_require_production_multi_worker success),
    (2) the RELEASE SOAK hard-gate arm (AURA_CHAOS_SOAK_HARD_GATE=1 —
    the env cmd_chaos_soak_hard_gate_2722 sets),
    (3) steal_safety_production_residual_zero_v_read() == 1,
    (4) sticky-fail == 0 (read after the poll so recovery wipes it).
    Soft (unlatched) returns 0 even with the soak armed — a Soft-green
    soak can never satisfy the deploy AND.
  - build.py: cmd_production_concurrency_ready_gate_4159 = the deploy
    gate AND: static rows → cmd_chaos_soak_hard_gate_2722() (the soak
    term) → the residual-zero runtime face
    (test_steal_safety_production_residual_zero) under the armed env.
    Registered in the command table.
  - .github/workflows/release.yml: the deploy step runs the 4159 gate
    AFTER the 2722 coverage row and BEFORE release-asset upload.
  - Soft/PR soak remains non-gating (#2722 AC5, #2755 AC2 unchanged);
    #3162/#3195 sticky faces and the #4158 edge-free hold contract are
    not weakened.

ACs:
  AC1  steal_safety.h deploy-AND accessor wired: cites #4159 in the
       accessor window, AND-s latch + soak arm + residual-zero SSOT +
       sticky-clear, wired sentinel + issue stamp present, <cstdlib>
       included; no new residual counters (g_4159_*) and no new query
       key (schema-4159) on the touched surfaces.
  AC2  build.py deploy gate AND: cmd_production_concurrency_ready_
       gate_4159 defined, invokes cmd_chaos_soak_hard_gate_2722() (the
       soak term), arms AURA_CHAOS_SOAK_HARD_GATE=1 +
       AURA_PRODUCTION_CONCURRENCY_GATE=1 and runs the residual-zero
       face binary; registered in the command table.
  AC3  Continuous deploy wiring: release.yml runs
       production-concurrency-ready-gate-4159 after the 2722 coverage
       row and before the release-asset upload; the linter is wired in
       build.py cmd_lint and listed in
       scripts/coverage/root_check_allowlist.txt (every gate run proves
       the AND statically — continuously, not only at RELEASE).
  AC4  No weakening / no invent: harness residual_zero_gate composition
       intact (chaos_soak_hard_gate() || prod_gate), #2722/#2755
       linters + sticky accessor + Soft pass-through intact, no
       tests/**/test_issue_4159.cpp (per #81934), no docs/design/4159-*
       (per #1655).

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
--self-test runs the rows against the real tree (must be clean) AND
against a broken synthetic (must be detected) to prove the checker
fails when the contract is violated.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "serve" / "steal_safety.h"
BUILD = ROOT / "build.py"
RELEASE = ROOT / ".github" / "workflows" / "release.yml"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"
CHAOS = ROOT / "tests" / "serve" / "test_chaos_mutate_steal_gc_mailbox.cpp"
RESIDUAL = ROOT / "tests" / "serve" / "test_steal_safety_production_residual_zero.cpp"

ACCESSOR = "steal_safety_production_concurrency_ready_gate_v_read"
GATE_CMD = "cmd_production_concurrency_ready_gate_4159"


def run_rows(texts: dict) -> list[str]:
    failures: list[str] = []

    def must(cond: bool, row: str) -> None:
        if not cond:
            failures.append(row)

    hdr = texts["hdr"]
    build = texts["build"]
    release = texts["release"]
    allow = texts["allow"]
    chaos = texts["chaos"]
    residual = texts["residual"]

    # ── AC1: deploy-AND accessor in steal_safety.h ──
    pos = hdr.find(ACCESSOR)
    must(pos >= 0, "AC1: deploy-AND accessor declared in steal_safety.h")
    if pos >= 0:
        win = hdr[max(0, pos - 1500) : pos + 1500]
        must("Issue #4159" in win, "AC1: accessor window cites #4159")
        must("AURA_CHAOS_SOAK_HARD_GATE" in win, "AC1: soak-arm term (cmd_chaos_soak_hard_gate_2722 env)")
        must(
            "aura_runtime_multi_worker_production_latched() != 0" in win,
            "AC1: Ready-latch term (require_production_multi_worker success)",
        )
        must(
            "steal_safety_production_residual_zero_v_read() == 1" in win,
            "AC1: residual-zero SSOT term (#3134/#3195)",
        )
        must(
            "g_steal_safety_production_residual_sticky_fail.load" in win,
            "AC1: sticky-clear term (#3162), read after the poll",
        )
    must(
        "g_steal_safety_production_concurrency_ready_gate_wired{1}" in hdr,
        "AC1: wired sentinel present",
    )
    must(
        "kStealSafetyProductionConcurrencyReadyGateIssue = 4159" in hdr,
        "AC1: issue stamp present",
    )
    must("#include <cstdlib>" in hdr, "AC1: <cstdlib> included for the soak-arm getenv")
    must(
        "!multi && aura_production_defaults_active_probe() == 0" in hdr,
        "AC1: #3134/#3195 Soft pass-through intact (no weakening)",
    )
    must(
        "steal_safety_production_residual_sticky_fail_v_read" in hdr,
        "AC1: #3162 sticky accessor intact (no weakening)",
    )

    # ── AC2: build.py deploy gate AND ──
    gpos = build.find(f"def {GATE_CMD}(")
    must(gpos >= 0, "AC2: deploy gate command defined in build.py")
    if gpos >= 0:
        gwin = build[gpos : gpos + 6000]
        must("Issue #4159" in gwin, "AC2: deploy gate cites #4159")
        must("cmd_chaos_soak_hard_gate_2722()" in gwin, "AC2: soak term — runs the 2722 hard gate")
        must("test_steal_safety_production_residual_zero" in gwin, "AC2: residual-zero runtime face run")
        must('env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"' in gwin, "AC2: deploy run arms the soak env")
        must(
            'env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"' in gwin,
            "AC2: deploy run arms the production concurrency env",
        )
    must(
        '"production-concurrency-ready-gate-4159": ' + GATE_CMD + "," in build,
        "AC2: deploy gate registered in the command table",
    )

    # ── AC3: continuous deploy wiring ──
    r2722 = release.find("chaos-soak-hard-gate-2722-coverage")
    r4159 = release.find("production-concurrency-ready-gate-4159")
    rupload = release.find("softprops/action-gh-release")
    must(r4159 >= 0, "AC3: release.yml runs the 4159 deploy gate")
    must(r2722 >= 0 and r4159 > r2722, "AC3: 4159 gate runs after the 2722 coverage row")
    must(rupload >= 0 and r4159 < rupload, "AC3: 4159 gate runs before release-asset upload")
    must("check_chaos_ready_residual_gate_4159" in build, "AC3: build.py wires the linter")
    must("check_chaos_ready_residual_gate_4159.py" in allow, "AC3: root_check_allowlist row present")

    # ── AC4: no weakening / no invent ──
    must(
        "residual_zero_gate = chaos_soak_hard_gate() || prod_gate" in chaos,
        "AC4: harness residual_zero_gate composition intact (#2755 AC2)",
    )
    must(
        (ROOT / "scripts" / "coverage" / "checks" / "check_chaos_soak_hard_gate_2722.py").is_file(),
        "AC4: #2722 coverage linter intact",
    )
    must(
        (ROOT / "scripts" / "coverage" / "checks" / "check_chaos_soak_residual_zero_2755.py").is_file(),
        "AC4: #2755 coverage linter intact",
    )
    must("Issue #4159" in residual, "AC4: residual-zero face extends with #4159 ACs")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4159.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4159.cpp (per #81934)",
    )
    must(
        not glob.glob(str(ROOT / "docs" / "design" / "4159-*")),
        "AC4: no docs/design/4159-* (per #1655)",
    )
    for name, t in (("steal_safety.h", hdr), ("build.py", build)):
        must("g_4159_" not in t, f"AC4: no new counters in {name}")
        must("schema-4159" not in t, f"AC4: no new query key in {name}")

    return failures


def main() -> int:
    texts = {
        "hdr": HEADER.read_text(),
        "build": BUILD.read_text(),
        "release": RELEASE.read_text(),
        "allow": ALLOW.read_text(),
        "chaos": CHAOS.read_text(),
        "residual": RESIDUAL.read_text(),
    }
    if "--self-test" in sys.argv[1:]:
        real = run_rows(texts)
        broken = dict(texts)
        # Break one row per surface: drop the accessor cite (steal_safety.h)
        # and the release.yml deploy step (continuous wiring).
        broken["hdr"] = texts["hdr"].replace("Issue #4159", "Issue #XXXX", 1)
        broken["release"] = texts["release"].replace("production-concurrency-ready-gate-4159", "", 1)
        failed = run_rows(broken)
        ok = not real and bool(failed)
        print(
            f"check_chaos_ready_residual_gate_4159 self-test: real-tree rows clean={not real}, "
            f"broken-detection rows={len(failed)}"
        )
        if not failed:
            print("  self-test FAILED: checker did not detect the broken synthetic")
        for row in real:
            print(f"  real-tree FAIL: {row}")
        return 0 if ok else 1
    failures = run_rows(texts)
    if failures:
        print(f"\ncheck_chaos_ready_residual_gate_4159: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_chaos_ready_residual_gate_4159: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
