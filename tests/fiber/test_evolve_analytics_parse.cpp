// @category: unit
// @reason: Issue #1724 — evolve-strategy must not silently swallow
// Issue #1724 (#1978 renamed): issue# moved from filename to header.
// std::stod/stoi analytics parse failures (#1669 pattern).
//
//   AC1: source cites #1724; catch (const std::exception&) + metric bump
//   AC2: agent_evolve_analytics_parse_failures field declared
//   AC3: malformed success-rate bumps metric; evolve still returns
//   AC4: well-formed analytics does not bump metric; heuristics apply

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1/AC2: source + metric field ──
    {
        std::println("\n--- AC1/AC2: catch narrow + metric field ---");
        std::string agent;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            agent = read_file(p);
            if (!agent.empty())
                break;
        }
        CHECK(!agent.empty(), "read agent");
        CHECK(agent.find("#1724") != std::string::npos, "cites #1724");
        CHECK(agent.find("agent_evolve_analytics_parse_failures") != std::string::npos,
              "bumps parse-failure metric");
        CHECK(agent.find("catch (const std::exception&") != std::string::npos,
              "narrows catch to std::exception");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() &&
                  msrc.find("agent_evolve_analytics_parse_failures") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: malformed rates → metric bump, no crash ──
    {
        std::println("\n--- AC3: malformed success-rate bumps metric ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "compiler_metrics wired");
        const auto m0 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);

        auto def =
            cs.eval(R"AURA((define-strategy "s1724bad" "(lambda (x) x)" :max-attempts 3))AURA");
        CHECK(static_cast<bool>(def), "define-strategy s1724bad");

        auto evo = cs.eval(
            R"AURA((evolve-strategy "s1724bad" "#(analytics total-runs:10 success-rate:not-a-number avg-attempts:xyz total-llm-calls:1 avg-duration-ms:1 top-errors:() by-task:())"))AURA");
        CHECK(static_cast<bool>(evo), "evolve with malformed rates returns");

        const auto m1 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);
        CHECK(m1 > m0, "agent_evolve_analytics_parse_failures bumped");

        // Defaults kept (0.0) → no low-success bump (would need rate<0.5 and avg>=3).
        auto field = cs.eval(R"AURA((strategy-field "s1724bad-v1" "max-attempts"))AURA");
        CHECK(field.has_value() && is_int(*field), "evolved strategy has max-attempts");
        if (field && is_int(*field))
            CHECK(as_int(*field) == 3, "malformed rates keep max-attempts unchanged (clone)");
    }

    // ── AC4: well-formed rates → no extra bump; heuristics work ──
    {
        std::println("\n--- AC4: well-formed analytics, no metric bump ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto m0 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);

        auto def =
            cs.eval(R"AURA((define-strategy "s1724ok" "(lambda (x) x)" :max-attempts 3))AURA");
        CHECK(static_cast<bool>(def), "define-strategy s1724ok");

        auto evo = cs.eval(
            R"AURA((evolve-strategy "s1724ok" "#(analytics total-runs:10 success-rate:0.2 avg-attempts:3 total-llm-calls:1 avg-duration-ms:1 top-errors:() by-task:())"))AURA");
        CHECK(static_cast<bool>(evo), "evolve well-formed analytics");

        const auto m1 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);
        CHECK(m1 == m0, "well-formed path does not bump parse failures");

        auto field = cs.eval(R"AURA((strategy-field "s1724ok-v1" "max-attempts"))AURA");
        CHECK(field.has_value() && is_int(*field), "ok max-attempts is int");
        if (field && is_int(*field))
            CHECK(as_int(*field) == 5, "low success-rate → max-attempts bumped to 5");
    }

    std::println("\n=== test_evolve_analytics_parse_1724: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
