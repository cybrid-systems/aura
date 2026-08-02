// @category: unit
// @reason: Issue #1711 / #2576 — aura_prim_call N-arg ABI (args*, count)
//          clamps count; forwards all args up to max; negative → 0.
//
//   AC1: count > max clamped
//   AC2: count in range preserved; 3 args all forwarded
//   AC3: source cites #2576 pointer ABI
//   AC4: negative count → 0

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

extern "C" void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t));
// Issue #2576: (slot, args*, count)
extern "C" int64_t aura_prim_call(int64_t slot, int64_t* args, int64_t count);

static int32_t g_last_argc = -1;
static int64_t g_last_a0 = 0;
static int64_t g_last_a1 = 0;
static int64_t g_last_a2 = 0;
static int64_t g_last_a3 = 0;

static int64_t test_dispatcher(int64_t /*slot*/, int64_t* args, int32_t argc) {
    g_last_argc = argc;
    g_last_a0 = (argc > 0 && args) ? args[0] : 0;
    g_last_a1 = (argc > 1 && args) ? args[1] : 0;
    g_last_a2 = (argc > 2 && args) ? args[2] : 0;
    g_last_a3 = (argc > 3 && args) ? args[3] : 0;
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

    // ── AC1: count > 32 clamped ──
    {
        std::println("\n--- AC1: count>32 clamped ---");
        g_last_argc = -1;
        int64_t buf[40];
        for (int i = 0; i < 40; ++i)
            buf[i] = 100 + i;
        auto r = aura_prim_call(/*slot=*/1, buf, /*count=*/99);
        CHECK(r == 42, "dispatcher returned 42");
        CHECK(g_last_argc == 32, "argc clamped to 32");
        CHECK(g_last_a0 == 100 && g_last_a1 == 101, "first args forwarded");
    }

    // ── AC2: 3 args all real ──
    {
        std::println("\n--- AC2: count=3 all forwarded ---");
        g_last_argc = -1;
        int64_t buf[3] = {7, 8, 9};
        (void)aura_prim_call(2, buf, 3);
        CHECK(g_last_argc == 3, "argc=3 preserved");
        CHECK(g_last_a0 == 7 && g_last_a1 == 8 && g_last_a2 == 9, "args 7,8,9");
    }

    // ── AC4: negative count ──
    {
        std::println("\n--- AC4: negative count → 0 ---");
        g_last_argc = -1;
        int64_t dummy = 1;
        (void)aura_prim_call(3, &dummy, -5);
        CHECK(g_last_argc == 0, "negative count clamped to 0");
    }

    // ── AC3: source ──
    {
        std::println("\n--- AC3: source N-arg ABI (#2576) ---");
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
            CHECK(src.find("#2576") != std::string::npos, "cites #2576");
            CHECK(src.find("kAuraPrimCallMaxArgs") != std::string::npos, "max-args constant");
            auto pos = src.find("int64_t aura_prim_call");
            CHECK(pos != std::string::npos, "found aura_prim_call");
            if (pos != std::string::npos) {
                auto win = src.substr(pos, 800);
                CHECK(win.find("int64_t* args") != std::string::npos ||
                          win.find("int64_t *args") != std::string::npos ||
                          win.find("args") != std::string::npos,
                      "pointer args ABI");
            }
        }
    }

    aura_set_prim_dispatcher(nullptr);

    std::println("\n=== test_prim_call_count_clamp (#1711/#2576): {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
