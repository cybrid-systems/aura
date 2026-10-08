#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4380: the #4233 tenant exec jail scanned commands token-by-token,
# but quotes are not meta — an absolute path hidden MID-TOKEN scanned clean:
#   python3 -c 'open("/etc/passwd").read()'
#   perl -e 'open(F,"/etc/passwd");print <F>'
# The exec child chdir's to the tenant root, but chdir is not a root jail —
# PATH is inherited, the interpreter opens the absolute path on the host.
# Second stage: the file face's lexical resolve + O_NOFOLLOW guard only the
# FINAL path component, so a symlink PRE-PLACED under the tenant root turns
# relative `e/passwd` into a host read/write outside the prefix with zero
# tenant-path-escape IsolationDeny.
#
# AC1 — the escape scanner (tenant_exec_cmd_escapes_root, tenant_host_path.hh
#       SSOT) denies any `/` that is not clearly part of a relative path:
#       token-initial (existing #4233 family) or preceded by a byte outside
#       [A-Za-z0-9._-] (quote, '=', '(', ':', ...) — embedded absolute paths
#       in interpreter arguments / quoted prose deny; `sub/file`, `./x`
#       relative-only contract stays.
# AC2 — Evaluator::check_tenant_host_path consults the symlink-component
#       walk (tenant_path_has_symlink_component) on resolved tenant paths
#       under active policy; symlink components (intermediate or final) fall
#       into the shared deny face (IsolationDeny tenant-path-escape row).
# AC3 — the walk lives in the tenant_host_path.hh SSOT next to the scanner
#       (no second model), cites #4380, and lstat's with AT_SYMLINK_NOFOLLOW
#       semantics (::lstat + S_ISLNK); missing components are not an escape.
# AC4 — runtime ACs extend the #3802 family host test file
#       (tests/core/test_tenant_isolation_enforcement.cpp) with the issue
#       repro commands + symlink-component denies; no
#       tests/core/test_issue_4380.cpp, no docs/design/4380-*.
# AC5 — build.py wires this linter and the root allowlist carries it.
#
# Self-test:
#   python3 scripts/check_tenant_jail_4380.py

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


failures: list[str] = []
passes = 0


def must(cond: bool, label: str) -> None:
    global passes
    if cond:
        passes += 1
    else:
        failures.append(label)


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> int:
    hh_raw = read("src/compiler/tenant_host_path.hh")
    hh = _strip_cpp_comments(hh_raw)

    # ── AC1: the escape scanner denies embedded absolute paths ──
    must("Issue #4380" in hh_raw, "AC1: scanner SSOT cites #4380")
    scan = hh.find("bool tenant_exec_cmd_escapes_root(")
    must(scan != -1, "AC1: tenant_exec_cmd_escapes_root defined in the SSOT")
    if scan != -1:
        body = hh[scan:]
        body = body[: body.find("\n[[nodiscard]]", 1)] if body.find("\n[[nodiscard]]", 1) != -1 else body
        must("embedded absolute path (#4380)" in hh_raw, "AC1: mid-token escape cite on the deny path")
        must("return true; // absolute path token" in hh_raw, "AC1: token-initial deny kept")
        must("rel_cont" in body, "AC1: relative-continuation byte set gates the deny")
        must(
            "prev == '.'" in body and "prev == '_'" in body and "prev == '-'" in body,
            "AC1: '.', '_', '-' stay relative-safe (sub/file, ./x contract)",
        )

    # ── AC2: the host-path gate consults the symlink walk ──
    sec_raw = read("src/compiler/evaluator_security.cpp")
    sec = _strip_cpp_comments(sec_raw)
    must("Issue #4380" in sec_raw, "AC2: security TU cites #4380")
    gate = sec.find("bool Evaluator::check_tenant_host_path(")
    must(gate != -1, "AC2: check_tenant_host_path defined")
    if gate != -1:
        body = sec[gate:]
        body = body[: body.find("\nbool ", 1)] if body.find("\nbool ", 1) != -1 else body
        must("tenant_path_has_symlink_component(" in body, "AC2: gate consults the symlink-component walk")
        must("TenantHostPathVerdict::Resolved" in body, "AC2: walk runs on resolved tenant paths")
        must("kTenantPathEscapeReason" in body, "AC2: shared deny reason retained")
        must("SecurityEventKind::IsolationDeny" in body, "AC2: IsolationDeny row retained")
        must("capture_security_correlated_audit" in body, "AC2: Typed correlate retained")

    # ── AC3: the walk lives in the SSOT with lstat semantics ──
    must("tenant_path_has_symlink_component" in hh, "AC3: walk defined in the SSOT")
    walk = hh.find("bool tenant_path_has_symlink_component(")
    if walk != -1:
        body = hh[walk:]
        body = body[: body.find("\n//", 1)] if body.find("\n//", 1) != -1 else body
        must("::lstat(" in body, "AC3: lstat (AT_SYMLINK_NOFOLLOW semantics) — no follow")
        must("S_ISLNK" in body, "AC3: symlink component detected via S_ISLNK")
        must("starts_with(root)" in body, "AC3: walk bounded below the tenant root")

    # ── AC4: runtime ACs extend the family host test file ──
    tf = read("tests/core/test_tenant_isolation_enforcement.cpp")
    must("#4380" in tf, "AC4: host test file cites #4380")
    must('open(\\"/etc/passwd\\")' in tf, "AC4: issue repro command pinned")
    must("tenant_path_has_symlink_component" in tf, "AC4: symlink walk pinned in ACs")
    must(not (ROOT / "tests/core/test_issue_4380.cpp").exists(), "AC4: no standalone test_issue_4380.cpp (#81934)")
    design = ROOT / "docs" / "design"
    must(not design.exists() or not any(design.glob("4380-*")), "AC4: no docs/design/4380-* (#1655)")

    # ── AC5: wiring ──
    build = read("build.py")
    must("check_tenant_jail_4380" in build, "AC5: build.py wires the #4380 linter")
    allow = read("scripts/coverage/root_check_allowlist.txt")
    must("check_tenant_jail_4380.py" in allow, "AC5: linter on the root_check_allowlist")

    print(f"check_tenant_jail_4380: {passes} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
