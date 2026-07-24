// tests/compiler/test_closure_view_batch.cpp — closure_view pair dup-merge (R19 phase 17).
// R19 phase17 — Issue #1888 + #1926 closure_view pair
//
//   #1888: ClosureView lifetime stamp / UAF guards
//   #1926: make_closure_view raw pointer lifetime guards (refine #1888 #1870 #1947).
//          View after move/GC/erase must not UAF.
//
//   AC1:  make_closure_view stamps source_lifetime_version + live (#1888 AC1)
//   AC2:  move-from tombstones; is_closure_view_valid(view, moved) false (#1888 AC2)
//   AC3:  make_closure_view on tombstoned rejects + dangling-prevented++ (#1888 AC3)
//   AC4:  soft invalid accessors return null / empty without requiring flat (#1888 AC4)
//   AC5:  query:closure-view-lifetime-stats schema 1888 (#1888 AC5)
//   AC6:  erase_active_closure tombstones before erase (#1888 AC6)
//   AC7:  source cites #1926; revalidate_closure_snapshot + dual-epoch stamp (#1926 AC1)
//   AC8:  query:closure-view-lifetime-stats schema-1926 + AC metric keys (#1926 AC2)
//   AC9:  make_closure_view stamps source_lifetime_version + bridge_bridge_epoch (#1926 AC3)
//   AC10: move/tombstone → strong check fails; dangling/invalid counters bump (#1926 AC4)
//   AC11: revalidate_closure_snapshot rejects after erase_active_closure (#1926 AC5)
//   AC12: find_active_closure returns nullopt for tombstoned entries (#1926 AC6)
//   AC13: multi-round view+invalidate stress (no crash; counters grow) (#1926 AC7)
//   AC14: #1888 lineage schema retained (#1926 AC8)

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
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
using aura::compiler::ClosureView;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::g_closure_view_dangling_prevented_total;
using aura::compiler::g_closure_view_invalid_access_total;
using aura::compiler::invalidate_closure_lifetime;
using aura::compiler::is_closure_view_valid;
using aura::compiler::make_closure_view;
using aura::compiler::make_invalid_closure_view;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:closure-view-lifetime-stats\") '{}')", key));
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

// ── #1888 ACs ──

static void ac1888_1_stamp() {
    std::println("\n--- AC1: make_closure_view stamps lifetime (#1888 AC1) ---");
    Closure cl;
    cl.name = "fn";
    cl.params = {static_cast<SymId>(1), static_cast<SymId>(2)};
    cl.body_id = 7;
    cl.lifetime_version = 3;
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 3, "source version 3");
    CHECK(v.arity() == 2, "arity 2");
    CHECK(v.param_at(0) == static_cast<SymId>(1), "param0");
    CHECK(is_closure_view_valid(v), "soft valid");
    CHECK(is_closure_view_valid(v, cl), "strong valid vs live cl");
}

static void ac1888_2_move_tombstone() {
    std::println("\n--- AC2: move tombstones source (#1888 AC2) ---");
    Closure cl;
    cl.name = "mv";
    cl.params = {static_cast<SymId>(9)};
    cl.lifetime_version = 5;
    auto v = make_closure_view(cl);
    CHECK(is_closure_view_valid(v, cl), "valid before move");
    Closure dest = std::move(cl);
    CHECK(cl.lifetime_version == 0, "moved-from version 0");
    CHECK(cl.flat == nullptr && cl.pool == nullptr, "moved-from pointees null");
    CHECK(dest.lifetime_version == 5, "dest keeps version");
    CHECK(!is_closure_view_valid(v, cl), "view invalid vs moved-from");
    CHECK(is_closure_view_valid(v, dest), "view still matches dest");
}

static void ac1888_3_reject_tombstoned() {
    std::println("\n--- AC3: reject tombstoned make_closure_view (#1888 AC3) ---");
    const auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    Closure cl;
    cl.name = "dead";
    cl.params = {static_cast<SymId>(1)};
    invalidate_closure_lifetime(cl);
    CHECK(cl.lifetime_version == 0, "tombstoned");
    auto v = make_closure_view(cl);
    CHECK(!v.live, "not live");
    CHECK(v.source_lifetime_version == 0, "version 0");
    CHECK(!is_closure_view_valid(v), "soft invalid");
    const auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after > before, "dangling-prevented bumped");
}

