#!/usr/bin/env python3
"""Issue #3721 source-cite gate: the MSE policy row joins the session mid.

security:grant-effect! with MacroSelfEvo bits landed a session-bound named
row via grant_effect_capability and then seeded the "macro-self-evo"
policy row via grant_macro_self_evo with make_grant_provenance(mid=0):
the policy row never got session_bound / single_use and bound mid=0, so
the outermost MutationBoundary dtor's revoke_session_grants_for_mid
(keys session_bound && bound_mutation_id == session_mid_at_enter_)
missed it — the TA-gated MSE grant stayed consumable across later
mutations until retain K=64 (dual-track).

ACs:
  AC1  the MSE arm in evaluator_primitives_security.cpp joins the #3143
       mid SSOT (join_audit_and_se_mid) before grant_macro_self_evo.
  AC2  grant_macro_self_evo's apply stamps session_bound + single_use
       under Restricted/Strict (#3721 cite); Soft/Off keeps legacy flags.
  AC3  the durable admin path (grant_effect_durable) does NOT route
       through grant_macro_self_evo — sticky durable MSE unaffected.
  AC4  tests cite #3721; no test_issue_3721.cpp; no docs/design/3721-*;
       no new query key.
  AC5  build.py wires this linter.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARM = ROOT / "src" / "compiler" / "evaluator_primitives_security.cpp"
CORE = ROOT / "src" / "core" / "capability_model.hh"
SEC = ROOT / "src" / "compiler" / "evaluator_security.cpp"
TST = ROOT / "tests" / "compiler" / "test_grant_epoch_retain_restricted.cpp"


def main() -> int:
    arm = ARM.read_text() if ARM.exists() else ""
    core = CORE.read_text() if CORE.exists() else ""
    sec = SEC.read_text() if SEC.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    good = (
        "Issue #3721: the MSE policy row joins the #3143 Mutation mid" in arm
        and "const auto mid = typed_audit::join_audit_and_se_mid(0);" in arm
    )
    report("AC1", good, "MSE arm joins the mid SSOT")

    good = (
        "Issue #3721: production MSE grant is the same lifetime as" in core
        and "g.session_bound = true;" in core
        and "g.single_use = true;" in core
        and "EffectSandboxMode::Restricted" in core
    )
    report("AC2", good, "apply stamps session_bound + single_use under production")

    dur = sec[sec.find("void Evaluator::grant_effect_durable") :] if sec else ""
    dur = (
        dur[: dur.find("void Evaluator::grant_effect_durable_sticky")]
        if "void Evaluator::grant_effect_durable_sticky" in dur
        else dur[:8000]
    )
    good = "grant_macro_self_evo" not in dur
    report("AC3", good, "durable path does not route through grant_macro_self_evo")

    bmf = ROOT / "tests" / "compiler" / "test_grant_bound_mid_force.cpp"
    bmf_src = bmf.read_text() if bmf.exists() else ""
    good = (
        "#3721" in tst
        and "#3721" in bmf_src
        and "revoke_session_grants_for_mid" in bmf_src
        and not (ROOT / "tests" / "compiler" / "test_issue_3721.cpp").exists()
        and not (ROOT / "docs" / "design" / "3721-mse-session-bound.md").exists()
    )
    report("AC4", good, "tests cite #3721 (retain + bound-mid-force); no new artifacts")

    good = "check_mse_session_bound_3721.py" in build
    report("AC5", good, "build.py wires this linter")

    print("Issue #3721 MSE session-bound linter: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
