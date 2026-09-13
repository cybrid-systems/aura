#!/usr/bin/env python3
"""Issue #3685 source-cite gate: clone-walk policy rides the session.

TopLevelMacroCapGuard armed only at depth==0 and wrote rest-hygiene /
gensym-ceiling / force_hygienic policy to TLS; nested at_depth recursion
did not re-arm, so a fiber yield mid-walk let another fiber's top-level
clone overwrite TLS and the resumed walk read foreign tenant policy
(rest `__rest_` skip = capture leak, or false / missed gensym ceiling).

ACs:
  AC1  CloneSessionPolicy exists; the walk reads session.allow_rest_hygiene
       / session.force_hygienic / effective_max_gensym_map_size(session).
  AC2  clone_macro_body_at_depth decl + def take the session; the wrapper
       passes defaults; the depth+1 recursion threads it.
  AC3  the guard captures the session from check_macro_self_evo at arm and
       depth==0 syncs it; TLS mirrors stay written for diagnostics.
  AC4  depth stays the explicit argument (hygiene_depth + 1 recursion);
       no TLS depth authority introduced.
  AC5  tests cite #3685 (clone depth suite); no test_issue_3685.cpp; no
       docs/design/3685-*; no new query key.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ME = ROOT / "src" / "compiler" / "macro_expansion.cpp"
LIM = ROOT / "tests" / "compiler" / "test_concurrent_clone_hygiene_depth.cpp"


def main() -> int:
    me = ME.read_text() if ME.exists() else ""
    lim = LIM.read_text() if LIM.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    good = (
        "struct CloneSessionPolicy" in me
        and "if (session.force_hygienic) {" in me
        and "effective_max_gensym_map_size(session)" in me
        and "if (!session.allow_rest_hygiene)" in me
        and "session.allow_rest_hygiene &&" in me
    )
    report("AC1", good, "walk reads the session, not TLS")

    good = (
        "const CloneSessionPolicy& session" in me
        and "child_qq_depth, session);" in me
        and "/*qq_depth=*/0, CloneSessionPolicy{});" in me
    )
    report("AC2", good, "signatures threaded (decl/def/recursion/wrapper)")

    good = (
        "CloneSessionPolicy session{};" in me
        and "session = top_cap_guard.session;" in me
        and "chk.effective.allow_rest_hygiene" in me
        and "s_force_hygienic = chk.effective.force_hygienic;" in me
    )
    report("AC3", good, "guard captures at arm; TLS mirrors stay diagnostics")

    good = "hygiene_depth + 1" in me
    report("AC4", good, "depth still the explicit argument")

    good = (
        "#3685" in lim
        and not (ROOT / "tests" / "compiler" / "test_issue_3685.cpp").exists()
        and not (ROOT / "docs" / "design" / "3685-clone-session-policy.md").exists()
        and "query:macro-hygiene-provenance-stats"
        in (ROOT / "src" / "compiler" / "evaluator_primitives_obs_jit.cpp").read_text()
    )
    report("AC5", good, "tests cite #3685; no new artifacts; query key unchanged")

    print("Issue #3685 clone session policy linter: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
