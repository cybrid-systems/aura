#!/usr/bin/env python3
"""Issue #3835: read/recon host-path isolation (mirror #3802 write-file).

Under Restricted+MT / Strict, read-file / file-exists? / file-size /
directory-list resolve via Evaluator::check_tenant_host_path (same policy
as #3802 writes). Cross-tenant escape → IsolationDeny SE
(tenant-path-escape), void/false/empty. Soft/Off / single-tenant Restricted
passthrough — Soft contract unchanged. Write paths retain their #3802 check.

Contract (one row per AC):
  AC1  Restricted+MT tenant A cannot read under tenant B prefix; deny + SE
  AC2  Soft/Off / single-tenant Restricted keep passthrough
  AC3  write paths still call check; read/recon wired; no invent / docs

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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    filep = _read("src/compiler/evaluator_primitives_file.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")
    sec = _read("src/compiler/evaluator_security.cpp")

    # ── AC1 ──
    must("Issue #3835", "AC1 cite read-file", filep)
    must('check_tenant_host_path(path, resolved, "read-file")', "AC1 read-file", filep)
    must('check_tenant_host_path(path, resolved, "file-exists?")', "AC1 file-exists?", filep)
    must('check_tenant_host_path(path, resolved, "file-size")', "AC1 file-size", filep)
    must(
        'check_tenant_host_path(dir_path, resolved, "directory-list")',
        "AC1 directory-list",
        filep,
    )
    must("3835 AC1", "AC1 test", test)
    must("IsolationDeny", "AC1 SE kind retained", sec)
    must("tenant-path-escape", "AC1 SE reason retained", sec)
    # open/lstat/readdir must use resolved (not raw path after check)
    if not re.search(
        r'check_tenant_host_path\(path, resolved, "read-file"\)[\s\S]{0,400}?::open\(resolved\.c_str\(\)',
        filep,
    ):
        fails.append("AC1: read-file does not open(resolved) after check")
    if not re.search(
        r'check_tenant_host_path\(path, resolved, "file-exists\?"\)[\s\S]{0,400}?::lstat\(resolved\.c_str\(\)',
        filep,
    ):
        fails.append("AC1: file-exists? does not lstat(resolved) after check")
    if not re.search(
        r'check_tenant_host_path\(path, resolved, "file-size"\)[\s\S]{0,500}?::lstat\(resolved\.c_str\(\)',
        filep,
    ):
        fails.append("AC1: file-size does not lstat(resolved) after check")
    if not re.search(
        r'check_tenant_host_path\(dir_path, resolved, "directory-list"\)[\s\S]{0,400}?opendir\(resolved\.c_str\(\)',
        filep,
    ):
        fails.append("AC1: directory-list does not opendir(resolved) after check")

    # ── AC2 Soft/Off ──
    must("3835 AC2", "AC2 test", test)
    must("Soft/Off", "AC2 Soft cite", test)
    must("single-tenant Restricted", "AC2 ST Restricted", test)
    # Soft contract: check_tenant_host_path itself still gates via policy_active
    must("tenant_host_path_policy_active", "AC2 policy gate", sec)

    # ── AC3 write retained + wiring ──
    must(
        'check_tenant_host_path(path, resolved_path, "write-file")',
        "AC3 write-file still checks",
        filep,
    )
    must("check_tenant_host_path_read_3835", "AC3 build.py", build)
    must("Issue #3835", "AC3 build cite", build)
    must("check_tenant_host_path_read_3835.py", "AC3 grandfather basename", gf)
    must(
        "scripts/coverage/checks/check_tenant_host_path_read_3835.py",
        "AC3 grandfather path",
        gf,
    )
    must("3835 AC3", "AC3 test", test)
    must_not("schema-3835", "AC3 no schema key", _read("src/compiler/evaluator_primitives_security.cpp"))
    must_not("issue-3835", "AC3 no issue key", _read("src/compiler/evaluator_primitives_security.cpp"))
    for rel in (
        "tests/core/test_issue_3835.cpp",
        "tests/compiler/test_issue_3835.cpp",
        "tests/issues/test_issue_3835.cpp",
    ):
        if _read(rel):
            fails.append(f"AC3: {rel} exists — forbidden")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3835-*.md"):
            fails.append(f"AC3: {f.relative_to(ROOT)} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3835 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3835 read/recon tenant host-path isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
