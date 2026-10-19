#!/usr/bin/env python3
"""Issue #4612 source-cite gate (hash lookup probes by hash).

The language hash table is open-addressed: a key whose hash is kh lives
at ((kh >> 1) + at) & (cap - 1) -- the first free slot at or after its
home slot. The LOOKUP, however, walked EVERY slot of the table and
compared every key, so a miss cost O(capacity) and a hit cost
O(position): (hash) was unusable as an index past a few thousand keys
(a miss grew to 810 us/op at 40,000 entries in the issue's repro).

This gate pins the shipped slice:
  AC1  src/compiler/evaluator_primitives_vector.cpp carries a shared
       open-addressed probe helper (eval_hash_probe) that walks the
       chain of the key hash and compares keys with eval_hash_keys_eq
       (the equal? semantics of #4489 and the in-range tenant compare
       of #4505 are unchanged).
  AC2  hash-ref / hash-has-key? / hash-set! / hash-remove! use that
       probe and no longer scan the whole table.
  AC3  the key hash is the one the native string-hash primitive returns
       (#4614): hash_meta.h fnv1a_bytes -- one string hash, not two.
  AC4  a removal writes a TOMBSTONE (hash_meta.h kTombstoneSlot), not an
       empty slot: an empty slot would end the probe chain and hide the
       keys displaced past the hole. The rehash paths and the table
       walkers skip the tombstone.
  AC5  the IR path (ir_executor_impl.cpp, the IROpcode::HashRef arm)
       dispatches to the same "hash-ref" primitive -- the same probe.
  AC6  the test home: tests/compiler/test_hash_probe.cpp carries the
       ac4612_* arms.
  AC7  wiring: build.py runs this linter, CMakeLists registers the test
       target and scripts/coverage/root_check_allowlist.txt lists the
       linter.
  AC8  no tests/issues/ home for this issue and no docs/design/4612-*.

Exit 0 = all rows satisfied. Exit 1 = a contract row regressed.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VEC = ROOT / "src" / "compiler" / "evaluator_primitives_vector.cpp"
META = ROOT / "src" / "compiler" / "hash_meta.h"
PAIR = ROOT / "src" / "compiler" / "evaluator_primitives_pair.cpp"
IR = ROOT / "src" / "compiler" / "ir_executor_impl.cpp"
JSON_SRC = ROOT / "src" / "compiler" / "evaluator_primitives_json.cpp"
GC = ROOT / "src" / "compiler" / "evaluator_gc.cpp"
TEST = ROOT / "tests" / "compiler" / "test_hash_probe.cpp"
BUILD = ROOT / "build.py"
CMAKE = ROOT / "CMakeLists.txt"
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


def prim_window(src: str, name: str) -> str:
    """The body of ONE primitive registration (name -> next register_prim)."""
    # The registration site is the '"<name>",' argument of register_prim --
    # a plain '"<name>"' also shows up in prose comments.
    at = src.find(f'"{name}",')
    if at < 0:
        return ""
    stop = src.find("register_prim(", at)
    if stop < 0:
        stop = len(src)  # the last registration in the file
    return src[at:stop]


def main() -> int:
    vec = read(VEC)
    meta = read(META)
    pair = read(PAIR)
    ir = read(IR)
    js = read(JSON_SRC)
    gc = read(GC)
    tst = read(TEST)
    build = read(BUILD)
    cmake = read(CMAKE)
    allow = read(ALLOW)

    # AC1 -- the shared open-addressed probe
    must("Issue #4612" in vec, "AC1: the vector primitives file cites Issue #4612")
    must("eval_hash_probe" in vec, "AC1: the shared probe helper exists")
    must("kNoHashSlot" in vec, "AC1: the probe has a miss sentinel")
    must(
        "((kh >> 1) + at) & (cap - 1)" in vec,
        "AC1: the probe walks the open-addressed chain of the key hash",
    )
    must(
        "eval_hash_keys_eq" in vec,
        "AC1: the probe compares keys with eval_hash_keys_eq (the #4489 / #4505 semantics)",
    )
    must("kEmptySlot" in vec, "AC1: the probe stops at the empty slot (the chain end)")

    # AC2 -- the four lookup prims probe, they do not scan
    for name in ("hash-ref", "hash-has-key?", "hash-set!", "hash-remove!"):
        w = prim_window(vec, name)
        must(bool(w), f"AC2: the {name} primitive is registered")
        must("eval_hash_probe" in w, f"AC2: the {name} primitive probes by hash")
        must(
            "i < ht->capacity" not in w,
            f"AC2: the {name} primitive no longer scans every slot of the table",
        )

    # AC3 -- one string hash (the native string-hash of #4614)
    must("fnv1a_bytes" in vec, "AC3: the key hash uses the native FNV-1a (hash_meta.h)")
    must("fnv1a_bytes" in pair, "AC3: the native string-hash primitive uses the same FNV-1a")

    # AC4 -- the removal writes a tombstone; the rehash / walk paths skip it
    must("kTombstoneSlot" in meta, "AC4: hash_meta.h carries the shared tombstone marker")
    must(
        "kTombstoneSlot = 0x7F" in meta,
        "AC4: the tombstone marker is 0x7F (below the fingerprint range)",
    )
    rm = prim_window(vec, "hash-remove!")
    must(
        "kTombstoneSlot" in rm and "meta[i] = 0xFF" not in rm,
        "AC4: hash-remove! writes a tombstone, not an empty slot",
    )
    must(
        vec.count("kTombstoneSlot") >= 5 and vec.count("hash_slot_is_live") >= 3,
        "AC4: the grow / insert / walk paths of the hash table know the tombstone",
    )
    must("kTombstoneSlot" in js, "AC4: the json rehash path skips the tombstone")
    must("kTombstoneSlot" in gc, "AC4: the GC walk skips the tombstone")
    # Every reader of a hash table must skip the deleted slot: a
    # tombstone keeps its (stale) key / value bytes, so a reader that
    # only tests "!= 0xFF" would resurrect a removed key.
    for rel in (
        "evaluator_primitives_json.cpp",
        "evaluator_primitives_pair.cpp",
        "evaluator_primitives_policy.cpp",
        "evaluator_primitives_query_workspace.cpp",
        "evaluator_primitives_agent.cpp",
        "evaluator_gc.cpp",
        "query_result_decode.hh",
    ):
        must(
            "hash_slot_is_live" in read(ROOT / "src" / "compiler" / rel),
            f"AC4: {rel} skips the deleted slots",
        )

    # AC5 -- the IR path uses the same primitive (the same probe)
    at = ir.find("case IROpcode::HashRef")
    must(at >= 0, "AC5: the IR executor has a HashRef opcode")
    if at >= 0:
        w = ir[at : at + 2000]
        must('"hash-ref"' in w, "AC5: the IR HashRef opcode dispatches to the hash-ref primitive")

    # AC6 -- the test home
    for arm in (
        "ac1_source_cite",
        "ac2_flat_costs",
        "ac3_removal_tombstone",
        "ac4_list_vector_keys",
        "ac5_ir_path",
        "ac6_gate_wiring",
    ):
        must(arm in tst, f"AC6: tests/compiler/test_hash_probe.cpp has {arm}")
    must("run_test_hash_probe" in tst, "AC6: the test file keeps its runner")
    must("Issue #4612" in tst, "AC6: the test file cites Issue #4612")

    # AC7 -- wiring
    must("check_hash_probe_4612.py" in build, "AC7: build.py runs the linter")
    must("test_hash_probe" in cmake, "AC7: CMakeLists registers the test target")
    must("check_hash_probe_4612.py" in allow, "AC7: the allowlist lists the linter")

    # AC8 -- house rules
    issues_dir = ROOT / "tests" / "issues"
    must(
        not list(issues_dir.glob("*4612*")) if issues_dir.is_dir() else True,
        "AC8: no tests/issues/ home for #4612",
    )
    design_dir = ROOT / "docs" / "design"
    must(
        not list(design_dir.glob("4612-*")) if design_dir.is_dir() else True,
        "AC8: no docs/design/4612-* file",
    )

    if failures:
        print(f"check_hash_probe_4612: FAIL ({len(failures)} row(s))")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_hash_probe_4612: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
