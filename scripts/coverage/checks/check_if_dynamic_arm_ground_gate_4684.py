#!/usr/bin/env python3
"""Issue #4684: production if must not skip the ground gate when one arm is
already Dynamic.

Root cause: InferenceEngine::lub returned dynamic_type() *before*
ConstraintSystem::normalize and the production ground gate whenever either
side was already Dynamic. synthesize_flat_if passes that through and
synthesize_flat cached it with clear_dirty, so e.g.
      (let ((d (mutate:rebind "a" "b"))) (if #t d 1))
published a solved Dynamic with no TypeError and kept
type_export_is_authoritative() true on the production face (sandbox non-off,
apply_production_security_defaults / TypedMutationAudit Full|Sampled ratio=1).

Contract (one row per AC):
  AC1 lub no longer early-returns Dynamic before normalize; the production
      block normalizes first, then is_var (#4515), then the same ground gate
      that reports TypeError + void_type() (#4107). Ordering is asserted.
  AC2 the gate records last_join_gate_error_ and synthesize_flat's IfExpr arm
      stamps the node TypeError + mark_dirty and returns before the cache
      tail (no solved Void cache); infer_flat's root IfExpr guard mirrors the
      #4316 Call refusal.
  AC3 tests/compiler/test_ir.cpp has the live ac4684_* ACs (Dynamic arm +
      Int, soft gradual, #4107, #4515, consistent_unify hard face).
  AC4 no new query key, no new constraint kind, consistent_unify untouched,
      no docs/design, no tests/issues file.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    impl = _read("src/compiler/type_checker_impl.cpp")
    tix = _read("src/compiler/type_checker.ixx")
    tir = _read("tests/compiler/test_ir.cpp")

    m = re.search(r"TypeId InferenceEngine::lub\(TypeId a, TypeId b\) \{(?P<body>.*?)\n\}", impl, re.DOTALL)
    if not m:
        fails.append("AC1: could not extract InferenceEngine::lub body")
        body = ""
    else:
        body = m.group("body")

    # AC1: the pre-normalize Dynamic early return is gone.
    if "a == reg_.dynamic_type() || b == reg_.dynamic_type()" in body:
        fails.append("AC1: pre-normalize Dynamic early return still present in lub")

    # AC1: production face normalizes both sides before is_var / the gate.
    n_norm = body.find("cs_.normalize(a)")
    n_isvar = body.find("reg_.is_var(a) || reg_.is_var(b)")
    n_gate = body.find("last_join_gate_error_ = true;")
    n_report = body.find("diag_.report(")
    if min(n_norm, n_isvar, n_gate, n_report) < 0:
        fails.append(
            "AC1: lub production gate shape incomplete "
            f"(normalize={n_norm} is_var={n_isvar} flag={n_gate} report={n_report})"
        )
    else:
        if not (n_norm < n_isvar < n_gate <= n_report):
            fails.append(
                "AC1: lub order must be normalize -> is_var -> gate flag -> report "
                f"(normalize={n_norm} is_var={n_isvar} flag={n_gate} report={n_report})"
            )

    # AC1: only the two intentional Dynamic returns remain — the #4515
    # unbound-var join inside the production block and the Soft/Off tail.
    if body.count("return reg_.dynamic_type()") != 2:
        fails.append("AC1: lub must keep exactly two Dynamic returns (#4515 unbound join + Soft/Off tail)")

    must("incompatible ground types", "AC1 gate message", body)
    must("Issue #4684", "AC1 cite impl", impl)

    # AC2: flag declared, set by the gate, consumed by the IfExpr arm.
    must("bool last_join_gate_error_ = false;", "AC2 flag decl", tix)
    must("last_join_gate_error_ = false;", "AC2 flag reset", impl)
    must("if (last_join_gate_error_) {", "AC2 IfExpr consume", impl)
    must("flat.set_node_error(id, static_cast<std::uint8_t>(ErrorKind::TypeError));", "AC2 node error stamp", impl)
    must("flat.mark_dirty(id);", "AC2 keep dirty", impl)
    must("if_join_refused", "AC2 root guard", impl)
    must("if (call_ground_refused || if_join_refused)", "AC2 root guard use", impl)

    # AC3: live runtime ACs next to the #4107 / #4515 shapes.
    for ac in (
        "ac4684_1_dynamic_arm_gate_prod",
        "ac4684_2_dynamic_arm_soft_gradual",
        "ac4684_3_4107_two_grounds_preserved",
        "ac4684_4_4515_unbound_rep_stays_dynamic",
        "ac4684_5_consistent_unify_dynamic_int_prod_false",
    ):
        must(ac, "AC3 runtime", tir)

    # AC4: no invented surfaces.
    for banned in ("query:4684", "g_4684_", "ConstraintKind::DynamicArmGround"):
        if banned in impl or banned in tix:
            fails.append(f"AC4: invented surface {banned!r}")
    if (ROOT / "tests" / "issues" / "test_issue_4684.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_4684.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4684-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4684 Dynamic-arm if join reaches the production ground gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
