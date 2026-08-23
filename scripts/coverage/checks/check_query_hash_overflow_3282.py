#!/usr/bin/env python3
"""Issue #3282 linter — residual fixed FlatHashTable::create(N) after #3020.

Post-#3020 residual (obs/query review @ 20c47e2d): security/mutate/obs_*/query_*
domain builders still used fixed `FlatHashTable::create(N)` with a manual
probe whose miss path silently dropped late additive keys (or returned void
solely due to capacity). This migrates every query:/engine:metrics hash
builder in the six named TUs to the #3020 checked pattern:

  FlatHashTable::create(query_hash_capacity_for(planned))
  + bool overflowed = false;
  + insert_kv_checked / overflowed=true on miss
  + query_hash_finish(ht, <heap>, overflowed)  -> stamps hash-overflow=1,
    schema-3020/3244 + issue-3020/3244, never returns void for capacity.

Gate rows:
  G1  six named TUs have NO bare `FlatHashTable::create(<literal>)`
      (only query_hash_capacity_for / dynamic-cap forms remain).
  G2  every query: builder converted to query_hash_finish(ht, *, overflowed)
      (security>=52 finish sites, mutate>=11, obs_jit>=118, obs_eval>=133,
      query>=14, query_obs_mid>=33).
  G3  overflowed flag present at each converted site
      (bool overflowed = false; counts match finish counts).
  G4  no destroy+make_void() capacity-miss path left in the six TUs
      (insert-miss now sets overflowed, keeps existing keys).
  G5  build.py wires this linter.
  G6  test ACs in tests/compiler/test_engine_metrics_facade.cpp (#81967)
      exercising forced overflow (hash-overflow=1 + non-void hash) via
      aura_query_hash_set_force_cap.
  G7  no docs/design/3282-* (per #1655), no tests/issue*/test_issue_3282.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

TUS = [
    "src/compiler/evaluator_primitives_security.cpp",
    "src/compiler/evaluator_primitives_mutate.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "src/compiler/evaluator_primitives_obs_eval.cpp",
    "src/compiler/evaluator_primitives_query.cpp",
    "src/compiler/evaluator_primitives_query_obs_mid.cpp",
]

# expected finish-site floors (pre-#3282 #3020 sites already used the pattern)
FINISH_FLOORS = {
    "src/compiler/evaluator_primitives_security.cpp": 52,
    "src/compiler/evaluator_primitives_mutate.cpp": 11,
    "src/compiler/evaluator_primitives_obs_jit.cpp": 118,
    "src/compiler/evaluator_primitives_obs_eval.cpp": 133,
    "src/compiler/evaluator_primitives_query.cpp": 14,
    "src/compiler/evaluator_primitives_query_obs_mid.cpp": 33,
}

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3282 residual fixed FlatHashTable::create(N) linter ===")
    build = read("build.py")
    test = read("tests/compiler/test_engine_metrics_facade.cpp")

    # G1: no bare create(literal) in the six TUs
    bare_total = 0
    for rel in TUS:
        src = read(rel)
        bare = re.findall(r"FlatHashTable::create\(\d+\)", src)
        bare_total += len(bare)
        if bare:
            print(f"    {rel}: {len(bare)} bare create(literal)")
    must(bare_total == 0, f"G1: no bare FlatHashTable::create(<literal>) in six TUs ({bare_total})")

    # G2/G3: finish + overflowed counts per TU
    for rel in TUS:
        src = read(rel)
        fin = len(re.findall(r"query_hash_finish\(ht,", src))
        ovf = len(re.findall(r"bool overflowed = false;", src))
        floor = FINISH_FLOORS[rel]
        must(fin >= floor, f"G2: {rel} query_hash_finish >= {floor} ({fin})")
        must(ovf >= floor, f"G3: {rel} overflowed flag >= {floor} ({ovf})")

    # G4: no capacity-miss destroy+make_void left in CONVERTED sites
    # (blocks containing query_hash_capacity_for). Pre-existing #3020
    # dynamic-cap builders (create(cap)/create(ncap)) keep their own
    # destroy-miss and are out of scope; null-evaluator guards returning
    # make_int (not make_void) are legitimate.
    void_miss = 0
    for rel in TUS:
        src = read(rel)
        # split into blocks at query_hash_capacity_for markers
        for m in re.finditer(r"query_hash_capacity_for\(", src):
            block = src[m.start() : m.start() + 4000]
            nxt = block.find("query_hash_finish")
            if nxt > 0:
                block = block[:nxt]
            if re.search(r"FlatHashTable::destroy\(ht\);\s*\n\s*return make_void\(\);", block):
                void_miss += 1
    must(void_miss == 0, f"G4: no capacity-miss destroy+make_void in converted blocks ({void_miss})")

    # G4b: converted lambdas route miss to overflowed (spot-check per TU)
    for rel in TUS:
        src = read(rel)
        ok = ("overflowed = true;" in src) or ("insert_kv_checked" in src)
        must(ok, f"G4b: {rel} overflow miss path present")

    # G5: build.py wires this linter
    must("check_query_hash_overflow_3282.py" in build, "G5: build.py wires linter")

    # G6: test ACs exercise forced overflow via force-cap hook
    must("aura_query_hash_set_force_cap" in test, "G6: test uses force-cap hook")
    must("hash-overflow" in test, "G6: test asserts hash-overflow sentinel")
    must("test_ac3282" in test or "3282" in test, "G6: test cites Issue #3282")

    # G7: no docs/design/3282-* per #1655; no tests/issue*/test_issue_3282.cpp
    docs_ok = True
    if (ROOT / "docs/design").exists():
        docs_ok = not any(p.name.startswith("3282-") for p in (ROOT / "docs/design").glob("3282-*"))
    must(docs_ok, "G7a: no docs/design/3282-* per #1655")
    must(
        not (ROOT / "tests" / "issues" / "test_issue_3282.cpp").exists(),
        "G7b: no tests/issues/test_issue_3282.cpp per #81967",
    )

    print()
    if failures:
        print(f"#3282 linter FAILED: {len(failures)} gate(s) — {failures}")
        return 1
    print("#3282 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
