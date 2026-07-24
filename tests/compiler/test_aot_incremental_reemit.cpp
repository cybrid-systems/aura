// @category: unit
// @reason: Issue #1930 — complete aura_reemit_aot_for_dirty LLVM re-emit
// Issue #1480/#1930/#1943/#1952 (#1978 renamed): issue# moved from filename to header.
// pipeline + stable name→func_id map (refine #1952 #1480 #1943).
//
//   AC1: source cites #1930; stable map + emit path + return-success
//   AC2: query:aot-incremental-reemit-stats schema-1930 + AC metric keys
//   AC3: aura_get_or_preserve_stable_func_id assigns then preserves
//   AC4: reemit with emit callback — return = success count; metrics
//   AC5: reemit without emit (skeleton) — return = would-reemit (#1480)
//   AC6: multi-round same names → func_id stable; preserved_total grows
//   AC7: 1000-iter fuzz candidates + partial emit failure — no crash
//   AC8: #1952 getters + #1480 count lineage retained

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h" // aura_set_aot_metrics
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <unordered_set>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

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
    std::unordered_set<std::string> fail_names;
    std::atomic<std::uint32_t> calls{0};
    std::atomic<std::uint32_t> ok{0};
};

static bool emit_fn(const char* name, std::uint64_t /*region*/, void* userdata) {
    auto* f = static_cast<EmitFixture*>(userdata);
    f->calls.fetch_add(1, std::memory_order_relaxed);
    if (!name)
        return false;
    if (f->fail_names.count(name))
        return false;
    f->ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static void ac1_source() {
    std::println("\n--- AC1: #1930 source surface ---");
    auto bridge =
        read_first({"src/compiler/aura_jit_bridge.cpp", "../src/compiler/aura_jit_bridge.cpp"});
    auto hdr = read_first({"src/compiler/aura_jit_bridge.h", "../src/compiler/aura_jit_bridge.h"});
    auto q = read_first({"src/compiler/evaluator_primitives_query.cpp",
                         "../src/compiler/evaluator_primitives_query.cpp"});
    CHECK(!bridge.empty() && bridge.find("#1930") != std::string::npos, "bridge cites #1930");
    CHECK(bridge.find("preserve_stable_func_id") != std::string::npos ||
              bridge.find("g_name_to_stable_func_id") != std::string::npos,
          "stable map present");
    CHECK(bridge.find("success_count") != std::string::npos, "success return path");
    CHECK(!hdr.empty() && hdr.find("aura_get_or_preserve_stable_func_id") != std::string::npos,
          "header API");
    CHECK(!q.empty() && q.find("schema-1930") != std::string::npos, "query schema-1930");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1930 on aot-incremental-reemit-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:aot-incremental-reemit-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1930, "schema 1930");
    CHECK(href(cs, "schema-1930") == 1930, "schema-1930");
    CHECK(href(cs, "issue-1930") == 1930, "issue-1930");
    CHECK(href(cs, "schema-1952") == 1952, "1952 lineage");
    CHECK(href(cs, "stable-func-id-map-wired") == 1, "map wired");
    CHECK(href(cs, "emit-callback-path-wired") == 1, "emit path");
    CHECK(href(cs, "return-success-when-emit-wired") == 1, "return success");
    CHECK(href(cs, "pipeline-phase") == 3, "phase 3");
    CHECK(href(cs, "aot_incremental_reemit_success_total") >= 0, "success key");
    CHECK(href(cs, "stable_func_id_preserved_total") >= 0, "preserved key");
    CHECK(href(cs, "stable_func_id_assigned_total") >= 0, "assigned key");
}

