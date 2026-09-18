#!/usr/bin/env python3
"""Issue #3872 — production + live mutate session always hard-gates.

Residual: requires_invariant_hard_gate under Sampled returned
`linear || match_sites || nodes >= force_n`; with production defaults the
#2053 arm (force_n == 1) covers any non-zero dirty cone, but a zero-node
dirty cone (observe-only self-mod attempt) still rode the soft path.

  AC1  gate-arm:          requires_invariant_hard_gate signature carries
      `mutate_session_active` and the #3872 production arm returns true.
  AC2  decide-mirror:     the #2281 Agent-visible decide() mirrors the arm
      (would_audit / would_hard_gate fold + "mutate-session" reason).
  AC3  call-sites-wired:  the three production call sites pass
      `/*mutate_session=*/true` (mutation boundary ×2, typecheck ×1).
  AC4  no-invent:         no docs/design/3872-*, no test_issue_3872.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (signature without the session arm) and must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
BOUNDARY = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TYPECHECK = ROOT / "src" / "compiler" / "evaluator_typecheck.cpp"


def ac1_gate_arm():
    text = HEADER.read_text()
    ok = (
        "Issue #3872" in text
        and "bool mutate_session_active = false" in text
        and "if (production_defaults_active() && mutate_session_active)" in text
    )
    print("AC(gate-arm): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_decide_mirror():
    text = HEADER.read_text()
    ok = (
        text.count("mutate_session_active") >= 3
        and '"mutate-session"' in text
        and "const bool mutate_session_force = d.production_defaults && mutate_session_active;" in text
    )
    print("AC(decide-mirror): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_call_sites_wired():
    boundary = BOUNDARY.read_text()
    typecheck = TYPECHECK.read_text()
    ok = boundary.count("/*mutate_session=*/true") >= 2 and "/*mutate_session=*/true" in typecheck
    print("AC(call-sites-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_no_invent():
    docs = list(ROOT.glob("docs/design/3872-*"))
    invented = (ROOT / "tests" / "compiler" / "test_issue_3872.cpp").exists()
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test():
    fixture = (
        "[[nodiscard]] inline bool requires_invariant_hard_gate(std::uint64_t nodes_changed,\n"
        "    bool linear_ops_present, bool strict_sandbox,\n"
        "    bool match_sites_present = false) noexcept {\n"
        "    const auto s = get_strategy();\n"
        "    if (s == AuditStrategy::Off)\n"
        "        return false;\n"
        "    return linear_ops_present || match_sites_present;\n"
        "}\n"
    )
    flagged = (
        "mutate_session_active" not in fixture
        and "if (production_defaults_active() && mutate_session_active)" not in fixture
    )
    print("self-test: " + ("PASS — stale signature (no session arm) detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_gate_arm(),
        ac2_decide_mirror(),
        ac3_call_sites_wired(),
        ac4_no_invent(),
    ]
    print(f"check_mutate_session_hard_gate_3872: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
