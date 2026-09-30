#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4233: EXEMPT_2ARG shell / command-output gained require_effect(Exec)
# (#3836) but never called check_tenant_host_path (#3802) — under
# Restricted+MT / Strict a tenant holding Exec could execl/popen against the
# shared host FS (clobber another tenant's files / escape the tenant root)
# while write-file / sys-* denied the same path with IsolationDeny
# tenant-path-escape.
#
# AC1 — the shell prim body runs check_tenant_exec_jail AFTER the
#       require_effect(Exec) choke and BEFORE the exec child fork; deny is
#       exit-code -1 (zero exec) and the IsolationDeny SE carries the audit.
# AC2 — the command-output prim body runs the same jail fence after its
#       Exec choke and before the exec child fork; popen is gone (no
#       pre-exec hook for the child chdir) — capture runs through the
#       jailed fork+pipe pattern with stdout + trailing-newline contract.
# AC3 — both exec children chdir under the caller's tenant root
#       (relative-only contract) via ::chdir(jail_root.c_str()).
# AC4 — Evaluator::check_tenant_exec_jail reuses the #3802 policy
#       predicate (tenant_host_path_policy_active + resolve_tenant_host_path,
#       no second capability model) and the deny joins the shared
#       IsolationDeny row (mid+tenant+fiber+epoch, kEffectExec face,
#       Typed correlate) with the shared tenant-path-escape reason.
# AC5 — the lexical escape scanner (tenant_exec_cmd_escapes_root) lives in
#       the tenant_host_path.hh SSOT and cites #4233; shared reason.
# AC6 — EXEMPT_2ARG inventory unchanged: shell/command-output stay EXEMPT
#       (no NodeId target), the frozen count guard stays intact, no new
#       exempt ops added by the jail fence.
# AC7 — build.py wires this linter, the root allowlist carries it, the
#       runtime ACs extend the #3802 family host test file
#       (tests/core/test_tenant_isolation_enforcement.cpp), no
#       tests/core/test_issue_4233.cpp, no docs/design/4233-*.
#
# Self-test:
#   python3 scripts/check_tenant_exec_jail_4233.py

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
    src = read("src/compiler/evaluator_primitives_file.cpp")
    stripped = _strip_cpp_comments(src)

    # ── slice the two exec prim bodies ──
    sh_start = stripped.find('defer_std_host_prim("shell"')
    co_start = stripped.find('"command-output"')
    dl_end = stripped.find('"directory-list"')
    must(
        sh_start != -1 and co_start != -1 and dl_end != -1 and sh_start < co_start < dl_end,
        "exec prim slices located",
    )
    if -1 in (sh_start, co_start, dl_end):
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    shell_body = stripped[sh_start:co_start]
    co_body = stripped[co_start:dl_end]

    # ── AC1: shell jail fence after the Exec choke, before the child fork ──
    sh_eff = shell_body.find('require_effect(kEffectExec, "shell")')
    sh_jail = shell_body.find('check_tenant_exec_jail(ev.string_heap_[idx], jail_root, "shell")')
    sh_fork = shell_body.find("::fork()")
    must(sh_eff != -1, "AC1: shell require_effect(Exec) present")
    must(sh_jail != -1, 'AC1: check_tenant_exec_jail(..., "shell") present')
    must(sh_eff != -1 and sh_jail != -1 and sh_eff < sh_jail, "AC1: jail fence follows the Exec choke")
    must(
        sh_fork != -1 and sh_jail != -1 and sh_jail < sh_fork,
        "AC1: jail fence precedes the exec child fork (zero exec on deny)",
    )

    # ── AC2: command-output jail fence + jailed fork+pipe capture ──
    co_eff = co_body.find('require_effect(kEffectExec, "command-output")')
    co_jail = co_body.find('check_tenant_exec_jail(cmd, jail_root, "command-output")')
    co_fork = co_body.find("::fork(")
    must(co_eff != -1, "AC2: command-output require_effect(Exec) present")
    must(co_jail != -1, 'AC2: check_tenant_exec_jail(..., "command-output") present')
    must(co_eff != -1 and co_jail != -1 and co_eff < co_jail, "AC2: jail fence follows the Exec choke")
    must(
        co_fork != -1 and co_jail != -1 and co_jail < co_fork,
        "AC2: jail fence precedes the exec child fork (zero exec on deny)",
    )
    must("::popen(" not in co_body, "AC2: popen is gone from the jailed capture body")
    must("::pipe(" in co_body and "::dup2(" in co_body, "AC2: capture runs through the fork+pipe pattern")
    must("result.back() == " + chr(39) + "\\n" + chr(39) in co_body, "AC2: trailing-newline trim stays contract")

    # ── AC3: both exec children chdir under the tenant root ──
    chdirs = stripped.count("::chdir(jail_root.c_str())")
    must(chdirs >= 2, "AC3: shell + command-output children chdir under the jail root")

    # ── AC4: the evaluator gate reuses the #3802 policy, joins the shared row ──
    sec = read("src/compiler/evaluator_security.cpp")
    sec_stripped = _strip_cpp_comments(sec)
    must("Issue #4233" in sec, "AC4: security TU cites #4233")
    js = sec_stripped.find("bool Evaluator::check_tenant_exec_jail(")
    must(js != -1, "AC4: Evaluator::check_tenant_exec_jail defined")
    if js != -1:
        body = sec_stripped[js:]
        body = body[: body.find("\nbool ", 1)] if body.find("\nbool ", 1) != -1 else body
        must("tenant_host_path_policy_active" in body, "AC4: jail reuses the #3802 policy predicate (no second model)")
        must(
            'resolve_tenant_host_path(".", capability_tenant_id_' in body,
            "AC4: jail root resolves through the shared resolver",
        )
        must("tenant_exec_cmd_escapes_root" in body, "AC4: lexical escape scanner consulted")
        must("kTenantPathEscapeReason" in body, "AC4: shared tenant-path-escape reason")
        must("SecurityEventKind::IsolationDeny" in body, "AC4: deny joins the IsolationDeny row")
        must("kEffectExec" in body, "AC4: exec effect face stamped on the row")
        must("capture_security_correlated_audit" in body, "AC4: Typed correlate joins mid+tenant+fiber+epoch")
        must(
            "bump_capability_denial" in body and "last_mutate_error_" in body,
            "AC4: deny carries last_mutate_error + denial counter",
        )

    # ── AC5: escape scanner lives in the tenant_host_path.hh SSOT ──
    hh = read("src/compiler/tenant_host_path.hh")
    must("Issue #4233" in hh, "AC5: header cites #4233")
    must("tenant_exec_cmd_escapes_root" in hh, "AC5: escape scanner in the shared SSOT")
    must('kTenantPathEscapeReason = "tenant-path-escape"' in hh, "AC5: shared deny reason defined once in the SSOT")

    # ── AC6: EXEMPT_2ARG inventory unchanged ──
    inv = read("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py")
    m_start = inv.find("EXEMPT_2ARG_OPS")
    must(
        m_start != -1 and "shell" in inv[m_start:] and '"command-output"' in inv[m_start:],
        "AC6: shell/command-output stay EXEMPT (no NodeId target)",
    )
    must("len(EXEMPT_2ARG_OPS) != 7" in inv, "AC6: frozen exempt-count guard intact")

    # ── AC7: wiring + runtime AC host file + no invent ──
    build = read("build.py")
    must("check_tenant_exec_jail_4233" in build, "AC7: build.py wires the #4233 linter")
    allow = read("scripts/coverage/root_check_allowlist.txt")
    must("check_tenant_exec_jail_4233.py" in allow, "AC7: linter on the root_check_allowlist")
    tf = read("tests/core/test_tenant_isolation_enforcement.cpp")
    must("#4233" in tf and "tenant-path-escape" in tf, "AC7: runtime ACs extend the #3802 family host test file")
    must(not (ROOT / "tests/core/test_issue_4233.cpp").exists(), "AC7: no standalone test_issue_4233.cpp (#81934)")
    design = ROOT / "docs" / "design"
    must(not design.exists() or not any(design.glob("4233-*")), "AC7: no docs/design/4233-* (#1655)")

    print(f"check_tenant_exec_jail_4233: {passes} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
