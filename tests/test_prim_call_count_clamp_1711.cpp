// @category: unit
// @reason: Issue #1711 — aura_prim_call must clamp argc to the 3-element
// stack args[] buffer so the dispatcher cannot read past the array.
//
//   AC1: dispatcher sees argc<=3 even when caller passes count>3
//   AC2: count<=3 still forwarded as-is
//   AC3: source clamps safe_count to 3
//   AC4: negative count treated as 0

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

// Mirrors aura_jit_runtime.cpp / aura_jit.cpp declarations.
extern "C" void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t));
extern "C" int64_t aura_prim_call(int64_t slot, int64_t a, int64_t b, int64_t count);

static int32_t g_last_argc = -1;
static int64_t g_last_a0 = 0;
static int64_t g_last_a1 = 0;
static int64_t g_last_a2 = 0;

static int64_t test_dispatcher(int64_t /*slot*/, int64_t* args, int32_t argc) {
    g_last_argc = argc;
    // Only read up to argc slots, and never past 3 (dispatcher contract).
    g_last_a0 = (argc > 0) ? args[0] : 0;
    g_last_a1 = (argc > 1) ? args[1] : 0;
    g_last_a2 = (argc > 2) ? args[2] : 0;
    return 42;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    aura_set_prim_dispatcher(test_dispatcher);

    // ── AC1: count > 3 clamped ──
    {
        std::println("\n--- AC1: count>3 clamped to 3 ---");
        g_last_argc = -1;
        auto r = aura_prim_call(/*slot=*/1, /*a=*/10, /*b=*/20, /*count=*/99);
        CHECK(r == 42, "dispatcher returned 42");
        CHECK(g_last_argc == 3, "argc clamped to 3");
        CHECK(g_last_a0 == 10 && g_last_a1 == 20, "a/b forwarded");
    }

    // ── AC2: count in range ──
    {
        std::println("\n--- AC2: count=2 forwarded ---");
        g_last_argc = -1;
        (void)aura_prim_call(2, 7, 8, 2);
        CHECK(g_last_argc == 2, "argc=2 preserved");
        CHECK(g_last_a0 == 7 && g_last_a1 == 8, "args 7,8");
    }

    // ── AC4: negative count ──
    {
        std::println("\n--- AC4: negative count → 0 ---");
        g_last_argc = -1;
        (void)aura_prim_call(3, 1, 2, -5);
        CHECK(g_last_argc == 0, "negative count clamped to 0");
    }

    // ── AC3: source ──
    {
        std::println("\n--- AC3: source clamps to 3 ---");
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
            CHECK(src.find("Issue #1711") != std::string::npos, "cites #1711");
            auto pos = src.find("int64_t aura_prim_call");
            CHECK(pos != std::string::npos, "found aura_prim_call");
            if (pos != std::string::npos) {
                auto win = src.substr(pos, 1200);
                CHECK(win.find("safe_count") != std::string::npos, "safe_count present");
                CHECK(win.find("safe_count > 3") != std::string::npos ||
                          win.find("safe_count = 3") != std::string::npos,
                      "clamps to 3");
            }
        }
    }

    // Detach dispatcher so later tests/process are clean.
    aura_set_prim_dispatcher(nullptr);

    std::println("\n=== test_prim_call_count_clamp_1711: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
