// @category: unit
// @reason: Issue #1860 — Env::lookup_binding (and lookup_by_symid
// parent_ walk) must share the MAX_ENV_DEPTH guard so a cyclic
// parent_ graph cannot stack-overflow.
//
//   AC1: source cites #1860; lookup_binding uses depth guard
//   AC2: normal parent chain finds binding
//   AC3: cyclic parent_ returns nullopt (no crash / hang)

#include "test_harness.hpp"

#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::Env;
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

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: lookup_binding depth guard ---");
        std::string src;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator_env.cpp");
        CHECK(src.find("#1860") != std::string::npos, "cites #1860");
        auto pos = src.find("Env::lookup_binding");
        CHECK(pos != std::string::npos, "lookup_binding present");
        auto win = src.substr(pos, 900);
        CHECK(win.find("env_lookup_enter") != std::string::npos ||
                  win.find("g_env_lookup_depth") != std::string::npos ||
                  win.find("EnvLookupDepthGuard") != std::string::npos,
              "uses shared depth guard");
        CHECK(win.find("parent_->lookup_binding") != std::string::npos, "still walks parent_");
        // lookup_by_symid also guarded (audit sibling).
        auto spos = src.find("Env::lookup_by_symid");
        CHECK(spos != std::string::npos, "lookup_by_symid present");
        auto swin = src.substr(spos, 900);
        CHECK(swin.find("env_lookup_enter") != std::string::npos ||
                  swin.find("EnvLookupDepthGuard") != std::string::npos,
              "lookup_by_symid depth guarded");
    }

    // ── AC2: normal chain ──
    {
        std::println("\n--- AC2: lookup_binding parent chain ---");
        Env root;
        root.bind("x", make_int(7));
        Env mid;
        mid.set_parent(&root);
        Env leaf;
        leaf.set_parent(&mid);
        auto v = leaf.lookup_binding("x");
        CHECK(v.has_value(), "lookup_binding found x");
        if (v)
            CHECK(is_int(*v) && as_int(*v) == 7, "value is 7");
        // lookup also works after lookup_binding (shared depth restored).
        auto v2 = leaf.lookup("x");
        CHECK(v2 && is_int(*v2) && as_int(*v2) == 7, "lookup after binding still ok");
    }

    // ── AC3: cyclic parent_ ──
    {
        std::println("\n--- AC3: cyclic parent_ does not overflow ---");
        Env a;
        Env b;
        a.set_parent(&b);
        b.set_parent(&a); // cycle
        // No local binding of "cycle-miss" — walk must hit depth limit.
        auto r = a.lookup_binding("cycle-miss");
        CHECK(!r.has_value(), "cycle → nullopt (depth guard)");
        auto r2 = a.lookup("cycle-miss");
        CHECK(!r2.has_value(), "lookup also bails on cycle");
        // After cycle attempt, a fresh acyclic chain still works.
        Env root;
        root.bind("y", make_int(1));
        Env leaf;
        leaf.set_parent(&root);
        auto y = leaf.lookup_binding("y");
        CHECK(y && is_int(*y) && as_int(*y) == 1, "post-cycle acyclic lookup_binding ok");
    }

    std::println("\n=== test_env_lookup_binding_depth_1860: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
