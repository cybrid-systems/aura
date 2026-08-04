// @category: unit
// @reason: Issue #2606 — harden PerEval / multi-AotState region
//          independence on reemit + invalidate.
//
//   AC1: Dual eval; dirty candidates A+B under reemit owner A → only A
//        reemitted; B slot fn_ptr / generation untouched by reemit body.
//   AC2: Invalidate_for_eval(A) does not clear B-owned gen-behind slots.
//   AC3: Single-eval / nullptr reemit owner path identical (no filter;
//        cross-eval skip stays 0).
//   AC4: Cross-eval skip counter visible on query; zero when only one eval.
//   AC5: Source-cite + dual-eval soak test + cmake/linter wiring.

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <unordered_set>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

extern "C" void aura_hot_update_set_reemit_boundary_policy(int policy);
extern "C" void aura_hot_update_reset_reemit_boundary_handshake_for_test(void);
extern "C" void aura_hot_update_reset_deopt_storm_state_for_test(void);

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

struct ReemitFixture {
    struct Candidate {
        std::string name;
        std::uint64_t region;
        bool from_closure_capture;
    };
    std::vector<Candidate> candidates;
    std::size_t cursor = 0;
};

static bool reemit_candidate_iter(void* userdata, const char** out_name, std::uint64_t* out_region,
                                  bool* out_from_closure_capture) {
    auto* f = static_cast<ReemitFixture*>(userdata);
    if (!f || f->candidates.empty())
        return false;
    if (f->cursor >= f->candidates.size()) {
        f->cursor = 0;
        return false;
    }
    const auto& c = f->candidates[f->cursor++];
    *out_name = c.name.c_str();
    *out_region = c.region;
    *out_from_closure_capture = c.from_closure_capture;
    return true;
}

struct EmitFixture {
    std::unordered_set<std::string> emitted;
    std::atomic<std::uint32_t> calls{0};
    std::atomic<std::uint32_t> ok{0};
};

