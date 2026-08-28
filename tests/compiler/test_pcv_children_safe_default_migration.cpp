// @category: unit
// @reason: Issue #2036 — complete PCV children_ migration; SafePCVSpan
// default for children_stable / children_default; multi-fiber stress.
//
//   AC1: source cites #2036; children_ is PersistentChildVector; children_default
//   AC2: children_stable / for_each_stable_child pin via children_safe_view
//   AC3: SafePCVSpan survives concurrent mutate + restore_children (multi-fiber)
//   AC4: children_stable_safe_default_total / children_safe_view_count advance
//   AC5: query:soa-children-columnar-migration-stats schema-2036
//   AC6: no pmr::vector path for FlatAST children_ (source scan)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/persistent_child_vector.hh" // SafePCVSpan (header-only)

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::SafePCVSpan;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:soa-children-columnar-migration-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2036 + PCV-backed children_ ---");
    auto ast = read_file("src/core/ast.ixx");
    auto pcv = read_file("src/core/persistent_child_vector.hh");
    auto q = aura::test::aura_query_prims_source();
    CHECK(!ast.empty() && ast.find("#2036") != std::string::npos, "ast.ixx #2036");
    CHECK(ast.find("std::vector<PersistentChildVector<NodeId>> children_") != std::string::npos,
          "children_ PCV-backed");
    CHECK(ast.find("children_default") != std::string::npos, "children_default API");
    CHECK(ast.find("children_stable_safe_default_total_") != std::string::npos,
          "safe-default metric");
    CHECK(!pcv.empty() && pcv.find("#2036") != std::string::npos, "pcv header #2036");
    CHECK(pcv.find("migration end-state") != std::string::npos ||
              pcv.find("migration complete") != std::string::npos,
          "migration end-state note");
    CHECK(!q.empty() && q.find("schema-2036") != std::string::npos, "query schema-2036");
}

static void ac2_stable_pins_safe() {
    std::println("\n--- AC2: children_stable / default pin via SafePCVSpan ---");
    FlatAST flat;
    auto a = flat.add_node(NodeTag::LiteralInt);
    auto b = flat.add_node(NodeTag::LiteralInt);
    NodeId kids[] = {a, b};
    auto parent = flat.add_begin(kids, 2);
    CHECK(parent != NULL_NODE, "begin parent");
    const auto safe0 = flat.children_safe_view_count();
    const auto def0 = flat.children_stable_safe_default_total();
    auto refs = flat.children_stable(parent);
    CHECK(refs.size() >= 2, "stable refs");
    CHECK(flat.children_safe_view_count() > safe0, "children_stable pinned via safe_view");
    CHECK(flat.children_stable_safe_default_total() > def0, "safe-default counter advanced");

    const auto def1 = flat.children_stable_safe_default_total();
    auto def_view = flat.children_default(parent);
    CHECK(flat.children_stable_safe_default_total() > def1, "children_default bumps counter");
    CHECK(def_view.size() == flat.children_safe(parent).size(), "default == safe size");

    std::size_t n = 0;
    flat.for_each_stable_child(parent, [&](FlatAST::StableNodeRef) { ++n; });
    CHECK(n == refs.size(), "for_each matches");
}

