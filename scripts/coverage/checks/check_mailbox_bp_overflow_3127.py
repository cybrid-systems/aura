#!/usr/bin/env python3
"""Issue #3127: mailbox BP scope map overflow \u2192 overflow-only bucket.

Per-scope BP gauge map (g_scope_bp_map) is bounded at
kMailboxBpScopeMapCap (256). When overflow hits and
production_defaults_active is true, redirect the new scope to a shared
overflow-only bucket (g_scope_bp_overflow) instead of LRU-evict + insert.
Soft/Off keeps the existing LRU-evict + insert behavior (zero extra
cost: overflow gauge never touched under Soft).

Contract:
  AC1 capability_model analogue: agent_spawn.h has ScopeBpOverflowGauge,
      g_scope_bp_overflow, and the new counter
      spawn_bp_scope_overflow_dropped_total.
  AC2 agent_spawn.h note_mailbox_bp_recent_event overflow path: under
      production_defaults_active, bumps overflow gauge + return early
      (skips insert). Under Soft/Off, falls through to existing
      LRU-evict + insert.
  AC3 agent_spawn.h load_mailbox_bp_recent falls back to overflow
      gauge recent when scope not in map (under production).
  AC4 agent_spawn.h maybe_decay_mailbox_bp_recent also decays the
      overflow gauge (production-gated).
  AC5 tests/orch/test_mailbox_bp_admit.cpp: ac3127_overflow_isolation
      covers source-cite + Soft/Off preservation + production path.
  AC6 no new tests/issues/test_issue_3127.cpp (per #81967 src-aligned).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    aspawn = _read("src/orch/agent_spawn.h")
    test_bp = _read("tests/orch/test_mailbox_bp_admit.cpp")
    test_per = _read("tests/orch/test_per_scope_bp_admit.cpp")

    # AC1 \u2014 ScopeBpOverflowGauge + g_scope_bp_overflow + new counter wired.
    must("struct ScopeBpOverflowGauge", "AC1", aspawn)
    must("inline ScopeBpOverflowGauge g_scope_bp_overflow{}", "AC1", aspawn)
    must("spawn_bp_scope_overflow_dropped_total", "AC1", aspawn)
    must("Issue #3127", "AC1", aspawn)
    # Production gating on the overflow gauge.
    must("production_defaults_active()", "AC1", aspawn)
    # Sibling surfaces preserved (Issue #2778 + LRU-evict + scope map cap).
    must("kMailboxBpScopeMapCap", "AC1 sibling", aspawn)
    must("spawn_bp_scope_overflow_total", "AC1 sibling", aspawn)
    must("g_scope_bp_map", "AC1 sibling", aspawn)
    must("LRU-evict", "AC1 sibling", aspawn)

    # AC2 \u2014 note_mailbox_bp_recent_event overflow path branches on
    # production_defaults_active. Soft/Off falls through to existing
    # LRU-evict + insert (zero extra cost: overflow gauge never touched).
    note_pos = aspawn.find("inline void note_mailbox_bp_recent_event")
    next_func = aspawn.find("inline std::shared_ptr<ScopeBpGauge> lookup_scope_bp_gauge", note_pos)
    note_block = aspawn[note_pos:next_func] if next_func > note_pos else aspawn[note_pos:]
    must("if (production_defaults_active())", "AC2 production-gated", note_block)
    must("g_scope_bp_overflow.recent.fetch_add", "AC2 overflow gauge bumped", note_block)
    must("g_scope_bp_overflow.last_event_us.store", "AC2 overflow clock", note_block)
    must("spawn_bp_scope_overflow_dropped_total.fetch_add", "AC2 dropped counter", note_block)
    must("return;", "AC2 early-return (skip insert)", note_block)
    # LRU-evict path preserved (the existing coldest-evict block stays).
    must("g_scope_bp_map.erase(coldest)", "AC2 LRU-evict preserved", note_block)
    must("spawn_bp_scope_overflow_total.fetch_add", "AC2 overflow_total counter preserved", note_block)

    # AC3 \u2014 load_mailbox_bp_recent falls back to overflow gauge under production.
    load_pos = aspawn.find("load_mailbox_bp_recent")
    decay_pos = aspawn.find("inline void maybe_decay_mailbox_bp_recent", load_pos)
    load_block = aspawn[load_pos:decay_pos] if decay_pos > load_pos else aspawn[load_pos:]
    must("return gauge->recent.load(std::memory_order_relaxed)", "AC3 scope gauge path", load_block)
    must("g_scope_bp_overflow.recent.load(std::memory_order_relaxed)", "AC3 overflow fallback", load_block)
    must("if (production_defaults_active())", "AC3 production gate", load_block)
    # Soft/Off returns 0 (no mis-fire signal).
    must("return 0;", "AC3 Soft/Off zero", load_block)

    # AC4 \u2014 maybe_decay_mailbox_bp_recent also decays the overflow gauge.
    decay_block = aspawn[decay_pos:] if decay_pos > 0 else aspawn
    must("g_scope_bp_overflow.recent.store(0, std::memory_order_release)", "AC4 overflow recent zero", decay_block)
    must(
        "g_scope_bp_overflow.last_event_us.store(0, std::memory_order_release)", "AC4 overflow clock zero", decay_block
    )
    must("production_defaults_active()", "AC4 decay production-gated", decay_block)
    # Per-scope decay loop preserved.
    must("for (const auto& [_, g] : g_scope_bp_map)", "AC4 sibling per-scope loop", decay_block)

    # AC5 \u2014 test_mailbox_bp_admit.cpp extended with ac3127_overflow_isolation.
    must("#3127", "AC5 test source-cite", test_bp)
    must("ScopeBpOverflowGauge", "AC5 test struct cite", test_bp)
    must("spawn_bp_scope_overflow_dropped_total", "AC5 test counter cite", test_bp)
    must("production_defaults_active()", "AC5 test production gate", test_bp)
    must("load_mailbox_bp_recent", "AC5 test load_mailbox_bp_recent call", test_bp)
    must("Soft/Off", "AC5 test Soft path", test_bp)
    must("kMailboxBpScopeMapCap", "AC5 test map size", test_bp)
    # Sibling #2228 ACs preserved (BP admit + counters intact).
    must("Issue #2228", "AC5 sibling #2228 intact", test_bp)
    must("spawn_bp_admit_reject_total", "AC5 sibling", test_bp)
    must("mailbox_bp_recent_total", "AC5 sibling", test_bp)
    # Sibling #2591 per-scope BP test file also kept.
    must("Issue #2591", "AC5 sibling #2591", test_per)

    # AC6 \u2014 no new tests/issues/test_issue_3127.cpp (per #81967 src-aligned).
    issue_test = _read("tests/issues/test_issue_3127.cpp")
    if issue_test:
        fails.append("AC6: tests/issues/test_issue_3127.cpp exists (must NOT \u2014 src/-aligned suite per #81967)")

    if fails:
        print("check_mailbox_bp_overflow_3127: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_mailbox_bp_overflow_3127: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