static bool emit_fn(const char* name, std::uint64_t /*region*/, void* userdata) {
    auto* f = static_cast<EmitFixture*>(userdata);
    f->calls.fetch_add(1, std::memory_order_relaxed);
    if (!name)
        return false;
    f->emitted.insert(name);
    f->ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// SoftEnter so reemit body runs outside MutationBoundary (test-only).
static void enable_soft_reemit() {
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_reemit_boundary_policy(0); // SoftEnter
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- #2606 AC5: source-cite dual-eval reemit filter ---");
    auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    auto hdr = read_file("src/compiler/aura_jit_bridge.h");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    auto cmake = read_file("CMakeLists.txt");

    CHECK(bridge.find("#2606") != std::string::npos, "AC5: bridge cites #2606");
    CHECK(bridge.find("filter_by_eval") != std::string::npos ||
              bridge.find("reemit_cross_eval_candidate_skipped_total") != std::string::npos,
          "AC5: ownership filter in reemit loop");
    CHECK(bridge.find("process-global") != std::string::npos ||
              bridge.find("process-global") != std::string::npos ||
              bridge.find("joint bridge/AOT table epoch remains process-global") !=
                  std::string::npos ||
              hdr.find("process-global") != std::string::npos,
          "AC5: documents process-global epoch invariant");
    CHECK(hdr.find("aura_aot_set_reemit_owner_eval") != std::string::npos,
          "AC5: reemit owner TLS API declared");
    CHECK(obs.find("reemit_cross_eval_candidate_skipped_total") != std::string::npos,
          "AC5: skip metric field");
    CHECK(q.find("schema-2606") != std::string::npos && q.find("issue-2606") != std::string::npos,
          "AC5: schema-2606 / issue-2606 query lineage");
    CHECK(q.find("reemit-cross-eval-filter-wired") != std::string::npos,
          "AC5: filter-wired sentinel");
    CHECK(dirty.find("ReemitEvalOwnerGuard") != std::string::npos ||
              dirty.find("aura_aot_set_reemit_owner_eval") != std::string::npos,
          "AC5: cascade host stamps reemit owner");
    CHECK(cmake.find("test_pereval_reemit_region_independence") != std::string::npos,
          "AC5: cmake registers test");
}

// ── AC1: dual-eval reemit only emits owned candidates ──
static void ac1_dual_eval_reemit_owner_filter() {
    std::println("\n--- #2606 AC1: dual-eval reemit candidates only from owner ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    enable_soft_reemit();

    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA601));
    void* eval_b = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB602));
    aura_set_aot_region_mask_for_eval(eval_a, 1);
    aura_set_aot_region_mask_for_eval(eval_b, 2);
    CHECK(aura_aot_state_map_size() >= 2, "AC1: dual-eval AotState map size >= 2");

    // Assign stable ids + register owned slots.
    int p = 0;
    const auto sid_a = aura_get_or_preserve_stable_func_id("fn_owner_a_2606", &p);
    const auto sid_b = aura_get_or_preserve_stable_func_id("fn_owner_b_2606", &p);
    CHECK(sid_a != 0 && sid_b != 0 && sid_a != sid_b, "AC1: distinct stable ids");

    constexpr std::uintptr_t kPtrA = 0xA1001000u;
    constexpr std::uintptr_t kPtrB = 0xB2002000u;
    aura_aot_set_register_owner_eval(eval_a);
    aura_register_fn_tracked(static_cast<std::int64_t>(sid_a), static_cast<std::int64_t>(kPtrA));
    aura_aot_set_register_owner_eval(eval_b);
    aura_register_fn_tracked(static_cast<std::int64_t>(sid_b), static_cast<std::int64_t>(kPtrB));
    aura_aot_set_register_owner_eval(nullptr);

    CHECK(aura_aot_probe_fn_ptr_raw(static_cast<std::int64_t>(sid_a)) == kPtrA,
          "AC1 setup: slot A live");
    CHECK(aura_aot_probe_fn_ptr_raw(static_cast<std::int64_t>(sid_b)) == kPtrB,
          "AC1 setup: slot B live");

    ReemitFixture rf;
    rf.candidates = {{"fn_owner_a_2606", 0, false}, {"fn_owner_b_2606", 0, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto skip0 =
        metrics.reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed);

    // Reemit under eval A: host may push A+B; filter keeps only A.
    aura_aot_set_reemit_owner_eval(eval_a);
    aura_aot_set_register_owner_eval(eval_a);
    rf.cursor = 0;
    const auto n = aura_reemit_aot_for_dirty(1);
    aura_aot_set_reemit_owner_eval(nullptr);
    aura_aot_set_register_owner_eval(nullptr);

    CHECK(n >= 1, "AC1: reemit under A returned >=1");
    CHECK(ef.emitted.count("fn_owner_a_2606") == 1, "AC1: A candidate reemitted");
    CHECK(ef.emitted.count("fn_owner_b_2606") == 0, "AC1: B candidate NOT reemitted");
    CHECK(metrics.reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed) >=
              skip0 + 1,
          "AC1: cross-eval skip counter bumped for B");
    // B slot still live at original ptr (reemit body never touched it).
    CHECK(aura_aot_probe_fn_ptr_raw(static_cast<std::int64_t>(sid_b)) == kPtrB,
          "AC1: B slot untouched by reemit under A");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_cleanup_aot_state(eval_a);
    aura_cleanup_aot_state(eval_b);
    aura_set_aot_metrics(nullptr);
}

// ── AC2: invalidate_for_eval(A) leaves B ──
static void ac2_invalidate_for_eval_isolation() {
    std::println("\n--- #2606 AC2: invalidate(A) does not clear B slots ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA611));
    void* eval_b = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB612));
    aura_set_aot_region_mask_for_eval(eval_a, 1);
    aura_set_aot_region_mask_for_eval(eval_b, 2);

    constexpr std::int64_t kFidA = 620;
    constexpr std::int64_t kFidB = 621;
    constexpr std::uintptr_t kPtrA = 0x11110010u;
    constexpr std::uintptr_t kPtrB = 0x22220020u;

    aura_aot_set_register_owner_eval(eval_a);
    aura_register_fn_tracked(kFidA, static_cast<std::int64_t>(kPtrA));
    aura_aot_set_register_owner_eval(eval_b);
    aura_register_fn_tracked(kFidB, static_cast<std::int64_t>(kPtrB));
    aura_aot_set_register_owner_eval(nullptr);
    aura_aot_bump_func_table_epoch(); // both gen-behind

    const auto n_a = aura_aot_invalidate_all_stale_slots_for_eval(eval_a);
    CHECK(n_a >= 1, "AC2: invalidate(A) cleared >=1");
    CHECK(aura_aot_probe_fn_ptr_raw(kFidA) == 0, "AC2: A cleared");
    CHECK(aura_aot_probe_fn_ptr_raw(kFidB) == kPtrB, "AC2: B remains");

    aura_cleanup_aot_state(eval_a);
    aura_cleanup_aot_state(eval_b);
    aura_set_aot_metrics(nullptr);
}

