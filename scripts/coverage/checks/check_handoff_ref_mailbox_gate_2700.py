#!/usr/bin/env python3
"""Issue #2700: mailbox + long-hold MutationBoundary interleaving gate linter.

Contract (one row per AC):
  AC1 src/serve/multi_fiber_mailbox.h documents the happens-before
     contract ("outermost MutationBoundary held ⇒ mailbox
     StableNodeRef payloads require handoff_completed; otherwise Closed +
     bump handoff_reject_total"). The push + broadcast_fanout gates
     share the same authority (`g_mf_mailbox_stats.handoff_reject_total`
     + local_stats_.handoff_reject_total).
  AC2 All push / broadcast_fanout / fan-out paths share the same gate.
     Zero-cost on ordinary string payloads (existing short-circuit
     preserved — `msg.held_ref_token.has_value()` guards).
  AC3 Chaos / unit coverage in
     tests/serve/test_mailbox_recv_mutation_boundary.cpp ac2700_1..6
     (Fiber A holds Guard; Fiber B push without handoff_ref → Closed +
     bump reject counter; after Guard exit + proper handoff → push
     succeeds).
  AC4 Soft / sandbox observe-only — production hard-rejects unexported
     refs under hold (linter asserts `is_strict` / `production`
     path branch presence in push/broadcast gates).
  AC5 Coverage linter locks the gate sites + test extension.
  AC6 No new docs/design/ per #1655.

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
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")

    # AC1 — contract documentation + gate counters
    must("Issue #2700", "AC1", mb)
    must("handoff_ref", "AC1", mb)
    must("handoff_completed", "AC1", mb)
    must("handoff_reject_total", "AC1", mb)
    must("g_mf_mailbox_stats", "AC1", mb)
    must("MutationBoundaryGuard", "AC1", mb)
    must("workspace_mtx_", "AC1", mb)

    # AC2 — push + broadcast_fanout gates share the same authority
    must("push", "AC2", mb)
    must("broadcast_fanout", "AC2", mb)
    must("held_ref_token.has_value()", "AC2", mb)
    must("handoff_reject_total.fetch_add", "AC2", mb)

    # AC4 — Soft / sandbox observe-only + production hard-rejects
    must("is_strict", "AC4", mb)
    must("Soft", "AC4", mb)
    must("Closed", "AC4", mb)

    # AC5 — query surface + linter + test extension
    must("query:handoff-ref-mailbox-gate", "AC5", q)
    must("handoff-reject-total", "AC5", q)
    must("mailbox-deferred-mutation-hold-total", "AC5", q)
    must("schema-2700", "AC5", q)
    must("issue-2700", "AC5", q)
    must("ac2700_1_push_under_guard_rejects", "AC5", t)
    must("ac2700_2_broadcast_fanout_under_guard_rejects", "AC5", t)
    must("ac2700_3_after_guard_exit_handoff_push_succeeds", "AC5", t)
    must("ac2700_4_zero_cost_on_string_payload", "AC5", t)
    must("ac2700_5_query_keys_and_source_cite", "AC5", t)
    must("ac2700_6_no_docs_design", "AC5", t)
    must("check_handoff_ref_mailbox_gate_2700", "AC5", build)

    # AC6 — no docs/design/2700-* on disk
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2700-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior #2699 + #2693 linters still green
    for prev in (
        "check_steal_safety_transaction_2699.py",
        "check_joint_epoch_bump_coverage.py",
    ):
        r = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "coverage" / "checks" / prev),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2700 mailbox + long-hold MutationBoundary interleaving gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
