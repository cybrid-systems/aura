#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4381: the git-* primitives (git-status, git-diff, git-log,
# git-commit, git-branch-current, git-stage, git-rev-parse) forked /
# popen'd / ran libgit2 against the SHARED PROCESS cwd — the host repo —
# with no tenant jail: under Restricted+MT / Strict a second tenant in
# the same process read and mutated the host git worktree instead of
# <tenant fs root>/t-<id>, and git-stage passed absolute / ../ path
# arguments straight to `git add` with no prefix check.
#
# AC1 — every git-* body consults check_tenant_exec_jail (the #4233
#       shell fence, no second model) BEFORE any repo access; on deny
#       the prim returns its legacy failure value (void / -1) with the
#       shared IsolationDeny row. Fixed argv per prim (no caller text in
#       the scanned command); git-commit keeps the #2072
#       require_effect(Exec|Network) choke FIRST, then the fence.
# AC2 — under an armed jail the child runs through run_git_jailed:
#       post-fork chdir under the caller's tenant root BEFORE execvp,
#       parent process cwd never mutated; the libgit2 in-process backend
#       (process-global repo state, not fiber-safe) is skipped on the
#       jailed path and stays Soft/Off-only.
# AC3 — git-stage resolves EVERY path argument through
#       check_tenant_host_path (#3802 face): absolute / cross-tenant /
#       prefix-escape paths deny IsolationDeny tenant-path-escape with
#       zero exec; surviving paths go to the jailed `git add` child as
#       tenant-rooted absolutes.
# AC4 — Soft/Off keeps the legacy passthrough: libgit2 / popen /
#       fork+execvp bodies unchanged on the jail_root-empty branch, and
#       the #1161 git-stage-no-shell fact (fork+execvp, no shell) is
#       preserved.
# AC5 — runtime ACs extend the #3802 family host test file
#       (tests/core/test_tenant_isolation_enforcement.cpp): jailed
#       lifecycle (status/rev-parse/stage/commit/log against a real
#       tenant-root repo), escape denies with SE rows, Soft/Off
#       passthrough; no tests/core/test_issue_4381.cpp, no
#       docs/design/4381-*.
# AC6 — build.py wires this linter and the root allowlist carries it.
#
# Self-test:
#   python3 scripts/check_git_tenant_jail_4381.py

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

GIT_PRIMS = [
    "git-status",
    "git-diff",
    "git-log",
    "git-commit",
    "git-branch-current",
    "git-stage",
    "git-rev-parse",
]

failures: list[str] = []
passes = 0


def must(cond: bool, label: str) -> None:
    global passes
    if cond:
        passes += 1
    else:
        failures.append(label)


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


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> int:
    src = read("src/compiler/evaluator_primitives_io.cpp")
    stripped = _strip_cpp_comments(src)

    # ── slice the seven git prim bodies (in registration order) ──
    spans: dict[str, tuple[int, int]] = {}
    starts = []
    ok = True
    for name in GIT_PRIMS:
        marker = f'defer_std_host_prim("{name}"'
        idx = stripped.find(marker)
        if idx == -1:
            ok = False
            break
        starts.append((name, idx))
    must(ok, "AC1: all seven git prim bodies located")
    if not ok:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    for i, (name, idx) in enumerate(starts):
        end = starts[i + 1][1] if i + 1 < len(starts) else len(stripped)
        spans[name] = (idx, end)

    # ── AC1: fence consulted before any repo access in every body ──
    for name in GIT_PRIMS:
        lo, hi = spans[name]
        body = stripped[lo:hi]
        jail = body.find("check_tenant_exec_jail(")
        must(jail != -1, f"AC1: {name} consults check_tenant_exec_jail")
        must(f'"{name}")' in body, f"AC1: {name} fence stamps its op name")
        if name == "git-commit":
            eff = body.find("require_effect(")
            must(eff != -1 and eff < jail, "AC1: git-commit keeps the #2072 effect choke before the fence")
        else:
            ctx = body.find("GitContext")
            popen = body.find("::popen(")
            first_io = min(x for x in (ctx, popen) if x != -1) if (ctx != -1 or popen != -1) else hi
            must(jail < first_io, f"AC1: {name} fences before any repo access")

    # ── AC2: jailed child chdir's under the tenant root; parent untouched ──
    helper = stripped.find("int run_git_jailed(")
    must(helper != -1, "AC2: run_git_jailed helper defined")
    if helper != -1:
        body = stripped[helper : helper + 4000]
        must("::chdir(jail_root.c_str())" in body, "AC2: child chdir under the tenant root")
        must("::fork()" in body and "::execvp(" in body, "AC2: fork+execvp (no shell)")
        must("::pipe(" in body, "AC2: stdout capture pipe")
    jailed_uses = (
        stripped.count("run_git_jailed({")
        + stripped.count("run_git_jailed(args,")
        + stripped.count("run_git_jailed(path_bufs")
        + stripped.count("run_git_jailed(std::vector")
    )
    must(jailed_uses >= 7, f"AC2: all seven prims run the jailed child (found {jailed_uses})")

    # ── AC3: git-stage resolves every path through the host-path gate ──
    lo, hi = spans["git-stage"]
    stage_body = stripped[lo:hi]
    must(
        'check_tenant_host_path(p, resolved, "git-stage")' in stage_body,
        "AC3: git-stage paths resolve through check_tenant_host_path",
    )
    jail_idx = stage_body.find("check_tenant_exec_jail(")
    path_idx = stage_body.find("check_tenant_host_path(")
    must(jail_idx != -1 and path_idx != -1 and jail_idx < path_idx, "AC3: git-stage fence precedes path resolution")

    # ── AC4: Soft/Off legacy passthrough preserved ──
    must("::popen(" in stripped, "AC4: legacy popen fallback kept for Soft/Off")
    must(
        "git_stage_no_shell" in read("src/compiler/observability_metrics.h"),
        "AC4: #1161 git-stage-no-shell metric fact retained",
    )

    # ── AC5: runtime ACs in the family host test file ──
    tf = read("tests/core/test_tenant_isolation_enforcement.cpp")
    must("#4381" in tf, "AC5: host test file cites #4381")
    must("git-status runs jailed" in tf or "jailed git-status" in tf, "AC5: jailed git lifecycle ACs present")
    must(not (ROOT / "tests/core/test_issue_4381.cpp").exists(), "AC5: no standalone test_issue_4381.cpp (#81934)")
    design = ROOT / "docs" / "design"
    must(not design.exists() or not any(design.glob("4381-*")), "AC5: no docs/design/4381-* (#1655)")

    # ── AC6: wiring ──
    build = read("build.py")
    must("check_git_tenant_jail_4381" in build, "AC6: build.py wires the #4381 linter")
    allow = read("scripts/coverage/root_check_allowlist.txt")
    must("check_git_tenant_jail_4381.py" in allow, "AC6: linter on the root_check_allowlist")

    print(f"check_git_tenant_jail_4381: {passes} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
