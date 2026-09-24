#!/usr/bin/env python3
"""Issue #4052: IsolationDeny epoch column is the Mutation epoch, not TypedMid.

Contract (one row per AC):
  AC1  WorkspaceIsolationPolicy::record_audit emits the IsolationDeny SE with
       (mutation_id = mid, epoch = epoch) -- the local current_mutation_epoch()
       is passed through instead of repeating the TypedMid in the epoch slot;
       mutation_id stays TypedMid (#3801 not reverted)
  AC2  Evaluator Typed correlated rows pass the Mutation epoch as the
       before/after epoch (require_effect_on_ref stale-ref,
       check_workspace_isolation, check_tenant_host_path); the Effect paths
       keep prov.epoch and no site regresses to (mid, op, mid)
  AC3  the resume / fiber-principal-mismatch correlated rows pass epoch while
       their IsolationDeny SE keeps (mid, epoch)
  AC4  ACs extend tests/core/test_tenant_isolation_enforcement.cpp #3801 AC1
       (SE epoch == 42 alongside mid == 777, typed before/after epoch == 42);
       no tests/**/test_issue_4052.cpp; no docs/design/4052-*
  AC5  build.py wires check_isolation_deny_epoch_4052 + root allowlist;
       no new query key (query:security-audit still prints epoch verbatim)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    iso = _read("src/core/workspace_isolation.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: record_audit epoch column ───────────────────────────────────
    must("Issue #4052", "AC1 record_audit cites issue", iso)
    must(
        "emit_security_event_durable(SecurityEventKind::IsolationDeny, caller != 0 ? caller : target,\n                                    mid, epoch, required_effects, op, reason, /*denied=*/true,",
        "AC1 SE emits (mid, epoch)",
        iso,
    )
    forbid("mid, mid, required_effects", "AC1 epoch slot repeats TypedMid", iso)
    must("const auto mid =", "AC1 TypedMid mid kept", iso)
    must("const auto epoch = current_mutation_epoch();", "AC1 epoch load kept", iso)
    # whole-file negative: no remaining (mid, mid) SE emit anywhere
    forbid(
        "SecurityEventKind::IsolationDeny,\n                                    mid, mid,",
        "AC1 residual mid-in-epoch emit",
        iso,
    )

    # ── AC2: Evaluator typed correlated rows ─────────────────────────────
    must("capture_security_correlated_audit(mid, op, epoch,", "AC2 typed row epoch", sec)
    forbid("capture_security_correlated_audit(mid, op, mid,", "AC2 regression to mid epoch", sec)
    # Effect paths keep prov.epoch (never rewritten to mid)
    must("capture_security_correlated_audit(mid, op, prov.epoch,", "AC2 Effect path prov.epoch kept", sec)
    must("Issue #4052", "AC2 sites cite issue", sec)
    # mutation_id stays the TypedMid join key at every rewritten site
    for label, needle in (
        (
            "stale-ref",
            "typed_audit::capture_security_correlated_audit(mid, op, epoch, /*denied=*/true,\n                                                           /*target_node=*/ref.id",
        ),
        (
            "check_workspace_isolation",
            "typed_audit::capture_security_correlated_audit(mid, op, epoch, /*denied=*/true,\n                                                       /*target_node=*/0",
        ),
        (
            "check_tenant_host_path",
            "typed_audit::capture_security_correlated_audit(mid, op, epoch, /*denied=*/true,\n                                                   /*target_node=*/0",
        ),
    ):
        must(needle, f"AC2 {label} row", sec)

    # ── AC3: fiber mutation rows ─────────────────────────────────────────
    must("Issue #4052", "AC3 fiber cites issue", fib)
    must('"fiber-principal-mismatch", epoch, /*denied=*/true,', "AC3 epoch arg", fib)
    forbid('"fiber-principal-mismatch", mid, /*denied=*/true,', "AC3 regression to mid epoch", fib)
    must(
        "emit_security_event_durable(SecurityEventKind::IsolationDeny, assigned, mid, epoch,",
        "AC3 SE keeps (mid, epoch)",
        fib,
    )

    # ── AC4: test placement + no invent ──────────────────────────────────
    must("4052: IsolationDeny SE epoch == Mutation epoch 42 (not TypedMid 777)", "AC4 SE epoch assertion", test)
    must("4052: typed correlated before_epoch == 42", "AC4 typed before epoch", test)
    must("4052: typed correlated after_epoch == 42", "AC4 typed after epoch", test)
    must("Issue #3801", "AC4 extends #3801 block", test)
    if _read("tests/issues/test_issue_4052.cpp"):
        fails.append("AC4: tests/issues/test_issue_4052.cpp exists")
    if _read("tests/compiler/test_issue_4052.cpp"):
        fails.append("AC4: tests/compiler/test_issue_4052.cpp exists")
    if _read("tests/core/test_issue_4052.cpp"):
        fails.append("AC4: tests/core/test_issue_4052.cpp exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in sorted(design.glob("4052-*")):
            fails.append(f"AC4: docs/design file present: {p.name}")

    # ── AC5: wiring + no new query key ───────────────────────────────────
    must("check_isolation_deny_epoch_4052", "AC5 build.py wires linter", build)
    must("Issue #4052", "AC5 build.py rationale", build)
    must("check_isolation_deny_epoch_4052.py", "AC5 root allowlist", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("4052 IsolationDeny epoch == Mutation epoch: all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
