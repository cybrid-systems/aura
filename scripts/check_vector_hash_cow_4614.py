#!/usr/bin/env python3
"""Issue #4614 source-cite gate (native vector ops, string hash, copy-on-write hash).

The issue shipped four gaps in the keyed-store story:
  1. hash-set / hash-remove looked functional but mutated in place (the
     aliasing trap that breaks checkpoints / replay / forked worldlines).
  2. no persistent map with structural sharing (left open -- feature-sized).
  3. vector-copy was an interpreted stdlib loop (564 us for 64 slots).
  4. no string / equal? hash primitive (a string->list fold, ~50 us / 8 chars).

This gate pins the shipped slice:
  AC1  src/compiler/evaluator_primitives_vector.cpp registers a native
       "vector-copy" (optional start/end, clamps, mint_vector) and cites
       Issue #4614.
  AC2  the same file registers a native "vector-fill!" (mutating, returns
       the vector).
  AC3  src/compiler/evaluator_primitives_pair.cpp registers a native
       "string-hash" that hashes the UTF-8 bytes with the repo's FNV-1a 64
       (hash_meta.h fnv1a_bytes -- the hash the table already uses for
       string keys) and cites Issue #4614.
  AC4  the same file registers a native "hash-copy" that clones the table
       slot-for-slot behind the tenant gate and stamps the new table.
  AC5  lib/std/list.aura no longer carries the interpreted vector-copy /
       vector-fill! loops (the native primitives are used) but still names
       them in the export list.
  AC6  lib/std/hash.aura: hash-set / hash-remove are COPY-ON-WRITE (they
       build a fresh table via hash-copy) and their docstrings say so; the
       in-place builders use hash-set!.
  AC7  docs/generated/stdlib-index.md says which (the generated index
       carries the std/hash module header).
  AC8  the test home: tests/compiler/test_hash_table_grow.cpp carries the
       ac4614_* arms and tests/suite/vector_hash_cow_4614.aura carries the
       end-to-end expectation.
  AC9  wiring: build.py runs this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
  AC10 no tests/issues/ home for this issue and no docs/design/4614-*.

Exit 0 = all rows satisfied. Exit 1 = a contract row regressed.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VEC = ROOT / "src" / "compiler" / "evaluator_primitives_vector.cpp"
PAIR = ROOT / "src" / "compiler" / "evaluator_primitives_pair.cpp"
LIST = ROOT / "lib" / "std" / "list.aura"
HASH = ROOT / "lib" / "std" / "hash.aura"
INDEX = ROOT / "docs" / "generated" / "stdlib-index.md"
TEST = ROOT / "tests" / "compiler" / "test_hash_table_grow.cpp"
SUITE = ROOT / "tests" / "suite" / "vector_hash_cow_4614.aura"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def read(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def strip_aura_comments(text: str) -> str:
    """Drop the ';' line comments of an Aura source (call-site counting)."""
    out = []
    for line in text.splitlines():
        stripped = line.lstrip()
        out.append("" if stripped.startswith(";") else line)
    return chr(10).join(out)


def window(text: str, start: str, end: str, label: str) -> str:
    """Text between the first start marker and the next end marker."""
    at = text.find(start)
    if at < 0:
        must(False, f"{label}: anchor not found: {start!r}")
        return ""
    stop = text.find(end, at + len(start))
    if stop < 0:
        must(False, f"{label}: closing anchor not found: {end!r}")
        return ""
    return text[at : stop + len(end)]


def main() -> int:
    vec = read(VEC)
    pair = read(PAIR)
    lst = read(LIST)
    hsh = read(HASH)
    idx = read(INDEX)
    tst = read(TEST)
    suite = read(SUITE)
    build = read(BUILD)
    allow = read(ALLOW)

    # AC1 -- native vector-copy
    must("Issue #4614" in vec, "AC1: the vector primitives file cites Issue #4614")
    cp = window(vec, '"vector-copy"', "pure_general(255,", "AC1 vector-copy")
    must(bool(cp), "AC1: vector-copy is registered")
    must("mint_vector" in cp, "AC1: vector-copy mints a fresh vector")
    must("as_int(a[1])" in cp and "as_int(a[2])" in cp, "AC1: vector-copy reads the optional start/end indexes")
    must(
        "start must be an integer" in cp and "end must be an integer" in cp,
        "AC1: vector-copy rejects a non-integer start/end",
    )
    must(
        "vector-copy: cross-tenant vector slot read denied" in cp,
        "AC1: vector-copy consults the vector-slot owner gate",
    )

    # AC2 -- native vector-fill!
    fl = window(vec, '"vector-fill!"', "mutate_general(2,", "AC2 vector-fill!")
    must(bool(fl), "AC2: vector-fill! is registered")
    must("kEffectMutate" in fl, "AC2: vector-fill! consults the mutate-effect owner gate")
    must("return a[0];" in fl, "AC2: vector-fill! returns the vector")

    # AC3 -- native string-hash
    must("Issue #4614" in pair, "AC3: the string primitives file cites Issue #4614")
    sh = window(pair, '"string-hash"', "pure_general(1,", "AC3 string-hash")
    must(bool(sh), "AC3: string-hash is registered")
    must("fnv1a_bytes" in sh, "AC3: string-hash hashes with the repo FNV-1a 64")
    must("hash_meta.h" in pair, "AC3: the string primitives file includes hash_meta.h")

    # AC4 -- native hash-copy
    hc = window(vec, '"hash-copy"', "pure_general(1,", "AC4 hash-copy")
    must(bool(hc), "AC4: hash-copy is registered")
    must("FlatHashTable::create" in hc, "AC4: hash-copy allocates a new table")
    must(hc.count("std::memcpy") == 3, "AC4: hash-copy copies metadata / keys / values")
    must("aura_hash_gate_checked" in hc, "AC4: hash-copy consults the hash tenant gate")
    must(
        "g_hash_tenants[newidx] = ev.capability_tenant_id();" in hc,
        "AC4: hash-copy stamps the new table with the executing principal",
    )

    # AC5 -- the stdlib no longer interprets the vector ops
    must("(define (vector-copy" not in lst, "AC5: std/list has no interpreted vector-copy")
    must("(define (vector-fill!" not in lst, "AC5: std/list has no interpreted vector-fill!")
    must("vector-copy" in lst and "vector-fill!" in lst, "AC5: std/list still names both in the export list")
    must("Issue #4614" in lst, "AC5: std/list cites Issue #4614")

    # AC6 -- copy-on-write hash-set / hash-remove
    must("(define (hash-set h k v)" in hsh, "AC6: std/hash defines hash-set")
    hs = window(hsh, "(define (hash-set h k v)", "(define (hash-get h k)", "AC6 hash-set")
    must("hash-copy" in hs, "AC6: hash-set builds a fresh table (copy-on-write)")
    must("hash-set!" in hs, "AC6: hash-set writes into the fresh table")
    must("(define (hash-remove h k)" in hsh, "AC6: std/hash defines hash-remove")
    hr = window(hsh, "(define (hash-remove h k)", ";; (#108 part 1)", "AC6 hash-remove")
    must("hash-copy" in hr, "AC6: hash-remove builds a fresh table (copy-on-write)")
    must("hash-remove!" in hr, "AC6: hash-remove writes into the fresh table")
    must("COPY-ON-WRITE" in hsh, "AC6: the std/hash docstrings say COPY-ON-WRITE")
    must(hsh.count("(hash-set! ") >= 3, "AC6: the in-place builders use hash-set!")
    hsh_code = strip_aura_comments(hsh)
    set_calls = re.findall(r"\(hash-set\s", hsh_code)
    set_defs = re.findall(r"\(define \(hash-set\s", hsh_code)
    must(len(set_calls) == len(set_defs) == 1, "AC6: no builder still calls the copy-on-write hash-set")
    rm_calls = re.findall(r"\(hash-remove\s", hsh_code)
    rm_defs = re.findall(r"\(define \(hash-remove\s", hsh_code)
    must(len(rm_calls) == len(rm_defs) == 1, "AC6: no builder still calls the copy-on-write hash-remove")

    # AC7 -- the generated index says which
    must("COPY-ON-WRITE" in idx, "AC7: docs/generated/stdlib-index.md says which (copy-on-write)")
    must("std/hash" in idx, "AC7: the generated index still lists std/hash")

    # AC8 -- the test home
    for arm in (
        "ac4614_vector_copy",
        "ac4614_vector_fill",
        "ac4614_string_hash",
        "ac4614_hash_cow",
        "ac4614_vector_copy_timing",
    ):
        must(arm in tst, f"AC8: tests/compiler/test_hash_table_grow.cpp has {arm}")
    must("run_test_hash_table_grow" in tst, "AC8: the test file keeps its runner")
    must(suite != "" and ";; expect:" in suite, "AC8: tests/suite/vector_hash_cow_4614.aura carries the expectation")
    must(
        "hash-set" in suite and "vector-copy" in suite and "string-hash" in suite,
        "AC8: the suite case exercises the shipped surface",
    )

    # AC9 -- wiring
    must("check_vector_hash_cow_4614.py" in build, "AC9: build.py runs the linter")
    must("check_vector_hash_cow_4614.py" in allow, "AC9: scripts/coverage/root_check_allowlist.txt lists the linter")

    # AC10 -- house rules
    must(
        not list((ROOT / "tests" / "issues").glob("*4614*")) if (ROOT / "tests" / "issues").is_dir() else True,
        "AC10: no tests/issues/ home for #4614",
    )
    must(
        not list((ROOT / "docs" / "design").glob("4614-*")) if (ROOT / "docs" / "design").is_dir() else True,
        "AC10: no docs/design/4614-* file",
    )

    if failures:
        print(f"check_vector_hash_cow_4614: FAIL ({len(failures)} row(s))")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_vector_hash_cow_4614: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
