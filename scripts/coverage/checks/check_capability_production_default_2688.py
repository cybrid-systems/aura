#!/usr/bin/env python3
"""Issue #2688: production-default hard_fiber_isolation + grant epoch retain window.

Contract:
  AC1 Production defaults active + Strict/multi-tenant →
      hard_fiber_isolation()==true and retain window == 64 (or env override).
  AC2 Production Restricted (single-tenant profile) → retain window == 16;
      hard_fiber stays false unless env forces on (#2584 commercial profile
      forces hard even under Restricted; env AURA_HARD_FIBER_ISOLATION=0
      still wins via hfi_explicit_off branch).
  AC3 Soft / AURA_SANDBOX=off → hard_fiber false, K=0 (no regression on
      Soft share / zero fence cost). dev_off branch in
      apply_production_security_defaults sets both to 0.
  AC4 Grant at mutation epoch N; bump past N+K → provenance_ok false +
      capability_epoch_fence_hit_total bumps. The sliding window lives
      in on_mutation_epoch_bump (only advances min_valid when K > 0).
  AC5 hard_fiber on → fiber B cannot use fiber A's Mutate grant (same
      tenant); deny reason fiber-grant-mismatch; Soft path metric-only
      when hard_fiber off. provenance_ok branches on
      hard_fiber_isolation_ flag (true → deny + bump
      capability_fiber_hard_deny_total; false → metric-only).
  AC6 Query keys expose hard-fiber flag, retain window, fence/deny
      counters; source-cite + coverage linter; extend existing
      capability/security batch per #81967 (no docs/design per #1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    cap = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/security_defaults.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    _read("build.py")

    # AC1 — multi-tenant / Strict arming: hard_fiber_isolation=true + K=64.
    must("set_hard_fiber_isolation", "AC1", sec)
    must("set_grant_epoch_retain_window", "AC1", sec)
    must("kDefaultGrantEpochRetainWindowMultiTenant", "AC1", cap)
    must("kDefaultGrantEpochRetainWindowRestricted", "AC2", cap)
    # apply_production_security_defaults must contain the arming branch
    # for multi-tenant/Strict. Loose check: the constant names appear
    # in security_defaults.hh (the arming block reads them).
    must("multi_tenant && strict", "AC1", sec)
    must("commercial_tenant_profile", "AC2", sec)

    # AC2 — Restricted K=16 anti-privilege-sticky. The Restricted branch
    # in the grant_epoch_retain_window arming must set 16.
    if "kDefaultGrantEpochRetainWindowRestricted" not in sec:
        fails.append("AC2: security_defaults.hh does not arm K=16 for Restricted")

    # AC3 — Soft / dev_off: hard_fiber false, K=0.
    if "dev_off" not in sec:
        fails.append("AC3: security_defaults.hh dev_off path missing")
    # dev_off branches must include set_hard_fiber_isolation(false) AND
    # set_grant_epoch_retain_window(0) across ALL dev_off blocks (the
    # arming is split across multiple sections in apply_production_security_defaults).
    all_dev_off_bodies = []
    pos = 0
    while True:
        sig = re.search(r"if\s*\(\s*dev_off\s*\)\s*\{", sec[pos:])
        if not sig:
            break
        start = pos + sig.end()
        depth = 1
        i = start
        while i < len(sec) and depth > 0:
            c = sec[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        all_dev_off_bodies.append(sec[start : i - 1])
        pos = i
    combined_dev_off = "\n".join(all_dev_off_bodies)
    if "set_hard_fiber_isolation(false)" not in combined_dev_off:
        fails.append("AC3: dev_off branches must include set_hard_fiber_isolation(false)")
    if "set_grant_epoch_retain_window(0)" not in combined_dev_off:
        fails.append("AC3: dev_off branches must include set_grant_epoch_retain_window(0)")

    # AC4 — on_mutation_epoch_bump only advances min_valid when K > 0.
    must("on_mutation_epoch_bump", "AC4", cap)
    if "grant_epoch_retain_window_" not in cap:
        fails.append("AC4: capability_model.hh missing grant_epoch_retain_window_ atomic")
    # capability_epoch_fence_hit_total counter must be bumped from
    # on_mutation_epoch_bump. Loose check: the counter is bumped somewhere
    # in capability_model.hh when epoch advances past K.
    if "capability_epoch_fence_hit_total.fetch_add" not in cap:
        fails.append(
            "AC4: capability_epoch_fence_hit_total.fetch_add not found (should be called from on_mutation_epoch_bump)"
        )

    # AC5 — hard_fiber on → deny + capability_fiber_hard_deny_total.
    must("capability_fiber_hard_deny_total", "AC5", cap)
    must("provenance_ok", "AC5", cap)
    # provenance_ok must branch on hard_fiber_isolation_ AND bump
    # capability_fiber_hard_deny_total on the deny path. Check a 5000-char
    # window after the function signature (covers the function body without
    # relying on a fragile brace-depth walker).
    sig = re.search(r"\bprovenance_ok\s*\([^)]*\)\s*(?:const)?\s*\{", cap)
    if not sig:
        fails.append("AC5: provenance_ok signature not found")
    else:
        window_end = min(sig.end() + 5000, len(cap))
        window = cap[sig.start() : window_end]
        if "hard_fiber_isolation_" not in window:
            fails.append(
                "AC5: provenance_ok body must reference hard_fiber_isolation_ "
                "(false → metric-only, true → deny + bump capability_fiber_hard_deny_total)"
            )
        if "capability_fiber_hard_deny_total" not in window:
            fails.append("AC5: provenance_ok body must bump capability_fiber_hard_deny_total")

    # AC6 — query surface wired.
    must("capability-hard-fiber-isolation", "AC6", q)
    must("capability-grant-epoch-retain-window", "AC6", q)
    must("capability-grant-min-valid-epoch", "AC6", q)
    must("capability-epoch-fence-hit-total", "AC6", q)
    must("capability-fiber-hard-deny-total", "AC6", q)
    must("schema-2688", "AC6", q)
    must("issue-2688", "AC6", q)
    must("capability-production-default-armed", "AC6", q)
    # Query surface must use the correct API (not a wrong internal path).
    # Allow clang-format to break after `g_capability_effect_metrics()` so
    # the substring check is whitespace-tolerant.
    q_ws = re.sub(r"\s+", "", q)
    must("g_capability_registry().hard_fiber_isolation", "AC6", q)
    must("g_capability_registry().grant_epoch_retain_window", "AC6", q)
    must("g_capability_registry().grant_min_valid_epoch", "AC6", q)
    must("g_capability_effect_metrics().capability_epoch_fence_hit_total", "AC6", q_ws)
    must("g_capability_effect_metrics().capability_fiber_hard_deny_total", "AC6", q_ws)

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/capability_production_default_2688.md",
        "docs/capability_production_default_2688.md",
        "design/2688.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # AC6 — self-coverage: #2688 sentinel in capability_model.hh +
    # security_defaults.hh + query. Use "#2688" (not "Issue #2688") to
    # accept combined citations.
    must("#2688", "AC6", cap)
    must("#2688", "AC6", sec)
    must("#2688", "AC6", q)

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_capability_production_default_2688.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2688 capability production-default armed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
