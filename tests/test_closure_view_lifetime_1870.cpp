// @category: unit
// @reason: Issue #1870 — ClosureView / make_closure_view is zero-copy
// (params span, name string_view, raw flat/pool/arena pointers). Valid
// only until the source Closure (or pointees) is mutated/freed; same
// class as EnvView #1868. No owned defensive copy.
//
//   AC1: ClosureView / make_closure_view document #1870 lifetime
//   AC2: sequential make_closure_view reads params / name / arity
//   AC3: remake after params growth sees new arity (supported pattern)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.evaluator;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::make_closure_view;
using aura::compiler::NULL_ENV_ID;
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
    // ── AC1: source contract ──
    {
        std::println("\n--- AC1: #1870 ClosureView lifetime docs ---");
        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        auto env_cpp =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(ixx.find("#1870") != std::string::npos, "ClosureView cites #1870");
        auto cpos = ixx.find("export struct ClosureView");
        CHECK(cpos != std::string::npos, "ClosureView present");
        auto cwin = ixx.substr(cpos > 900 ? cpos - 900 : 0, 1600);
        CHECK(cwin.find("zero-copy") != std::string::npos || cwin.find("span") != std::string::npos,
              "documents zero-copy / span");
        CHECK(cwin.find("invalid") != std::string::npos ||
                  cwin.find("realloc") != std::string::npos ||
                  cwin.find("dangle") != std::string::npos ||
                  cwin.find("outlive") != std::string::npos,
              "documents invalidation / lifetime");
        CHECK(cwin.find("#1868") != std::string::npos || cwin.find("EnvView") != std::string::npos,
              "ties to EnvView / #1868");
        CHECK(env_cpp.find("#1870") != std::string::npos, "make_closure_view cites #1870");
        auto mpos = env_cpp.find("make_closure_view");
        CHECK(mpos != std::string::npos, "make_closure_view present");
        auto mwin = env_cpp.substr(mpos > 400 ? mpos - 400 : 0, 900);
        CHECK(mwin.find("zero-copy") != std::string::npos ||
                  mwin.find("dangle") != std::string::npos ||
                  mwin.find("#1870") != std::string::npos,
              "make_closure_view documents lifetime");
        // Must remain zero-copy (no owned params vector on ClosureView).
        CHECK(ixx.find("params_owned") == std::string::npos, "no defensive owned params");
    }

    // ── AC2: sequential zero-copy view ──
    {
        std::println("\n--- AC2: sequential make_closure_view ---");
        Closure cl;
        cl.name = "my-fn";
        cl.params = {static_cast<SymId>(1), static_cast<SymId>(2), static_cast<SymId>(3)};
        cl.body_id = 42;
        cl.dotted = false;
        cl.env_id = NULL_ENV_ID;
        auto view = make_closure_view(cl);
        CHECK(view.arity() == 3, "arity 3");
        CHECK(view.body_id == 42, "body_id 42");
        CHECK(view.name == "my-fn", "name my-fn");
        CHECK(view.param_at(0) == static_cast<SymId>(1), "param 0");
        CHECK(view.param_at(1) == static_cast<SymId>(2), "param 1");
        CHECK(view.param_at(2) == static_cast<SymId>(3), "param 2");
        CHECK(view.param_at(99) == SymId{}, "OOB param → null SymId");
        CHECK(view.env_id == NULL_ENV_ID, "env_id");
        CHECK(view.flat == nullptr && view.pool == nullptr, "null pointees ok");
    }

    // ── AC3: remake after mutation (supported pattern) ──
    {
        std::println("\n--- AC3: remake view after params growth ---");
        Closure cl;
        cl.name = "grow";
        cl.params = {static_cast<SymId>(10)};
        auto v1 = make_closure_view(cl);
        CHECK(v1.arity() == 1, "v1 arity 1");
        // Contract: discard v1 before mutating params; remake.
        for (int i = 0; i < 64; ++i)
            cl.params.push_back(static_cast<SymId>(100 + i));
        cl.name = "grown";
        auto v2 = make_closure_view(cl);
        CHECK(v2.arity() == 65, "v2 arity 65");
        CHECK(v2.name == "grown", "v2 name updated");
        CHECK(v2.param_at(0) == static_cast<SymId>(10), "v2 first param");
        CHECK(v2.param_at(64) == static_cast<SymId>(163), "v2 last param");
    }

    std::println("\n=== test_closure_view_lifetime_1870: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
