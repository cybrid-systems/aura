#!/usr/bin/env python3
"""Issue #2755: chaos SOAK hard gate must zero residual steal-safety counters
(extend #2722).

Contract (one row per AC):
  AC1 cmd_chaos_soak_hard_gate_2722 under AURA_PRODUCTION_CONCURRENCY_GATE=1
     + Hard reads residual counters at end-of-run and fails if any > 0.
     Chaos harness run_chaos_pass asserts residual hard-AND deltas == 0
     when chaos_soak_hard_gate() || production_concurrency_gate().
  AC2 Soft / local iteration paths remain non-gating (residual_zero_gate
     only under SOAK hard gate / production concurrency; Soft print-only).
  AC3 Counter list documented in the gate function docstring + this
     linter + chaos harness comments (four residual + related surfaces).
  AC4 Existing #2722 ACs preserved (duration / fiber count / production
     envelope / ordering before softprops/action-gh-release). Additive.
  AC5 Source-cite + linter wired in build.py; no docs/design/2755-*.
     Extend tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    build = _read("build.py")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    hdr = _read("src/serve/steal_safety.h")
    release = _read(".github/workflows/release.yml")

    # AC1 — residual-zero assert under hard gate / production concurrency.
    must("Issue #2755", "AC1", chaos)
    must("chaos_soak_hard_gate", "AC1", chaos)
    must("residual_zero_gate", "AC1", chaos)
    must("steal_safety_residual_boundary_unsafe_total_v_read", "AC1", chaos)
    must("steal_safety_residual_layout_stamp_mismatch_total_v_read", "AC1", chaos)
    must("steal_safety_residual_ticket_mismatch_total_v_read", "AC1", chaos)
    must("steal_safety_residual_gc_defer_armed_total_v_read", "AC1", chaos)
    must("#2755: residual_boundary_unsafe delta == 0", "AC1", chaos)
    must("#2755: residual_layout_stamp_mismatch delta == 0", "AC1", chaos)
    must("#2755: residual_ticket_mismatch delta == 0", "AC1", chaos)
    must("#2755: residual_gc_defer_armed delta == 0", "AC1", chaos)
    # Gate function still runs under production concurrency + SOAK hard gate.
    must('env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"', "AC1", build)
    must('env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"', "AC1", build)
    must("cmd_chaos_soak_residual_zero_2755_coverage", "AC1", build)

    # AC2 — Soft / local non-gating.
    must("Soft / local", "AC2", chaos)
    must("non-gating", "AC2", chaos)
    must("residual_zero_gate = chaos_soak_hard_gate() || prod_gate", "AC2", chaos)
    # Soft steal still popped under hard gate (preserve #2722 AC5).
    must('env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)', "AC2", build)

    # AC3 — counter list documented in gate docstring + linter + harness.
    for c in (
        "g_steal_safety_residual_boundary_unsafe_total",
        "g_steal_safety_residual_layout_stamp_mismatch_total",
        "g_steal_safety_residual_ticket_mismatch_total",
        "g_steal_safety_residual_gc_defer_armed_total",
    ):
        must(c, "AC3 build docstring", build)
        must(c, "AC3 chaos harness", chaos)
        must(c, "AC3 steal_safety.h", hdr)
    # Related surfaces (force_deopt + resume_hard hard-zero; layout_resume observe).
    must("layout_stamp_resume_mismatch", "AC3", chaos)
    must("steal_snapshot_mismatch_force_deopt", "AC3", chaos)
    must("resume_hard_fail", "AC3", chaos)
    must("#2755: steal_snapshot_mismatch_force_deopt delta == 0", "AC3 hard", chaos)
    must("#2755: resume_hard_fail delta == 0", "AC3 hard", chaos)
    must("layout_stamp_resume_mismatch_total", "AC3", build)
    must("steal_snapshot_mismatch_force_deopt_total", "AC3", build)
    must("resume_hard_fail_total", "AC3", build)
    must("observe-only", "AC3 layout_resume observe", chaos)

    # AC4 — #2722 surfaces preserved (additive).
    must("def cmd_chaos_soak_hard_gate_2722(", "AC4", build)
    must("workers  : 8", "AC4", build)
    must("fibers   : 256", "AC4", build)
    must("duration : 300s", "AC4", build)
    must("chaos-soak-hard-gate-2722", "AC4", release)
    upload = release.find("softprops/action-gh-release")
    gate = release.find("chaos-soak-hard-gate-2722")
    if gate == -1 or upload == -1:
        fails.append("AC4: release.yml missing gate or upload step")
    elif gate >= upload:
        fails.append("AC4: gate must PRECEDE softprops/action-gh-release")
    must("check_chaos_soak_hard_gate_2722", "AC4", build)

    # AC5 — source-cite + test extension + linter wire-up + no docs/design.
    must("ac2755_1_residual_zero_under_hard_gate", "AC5", chaos)
    must("ac2755_2_soft_non_gating", "AC5", chaos)
    must("ac2755_3_counter_list_documented", "AC5", chaos)
    must("ac2755_4_2722_preserved", "AC5", chaos)
    must("ac2755_5_source_and_linter", "AC5", chaos)
    must("check_chaos_soak_residual_zero_2755", "AC5", build)
    must("or cmd_chaos_soak_residual_zero_2755_coverage()", "AC5", build)
    must('"chaos-soak-residual-zero-2755-coverage": cmd_chaos_soak_residual_zero_2755_coverage,', "AC5", build)
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2755-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2755.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_2755.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2755 chaos SOAK residual-zero — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
