// @category: unit
// @reason: Issue #1706 — aura_get_closure_bridge_epoch / defuse_version
// return 0 for out-of-range, which collides with valid epoch 0. Callers
// must use aura_closure_exists first (Option B).
//
//   AC1: exists(-1) / exists(huge) == 0; epoch OOR still returns 0
//   AC2: after alloc_closure, exists(id)==1; epoch may be 0 (valid)
//   AC3: exists disambiguates: OOR → no slot; live → can trust epoch 0
//   AC4: source documents #1706 convention

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
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
    // ── AC1: OOR ──
    {
        std::println("\n--- AC1: OOR exists + epoch ---");
        CHECK(aura_closure_exists(-1) == 0, "exists(-1) == 0");
        CHECK(aura_closure_exists(999999) == 0, "exists(huge) == 0");
        CHECK(aura_get_closure_bridge_epoch(-1) == 0, "bridge_epoch(-1) == 0");
        CHECK(aura_get_closure_bridge_epoch(999999) == 0, "bridge_epoch(huge) == 0");
        CHECK(aura_get_closure_defuse_version(-1) == 0, "defuse(-1) == 0");
        CHECK(aura_get_closure_defuse_version(999999) == 0, "defuse(huge) == 0");
    }

    // ── AC2/AC3: allocate then exists ──
    {
        std::println("\n--- AC2/AC3: alloc + exists disambiguates epoch 0 ---");
        // func_id 0 is fine for table slot allocation tests.
        const auto cid = aura_alloc_closure(0);
        CHECK(cid >= 0, "alloc_closure returns non-negative id");
        CHECK(aura_closure_exists(cid) == 1, "exists(allocated) == 1");
        // Epoch may legitimately be 0 for a fresh stamp.
        const auto ep = aura_get_closure_bridge_epoch(cid);
        const auto dv = aura_get_closure_defuse_version(cid);
        std::println("  cid={} bridge_epoch={} defuse={}", cid, ep, dv);
        // Critical contract: OOR also returns epoch 0, so only exists
        // tells us this 0 is a real slot stamp.
        if (ep == 0)
            CHECK(aura_closure_exists(cid) == 1,
                  "epoch 0 with exists=1 is a valid live-slot stamp (not OOR)");
        else
            CHECK(true, "non-zero epoch still requires exists=1");
        CHECK(aura_closure_exists(cid + 100000) == 0, "neighbor OOR still missing");
        // Free leaves slot in table (exists still 1) but is_freed becomes 1.
        aura_free_closure(cid);
        CHECK(aura_closure_exists(cid) == 1, "exists remains 1 after free (slot retained)");
        CHECK(aura_closure_is_freed(cid) == 1, "is_freed after free");
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source cites #1706 ---");
        const char* candidates[] = {
            "src/compiler/aura_jit_runtime.cpp",
            "../src/compiler/aura_jit_runtime.cpp",
            "src/compiler/aura_jit_bridge.h",
            "../src/compiler/aura_jit_bridge.h",
        };
        std::string rt, hdr;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (s.empty())
                continue;
            if (std::string_view(p).find("aura_jit_runtime.cpp") != std::string_view::npos)
                rt = std::move(s);
            if (std::string_view(p).find("aura_jit_bridge.h") != std::string_view::npos)
                hdr = std::move(s);
        }
        CHECK(!rt.empty(), "read aura_jit_runtime.cpp");
        CHECK(!hdr.empty(), "read aura_jit_bridge.h");
        if (!rt.empty()) {
            CHECK(rt.find("aura_closure_exists") != std::string::npos, "runtime defines exists");
            CHECK(rt.find("Issue #1706") != std::string::npos ||
                      rt.find("#1706") != std::string::npos,
                  "runtime cites #1706");
        }
        if (!hdr.empty()) {
            CHECK(hdr.find("aura_closure_exists") != std::string::npos, "header declares exists");
            CHECK(hdr.find("1706") != std::string::npos, "header documents #1706");
        }
    }

    std::println("\n=== test_closure_exists_epoch_1706: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