static void ac3_stable_map_api() {
    std::println("\n--- AC3: get_or_preserve assigns then preserves ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "map empty");
    int p0 = -1;
    auto id1 = aura_get_or_preserve_stable_func_id("fn_alpha", &p0);
    CHECK(id1 != 0, "assigned non-zero");
    CHECK(p0 == 0, "first is assign");
    int p1 = -1;
    auto id2 = aura_get_or_preserve_stable_func_id("fn_alpha", &p1);
    CHECK(id2 == id1, "same id on re-preserve");
    CHECK(p1 == 1, "second is preserved");
    CHECK(aura_lookup_stable_func_id("fn_alpha") == id1, "lookup");
    CHECK(aura_lookup_stable_func_id("missing") == 0, "missing 0");
    int p2 = -1;
    auto id3 = aura_get_or_preserve_stable_func_id("fn_beta", &p2);
    CHECK(id3 != id1 && id3 != 0, "distinct id for other name");
    CHECK(p2 == 0, "beta assigned");
    CHECK(aura_stable_func_id_map_size() == 2, "map size 2");
    aura_clear_stable_func_id_map();
}

static void ac4_emit_success_return() {
    std::println("\n--- AC4: emit callback — return = success count ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);

    ReemitFixture rf;
    rf.candidates = {{"a", 1, false}, {"b", 2, true}, {"c", 3, false}};
    EmitFixture ef;
    ef.fail_names.insert("b"); // one failure
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto before_success = metrics.aot_incremental_reemit_success_total.load();
    const auto before_assigned = metrics.stable_func_id_assigned_total.load();
    const auto before_count = metrics.aot_incremental_reemit_count.load();

    const auto result = aura_reemit_aot_for_dirty(0);
    CHECK(result == 2, "returns success count 2 (a+c; b failed)");
    CHECK(aura_reemit_success_count() == 2, "last success 2");
    CHECK(aura_reemit_dirty_count() == 3, "would-reemit still 3");
    CHECK(metrics.aot_incremental_reemit_success_total.load() == before_success + 2,
          "success metric +2");
    CHECK(metrics.aot_incremental_reemit_count.load() == before_count + 3, "count +3");
    CHECK(metrics.stable_func_id_assigned_total.load() == before_assigned + 2, "assigned a+c");
    CHECK(aura_lookup_stable_func_id("a") != 0, "a mapped");
    CHECK(aura_lookup_stable_func_id("c") != 0, "c mapped");
    CHECK(aura_lookup_stable_func_id("b") == 0, "failed emit not mapped");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac5_skeleton_return() {
    std::println("\n--- AC5: skeleton would-reemit return (#1480) ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask((1ULL << 1) | (1ULL << 3));

    ReemitFixture rf;
    rf.candidates = {{"foo", 1, false}, {"bar", 2, true}, {"baz", 3, false}};
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);

    const auto result = aura_reemit_aot_for_dirty(0);
    CHECK(result == 2, "skeleton returns would-reemit 2");
    CHECK(aura_reemit_dirty_count() == 2, "dirty 2");
    CHECK(aura_reemit_success_count() == 0, "no emit fn → success 0");
    CHECK(aura_stable_func_id_map_size() == 2, "foo+baz mapped on skeleton");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac6_multi_round_stable() {
    std::println("\n--- AC6: multi-round same names keep func_id ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);

    ReemitFixture rf;
    rf.candidates = {{"hot", 1, false}, {"cold", 2, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    CHECK(aura_reemit_aot_for_dirty(0) == 2, "round1 success 2");
    const auto id_hot = aura_lookup_stable_func_id("hot");
    const auto id_cold = aura_lookup_stable_func_id("cold");
    CHECK(id_hot != 0 && id_cold != 0 && id_hot != id_cold, "distinct ids");

    const auto before_pres = metrics.stable_func_id_preserved_total.load();
    const auto before_asg = metrics.stable_func_id_assigned_total.load();
    CHECK(aura_reemit_aot_for_dirty(0) == 2, "round2 success 2");
    CHECK(aura_lookup_stable_func_id("hot") == id_hot, "hot stable");
    CHECK(aura_lookup_stable_func_id("cold") == id_cold, "cold stable");
    CHECK(metrics.stable_func_id_preserved_total.load() == before_pres + 2, "preserved +2");
    CHECK(metrics.stable_func_id_assigned_total.load() == before_asg, "no new assign");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac7_fuzz() {
    std::println("\n--- AC7: 1000-iter fuzz ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);

    ReemitFixture rf;
    rf.candidates = {
        {"f0", 1, false}, {"f1", 2, true}, {"f2", 3, false}, {"f3", 4, false}, {"f4", 5, true}};
    EmitFixture ef;
    ef.fail_names.insert("f1"); // always fail one
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    std::uint64_t total_success = 0;
    for (int i = 0; i < 1000; ++i) {
        total_success += aura_reemit_aot_for_dirty(0);
    }
    CHECK(total_success == 4000, "1000*4 success (f1 always fails)");
    CHECK(aura_stable_func_id_map_size() == 4, "4 stable ids");
    const auto id0 = aura_lookup_stable_func_id("f0");
    CHECK(id0 != 0, "f0 id");
    // Spot-check stability after fuzz
    CHECK(aura_lookup_stable_func_id("f0") == id0, "f0 still stable");
    CHECK(metrics.aot_incremental_reemit_success_total.load() >= 4000, "success metric");
    CHECK(metrics.stable_func_id_preserved_total.load() > 0, "preserved grew");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1952 getters + schema lineage ---");
    Evaluator ev;
    CHECK(ev.get_aot_incremental_reemit_success_total() == 0, "success 0");
    CHECK(ev.get_stable_func_id_preserved_total() == 0, "preserved 0");
    CHECK(ev.get_stable_func_id_assigned_total() == 0, "assigned 0");
    CHECK(ev.get_aot_incremental_reemit_count() == 0, "count 0");
    CompilerService cs;
    CHECK(href(cs, "schema-1952") == 1952, "1952");
    CHECK(href(cs, "active") == 1, "active");
}

} // namespace

int main() {
    std::println("=== Issue #1930: AOT incremental re-emit + stable func_id ===");
    ac1_source();
    ac2_schema();
    ac3_stable_map_api();
    ac4_emit_success_return();
    ac5_skeleton_return();
    ac6_multi_round_stable();
    ac7_fuzz();
    ac8_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
