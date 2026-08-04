#!/usr/bin/env python3
"""Issue #2179 — cross-function instruction-level impact scope coverage linter.

Verifies the AC1-AC5 surface:
  - ir_cache_pure.ixx has the new compute_impact_scope overload that
    takes (irs, node_dep_graph, mutated_name) + import for
    aura.compiler.dirty_propagation + cross-function cascade logic
    that scans IROpcode::Call + uses node_dep_graph.dependents.
  - observability_metrics.h has 2 new counters:
    impact_scope_cross_fn_blocks_total{0} +
    impact_scope_cross_fn_instrs_total{0}.
  - service_dirty.cpp wires the new overload + bumps both counters.
  - evaluator_primitives_query.cpp registers query:impact-scope-stats
    primitive with schema-2179 / issue-2179 / wired sentinel +
    blocks-total key + instrs-total key.
  - test_instruction_level_impact_partial_2109.cpp has AC7
    function call + Issue #2179 source-cite.

--self-test validates the regex patterns against stub inputs.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

CONTRACT_ROWS = [
    {
        "name": "ir_cache_pure new overload cites 2179",
        "path": "src/compiler/ir_cache_pure.ixx",
        "patterns": [
            r"Issue #2179",
            r"aura\.compiler\.dirty_propagation",
            r"IROpcode::Call",
            r"node_dep_graph\.dependents",
            # Issue #2246 — refine #2179 — indirect + unresolved
            r"Issue #2246",
            r"cross_fn_indirect_hits",
            r"unresolved_callee_hits",
            r"is_unresolved_callish_for_2246",
            r"IROpcode::Apply",
        ],
    },
    {
        "name": "observability_metrics cross-fn counters",
        "path": "src/compiler/observability_metrics.h",
        "patterns": [
            r"impact_scope_cross_fn_blocks_total\{0\}",
            r"impact_scope_cross_fn_instrs_total\{0\}",
            r"Issue #2179",
            # Issue #2246 — 2 new counters (indirect + unresolved)
            r"impact_scope_cross_fn_indirect_total\{0\}",
            r"impact_scope_unresolved_callee_total\{0\}",
            r"Issue #2246",
        ],
    },
    {
        "name": "service_dirty wires cross-fn overload + bumps counters",
        "path": "src/compiler/service_dirty.cpp",
        "patterns": [
            r"impact_scope_cross_fn_blocks_total",
            r"impact_scope_cross_fn_instrs_total",
            r"Issue #2179",
            # Issue #2246 — bump sites for 2 new counters
            r"impact_scope_cross_fn_indirect_total",
            r"impact_scope_unresolved_callee_total",
        ],
    },
    {
        "name": "query:impact-scope-stats primitive",
        "path": "src/compiler/evaluator_primitives_query.cpp",
        "patterns": [
            r'"query:impact-scope-stats"',
            r"impact-scope-cross-fn-blocks-total",
            r"impact-scope-cross-fn-instrs-total",
            r"schema-2179",
            # Issue #2246 — 2 new query keys + schema-2246 lineage
            r"impact-scope-cross-fn-indirect-total",
            r"impact-scope-unresolved-callee-total",
            r"schema-2246",
        ],
    },
    {
        "name": "test AC7 + 2179 source-cite",
        "path": "tests/compiler/test_instruction_level_impact_partial_2109.cpp",
        "patterns": [
            r"ac7_cross_function_instr_2179",
            r"Issue #2179",
            r'schema-2179"\)\s*==\s*2179',
            r'impact-scope-cross-fn-wired"\)\s*==\s*1',
        ],
    },
    {
        "name": "test AC8 + AC9 + #2246 source-cite",
        "path": "tests/compiler/test_instruction_level_impact_partial_2109.cpp",
        "patterns": [
            r"ac8_cross_function_indirect_2246",
            r"ac9_cross_function_unresolved_2246",
            r"Issue #2246",
        ],
    },
]


def check() -> int:
    failed = 0
    for row in CONTRACT_ROWS:
        path = REPO / row["path"]
        if not path.exists():
            print(f"FAIL: {row['name']}: missing file {row['path']}")
            failed += 1
            continue
        text = path.read_text()
        for pat in row["patterns"]:
            if not re.search(pat, text):
                print(f"FAIL: {row['name']}: missing /{pat}/ in {row['path']}")
                failed += 1
            else:
                print(f"  ok  {row['name']}: {pat}")
    if failed:
        print(f"\n{len(CONTRACT_ROWS) - failed}/{len(CONTRACT_ROWS)} contract rows OK; {failed} FAILED")
        return 1
    print(f"\n{len(CONTRACT_ROWS)}/{len(CONTRACT_ROWS)} contract rows OK")
    return 0


def self_test() -> int:
    """Verify the regex patterns match their intended anchors."""
    stub_ir = (
        "Issue #2179: cross-fn cascade\n"
        "Issue #2246: indirect / unresolved callees\n"
        "import aura.compiler.dirty_propagation;\n"
        "if (ins.opcode != aura::ir::IROpcode::Call) continue;\n"
        "auto* callers = node_dep_graph.dependents(mutated_nid);\n"
        "if (ins.opcode == aura::ir::IROpcode::Apply) {\n"
        "    ++result.cross_fn_indirect_hits;\n"
        "}\n"
        "if (is_unresolved_callish_for_2246(ins)) {\n"
        "    ++result.unresolved_callee_hits;\n"
        "}\n"
    )
    stub_om = (
        "std::atomic<std::uint64_t> impact_scope_cross_fn_blocks_total{0}; // #2179\n"
        "std::atomic<std::uint64_t> impact_scope_cross_fn_instrs_total{0}; // #2179\n"
        "std::atomic<std::uint64_t> impact_scope_cross_fn_indirect_total{0}; // #2246\n"
        "std::atomic<std::uint64_t> impact_scope_unresolved_callee_total{0}; // #2246\n"
        "// Issue #2179: cross-function instruction-level impact scope\n"
        "// Issue #2246: indirect + unresolved callee hits\n"
    )
    stub_dirty = (
        "// Issue #2179: cross-function impact scope\n"
        "metrics_.impact_scope_cross_fn_blocks_total.fetch_add(1);\n"
        "metrics_.impact_scope_cross_fn_instrs_total.fetch_add(1);\n"
        "metrics_.impact_scope_cross_fn_indirect_total.fetch_add(1);\n"
        "metrics_.impact_scope_unresolved_callee_total.fetch_add(1);\n"
    )
    stub_epq = (
        "ObservabilityPrims::register_stats_impl(\n"
        '    "query:impact-scope-stats",\n'
        '    insert_kv("impact-scope-cross-fn-blocks-total", 1);\n'
        '    insert_kv("impact-scope-cross-fn-instrs-total", 1);\n'
        '    insert_kv("impact-scope-cross-fn-indirect-total", 1);\n'
        '    insert_kv("impact-scope-unresolved-callee-total", 1);\n'
        '    insert_kv("schema-2179", 2179);\n'
        '    insert_kv("schema-2246", 2246);\n'
        '    insert_kv("issue-2246", 2246);\n'
        '    insert_kv("impact-scope-cross-fn-wired", 1);\n'
        ");\n"
    )
    stub_test = (
        "// Issue #2179\n"
        "// Issue #2246\n"
        "static void ac7_cross_function_instr_2179() {}\n"
        "static void ac8_cross_function_indirect_2246() {}\n"
        "static void ac9_cross_function_unresolved_2246() {}\n"
        'CHECK(href(cs, "schema-2179") == 2179, "schema-2179 sentinel");\n'
        'CHECK(href(cs, "impact-scope-cross-fn-wired") == 1, "wired sentinel");\n'
    )
    stubs = {
        "src/compiler/ir_cache_pure.ixx": stub_ir,
        "src/compiler/observability_metrics.h": stub_om,
        "src/compiler/service_dirty.cpp": stub_dirty,
        "src/compiler/evaluator_primitives_query.cpp": stub_epq,
        "tests/compiler/test_instruction_level_impact_partial_2109.cpp": stub_test,
    }
    failed = 0
    for row in CONTRACT_ROWS:
        text = stubs.get(row["path"], "")
        for pat in row["patterns"]:
            if not re.search(pat, text):
                print(f"FAIL self-test: {row['name']}: /{pat}/")
                failed += 1
    if failed:
        print(f"self-test: {failed} pattern failures")
        return 1
    print("self-test: all patterns OK")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(check())
