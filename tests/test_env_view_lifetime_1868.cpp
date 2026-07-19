// @category: unit
// @reason: Issue #1868 — EnvView / make_env_view zero-copy spans are
// valid only until the source Env is mutated (bind realloc). Concurrent
// mutation is unsupported under #1861 single-writer; no owned copy.
//
//   AC1: EnvView / make_env_view document #1868 lifetime contract
//   AC2: sequential make_env_view + lookup (zero-copy still works)
//   AC3: after bind growth, remade view sees new binding; contract
//        forbids using stale view (documented, not UAF-tested)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

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
    // ── AC1: source contract ──
    {
        std::println("\n--- AC1: #1868 EnvView lifetime docs ---");
        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        auto env_cpp =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(ixx.find("#1868") != std::string::npos, "EnvView cites #1868");
        auto epos = ixx.find("export struct EnvView");
        CHECK(epos != std::string::npos, "EnvView present");
        auto ewin = ixx.substr(epos > 800 ? epos - 800 : 0, 1600);
        CHECK(ewin.find("zero-copy") != std::string::npos || ewin.find("span") != std::string::npos,
              "documents spans");
        CHECK(ewin.find("invalid") != std::string::npos || ewin.find("UAF") != std::string::npos ||
                  ewin.find("realloc") != std::string::npos,
              "documents invalidation / realloc");
        CHECK(ewin.find("single-writer") != std::string::npos ||
                  ewin.find("#1861") != std::string::npos,
              "ties to single-writer / #1861");
        CHECK(env_cpp.find("#1868") != std::string::npos, "make_env_view cites #1868");
        auto mpos = env_cpp.find("make_env_view");
        CHECK(mpos != std::string::npos, "make_env_view present");
        auto mwin = env_cpp.substr(mpos > 400 ? mpos - 400 : 0, 900);
        CHECK(mwin.find("zero-copy") != std::string::npos ||
                  mwin.find("dangle") != std::string::npos ||
                  mwin.find("bind") != std::string::npos,
              "make_env_view documents lifetime");
        // Must remain span-based (no owned vector field on EnvView).
        CHECK(ixx.find("string_bindings_owned") == std::string::npos,
              "no defensive owned copy field");
    }

    // ── AC2: sequential zero-copy view ──
    {
        std::println("\n--- AC2: sequential make_env_view + lookup ---");
        Env env;
        env.bind("foo", make_int(42));
        env.bind("bar", make_int(7));
        auto view = make_env_view(env);
        CHECK(view.size() == 2, "two string bindings");
        auto foo = view.lookup("foo");
        CHECK(foo && is_int(*foo) && as_int(*foo) == 42, "lookup foo");
        auto bar = view.lookup("bar");
        CHECK(bar && is_int(*bar) && as_int(*bar) == 7, "lookup bar");
        auto miss = view.lookup("nope");
        CHECK(!miss.has_value(), "missing → nullopt");
        // Parent walk.
        Env child;
        child.set_parent(&env);
        auto cv = make_env_view(child);
        auto via = cv.lookup("foo");
        CHECK(via && is_int(*via) && as_int(*via) == 42, "parent walk via view");
    }

    // ── AC3: remake after bind (supported pattern) ──
    {
        std::println("\n--- AC3: remake view after bind ---");
        Env env;
        env.bind("a", make_int(1));
        auto v1 = make_env_view(env);
        CHECK(v1.lookup("a") && as_int(*v1.lookup("a")) == 1, "v1 sees a");
        // Contract: discard v1 before mutating; remake.
        for (int i = 0; i < 64; ++i)
            env.bind(std::string("k") + std::to_string(i), make_int(i));
        auto v2 = make_env_view(env);
        CHECK(v2.size() >= 65, "v2 sees grown bindings");
        auto k63 = v2.lookup("k63");
        CHECK(k63 && is_int(*k63) && as_int(*k63) == 63, "v2 lookup k63");
        // Still finds original.
        CHECK(v2.lookup("a") && as_int(*v2.lookup("a")) == 1, "v2 still finds a");
    }

    std::println("\n=== test_env_view_lifetime_1868: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
