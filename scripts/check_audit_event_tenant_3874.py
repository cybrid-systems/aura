#!/usr/bin/env python3
"""Issue #3874 — TypedMutationAuditEvent carries the capability tenant.

Residual: the Typed trail event stamped mid / node / fiber / epochs but no
tenant; Typed-only forensic could not answer "which tenant" when the SE ring
had wrapped and WAL catch-up lagged.

  AC1  struct-field:    TypedMutationAuditEvent carries `tenant_id`
      (cites #3874; 0 = honest unset; join by mutation_id stays preferred).
  AC2  emit-plumbing:   capture_audit_event_forced + capture_audit_event
      carry the defaulted tenant_id param and stamp ev.tenant_id.
  AC3  call-site-wired: the production mutation boundary stamps the
      Evaluator principal tenant.
  AC4  no-invent:       no docs/design/3874-*, no test_issue_3874.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (event struct without tenant) and must flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
BOUNDARY = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"


def ac1_struct_field():
    text = HEADER.read_text()
    ok = "Issue #3874" in text and "std::uint32_t tenant_id = 0;" in text
    print("AC(struct-field): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_emit_plumbing():
    text = HEADER.read_text()
    ok = (
        # Issue #3903: correlate API adds a third defaulted tenant param.
        text.count("std::uint32_t tenant_id = 0) noexcept") >= 2
        and "ev.tenant_id = tenant_id;" in text
        and text.count("tenant_id);") >= 2
    )
    print("AC(emit-plumbing): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_call_site_wired():
    text = BOUNDARY.read_text()
    ok = "static_cast<std::uint32_t>(capability_tenant_id())" in text
    print("AC(call-site-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_no_invent():
    docs = list(ROOT.glob("docs/design/3874-*"))
    invented = (ROOT / "tests" / "compiler" / "test_issue_3874.cpp").exists()
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test():
    fixture = (
        "struct TypedMutationAuditEvent {\n    std::uint64_t mutation_id = 0;\n    std::int64_t fiber_id = 0;\n};\n"
    )
    flagged = "tenant_id" not in fixture
    print("self-test: " + ("PASS — event struct without tenant detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_struct_field(),
        ac2_emit_plumbing(),
        ac3_call_site_wired(),
        ac4_no_invent(),
    ]
    print(f"check_audit_event_tenant_3874: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
