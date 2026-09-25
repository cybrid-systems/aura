#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4059: load read any host path behind a bare io-read string gate
# (only path_is_denied in front), then installed the parse result as the
# current workspace and eval_flat'ed it — no check_tenant_host_path, no
# Mutate. Under Restricted+MT / Strict, cross-tenant reads and workspace
# replacement happened with zero IsolationDeny / EffectDeny.
#
# AC1 — the load prim body runs check_tenant_host_path(path, resolved,
#       "load") BEFORE any read or install; deny is a silent void (the SE
#       row carries the audit) and the workspace pointers never move.
# AC2 — the body pays require_effect(Mutate, op) for the swap AFTER the
#       host-path gate and BEFORE the workspace_flat_/pool_ install;
#       dispatch never stamps Mutate for "load" so the body choke is the
#       sole effect gate (no single-use double-consume).
# AC3 — the ifstream opens the RESOLVED path, not the raw argument.
# AC4 — the effect-deny error uses the existing format_deny_reason
#       builder; no new deny reason literal is introduced in the load
#       body (path denies come from the shared gate's tenant-path-escape).
# AC5 — "load" is NOT added to EXEMPT_2ARG_OPS (#2942 / #2839): the op
#       travels as a variable (the no-target 2-arg shape the linter
#       blesses), and the frozen count guards stay intact.
# AC6 — the io-read string gate stays as a second layer: kCapIoRead /
#       kCapIo / kCapWildcard checks remain in the body.
# AC7 — runtime ACs extend tests/compiler/test_load_cap_io_read.cpp (the
#       #2485 host file, per #81934); no tests/core/test_issue_4059.cpp;
#       no docs/design/4059-* (per #1655).
#
# Self-test:
#   python3 scripts/check_load_tenant_mutate_4059.py

from __future__ import annotations

import re
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
    # ── slice the load prim body ──
    src = read("src/compiler/evaluator_primitives_eval.cpp")
    stripped = _strip_cpp_comments(src)
    start = stripped.find('add("load"')
    end = stripped.find('add("eval-expr"')
    must(start != -1 and end != -1 and start < end, "load slice located")
    if start == -1 or end == -1 or start >= end:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    body = stripped[start:end]

    # ── AC1: host-path gate before any read/install ──
    hp = body.find('check_tenant_host_path(path, resolved_path, "load")')
    must(hp != -1, 'AC1: check_tenant_host_path(path, resolved_path, "load") present')
    iff = body.find("std::ifstream")
    ws = body.find("workspace_flat_ =")
    must(iff != -1 and hp != -1 and hp < iff, "AC1: host-path gate precedes the read")
    must(ws != -1 and hp < ws, "AC1: host-path gate precedes the workspace install")

    # ── AC2: Mutate choke for the swap ──
    re_eff = body.find("require_effect(")
    must(re_eff != -1, "AC2: require_effect present in the load body")
    must(body.find("kEffectMutate") != -1, "AC2: Mutate effect required")
    must(hp != -1 and re_eff != -1 and hp < re_eff, "AC2: Mutate choke comes after the host-path gate")
    must(ws != -1 and re_eff != -1 and re_eff < ws, "AC2: Mutate choke precedes the workspace install")

    # ── AC3: read the resolved path ──
    must("std::ifstream f(resolved_path)" in body, "AC3: ifstream opens the resolved path")
    must("std::ifstream f(path)" not in body, "AC3: raw-path read is gone")

    # ── AC4: existing deny builders only ──
    must("format_deny_reason" in body, "AC4: effect deny uses format_deny_reason")
    must('"tenant-path-escape"' not in body, "AC4: no new deny reason literal in the load body (shared gate owns it)")

    # ── AC5: EXEMPT_2ARG_OPS frozen, load not listed ──
    for lint in (
        "scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py",
        "scripts/coverage/checks/check_side_effect_fiber_principal_2839.py",
    ):
        text = read(lint)
        m = re.search(r"EXEMPT_2ARG_OPS[^=]*=\s*\{(.*?)\n\}", text, re.S)
        must(m is not None and '"load"' not in m.group(1), f"AC5: load not in EXEMPT_2ARG_OPS ({Path(lint).name})")
    must(
        "len(EXEMPT_2ARG_OPS) != 7" in read("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py"),
        "AC5: frozen exempt-count guard intact",
    )

    # ── AC6: io-read string gate stays ──
    must(
        "kCapIoRead" in body and "kCapIo" in body and "kCapWildcard" in body,
        "AC6: io-read string gate stays as the second layer",
    )

    # ── AC7: runtime ACs in the host file; no standalone artifacts ──
    tf = read("tests/compiler/test_load_cap_io_read.cpp")
    must("#4059" in tf and "tenant-path-escape" in tf, "AC7: deny-side runtime ACs extend the #2485 host test file")
    tf2 = read("tests/compiler/test_dispatch_required_effects.cpp")
    must(
        "#4059" in tf2 and "EffectAllow" in tf2 and "note_boundary_audit_mid_for_test" in tf2,
        "AC7: allow-side runtime AC extends the #2152/#4036/#4057 host test file",
    )
    must(not (ROOT / "tests/core/test_issue_4059.cpp").exists(), "AC7: no standalone test_issue_4059.cpp (#81934)")
    design = ROOT / "docs" / "design"
    must(not design.exists() or not any(design.glob("4059-*")), "AC7: no docs/design/4059-* (#1655)")

    print(f"check_load_tenant_mutate_4059: {passes} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
