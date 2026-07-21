// @category: unit
// @reason: Issue #1728 — commit_panic_checkpoint must bump bridge_epoch
// Issue #1728 (#1978 renamed): issue# moved from filename to header.
// so cross-COW closure freshness checks observe the commit.
//
//   AC1: source cites #1728; commit calls bridge_epoch_bump_fn_
//   AC2: commit advances current_bridge_epoch()
//   AC3: commit bumps metrics.bridge_epoch_bumps_total
//   AC4: clear_panic_checkpoint alone does not bump epoch

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string commit_window(const std::string& src) {
    auto pos = src.find("void commit_panic_checkpoint()");
    if (pos == std::string::npos)
        return {};
    // Capture method body until next top-level-ish method.
    auto end = src.find("\n    // Check if a safe checkpoint", pos);
    if (end == std::string::npos)
        end = src.find("\n    bool has_panic_checkpoint", pos);
    return src.substr(pos, end == std::string::npos ? 1200 : end - pos);
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: commit_panic_checkpoint wires bridge bump ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1728") != std::string::npos, "cites #1728");
        auto win = commit_window(ixx);
        CHECK(!win.empty(), "found commit_panic_checkpoint");
        CHECK(win.find("bridge_epoch_bump_fn_") != std::string::npos,
              "calls bridge_epoch_bump_fn_");
        // clear path must NOT include the bump (only commit does).
        auto cpos = ixx.find("void clear_panic_checkpoint()");
        CHECK(cpos != std::string::npos, "has clear_panic_checkpoint");
        if (cpos != std::string::npos) {
            auto cend = ixx.find("void commit_panic_checkpoint()", cpos);
            auto cwin = ixx.substr(cpos, cend == std::string::npos ? 800 : cend - cpos);
            CHECK(cwin.find("bridge_epoch_bump_fn_") == std::string::npos,
                  "clear_panic_checkpoint does not bump bridge_epoch");
        }
    }

    // ── AC2/AC3: commit advances epoch + metric ──
    {
        std::println("\n--- AC2/AC3: commit bumps epoch + metric ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto e0 = ev.current_bridge_epoch();
        const auto m0 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        const auto aot0 = aura_get_current_bridge_epoch();

        ev.commit_panic_checkpoint();

        const auto e1 = ev.current_bridge_epoch();
        const auto m1 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        const auto aot1 = aura_get_current_bridge_epoch();
        CHECK(e1 == e0 + 1, "current_bridge_epoch advanced by 1");
        CHECK(m1 == m0 + 1, "bridge_epoch_bumps_total +1");
        CHECK(aot1 == aot0 + 1 || aot1 == e1, "AOT current_bridge_epoch lockstep");
    }

    // ── AC4: clear alone does not bump ──
    {
        std::println("\n--- AC4: clear_panic_checkpoint does not bump ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto e0 = ev.current_bridge_epoch();
        const auto m0 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);

        ev.clear_panic_checkpoint();

        const auto e1 = ev.current_bridge_epoch();
        const auto m1 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        CHECK(e1 == e0, "clear does not advance epoch");
        CHECK(m1 == m0, "clear does not bump metric");
    }

    std::println("\n=== test_commit_panic_bridge_epoch_1728: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
