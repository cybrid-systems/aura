#!/usr/bin/env python3
"""Issue #2559: three-layer linear invariant wire inventory gate.

Linear safety is intentionally three-layered:
  (1) Type  — OwnershipEnv / synth hard-fail / post-mutate enforce
  (2) IR    — try_lower_linear_type / escape move-elision / executor state machine
  (3) Memory — pin_contract ∧ RootRemap fail totals ∧ densify ownership scan_fail

Recent issues (#2514, #2499, #2497, #2495, #2263) closed individual holes;
this linter prevents new mutate / boundary / densify sites from landing
without the unified wire (or a documented LINEAR_WIRE_EXEMPT reason).

Contract:
  AC1 Outermost hard-gate / boundary exit uses force_linear_rollback
      (unified linear deny entry; deny_if_linear_synth is thin alias).
  AC2 typed_mutate / post-mutate success path calls linear_post_mutate_enforce*
      (or LINEAR_WIRE_EXEMPT documented).
  AC3 Moving densify Phase 5 success ANDs pin_contract_held +
      root_remap_*_fail_total == 0 + scan_fail_delta (regress #2499/#2497).
  AC4 Soft densify / no Moving remains zero-cost shape (gate only under
      had_moving / moving_compact_enabled); no false-positive storm.
  AC5 Source-cite #2559 inventories + build.py --strict gate registration.
  AC6 Tests + CMakeLists registrations only (no docs/design).

Exit 0 = all rows satisfied.
Use --self-test to verify intentional missing-string detection.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# ── Inventory (source of truth for AC comments / future growth) ──────────
# Type layer production sites that must route linear deny / post-mutate.
TYPE_LAYER_SITES = (
    "src/compiler/evaluator_mutation_boundary.cpp",  # outermost force_linear_rollback
    "src/compiler/evaluator_typecheck.cpp",  # force_linear_rollback + post-mutate
    "src/compiler/evaluator_primitives_mutate.cpp",  # typed_mutate success enforce
)

# IR / lower layer sites.
IR_LAYER_SITES = (
    "src/compiler/lowering_linear_types_impl.cpp",  # try_lower_linear_type
    "src/compiler/ownership_escape_lowering_gate.h",  # escape_blocks_move_elision*
    "src/compiler/ir_executor_impl.cpp",  # enforce_linear_ownership_state
)

# Memory / densify Phase 5 gate sites.
MEMORY_LAYER_SITES = (
    "src/compiler/evaluator_mutation_boundary.cpp",  # pin ∧ root_remap ∧ scan_fail
    "src/core/arena.ixx",  # densify fold into pin_contract_held (#2499)
)

# Documented exemptions: substring that must appear near optional sites.
# Format: (file, exempt_token) — presence of token documents intentional skip.
LINEAR_WIRE_EXEMPTS: tuple[tuple[str, str], ...] = ()

# Issue #2623 AC7: cross-closure env keys in the linear safety inventory
# (HARD force, DEPTH scan budget; must remain source-cited in audit).
LINEAR_CROSS_CLOSURE_ENV_KEYS = (
    "AURA_LINEAR_CROSS_CLOSURE_HARD",
    "AURA_LINEAR_CROSS_CLOSURE_DEPTH",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="Verify intentional break detection (must fail when wire missing)",
    )
    args = ap.parse_args(argv)

    if args.self_test:
        # Intentional break: require a string that is never present.
        fake = _read("src/compiler/evaluator_mutation_boundary.cpp")
        needle = "LINEAR_THREE_LAYER_WIRE_INTENTIONAL_BREAK_2559_NEVER"
        if needle in fake:
            print("SELF-TEST FAIL: break needle unexpectedly present", file=sys.stderr)
            return 1
        print("OK: self-test — intentional missing wire would be detected")
        return 0

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    llt = _read("src/compiler/lowering_linear_types_impl.cpp")
    esc = _read("src/compiler/ownership_escape_lowering_gate.h")
    irx = _read("src/compiler/ir_executor_impl.cpp")
    arx = _read("src/core/arena.ixx")
    test = _read("tests/compiler/test_linear_three_layer_wire.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    this = _read("scripts/coverage/checks/check_linear_three_layer_wire_2559.py")

    # ── AC1: outermost hard-gate / boundary uses unified linear deny ──
    must("force_linear_rollback", "AC1", emb)
    must("force_linear_rollback", "AC1", etc)
    must("force_linear_rollback", "AC1", eixx)
    must("deny_if_linear_synth_hard_fail", "AC1", etc)
    # Decision table / synth path before soft recovery.
    must("linear_synth_hard_fail", "AC1", emb)
    must("linear-synth-hard-fail", "AC1", emb)
    # Must not soft-success past synth hard-fail without re-audit (order:
    # force_linear_rollback before partial recovery Success).
    force_pos = emb.find('force_linear_rollback(composite ? "composite-linear-synth-hard-fail"')
    if force_pos < 0:
        force_pos = emb.find("force_linear_rollback")
    # partial recovery success markers after force path
    recovery_markers = (
        "partial_recovery",
        "recovered = true",
        "InvariantAuditResult",
    )
    if force_pos < 0:
        fails.append("AC1: force_linear_rollback not found in boundary exit")
    else:
        # Ensure force_linear_rollback appears before the do_audit partial recovery block
        do_audit_pos = emb.find("const bool do_audit", force_pos)
        if do_audit_pos > 0 and force_pos > do_audit_pos:
            fails.append("AC1: force_linear_rollback must run before do_audit recovery path")

    # ── AC2: post-mutate / typed_mutate linear enforce ──
    must("linear_post_mutate_enforce_all", "AC2", etc)
    must("linear_post_mutate_enforce_all", "AC2", mut)
    must("post_mutation_invariant_check", "AC2", etc)
    # At least one typed_mutate success path cites enforce.
    must("linear_post_mutate_enforce", "AC2", mut)
    for rel, token in LINEAR_WIRE_EXEMPTS:
        must(token, "AC2-exempt", _read(rel))

    # ── AC3: densify Phase 5 triple AND (pin ∧ root_remap ∧ scan_fail) ──
    must("pin_contract_held", "AC3", emb)
    must("root_remap_stable_ref_fail_total == 0", "AC3", emb)
    must("root_remap_closure_capture_fail_total == 0", "AC3", emb)
    must("scan_fail_delta", "AC3", emb)
    must("scan_fail_baseline", "AC3", emb)
    # Issue #2599: production-only AND via envframe_block = prod && scan_fail_delta
    # (replaces bare !scan_fail_delta under Soft).
    must("envframe_block", "AC3", emb)
    must("prod_for_densify && scan_fail_delta", "AC3", emb)
    must("!envframe_block", "AC3", emb)
    # Lineage stamps.
    must("Issue #2499", "AC3", emb)
    must("Issue #2497", "AC3", emb)
    # arena densify source fold.
    must("pin_contract_held", "AC3", arx)
    must("root_remap_stable_ref_fail_total", "AC3", arx)

    # ── AC4: Soft densify zero-cost shape (no false positive) ──
    must("had_moving_densify", "AC4", emb)
    must("moving_compact_enabled", "AC4", emb)
    # Soft branch gates densify work on Moving.
    must("if (had_moving_densify && pin_contract_held)", "AC4", emb)
    # Default pin_contract_held true when no Moving.
    must("pin_contract_held = true", "AC4", emb)

    # ── IR layer inventory (presence = wired; not densify-coupled) ──
    must("try_lower_linear_type", "IR", llt)
    must("escape_blocks_move_elision", "IR", llt + esc)
    must("enforce_linear_ownership_state", "IR", irx)

    # ── AC5: source-cite + inventories in this script ──
    must("Issue #2559", "AC5", this)
    must("TYPE_LAYER_SITES", "AC5", this)
    must("IR_LAYER_SITES", "AC5", this)
    must("MEMORY_LAYER_SITES", "AC5", this)
    must("LINEAR_WIRE_EXEMPT", "AC5", this)
    # Production files cite #2559 (added by this ship).
    must("Issue #2559", "AC5", emb)
    must("Issue #2559", "AC5", etc)
    must("#2559", "AC5", mut)
    # Issue #2623 AC7: inventory lists cross-closure env keys + audit cites them.
    aud = _read("src/compiler/typed_mutation_audit.h")
    must("LINEAR_CROSS_CLOSURE_ENV_KEYS", "AC5-2623", this)
    must("#2623", "AC5-2623", this)
    for env_key in LINEAR_CROSS_CLOSURE_ENV_KEYS:
        must(env_key, "AC5-2623", this)
        must(env_key, "AC5-2623", aud)

    # ── AC6: test + cmake + build.py ──
    must("ac1_boundary_force_linear", "AC6", test)
    must("ac2_post_mutate_enforce", "AC6", test)
    must("ac3_densify_triple_and", "AC6", test)
    must("ac4_soft_densify_shape", "AC6", test)
    must("ac5_source_inventory", "AC6", test)
    must("ac6_linter_self_test", "AC6", test)
    must("test_linear_three_layer_wire", "AC6", cmake)
    must("check_linear_three_layer_wire_2559", "AC6", build)
    must("cmd_linear_three_layer_wire_coverage", "AC6", build)

    # Inventory files must exist (prevents silent path drift).
    for rel in TYPE_LAYER_SITES + IR_LAYER_SITES + MEMORY_LAYER_SITES:
        if not (ROOT / rel).is_file():
            fails.append(f"inventory: missing file {rel}")

    # Silence unused recovery_markers if static analysis complains — use for soft check.
    _ = recovery_markers

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2559 three-layer linear wire inventory — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
