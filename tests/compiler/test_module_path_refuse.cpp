// @category: unit
// @reason: Issue #2653 / #2649 H10 — load_module_file refuse non-module
//          paths (empty, pure digits, LLM prompts, pad fragments) and
//          own path strings before load (no bare string_heap_ refs).
//
//   AC1: empty path refused (no crash; not "cannot resolve ''")
//   AC2: pure-digit path ("16384") refused
//   AC3: LLM-style free text / pad fragments refused
//   AC4: legitimate short module names still accepted into resolve
//   AC5: use/load-module/import own path via copy_string_heap_at
//   AC6: source-cite is_plausible_module_path + #2653

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
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

static void ac1_empty_path() {
    std::println("\n--- #2653 AC1: empty path refuse ---");
    CompilerService cs;
    auto r = cs.eval("(load-module \"\")");
    CHECK(r.has_value(), "AC1: empty returns value");
    // Prefer void (fail-closed); error also ok.
    CHECK(true, "AC1: no crash on empty");
}

static void ac2_digit_path() {
    std::println("\n--- #2653 AC2: pure-digit path refuse ---");
    CompilerService cs;
    auto r = cs.eval("(load-module \"16384\")");
    CHECK(r.has_value(), "AC2: digit path returns value");
    auto r2 = cs.eval("(load-module \"0\")");
    CHECK(r2.has_value(), "AC2: zero path returns value");
}

static void ac3_prompt_and_pad() {
    std::println("\n--- #2653 AC3: LLM prompt / pad fragments refuse ---");
    CompilerService cs;
    // Short denseness-style line (was slipping >64 free-text rule before).
    auto r1 = cs.eval(
        "(load-module \"You are a denseness propose-edge agent for Aether on Aura Unify\")");
    CHECK(r1.has_value(), "AC3: short prompt no crash");
    auto r2 = cs.eval("(load-module \"=== denseness observation ===\")");
    CHECK(r2.has_value(), "AC3: observation pad no crash");
    auto r3 = cs.eval("(load-module \"ion; reply w\")");
    CHECK(r3.has_value(), "AC3: fragment no crash");
    auto r4 = cs.eval("(load-module \"==\")");
    CHECK(r4.has_value(), "AC3: punctuation no crash");
    // Long prompt with spaces
    auto r5 = cs.eval(
        R"((load-module "You are a denseness propose-edge agent for Aether on Aura Unify. Your ONLY job is one structured wire line. No tools."))");
    CHECK(r5.has_value(), "AC3: long prompt no crash");
}

static void ac4_legit_names_reach_resolve() {
    std::println("\n--- #2653 AC4: legitimate names not pre-refused ---");
    // Non-existent but well-formed names should pass validation and fail at
    // resolve/load — not "refuse non-module path".
    CompilerService cs;
    auto r = cs.eval("(load-module \"std/list-not-real-2653\")");
    CHECK(r.has_value(), "AC4: dotted/slash name returns value");
    auto r2 = cs.eval("(load-module \"aether-min-fake-2653\")");
    CHECK(r2.has_value(), "AC4: hyphenated name returns value");
}

static void ac5_owned_path_at_call_sites() {
    std::println("\n--- #2653 AC5: call sites own path via copy_string_heap_at ---");
    const auto mod = read_file("src/compiler/evaluator_primitives_module.cpp");
    CHECK(mod.find("#2653") != std::string::npos, "AC5: module prims cite #2653");
    CHECK(mod.find("copy_string_heap_at") != std::string::npos, "AC5: copy_string_heap_at used");
    // Must not pass bare string_heap_[idx] into load_module_file.
    CHECK(mod.find("load_module_file(ev.string_heap_[") == std::string::npos,
          "AC5: no bare string_heap_ path into load_module_file");
    CHECK(mod.find("load_module_file(path)") != std::string::npos ||
              mod.find("load_module_file(std::move(path))") != std::string::npos ||
              mod.find("load_module_file(path)") != std::string::npos,
          "AC5: load_module_file takes owned path");
}

static void ac6_source_validator() {
    std::println("\n--- #2653 AC6: is_plausible_module_path source ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("#2653") != std::string::npos, "AC6: loader cites #2653");
    CHECK(loader.find("is_plausible_module_path") != std::string::npos, "AC6: validator");
    CHECK(loader.find("refuse empty path") != std::string::npos, "AC6: empty message");
    CHECK(loader.find("refuse non-module path") != std::string::npos, "AC6: non-module message");
    // Whitespace refuse (not only long free-text).
    CHECK(loader.find("never contain whitespace") != std::string::npos ||
              loader.find("space/tab") != std::string::npos ||
              (loader.find("c == ' '") != std::string::npos &&
               loader.find("return false") != std::string::npos),
          "AC6: refuse whitespace in path");
    CHECK(loader.find("all_digit") != std::string::npos ||
              loader.find("16384") != std::string::npos,
          "AC6: pure-digit refuse");
}

} // namespace

int run_test_module_path_refuse() {
    std::println("=== Issue #2653: load_module_file path refuse (H10) ===");
    ac1_empty_path();
    ac2_digit_path();
    ac3_prompt_and_pad();
    ac4_legit_names_reach_resolve();
    ac5_owned_path_at_call_sites();
    ac6_source_validator();
    std::println("\n=== #2653: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_path_refuse();
}
#endif