// ── AC3: nullptr / single-eval path no filter ──
static void ac3_nullptr_path_no_filter() {
    std::println("\n--- #2606 AC3: nullptr reemit owner → no cross-eval filter ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    enable_soft_reemit();

    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA621));
    void* eval_b = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB622));
    int p = 0;
    const auto sid_a = aura_get_or_preserve_stable_func_id("fn_null_a_2606", &p);
    const auto sid_b = aura_get_or_preserve_stable_func_id("fn_null_b_2606", &p);
    aura_aot_set_register_owner_eval(eval_a);
    aura_register_fn_tracked(static_cast<std::int64_t>(sid_a), 0xA300u);
    aura_aot_set_register_owner_eval(eval_b);
    aura_register_fn_tracked(static_cast<std::int64_t>(sid_b), 0xB300u);
    aura_aot_set_register_owner_eval(nullptr);

    ReemitFixture rf;
    rf.candidates = {{"fn_null_a_2606", 0, false}, {"fn_null_b_2606", 0, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto skip0 =
        metrics.reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed);

    // Explicit nullptr filter (process-default) — both candidates reemit.
    aura_aot_set_reemit_owner_eval(nullptr);
    aura_aot_set_register_owner_eval(nullptr);
    rf.cursor = 0;
    const auto n = aura_reemit_aot_for_dirty(1);
    CHECK(n >= 2, "AC3: nullptr path reemits both candidates");
    CHECK(ef.emitted.count("fn_null_a_2606") == 1, "AC3: A emitted");
    CHECK(ef.emitted.count("fn_null_b_2606") == 1, "AC3: B emitted");
    CHECK(metrics.reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed) ==
              skip0,
          "AC3: cross-eval skip unchanged under nullptr filter");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_cleanup_aot_state(eval_a);
    aura_cleanup_aot_state(eval_b);
    aura_set_aot_metrics(nullptr);
}

// ── AC4: query counter + single-eval zero ──
static void ac4_query_and_single_eval_zero() {
    std::println("\n--- #2606 AC4: query surface + single-eval skip=0 ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2606") == 2606, "AC4: schema-2606=2606");
    CHECK(href(cs, "issue-2606") == 2606, "AC4: issue-2606=2606");
    CHECK(href(cs, "reemit-cross-eval-filter-wired") == 1, "AC4: filter-wired=1");
    CHECK(href(cs, "reemit_cross_eval_candidate_skipped_total") >= 0,
          "AC4: skip counter key present");

    // Single-eval soak: one owner, own candidates → skip stays flat.
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    enable_soft_reemit();

    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA631));
    int p = 0;
    const auto sid = aura_get_or_preserve_stable_func_id("fn_single_2606", &p);
    aura_aot_set_register_owner_eval(eval_a);
    aura_register_fn_tracked(static_cast<std::int64_t>(sid), 0xC100u);
    aura_aot_set_register_owner_eval(nullptr);

    ReemitFixture rf;
    rf.candidates = {{"fn_single_2606", 0, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto skip0 =
        metrics.reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed);
    aura_aot_set_reemit_owner_eval(eval_a);
    for (int i = 0; i < 8; ++i) {
        rf.cursor = 0;
        (void)aura_reemit_aot_for_dirty(static_cast<std::uint64_t>(i + 1));
    }
    aura_aot_set_reemit_owner_eval(nullptr);
    CHECK(metrics.reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed) ==
              skip0,
          "AC4: single-eval soak keeps skip counter flat");
    CHECK(ef.ok.load() >= 1, "AC4: single-eval reemit progressed");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_cleanup_aot_state(eval_a);
    aura_set_aot_metrics(nullptr);
}

} // namespace

int run_test_pereval_reemit_region_independence() {
    std::println("=== test_pereval_reemit_region_independence ===");
    ac5_source_cite();
    ac1_dual_eval_reemit_owner_filter();
    ac2_invalidate_for_eval_isolation();
    ac3_nullptr_path_no_filter();
    ac4_query_and_single_eval_zero();
    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pereval_reemit_region_independence();
}
#endif
