// @category: unit
// @reason: Issue #1739 — truncate_env_frames_to_checkpoint must bump
// bridge_epoch so cross-COW closure freshness checks observe the
// truncated env_frames_ arena (same bug class as #1728).
//
//   AC1: source cites #1739; truncate calls bridge_epoch_bump_fn_
//   AC2: truncate that drops frames advances current_bridge_epoch()
//   AC3: truncate that drops frames bumps metrics.bridge_epoch_bumps_total
//   AC4: no-op truncate (nothing past checkpoint) does not bump epoch

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

std::string truncate_window(const std::string& src) {
    auto pos = src.find("Evaluator::truncate_env_frames_to_checkpoint()");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("\nconst EnvFrame* Evaluator::resolve_env_frame", pos);
    if (end == std::string::npos)
        end = src.find("\nEnvFrame* Evaluator::resolve_env_frame_mut", pos);
    return src.substr(pos, end == std::string::npos ? 1600 : end - pos);
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: truncate wires bridge bump ---");
        std::string env;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            env = read_file(p);
            if (!env.empty())
                break;
        }
        CHECK(!env.empty(), "read evaluator_env.cpp");
        CHECK(env.find("#1739") != std::string::npos, "cites #1739");
        auto win = truncate_window(env);
        CHECK(!win.empty(), "found truncate_env_frames_to_checkpoint");
        CHECK(win.find("bridge_epoch_bump_fn_") != std::string::npos,
              "calls bridge_epoch_bump_fn_");
        // No-op early return must not bump (bump is after resize path).
        auto early = win.find("return 0;");
        CHECK(early != std::string::npos, "has early no-op return");
        if (early != std::string::npos) {
            auto before_bump = win.substr(0, early + 10);
            // Early path ends at first return 0; bump should appear later.
            auto bump_pos = win.find("bridge_epoch_bump_fn_");
            CHECK(bump_pos != std::string::npos && bump_pos > early,
                  "bump is after early-return path");
        }
    }

    // ── AC2/AC3: truncate that drops frames advances epoch + metric ──
    {
        std::println("\n--- AC2/AC3: truncate drops → epoch + metric ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        for (int i = 0; i < 5; ++i)
            (void)ev.alloc_env_frame();
        const std::size_t base = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base);
        for (int i = 0; i < 8; ++i)
            (void)ev.alloc_env_frame();

        const auto e0 = ev.current_bridge_epoch();
        const auto m0 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        const auto aot0 = aura_get_current_bridge_epoch();

        const std::size_t dropped = ev.truncate_env_frames_to_checkpoint();
        CHECK(dropped == 8, "dropped 8 frames");

        const auto e1 = ev.current_bridge_epoch();
        const auto m1 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        const auto aot1 = aura_get_current_bridge_epoch();
        CHECK(e1 == e0 + 1, "current_bridge_epoch advanced by 1");
        CHECK(m1 == m0 + 1, "bridge_epoch_bumps_total +1");
        CHECK(aot1 == aot0 + 1 || aot1 == e1, "AOT current_bridge_epoch lockstep");
    }

    // ── AC4: no-op truncate does not bump ──
    {
        std::println("\n--- AC4: no-op truncate does not bump ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        for (int i = 0; i < 3; ++i)
            (void)ev.alloc_env_frame();
        const std::size_t base = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base);

        const auto e0 = ev.current_bridge_epoch();
        const auto m0 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);

        CHECK(ev.truncate_env_frames_to_checkpoint() == 0, "no-op drops 0");

        const auto e1 = ev.current_bridge_epoch();
        const auto m1 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        CHECK(e1 == e0, "no-op does not advance epoch");
        CHECK(m1 == m0, "no-op does not bump metric");
    }

    std::println("\n=== test_truncate_env_bridge_epoch_1739: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
