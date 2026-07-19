// @category: unit
// @reason: Issue #1772 — mutate:from-verification-feedback must reject
// OOB/negative NodeId before eda:* delegation and bump
// mutate_from_feedback_invalid_node_total.
//
//   AC1: source cites #1772; invalid_node metric bump present
//   AC2: metric field declared
//   AC3: OOB node_id → #f + counter +1
//   AC4: valid node_id does not bump invalid counter (may still fail eda:*)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
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
    // ── AC1/AC2 ──
    {
        std::println("\n--- AC1/AC2: source + metric field ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_04.cpp",
                              "../src/compiler/evaluator_primitives_compile_04.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_04.cpp");
        CHECK(src.find("#1772") != std::string::npos, "cites #1772");
        CHECK(src.find("mutate_from_feedback_invalid_node_total") != std::string::npos,
              "bumps invalid_node metric");
        CHECK(src.find("mutate:from-verification-feedback") != std::string::npos,
              "primitive present");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() &&
                  msrc.find("mutate_from_feedback_invalid_node_total") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: OOB ──
    {
        std::println("\n--- AC3: OOB node_id rejected ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code seeds workspace");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        const auto n0 = m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed);

        auto r =
            cs.eval("(mutate:from-verification-feedback \"weaken-property\" 999999 \"reset\")");
        CHECK(r && is_bool(*r) && !as_bool(*r), "OOB returns #f");
        CHECK(m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed) == n0 + 1,
              "invalid_node_total +1");

        auto r2 = cs.eval("(mutate:from-verification-feedback \"weaken-property\" -1 \"reset\")");
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "negative returns #f");
        CHECK(m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed) == n0 + 2,
              "invalid_node_total +2");
    }

    // ── AC4: in-range does not bump invalid ──
    {
        std::println("\n--- AC4: in-range node_id no invalid bump ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code seeds workspace");
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        auto* ws = ev.workspace_flat();
        if (!ws || ws->size() == 0) {
            CHECK(false, "workspace non-empty after set-code");
        } else {
            const auto n0 =
                m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed);
            const auto nid = static_cast<std::int64_t>(ws->size() - 1);
            auto r = cs.eval(std::format(
                "(mutate:from-verification-feedback \"weaken-property\" {} \"reset\")", nid));
            CHECK(r && is_bool(*r), "returns bool");
            CHECK(m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed) == n0,
                  "no invalid bump for in-range id");
        }
    }

    std::println("\n=== test_mutate_from_feedback_node_id_1772: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
