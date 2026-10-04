#!/usr/bin/env python3
"""Issue #4164 source-cite gate: free-slot gen paint on layout capture.

FlatAST::make_ref_layout filled StableNodeRef.gen = generation_ even when
node_gen_[id] == 0 (free slot — rollback's free_orphan_nodes_from zeroes
node_gen_ only). Production export faces that layout-stamp without the
#4162 walk/gate consult could therefore publish schema-2 refs whose packed
gen equals the live workspace generation while the slot is dead —
green-looking tombstone memory (the false-Fresh window only closes later at
resolve time via is_live_node / StaleByEpoch).

Fix face (#4162 landed the export-gate refusals; #4164 closes the paint
source): make_ref_layout / make_safe_ref_layout take a caller-side
production flag (#3397 pattern — core cannot consult the compiler-side
production face) and return the NULL_NODE layout for free slots under it;
the evaluator production stamp authority (make_stamped_ref /
make_stamped_safe_ref) and the schema-2 stamp remake path
(stamp_query_stable_ref_export layout_missing) thread
typed_audit::production_defaults_active(). Soft keeps the legacy paint.

ACs:
  AC1  ast.ixx make_ref_layout: production free-slot guard
       (`if (production && is_free_slot(id))` → `return StableNodeRef{};`)
       + #4164 block doc stating Soft keeps the legacy paint.
  AC2  ast.ixx make_safe_ref_layout: threads the production flag into
       make_ref_layout and propagates the refused NULL_NODE layout.
  AC3  evaluator_security.cpp: production refuses a wrap/cow-mismatched
       capture before any occupancy remake (#4314); the layout_missing
       remake is Soft-only and keeps the legacy paint. The two production
       layout-threading stamp sites (make_stamped_ref, make_stamped_safe_ref)
       thread production_defaults_active() citing #4164.
  AC4  test home + registry: test_query_result_full_provenance.cpp hosts
       the ac4164_* runtime ACs (freed id → NULL layout, Soft paint kept),
       test_stable_ref_export_validate.cpp hosts the export-face zone;
       no tests/compiler/test_issue_4164.cpp; no docs/design/4164-*;
       build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AST = ROOT / "src" / "core" / "ast.ixx"
SEC = ROOT / "src" / "compiler" / "evaluator_security.cpp"
TST_QRP = ROOT / "tests" / "compiler" / "test_query_result_full_provenance.cpp"
TST_EXP = ROOT / "tests" / "compiler" / "test_stable_ref_export_validate.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

checks: list[tuple[bool, str]] = []


def must(cond: bool, label: str) -> None:
    checks.append((bool(cond), label))


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i:j] if j > 0 else text[i:]


def count(text: str, tok: str) -> int:
    hits = 0
    pos = 0
    while (pos := text.find(tok, pos)) != -1:
        hits += 1
        pos += len(tok)
    return hits


def main() -> int:
    ast_src = AST.read_text(encoding="utf-8")
    sec = SEC.read_text(encoding="utf-8")
    qrp = TST_QRP.read_text(encoding="utf-8")
    exp = TST_EXP.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    allow = ALLOW.read_text(encoding="utf-8")

    # ── AC1: make_ref_layout production free-slot guard ──
    layout = _between(
        ast_src,
        "StableNodeRef make_ref_layout(NodeId id",
        "// Issue #191: make a StableNodeRef capturing the current\n    // generation. Use this in EDSL / query / mutate primitives",
    )
    must("bool production = false" in layout, "AC1: make_ref_layout takes the production flag (default false)")
    must("if (production && is_free_slot(id))" in layout, "AC1: production free-slot guard present")
    must("return StableNodeRef{};" in layout, "AC1: refused layout returns the NULL_NODE layout")
    must("Issue #4164" in layout, "AC1: guard cites #4164")
    must(
        "Soft keeps the legacy paint" in layout,
        "AC1: doc states Soft keeps the legacy paint (EDSL faces unchanged)",
    )
    must(
        "generation_" in layout and "false-Fresh" in layout,
        "AC1: doc names the gen-paint / false-Fresh failure mode",
    )

    # ── AC2: make_safe_ref_layout threads the flag ──
    safe = _between(
        ast_src, "StableNodeRef make_safe_ref_layout(NodeId id", "// Issue #303: make_safe_ref records full provenance"
    )
    must("bool production = false" in safe, "AC2: make_safe_ref_layout takes the production flag")
    must("make_ref_layout(id, production)" in safe, "AC2: threads the flag into make_ref_layout")
    must("Issue #4164" in safe, "AC2: safe-layout face cites #4164")
    must("ref.id == NULL_NODE" in safe, "AC2: propagates the refused NULL_NODE layout")

    # ── AC3: security TU production threading ──
    # Issue #4314: a packed v2 ref captured at epoch 0 is the first cycle,
    # not a missing layout. Production refuses a wrap/cow-mismatched capture
    # before any occupancy remake, so the layout_missing remake is Soft-only
    # and keeps the legacy paint (production flag false).
    must(
        "if (hard && (ref.wrap_epoch != we || ref.cow_epoch_at_capture != ce))" in sec,
        "AC3: production mismatched capture returns early (no occupancy remake)",
    )
    remake = _between(sec, "if (layout_missing) {", "record_query_stable_ref_unstamped_prevented")
    must("make_ref_layout(id, false)" in remake, "AC3: Soft-only remake keeps the legacy layout paint")
    must("production_defaults_active()" not in remake, "AC3: Soft-only remake does not thread the production face")
    stamped = _between(sec, "StableNodeRef Evaluator::make_stamped_ref(", "Evaluator::make_stamped_safe_ref")
    must("Issue #4164" in stamped, "AC3: make_stamped_ref cites #4164")
    must("production_defaults_active()" in stamped, "AC3: make_stamped_ref threads the production face")
    must("ref.id == ast::NULL_NODE" in stamped, "AC3: make_stamped_ref returns the refused layout early (no stamp)")
    safe_ref = _between(sec, "Evaluator::make_stamped_safe_ref(", "// Issue #2224 / #2404: sole public outbound helper")
    must("Issue #4164" in safe_ref, "AC3: make_stamped_safe_ref cites #4164")
    must("production_defaults_active()" in safe_ref, "AC3: make_stamped_safe_ref threads the production face")
    must("ref.id == ast::NULL_NODE" in safe_ref, "AC3: make_stamped_safe_ref returns the refused layout early")
    must(
        count(sec, "typed_audit::production_defaults_active());") == 2,
        "AC3: exactly two production layout-threading sites (make_stamped_ref/-safe_ref)",
    )
    must(count(sec, "Issue #4164") >= 3, "AC3: security TU carries the #4164 cites")

    # ── AC4: test home + registry ──
    must("test_ac4164_1_prod_stamped_ref_refuses_free_slot" in qrp, "AC4: freed-id stamp refusal AC present")
    must("test_ac4164_2_layout_paint_soft_kept_prod_refused" in qrp, "AC4: Soft-paint-kept / prod-refused AC present")
    must("test_ac4164_1_prod_stamped_ref_refuses_free_slot();" in qrp, "AC4: dispatcher invokes AC1")
    must("test_ac4164_2_layout_paint_soft_kept_prod_refused();" in qrp, "AC4: dispatcher invokes AC2")
    must("make_ref_layout(ghost, /*production=*/true)" in qrp, "AC4: production layout face probed explicitly")
    must("4164 AC1: prod make_stamped_ref(freed) -> NULL layout" in qrp, "AC4: freed-id runtime CHECK present")
    must("#4164 AC1-AC4: free-slot gen paint refused on export faces" in exp, "AC4: export-face zone present")
    must("4164 AC2: prod export_ref(freed) -> null" in exp, "AC4: export null-reject runtime CHECK present")
    must("4164 AC3: Soft keeps legacy layout paint" in exp, "AC4: Soft layout-paint CHECK present")
    must(
        not (ROOT / "tests" / "compiler" / "test_issue_4164.cpp").exists(), "AC4: no tests/compiler/test_issue_4164.cpp"
    )
    must(not list((ROOT / "docs" / "design").glob("4164-*")), "AC4: no docs/design/4164-* markdown")
    must("check_ref_layout_gen_paint_4164.py" in build, "AC4: build.py registers this linter")
    must("check_ref_layout_gen_paint_4164.py" in allow, "AC4: root_check_allowlist.txt lists this linter")

    failed = 0
    for ok, label in checks:
        if ok:
            print(f"  ok   {label}")
        else:
            failed += 1
            print(f"  FAIL {label}")
    print(f"check_ref_layout_gen_paint_4164: {len(checks) - failed}/{len(checks)} rows green")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
