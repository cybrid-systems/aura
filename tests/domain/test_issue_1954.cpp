// tests/domain/test_issue_1954.cpp — Wave 4 relocate from tests/test_issue_1954.cpp
// Prefer domain/; do not re-add under tests/ root. (#root_test_classification)
// @category: unit
// @reason: Issue #1954 — refine #1929 make_closure_view lifetime +
// walk_active_closures boundary mandate for hot-update Closure Bridge.
//
//   AC1: schema-1954 / issue-1954 on query:closure-view-lifetime-stats
//   AC2: AC metrics closure_view_dangling_prevented_total +
//        live_closure_stale_prevented_total live
//   AC3: make_closure_view dual-epoch stamp; tombstone reject + counters
//   AC4: walk_active_closures + force-drop flags wired; linear pair schema-1954
//   AC5: concurrent stress (views + invalidate) — counters mono; no crash
//   AC6: source cites #1954 (make_closure_view + query)

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::closure_view_flat;
using aura::compiler::closure_view_pool;
using aura::compiler::CompilerService;
using aura::compiler::g_closure_view_dangling_prevented_total;
using aura::compiler::g_closure_view_invalid_access_total;
using aura::compiler::invalidate_closure_lifetime;
using aura::compiler::is_closure_view_valid;
using aura::compiler::make_closure_view;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:closure-view-lifetime-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_linear(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
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

static Closure make_fresh(std::uint64_t life = 1, std::uint64_t bridge = 42) {
    Closure cl;
    cl.name = "c1954";
    cl.params = {static_cast<SymId>(1)};
    cl.body_id = 1;
    cl.env_id = NULL_ENV_ID;
    cl.lifetime_version = life;
    cl.bridge_epoch = bridge;
    return cl;
}

static void ac1_schema() {
    std::println("\n--- AC1: schema-1954 surface ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:closure-view-lifetime-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1888, "lineage 1888");
    CHECK(href(cs, "schema-1929") == 1929, "schema-1929 retained");
    CHECK(href(cs, "schema-1954") == 1954, "schema-1954");
    CHECK(href(cs, "issue-1954") == 1954, "issue-1954");
    CHECK(href(cs, "make-closure-view-lifetime-guard") == 1, "view guard");
    CHECK(href(cs, "safe-accessors-wired") == 1, "safe accessors");
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk wired");
    CHECK(href(cs, "boundaries-wired-count") == 6, "6 boundaries");
    CHECK(href(cs, "force-drop-or-mark-invalid-wired") == 1, "force-drop flag");
    CHECK(href(cs, "lifetime-version-stamp-wired") == 1, "lifetime stamp flag");
    CHECK(href(cs, "dual-epoch-wired") == 1, "dual-epoch");
}

static void ac2_metrics() {
    std::println("\n--- AC2: AC metrics live ---");
    CompilerService cs;
    CHECK(href(cs, "closure_view_dangling_prevented_total") >= 0, "dangling metric");
    CHECK(href(cs, "live_closure_stale_prevented_total") >= 0, "stale metric");
    CHECK(href(cs, "live-closure-stale-prevented-total") >= 0, "stale alias");
    CHECK(href(cs, "closure_view_invalid_access_total") >= 0, "invalid-access metric");
}

static void ac3_stamp_tombstone() {
    std::println("\n--- AC3: dual stamp + tombstone ---");
    auto cl = make_fresh(7, 99);
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 7, "life 7");
    CHECK(v.source_bridge_epoch == 99, "bridge 99");
    CHECK(is_closure_view_valid(v, cl), "strong ok");

    const auto d0 = g_closure_view_dangling_prevented_total.load();
    const auto i0 = g_closure_view_invalid_access_total.load();
    invalidate_closure_lifetime(cl);
    CHECK(!is_closure_view_valid(v, cl), "strong fails after tombstone");
    CHECK(g_closure_view_invalid_access_total.load() > i0, "invalid-access +");
    auto v2 = make_closure_view(cl);
    CHECK(!v2.live, "reject tombstoned");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling +");
    CHECK(closure_view_flat(v2) == nullptr, "flat null");
    CHECK(closure_view_pool(v2) == nullptr, "pool null");
}

static void ac4_walk_and_linear() {
    std::println("\n--- AC4: walk_active_closures + linear pair ---");
    CompilerService cs;
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk on view stats");
    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "linear hash");
    CHECK(href_linear(cs, "schema-1929") == 1929, "linear schema-1929");
    CHECK(href_linear(cs, "schema-1954") == 1954, "linear schema-1954");
    CHECK(href_linear(cs, "closure-view-lifetime-paired") == 1, "paired");
    CHECK(href_linear(cs, "boundaries-wired-count") == 6, "6 boundaries");
}

static void ac5_stress() {
    std::println("\n--- AC5: concurrent view + invalidate stress ---");
    const auto d0 = g_closure_view_dangling_prevented_total.load();
    const auto i0 = g_closure_view_invalid_access_total.load();
    std::atomic<int> ops{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 250; ++i) {
                auto cl = make_fresh(static_cast<std::uint64_t>(i + 1 + t * 1000),
                                     static_cast<std::uint64_t>(i + t));
                auto v = make_closure_view(cl);
                if ((i + t) % 3 == 0) {
                    invalidate_closure_lifetime(cl);
                    (void)is_closure_view_valid(v, cl);
                    (void)make_closure_view(cl);
                    (void)closure_view_flat(v);
                } else {
                    (void)is_closure_view_valid(v, cl);
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(ops.load() >= 1000, "ops completed");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling grew");
    CHECK(g_closure_view_invalid_access_total.load() > i0, "invalid grew");
    CompilerService cs;
    CHECK(href(cs, "schema-1954") == 1954, "schema after stress");
}

static void ac6_source() {
    std::println("\n--- AC6: source cites #1954 ---");
    auto env = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto mut = read_first({"src/compiler/evaluator_primitives_mutate.cpp",
                           "../src/compiler/evaluator_primitives_mutate.cpp"});
    auto safety = read_first({"tests/test_closure_bridge_lifetime_safety.cpp",
                              "../tests/test_closure_bridge_lifetime_safety.cpp"});
    CHECK(!env.empty() && env.find("#1954") != std::string::npos, "env cites #1954");
    CHECK(env.find("make_closure_view") != std::string::npos, "make_closure_view");
    CHECK(!ixx.empty() && ixx.find("#1954") != std::string::npos, "ixx cites #1954");
    CHECK(!mut.empty() && mut.find("schema-1954") != std::string::npos, "query schema-1954");
    CHECK(!safety.empty() && safety.find("1954") != std::string::npos, "safety test cites 1954");
}

} // namespace

int main() {
    std::println("=== Issue #1954: Closure Bridge lifetime refine of #1929 ===");
    ac1_schema();
    ac2_metrics();
    ac3_stamp_tombstone();
    ac4_walk_and_linear();
    ac5_stress();
    ac6_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
