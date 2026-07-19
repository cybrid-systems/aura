// @category: unit
// @reason: Issue #1863 — Env::bindings_with_names walks bindings_symid_
// and calls pool_->resolve without a shared_lock; concurrent bind is
// unsupported under the #1861 single-writer contract.
//
//   AC1: source cites #1863 on bindings_with_names; no env_symid_mtx_
//   AC2: sequential bind_symid + pool → named view matches
//   AC3: no-pool fallback @symid:N; empty env → empty vector

#include "test_harness.hpp"

#include <algorithm>
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
        std::println("\n--- AC1: #1863 bindings_with_names single-writer docs ---");
        auto env_cpp =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        auto env_ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(!env_ixx.empty(), "read evaluator.ixx");
        CHECK(env_cpp.find("#1863") != std::string::npos, "cites #1863");
        auto pos = env_cpp.find("Env::bindings_with_names");
        CHECK(pos != std::string::npos, "bindings_with_names present");
        auto win = env_cpp.substr(pos > 600 ? pos - 600 : 0, 1400);
        CHECK(win.find("#1863") != std::string::npos, "window cites #1863");
        CHECK(win.find("single-writer") != std::string::npos ||
                  win.find("#1861") != std::string::npos,
              "ties to single-writer / #1861");
        CHECK(win.find("env_symid_mtx_") != std::string::npos ||
                  win.find("no shared_lock") != std::string::npos ||
                  win.find("unlocked") != std::string::npos,
              "documents no lock");
        CHECK(win.find("resolve") != std::string::npos, "mentions pool resolve");
        CHECK(env_ixx.find("env_symid_mtx_{") == std::string::npos &&
                  env_ixx.find("env_symid_mtx_;") == std::string::npos &&
                  env_ixx.find("mutex env_symid") == std::string::npos,
              "no env_symid_mtx_ member declaration");
        CHECK(env_ixx.find("#1863") != std::string::npos, "Env class cites #1863 sibling");
    }

    // ── AC2: sequential bind + named view ──
    {
        std::println("\n--- AC2: sequential bind_symid → bindings_with_names ---");
        StringPool pool;
        auto sx = pool.intern("alpha");
        auto sy = pool.intern("beta");
        Env env;
        env.set_pool(&pool);
        env.bind_symid(sx, make_int(1));
        env.bind_symid(sy, make_int(2));

        auto named = env.bindings_with_names();
        CHECK(named.size() == 2, "two named bindings");
        // Order matches bindings_symid_ (bind order).
        CHECK(named[0].first == "alpha", "first name alpha");
        CHECK(named[1].first == "beta", "second name beta");
        CHECK(is_int(named[0].second) && as_int(named[0].second) == 1, "alpha value 1");
        CHECK(is_int(named[1].second) && as_int(named[1].second) == 2, "beta value 2");

        // After more binds (vector growth), still consistent.
        for (int i = 0; i < 40; ++i) {
            auto n = std::string("v") + std::to_string(i);
            env.bind_symid(pool.intern(n), make_int(100 + i));
        }
        named = env.bindings_with_names();
        CHECK(named.size() == 42, "42 after growth");
        auto it = std::find_if(named.begin(), named.end(),
                               [](const auto& p) { return p.first == "v39"; });
        CHECK(it != named.end(), "finds v39");
        if (it != named.end())
            CHECK(is_int(it->second) && as_int(it->second) == 139, "v39 value 139");
    }

    // ── AC3: no-pool fallback + empty ──
    {
        std::println("\n--- AC3: no-pool @symid fallback; empty env ---");
        Env empty;
        CHECK(empty.bindings_with_names().empty(), "empty env → empty vector");

        Env env;
        // Synthetic SymIds without pool — fallback labels.
        env.bind_symid(/*s=*/7, make_int(70));
        env.bind_symid(/*s=*/9, make_int(90));
        auto named = env.bindings_with_names();
        CHECK(named.size() == 2, "two fallback names");
        CHECK(named[0].first == "@symid:7", "fallback @symid:7");
        CHECK(named[1].first == "@symid:9", "fallback @symid:9");
        CHECK(is_int(named[0].second) && as_int(named[0].second) == 70, "value 70");
        CHECK(is_int(named[1].second) && as_int(named[1].second) == 90, "value 90");
    }

    std::println("\n=== test_env_bindings_with_names_race_1863: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
