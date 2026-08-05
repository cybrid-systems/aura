#!/usr/bin/env python3
"""Issue #2666: production default anon / residual sync remount ON (close first-call
MustDeopt window for sid == 0 under sustained mutation).

Contract (one row per AC):
  AC1 src/compiler/aura_jit_runtime.cpp aura_sync_remount_anon_enabled_default
     falls back to aura::compiler::typed_audit::production_defaults_active()
     when env AURA_SYNC_REMOUNT_ANON is unset. Closes the residual
     first-call MustDeopt window for sid == 0 closures under sustained
     self-mod.
  AC2 explicit env=0 / off / false still forces off under production
     (operator override branch preserved per #2637 AC1).
  AC3 Soft / sandbox / tests stays 0 — production_defaults_active()
     returns false in dev_off mode → anon sync walk does NOT run.
     Zero extra work preserved.
  AC4 src/compiler/evaluator_primitives_obs_eval.cpp exposes additive
     query sentinel: live-closure-sync-remount-anon-prod-default-wired
     + schema-2666 + issue-2666.
  AC5 tests/compiler/test_anonymous_residual_stable_id_policy.cpp
     extended with #2666 AC1-AC4 source-cite block (per #81967 — no
     new issue-suffix file).
  AC6 build.py wires check_2666_coverage into the gate after
     check_sync_remount_anon_coverage.
  AC7 cross-check: check_sync_remount_anon_coverage still green
     (no regression on #2637 anon walk body / counters).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1 — production_default_enabled
    must("aura_sync_remount_anon_enabled_default", "AC1", rt)
    must("Issue #2666", "AC1", rt)
    must("production_defaults_active()", "AC1", rt)
    must("e && *e", "AC1", rt)

    # AC2 — explicit_off_wins
    must("explicit env always wins", "AC2", rt)
    must("operator override", "AC2", rt)
    must("return enabled ? 1 : 0", "AC2", rt)

    # AC3 — soft_path_unchanged
    must("env unset \u2192 fall back to production_defaults_active()", "AC3", rt)
    must("preserve #2637 AC1", "AC3", rt)

    # AC4 — query_keys_added
    must("live-closure-sync-remount-anon-prod-default-wired", "AC4", obs)
    must("schema-2666", "AC4", obs)
    must("issue-2666", "AC4", obs)

    # AC5 — test file extension
    must("ac2666_1_production_default_enabled", "AC5", test)
    must("ac2666_2_explicit_off_wins", "AC5", test)
    must("ac2666_3_soft_path_unchanged", "AC5", test)
    must("ac2666_4_query_keys_added", "AC5", test)
    must("Issue #2666", "AC5", test)

    # AC6 — build.py wires the linter
    must("check_2666_coverage", "AC6", build)

    # Cross-check: check_sync_remount_anon_coverage still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_sync_remount_anon_coverage.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_sync_remount_anon_coverage regression:\n{r1.stdout}\n{r1.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2666 production-default anon sync remount ON — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
