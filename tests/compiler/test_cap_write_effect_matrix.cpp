// @category: unit
// @reason: Issue #2532 — workspace/fiber write-control into Effect matrix.
//
//   AC1: workspace/fiber map to non-None effects
//   AC2: query remains None (read-only exempt)
//   AC3: mutate/ffi unchanged
//   AC4: source-cite SECURITY_EXEMPT + mapping
//   AC5: Restricted no grant → effect matrix deny path for workspace
//   AC6: linter

#include "test_harness.hpp"
#include "core/capability_model.hh"
#include <fstream>
#include <print>
#include <string>

import std;

namespace {
using aura::core::capability::Effect;
using aura::core::capability::effect_for_cap_name;
using aura::core::capability::has_effect;
using aura::test::g_failed;
using aura::test::g_passed;
std::string read_file(const char* path) {
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

int run_test_cap_write_effect_matrix() {
    std::println("=== Issue #2532: write caps into Effect matrix ===");
    auto ws = effect_for_cap_name("workspace");
    auto fb = effect_for_cap_name("fiber");
    auto q = effect_for_cap_name("query");
    auto mut = effect_for_cap_name("mutate");
    auto ffi = effect_for_cap_name("ffi");
    CHECK(ws != Effect::None, "AC1: workspace mapped");
    CHECK(has_effect(ws, Effect::Mutate) || has_effect(ws, Effect::TenantAdmin), "AC1: write-ish");
    CHECK(fb != Effect::None, "AC1: fiber mapped");
    CHECK(has_effect(fb, Effect::TenantAdmin), "AC1: fiber TenantAdmin");
    CHECK(q == Effect::None, "AC2: query remains None (read-only)");
    CHECK(effect_for_cap_name("compile") == Effect::None, "AC2: compile exempt");
    CHECK(mut == Effect::Mutate, "AC3: mutate");
    CHECK(ffi == Effect::Ffi, "AC3: ffi");
    auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("2532") != std::string::npos, "AC4: cite");
    CHECK(cap.find("SECURITY_EXEMPT: read-only observability") != std::string::npos ||
              cap.find("read-only observability") != std::string::npos,
          "AC4: exempt contract");
    CHECK(cap.find("workspace") != std::string::npos && cap.find("fiber") != std::string::npos,
          "AC4: mapping present");
    std::println("\n=== #2532: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cap_write_effect_matrix();
}
#endif
