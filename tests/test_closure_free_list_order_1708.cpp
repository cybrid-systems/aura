// @category: unit
// @reason: Issue #1708 — aura_free_closure must push free_list before
// setting g_closure_freed[cid]=1 so a throw mid-pair cannot leak slots.
//
//   AC1: free then alloc reuses a slot (reuse_total increases or id recycled)
//   AC2: free_list order in source is push then freed=1
//   AC3: double-free is idempotent
//   AC4: source cites #1708

#include "test_harness.hpp"

#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

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
    // ── AC1: free → alloc reuses ──
    {
        std::println("\n--- AC1: free then alloc reuses slot ---");
        const auto reuse_before = aura_closure_reuse_total();
        const auto free_before = aura_closure_free_total();
        auto c1 = aura_alloc_closure(42);
        CHECK(c1 >= 0, "alloc c1");
        aura_free_closure(c1);
        CHECK(aura_closure_is_freed(c1) == 1, "c1 freed");
        CHECK(aura_closure_free_total() > free_before, "free_total bumped");
        auto c2 = aura_alloc_closure(43);
        CHECK(c2 >= 0, "alloc c2");
        // Prefer exact reuse of c1 when free_list non-empty.
        CHECK(c2 == c1 || aura_closure_reuse_total() > reuse_before,
              "slot reused (same id or reuse_total++)");
        if (c2 == c1)
            CHECK(aura_closure_is_freed(c2) == 0, "reused slot not marked freed");
        aura_free_closure(c2);
    }

    // ── AC2/AC4: source order ──
    {
        std::println("\n--- AC2/AC4: source push before freed=1 ---");
        const char* candidates[] = {
            "src/compiler/aura_jit_runtime.cpp",
            "../src/compiler/aura_jit_runtime.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read aura_jit_runtime.cpp");
        if (!src.empty()) {
            CHECK(src.find("Issue #1708") != std::string::npos, "cites #1708");
            auto pos_fn = src.find("void aura_free_closure");
            CHECK(pos_fn != std::string::npos, "found aura_free_closure");
            if (pos_fn != std::string::npos) {
                // Look only in the free function body (~80 lines).
                auto win = src.substr(pos_fn, 2500);
                auto p_push = win.find("g_closure_free_list.push_back(cid)");
                auto p_freed = win.find("g_closure_freed[cid] = 1");
                CHECK(p_push != std::string::npos, "has free_list.push_back");
                CHECK(p_freed != std::string::npos, "has freed[cid]=1");
                if (p_push != std::string::npos && p_freed != std::string::npos)
                    CHECK(p_push < p_freed, "push_back precedes freed=1");
            }
        }
    }

    // ── AC3: double free ──
    {
        std::println("\n--- AC3: double free idempotent ---");
        auto c = aura_alloc_closure(7);
        CHECK(c >= 0, "alloc for double-free");
        aura_free_closure(c);
        auto free_mid = aura_closure_free_total();
        aura_free_closure(c); // second free must not re-push / double-count
        CHECK(aura_closure_free_total() == free_mid, "double free does not re-bump free_total");
        CHECK(aura_closure_is_freed(c) == 1, "still freed");
    }

    std::println("\n=== test_closure_free_list_order_1708: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
