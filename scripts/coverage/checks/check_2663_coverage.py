#!/usr/bin/env python3
"""Issue #2663: enforce StableNodeRef handoff_ref on mailbox push / fanout paths.

Contract (one row per AC):
  AC1 MailMessage declares held_ref_token (optional<uint64_t>) +
     handoff_completed (bool) — structured fields for the held-ref export
     token + the handoff-completed marker.
  AC2 push() gate reads held_ref_token.has_value() && !handoff_completed
     and returns PushStatus::Closed + bumps handoff_reject_total (process-wide
     + per-mailbox local).
  AC3 Ordinary string payload (held_ref_token empty, default) pays zero
     cost — the gate is short-circuited by the optional<>::has_value() check
     before reading handoff_completed or bumping any counter.
  AC4 broadcast_fanout() honors the same gate — all-or-nothing reject (no
     partial fan-out of unexported refs to a subset of attachers).
  AC5 Soft path documented in source comments (production-safe default:
     always Closed + counter bump; Soft / sandbox=off may interpret as
     metric-only — future enhancement).
  AC6 tests/compiler/test_stable_ref_export_validate.cpp extended with
     #2663 AC1-AC6 source-cite block.
  AC7 build.py wires check_2663_coverage into the gate after
     check_export_held_handoff_coverage.
  AC8 cross-check: check_export_held_handoff_coverage + check_pure_parallel_
     isolation_wording remain green (no regression).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mb = _read("src/serve/multi_fiber_mailbox.h")
    test = _read("tests/compiler/test_stable_ref_export_validate.cpp")
    build = _read("build.py")

    # AC1 — MailMessage fields
    must("held_ref_token", "AC1", mb)
    must("handoff_completed", "AC1", mb)
    must("std::optional<std::uint64_t> held_ref_token", "AC1", mb)
    must("bool handoff_completed", "AC1", mb)
    must("Issue #2663", "AC1", mb)

    # AC2 — push() gate
    must("if (msg.held_ref_token.has_value() && !msg.handoff_completed)", "AC2", mb)
    must("g_mf_mailbox_stats.handoff_reject_total.fetch_add(1, std::memory_order_relaxed)", "AC2", mb)
    must("local_stats_.handoff_reject_total.fetch_add(1, std::memory_order_relaxed)", "AC2", mb)
    must("return PushStatus::Closed", "AC2", mb)

    # AC3 — zero cost on ordinary string payloads
    must("held_ref_token.has_value()", "AC3", mb)
    must("zero cost", "AC3", mb)
    must("Ordinary string payloads", "AC3", mb)

    # AC4 — broadcast_fanout gate
    must("if (proto.held_ref_token.has_value() && !proto.handoff_completed)", "AC4", mb)
    must("broadcast_fanout", "AC4", mb)
    must("all-or-nothing reject", "AC4", mb)

    # AC5 — Soft path documented
    must("production-safe default", "AC5", mb)
    must("Soft / sandbox=off", "AC5", mb)

    # AC6 — test file extended
    must("2663 AC1", "AC6", test)
    must("2663 AC2", "AC6", test)
    must("2663 AC3", "AC6", test)
    must("2663 AC4", "AC6", test)
    must("2663 AC5", "AC6", test)
    must("2663 AC6", "AC6", test)
    must("Issue #2663", "AC6", test)

    # AC7 — build.py wires the linter
    must("check_2663_coverage", "AC7", build)

    # Cross-check: check_export_held_handoff_coverage still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_export_held_handoff_coverage.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_export_held_handoff_coverage regression:\n{r1.stdout}\n{r1.stderr}")

    # Cross-check: check_pure_parallel_isolation_wording --self-test still green
    r2 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_pure_parallel_isolation_wording.py"),
            "--self-test",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"check_pure_parallel_isolation_wording --self-test regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2663 mailbox handoff enforcement coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
