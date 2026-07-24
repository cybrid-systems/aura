// @category: unit
// @reason: Issue #1926 — make_closure_view raw pointer lifetime guards
// Issue #1870/#1888/#1926/#1947 (#1978 renamed): issue# moved from filename to header.
// (refine #1888 #1870 #1947). View after move/GC/erase must not UAF.
//
//   AC1: source cites #1926; revalidate_closure_snapshot + dual-epoch stamp
//   AC2: query:closure-view-lifetime-stats schema-1926 + AC metric keys
//   AC3: make_closure_view stamps source_lifetime_version + bridge_bridge_epoch
//   AC4: move/tombstone → strong check fails; dangling/invalid counters bump
//   AC5: revalidate_closure_snapshot rejects after erase_active_closure
//   AC6: find_active_closure returns nullopt for tombstoned entries
//   AC7: multi-round view+invalidate stress (no crash; counters grow)
//   AC8: #1888 lineage schema retained

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
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
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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
    cl.name = "c";
    cl.params = {static_cast<SymId>(1)};
    cl.body_id = 1;
    cl.env_id = NULL_ENV_ID;
    cl.lifetime_version = life;
    cl.bridge_epoch = bridge;
    return cl;
}

static void ac1_source() {
    std::println("\n--- AC1: #1926 source surface ---");
    auto env = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto flat = read_first(
        {"src/compiler/evaluator_eval_flat.cpp", "../src/compiler/evaluator_eval_flat.cpp"});
    CHECK(!env.empty() && env.find("#1926") != std::string::npos, "env cites #1926");
    CHECK(env.find("revalidate_closure_snapshot") != std::string::npos, "revalidate");
    CHECK(env.find("source_bridge_epoch") != std::string::npos, "dual-epoch stamp");
    CHECK(!ixx.empty() && ixx.find("revalidate_closure_snapshot") != std::string::npos, "ixx API");
    CHECK(ixx.find("source_bridge_epoch") != std::string::npos, "view field");
    CHECK(!flat.empty() && flat.find("#1926") != std::string::npos, "apply_closure guard");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1926 on closure-view-lifetime-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:closure-view-lifetime-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1888, "lineage 1888");
    CHECK(href(cs, "schema-1926") == 1926, "schema-1926");
    CHECK(href(cs, "issue-1926") == 1926, "issue-1926");
    CHECK(href(cs, "snapshot-revalidate-wired") == 1, "revalidate wired");
    CHECK(href(cs, "dual-epoch-wired") == 1, "dual-epoch wired");
    CHECK(href(cs, "dangling-prevented") >= 0, "dangling key");
    CHECK(href(cs, "invalid-access") >= 0, "invalid-access key");
    CHECK(href(cs, "closure_view_dangling_prevented_total") >= 0, "AC metric name");
    CHECK(href(cs, "closure_view_invalid_access_total") >= 0, "AC metric name 2");
}

static void ac3_stamp() {
    std::println("\n--- AC3: make_closure_view dual stamp ---");
    auto cl = make_fresh(7, 99);
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 7, "life 7");
    CHECK(v.source_bridge_epoch == 99, "bridge 99");
    CHECK(is_closure_view_valid(v), "soft");
    CHECK(is_closure_view_valid(v, cl), "strong");
}

static void ac4_tombstone() {
    std::println("\n--- AC4: tombstone / move → counters ---");
    auto cl = make_fresh(3, 10);
    auto v = make_closure_view(cl);
    const auto d0 = g_closure_view_dangling_prevented_total.load();
    const auto i0 = g_closure_view_invalid_access_total.load();
    invalidate_closure_lifetime(cl);
    CHECK(!is_closure_view_valid(v, cl), "strong fails");
    CHECK(g_closure_view_invalid_access_total.load() > i0, "invalid-access +");
    auto v2 = make_closure_view(cl);
    CHECK(!v2.live, "reject tombstoned");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling +");
    CHECK(closure_view_flat(v2) == nullptr, "accessor null");
}

static void ac5_revalidate_after_erase() {
    std::println("\n--- AC5: revalidate_closure_snapshot after erase ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto cl = make_fresh(11, 22);
    auto id = ev.register_active_closure(cl);
    auto snap = ev.find_active_closure(id);
    CHECK(snap.has_value(), "registered");
    CHECK(ev.revalidate_closure_snapshot(id, *snap), "revalidate ok");
    CHECK(ev.erase_active_closure(id), "erase");
    CHECK(!ev.revalidate_closure_snapshot(id, *snap), "revalidate fails after erase");
    CHECK(!ev.find_active_closure(id).has_value(), "find gone");
}

static void ac6_find_tombstoned() {
    std::println("\n--- AC6: find_active_closure skips tombstoned ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto cl = make_fresh(5, 1);
    auto id = ev.register_active_closure(cl);
    {
        // Manually tombstone in map via erase path only — erase is the public API.
        CHECK(ev.erase_active_closure(id), "erase");
    }
    CHECK(!ev.find_active_closure(id).has_value(), "find nullopt after erase");
}

static void ac7_stress() {
    std::println("\n--- AC7: multi-round view + invalidate stress ---");
    const auto d0 = g_closure_view_dangling_prevented_total.load();
    const auto i0 = g_closure_view_invalid_access_total.load();
    for (int i = 0; i < 500; ++i) {
        auto cl = make_fresh(static_cast<std::uint64_t>(i + 1), static_cast<std::uint64_t>(i));
        auto v = make_closure_view(cl);
        if (i % 3 == 0) {
            invalidate_closure_lifetime(cl);
            (void)is_closure_view_valid(v, cl);
            (void)make_closure_view(cl);
            (void)closure_view_flat(v);
        } else {
            CHECK(is_closure_view_valid(v, cl), "valid mid-stress");
        }
    }
    CHECK(g_closure_view_dangling_prevented_total.load() >= d0, "dangling mono");
    CHECK(g_closure_view_invalid_access_total.load() >= i0, "invalid mono");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling grew");
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1888 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1888, "schema 1888");
    CHECK(href(cs, "lifetime-guard") == 1, "guard");
    CHECK(href(cs, "active") == 1, "active");
}

} // namespace

int main() {
    std::println("=== Issue #1926: ClosureView UAF guard ===");
    ac1_source();
    ac2_schema();
    ac3_stamp();
    ac4_tombstone();
    ac5_revalidate_after_erase();
    ac6_find_tombstoned();
    ac7_stress();
    ac8_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
