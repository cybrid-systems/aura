#!/usr/bin/env python3
"""Issue #4091 source-cite gate: shape stability loss — result cone, O(1) FnKey.

The shape dirty hook (ShapeProfiler::set_dirty_hook →
CompilerService::shape_dirty_hook_trampoline → mark_shape_dirty_for_fn_key)
walked EVERY ir_cache_v2_ entry hashing session_id_ + name until one matched,
then mark_all_blocks_dirty() on that entry. One result-shape change therefore
scanned the whole define cache (O(cached defines), string hash per entry) and
marked every block dirty, so the next lower/opt rebuilt the whole function
instead of the dirty cone.

ACs:
  AC1  mark_shape_dirty_for_fn_key resolves via the FnKey → name side index
       (shape_fnkey_names_.find) BEFORE any fallback scan, and its body no
       longer calls mark_all_blocks_dirty.
  AC2  the dirty mark goes through the mark_blocks_dirty batch entry
       (#2522/#2615 discipline) targeting Return blocks — the single-block
       mark_block_dirty (deleted under AURA_PRODUCTION_PACK) is not used.
  AC3  the side index is maintained at the ir_cache_v2_ mutation sites:
       insert_or_assign at the store paths and erase at the eviction /
       define-removal paths.
  AC4  test_shape_storm_partial_relower drives the runtime ACs (#4091
       labels), the linter is wired in build.py +
       scripts/coverage/root_check_allowlist.txt.
  AC5  no new query key and no stray ship files (no
       tests/**/test_issue_4091.cpp, no docs/design/4091-*).
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

    # AC1: the FnKey side index answers first; mark_all_blocks_dirty is
    # gone from the hook path.
    body = function_body(svc, "void mark_shape_dirty_for_fn_key(shape::FnKey fn_key) {")
    idx_pos = body.find("shape_fnkey_names_.find(fn_key)")
    scan_pos = body.find("for (const auto& [name, entry] : ir_cache_v2_)")
    report(
        "AC1 side-index resolution, no full-cache mark",
        bool(body) and idx_pos >= 0 and "mark_all_blocks_dirty" not in body and (scan_pos < 0 or idx_pos < scan_pos),
        "FnKey → name index resolves O(1) before the cold fallback; no mark_all_blocks_dirty",
    )

    # AC2: the dirty mark uses the mark_blocks_dirty batch entry and targets
    # the result cone (Return blocks); mark_block_dirty is not used.
    body = function_body(svc, "bool mark_shape_dirty_result_block_(const std::string& name) {")
    report(
        "AC2 result cone via batch entry",
        bool(body)
        and "entry.mark_blocks_dirty(func_idx," in body
        and "IROpcode::Return" in body
        and "mark_block_dirty(" not in body.replace("mark_blocks_dirty(", ""),
        "Return blocks → entry.mark_blocks_dirty(func_idx, span); no single-block mark",
    )

    # AC3: the side index is maintained at the ir_cache_v2_ mutation sites.
    inserts = svc.count("shape_fnkey_names_.insert_or_assign(")
    erases = svc.count("shape_fnkey_names_.erase(")
    declared = "std::unordered_map<shape::FnKey, std::string> shape_fnkey_names_" in svc
    report(
        "AC3 side index maintained",
        declared and inserts >= 3 and erases >= 3,
        f"declared; {inserts} insert sites (store paths), {erases} erase sites (evict/remove)",
    )

    # AC4: runtime ACs in the hosting regression + gate wiring.
    report(
        "AC4 test ACs + wiring",
        "#4091 AC1" in tst
        and "#4091 AC2" in tst
        and "#4091 AC3" in tst
        and "check_shape_dirty_cone_4091.py" in build
        and "check_shape_dirty_cone_4091.py" in allow,
        "test_shape_storm_partial_relower ACs 1-3, build.py + allowlist wiring",
    )

    # AC5: negative contracts — no new query key, no stray ship files.
    stray_test = list(ROOT.glob("tests/**/test_issue_4091.cpp"))
    stray_doc = list(ROOT.glob("docs/design/4091-*"))
    report(
        "AC5 no new query key / no stray files",
        "query:shape-dirty-cone" not in svc and not stray_test and not stray_doc,
        "no query:shape-dirty-cone* key; no test_issue_4091.cpp; no docs/design/4091-*",
    )

    print("check_shape_dirty_cone_4091: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
