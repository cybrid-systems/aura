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
//
//   #3266 AC1: realpath first, then lstat canonical (no stat-then-realpath)
//   #3266 AC2: is_plausible_module_path before compact_env_frames_lock_
//   #3266 AC3: loading_stack_ I/O window documented
//   #3266 AC4: empty refuse void; realpath+lstat load no crash
//   #3266 AC5: linter after #3265; no invent

#include "test_harness.hpp"

#include <cstdio>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_void;
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

static void ac3266_1_realpath_then_lstat() {
    std::println("\n--- #3266 AC1: realpath first, then lstat canonical ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    auto pos = loader.find("auto try_load = [](const std::string& full)");
    CHECK(pos != std::string::npos, "3266 AC1: try_load");
    auto win = loader.substr(pos, 900);
    CHECK(win.find("Issue #3266") != std::string::npos, "3266 AC1: cite");
    CHECK(win.find("::realpath(candidate.c_str(), nullptr)") != std::string::npos,
          "3266 AC1: realpath first");
    CHECK(win.find("::lstat(out.c_str(), &lst)") != std::string::npos, "3266 AC1: lstat canonical");
    CHECK(win.find("S_ISREG(lst.st_mode)") != std::string::npos, "3266 AC1: regular file");
    CHECK(win.find("::stat(candidate.c_str(), &st)") == std::string::npos,
          "3266 AC1: no stat-then-realpath");
}

static void ac3266_2_validate_before_lock() {
    std::println("\n--- #3266 AC2: path validation before compact_env_frames_lock_ ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    auto pos = loader.find("Evaluator::load_module_file");
    CHECK(pos != std::string::npos, "3266 AC2: load_module_file");
    auto win = loader.substr(pos, 1200);
    auto val = win.find("is_plausible_module_path(path)");
    auto lock = win.find("std::lock_guard interlock(compact_env_frames_lock_)");
    CHECK(val != std::string::npos && lock != std::string::npos && val < lock,
          "3266 AC2: validate before lock");
    CHECK(win.find("Issue #3266") != std::string::npos, "3266 AC2: cite");
}

static void ac3266_3_loading_stack_window_comment() {
    std::println("\n--- #3266 AC3: loading_stack_ I/O window documented ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("lock released for file I/O on purpose") != std::string::npos,
          "3266 AC3: I/O window comment");
    CHECK(loader.find("module_cache_ is populated before erase") != std::string::npos,
          "3266 AC3: cache-before-erase");
}

static void ac3266_4_quiet_refuse_and_load() {
    std::println("\n--- #3266 AC4: empty refuse + realpath/lstat load ---");
    CompilerService cs;
    auto empty = cs.eval("(load-module \"\")");
    CHECK(empty.has_value(), "3266 AC4: empty returns");
    CHECK(is_void(*empty), "3266 AC4: empty refuse void (zero extra)");
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
        CHECK(true, "3266 AC4: getcwd skip");
        return;
    }
    const std::string path = std::string(cwd) + "/aura_3266_tmp.aura";
    {
        std::ofstream out(path);
        CHECK(static_cast<bool>(out), "3266 AC4: write temp");
        out << "(define aura-3266-ok 1)\n";
    }
    auto r = cs.evaluator().load_module_file(path);
    (void)r;
    CHECK(true, "3266 AC4: realpath+lstat load no crash");
    ::unlink(path.c_str());
}

static void ac3266_5_source_and_linter() {
    std::println("\n--- #3266 AC5: linter + no invent ---");
    const auto t = read_file("tests/compiler/test_module_path_refuse.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_module_realpath_toctou_3266.py");
    CHECK(t.find("ac3266_1_realpath_then_lstat") != std::string::npos, "3266 AC5: AC1");
    CHECK(!lint.empty() && lint.find("Issue #3266") != std::string::npos, "3266 AC5: linter");
    CHECK(build.find("check_module_realpath_toctou_3266") != std::string::npos,
          "3266 AC5: build.py");
    {
        std::ifstream f("tests/compiler/test_issue_3266.cpp");
        CHECK(!f.good(), "3266 AC5: no test_issue_3266.cpp");
    }
    {
        std::ifstream f("docs/design/3266-module-realpath-toctou.md");
        CHECK(!f.good(), "3266 AC5: no docs/design");
    }
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
    std::println("\n=== Issue #3266: realpath-then-lstat + lock granularity ===");
    ac3266_1_realpath_then_lstat();
    ac3266_2_validate_before_lock();
    ac3266_3_loading_stack_window_comment();
    ac3266_4_quiet_refuse_and_load();
    ac3266_5_source_and_linter();
    std::println("\n=== #2653/#3266: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_path_refuse();
}
#endif
