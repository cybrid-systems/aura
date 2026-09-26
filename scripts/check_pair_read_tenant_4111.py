#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4111: interpreter car/cdr reads of the process-shared g_pair_slots
# table had NO tenant consult - a foreign (or unstamped) slot's content was
# returned verbatim under the production face, a confidentiality bypass vs
# the #4057 write gate (same process-shared index space, dual-track).
#
# AC1 - car and cdr gate the shared-slot read under the production face
#       (sandbox != 0 and Strict or Restricted+MT - the identical #4057
#       predicate), comparing the slot stamp against the caller principal
#       BEFORE the read; the deny rides check_workspace_isolation with
#       required_effects = 0 (no effect consume; dispatch stays the Mutate
#       choke for writers) and the error string cites #4111.
# AC2 - ordering: the face probe precedes the tenant-array load, which
#       precedes the isolation call, which precedes the slot read - Soft/Off
#       never reads the tenant array (zero-cost contract) and a denied read
#       never dereferences the slot.
# AC3 - the local pairs_ branch stays first and unconditional (own-pair
#       reads unchanged); the reader lambdas capture &ev.
# AC4 - the caar/cadr/... shorthands never fall through to g_pair_slots
#       (documented no-fallthrough; no shorthand touches the tenant array).
# AC5 - no new query keys / metrics fields in the reader gates (the deny
#       reuses the isolation counters via check_workspace_isolation).
# AC6 - the #4057 write face is untouched (set-car!/set-cdr! compare + the
#       Mutate-choke calls keep their original counts).
# AC7 - ACs extend tests/compiler/test_dispatch_required_effects.cpp (per
#       #81934); no test_issue_4111.cpp; no docs/design/4111-* (per #1655).
#
# Self-test:
#   python3 scripts/check_pair_read_tenant_4111.py
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


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    pair_raw = _read("src/compiler/evaluator_primitives_pair.cpp")
    if not pair_raw:
        print("check_pair_read_tenant_4111: FAIL (pair source missing)")
        return 1
    pair = _strip_cpp_comments(pair_raw)

    # writer-gate vocabulary (token spellings come from the file itself)
    m = re.search(
        r"const std::uint64_t (\w+) =\n +\((\w+) < ([\w:]+)\.size\(\)\) \? \3\[\2\] : 0;",
        pair,
    )
    if not m:
        fails.append("vocab: #4057 tenant fragment not found")
        print("check_pair_read_tenant_4111: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    _vt, tenants_tok = m.group(1), m.group(3)
    m = re.search(r"if \((\w+) && (\w+) != (ev\.\w+\(\))\) \{", pair)
    assert m
    face_name = m.group(1)
    m = re.search(r"\(void\)ev\.(\w+)\(", pair)
    assert m
    iso_tok = m.group(1)

    for op in ("car", "cdr"):
        anchor = 'add, ev, "' + op + '"'
        a = pair.find(anchor)
        if a < 0:
            fails.append("AC1: " + op + " registration not found")
            continue
        t = pair.find("pure_general(1,", a)
        win = pair[a:t]
        raw_a = pair_raw.find(anchor)
        raw_win = pair_raw[raw_a : raw_a + (t - a) + 400]

        # AC1: the gate exists and rides the isolation path
        if face_name not in win:
            fails.append("AC1: " + op + " lacks the production-face probe")
        if tenants_tok not in win:
            fails.append("AC1: " + op + " never reads the slot tenant array")
        if iso_tok not in win:
            fails.append("AC1: " + op + " deny does not ride " + iso_tok)
        if "4111" not in raw_win:
            fails.append("AC1: " + op + " gate lacks the #4111 citation")
        if "required_effects" not in raw_win:
            fails.append("AC1: " + op + " deny lacks the required_effects=0 marker")
        if "[&ev, " not in win:
            fails.append("AC3: " + op + " lambda does not capture &ev")
        # AC2: probe -> tenant load -> isolation -> read
        p1 = win.find(face_name)
        p2 = win.find(tenants_tok)
        p3 = win.find(iso_tok)
        m4 = re.search(r"return [\w:]+\{[\w:]+\[id\]->" + op + r"\};", win)
        p4 = m4.start() if m4 else -1
        if -1 in (p1, p2, p3, p4):
            fails.append("AC2: " + op + " gate shape incomplete (positions)")
        elif not (p1 < p2 < p3 < p4):
            fails.append("AC2: " + op + " ordering is not probe<load<deny<read")
        # AC3: local pairs_ branch first and unconditional
        m5 = re.search(r"if \(id < (\w+)\.size\(\)\)\n +return \1\[id\]\." + op + ";", win)
        if not m5:
            fails.append("AC3: " + op + " local-branch shape not found")
        elif m5.start() > p1:
            fails.append("AC3: " + op + " local branch no longer precedes the gate")
        # AC5: no new metrics/query surface in the reader gates
        if "metrics" in win.lower() or "query:" in win:
            fails.append("AC5: " + op + " gate introduces metrics/query surface")

    # AC4: shorthands never touch the shared table; doc comment present
    # (section order: pair? precedes the shorthands; the section runs to
    # the set-car! registration)
    i_caar = pair.find('"caar"')
    i_sc = pair.find('"set-car!"')
    if i_caar < 0 or i_sc < 0 or i_caar > i_sc:
        fails.append("AC4: shorthand section anchors not found")
    elif tenants_tok in pair[i_caar:i_sc]:
        fails.append("AC4: a shorthand body touches the shared slot table")
    if ("never fall through to " + tenants_tok) not in pair_raw:
        fails.append("AC4: no-fallthrough documentation missing")

    # AC6: the #4057 write face is untouched (per-writer-window checks;
    # the readers legitimately add their own owner-compares)
    m = re.search(r"if \((\w+) && (\w+) != (ev\.\w+\(\))\) \{", pair)
    assert m
    cmp_txt = m.group(2) + " != " + m.group(3)
    m = re.search(r"aura::[\w:]*security::kEffect\w+", pair)
    choke_tok = m.group(0) if m else ""
    for nm in ("set-car!", "set-cdr!"):
        a = pair.find('add, ev, "' + nm + '"')
        if a < 0:
            fails.append("AC6: " + nm + " registration not found")
            continue
        win = pair[a : a + 2600]
        if cmp_txt not in win:
            fails.append("AC6: " + nm + " owner-compare missing")
        if choke_tok and choke_tok not in win:
            fails.append("AC6: " + nm + " Mutate-choke call missing")
    if pair_raw.count("4057") < 4:
        fails.append("AC6: #4057 citations thinned in the pair source")

    # AC7: family extension, no stray files
    test_src = _read("tests/compiler/test_dispatch_required_effects.cpp")
    if test_src.count("4111") < 12:
        fails.append("AC7: dispatch-family test lacks the #4111 AC block")
    for stray in (
        (ROOT / "tests" / "core").glob("test_issue_4111*.cpp"),
        (ROOT / "tests" / "compiler").glob("test_issue_4111*.cpp"),
        (ROOT / "tests" / "issues").glob("test_issue_4111*.cpp"),
    ):
        if any(stray):
            fails.append("AC7: test_issue_4111.cpp exists (extend the family, #81934)")
    if any((ROOT / "docs" / "design").glob("4111-*")):
        fails.append("AC7: docs/design/4111-* exists (banned per #1655)")

    if fails:
        print("check_pair_read_tenant_4111: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("check_pair_read_tenant_4111: OK (AC1-AC7)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
