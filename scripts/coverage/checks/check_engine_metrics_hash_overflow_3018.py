#!/usr/bin/env python3
"""Issue #3018: engine:metrics :all / :prefix fail-soft on hash insert miss.

Contract (one row per AC):
  AC1  Forced undersized table still returns a hash with schema + overflow=1;
       never FlatHashTable::destroy + void for capacity alone.
  AC2  Normal :all under current catalog is a full map (no overflow key).
  AC3  :prefix "query:" stays a hash; missing impl is still void-per-key.
  AC4  Soft/Off extra cost is one force-cap load; no second metrics bus.
  AC5  Extend test_engine_metrics_facade + engine_metrics.aura; no
       test_issue_3018.cpp; no docs/design/ (#1655). Additive overflow
       counter only.

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

    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_engine_metrics_facade.cpp")
    suite = _read("tests/suite/engine_metrics.aura")
    build = _read("build.py")

    # ── AC1: fail-soft builder ──
    must("Issue #3018", "AC1", obs)
    must("build_engine_metrics_hash", "AC1", obs)
    must("kv.size()) * 2 + 8", "AC1 headroom", obs)
    must("never FlatHashTable::destroy + return void for capacity alone", "AC1", obs)
    must("engine_metrics_hash_force_overflow_sentinel", "AC1 sentinel", obs)
    must("overflow", "AC1 overflow key", obs)
    # engine:metrics and stats:get all must use the shared helper.
    must("return build_engine_metrics_hash(ev, kv);", "AC1 engine:metrics", obs)
    # Must not destroy+void in the facade helper itself.
    helper = obs[obs.find("static EvalValue build_engine_metrics_hash") :]
    helper = helper[: helper.find("void ObservabilityPrims::register_metrics_facade")]
    if "FlatHashTable::destroy" in helper:
        fails.append("AC1: build_engine_metrics_hash must not destroy on insert miss")
    if "return make_void()" not in helper:
        fails.append("AC1: OOM create() may still return void")

    # ── AC2 / AC3: tests ──
    must("#3018 AC2: normal :all has no overflow key", "AC2", test)
    must("#3018 AC3: :prefix query: returns hash", "AC3", test)
    must("#3018 AC3: missing impl still void-per-key", "AC3", test)
    must("engine:metrics-overflow-3018", "AC3 suite", suite)
    must('(not (hash-has-key? all "overflow"))', "AC2 suite", suite)

    # ── AC4: Soft / no second bus ──
    must("g_engine_metrics_force_hash_cap.load", "AC4", obs)
    must("no second metrics bus", "AC4", obs)
    if "AgentRegistry" in helper:
        fails.append("AC4: must not introduce AgentRegistry")

    # ── AC5: wiring ──
    must("#3018 AC1: forced undersized :all still returns hash", "AC5", test)
    must("aura_engine_metrics_set_force_hash_cap", "AC5 hook", test)
    must("g_engine_metrics_hash_overflow_total", "AC5 counter", obs)
    must("check_engine_metrics_hash_overflow_3018", "AC5 build", build)
    must("cmd_engine_metrics_hash_overflow_3018", "AC5 build cmd", build)
    must("aura_add_issue_test(test_engine_metrics_facade)", "AC5 cmake", _read("CMakeLists.txt"))
    for rel in (
        "tests/compiler/test_issue_3018.cpp",
        "tests/core/test_issue_3018.cpp",
    ):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #81967")
    if _read("docs/design/3018-engine-metrics-hash-overflow.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3018 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3018 engine:metrics hash overflow fail-soft — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
