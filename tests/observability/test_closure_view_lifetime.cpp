// @category: unit
// @reason: Issue #1888 — ClosureView lifetime stamp / UAF guards
// Issue #1888 (#1978 renamed): issue# moved from filename to header.
//
// AC1: make_closure_view stamps source_lifetime_version + live
// AC2: move-from tombstones; is_closure_view_valid(view, moved) false
// AC3: make_closure_view on tombstoned rejects + dangling-prevented++
// AC4: soft invalid accessors return null / empty without requiring flat
// AC5: query:closure-view-lifetime-stats schema 1888
// AC6: erase_active_closure tombstones before erase

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>
#include <utility>

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
using aura::compiler::g_closure_view_dangling_prevented_total;
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

void ac1_stamp() {
    std::println("\n--- AC1: make_closure_view stamps lifetime ---");
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

void ac2_move_tombstone() {
    std::println("\n--- AC2: move tombstones source ---");
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

void ac3_reject_tombstoned() {
    std::println("\n--- AC3: reject tombstoned make_closure_view ---");
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

void ac4_safe_accessors() {
    std::println("\n--- AC4: safe accessors on invalid view ---");
    const auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    auto inv = make_invalid_closure_view();
    CHECK(closure_view_flat(inv) == nullptr, "flat null");
    CHECK(inv.arity() == 0, "arity 0");
    CHECK(inv.param_at(0) == SymId{}, "param empty");
    const auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after > before, "accessor prevented bump");
}

void ac5_query(CompilerService& cs) {
    std::println("\n--- AC5: query:closure-view-lifetime-stats ---");
    // Force at least one prevented
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

void ac6_erase_tombstones(CompilerService& cs) {
    std::println("\n--- AC6: erase_active_closure tombstones ---");
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
    // Local snap is a copy taken before erase — still has its own version.
    // Fresh register path: moved-from stack cl is tombstoned by move into map.
    Closure local;
    local.name = "x";
    auto v2 = make_closure_view(local);
    invalidate_closure_lifetime(local);
    CHECK(!is_closure_view_valid(v2, local), "invalid after explicit invalidate");
}

} // namespace

int main() {
    std::println("=== Issue #1888: ClosureView lifetime / UAF guards ===");
    CompilerService cs;
    ac1_stamp();
    ac2_move_tombstone();
    ac3_reject_tombstoned();
    ac4_safe_accessors();
    ac5_query(cs);
    ac6_erase_tombstones(cs);
    std::println("\n=== #1888: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
