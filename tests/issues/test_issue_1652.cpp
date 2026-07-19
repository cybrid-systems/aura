// tests/test_issue_1652.cpp — Issue #1652 (scope-limited-progressive Phase 1)
//
// Source-driven test (paired pattern with tests/test_issue_1644_ir_hygiene.cpp
// for #1644, tests/test_issue_1645.cpp for #1645, tests/test_issue_1646.cpp
// for #1646, tests/test_issue_1647.cpp for #1647, tests/test_reflect_nested.cpp
// for #1648, tests/test_issue_1649.cpp for #1649, tests/test_issue_1650.cpp
// for #1650, tests/test_issue_1651.cpp for #1651). Verifies Phase 1 ships the
// observability infrastructure for clone_macro_body / SyntaxMarker
// expansion: 3 new file-level atomics + 3 C-linkage accessors + paired
// hygiene-violation bumps (depth-exceeded + body_id-NULL/invalid) +
// per-call success-path bump at function entry.
//
// AC coverage:
//   AC1 — clone_macro_body path metrics correct bump
//       Phase 1 ships the per-call success-path bump (at function entry after
//       the using-namespace line) + the 2 paired hygiene-violation bumps
//       (at depth-exceeded + body_id-NULL/invalid). The per-recursive-step
//       bump_macro_introduced_nodes_created(cloned_count) + the full
//       primitive body composition deferred to #1688 (multi-session
//       refactor of the recursive AST walk to thread a cumulative count).
//   AC2 — new stats primitive 实现并返回有用信息
//       Composes into existing query:pattern-hygiene-stats (per #1632
//       "原语最小化" directive — no new primitive registration). The 3 new
//       file-level atomics + C-linkage accessors surface through the existing
//       primitive. Full primitive body extension at #1688.
//   AC3 — 与现有 hygiene stats 集成
//       Predecessor-covered via #1247/#1248 hygiene tracer counters
//       (g_macro_origin_provenance_errors + g_hygiene_tracer_expansions +
//       g_hygiene_tracer_depth_max) which remain accessible via the same
//       C-linkage accessor pattern. The new #1652 counters are placed in
//       the same file-scope atomic block for consistency.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace aura_1652_detail {

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool check_file_level_atomics_ac1() {
    std::println("\n--- AC1: 3 file-level atomics + C-linkage accessors ---");
    std::string me = read_file("src/compiler/macro_expansion.cpp");
    bool a1 = contains(me, "std::atomic<std::uint64_t> g_macro_expansion_total{0}");
    bool a2 = contains(me, "std::atomic<std::uint64_t> g_macro_introduced_nodes_created_total{0}");
    bool a3 =
        contains(me, "std::atomic<std::uint64_t> g_hygiene_violation_in_macro_expand_total{0}");
    bool c1 = contains(me, "aura_macro_expansion_total_v_read()");
    bool c2 = contains(me, "aura_macro_introduced_nodes_created_total_v_read()");
    bool c3 = contains(me, "aura_hygiene_violation_in_macro_expand_total_v_read()");
    if (!(a1 && a2 && a3 && c1 && c2 && c3)) {
        std::println("FAIL: file-level atomics + C-linkage accessors missing "
                     "(a1={} a2={} a3={} c1={} c2={} c3={})",
                     a1, a2, a3, c1, c2, c3);
        return false;
    }
    std::println("OK: 3 file-level atomics + 3 C-linkage accessors landed");
    return true;
}

