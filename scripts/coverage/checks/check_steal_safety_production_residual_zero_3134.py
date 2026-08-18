#!/usr/bin/env python3
"""Issue #3134: P0 residual — production-readiness residual-zero check
fail-closed under production multi-worker (chaos soak gate).

Closes the silent-corruption window where steal_safety_transaction's
residual hard-AND rejects could leave non-zero counters under production
without failing the production-readiness gate.

Contract:
  AC1 steal_safety.h + evaluator_primitives_query_type_stats.cpp —
     steal_safety_production_residual_zero_v_read() returns 1 iff
     production_defaults_active() is 0 (Soft) OR both named residual
     counters (#2901/#3038 + #2957) are 0. Wired into schema-3073
     production-readiness-soak-gate-wired + new additive
     production-readiness-steal-residual-zero + schema-3134 / issue-3134.
  AC2 Soft / sandbox=off / single-worker: zero behavioural change —
     probe returns 0 → check returns 1 (pass-through). steal_safety_
     transaction's quiet Ok path unchanged.
  AC3 Quiet happy path (no residual, no densify, no concurrent decision):
     no extra atomics beyond the existing hard-AND loads.
  AC4 Additive only — reuses g_steal_safety_residual_rearm_race_total +
     g_steal_safety_residual_lifetime_proof_reject_total +
     g_steal_safety_last_reject_invariant_bits. ONE additive readiness
     key (production-readiness-steal-residual-zero) + schema-3134 /
     issue-3134 (per AC4 acceptable).
  AC5 Regression test in tests/serve/ (src/-aligned per #81967). No
     tests/issues/test_issue_3134.cpp. No docs/design/3134-* (#1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    sh = _read("src/serve/steal_safety.h")
    cpp = _read("src/serve/steal_safety.cpp")
    qts = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    test = _read("tests/serve/test_steal_safety_production_residual_zero.cpp")

    # ── AC1: source shape ──
    check_pos = sh.find("steal_safety_production_residual_zero_v_read()")
    if check_pos == -1:
        fails.append("AC1: steal_safety_production_residual_zero_v_read not declared")
    else:
        # Anchor backwards to include the comment block (issue stamp +
        # documentation), and forwards to include the inline impl.
        check_start = max(0, check_pos - 1500)
        check_end = check_pos + 1500
        check_win = sh[check_start:check_end]
        must("Issue #3134", "AC1 declaration cites #3134", check_win)
        must("g_steal_safety_residual_rearm_race_total", "AC1 consults #2901/#3038 re-arm race counter", check_win)
        must(
            "g_steal_safety_residual_lifetime_proof_reject_total",
            "AC1 consults #2957 LifetimeProof residual",
            check_win,
        )
        must("aura_production_defaults_active_probe", "AC1 consults production probe (Soft pass-through)", check_win)
        must("kStealSafetyProductionResidualZeroIssue = 3134", "AC1 issue stamp constant", check_win)
        # Wired sentinel (atomic uint32, not uint64).
        must("g_steal_safety_production_residual_zero_wired", "AC1 wired sentinel declared", check_win)

    must("steal_safety_production_residual_zero_v_read", "AC1 query primitive consults the new check", qts)
    # The string literal is split across two lines by clang-format
    # (adjacent string literal concatenation produces the joined key
    # "production-readiness-steal-residual-zero" at runtime). Match
    # the source-visible prefix that IS continuous in the source.
    must('"production-readiness-steal-"', "AC1 additive readiness key inserted (split-string prefix)", qts)
    must('"residual-zero"', "AC1 additive readiness key inserted (split-string suffix)", qts)
    must("schema-3134", "AC1 schema-3134 key", qts)
    must("issue-3134", "AC1 issue-3134 key", qts)
    must("Issue #3134", "AC1 query primitive cites #3134", qts)

    # ── AC2: Soft pass-through; hot path unchanged ──
    if check_pos != -1:
        must("aura_production_defaults_active_probe() == 0", "AC2 Soft pass-through guard in check", check_win)
        must("return 1", "AC2 Soft returns 1 (pass-through)", check_win)
    # steal_safety_transaction's quiet Ok path must NOT call the readiness
    # check (zero extra atomics / zero extra control flow).
    fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(")
    if fn_pos == -1:
        fails.append("AC2: steal_safety_transaction body missing")
    else:
        fn_end = cpp.find("\n}\n", fn_pos)
        if fn_end == -1 or fn_end > fn_pos + 6000:
            fn_end = fn_pos + 6000
        fn_win = cpp[fn_pos:fn_end]
        if "production_residual_zero_v_read" in fn_win:
            fails.append(
                "AC2: steal_safety_transaction body calls the readiness check "
                "(breaks Soft contract + zero extra atomics)"
            )
        must("evaluate_residual_hard_and_bits", "AC2 hot path still uses existing hard-AND", fn_win)

    # ── AC3: quiet happy path — no extra atomics ──
    # Implicit (covered by AC2 — the check is consulted once per query,
    # not per steal). Verify the check is in steal_safety.h (header) and
    # not in steal_safety.cpp hot path.
    if check_pos == -1:
        fails.append("AC3: check function missing in steal_safety.h")

    # ── AC4: additive only; existing counters + ONE additive readiness key ──
    must("g_steal_safety_residual_rearm_race_total", "AC4 reuses existing #2901/#3038 counter", sh)
    must("g_steal_safety_residual_lifetime_proof_reject_total", "AC4 reuses existing #2957 counter", sh)
    must("g_steal_safety_last_reject_invariant_bits", "AC4 reuses existing last_reject_invariant_bits", sh)
    # Wired sentinel is atomic<uint32_t>, NOT a new uint64 residual
    # counter (additive only, no new metric keys).
    sentinel_pos = sh.find("g_steal_safety_production_residual_zero_wired")
    if sentinel_pos != -1:
        sentinel_end = sentinel_pos + 400
        sentinel_win = sh[sentinel_pos:sentinel_end]
        if "std::atomic<std::uint64_t>" in sentinel_win:
            fails.append(
                "AC4: new uint64 atomic in production-residual-zero sentinel (must be a 1-bit wired sentinel only)"
            )

    # ── AC5: src-aligned test, no tests/issues/test_issue_3134.cpp, no plan doc ──
    must("Issue #3134", "AC5 regression test cites", test)
    must("steal_safety_production_residual_zero_v_read", "AC5 regression test asserts the check", test)
    if (ROOT / "tests" / "issues" / "test_issue_3134.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3134.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "serve" / "test_issue_3134.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3134.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3134-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    # Coverage wiring.
    must(
        "check_steal_safety_production_residual_zero_3134",
        "AC6 build.py wiring",
        _read("build.py") + _read("pyproject.toml"),
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3134 production-readiness residual-zero check — all 5 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
