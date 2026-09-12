#!/usr/bin/env python3
"""Issue #3683 source-cite gate: MacroIntroduced deny kind unified.

The Agent-facing deny face for MacroIntroduced default-reject was
dual-track: reject_structural_macro_hygiene / the atomic-batch walk /
the sub-op conversion stamped ("hygiene", ...) while
hygiene_protected_error stamped ("hygiene-protected", ...); the
lockless eval_flat arms returned Diagnostic{InternalError, ...}.
Replay keying on the tagged kind could not fold one self-evo deny.
Sibling: the capability deny published clone last_reject_reason=1
(gensym-ceiling code) although note_hygiene_last_limit_reason already
stamps the unified 7 sentinel.

ACs:
  AC1  one tagged deny kind ("hygiene-protected") across reject_structural,
       set-body / insert-child inline faces, rename-symbol, the atomic-batch
       walk and the sub-op conversion; no ("hygiene", ...) deny remains.
  AC2  capability deny does not publish clone last_reject_reason=1;
       note_hygiene_last_limit_reason(kHygieneLimitReasonCapabilityDeny)
       kept at both chokepoints (last_limit_reason 7 = query surface).
  AC3  :allow-macro? without MacroSelfEvo still denied (deny_macro_opt_out_
       without_mse face, #3542) — kind hygiene-protected, not silent allow.
  AC4  Soft/Off: one is_macro_introduced load then return (short-circuit).
  AC5  tests extended (closed-loop ac3683 + capability-uniformity cite
       #3683); no test_issue_3683.cpp; no docs/design/3683-*; query key
       query:macro-hygiene-provenance-stats unchanged.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MUT = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
ME = ROOT / "src" / "compiler" / "macro_expansion.cpp"
CLOSED = ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp"
UNIF = ROOT / "tests" / "compiler" / "test_capability_macro_self_evo_reason_uniformity.cpp"


def check_ac1(mut: str) -> tuple[bool, str]:
    # exact un-unified kind (not a prefix of hygiene-protected)
    if re.search(r'me[v]\("hygiene"', mut) or re.search(r'make_merr\(\s*"hygiene"', mut):
        return False, 'un-unified ("hygiene", ...) deny still present'
    for site in ("reject_structural_macro_hygiene", "hygiene_protected_error", "deny_macro_opt_out_without_mse"):
        if site not in mut:
            return False, f"{site} missing"
    if mut.count('"hygiene-protected"') < 4:
        return False, "unified kind insufficiently present"
    return True, "one tagged deny kind across structural / inline / batch faces"


def check_ac2(flat: str, me: str) -> tuple[bool, str]:
    import re as _re

    for txt in (flat, me):
        if _re.search(r"g_macro_clone_last_reject_reason\.store\(1\s*,", txt):
            return False, "capability deny still publishes clone last_reject_reason=1"
    if "kHygieneLimitReasonCapabilityDeny" not in flat or "kHygieneLimitReasonCapabilityDeny" not in me:
        return False, "last_limit_reason 7 dropped from a chokepoint"
    return True, "capability deny publishes only last_limit_reason 7"


def check_ac3(mut: str) -> tuple[bool, str]:
    i = mut.find("deny_macro_opt_out_without_mse")
    if i < 0:
        return False, "MSE opt-out deny face missing"
    win = mut[i : i + 2600]
    if 'mev("hygiene-protected"' not in win and "return mev(" not in win:
        return False, "MSE deny face lost its tagged pair"
    if "MacroSelfEvo" not in win:
        return False, "MSE face no longer requires MacroSelfEvo (#3542)"
    return True, ":allow-macro? without MacroSelfEvo still denied (hygiene-protected)"


def check_ac4(mut: str) -> tuple[bool, str]:
    if "!flat.is_macro_introduced(id)" not in mut:
        return False, "non-macro short-circuit missing"
    if "effect_sandbox_mode() == 0" not in mut:
        return False, "Soft/Off mode short-circuit missing"
    return True, "Soft/Off: one is_macro_introduced load then return"


def check_ac5() -> tuple[bool, str]:
    closed = CLOSED.read_text() if CLOSED.exists() else ""
    unif = UNIF.read_text() if UNIF.exists() else ""
    if "ac3683_deny_kind_unified" not in closed:
        return False, "closed-loop test must carry the #3683 ACs"
    if "#3683" not in unif:
        return False, "capability-uniformity test must cite #3683"
    for f in glob_flip():
        if '== "hygiene"' in f.read_text():
            return False, f"stale exact-equality pin in {f.name}"
    if (ROOT / "tests" / "compiler" / "test_issue_3683.cpp").exists():
        return False, "forbidden tests/**/test_issue_3683.cpp (per #81934)"
    if (ROOT / "docs" / "design" / "3683-hygiene-deny-kind.md").exists():
        return False, "forbidden docs/design/3683-* (per #1655)"
    obs = (ROOT / "src" / "compiler" / "evaluator_primitives_obs_jit.cpp").read_text()
    if "query:macro-hygiene-provenance-stats" not in obs:
        return False, "query key renamed (forbidden)"
    return True, "tests extended; query key unchanged; no new artifacts"


def glob_flip():
    return [
        ROOT / "tests" / "compiler" / "test_move_node_hygiene.cpp",
        ROOT / "tests" / "compiler" / "test_tweak_literal_audit_consistency.cpp",
        ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp",
        ROOT / "tests" / "compiler" / "test_replace_subtree_new_body_hygiene.cpp",
        ROOT / "tests" / "compiler" / "test_scalar_mutate_record_patch_hygiene.cpp",
    ]


def main() -> int:
    mut = MUT.read_text() if MUT.exists() else ""
    flat = FLAT.read_text() if FLAT.exists() else ""
    me = ME.read_text() if ME.exists() else ""
    if not mut:
        print(f"FAIL: cannot read {MUT}")
        return 1
    ok = True
    for name, fn in (
        ("AC1", lambda: check_ac1(mut)),
        ("AC2", lambda: check_ac2(flat, me)),
        ("AC3", lambda: check_ac3(mut)),
        ("AC4", lambda: check_ac4(mut)),
        ("AC5", check_ac5),
    ):
        good, msg = fn()
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good
    if not ok:
        print("Issue #3683 hygiene deny kind uniformity linter: FAIL")
        return 1
    print("Issue #3683 hygiene deny kind uniformity linter: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
