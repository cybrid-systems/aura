// @category: unit
// @reason: Issue #1733 — walk_active_closures must isolate callback
// exceptions and continue enumeration.
//
//   AC1: source cites #1733; per-callback catch + metric
//   AC2: metric field walk_active_closures_callback_exceptions
//   AC3: throwing callback does not abort walk; later closures visited
//   AC4: successful walk does not bump exception metric

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
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
    // ── AC1/AC2: source + metric ──
    {
        std::println("\n--- AC1/AC2: catch isolation + metric ---");
        std::string env_cpp;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            env_cpp = read_file(p);
            if (!env_cpp.empty())
                break;
        }
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(env_cpp.find("#1733") != std::string::npos, "cites #1733");
        CHECK(env_cpp.find("walk_active_closures_callback_exceptions") != std::string::npos,
              "bumps exception metric");
        CHECK(env_cpp.find("catch (const std::exception&") != std::string::npos,
              "catches std::exception");
        CHECK(env_cpp.find("catch (...)") != std::string::npos, "catches ...");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() &&
                  msrc.find("walk_active_closures_callback_exceptions") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: throw mid-walk continues ──
    {
        std::println("\n--- AC3: throw mid-walk continues enumeration ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");

        // Register several closures.
        std::vector<aura::compiler::ClosureId> ids;
        for (int i = 0; i < 5; ++i) {
            Closure cl;
            cl.env_id = NULL_ENV_ID;
            ids.push_back(ev.register_active_closure(std::move(cl)));
        }
        CHECK(ids.size() == 5, "registered 5 closures");

        const auto ex0 =
            m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
        int visited = 0;
        int throw_at = 2; // throw on 3rd visit (0-based)
        bool saw_after_throw = false;
        int visit_index = 0;

        ev.walk_active_closures([&](auto id, auto& /*cl*/) {
            (void)id;
            if (visit_index == throw_at) {
                ++visit_index;
                throw std::runtime_error("walk-callback-test");
            }
            ++visited;
            if (visit_index > throw_at)
                saw_after_throw = true;
            ++visit_index;
        });

        const auto ex1 =
            m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
        CHECK(ex1 == ex0 + 1, "exception metric +1");
        CHECK(visited >= 4, "visited remaining closures after throw");
        CHECK(saw_after_throw || visited >= 4, "walk continued past throw site");
    }

    // ── AC4: clean walk no metric bump ──
    {
        std::println("\n--- AC4: clean walk does not bump exception metric ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        Closure cl;
        cl.env_id = NULL_ENV_ID;
        (void)ev.register_active_closure(std::move(cl));

        const auto ex0 =
            m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
        int n = 0;
        ev.walk_active_closures([&](auto, auto&) { ++n; });
        const auto ex1 =
            m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
        CHECK(n >= 1, "visited at least one");
        CHECK(ex1 == ex0, "no exception metric bump on clean walk");
    }

    std::println("\n=== test_walk_active_closures_ex_1733: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
