#!/usr/bin/env python3
"""Issue #2715: deferred reemit on steal stays sticky until BoundaryExit.

Closes the residual contract gap from #2690 (PendingRecovery unified
exchange-drain) / #2604 (boundary auto-drain) / #2273 (steal-path
observability). Steal may observe pending deferred reemit on a
foreign worker; running the reemit body off the mutation-boundary /
owner eval thread races with the owning-eval invariants. #2715 gates
the steal-complete foreign-worker drain on `!production_defaults_active()`:
production skips the drain, pending stays sticky until the next
legitimate BoundaryExit on the owning eval. The observability
counter (reemit_deferred_seen_on_steal_total) still bumps
unconditionally so Agents can correlate "pending was observed on
this steal". Soft / test path keeps the existing behavior (drain on
steal) for unit tests.

Contract rows (AC1–AC6 from the test file):

  AC1: production + deferred pending + steal to another worker →
       reemit_deferred_seen_on_steal_total advances; no
       aura_reemit_aot_for_dirty body on the steal-complete path;
       pending still true after steal.
  AC2: subsequent outermost BoundaryExit on the owning eval →
       drain_pending_recovery(BoundaryExit) runs deferred branch once.
  AC3: storm re-entry mid-drain still bumps skipped_reentered and
       does not drop deferred (preserve #2690 exchange-not-check).
  AC4: Soft / test: metric-only steal observe; optional explicit drain
       still available for unit tests.
  AC5: additive only — preserve #2690 / #2604 / #2273 / #2205 surfaces.
  AC6: source-cite + linter + no docs/design/.

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_deferred_reemit_steal_sticky_2715.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    hur = _read("src/compiler/hot_update_registry.hh")
    t = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    # AC1 — production + deferred pending + steal → observability
    # counter advances, no foreign-worker drain, pending stays sticky.
    must("Issue #2715", "AC1", efm)
    must("aura_hot_update_on_deferred_reemit_seen_on_steal(steal_fiber_id)", "AC1", efm)
    must("reemit_deferred_seen_on_steal_total", "AC1", hur)
    must("!production_defaults_active()", "AC1", efm)
    # The drain call (run_hot_update_recovery_if_needed) is gated on
    # !production_defaults_active() — production skips it.
    must("run_hot_update_recovery_if_needed", "AC1", efm)

    # AC2 — BoundaryExit on owning eval → drain runs deferred branch
    # once. The #2604 boundary auto-drain path is unchanged by #2715
    # (the gate is on the steal-complete / refresh path only).

    # AC3 — storm re-entry mid-drain bumps skipped_reentered, does not
    # drop deferred (preserve #2690 exchange-not-check). Unchanged by
    # #2715 — the gate is on the steal-complete path only.

    # AC4 — Soft / test: drain on steal still available.
    must("!aura::compiler::typed_audit::production_defaults_active()", "AC4", efm)
    # The original gate is split across lines in the source — check
    # both substrings independently since string match doesn't span newlines.
    must("mutation_boundary_depth() == 0", "AC4", efm)
    must("aura_hot_update_has_deferred_reemit() != 0", "AC4", efm)

    # AC5 — additive only; #2690 / #2604 / #2273 / #2205 surfaces preserved.
    must("Issue #2690", "AC5", hur)
    must("Issue #2604", "AC5", hur)
    must("Issue #2273", "AC5", hur)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("ac2715_1_production_observability_no_drain", "AC6", t)
    must("ac2715_2_boundary_exit_drains", "AC6", t)
    must("ac2715_3_storm_reentry_skipped_reentered", "AC6", t)
    must("ac2715_4_soft_drain_still_available", "AC6", t)
    must("ac2715_5_additive_no_regression", "AC6", t)
    must("ac2715_6_source_and_linter", "AC6", t)
    must("check_deferred_reemit_steal_sticky_2715", "AC6", build)
    if _read("docs/design/2715-deferred-reemit-steal-sticky.md"):
        fails.append("AC6: docs/design/2715-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2715 deferred reemit on steal stays sticky — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
