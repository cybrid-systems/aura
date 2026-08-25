#!/usr/bin/env python3
"""Issue #3299: production multi-agent **mutate** batches stay host-dependent
on explicit :region-keys (post-#3243 residual).

#3243 already landed the observe/deny mechanisms (decide_isolation,
region_key_missing_serialized, AURA_PARALLEL_REQUIRE_REGION_KEYS deny,
serialized-reason projection). #3299 closes the residual *validation* gap:

  AC1  Soft mutate batch: zero behavior change (serialized, no missing
       signal, no serialized-reason)
  AC2  production + multi-task mutate + all-0 keys: Serialized +
       serialized-reason=missing-or-overlap-keys + per-batch missing=1 +
       counter bumped
  AC3  production + mutate + ≥2 distinct keys: RegionConcurrent + eligible
  AC4  overlap keys mutate: Serialized + missing
  AC5  AURA_PARALLEL_REQUIRE_REGION_KEYS=1 structured deny applies to
       mutate batches
  AC6  README guidance ("multi-agent mutate batches MUST supply
       :region-keys") + no invent test / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    md = _read("src/orch/README.md")
    build = _read("build.py")

    # AC2/AC3/AC4: real mutate soak (mutate:set-body thunks, not pure lambdas)
    must("mutate:set-body", "AC2 mutate soak", test)
    must("kMutateZeroKeys", "AC2 zero-keys snippet", test)
    must("kMutateTwoKeys", "AC3 two-keys snippet", test)
    must("kMutateOverlapKeys", "AC4 overlap snippet", test)
    must("region-key-missing-serialized", "AC2 batch flag", test)
    must("missing-or-overlap-keys", "AC2 reason", test)
    must("region-concurrent", "AC3 RegionConcurrent", test)
    must("AURA_PARALLEL_REQUIRE_REGION_KEYS", "AC5 env", test)
    must("3299 AC6", "AC6 stamp", test)

    # AC1: Soft mutate batch zero behavior change
    must("Soft mutate batch stays serialized", "AC1 Soft", test)
    must("Soft mutate no missing signal", "AC1 no signal", test)

    # AC6: README must carry the recommended path (#2886 alignment)
    must("Multi-agent mutate batches MUST supply", "AC6 README", md)
    must(":region-keys", "AC6 README keys", md)
    must("AURA_PARALLEL_REQUIRE_REGION_KEYS=1", "AC6 README env", md)

    # build.py must wire this linter
    must("check_parallel_mutate_region_keys_3299", "AC6 build.py", build)

    if (ROOT / "tests" / "orch" / "test_issue_3299.cpp").is_file():
        fails.append("AC6: tests/orch/test_issue_3299.cpp present (forbidden #81967)")
    if _read("docs/design/3299-mutate-region-keys.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3299 parallel_mutate_region_keys:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3299 parallel_mutate_region_keys: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
