#!/usr/bin/env python3
"""Issue #3903 — capture_security_correlated_audit carries tenant_id.

Residual (#3874/#3879 follow-on): the correlate API
capture_security_correlated_audit had no tenant argument — forced
capture defaulted tenant_id=0, so Typed-alone tenant filters showed 0
on WAL-miss / EffectDeny correlate rows even when the SE carried the
real principal.

Shipped: tail param std::uint32_t tenant_id = 0, passed through to
capture_audit_event_forced; all six call sites pass the real tenant
(ev->capability_tenant_id() / assigned / tenant_id / tenant /
capability_tenant_id()). Mid join and privilege fail-closed
unchanged; omitted-tenant callers keep tenant_id=0.

  AC1  site-fix: typed_mutation_audit.h cites Issue #3903, the
      correlate signature carries tenant_id, and the forced call
      passes it through.
  AC2  call sites: evaluator_security.cpp (4 sites) and
      evaluator_fiber_mutation.cpp (2 sites) cite Issue #3903.
  AC3  test-wired + no-invent: test_audit_mutation_id_unify.cpp
      (built member of test_security_capability_batch) carries the
      #3903 behavioral ACs; no docs/design/3903-*, no tests/issues/
      test_issue_3903.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (correlate without tenant passthrough) and must
flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TMA = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
ES = ROOT / "src" / "compiler" / "evaluator_security.cpp"
FM = ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp"
TEST = ROOT / "tests" / "compiler" / "test_audit_mutation_id_unify.cpp"
CITE = "Issue #3903"


def ac1_site_fix() -> bool:
    text = TMA.read_text()
    ok = (
        CITE in text
        and "std::uint32_t tenant_id = 0) noexcept" in text
        and "/*tenant_id=*/tenant_id); // Issue #3903" in text
    )
    print("AC(site-fix): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_call_sites() -> bool:
    es = ES.read_text()
    fm = FM.read_text()
    ok = es.count(CITE) >= 4 and fm.count(CITE) >= 2
    print("AC(call-sites): " + ("PASS" if ok else "FAIL") + f" (es={es.count(CITE)}, fm={fm.count(CITE)})")
    return ok


def ac3_test_wired_no_invent() -> bool:
    text = TEST.read_text()
    wired = (
        "3903 AC1" in text
        and "3903 AC2" in text
        and "ac3903_1_correlate_tenant_passthrough" in text
        and "ac3903_2_default_tenant_zero_unchanged" in text
    )
    docs = list(ROOT.glob("docs/design/3903-*"))
    invented = (ROOT / "tests" / "issues" / "test_issue_3903.cpp").exists()
    ok = wired and not docs and not invented
    print("AC(test-wired/no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test() -> int:
    fixture = (
        "inline void capture_security_correlated_audit(std::uint64_t mutation_id, "
        "std::string_view op,\n"
        "                                              std::uint64_t epoch, bool denied,\n"
        "                                              std::uint32_t target_node = 0,\n"
        "                                              std::int64_t fiber_id = 0) noexcept {\n"
        "  capture_audit_event_forced(..., fiber_id, 0);\n"
        "}\n"
    )
    flagged = "/*tenant_id=*/tenant_id" not in fixture
    print("self-test: " + ("PASS — correlate without tenant passthrough detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_site_fix(),
        ac2_call_sites(),
        ac3_test_wired_no_invent(),
    ]
    print(f"check_correlate_tenant_passthrough_3903: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
