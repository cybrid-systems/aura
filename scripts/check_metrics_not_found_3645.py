#!/usr/bin/env python3
# scripts/check_metrics_not_found_3645.py -- Issue #3645 source-cite gate.
#
# AC1: engine:metrics by-name lookup miss returns a typed not-found hash
#      (ok=#f, status=not-found, name echoed, schema-3531) — no longer
#      make_void(). Built via the #3018 fail-soft builder, so the failure
#      shape can never void on capacity either.
# AC2: success shapes unchanged — :all / :prefix keep {"schema", 2}; no
#      ok=#t is added to existing hashes (old consumers keep their keys).
# AC3: :prefix "query:" still lists the #3603 forensic names; the miss
#      path is gated by nothing (Soft / s0 typed miss) — no production
#      gate inserted on the by-name branch.
# AC4: test fixture flips — the old "missing stats name → void" rows now
#      assert the not-found hash; ac3645 rows present in
#      tests/compiler/test_engine_metrics_facade.cpp.
# AC5: lib/std/stats.aura lists the forensic names (dual-track aligned);
#      no new query:* prim; no docs/design/3645*; no tests/**/
#      test_issue_3645.cpp; build.py registration.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

OBS = "src/compiler/evaluator_primitives_obs_jit.cpp"
TEST = "tests/compiler/test_engine_metrics_facade.cpp"
STATS_AURA = "lib/std/stats.aura"
BUILD = "build.py"

LINTER = "check_metrics_not_found_3645"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(obs: str, test: str, stats_aura: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — typed not-found hash on by-name miss.
    must("Issue #3645", "AC1 cite", obs)
    must('"not-found"', "AC1 status token", obs)
    must('{"schema-3531", make_int(3531)}', "AC1 schema stamp", obs)
    must('{"ok", make_bool(false)}', "AC1 ok=#f", obs)
    must('{"name", make_string(nm_idx)}', "AC1 name echo", obs)
    must("return build_hash(miss_kv);", "AC1 routed through #3018 builder", obs)
    must("ev.string_heap_.push_back(miss_name);", "AC1 name interned post-copy", obs)

    # AC2 — success shapes unchanged (schema 2 on :all / :prefix; no ok key).
    must('kv.push_back({"schema", make_int(2)});', "AC2 :all/:prefix schema 2 intact", obs)

    # AC3 — gate-free miss (Soft / s0 typed miss), prefix catalog intact.
    must("No production gate on this path", "AC3 gate-free note", obs)

    # AC4 — fixture flips + ac3645 suite rows.
    must("missing stats name → not-found hash (#3645)", "AC4 flip A row", test)
    must("#3018 AC3 / #3645: by-name miss is typed not-found hash", "AC4 flip B row", test)
    must("3645 AC1: miss returns hash, not void", "AC4 suite AC1", test)
    must("3645 AC1: status=not-found", "AC4 suite status row", test)
    must("3645 AC2: query:security-posture non-void", "AC4 suite AC2", test)
    must("3645 AC2: evolution-audit-decision non-void", "AC4 suite AC2b", test)
    must("3645 AC3: :prefix query: lists security-audit", "AC4 suite AC3", test)
    must("3645 AC5: no docs/design/3645-* per #1655", "AC4 suite AC5", test)

    # AC5 — stats.aura dual-track alignment; no invent; build.py wiring.
    must('"query:security-audit"', "AC5 stats.aura security-audit", stats_aura)
    must('"query:evolution-audit-decision"', "AC5 stats.aura evolution-audit-decision", stats_aura)
    must('"query:security-schedule-gate"', "AC5 stats.aura schedule-gate", stats_aura)
    must("Issue #3645", "AC5 stats.aura cite", stats_aura)
    must_not("engine-metrics-not-found:", "AC5 no new query:* prim", obs)
    for rel in ("tests/compiler/test_issue_3645.cpp", "tests/issues/test_issue_3645.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: {rel} exists")
    if list(Path(ROOT / "docs" / "design").glob("3645*")):
        fails.append("AC5: docs/design/3645-* exists")
    must(LINTER, "AC5 build registration", build)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue 3645 metrics not-found gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    obs = _read(OBS)
    test = _read(TEST)
    stats_aura = _read(STATS_AURA)
    build = _read(BUILD)
    if args.self_test:
        broken = obs.replace("schema-3531", "schema-redacted")
        self_fails = _rows(broken, test, stats_aura, build)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(obs, test, stats_aura, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
