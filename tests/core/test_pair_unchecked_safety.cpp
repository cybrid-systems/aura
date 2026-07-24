// @category: unit
// @reason: Issue #1710 — aura_pair_car/cdr_unchecked must not raw-index
// Issue #1710 (#1978 renamed): issue# moved from filename to header.
// g_pair_slots without bounds + lock (UAF under concurrent realloc).
//
//   AC1: unchecked car/cdr on live pair matches safe path
//   AC2: OOB pair id returns 0 and bumps fallback metric
//   AC3: defuse stamp drift bumps fallback metric
//   AC4: source has #1710 lock+bounds path

#include "test_harness.hpp"

#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

extern "C" void aura_counters_reset();

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
    // ── AC1: live pair ──
    {
        std::println("\n--- AC1: unchecked matches safe path ---");
        aura_counters_reset();
        auto p = aura_alloc_pair(11, 22);
        CHECK(p != 0, "alloc_pair");
        auto car_s = aura_pair_car(p);
        auto cdr_s = aura_pair_cdr(p);
        auto car_u = aura_pair_car_unchecked(p);
        auto cdr_u = aura_pair_cdr_unchecked(p);
        CHECK(car_s == 11 && car_u == 11, "car safe==unchecked==11");
        CHECK(cdr_s == 22 && cdr_u == 22, "cdr safe==unchecked==22");
    }

    // ── AC2: OOB ──
    {
        std::println("\n--- AC2: OOB returns 0 + fallback ---");
        aura_counters_reset();
        // Fabricate a pair-tagged id far beyond table: (huge << 2) | 1
        const int64_t bogus = (static_cast<int64_t>(1) << 40) | 1;
        const auto fb0 = aura_unchecked_pair_fallback_total();
        auto car = aura_pair_car_unchecked(bogus);
        auto cdr = aura_pair_cdr_unchecked(bogus);
        CHECK(car == 0 && cdr == 0, "OOB car/cdr return 0");
        CHECK(aura_unchecked_pair_fallback_total() > fb0, "fallback metric bumped");
    }

    // ── AC3: defuse stamp drift ──
    {
        std::println("\n--- AC3: stamp drift bumps fallback ---");
        aura_counters_reset();
        auto p = aura_alloc_pair(3, 4);
        aura_pair_l2_stamp_defuse(123456789ull); // unlikely to match live defuse
        const auto fb0 = aura_unchecked_pair_fallback_total();
        (void)aura_pair_car_unchecked(p);
        // Drift check only bumps when hooks provide a different live version,
        // or stamp != UINT64_MAX and cur differs. If hooks unbound, get_version=0
        // so stamp 123456789 != 0 → fallback bumps.
        CHECK(aura_unchecked_pair_fallback_total() > fb0 ||
                  aura_unchecked_pair_fallback_total() >= fb0,
              "drift path executed (fallback may or may not bump if versions match)");
        // Force mismatch expectation when unbound hooks → cur==0, stamp!=0
        CHECK(aura_unchecked_pair_fallback_total() > fb0,
              "unbound hooks: stamp 123456789 vs defuse 0 bumps fallback");
        aura_pair_l2_clear_defuse_stamp();
        CHECK(aura_pair_car_unchecked(p) == 3, "after clear stamp, car still 3");
    }

    // ── AC4: source ──
    {
        std::println("\n--- AC4: source has #1710 defensive path ---");
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
            CHECK(src.find("Issue #1710") != std::string::npos, "cites #1710");
            CHECK(src.find("aura_lock_workspace_read()") != std::string::npos,
                  "uses workspace read lock");
            CHECK(src.find("g_unchecked_pair_fallback_total") != std::string::npos,
                  "fallback metric");
            CHECK(src.find("pair_field_locked") != std::string::npos, "pair_field_locked helper");
            // Must not have raw g_pair_slots[id]->car without size check in unchecked.
            auto car_u = src.find("int64_t aura_pair_car_unchecked");
            CHECK(car_u != std::string::npos, "found car_unchecked");
            if (car_u != std::string::npos) {
                auto win = src.substr(car_u, 900);
                CHECK(win.find("pair_field_locked") != std::string::npos ||
                          win.find("g_pair_slots.size()") != std::string::npos,
                      "car_unchecked uses locked/bounds path");
            }
        }
    }

    std::println("\n=== test_pair_unchecked_safety_1710: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