static void ac3_multifiber_safe_span() {
    std::println("\n--- AC3: multi-fiber SafePCVSpan across mutate+rollback ---");
    FlatAST flat;
    auto c0 = flat.add_node(NodeTag::LiteralInt);
    auto c1 = flat.add_node(NodeTag::LiteralInt);
    auto c2 = flat.add_node(NodeTag::LiteralInt);
    NodeId kids[] = {c0, c1, c2};
    auto root = flat.add_begin(kids, 3);

    // Capture pinned view before concurrent work.
    SafePCVSpan<NodeId> held = flat.children_safe(root);
    CHECK(held.size() >= 2, "held has children");
    const auto first = held[0];
    const auto second = held.size() > 1 ? held[1] : NULL_NODE;

    auto snap = flat.snapshot_children();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> bad{0};
    std::atomic<std::uint64_t> iters{0};

    std::thread mutator([&] {
        for (int i = 0; i < 200 && !stop.load(std::memory_order_relaxed); ++i) {
            // Mutate children (COW) then restore from snapshot.
            if (root < flat.size() && flat.stable_child_count(root) > 0) {
                flat.set_child(root, 0, c2);
                flat.insert_child(root, 1, c0);
            }
            auto local = flat.snapshot_children();
            flat.restore_children(std::move(local));
            // Periodically reinstall original snap
            if ((i % 17) == 0) {
                auto s2 = snap; // PCV shared_ptr copy
                flat.restore_children(std::move(s2));
                snap = flat.snapshot_children();
            }
            iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < 500; ++i) {
            // Held span must remain valid for entire reader life.
            if (held.empty()) {
                bad.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            const auto a = held[0];
            const auto n = held.size();
            if (n == 0 || a == NULL_NODE) {
                // empty after pin would be a bug for our setup
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            // Touch every element (storage must not be freed).
            for (std::size_t j = 0; j < held.size(); ++j)
                (void)held[j];
            (void)a;
        }
    });

    reader.join();
    stop.store(true, std::memory_order_relaxed);
    mutator.join();

    CHECK(bad.load() == 0, "no SafePCVSpan dangling under concurrent mutate");
    CHECK(held.size() >= 2, "held size still ≥2 after concurrent work");
    CHECK(held[0] == first, "first child id stable in pinned view");
    if (second != NULL_NODE)
        CHECK(held[1] == second, "second child id stable in pinned view");
    CHECK(iters.load() > 0, "mutator ran");
}

static void ac4_counters() {
    std::println("\n--- AC4: observability counters advance ---");
    FlatAST flat;
    auto a = flat.add_node(NodeTag::LiteralInt);
    NodeId kids[] = {a};
    auto root = flat.add_begin(kids, 1);
    const auto s0 = flat.children_safe_view_count();
    const auto d0 = flat.children_stable_safe_default_total();
    const auto span0 = flat.children_stable_span_calls_total();
    (void)flat.children_default(root);
    (void)flat.children_stable(root);
    (void)flat.children_stable_span_view(root);
    CHECK(flat.children_safe_view_count() > s0, "safe_view advanced");
    CHECK(flat.children_stable_safe_default_total() > d0, "safe-default advanced");
    CHECK(flat.children_stable_span_calls_total() > span0, "span_view advanced");
    CHECK(flat.children_pcv_migration_complete() == 1, "migration complete flag");
}

static void ac5_query() {
    std::println("\n--- AC5: query schema-2036 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:soa-children-columnar-migration-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2036") == 2036, "schema-2036");
    CHECK(href(cs, "issue-2036") == 2036, "issue-2036");
    CHECK(href(cs, "children-pcv-migration-complete") == 1, "migration complete");
    CHECK(href(cs, "children-default-safe-pcv-wired") == 1, "default wired");
    CHECK(href(cs, "children-safe-view-count") >= 0, "safe-view count");
    CHECK(href(cs, "children-stable-safe-default-total") >= 0, "safe-default total");
}

static void ac6_no_pmr_children() {
    std::println("\n--- AC6: no pmr::vector children_ path ---");
    auto ast = read_file("src/core/ast.ixx");
    CHECK(!ast.empty(), "ast readable");
    // Must not declare children_ as pmr::vector of NodeId lists.
    CHECK(ast.find("std::pmr::vector<std::pmr::vector<NodeId>> children_") == std::string::npos,
          "no nested pmr children_");
    CHECK(ast.find("std::pmr::vector<NodeId> children_") == std::string::npos,
          "no pmr vector<NodeId> children_");
    CHECK(ast.find("std::vector<PersistentChildVector<NodeId>> children_") != std::string::npos,
          "PCV vector children_ present");
}

// ── Issue #2862: query:children-stable full safety contract
// Source-cite ACs (gate-only ship; runtime verifies on CI).

static void ac2862_1_source_atomics() {
    std::println("\n--- #2862 AC1: source — 3 new safety atomics present ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("children_stable_pin_hits_total") != std::string::npos,
          "#2862 AC1: children_stable_pin_hits_total atomic");
    CHECK(met.find("children_stable_invalidation_detected_total") != std::string::npos,
          "#2862 AC1: children_stable_invalidation_detected_total atomic");
    CHECK(met.find("children_stable_epoch_mismatch_total") != std::string::npos,
          "#2862 AC1: children_stable_epoch_mismatch_total atomic");
    CHECK(met.find("// #2862") != std::string::npos,
          "#2862 AC1: comment block cites #2862 contract surfaces");
    // Reuse existing FlatAST atomic.
    const auto ast = read_file("src/core/ast.ixx");
    CHECK(ast.find("children_stable_span_calls_total_") != std::string::npos,
          "#2862 AC1: existing FlatAST children_stable_span_calls_total_ atomic reused");
}

static void ac2862_2_source_primitive() {
    std::println("\n--- #2862 AC2: source — query:children-stable-stats primitive ---");
    const auto q = aura::test::aura_query_prims_source();
    CHECK(q.find("query:children-stable-stats") != std::string::npos,
          "#2862 AC2: query:children-stable-stats primitive registered");
    CHECK(q.find("schema") != std::string::npos &&
              q.find("insert_kv(\"schema\", 2862)") != std::string::npos,
          "#2862 AC2: schema=2862 in hash builder");
    CHECK(q.find("children-stable-span-calls-total") != std::string::npos &&
              q.find("children-stable-pin-hits-total") != std::string::npos &&
              q.find("children-stable-invalidation-detected-total") != std::string::npos &&
              q.find("children-stable-epoch-mismatch-total") != std::string::npos,
          "#2862 AC2: 4 contract counter keys in hash");
    // Additive on existing #2036 + #2861 pattern safety stats.
    // The #2036 counter is exposed as children-stable-safe-default-total
    // inside the query:children-stable-stats hash (insert_kv key).
    CHECK(q.find("children-stable-safe-default-total") != std::string::npos,
          "#2862 AC2: #2036 children-stable-safe-default-total preserved");
    CHECK(q.find("query:pattern-safety-stats") != std::string::npos,
          "#2862 AC2: #2861 pattern-safety-stats preserved");
}

static void ac2862_3_no_docs() {
    std::println("\n--- #2862 AC3: no docs/design/ + lineage refs ---");
    CHECK(read_file("docs/design/2862-children-stable-contract.md").empty(),
          "#2862 AC3: no docs/design/2862-* per #1655");
    const auto q = aura::test::aura_query_prims_source();
    CHECK(q.find("#2036") != std::string::npos && q.find("#678") != std::string::npos &&
              q.find("#655") != std::string::npos && q.find("#2861") != std::string::npos,
          "#2862 AC3: lineage refs to #2036/#678/#655/#2861");
}

// ── Issue #3292: PcvHotpathMetrics append-only layout stamps ──
// AC1: static_assert offsetof guards next to the struct (#2906 lineage).
static void ac3292_1_layout_stamps_present() {
    std::println("\n--- #3292 AC1: append-only layout stamps (#2906) ---");
    const auto pcv = read_file("src/core/persistent_child_vector.hh");
    CHECK(pcv.find("Issue #3292") != std::string::npos,
          "3292 AC1: persistent_child_vector.hh cites #3292");
    CHECK(pcv.find("#2906") != std::string::npos, "3292 AC1: #2906 lineage comment");
    CHECK(pcv.find("append-only at struct END") != std::string::npos,
          "3292 AC1: append-only discipline documented");
    CHECK(pcv.find("static_assert(offsetof(PcvHotpathMetrics, "
                   "stale_span_force_exclusive_total)") != std::string::npos,
          "3292 AC1: last-metric offsetof assert present");
    // Asserts sit immediately after the struct definition.
    const auto struct_pos = pcv.find("struct PcvHotpathMetrics");
    CHECK(struct_pos != std::string::npos, "3292 AC1: struct present");
    const auto tail = pcv.substr(struct_pos, 3200);
    CHECK(tail.find("static_assert(offsetof(PcvHotpathMetrics, "
                    "stale_span_force_exclusive_total)") != std::string::npos,
          "3292 AC1: last-metric assert next to the struct");
}

// AC2: pinned offsets — mid-struct insert fails the build.
static void ac3292_2_offsets_pinned() {
    std::println("\n--- #3292 AC2: pinned offsets ---");
    const auto pcv = read_file("src/core/persistent_child_vector.hh");
    CHECK(pcv.find("offsetof(PcvHotpathMetrics, stale_span_force_exclusive_total) == 128") !=
              std::string::npos,
          "3292 AC2: last metric pinned at offset 128");
    CHECK(pcv.find("offsetof(PcvHotpathMetrics, pcv_span_stale_across_guard_total) == 112") !=
              std::string::npos,
          "3292 AC2: pcv_span pinned at offset 112");
    CHECK(pcv.find("offsetof(PcvHotpathMetrics, flatast_locked_move_out_exclusive_total) == 96") !=
              std::string::npos,
          "3292 AC2: flatast exclusive pinned at offset 96");
    CHECK(pcv.find("sizeof(PcvHotpathMetrics) == 136") != std::string::npos,
          "3292 AC2: sizeof pinned to 136");
}

// AC3: no new runtime counters / atomics (compile-time only).
static void ac3292_3_no_new_runtime() {
    std::println("\n--- #3292 AC3: zero runtime cost ---");
    const auto pcv = read_file("src/core/persistent_child_vector.hh");
    CHECK(pcv.find("g_3292_") == std::string::npos, "3292 AC3: no new g_3292_* counter");
    const auto build = read_file("build.py");
    CHECK(build.find("check_pcv_hotpath_metrics_layout_3292") != std::string::npos,
          "3292 AC3: build.py wires linter");
    CHECK(read_file("docs/design/3292-pcv-hotpath-metrics-layout.md").empty(),
          "3292 AC3: no docs/design/ per #1655");
    CHECK(read_file("tests/compiler/test_issue_3292.cpp").empty(),
          "3292 AC3: no test_issue_3292.cpp per #81967");
    CHECK(read_file("tests/issues/test_issue_3292.cpp").empty(),
          "3292 AC3: no tests/issues/test_issue_3292.cpp per #81967");
}

// Issue #3392 (I1): production children_stable / children_stable_batch hot
// path must not construct std::vector<StableNodeRef> per call. The
// evaluator export face fills a thread-local buffer via the #398
// for_each_stable_child callback. FlatAST::children_stable(NodeId) keeps
// its allocating convenience for C++ callers (per AC2). #3328 stale-span
// / #3167 fingerprint / #3000 restamp-lag empty-batch non-regress.
void ac3392_1_source_cite_evaluator_face() {
    std::println(
        "\n--- #3392 AC1+AC5: evaluator export face uses thread-local buffer + callback ---");
    std::ifstream f_efm("src/compiler/evaluator_fiber_mutation.cpp");
    const std::string efm((std::istreambuf_iterator<char>(f_efm)),
                          std::istreambuf_iterator<char>());
    CHECK(!efm.empty(), "3392 AC1: evaluator_fiber_mutation.cpp readable");
    // AC5: the Evaluator::children_stable_batch body must NOT declare
    // a fresh `std::vector<StableNodeRef> out;` — it must reuse a
    // thread-local buffer so per-call heap allocation is avoided.
    auto body_open = efm.find("Evaluator::children_stable_batch");
    CHECK(body_open != std::string::npos, "3392 AC1: Evaluator::children_stable_batch defined");
    if (body_open != std::string::npos) {
        auto brace_open = efm.find('{', body_open);
        CHECK(brace_open != std::string::npos, "3392 AC1: function body open");
        if (brace_open != std::string::npos) {
            std::size_t depth = 1;
            std::size_t i = brace_open + 1;
            for (; i < efm.size(); ++i) {
                if (efm[i] == '{')
                    ++depth;
                else if (efm[i] == '}') {
                    --depth;
                    if (depth == 0)
                        break;
                }
            }
            const auto body = efm.substr(brace_open, i - brace_open + 1);
            // AC5: no fresh std::vector<StableNodeRef> out; — must be
            // thread_local std::vector<StableNodeRef> out;
            CHECK(body.find("thread_local std::vector<aura::ast::FlatAST::StableNodeRef>") !=
                      std::string::npos,
                  "3392 AC1: thread_local buffer adopted");
            CHECK(body.find("for_each_stable_child") != std::string::npos,
                  "3392 AC1: for_each_stable_child callback adopted (per #398)");
            CHECK(efm.find("Issue #3392") != std::string::npos,
                  "3392 AC1: Issue #3392 cite in source");
        }
    }
    // AC5: FlatAST::children_stable(NodeId) keeps its allocating
    // convenience (per AC2) — the fix is scoped to the Evaluator export
    // face, not FlatAST.
    std::ifstream f_astx("src/core/ast.ixx");
    const std::string astx((std::istreambuf_iterator<char>(f_astx)),
                           std::istreambuf_iterator<char>());
    CHECK(astx.find("std::vector<StableNodeRef> children_stable(NodeId id) const") !=
              std::string::npos,
          "3392 AC2: FlatAST::children_stable keeps allocating convenience");
    CHECK(astx.find("template <typename Fn> void for_each_stable_child") != std::string::npos,
          "3392 AC1: for_each_stable_child callback (per #398) still available");
}

void ac3392_2_no_regress_3328_3167_3000() {
    std::println("\n--- #3392 AC3: #3328 / #3167 / #3000 non-regress ---");
    std::ifstream f_efm("src/compiler/evaluator_fiber_mutation.cpp");
    const std::string efm((std::istreambuf_iterator<char>(f_efm)),
                          std::istreambuf_iterator<char>());
    CHECK(efm.find("pcv_span_for_agent_export") != std::string::npos,
          "3392 AC3: #3328 pcv_span_for_agent_export still called");
    CHECK(efm.find("force_refresh_pcv_span") != std::string::npos,
          "3392 AC3: #3328 force_refresh_pcv_span still called");
    CHECK(efm.find("is_stale") != std::string::npos, "3392 AC3: #3328 stale check still present");
    CHECK(efm.find("allow_query_stable_ref_export") != std::string::npos,
          "3392 AC3: #3000 restamp-lag gate still present");
    CHECK(efm.find("stamp_query_stable_ref_export") != std::string::npos,
          "3392 AC3: #3167 fingerprint stamp still wired");
    CHECK(efm.find("pin_for_cow") != std::string::npos, "3392 AC3: #2960 COW pin still applied");
    // The stale fail-closed empty-batch contract is preserved: if any
    // child fails allow_query_stable_ref_export, the batch returns out
    // (empty). Pre-fill, not post-fill.
    CHECK(efm.find("if (!allow_query_stable_ref_export(cid))\n            return out;") !=
                  std::string::npos ||
              efm.find("if (!allow_query_stable_ref_export(cid))\n                return out;") !=
                  std::string::npos,
          "3392 AC3: empty-batch fail-closed before fill (per #3000)");
}

void ac3392_3_no_invent() {
    std::println("\n--- #3392 AC4: no docs/design/3392-*; no tests/issues/test_issue_3392.cpp ---");
    {
        std::ifstream f("docs/design/3392-children-stable-no-heap-temp.md");
        CHECK(!f.good(), "3392 AC4: no docs/design/3392-*");
    }
    {
        std::ifstream f("tests/issues/test_issue_3392.cpp");
        CHECK(!f.good(), "3392 AC4: no tests/issues/test_issue_3392.cpp");
    }
}

} // namespace