static void ac1888_4_safe_accessors() {
    std::println("\n--- AC4: safe accessors on invalid view (#1888 AC4) ---");
    const auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    auto inv = make_invalid_closure_view();
    CHECK(closure_view_flat(inv) == nullptr, "flat null");
    CHECK(inv.arity() == 0, "arity 0");
    CHECK(inv.param_at(0) == SymId{}, "param empty");
    const auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after > before, "accessor prevented bump");
}

static void ac1888_5_query(CompilerService& cs) {
    std::println("\n--- AC5: query:closure-view-lifetime-stats (#1888 AC5) ---");
    Closure cl;
    invalidate_closure_lifetime(cl);
    (void)make_closure_view(cl);
    auto h = cs.eval("(engine:metrics \"query:closure-view-lifetime-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1888, "schema 1888");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "lifetime-guard") == 1, "lifetime-guard");
    CHECK(href(cs, "dangling-prevented") >= 1, "dangling-prevented");
}

static void ac1888_6_erase_tombstones(CompilerService& cs) {
    std::println("\n--- AC6: erase_active_closure tombstones (#1888 AC6) ---");
    auto& ev = cs.evaluator();
    Closure cl;
    cl.name = "reg";
    cl.params = {static_cast<SymId>(3)};
    cl.env_id = NULL_ENV_ID;
    auto id = ev.register_active_closure(std::move(cl));
    auto snap = ev.find_active_closure(id);
    CHECK(snap.has_value(), "registered");
    auto v = make_closure_view(*snap);
    CHECK(is_closure_view_valid(v, *snap), "valid before erase");
    CHECK(ev.erase_active_closure(id), "erased");
    CHECK(!ev.find_active_closure(id).has_value(), "gone");
    Closure local;
    local.name = "x";
    auto v2 = make_closure_view(local);
    invalidate_closure_lifetime(local);
    CHECK(!is_closure_view_valid(v2, local), "invalid after explicit invalidate");
}

// ── #1926 ACs ──

static void ac1926_1_source() {
    std::println("\n--- AC7: #1926 source surface (#1926 AC1) ---");
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

static void ac1926_2_schema() {
    std::println("\n--- AC8: schema-1926 on closure-view-lifetime-stats (#1926 AC2) ---");
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

static void ac1926_3_stamp() {
    std::println("\n--- AC9: make_closure_view dual stamp (#1926 AC3) ---");
    auto cl = make_fresh(7, 99);
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 7, "life 7");
    CHECK(v.source_bridge_epoch == 99, "bridge 99");
    CHECK(is_closure_view_valid(v), "soft");
    CHECK(is_closure_view_valid(v, cl), "strong");
}

static void ac1926_4_tombstone() {
    std::println("\n--- AC10: tombstone / move → counters (#1926 AC4) ---");
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

static void ac1926_5_revalidate_after_erase() {
    std::println("\n--- AC11: revalidate_closure_snapshot after erase (#1926 AC5) ---");
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

static void ac1926_6_find_tombstoned() {
    std::println("\n--- AC12: find_active_closure skips tombstoned (#1926 AC6) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto cl = make_fresh(5, 1);
    auto id = ev.register_active_closure(cl);
    CHECK(ev.erase_active_closure(id), "erase");
    CHECK(!ev.find_active_closure(id).has_value(), "find nullopt after erase");
}

static void ac1926_7_stress() {
    std::println("\n--- AC13: multi-round view + invalidate stress (#1926 AC7) ---");
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

static void ac1926_8_lineage() {
    std::println("\n--- AC14: #1888 lineage (#1926 AC8) ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1888, "schema 1888");
    CHECK(href(cs, "lifetime-guard") == 1, "guard");
    CHECK(href(cs, "active") == 1, "active");
}

} // namespace

int main() {
    std::println("=== closure_view pair: #1888 (lifetime) + #1926 (UAF guard) ===\n");
    CompilerService cs;
    ac1888_1_stamp();
    ac1888_2_move_tombstone();
    ac1888_3_reject_tombstoned();
    ac1888_4_safe_accessors();
    ac1888_5_query(cs);
    ac1888_6_erase_tombstones(cs);
    ac1926_1_source();
    ac1926_2_schema();
    ac1926_3_stamp();
    ac1926_4_tombstone();
    ac1926_5_revalidate_after_erase();
    ac1926_6_find_tombstoned();
    ac1926_7_stress();
    ac1926_8_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
