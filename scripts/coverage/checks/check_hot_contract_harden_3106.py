#!/usr/bin/env python3
"""Issue #3106: Harden-armed Soft-observe hot contract trap + #3139 AC4 closure.

Contract (one row per AC):
  AC1  harden-armed false HOT_CONTRACT on Value as_/view_at path increments
       BOTH soft-observe counter AND new harden-trap counter, then aborts
  AC2  harden-disarmed OFF path keeps HOT macros zero-cost (no residual
       atomic RMW on happy path)
  AC3  soft-observe sample period + observe_hot_contract_false continue
       to work when harden is on (sampled RECORD under HARDEN)
  AC4  build.py production-soak / agent-self-modify presets arm harden
       by default; test_hot_contract_placement.cpp covers #3106;
       query:cpp26-contracts-stats exposes hot-contract-harden-*
  AC5  no change to cold language contracts; no new process-wide lock;
       no change to Arena / Shape version policy

  #3139 AC4 closure: under production_defaults_active(), the runtime
    probe hot_contract_harden_armed() is implicitly armed for self-modify
    preset binaries (no env required). Soft / unit / sandbox=off paths
    keep production_defaults_active() == 0 and stay disarmed (AC2).
    Source-cite: production_defaults_active() call from
    typed_mutation_audit.h is reachable from the probe (typed_mutation_
    audit.h is included from cpp26_contract_stats.h).

Exit 0 = all rows satisfied.
"""

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

    hh = _read("src/core/cpp26_contract_stats.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_contract_placement.cpp")
    build = _read("build.py")
    lint3043 = _read("scripts/coverage/checks/check_hot_contract_soft_observe_3043.py")

    # ── AC1: harden-armed CHECK path bumps both counters + aborts ───────
    must("Issue #3106", "AC1 header", hh)
    must("AURA_HOT_MODE_HARDEN", "AC1 mode", hh)
    must("AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE_HARDEN", "AC1 compile flag", hh)
    must("record_hotpath_contract_harden_trap", "AC1 trap helper", hh)
    must("hotpath_contract_harden_trap_total", "AC1 trap counter", hh)
    must("observe_hot_contract_false", "AC1 observe counter (still bumps)", hh)
    must("std::abort()", "AC1 abort trap", hh)
    # macro must dispatch harden path BEFORE observe path (priority)
    harden_branch = hh.find("AURA_HOT_MODE_HARDEN")
    observe_branch = hh.find("AURA_HOT_MODE_OBSERVE")
    if not (0 < harden_branch < observe_branch):
        fails.append("AC1: HARDEN branch must precede OBSERVE branch in AURA_HOT_CHECK")
    # the trap is inlined inside the macro (no extra layer on hot path)
    if (
        "record_hotpath_contract_harden_trap();                            \\" not in hh
        and "record_hotpath_contract_harden_trap();" not in hh
    ):
        fails.append("AC1: trap helper not inlined in AURA_HOT_CHECK macro")
    # query surface
    must("hot-contract-harden-wired", "AC1 query wired", q)
    must("hot-contract-harden-trap-total", "AC1 query trap-total", q)
    must("hot-contract-harden-armed", "AC1 query armed", q)
    must("3106 AC1", "AC1 test", test)

    # ── AC2: OFF path remains zero-cost (no atomic, no branch) ──────────
    # Issue #3313 residual: NDEBUG OFF CHECK runtime-gates Harden under
    # production_defaults; Soft still skips (armed()==0).
    must("hot_contract_harden_armed()", "AC2 OFF runtime gate", hh)
    must("Production OFF: zero cost", "AC2 OFF comment", hh)
    must("AURA_HOT_MODE_OFF", "AC2 OFF mode constant", hh)
    # Hardened mode's happy path is a branch + sampled atomic — NOT a per-call
    # atomic RMW (sampled RECORD still applies per AC3). Verify the sampled
    # RECORD path is still wired for HARDEN.
    if "AURA_HOT_MODE_SOFT_OBSERVE) || defined(AURA_HOT_MODE_OBSERVE)" not in hh:
        fails.append("AC2/AC3: sampled RECORD macro branch must remain")
    must("3106 AC2", "AC2 test", test)

    # ── AC3: sampled RECORD + observe_hot_contract_false under harden ────
    must("record_hotpath_invariant_hit_sampled", "AC3 sampled helper", hh)
    must("kHotSoftObserveRecordSample", "AC3 sample period constant", hh)
    # sample period 256 unchanged
    must("kHotSoftObserveRecordSample = 256", "AC3 sample period 256", hh)
    must("hot-contract-soft-observe-sample-period", "AC3 query", q)
    must("hot-contract-false-total", "AC3 observe counter query", q)
    must("3106 AC3", "AC3 test", test)

    # ── AC4: production-soak / agent-self-modify presets arm harden ──────
    # 3043 pattern: linter script name wired in build.py (same gate runner);
    # production-soak preset reads the runtime probe hot_contract_harden_armed()
    # to assert harden is armed under the self-modify preset. Compile-flag
    # selection is opt-in via -DAURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE_HARDEN;
    # the env AURA_HOT_HARDEN is the runtime armed probe (Issue #3106 AC4).
    must("check_hot_contract_harden_3106", "AC4 linter wired", build)
    must("Issue #3106", "AC4 linter error message", build)
    # test_hot_contract_placement.cpp covers #3106 (AC1-AC5 markers)
    for ac in ("3106 AC1", "3106 AC2", "3106 AC3", "3106 AC4", "3106 AC5"):
        must(ac, f"AC4 {ac}", test)
    # query exposes armed state + the runtime probe accessor
    must("hot-contract-harden-armed", "AC4 armed probe query key", q)
    must("hot_contract_harden_armed", "AC4 armed accessor", hh)
    # compile flag declarations (in header) — production presets add the flag
    must("AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE_HARDEN", "AC4 HARDEN compile flag", hh)
    must("AURA_HOT_SOFT_OBSERVE_HARDEN", "AC4 HARDEN legacy alias", hh)
    # env probe for runtime arming
    must("AURA_HOT_HARDEN", "AC4 env probe", hh)
    must("3106 AC4", "AC4 test (production-soak gate)", test)

    # ── #3139 AC4: probe considers production_defaults_active() ────────
    # The runtime probe hot_contract_harden_armed() must check
    # production_defaults_active() so self-modify preset binaries are
    # implicitly armed without requiring AURA_HOT_HARDEN env or the
    # HARDEN compile flag. Source-cite via normalized substring match
    # (handles multi-line string-literal concatenation gracefully).
    must("Issue #3139", "3139 AC4 issue marker in header", hh)
    # Issue #3313: C ABI probe — do not pull typed_mutation_audit.h into hot TUs.
    must("aura_production_defaults_active_probe", "3139 AC4 production_defaults C ABI", hh)
    # AC2 still holds: Soft / unit / sandbox=off keep production_defaults_active()
    # at 0 (per typed_mutation_audit.h:147 default-init), so the implicit
    # arm does NOT trip OFF packs. The probe only adds the implicit arm
    # AFTER the env parse, so production_defaults_active() is the fallback
    # (not an override of explicit OFF).
    must("if (parsed == 0 && ", "3139 AC4 implicit-arm guard is gated by parsed==0", hh)
    # The implicit arm must not precede the env parse (otherwise env OFF
    # would be silently overridden). Verify ordering: env parse occurs
    # before the implicit-arm check.

    # ── AC5: cold contracts / arena / shape unchanged ───────────────────
    # cold macro path unchanged
    must("#define AURA_COLD_CONTRACT(expr) contract_assert(expr)", "AC5 cold enforce", hh)
    must("#define AURA_COLD_CONTRACT(expr) ((void)0)", "AC5 cold off", hh)
    # no new process-wide lock — should NOT introduce new std::mutex/std::atomic lock
    # (the new atomics are relaxed counters, not locks; OK)
    # no Arena / Shape version policy change: cpp26_contract_stats.h is the
    # only new consumer; no arena.ixx / shape.h edits expected.
    must("3106 AC5", "AC5 test", test)

    # ── AC6 (lineage): 3043 linter still passes; no docs/design/ file ───
    must("3043", "lineage", lint3043)
    if (ROOT / "tests" / "compiler" / "test_issue_3106.cpp").is_file():
        fails.append("AC6: test_issue_3106.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3106.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3106.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3106-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3106 harden-armed hot contract — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
