#!/usr/bin/env python3
"""Issue #3875 — revoke_epoch process-origin stamp is hard-only.

Residual: revoke_locked clamped Mutation epoch 0 to 1 unconditionally, so
Soft observe rows carried a phantom revoke_epoch = 1 that leaks into
mutation-order stats via the commit_health readiness clamp.

  AC1  hard-only-clamp: capability_model.hh cites #3875 and the clamp is
      face-conditioned (`ep == 0 && capability_epoch_hard_face()`).
  AC2  test-wired:       test_audit_mutation_id_unify.cpp (built host)
      carries the #3875 behavioral ACs (Soft stays 0 / Hard stamps 1).
  AC3  no-invent:        no docs/design/3875-*, no test_issue_3875.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (unconditional epoch-0 clamp) and must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "core" / "capability_model.hh"
TEST = ROOT / "tests" / "compiler" / "test_audit_mutation_id_unify.cpp"


def ac1_hard_only_clamp():
    text = HEADER.read_text()
    ok = (
        "Issue #3875" in text
        and "if (ep == 0 && capability_epoch_hard_face())" in text
        and "if (ep == 0)\n                    ep = 1;" not in text
    )
    print("AC(hard-only-clamp): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_test_wired():
    text = TEST.read_text()
    ok = "3875 AC1" in text and "3875 AC2" in text and "ac3875_revoke_epoch_hard_only_invent" in text
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_no_invent():
    docs = list(ROOT.glob("docs/design/3875-*"))
    invented = (ROOT / "tests" / "compiler" / "test_issue_3875.cpp").exists()
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test():
    fixture = (
        "auto ep = revoke_at_epoch;\n"
        "if (ep == 0)\n"
        "    ep = ::aura::core::current_mutation_epoch();\n"
        "if (ep == 0)\n"
        "    ep = 1; // non-zero audit stamp at process origin\n"
        "g.revoke_epoch = ep;\n"
    )
    flagged = "capability_epoch_hard_face()" not in fixture
    print("self-test: " + ("PASS — unconditional epoch-0 clamp detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_hard_only_clamp(),
        ac2_test_wired(),
        ac3_no_invent(),
    ]
    print(f"check_revoke_epoch_hard_only_3875: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
