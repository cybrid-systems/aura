#!/usr/bin/env python3
"""Issue #2915: PrimRegistrar + PrimMeta scaffolding + agent authoring contract.

AC:
  1. Scaffold header prim_registrar_scaffold.hh exists with register_prim + PrimSpec
  2. At least one register_* file (misc) migrated to register_prim as proof
  3. Authoring contract doc present under docs/stdlib/
  4. docs/contributing.md references the contract / scaffold
  5. Registry comment forbids inventing registration styles (#2915)
  6. gen_docs / generated pointers mention authoring contract
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    scaffold = ROOT / "src/compiler/prim_registrar_scaffold.hh"
    if not scaffold.is_file():
        fails.append("missing src/compiler/prim_registrar_scaffold.hh")
    else:
        t = scaffold.read_text(encoding="utf-8", errors="replace")
        for needle in (
            "Issue #2915",
            "register_prim",
            "struct PrimSpec",
            "pure_general",
            "required_effects",
            "do not invent registration styles",
        ):
            if needle not in t:
                fails.append(f"scaffold missing {needle!r}")

    misc = ROOT / "src/compiler/evaluator_primitives_misc.cpp"
    if not misc.is_file():
        fails.append("missing evaluator_primitives_misc.cpp")
    else:
        t = misc.read_text(encoding="utf-8", errors="replace")
        if "prim_registrar_scaffold.hh" not in t:
            fails.append("misc not including prim_registrar_scaffold.hh")
        if "register_prim" not in t:
            fails.append("misc not using register_prim (proof migration)")
        if "Issue #2915" not in t:
            fails.append("misc missing #2915 cite")
        for name in (
            "current-time",
            "current-time-ms",
            "monotonic-ms",
            "arena-offset",
        ):
            if name not in t:
                fails.append(f"misc lost prim name {name!r}")

    contract = ROOT / "docs/stdlib/primitive-authoring-contract.md"
    if not contract.is_file():
        fails.append("missing docs/stdlib/primitive-authoring-contract.md")
    else:
        t = contract.read_text(encoding="utf-8", errors="replace")
        if "2915" not in t or "register_prim" not in t:
            fails.append("authoring contract incomplete")
        if "do not invent" not in t.lower() and "Do not invent" not in t:
            fails.append("authoring contract missing do-not-invent rule")

    contrib = ROOT / "docs/contributing.md"
    if not contrib.is_file():
        fails.append("missing docs/contributing.md")
    else:
        t = contrib.read_text(encoding="utf-8", errors="replace")
        if "primitive-authoring-contract" not in t and "prim_registrar_scaffold" not in t:
            fails.append("contributing.md does not reference authoring contract/scaffold")

    registry = ROOT / "src/compiler/evaluator_primitives_registry.cpp"
    if not registry.is_file():
        fails.append("missing evaluator_primitives_registry.cpp")
    else:
        t = registry.read_text(encoding="utf-8", errors="replace")
        if "#2915" not in t:
            fails.append("registry missing #2915 cite")
        if "do NOT invent" not in t and "do not invent" not in t.lower():
            fails.append("registry missing do-not-invent-styles note")
        if "prim_registrar_scaffold" not in t and "register_prim" not in t:
            fails.append("registry does not point at scaffold")

    gen_docs = ROOT / "scripts/tools/gen_docs.py"
    if gen_docs.is_file():
        t = gen_docs.read_text(encoding="utf-8", errors="replace")
        if "2915" not in t or "primitive-authoring-contract" not in t:
            fails.append("gen_docs.py missing #2915 authoring pointers")
        if "REGISTER_PRIM_RE" not in t and "register_prim" not in t:
            fails.append("gen_docs.py must scan register_prim registrations")

    meta_h = ROOT / "src/compiler/primitives_meta.h"
    if meta_h.is_file():
        t = meta_h.read_text(encoding="utf-8", errors="replace")
        if "kPrimitivesExtensionKitVersion = 4" not in t and "#2915" not in t:
            fails.append("primitives_meta.h not bumped for #2915 extension kit")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2915 PrimRegistrar scaffold + authoring contract — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
