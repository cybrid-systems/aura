#!/usr/bin/env python3
"""Issue #4108 source-cite gate: stable shape sync — side index, result blocks.

CompilerService::sync_shape_ids_for_fn_key (src/compiler/service.ixx) was the
last stable-path holdout from the #4091 discipline: on EVERY stable IR/JIT
result it walked ir_cache_v2_ hashing session_id_ + name until make_fn_key
matched, then painted the result shape onto every unset shape_ids_ column of
EVERY function in the entry. A stabilized multi-round self-modify workload
paid a full define-cache walk on the success path, and a nested function's
unset columns inherited the outer result shape — the next lower/JIT
specialized those ops to a shape they never return. make_fn_key is
hash(session) ^ (hash(name) << 1) with no name recheck, so the first cache
entry that hashed equal won the sync and a colliding name never got its own
row.

ACs:
  AC1  sync_shape_ids_for_fn_key resolves the FnKey through the #4091 side
       index (shape_fnkey_names_.find) BEFORE any cache scan; the cold
       fallback scan sits behind the index-miss guard, so the hit path
       never iterates ir_cache_v2_.
  AC2  the stamp targets the ENTRY function only (irs[fi].name == name
       selection, else function 0) and only the columns of its Return-block
       positions — no per-function blanket stamp, no all-columns walk.
  AC3  the name recheck: the sync takes the define's name, the cold scan
       requires name equality in addition to make_fn_key equality before
       insert_or_assign, and both call sites in record_eval_result_shape
       pass the defining name.
  AC4  the runtime ACs live in the #4091 home (test_shape_storm_partial_
       relower) and the linter is wired in build.py +
       scripts/coverage/root_check_allowlist.txt.
  AC5  no new query key and no stray ship files (no
       tests/**/test_issue_4108.cpp, no docs/design/4108-*).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVC = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_shape_storm_partial_relower.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def function_body(src: str, signature: str) -> str:
    """Return the text from `signature` up to the next top-level closing brace."""
    i = src.find(signature)
    if i < 0:
        return ""
    j = src.find("\n    }", i)
    return src[i : j if j >= 0 else len(src)]


def main() -> int:
    svc = SVC.read_text() if SVC.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    body = function_body(
        svc, "void sync_shape_ids_for_fn_key(shape::FnKey fn_key, const std::string& want_name,"
    ) or function_body(svc, "void sync_shape_ids_for_fn_key(")

    # AC1: side index first — the FnKey → name row answers before any
    # ir_cache_v2_ scan; the scan is the cold-miss fallback only.
    idx_pos = body.find("shape_fnkey_names_.find(fn_key)")
    scan_pos = body.find("for (const auto& [name, entry] : ir_cache_v2_)")
    miss_guard = "idx == shape_fnkey_names_.end()" in body
    report(
        "AC1 side-index resolution before any scan",
        bool(body)
        and idx_pos >= 0
        and miss_guard
        and (scan_pos < 0 or idx_pos < scan_pos)
        and body.rfind("if (idx == shape_fnkey_names_.end())") < scan_pos
        if scan_pos >= 0
        else bool(body) and idx_pos >= 0 and miss_guard,
        "FnKey → name index resolves O(1); the cache scan sits behind the miss guard",
    )

    # AC2: entry-function selection + Return-block columns only; the
    # per-function / per-column blanket stamps are gone.
    report(
        "AC2 entry function's result blocks only",
        bool(body)
        and "entry.irs[fi].name == want_name" in body
        and "functions[func_idx]" in body
        and "IROpcode::Return" in body
        and "shape_ids_[ci] == 0" in body
        and "shape_ids_[ci] = sid;" in body
        and "for (auto& soa_fn : entry.soa_mod.functions)" not in body
        and "for (auto& col : soa_fn.shape_ids_)" not in body,
        "irs[fi].name selection + Return-block column stamp; no blanket function/column walk",
    )

    # AC3: name recheck — the cold scan requires the stored name to equal
    # the wanted name before it claims the side-index row.
    report(
        "AC3 name recheck before insert_or_assign",
        bool(body)
        and "name != want_name" in body
        and "shape::make_fn_key(session_id_, name) != fn_key" in body
        and body.count("shape_fnkey_names_.insert_or_assign(") == 1
        and body.find("name != want_name") < body.find("shape_fnkey_names_.insert_or_assign("),
        "stored-name equality guards the only insert_or_assign in the sync path",
    )

    # AC3b/AC4: both record_eval_result_shape call sites pass the name, the
    # runtime ACs live in the #4091 home, and the wiring exists. The
    # collision contract is pinned structurally here (the #4091 O(1)
    # discipline): no eval record path can drive a colliding key at runtime.
    call_sites = (
        'sync_shape_ids_for_fn_key(fn_key, "__eval__", shape_id)' in svc
        and "sync_shape_ids_for_fn_key(fn_key, fn.name, shape_id)" in svc
    )
    report(
        "AC4 call sites + test ACs + wiring",
        call_sites
        and "#4108 AC1" in tst
        and "#4108 AC2" in tst
        and "#4108 AC3" in tst
        and "ac4108_stable_sync_side_index();" in tst
        and "check_shape_sync_index_4108.py" in build
        and "check_shape_sync_index_4108.py" in allow,
        "named call sites; test_shape_storm_partial_relower ACs 1-3; build.py + allowlist",
    )

    # AC5: negative contracts — no new query key, no stray ship files.
    stray_test = list(ROOT.glob("tests/**/test_issue_4108.cpp"))
    stray_doc = list(ROOT.glob("docs/design/4108-*"))
    report(
        "AC5 no new query key / no stray files",
        "query:shape-sync-index" not in svc and not stray_test and not stray_doc,
        "no query:shape-sync-index* key; no test_issue_4108.cpp; no docs/design/4108-*",
    )

    print("check_shape_sync_index_4108: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