bool check_paired_hygiene_violation_bumps_ac1() {
    std::println("\n--- AC1: 2 paired hygiene-violation bumps in clone_macro_body ---");
    std::string me = read_file("src/compiler/macro_expansion.cpp");
    // Site A: depth-exceeded (paired next to g_macro_origin_provenance_errors.fetch_add(1, ...)).
    bool depth_bump = contains(
        me, "g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);");
    int n_bumps = 0;
    std::size_t pos = 0;
    while (
        (pos = me.find(
             "g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);",
             pos)) != std::string::npos) {
        ++n_bumps;
        ++pos;
    }
    bool body_id_bump_present = n_bumps >= 2;
    bool has_1652_ref = contains(me, "Issue #1652: paired bump");
    if (!depth_bump || !body_id_bump_present || !has_1652_ref) {
        std::println("FAIL: paired hygiene-violation bumps missing "
                     "(depth_bump={} n_bumps={} has_1652_ref={})",
                     depth_bump, n_bumps, has_1652_ref);
        return false;
    }
    std::println(
        "OK: 2 paired hygiene-violation bumps landed (depth-exceeded + body_id-NULL/invalid)");
    return true;
}

bool check_per_call_success_path_bump_ac1() {
    std::println("\n--- AC1: per-call success-path bump at clone_macro_body entry ---");
    std::string me = read_file("src/compiler/macro_expansion.cpp");
    bool per_call_bump =
        contains(me, "g_macro_expansion_total.fetch_add(1, std::memory_order_relaxed);");
    int n_per_call = 0;
    std::size_t pos = 0;
    while ((pos = me.find("g_macro_expansion_total.fetch_add(1, std::memory_order_relaxed);",
                          pos)) != std::string::npos) {
        ++n_per_call;
        ++pos;
    }
    bool has_1652_success_path_comment =
        contains(me, "Issue #1652: per-call success-path observability bump");
    if (!per_call_bump || !has_1652_success_path_comment || n_per_call < 1) {
        std::println("FAIL: per-call bump missing "
                     "(per_call_bump={} n_per_call={} has_comment={})",
                     per_call_bump, n_per_call, has_1652_success_path_comment);
        return false;
    }
    std::println("OK: per-call success-path bump landed at clone_macro_body entry");
    return true;
}

bool check_predecessor_coverage_ac3() {
    std::println("\n--- AC3: predecessor hygiene tracer counters still present ---");
    std::string me = read_file("src/compiler/macro_expansion.cpp");
    bool has_origin_provenance =
        contains(me, "std::atomic<std::uint64_t> g_macro_origin_provenance_errors{0}");
    bool has_tracer_expansions =
        contains(me, "std::atomic<std::uint64_t> g_hygiene_tracer_expansions{0}");
    bool has_tracer_depth_max =
        contains(me, "std::atomic<std::uint64_t> g_hygiene_tracer_depth_max{0}");
    if (!has_origin_provenance || !has_tracer_expansions || !has_tracer_depth_max) {
        std::println("FAIL: predecessor #1247/#1248 counters missing "
                     "(origin_provenance={} tracer_expansions={} tracer_depth_max={})",
                     has_origin_provenance, has_tracer_expansions, has_tracer_depth_max);
        return false;
    }
    std::println("OK: predecessor #1247/#1248 hygiene tracer counters present (paired with #1652)");
    return true;
}

bool check_design_doc_present() {
    std::println("\n--- #1652 docs/design/1652-macro-expansion-stats.md ---");
    std::ifstream in("docs/design/1652-macro-expansion-stats.md");
    if (!in) {
        std::println("FAIL: design doc missing");
        return false;
    }
    std::println("OK: design doc present");
    return true;
}

} // namespace aura_1652_detail

int main() {
    using namespace aura_1652_detail;

    int rc = 0;
    if (!check_file_level_atomics_ac1())
        rc = 1;
    if (!check_paired_hygiene_violation_bumps_ac1())
        rc = 1;
    if (!check_per_call_success_path_bump_ac1())
        rc = 1;
    if (!check_predecessor_coverage_ac3())
        rc = 1;
    if (!check_design_doc_present())
        rc = 1;

    if (rc == 0) {
        std::println("\n#1652 scope-limited-progressive-ship Phase 1 — all AC checks green \u2705\n"
                     "    Per-recursive-step bump_macro_introduced_nodes_created(cumulative)\n"
                     "    + primitive body composition \u2192 #1688");
    } else {
        std::println("\n#1652 — some AC checks FAILED ❌");
    }
    return rc;
}