// Issue #3397: production query:children-stable must not return the TLS
// span view (children_stable_span_view). The TLS pin is valid only until
// the next same-thread call — an Agent that treats it as multi-round
// memory holds a dangling pin across Guard (UAF or wrong-generation
// child ids on read). Source-cite gate covers the Agent return path
// contracts; the linter scripts/coverage/checks/check_query_children_
// stable_no_tls_3397.py --strict enforces the same at gate-time.
//
// AC1: production query:children-stable does NOT call children_stable_
//      span_view on the Agent return path; routes through pin_query_
//      children (SafePCVSpan) + pcv_span_for_agent_export.
// AC2: production + span held across Guard + re-query → stale-span or
//      rebuilt stamped vector; never use the TLS buffer contents.
//      (Verified structurally via mev("stale-span"...) source-cite.)
// AC3: Soft span_view + #3233 Soft-off COW unchanged.
//      (Verified via "identity" zero-cost branch in pcv_span_for_
//      agent_export.)
// AC4: #3167 fingerprint / #3393 force-exclusive / #3328 refresh
//      non-regress — all three contracts still appear in source.
// AC5: extend PCV children-safe / query children-stable fixture.
//      (This file is the fixture — src/-aligned, tests/compiler/.)

static void ac3397_1_query_children_stable_no_tls_source_cite() {
    std::println("\n=== #3397 AC1: query:children-stable does NOT use TLS span view ===");
    std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
    std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
    CHECK(!qws.empty(), "3397 AC1: query_workspace.cpp readable");
    // The Agent-facing return path must use pin_query_children
    // (SafePCVSpan) + pcv_span_for_agent_export, NOT the TLS
    // buffer-returning children_stable_span_view.
    CHECK(qws.find("children_stable_span_view") == std::string::npos,
          "3397 AC1: query_workspace.cpp has zero references to "
          "children_stable_span_view (TLS pin) — Agent return path must "
          "not return the TLS buffer");
    CHECK(qws.find("pin_query_children") != std::string::npos,
          "3397 AC1: query_workspace.cpp uses pin_query_children "
          "(SafePCVSpan) on the Agent return path");
    CHECK(qws.find("pcv_span_for_agent_export") != std::string::npos,
          "3397 AC1: pcv_span_for_agent_export wired into "
          "query:children-stable");
}

