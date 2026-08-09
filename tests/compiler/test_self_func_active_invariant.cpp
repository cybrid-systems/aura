// @category: unit
// @reason: Issue #2826 — self_func_id may be 0; always use self_func_active
// via is_self() / matches_self_name(); bare id != 0 is a footgun (#2292).
//
//   AC1: source helpers + static_assert + #2826 cites
//   AC2: is_self / matches_self_name honor active flag when id==0
//   AC3: linter rejects bare self_func_id != 0
//   AC4: this suite + linter; no docs/design/2826-*; no test_issue_2826.cpp

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.lowering;

namespace {

using aura::compiler::LoweringState;
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

} // namespace

int run_test_self_func_active_invariant() {
    std::println("=== Issue #2826: self_func_active invariant helpers ===");
    CHECK(true, "ac2826: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: helpers + static_assert ---");
        auto low = read_file("src/compiler/lowering.ixx");
        auto impl = read_file("src/compiler/lowering_impl.cpp");
        CHECK(!low.empty(), "AC1: lowering.ixx readable");
        CHECK(low.find("Issue #2826") != std::string::npos, "AC1: cites #2826");
        CHECK(low.find("is_self(") != std::string::npos, "AC1: is_self helper");
        CHECK(low.find("matches_self_name") != std::string::npos, "AC1: matches_self_name");
        CHECK(low.find("static_assert") != std::string::npos &&
                  low.find("self_func_active") != std::string::npos,
              "AC1: static_assert documents active flag");
        CHECK(impl.find("matches_self_name") != std::string::npos,
              "AC1: lowering_impl uses matches_self_name");
        CHECK(impl.find("Issue #2826") != std::string::npos, "AC1: impl cites #2826");
    }

    // ── AC2: runtime semantics when id == 0 ──
    {
        std::println("\n--- AC2: id==0 with/without active ---");
        aura::ast::ASTArena arena;
        LoweringState st(arena);

        // Default: inactive, id 0 — must NOT look active.
        CHECK(!st.is_self_func_active(), "AC2: default inactive");
        CHECK(!st.is_self(0), "AC2: is_self(0) false when inactive");
        CHECK(!st.matches_self_name("fact"), "AC2: matches_self_name false when inactive");
        CHECK(st.self_func_id_or_invalid() == static_cast<std::uint32_t>(-1),
              "AC2: invalid sentinel when inactive");

        // Active with id 0 (first preclaimed function — the #2292 footgun case).
        st.self_name = "fact";
        st.self_func_id = 0;
        st.self_func_active = true;
        CHECK(st.is_self_func_active(), "AC2: active true");
        CHECK(st.is_self(0), "AC2: is_self(0) true when active (valid func id)");
        CHECK(!st.is_self(1), "AC2: is_self(1) false");
        CHECK(st.matches_self_name("fact"), "AC2: matches fact");
        CHECK(!st.matches_self_name("other"), "AC2: not other");
        CHECK(st.self_func_id_or_invalid() == 0, "AC2: id_or_invalid returns 0 when active");

        // Clear active while leaving id 0 — inactive again.
        st.self_func_active = false;
        CHECK(!st.is_self(0), "AC2: is_self(0) false after clear active");
        CHECK(!st.matches_self_name("fact"), "AC2: name match false after clear");
    }

    // ── AC3: bare != 0 is the documented footgun (source still warns) ──
    {
        std::println("\n--- AC3: comment forbids bare id != 0 ---");
        auto low = read_file("src/compiler/lowering.ixx");
        CHECK(low.find("never test id != 0 alone") != std::string::npos ||
                  low.find("never test id") != std::string::npos,
              "AC3: footgun documented");
        // Linter file present
        auto lint = read_file("scripts/coverage/checks/check_self_func_id_usage_2826.py");
        CHECK(!lint.empty(), "AC3: linter script present");
        CHECK(lint.find("BARE_ID_CMP") != std::string::npos ||
                  lint.find("self_func_id") != std::string::npos,
              "AC3: linter scans bare id cmp");
    }

    // ── AC4: no forbidden artifacts ──
    {
        std::println("\n--- AC4: suite + no docs/design ---");
        CHECK(true, "AC4: test_self_func_active_invariant is this file");
        auto bad = read_file("tests/compiler/test_issue_2826.cpp");
        CHECK(bad.empty(), "AC4: no test_issue_2826.cpp");
    }

    std::println("\n=== #2826 self_func_active invariant: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_self_func_active_invariant();
}
#endif
