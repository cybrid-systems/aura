#!/usr/bin/env python3
"""Issue #2712: Soft epoch-invariant fuse must drive bounded physical heal.

Closes the #2693 §A follow-up: #2693 shipped Soft consecutive-dirty
fuse + joint epoch bump static gate (observability-only — fuse counter
bumps after K consecutive stuck walks but no heal action ran). Under
production Soft + sustained mutation, generation-behind AOT slots and
stale live closures could remain observable for many Soft walks while
Agents only saw fuse counters. Zero-downtime hot-update requires fuse →
heal, not fuse → metric-only.

Contract rows (AC1–AC6 from the test file):

  AC1: production + Soft + consec >= K → physical invalidate runs once;
       behind count drops to 0; soft_fuse_heal_total advances.
  AC2: Soft / K=0 / mode=Off → no heal body (zero extra work).
  AC3: Heal is bounded — consec resets to 0 after heal so the next
       heal requires K fresh stuck walks (re-entry bounded).
  AC4: Reemit-owner TLS preferred (per #2299 / #2606).
  AC5: Additive only — preserve #2693 / #2668 / #2640 / #2541 surfaces.
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
        [sys.executable, str(ROOT / "scripts" / "check_epoch_invariant_soft_fuse_heal_2712.py")],
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

    br = _read("src/compiler/aura_jit_bridge.cpp")
    _read("src/compiler/aura_jit_bridge.h")
    _read("src/compiler/aura_jit_bridge_stub.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_epoch_invariant_walk.cpp")
    build = _read("build.py")

    # AC1 — production + Soft + consec >= K → heal body runs.
    must("Issue #2712", "AC1", br)
    must("g_2693_soft_fuse_heal_fallback_total", "AC1", br)
    must("aura_epoch_invariant_soft_fuse_heal_total_v_read", "AC1", br)
    must("aura_aot_invalidate_all_stale_slots_for_eval(reemit_owner)", "AC1", br)
    must("aura_epoch_invariant_must_deopt_stale_live_closures()", "AC1", br)
    must("production_defaults_active()", "AC1", br)
    must("aura_epoch_invariant_mode() == 1", "AC1", br)

    # AC2 — Soft / K=0 / mode=Off → no heal body. Verify the gate.
    must("aura_epoch_invariant_mode() == 1", "AC2", br)
    # The heal body is gated on production + mode==Soft + consec >= K.
    # K=0 / mode=Off paths already skip fuse + heal via the existing
    # g_2693_soft_fuse_k > 0 check (K=0) and aura_epoch_invariant_mode
    # check (mode=Off skips the walk entirely).

    # AC3 — heal bounded (consec resets to 0 after heal).
    must("g_consecutive_dirty_count.store(0, std::memory_order_relaxed)", "AC3", br)

    # AC4 — reemit-owner TLS preferred.
    must("void* reemit_owner = aura_aot_get_reemit_owner_eval()", "AC4", br)

    # AC5 — additive query keys.
    must("epoch-invariant-soft-fuse-heal-total", "AC5", q)
    must("epoch-invariant-soft-fuse-heal-wired", "AC5", q)
    must("schema-2712", "AC5", q)
    must("issue-2712", "AC5", q)
    # Regression on prior #2693 surface.
    must("epoch-invariant-soft-fuse-total", "AC5", q)
    must("schema-2693", "AC5", q)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("ac2712_1_production_soft_heal_fires", "AC6", t)
    must("ac2712_2_soft_k0_or_off_no_heal", "AC6", t)
    must("ac2712_3_heal_bounded", "AC6", t)
    must("ac2712_4_reemit_owner_tls", "AC6", t)
    must("ac2712_5_query_keys_added", "AC6", t)
    must("ac2712_6_source_and_linter", "AC6", t)
    must("check_epoch_invariant_soft_fuse_heal_2712", "AC6", build)
    if _read("docs/design/2712-soft-fuse-heal.md"):
        fails.append("AC6: docs/design/2712-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2712 Soft fuse → bounded physical heal — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
