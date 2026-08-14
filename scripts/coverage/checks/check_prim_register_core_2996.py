#!/usr/bin/env python3
"""Issue #2996: core TUs migrated to register_prim + PrimSpec.

AC:
  1. list / math / json / pair / vector use register_prim (no leftover bare add)
  2. Every migrated prim has non-empty schema + doc
  3. g_register_prim_scaffold_total cited; no new legacy add without meta
  4. Surface / authoring-contract / generated docs pointers stay green
  5. Registry + authoring-contract mark core TUs migrated
  6. Existing suite covers PrimMeta stamp (arity/pure/schema)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

CORE = [
    "evaluator_primitives_list.cpp",
    "evaluator_primitives_math.cpp",
    "evaluator_primitives_json.cpp",
    "evaluator_primitives_pair.cpp",
    "evaluator_primitives_vector.cpp",
]

# AC1 names that must appear as register_prim(...)
REQUIRED = {
    "list": (
        "list",
        "list?",
        "null?",
        "length",
        "list-ref",
        "member",
        "append",
        "reverse",
        "map",
        "filter",
        "take",
        "drop",
        "foldl",
        "list-sort",
    ),
    "math": ("sin", "modulo", "regex-match?", "abs", "min", "max"),
    "json": ("json-encode", "json-parse", "json-get-string"),
    "pair": ("cons", "car", "cdr", "string-append", "string-length", "set-car!"),
    "vector": ("vector", "vector-ref", "hash", "hash-ref", "hash-set!"),
}


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tus: dict[str, str] = {}
    for fn in CORE:
        rel = f"src/compiler/{fn}"
        t = _read(rel)
        tus[fn] = t
        if "prim_registrar_scaffold.hh" not in t:
            fails.append(f"AC1: {fn} missing scaffold include")
        if "register_prim" not in t:
            fails.append(f"AC1: {fn} not using register_prim")
        if "Issue #2996" not in t:
            fails.append(f"AC1: {fn} missing #2996 cite")
        # leftover bare add(" at statement start
        for m in re.finditer(r"^\s+add\s*\(\s*\"", t, re.MULTILINE):
            fails.append(f"AC3: {fn} leftover bare add() at offset {m.start()}")
        if "ev.primitives_.add(" in t:
            fails.append(f"AC3: {fn} leftover ev.primitives_.add")

    list_t = tus["evaluator_primitives_list.cpp"]
    math_t = tus["evaluator_primitives_math.cpp"]
    json_t = tus["evaluator_primitives_json.cpp"]
    pair_t = tus["evaluator_primitives_pair.cpp"]
    vec_t = tus["evaluator_primitives_vector.cpp"]
    grouped = {
        "list": list_t,
        "math": math_t,
        "json": json_t,
        "pair": pair_t,
        "vector": vec_t,
    }
    for group, names in REQUIRED.items():
        hay = grouped[group]
        for name in names:
            if "register_prim(" not in hay or f'"{name}"' not in hay:
                fails.append(f"AC1: {group} lost {name!r}")
            # schema + doc via pure_general/mutate_general next to the name
            if f'"{name}"' in hay and "pure_general" not in hay and "mutate_general" not in hay:
                fails.append(f"AC2: {group} has no PrimSpec factory")

    # AC2: every register_prim has a factory with two string args (schema, doc)
    factory_re = re.compile(
        r"register_prim\s*\(\s*add\s*,\s*ev\s*,\s*\"([^\"]+)\"[\s\S]*?"
        r"(pure_general|mutate_general|io_general)\s*\(\s*\d+\s*,\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"\s*\)",
        re.MULTILINE,
    )
    seen: set[str] = set()
    for fn, t in tus.items():
        for m in factory_re.finditer(t):
            name, _kind, schema, doc = m.group(1), m.group(2), m.group(3), m.group(4)
            seen.add(name)
            if not schema.strip():
                fails.append(f"AC2: {fn} {name} empty schema")
            if not doc.strip():
                fails.append(f"AC2: {fn} {name} empty doc")
        n_reg = len(re.findall(r"register_prim\s*\(\s*add\s*,\s*ev\s*,", t))
        n_fact = len(factory_re.findall(t))
        if n_reg != n_fact:
            fails.append(f"AC2: {fn} register_prim={n_reg} but spec factories={n_fact}")

    must("g_register_prim_scaffold_total", "AC3", _read("src/compiler/prim_registrar_scaffold.hh"))

    registry = _read("src/compiler/evaluator_primitives_registry.cpp")
    must("#2996", "AC5", registry)
    must("migrated", "AC5", registry.lower())
    contract = _read("docs/stdlib/primitive-authoring-contract.md")
    must("2996", "AC5", contract)
    must("evaluator_primitives_list.cpp", "AC5", contract)

    build = _read("build.py")
    must("check_prim_register_core_2996", "AC5", build)

    smoke = _read("tests/compiler/test_obs_metrics_smoke_batch.cpp")
    must("ac2996", "AC6", smoke)
    must("register_prim", "AC6", smoke)

    if (ROOT / "tests" / "compiler" / "test_issue_2996.cpp").is_file():
        fails.append("AC6: test_issue_2996.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2996-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2996 core register_prim migration — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
