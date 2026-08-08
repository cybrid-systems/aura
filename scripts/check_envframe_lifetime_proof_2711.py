#!/usr/bin/env python3
"""Issue #2711: EnvFrame dual-epoch Agent-visible lifetime proof.

Closes the multi-fiber Agent observability gap: agents previously had to
join several counters (hold_gen / compact_gen / workspace_epoch /
densify_ownership_scan_* / hold_gen_mismatch_total) to answer "have my
EnvFrame refs survived densify + steal without dual-path lag?". #2711
adds a single read-only snapshot (symmetric to TypeLinearCommitProof
#2697 for type×linear) that packages all the relevant state into one
struct + one query surface.

Contract rows (AC1–AC6 from the test file):

  AC1: snapshot_envframe_lifetime_proof() returns hold_gen / compact_gen /
       mutation_epoch / scans_run / densify_scan_total / densify_scan_fail /
       hold_gen_mismatch_total / would_allow_commit / force_reason_code.
  AC2: Soft + no densify + no guard → zero-cost / empty-healthy.
  AC3: After Moving densify success with ownership scan fail inject →
       proof reports fail / would_allow_commit=false under production.
  AC4: Read-only snapshot — does not replace EnvFrameLifetimeGuard.
  AC5: Additive only — preserve existing envframe-lifetime-* /
       densify-ownership-* keys and schema-2164 / 2340 / 2361 lineage.
  AC6: source-cite + linter + no docs/design/.

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_envframe_lifetime_proof_2711.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        # clang-format may split adjacent string literals; strip quotes/ws.
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    efl = _read("src/core/envframe_lifetime.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_densify_ownership_scan_fail_gate.cpp")
    build = _read("build.py")

    # AC1 — proof struct + snapshot function with all 9 fields.
    must("struct EnvFrameLifetimeProof", "AC1", efl)
    must("snapshot_envframe_lifetime_proof", "AC1", efl)
    must("hold_gen", "AC1", efl)
    must("compact_gen", "AC1", efl)
    must("mutation_epoch", "AC1", efl)
    must("scans_run", "AC1", efl)
    must("densify_scan_total", "AC1", efl)
    must("densify_scan_fail", "AC1", efl)
    must("hold_gen_mismatch_total", "AC1", efl)
    must("would_allow_commit", "AC1", efl)
    must("force_reason_code", "AC1", efl)
    must("kEnvFrameLifetimeProofIssue = 2711", "AC1", efl)

    # AC2 — Soft zero-cost empty-healthy.
    must("g_envframe_last_hold_gen_at_enter", "AC2", efl)
    must("current_mutation_epoch", "AC2", efl)
    must("would_allow_commit = !(fail_densify || fail_mismatch)", "AC2", efl)

    # AC3 — scan_fail reflected in proof.
    must("fail_densify", "AC3", efl)
    must("force_reason_code = (fail_densify ? 1 : 0)", "AC3", efl)
    must("(fail_mismatch ? 2 : 0)", "AC3", efl)

    # AC4 — read-only snapshot.
    must("class EnvFrameLifetimeGuard", "AC4", efl)
    must("no extra atomics on the quiet path", "AC4", efl)

    # AC5 — additive query keys + schema sentinels (format-robust).
    must_key("envframe-lifetime-proof-hold-gen", "AC5", q)
    must_key("envframe-lifetime-proof-compact-gen", "AC5", q)
    must_key("envframe-lifetime-proof-mutation-epoch", "AC5", q)
    must_key("envframe-lifetime-proof-densify-scan-fail", "AC5", q)
    must_key("envframe-lifetime-proof-would-allow-commit", "AC5", q)
    must_key("envframe-lifetime-proof-force-reason-code", "AC5", q)
    must_key("schema-2711", "AC5", q)
    must_key("issue-2711", "AC5", q)
    # Prior surface preserved (regression).
    must_key("schema-2697", "AC5", q)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("Issue #2711", "AC6", efl)
    must("Issue #2711", "AC6", q)
    must("ac2711_1_proof_struct_and_function", "AC6", t)
    must("ac2711_2_soft_zero_cost", "AC6", t)
    must("ac2711_3_scan_fail_reflected_in_proof", "AC6", t)
    must("ac2711_4_read_only_snapshot", "AC6", t)
    must("ac2711_5_query_keys_added", "AC6", t)
    must("ac2711_6_source_and_linter", "AC6", t)
    must("check_envframe_lifetime_proof_2711", "AC6", build)
    if _read("docs/design/2711-envframe-lifetime-proof.md"):
        fails.append("AC6: docs/design/2711-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2711 EnvFrame dual-epoch lifetime proof — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