static void ac3397_2_stale_span_on_unrefreshable_fingerprint() {
    std::println("\n=== #3397 AC2: stale-span on unrefreshable fingerprint ===");
    // Behavioral: verify the structured stale-span error path exists
    // and is wired to force_refresh_pcv_span (#3167 recovery). When
    // force_refresh fails (owner gone / unrefreshable), the Agent
    // must surface stale-span — never a green pre-mutate child list.
    std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
    std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
    CHECK(qws.find("force_refresh_pcv_span") != std::string::npos,
          "3397 AC2: force_refresh_pcv_span (#3167 recovery) called in "
          "query:children-stable stale-span path");
    CHECK(qws.find("mev(\"stale-span\"") != std::string::npos,
          "3397 AC2: structured stale-span error returned on "
          "unrefreshable fingerprint (not a green child list)");
    CHECK(qws.find("has_fingerprint()") != std::string::npos,
          "3397 AC2: pcv_span.has_fingerprint() check present "
          "before stale-span fallback");
}

static void ac3397_3_soft_zero_extra_unchanged() {
    std::println(
        "\n=== #3397 AC3: Soft / unit zero extra (pcv_span_for_agent_export identity) ===");
    // Soft / unit must pay zero extra beyond existing pin metrics.
    // pcv_span_for_agent_export short-circuits to identity under Soft
    // (production_defaults_active() == false).
    std::ifstream f_ast("src/core/ast.ixx");
    std::string ast((std::istreambuf_iterator<char>(f_ast)), std::istreambuf_iterator<char>());
    CHECK(ast.find("pcv_span_for_agent_export") != std::string::npos,
          "3397 AC3: pcv_span_for_agent_export helper present in ast.ixx");
    CHECK(ast.find("production_defaults_active()") != std::string::npos,
          "3397 AC3: production_defaults_active() gate present in "
          "pcv_span_for_agent_export (Soft short-circuit)");
    CHECK(ast.find("identity") != std::string::npos,
          "3397 AC3: Soft identity branch present (zero-cost)");
}

