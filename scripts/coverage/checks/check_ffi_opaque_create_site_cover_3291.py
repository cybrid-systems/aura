#!/usr/bin/env python3
"""Issue #3291: densify-tracked FFI opaque create sites must be
machine-checkable pin/slot/EXEMPT cover (residual of production Moving
under AI multi-round).

#3274 wired note_ffi_opaque_alias_densify_cover at the known create sites
(ffi-return-external in evaluator_eval_flat.cpp, opaque-struct-copy in
ffi_primitives_impl.cpp) and kept libc-heap / external-native-addr as true
non-arena EXEMPT. Residual: the #3274 linter only checks helper presence
per file — a future NAKED opaque push site (no cover, no EXEMPT) would not
fail CI. This linter enumerates EVERY opaque push site across src/ and
classifies each as slot-cover / canary-cover / EXEMPT.

Gate rows:
  G1  every opaque_heap_.push_back / oh->push_back site in src/ is
      followed (within a text window) by note_ffi_opaque_alias_densify_cover
      OR note_ffi_opaque_create_exempt — a naked push fails CI.
  G2  the 4 known sites keep their classification: eval_flat
      ffi-return-external → densify cover; ffi_primitives
      opaque-struct-copy → densify cover; external-native-addr / libc-heap
      → EXEMPT (true non-arena).
  G3  EXEMPT reasons are whitelisted (libc-heap / external-native-addr);
      a densify-tracked path must NOT be labeled EXEMPT.
  G4  densify-cover helper semantics preserved in arena.ixx (slot OR
      #3210 canary; slot/canary exclusive; Soft falls back to EXEMPT).
  G5  Soft / Off / unset: zero extra pin cost (linter offline; no new
      g_3291_* counters).
  G6  test ACs in test_moving_densify_fail_closed.cpp (#81967); no
      test_issue_3291.cpp; no docs/design/ (#1655).
  G7  build.py wires this linter; #3274 linter preserved.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []

# EXEMPT reasons that are TRUE non-arena (allowed). Anything else labeled
# EXEMPT on a densify-tracked alias must fail.
_ALLOWED_EXEMPT_REASONS = {"libc-heap", "external-native-addr"}

_PUSH = re.compile(r"(?:opaque_heap_|oh)\s*(?:\.|->)\s*push_back\s*\(")
_COVER = "note_ffi_opaque_alias_densify_cover"
_EXEMPT = "note_ffi_opaque_create_exempt"


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _iter_src_tus() -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    src = ROOT / "src"
    if not src.is_dir():
        return out
    for p in sorted(src.rglob("*")):
        if p.suffix not in {".cpp", ".h", ".hh", ".ixx", ".cc"}:
            continue
        out.append((str(p.relative_to(ROOT)), p.read_text(encoding="utf-8", errors="replace")))
    return out


def _classify_site(text: str, push_pos: int) -> str:
    """Classify one opaque push site: 'cover' | 'exempt' | 'naked'."""
    window = text[push_pos : push_pos + 900]
    if _COVER in window:
        return "cover"
    if _EXEMPT in window:
        return "exempt"
    return "naked"


def main() -> int:
    arena = _read("src/core/arena.ixx")
    ffi = _read("src/compiler/ffi_primitives_impl.cpp")
    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")
    lint3274 = _read("scripts/coverage/checks/check_ffi_opaque_densify_cover_3274.py")

    # ── G1: every opaque push site classified ──
    naked_sites: list[str] = []
    sites_by_tu: dict[str, list[str]] = {}
    for rel, text in _iter_src_tus():
        for m in _PUSH.finditer(text):
            cls = _classify_site(text, m.start())
            sites_by_tu.setdefault(rel, []).append(cls)
            if cls == "naked":
                naked_sites.append(f"{rel}:{m.start()}")
    total_sites = sum(len(v) for v in sites_by_tu.values())
    must(total_sites >= 4, f"G1: found {total_sites} opaque push sites (expect >= 4)")
    must(not naked_sites, f"G1: naked opaque push sites: {naked_sites}")
    # No new TU may add a push site without this linter seeing cover/exempt.
    for rel, classes in sites_by_tu.items():
        for cls in classes:
            must(cls in {"cover", "exempt"}, f"G1: {rel} push classified ({cls})")

    # ── G2: known sites keep their classification ──
    # evaluator_eval_flat.cpp ffi-return-external → cover
    ev_ok = all(_classify_site(ev, m.start()) == "cover" for m in _PUSH.finditer(ev))
    must(ev_ok, "G2: evaluator_eval_flat.cpp push sites all densify-cover")
    # ffi_primitives_impl.cpp: opaque-struct-copy → cover; native/libc → exempt
    ffi_cover = ffi_ok = True
    for m in _PUSH.finditer(ffi):
        cls = _classify_site(ffi, m.start())
        win = ffi[m.start() : m.start() + 900]
        if "opaque-struct-copy" in win:
            ffi_cover = ffi_cover and cls == "cover"
        elif "external-native-addr" in win or "libc-heap" in win:
            ffi_ok = ffi_ok and cls == "exempt"
        else:
            ffi_ok = False
    must(ffi_cover, "G2: opaque-struct-copy is densify-cover (not EXEMPT)")
    must(ffi_ok, "G2: external-native-addr / libc-heap keep EXEMPT (true non-arena)")

    # ── G3: EXEMPT reasons whitelist ──
    bad_exempt: list[str] = []
    for m in re.finditer(r'note_ffi_opaque_create_exempt\("([^"]+)"\)', ffi + ev + arena):
        reason = m.group(1)
        if reason not in _ALLOWED_EXEMPT_REASONS:
            bad_exempt.append(reason)
    must(not bad_exempt, f"G3: EXEMPT reasons outside whitelist {sorted(_ALLOWED_EXEMPT_REASONS)}: {bad_exempt}")

    # ── G4: helper semantics preserved in arena.ixx ──
    must(_COVER in arena, "G4: note_ffi_opaque_alias_densify_cover in arena.ixx")
    must("note_temporary_moving_live_ptr(p)" in arena, "G4: no-slot → #3210 canary")
    must("Slot and canary are EXCLUSIVE" in arena, "G4: slot/canary exclusivity documented")
    must("moving_compact_enabled()" in arena, "G4: Soft/Off falls back to EXEMPT")

    # ── G5: Soft / Off zero extra; no new counters ──
    must("g_3291_" not in arena and "g_3291_" not in ffi and "g_3291_" not in ev, "G5: no new g_3291_* counter")
    must("note_ffi_opaque_create_exempt(reason)" in arena, "G5: Soft EXEMPT fallback preserved")

    # ── G6: src-aligned suite home (#81967) ──
    must("3291 AC1" in test, "G6: AC1 test present")
    must("3291 AC2" in test, "G6: AC2 test present")
    must("3291 AC3" in test, "G6: AC3 test present")
    must("3291 AC4" in test, "G6: AC4 test present")
    must(not _read("tests/core/test_issue_3291.cpp"), "G6: no tests/core/test_issue_3291.cpp per #81967")
    must(not _read("tests/issues/test_issue_3291.cpp"), "G6: no tests/issues/test_issue_3291.cpp per #81967")

    # ── G7: build.py wiring + #3274 preserved ──
    must("check_ffi_opaque_create_site_cover_3291" in build, "G7: build.py wires linter")
    must("Issue #3274" in lint3274, "G7: #3274 linter preserved")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        bad = [f.name for f in sorted(docs.glob("3291-*"))]
        must(not bad, "G7: no docs/design/3291-* per #1655")
    else:
        must(True, "G7: no docs/design/3291-* per #1655")

    if failures:
        print(f"\n#3291 linter: {len(failures)} gate(s) FAILED")
        return 1
    print("\n#3291 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
