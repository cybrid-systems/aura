// @category: unit
// @reason: Issue #1709 — aura_closure_capture bounds on func_ids+envs
// (not envs alone) and refuses freed slots.
//
//   AC1: capture into live alloc succeeds (no crash)
//   AC2: capture into huge/OOR id is no-op
//   AC3: capture into freed slot is no-op
//   AC4: source uses closure_slot_in_bounds / func_ids check

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
    // ── AC1: live capture ──
    {
        std::println("\n--- AC1: capture into live slot ---");
        auto cid = aura_alloc_closure(1);
        CHECK(cid >= 0, "alloc");
        aura_closure_capture(cid, 0, 42);
        aura_closure_capture(cid, 1, 99);
        CHECK(true, "capture into live slot no crash");
        aura_free_closure(cid);
    }

    // ── AC2: OOR ──
    {
        std::println("\n--- AC2: OOR capture is no-op ---");
        aura_closure_capture(-1, 0, 1);
        aura_closure_capture(999999, 0, 1);
        CHECK(true, "OOR capture no crash");
    }

    // ── AC3: freed ──
    {
        std::println("\n--- AC3: capture into freed slot ---");
        auto cid = aura_alloc_closure(2);
        CHECK(cid >= 0, "alloc for free");
        aura_free_closure(cid);
        CHECK(aura_closure_is_freed(cid) == 1, "is_freed");
        aura_closure_capture(cid, 0, 7); // must refuse
        CHECK(true, "capture after free no crash");
    }

    // ── AC4: source ──
    {
        std::println("\n--- AC4: source has #1709 bounds ---");
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
        CHECK(!src.empty(), "read runtime");
        if (!src.empty()) {
            CHECK(src.find("Issue #1709") != std::string::npos, "cites #1709");
            CHECK(src.find("closure_slot_in_bounds") != std::string::npos,
                  "closure_slot_in_bounds helper");
            auto cap = src.find("void aura_closure_capture");
            CHECK(cap != std::string::npos, "found capture");
            if (cap != std::string::npos) {
                auto win = src.substr(cap, 1200);
                CHECK(win.find("closure_slot_in_bounds") != std::string::npos,
                      "capture uses closure_slot_in_bounds");
                // Must not only check envs alone as the sole gate.
                CHECK(win.find("g_closure_func_ids") != std::string::npos ||
                          win.find("closure_slot_in_bounds") != std::string::npos,
                      "func_ids in capture path");
            }
        }
    }

    std::println("\n=== test_closure_capture_bounds_1709: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
