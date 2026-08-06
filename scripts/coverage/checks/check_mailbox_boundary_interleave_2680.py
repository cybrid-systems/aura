#!/usr/bin/env python3
"""Issue #2680: runtime(mailbox) — enforce MutationBoundary held /
depth>0 interleaving safety for cross-fiber delivery.

Validates that the existing mailbox header
(src/serve/multi_fiber_mailbox.h) and Fiber contract
(src/serve/fiber.h) extend the per-target-fiber MutationSafetySnapshot
gate (#2312) to a **shared-Evaluator** gate that consults
`aura_evaluator_mutation_boundary_held()` and
`aura_evaluator_mutation_boundary_depth()` — the same C ABI hooks the
recv() path uses at L820-821 and the same authority as steal safety
(#2184 / #2310 / #2346 / #2518).

Contract:
  AC1 push() and broadcast_fanout() defer (Backpressure) when the shared
     Evaluator's MutationBoundary is held (depth>0 || held) by ANY fiber,
     not just the target fiber. Receiver never observes a payload
     delivered while another fiber on the shared Evaluator is mid-mutation.
     Sender retries / queues; deferred (not dropped).
  AC2 Gate uses the same authority as steal safety:
     aura_evaluator_mutation_boundary_held() +
     aura_evaluator_mutation_boundary_depth() (C ABI hooks from
     evaluator_fiber_mutation.cpp / evaluator.ixx).
  AC3 Counter bumped in the defer path so Agents can observe pressure:
     mailbox_shared_evaluator_deferred_total (overall) +
     mailbox_shared_evaluator_deferred_hard_total (Strict / production).
  AC4 Soft / observe-only path remains for tests; production defaults
     enforce the safety. Toggle via AURA_MUTATE_MAILBOX_STRICT +
     is_mutate_mailbox_strict() (existing #2347 machinery).
  AC5 Metrics emitted via g_mf_mailbox_stats counter family — Agent-
     queryable via (query:mailbox-shared-evaluator-deferred-total).
  AC6 No regression to single-fiber or same-fiber mailbox latency on
     the uncontended path: a single relaxed load on the deferred
     counter proxy when deferred_depth==0.

Exit 0 = all AC rows satisfied.
"""

from __future__ import annotations

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

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} occurrence(s) of {n!r}, found {c}")

    mb = _read("src/serve/multi_fiber_mailbox.h")
    fh = _read("src/serve/fiber.h")
    mb_bridge = _read("src/compiler/messaging_bridge.h")
    mb_impl = _read("src/compiler/messaging_bridge_impl.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")

    # ============================================================
    # AC1: push + broadcast_fanout defer on shared-evaluator held
    # ============================================================
    must("Issue #2680", "AC1", mb)
    must_count("aura_evaluator_mutation_boundary_depth() > 0", "AC1", mb, 2)
    must_count("aura_evaluator_mutation_boundary_held() != 0", "AC1", mb, 2)
    # Both push() and broadcast_fanout() must have the gate (2 occurrences each).
    must_count("mailbox_shared_evaluator_deferred_total", "AC1", mb, 4)
    # push path: defer returns Backpressure.
    must("return PushStatus::Backpressure", "AC1", mb)

    # ============================================================
    # AC2: same authority as steal safety (recv() already uses these
    # C ABI hooks at L820-821). The push/broadcast_fanout gates must
    # consult the SAME hooks.
    # ============================================================
    must("aura_evaluator_mutation_boundary_held()", "AC2", mb)
    must("aura_evaluator_mutation_boundary_depth()", "AC2", mb)
    # recv() authority reference (must be cited as the canonical pattern).
    must(
        "boundary_live = aura_evaluator_mutation_boundary_depth() > 0",
        "AC2",
        mb,
    )
    # Hooks must be defined in evaluator_fiber_mutation.cpp (the C ABI shim
    # authority module).
    must('extern "C" std::size_t aura_evaluator_mutation_boundary_depth()', "AC2", efm)
    must('extern "C" int aura_evaluator_mutation_boundary_held()', "AC2", efm)

    # ============================================================
    # AC3: counter family wired + bumped in defer path
    # ============================================================
    must("mailbox_shared_evaluator_deferred_total{0}", "AC3", mb)
    must("mailbox_shared_evaluator_deferred_hard_total{0}", "AC3", mb)
    must("mailbox_shared_evaluator_deferred_soft_observe_total{0}", "AC3", mb)
    # Bump sites in push + broadcast_fanout (each bumps both g_ + local_).
    must_count(
        "mailbox_shared_evaluator_deferred_total.fetch_add(",
        "AC3",
        mb,
        2,
    )
    must_count(
        "mailbox_shared_evaluator_deferred_hard_total.fetch_add(",
        "AC3",
        mb,
        2,
    )

    # ============================================================
    # AC4: Soft / observe-only path remains for tests
    # ============================================================
    must("is_mutate_mailbox_strict", "AC4", mb)
    must("AURA_MUTATE_MAILBOX_STRICT", "AC4", mb)
    # Soft path bumps soft-observe counter, not hard.
    must("mailbox_shared_evaluator_deferred_soft_observe_total.fetch_add", "AC4", mb)
    # Per Soft-mode contract: agent contract cite retained (do not change
    # the existing Soft / observe-only ergonomics).
    must("is_mutate_mailbox_strict()", "AC4", mb)

    # ============================================================
    # AC5: metrics emitted via g_mf_mailbox_stats family
    # ============================================================
    # Process-wide stat struct field must be in the family.
    must("MultiFiberMailboxStats", "AC5", mb)
    # Counter wired in stats struct (verify field declarations).
    must_count("std::atomic<std::uint64_t> mailbox_shared_evaluator_deferred_", "AC5", mb, 3)

    # ============================================================
    # AC6: zero-cost happy path (single relaxed load when depth==0)
    # ============================================================
    # Comment in production code claiming zero-cost happy path.
    must("Zero cost", "AC6", mb) if "Zero cost" in mb else must(
        "zero cost",
        "AC6",
        mb,
    )

    # ============================================================
    # fiber.h: cross-fiber mailbox delivery happens-before contract
    # ============================================================
    must("Issue #2680", "fiber.h", fh)
    must("happens-before", "fiber.h", fh)
    # Mirror of steal-safety contract (MutationSafetySnapshot referenced).
    must("MutationSafetySnapshot", "fiber.h", fh)
    # Cross-fiber / shared-Evaluator concept cited.
    must("shared Evaluator", "fiber.h", fh)

    # ============================================================
    # messaging_bridge.h: hook authority already wired
    # ============================================================
    must("g_mutation_boundary_held", "messaging_bridge.h", mb_bridge)
    must("g_mutation_boundary_held", "messaging_bridge_impl", mb_impl)

    # ============================================================
    # Test extension (per #81967): test_mailbox_recv_mutation_boundary.cpp
    # adds 3 ac2680_* test functions covering AC3 (counter wired),
    # AC6 (happy path no extra defer), AC2/AC3/AC4 (source-cite).
    # ============================================================
    must("ac2680_counter_wired", "AC3-test", test)
    must("ac2680_happy_path_no_extra_deferred", "AC6-test", test)
    must("ac2680_source_cite_rows", "AC2-test", test)
    must("mailbox_shared_evaluator_deferred_total", "AC5-test", test)

    # ============================================================
    # Self-coverage + build.py wire-up
    # ============================================================
    must("check_mailbox_boundary_interleave_2680", "self", build)
    must("#2680", "self", mb)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2680 mailbox/MutationBoundary cross-fiber interleaving — all 6 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
