// @category: unit
// @reason: Issue #1753 — walk_env_frames uses static_assert for F
// signature (not solely a C++20 requires clause).
//
//   AC1: source cites #1753; static_assert on invocable + bool return
//   AC2: no `requires aura::core::AuraInvocable` on walk_env_frames
//   AC3: happy-path walk visits parent chain
//   AC4: returning false stops the walk early

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::EnvFrame;
using aura::compiler::EnvId;
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

std::string walk_window(const std::string& src) {
    auto pos = src.find("void walk_env_frames(EnvId start, F&& f)");
    if (pos == std::string::npos)
        return {};
    // Include a bit of preceding comment for #1753 citation.
    auto begin = pos > 400 ? pos - 400 : 0;
    auto end = src.find("\n    // Introspection: number of frames", pos);
    if (end == std::string::npos)
        end = pos + 1200;
    return src.substr(begin, end - begin);
}

} // namespace

int main() {
    // ── AC1/AC2: source shape ──
    {
        std::println("\n--- AC1/AC2: static_assert, no requires on walk_env_frames ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        auto win = walk_window(ixx);
        CHECK(!win.empty(), "found walk_env_frames");
        CHECK(win.find("#1753") != std::string::npos, "cites #1753");
        CHECK(win.find("static_assert") != std::string::npos, "has static_assert");
        CHECK(win.find("is_invocable_v") != std::string::npos, "checks is_invocable_v");
        CHECK(win.find("is_convertible_v") != std::string::npos ||
                  win.find("invoke_result_t") != std::string::npos,
              "checks bool-convertible return");
        // The template must not use requires AuraInvocable on the signature.
        CHECK(win.find("requires aura::core::AuraInvocable") == std::string::npos,
              "no requires AuraInvocable on walk_env_frames");
    }

    // ── AC3: happy-path chain walk ──
    {
        std::println("\n--- AC3: parent chain visited ---");
        Evaluator ev;
        EnvId a = ev.alloc_env_frame(NULL_ENV_ID);
        EnvId b = ev.alloc_env_frame(a);
        EnvId c = ev.alloc_env_frame(b);
        CHECK(a != NULL_ENV_ID && b != NULL_ENV_ID && c != NULL_ENV_ID, "alloc frames");
        std::vector<EnvId> seen;
        ev.walk_env_frames(c, [&](EnvId id, const EnvFrame&) {
            seen.push_back(id);
            return true;
        });
        CHECK(seen.size() == 3, "visited 3 frames");
        if (seen.size() >= 3) {
            CHECK(seen[0] == c, "starts at c");
            CHECK(seen[1] == b, "then b");
            CHECK(seen[2] == a, "then a");
        }
        CHECK(ev.env_depth(c) == 3, "env_depth matches");
    }

    // ── AC4: early stop ──
    {
        std::println("\n--- AC4: false stops walk ---");
        Evaluator ev;
        EnvId a = ev.alloc_env_frame(NULL_ENV_ID);
        EnvId b = ev.alloc_env_frame(a);
        EnvId c = ev.alloc_env_frame(b);
        int n = 0;
        ev.walk_env_frames(c, [&](EnvId, const EnvFrame&) {
            ++n;
            return n < 2; // stop after 2 visits
        });
        CHECK(n == 2, "stopped after 2");
    }

    std::println("\n=== test_walk_env_frames_static_assert_1753: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
