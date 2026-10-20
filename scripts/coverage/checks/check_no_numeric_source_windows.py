#!/usr/bin/env python3
"""Reintroduction guard: no bare numeric source windows in converted checks.

The source-cite checks listed here were converted to structural windows
(tests/compiler/source_contract.hpp, scripts/source_window.py). A fixed byte
window in one of them is almost always a regression: growing the function body
silently pushes the marker out of the slice. Keep new edits anchor-based.

Allowed exceptions are explicit per-file substrings and must carry a reason.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

TARGETS = [
    "tests/compiler/test_escape_move_elision_gate.cpp",
    "tests/core/test_per_symbol_dirty_pool_lock.cpp",
    "tests/core/test_type_cache_stats_snapshot.cpp",
    "tests/core/test_raw_pointer_safety.cpp",
    "tests/core/test_force_compact_hard_mutex.cpp",
    "tests/core/test_arena_dtor_clears_hooks.cpp",
    "tests/core/test_capability_single_use_consume.cpp",
    "tests/core/test_restamp_sla_observability.cpp",
    "tests/core/test_arena_required_cover_no_value_only.cpp",
    "tests/core/test_general_object_pin_coverage_gate.cpp",
    "tests/core/test_arena_defrag.cpp",
    "tests/core/test_transaction_guard.cpp",
    "tests/core/test_marker_metadata_lock.cpp",
    "tests/core/test_stable_ref_tenant_capture.cpp",
    "scripts/check_agent_fiber_isolation_4165.py",
    "scripts/check_agent_mint_per_agent_4235.py",
    "scripts/check_apply_closure_tls_seq_4169.py",
    "scripts/check_fiber_body_exception_4231.py",
    "scripts/coverage/checks/check_aot_facade_content_restamp_3481.py",
]

# file -> list of exact line substrings that are allowed. Each entry is a
# deliberately non-body window (call-site / registration region) documented in
# the structural-window audit; do not add entries casually.
ALLOW = {
    "tests/core/test_restamp_sla_observability.cpp": [
        # Per-call-site statement window around abort_restore_dual_topology();
        # the five call sites are distinct and there is no single sibling
        # anchor. The surrounding suite still asserts the exact site count.
        "mb.substr(p, 1200)",
    ],
    "tests/core/test_general_object_pin_coverage_gate.cpp": [
        # In-function OR-condition block: the four preserved result fields are
        # asserted, but the block has no stable closing anchor of its own.
        "arena.substr(fail_close_pos, 1500)",
    ],
    "tests/core/test_arena_defrag.cpp": [
        # Primitive-registration region; markers are comments rather than
        # body-scoped code, so a brace slice would not cover them.
        "mem.substr(pos, 1800)",
    ],
}

CPP_SUBSTR = re.compile(r"\.substr\([^;]*?,\s*(\d{4,})\s*\)")
CPP_INDEX = re.compile(r"\[[^\]\n]*:\s*\w+\s*\+\s*(\d{4,})\s*\]")
PY_INDEX = re.compile(r"\[[^\]\n]*:\s*\w+\s*\+\s*(\d{4,})\s*\]")
PY_WIDTH = re.compile(r"width\s*=\s*(\d{4,})")


def main() -> int:
    fails: list[str] = []
    for rel in TARGETS:
        p = ROOT / rel
        if not p.is_file():
            fails.append(f"{rel}: missing converted check")
            continue
        is_py = p.suffix == ".py"
        for lineno, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if any(allowed in line for allowed in ALLOW.get(rel, [])):
                continue
            patterns = (PY_INDEX, PY_WIDTH) if is_py else (CPP_SUBSTR, CPP_INDEX)
            for pat in patterns:
                m = pat.search(line)
                if m:
                    fails.append(
                        f"{rel}:{lineno}: numeric source window {line.strip()!r} "
                        f">= 1000; use source_contract.hpp / source_window.py anchors"
                    )
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("check_no_numeric_source_windows: OK (all converted checks anchor-based)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
