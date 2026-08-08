// @category: unit
// @reason: Issue #2787 — workspace:rollback-latest single reverse walk
// (no O(N) second search by mutation_id; field path uses try_rollback_record).
//
//   AC1: source has no second all_mutations() ID walk inside rollback-latest
//   AC2: source uses try_rollback_record (not rollback(mid) re-search)
//   AC3: sequential field rollback undoes N commits (latest-first)
//   AC4: empty / no-committed returns 0
//   AC5: this suite + linter; no docs/design/2787-*; no test_issue_2787.cpp

#include "test_harness.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::string rollback_window(const std::string& src) {
    auto pos = src.find("workspace:rollback-latest");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("workspace:mutation-count", pos);
    if (end == std::string::npos)
        end = pos + 2500;
    return src.substr(pos, end - pos);
}

} // namespace

int run_test_workspace_rollback_latest() {
    std::println("=== Issue #2787: workspace:rollback-latest single-walk ===");
    CHECK(true, "ac2787: issue stamp");

    // ── AC1/AC2: source shape ──
    {
        std::println("\n--- AC1/AC2: no second ID walk; try_rollback_record ---");
        auto src = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(!src.empty(), "AC1: workspace primitives readable");
        auto win = rollback_window(src);
        CHECK(!win.empty(), "AC1: rollback-latest present");
        CHECK(win.find("Issue #2787") != std::string::npos, "AC1: cites #2787");
        CHECK(win.find("try_rollback_record") != std::string::npos,
              "AC2: uses try_rollback_record");
        CHECK(win.find("r.mutation_id == it->mutation_id") == std::string::npos,
              "AC1: no second-walk by it->mutation_id");
        // Old field path used rollback(mid) which re-walks by id.
        CHECK(win.find("->rollback(") == std::string::npos &&
                  win.find(".rollback(") == std::string::npos,
              "AC2: no rollback(mid) re-search");
        CHECK(win.find("for (std::size_t ri") != std::string::npos,
              "AC1: index reverse walk present");
    }

    // ── AC3: live sequential field rollback ──
    {
        std::println("\n--- AC3: live workspace:rollback-latest sequential ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 0)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        (void)cs.eval("(workspace :create \"rb-2787\")");

        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "AC3: workspace flat");
        aura::ast::NodeId lit = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (!flat->is_live_node(id))
                continue;
            if (flat->tag(id) == aura::ast::NodeTag::LiteralInt) {
                lit = id;
                break;
            }
        }
        CHECK(lit != aura::ast::NULL_NODE, "AC3: found LiteralInt");

        constexpr int N = 64;
        for (int i = 1; i <= N; ++i) {
            auto r = cs.eval(std::format("(mutate:replace-value {} {} \"ac2787-mut\")", lit, i));
            CHECK(r.has_value(), std::format("AC3: mutate {}", i).c_str());
        }
        CHECK(static_cast<int>(flat->int_val(lit)) == N, "AC3: value is N");

        const auto t0 = std::chrono::steady_clock::now();
        int rolled = 0;
        for (;;) {
            auto r = cs.eval("(workspace:rollback-latest)");
            if (!r || !is_int(*r) || as_int(*r) == 0)
                break;
            ++rolled;
            if (rolled > N + 5)
                break; // safety
        }
        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        CHECK(rolled == N, "AC3: rolled back exactly N mutations");
        CHECK(static_cast<int>(flat->int_val(lit)) == 0, "AC3: value restored to 0");
        // Soft perf budget: O(N) path should finish well under 5s for N=64.
        CHECK(ms < 5000, "AC3: sequential rollback finishes under soft budget");
        std::println("  AC3: rolled {} in {} ms", rolled, ms);

        auto z = cs.eval("(workspace:rollback-latest)");
        CHECK(z && is_int(*z) && as_int(*z) == 0, "AC4: empty committed log returns 0");
    }

    // ── AC4: no workspace returns 0 ──
    {
        std::println("\n--- AC4: fresh service rollback returns 0 ---");
        CompilerService cs;
        auto z = cs.eval("(workspace:rollback-latest)");
        CHECK(z && is_int(*z) && as_int(*z) == 0, "AC4: no-flat returns 0");
    }

    std::println("\n=== #2787 rollback-latest: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_rollback_latest();
}
#endif
