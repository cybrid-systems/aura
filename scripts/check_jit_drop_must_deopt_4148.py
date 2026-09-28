#!/usr/bin/env python3
"""Issue #4148 source-cite gate: JIT-native drop arms MustDeopt on live closures.

`drop_jit_fn_native_for_define_locked` (#4083, used by the production facade
`evict_jit_cache_after_production_facade_` #4100, the Soft partial-relower
teardown #1514, the abort-force walk, and the #4146 cone fan-out) erased
`g_jit_fns` / by-name / overflow / closure-cache entries for define F but
never set `g_closure_must_deopt` on matching live closures. When epochs
co-advance (owner-scoped multi-eval freezes C clocks #2841/#2951/#3605),
dual-fresh / `is_fn_epoch_stale` still leave-native and leave-native then
depended only on the null `fn` slot until a later remount/reemit — thinner
than the #2503 remount-fail transaction (MustDeopt + named batch_deopt_for)
and the pure-anon budget-skip (MustDeopt + epoch poison + sticky fence).

#4148 arms the belt inside the existing name-matching walk: same
`jit_key_matches_define` predicate (name / name#), peer resize+store shape
(#2503/#3060), teardown and lock shape unchanged, MustDeopt clear stays the
remount-heal dual-fresh SSOT (#2128), no second closure table, no
process-wide table epoch bump (#2951 owner scope). Soft BFS callers reach
the drop too — the fail-closed set is acceptable; no matching live cids =
empty walk, zero extra work.

ACs:
  AC1  drop_jit_fn_native_for_define_locked cites Issue #4148 and arms
       MustDeopt inside the name-matching walk: peer column resize then
       `g_closure_must_deopt[cid] = 1`.
  AC2  no second model: the arm sits under the existing
       `jit_key_matches_define` walk (name / name# predicate reused); the
       #4083 teardown stays intact (by-name erase, g_jit_fns null,
       overflow erase, closure-cache invalidate/clear).
  AC3  lock shape + scope unchanged: the C ABI still takes the closure
       table lock only when workspace is not already held, and the helper
       never bumps the process table epoch (#2951).
  AC4  clear stays remount-heal SSOT: no MustDeopt clear inside the drop
       helper; the call-path force-deopt consumption (#3247 window) is
       untouched.
  AC5  runtime doors + wiring: test_must_deopt_before_next_call.cpp cites
       #4148 with ac4148_* labels; no tests/compiler/test_issue_4148.cpp;
       no docs/design/4148-*; no new query key; build.py wires this linter
       and scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
TST = ROOT / "tests" / "compiler" / "test_must_deopt_before_next_call.cpp"

DROP_DEF = "static void drop_jit_fn_native_for_define_locked(std::string_view name)"
DROP_ABI = 'extern "C" void aura_drop_jit_fn_native_for_define(const char* name)'
CALL_ENTRY = "int64_t aura_closure_dispatch_native_checked("


def _strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        out.append(line[:cut] if cut >= 0 else line)
    return "\n".join(out)


def main() -> int:
    rt_raw = RT.read_text() if RT.exists() else ""
    rt = _strip_comments(rt_raw)
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        if not good:
            ok = False
        print(f"  [{'PASS' if good else 'FAIL'}] {name}: {msg}")

    # AC1 — the MustDeopt arm lives inside the name-matching walk with the
    # peer resize+store shape (#2503 remount-fail / #3060 drain).
    fn = rt.find(DROP_DEF)
    report("AC1.helper", fn != -1, "drop_jit_fn_native_for_define_locked present")
    report(
        "AC1.cite",
        "Issue #4148" in rt_raw,
        "aura_jit_runtime.cpp cites Issue #4148",
    )
    if fn != -1:
        end = rt.find('extern "C" void aura_drop_jit_fn_native_for_define', fn)
        win = rt[fn : (end if end != -1 else fn + 4200)]
        walk = win.find("jit_key_matches_define(g_closure_names[cid], name)")
        resize = win.find("g_closure_must_deopt.resize(g_closure_func_ids.size(), 0)")
        store = win.find("g_closure_must_deopt[cid] = 1")
        report(
            "AC1.arm",
            walk != -1 and resize != -1 and store != -1 and walk < resize < store,
            "MustDeopt resize+store inside the name-matching walk (peer shape)",
        )
        report(
            "AC1.column_guard",
            win.find("if (g_closure_must_deopt.size() <= cid)") != -1,
            "column sized only when the slot exceeds it (no shrink)",
        )
    else:
        report("AC1.arm", False, "drop helper missing")
        report("AC1.column_guard", False, "drop helper missing")

    # AC2 — teardown intact (additive fix, #4083 shape preserved).
    if fn != -1:
        end = rt.find('extern "C" void aura_drop_jit_fn_native_for_define', fn)
        win = rt[fn : (end if end != -1 else fn + 4200)]
        byname = win.find("g_jit_fns_by_name.erase(it)")
        null_slot = win.find("g_jit_fns[fid] = {nullptr, 0, 0, 0}")
        overflow = win.find("g_jit_fns_overflow.erase(oit)")
        inval = win.find("invalidate_closure_cache_for(static_cast<int64_t>(cid))")
        report(
            "AC2.teardown",
            byname != -1 and null_slot != -1 and overflow != -1 and inval != -1,
            "#4083 teardown rows intact (by-name / slot / overflow / cache)",
        )
    else:
        report("AC2.teardown", False, "drop helper missing")

    # AC3 — lock shape + owner scope unchanged (#4100 shape, #2951 scope).
    abi = rt.find(DROP_ABI)
    if abi != -1:
        awin = rt[abi : abi + 900]
        held = awin.find("is_held(aura::compiler::lock_order::Level::Workspace)")
        tlock = awin.find("std::unique_lock<std::shared_mutex> tlock(g_closure_table_mtx)")
        report(
            "AC3.lock_shape",
            held != -1 and tlock != -1,
            "C ABI still takes the table lock only when workspace not held",
        )
        report(
            "AC3.no_process_bump",
            awin.find("aura_aot_bump_func_table_epoch") == -1,
            "drop path never bumps the process table epoch (#2951 owner scope)",
        )
    else:
        report("AC3.lock_shape", False, "drop ABI missing")
        report("AC3.no_process_bump", False, "drop ABI missing")

    # AC4 — clear stays the remount-heal dual-fresh SSOT (#2128): the drop
    # helper never stores 0; the call-path force-deopt consumption remains.
    if fn != -1:
        end = rt.find('extern "C" void aura_drop_jit_fn_native_for_define', fn)
        win = rt[fn : (end if end != -1 else fn + 4200)]
        report(
            "AC4.no_clear_in_drop",
            win.find("g_closure_must_deopt[cid] = 0") == -1,
            "no MustDeopt clear inside the drop helper (remount-heal SSOT)",
        )
    else:
        report("AC4.no_clear_in_drop", False, "drop helper missing")
    call = rt.find(CALL_ENTRY)
    if call != -1:
        cwin = rt[call : call + 6500]
        report(
            "AC4.call_consumes",
            cwin.find("g_closure_must_deopt[cid] = 0") != -1,
            "call-path force-deopt consumption untouched (#3247 window)",
        )
    else:
        report("AC4.call_consumes", False, "blessed dispatch entry missing")

    # AC5 — runtime doors + wiring; no invented test file / doc / query key.
    report(
        "AC5.tests",
        "#4148" in tst and "ac4148_1_drop" in tst and "ac4148_3_call" in tst,
        "test_must_deopt_before_next_call.cpp cites #4148 with ac4148_* doors",
    )
    no_test_file = not (ROOT / "tests" / "compiler" / "test_issue_4148.cpp").exists()
    no_doc = not any((ROOT / "docs" / "design").glob("4148-*")) if (ROOT / "docs" / "design").exists() else True
    report(
        "AC5.no_invent",
        no_test_file and no_doc,
        "no tests/compiler/test_issue_4148.cpp; no docs/design/4148-*",
    )
    report(
        "AC5.no_query_key",
        "schema-4148" not in rt_raw,
        "no new query key",
    )
    report(
        "AC5.wiring",
        "check_jit_drop_must_deopt_4148.py" in build and "check_jit_drop_must_deopt_4148.py" in allow,
        "build.py wires this linter; root_check_allowlist.txt lists it",
    )

    print(f"check_jit_drop_must_deopt_4148: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