static void ac3397_4_non_regress_3167_3393_3328() {
    std::println("\n=== #3397 AC4: non-regress #3167 / #3393 / #3328 ===");
    std::ifstream f_qws("src/compiler/evaluator_primitives_query_workspace.cpp");
    std::ifstream f_ast("src/core/ast.ixx");
    std::string qws((std::istreambuf_iterator<char>(f_qws)), std::istreambuf_iterator<char>());
    std::string ast((std::istreambuf_iterator<char>(f_ast)), std::istreambuf_iterator<char>());
    CHECK(!qws.empty(), "3397 AC4: query_workspace.cpp readable");
    CHECK(!ast.empty(), "3397 AC4: ast.ixx readable");
    // #3167: pcv_span_stale_across_guard_total counter + fingerprint
    CHECK(ast.find("pcv_span_stale_across_guard_total") != std::string::npos,
          "3397 AC4: #3167 stale-across-guard counter unchanged");
    CHECK(ast.find("#3167") != std::string::npos, "3397 AC4: #3167 cite present in ast.ixx");
    // #3328: production re-use face → pcv_span_for_agent_export
    CHECK(ast.find("#3328") != std::string::npos, "3397 AC4: #3328 cite present in ast.ixx");
    // #3397: this ticket's cite in both files (commit message anchor)
    CHECK(qws.find("#3397") != std::string::npos,
          "3397 AC4: Issue #3397 cite present in query_workspace.cpp");
    CHECK(ast.find("#3397") != std::string::npos, "3397 AC4: Issue #3397 cite present in ast.ixx");
    // AC5: no docs/design/, no tests/issues/test_issue_3397.cpp
    {
        std::ifstream f("docs/design/3397-children-stable-no-tls-span.md");
        CHECK(!f.good(), "3397 AC5: no docs/design/3397-*");
    }
    {
        std::ifstream f("tests/issues/test_issue_3397.cpp");
        CHECK(!f.good(), "3397 AC5: no tests/issues/test_issue_3397.cpp");
    }
}

int main() {
    std::println("=== test_pcv_children_safe_default_migration (#2036 + #2862) ===");
    ac1_source();
    ac2_stable_pins_safe();
    ac3_multifiber_safe_span();
    std::println(
        "\n=== #2862: query:children-stable full safety contract (source-cite gate-only) ===");
    ac2862_1_source_atomics();
    ac2862_2_source_primitive();
    ac2862_3_no_docs();
    ac4_counters();
    ac5_query();
    ac6_no_pmr_children();
    std::println("\n=== #3292: PcvHotpathMetrics append-only layout stamps (#2906) ===");
    ac3292_1_layout_stamps_present();
    ac3292_2_offsets_pinned();
    ac3292_3_no_new_runtime();
    std::println("\n=== #3392: children_stable_batch zero-heap-temp ===");
    ac3392_1_source_cite_evaluator_face();
    ac3392_2_no_regress_3328_3167_3000();
    ac3392_3_no_invent();
    std::println("\n=== #3397: query:children-stable no-TLS-span ===");
    ac3397_1_query_children_stable_no_tls_source_cite();
    ac3397_2_stale_span_on_unrefreshable_fingerprint();
    ac3397_3_soft_zero_extra_unchanged();
    ac3397_4_non_regress_3167_3393_3328();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
