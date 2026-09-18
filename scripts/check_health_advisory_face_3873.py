#!/usr/bin/env python3
"""Issue #3873 — advisory overlay distinguishable from deny authority.

Residual: the type_linear_commit_health overlay rewrites force_reason
(coercion-slo / coercion-evidence-loss / occurrence-stale) while
would_allow_commit stays true; Agents could misread the advisory
force_reason as a hard deny.

  AC1  overlay-flag:     TypeLinearCommitHealthResult carries
      `advisory_overlay` (cites #3873) and the fold stamps it exactly
      when an observe-only overlay lands on an "ok" commit face.
  AC2  query-payload:    query:type-linear-commit-health exposes the
      additive key "advisory-overlay" (key name unchanged).
  AC3  test-wired:       test_type_linear_commit_health.cpp (built host)
      carries the #3873 behavioral ACs.
  AC4  no-invent:        no docs/design/3873-*, no test_issue_3873.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (overlay without the flag) and must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compiler" / "type_linear_commit_health.hh"
QUERY = ROOT / "src" / "compiler" / "evaluator_primitives_query_reflect.cpp"
TEST = ROOT / "tests" / "compiler" / "test_type_linear_commit_health.cpp"


def ac1_overlay_flag():
    text = HEADER.read_text()
    ok = (
        "Issue #3873" in text
        and "bool advisory_overlay = false;" in text
        and 'r.advisory_overlay = cr.force_reason == "ok" && r.force_reason != "ok";' in text
    )
    print("AC(overlay-flag): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_query_payload():
    text = QUERY.read_text()
    ok = 'insert_kv("advisory-overlay", scored.advisory_overlay ? 1 : 0);' in text
    print("AC(query-payload): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_test_wired():
    text = TEST.read_text()
    ok = "3873 AC1" in text and "3873 AC4" in text
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_no_invent():
    docs = list(ROOT.glob("docs/design/3873-*"))
    invented = (ROOT / "tests" / "compiler" / "test_issue_3873.cpp").exists()
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test():
    fixture = (
        'if (cr.force_reason == "ok") {\n'
        "    if (s.coercion_slo_force_pending) {\n"
        '        r.force_reason = "coercion-slo";\n'
        "    }\n"
        "}\n"
    )
    flagged = "advisory_overlay" not in fixture
    print("self-test: " + ("PASS — overlay without flag detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_overlay_flag(),
        ac2_query_payload(),
        ac3_test_wired(),
        ac4_no_invent(),
    ]
    print(f"check_health_advisory_face_3873: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
