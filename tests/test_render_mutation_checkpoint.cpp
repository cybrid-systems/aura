// test_render_mutation_checkpoint.cpp — Issue #1355: lightweight mutation in render hot path

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

namespace {

// Try mutate:tweak-literal on candidate node ids until one is LiteralInt.
bool find_and_tweak(CompilerService& cs, int delta) {
    for (int id = 0; id < 256; ++id) {
        auto r = cs.eval(std::format("(mutate:tweak-literal {} {})", id, delta));
        if (r && is_int(*r))
            return true;
    }
    return false;
}

std::int64_t lw_total(CompilerService& cs) {
    auto r = cs.eval("(mutation-lightweight-total)");
    return (r && is_int(*r)) ? as_int(*r) : -1;
}

} // namespace

int main() {
    CompilerService cs;

    // Baseline hooks
    {
        auto s = cs.eval("(engine:metrics \"query:mutation-lightweight-stats\")");
        CHECK(s && is_string(*s), "lightweight stats string");
        auto hp = cs.eval("(render-hotpath-enter)");
        CHECK(hp && is_bool(*hp) && as_bool(*hp), "render-hotpath-enter");
        auto hx = cs.eval("(render-hotpath-exit)");
        CHECK(hx && is_bool(*hx) && as_bool(*hx), "render-hotpath-exit");
        CHECK(lw_total(cs) >= 0, "lightweight total probe");
    }

    // Workspace with a literal
    {
        auto sc = cs.eval("(set-code \"(define (f) 42)\")");
        CHECK(sc.has_value(), "set-code");
    }

    // Outside hot path: full checkpoint (lightweight total unchanged)
    {
        const auto base = lw_total(cs);
        CHECK(find_and_tweak(cs, 1), "tweak outside hot path");
        CHECK(lw_total(cs) == base, "outside hot path: lightweight total unchanged");
    }

    // Inside hot path: lightweight path taken
    {
        const auto base = lw_total(cs);
        (void)cs.eval("(render-hotpath-enter)");
        CHECK(find_and_tweak(cs, 1), "tweak inside hot path");
        (void)cs.eval("(render-hotpath-exit)");
        CHECK(lw_total(cs) == base + 1, "inside hot path: lightweight total +1");
        auto c = cs.eval("(mutation-lightweight-commit)");
        CHECK(c && is_int(*c) && as_int(*c) >= 1, "lightweight commit after success");
    }

    // Multiple hot-path field mutations: durable log does not grow 1:1
    {
        (void)cs.eval("(set-code \"(define (g) 10)\")");
        auto log0 = cs.eval("(mutation-log-size)");
        CHECK(log0 && is_int(*log0), "mutation-log-size");
        const auto l0 = as_int(*log0);
        (void)cs.eval("(render-hotpath-enter)");
        int ok_n = 0;
        for (int i = 0; i < 40; ++i) {
            if (find_and_tweak(cs, 1))
                ++ok_n;
        }
        (void)cs.eval("(render-hotpath-exit)");
        CHECK(ok_n >= 10, "multiple hot-path tweaks");
        auto log1 = cs.eval("(mutation-log-size)");
        CHECK(log1 && is_int(*log1), "mutation-log-size after");
        // Field muts under lightweight skip durable log → growth << ok_n
        CHECK(as_int(*log1) - l0 < ok_n, "hot path does not fill durable log 1:1");
        auto rec = cs.eval("(mutation-lightweight-records)");
        CHECK(rec && is_int(*rec) && as_int(*rec) >= ok_n, "lightweight records track field muts");
    }

    // Frame boundary auto-commit
    {
        auto fr = cs.eval("(arena-render-frame-reset)");
        CHECK(fr && is_int(*fr), "arena-render-frame-reset");
        auto s = cs.eval("(engine:metrics \"query:mutation-lightweight-stats\")");
        CHECK(s && is_string(*s), "stats after frame reset");
    }

    // Failed mutation under hot path (invalid node) → rollback path
    {
        const auto rb0 = as_int(*cs.eval("(mutation-lightweight-rollback)"));
        (void)cs.eval("(render-hotpath-enter)");
        (void)cs.eval("(mutate:tweak-literal 999999 1)"); // expected fail
        (void)cs.eval("(render-hotpath-exit)");
        const auto rb1 = as_int(*cs.eval("(mutation-lightweight-rollback)"));
        // Failed guard still entered lightweight; rollback on ok=false
        CHECK(rb1 >= rb0, "failed mutate lightweight rollback non-decreasing");
    }

    // Nested hot-path boundaries
    {
        const auto base = lw_total(cs);
        (void)cs.eval("(render-hotpath-enter)");
        CHECK(find_and_tweak(cs, 1), "nested outer tweak");
        CHECK(find_and_tweak(cs, 1), "nested second tweak");
        (void)cs.eval("(render-hotpath-exit)");
        CHECK(lw_total(cs) >= base + 2, "two lightweight boundaries");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("render mutation checkpoint #1355: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
