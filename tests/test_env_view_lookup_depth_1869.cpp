// @category: unit
// @reason: Issue #1869 — EnvView::lookup / lookup_by_intern /
// lookup_by_symid must share MAX_ENV_DEPTH so a cyclic parent Env
// graph cannot stack-overflow via the view fallthrough.
//
//   AC1: source cites #1869; EnvView lookups use env_lookup_enter
//   AC2: normal parent chain finds binding via view
//   AC3: cyclic parent_ returns nullopt (no crash)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::ast::StringPool;
using aura::compiler::Env;
using aura::compiler::make_env_view;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: EnvView lookup depth guard ---");
        auto src =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        CHECK(!src.empty(), "read evaluator_env.cpp");
        CHECK(src.find("#1869") != std::string::npos, "cites #1869");
        // Prefer definitions (skip early comment "EnvView::lookup*").
        auto pos = src.find("EnvView::lookup(const std::string");
        if (pos == std::string::npos)
            pos = src.find("std::optional<EvalValue> EnvView::lookup");
        CHECK(pos != std::string::npos, "EnvView::lookup present");
        auto win = src.substr(pos, 2800);
        CHECK(win.find("env_lookup_enter") != std::string::npos, "uses env_lookup_enter");
        CHECK(win.find("EnvLookupDepthGuard") != std::string::npos, "uses EnvLookupDepthGuard");
        CHECK(win.find("EnvView::lookup_by_intern") != std::string::npos,
              "lookup_by_intern present");
        CHECK(win.find("EnvView::lookup_by_symid") != std::string::npos, "lookup_by_symid present");
        std::size_t count = 0;
        for (std::size_t i = 0; (i = win.find("env_lookup_enter", i)) != std::string::npos; i += 1)
            ++count;
        CHECK(count >= 3, "all three EnvView lookups enter depth");
    }

    // ── AC2: normal chain ──
    {
        std::println("\n--- AC2: EnvView parent chain ---");
        Env root;
        root.bind("x", make_int(11));
        Env mid;
        mid.set_parent(&root);
        Env leaf;
        leaf.set_parent(&mid);
        auto view = make_env_view(leaf);
        auto v = view.lookup("x");
        CHECK(v && is_int(*v) && as_int(*v) == 11, "view.lookup through parents");
        // Local shadow.
        leaf.bind("x", make_int(99));
        auto view2 = make_env_view(leaf);
        auto v2 = view2.lookup("x");
        CHECK(v2 && is_int(*v2) && as_int(*v2) == 99, "local shadow wins");
        // SymId path with pool.
        StringPool pool;
        auto sx = pool.intern("y");
        root.set_pool(&pool);
        root.bind_symid(sx, make_int(5));
        auto vs = make_env_view(leaf).lookup_by_symid(sx);
        CHECK(vs && is_int(*vs) && as_int(*vs) == 5, "lookup_by_symid via parents");
        auto vi = make_env_view(leaf).lookup_by_intern("y", &pool);
        CHECK(vi && is_int(*vi) && as_int(*vi) == 5, "lookup_by_intern via parents");
    }

    // ── AC3: cyclic parent ──
    {
        std::println("\n--- AC3: cyclic parent_ does not overflow ---");
        Env a;
        Env b;
        a.set_parent(&b);
        b.set_parent(&a);
        auto va = make_env_view(a);
        auto r = va.lookup("cycle-miss");
        CHECK(!r.has_value(), "cycle → nullopt via view.lookup");
        auto r2 = va.lookup_by_symid(1);
        CHECK(!r2.has_value(), "cycle → nullopt via lookup_by_symid");
        StringPool pool;
        auto r3 = va.lookup_by_intern("cycle-miss", &pool);
        CHECK(!r3.has_value(), "cycle → nullopt via lookup_by_intern");
        // After cycle, acyclic still works.
        Env root;
        root.bind("z", make_int(3));
        Env leaf;
        leaf.set_parent(&root);
        auto z = make_env_view(leaf).lookup("z");
        CHECK(z && is_int(*z) && as_int(*z) == 3, "post-cycle acyclic view ok");
    }

    std::println("\n=== test_env_view_lookup_depth_1869: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
