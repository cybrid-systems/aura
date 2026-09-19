#!/usr/bin/env python3
"""Issue #3902 — grant-row revoke stamps honest under Soft / epoch=0.

Residual: #3875 closed the named revoke_locked stamp (invent 1 only
under capability_epoch_hard_face()), but the other Soft-visible
grant-row paths still invented ep=1 unconditionally when the Mutation
epoch was 0: single_use consume, session scope-dtor cascade, orphan
sweep, and the epoch-bound revoke fallback. Forensic Soft rows showed
a phantom 1 that can collide with a later real epoch=1.

Shipped: all four grant-row stamp sites now gate the invent on
capability_epoch_hard_face(); Soft keeps 0 honest unset. Named-revoke
#3875 ACs untouched (that site already used the vocabulary); session
bits still cleared on every path.

  AC1  site-fix: capability_model.hh cites Issue #3902 at all four
      stamp sites; each invent is gated on capability_epoch_hard_face().
  AC2  test-wired: test_capability_single_use_consume.cpp (built member
      of test_security_capability_batch) carries the #3902 behavioral
      ACs (Soft epoch=0 consume/cascade keep revoke_epoch 0 with bits
      cleared; hard face invents 1; real epoch passes through).
  AC3  no-invent: no docs/design/3902-*, no tests/issues/test_issue_
      3902.cpp, no tests/core/test_issue_3902.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (unconditional invent, no hard-face gate) and must
flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "src" / "core" / "capability_model.hh"
TEST = ROOT / "tests" / "core" / "test_capability_single_use_consume.cpp"
CITE = "Issue #3902"


def ac1_site_fix() -> bool:
    text = SITE.read_text()
    gated = text.count("if (ep == 0 && capability_epoch_hard_face())")
    ok = text.count(CITE) >= 4 and gated >= 4
    print("AC(site-fix): " + ("PASS" if ok else "FAIL") + f" (cites={text.count(CITE)}, gated={gated})")
    return ok


def ac2_test_wired() -> bool:
    text = TEST.read_text()
    ok = (
        all(f"3902 AC{n}" in text for n in range(1, 6))
        and "ac3902_1_soft_epoch0_consume_honest_unset" in text
        and "ac3902_2_hard_face_invent_1" in text
        and "ac3902_4_soft_session_cascade_honest" in text
    )
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_no_invent() -> bool:
    docs = list(ROOT.glob("docs/design/3902-*"))
    invented = any(
        (ROOT / p).exists()
        for p in (
            "tests/issues/test_issue_3902.cpp",
            "tests/core/test_issue_3902.cpp",
        )
    )
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test() -> int:
    fixture = (
        "auto ep = ::aura::core::current_mutation_epoch();\n"
        "if (ep == 0)\n"
        "    ep = 1;  // unconditional invent, no hard-face gate\n"
        "g.revoke_epoch = ep;\n"
    )
    flagged = "capability_epoch_hard_face()" not in fixture
    print("self-test: " + ("PASS — unconditional invent detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_site_fix(),
        ac2_test_wired(),
        ac3_no_invent(),
    ]
    print(f"check_soft_revoke_epoch_honest_3902: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
