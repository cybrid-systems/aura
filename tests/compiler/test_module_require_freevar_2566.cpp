// @category: unit
// @reason: Issue #2566 — non-std module free-var resolve of required
//          std/mutate (and other std) bindings matches top-level.
//
//   AC1: (require "std/mutate" all:) inside non-std module → closures
//        resolve mutate:boundary-safe? same as top-level (#t under sandbox off)
//   AC2: Top-level require still injects into top_ (regression)
//   AC3: Nested require injects into module env (source-cite)
//   AC4: SoA live top_ fallback when walk reaches root frame
//   AC5: test + cmake + gate; no docs/design

#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

namespace fs = std::filesystem;

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

static fs::path find_lib_std() {
    for (const auto& p : {fs::path("lib/std"), fs::path("../lib/std"), fs::path("../../lib/std")}) {
        if (fs::is_directory(p) && fs::exists(p / "mutate.aura"))
            return fs::absolute(p.parent_path()); // lib/
    }
    return {};
}

static bool eval_bool(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    if (!r || !is_bool(*r))
        return false;
    return as_bool(*r);
}

// ── AC1: non-std module free-var parity with top-level ──
static void ac1_module_freevar_parity() {
    std::println("\n--- #2566 AC1: non-std module free-var parity with top-level ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC1: lib/ with std/mutate found");
    if (lib.empty())
        return;

    // Temp non-std module that requires std/mutate and closes over free vars.
    auto tmp = fs::temp_directory_path() / "aura_2566_mod";
    fs::create_directories(tmp);
    {
        std::ofstream out(tmp / "m.aura");
        out << "(export m:safe?)\n";
        out << "(require \"std/mutate\" all:)\n";
        out << "(define (m:safe?) (try (mutate:boundary-safe?) (catch (e) #f)))\n";
    }

    const auto path = lib.string() + ":" + tmp.string();
    setenv("AURA_PATH", path.c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);

    CompilerService cs;
    // Top-level probe first.
    CHECK(eval_bool(cs, "(begin (require \"std/mutate\" all:) (mutate:boundary-safe?))"),
          "AC1: top-level mutate:boundary-safe? → #t (sandbox off)");
    // Module free-var path must match.
    CHECK(eval_bool(cs, "(begin (require \"m\" all:) (m:safe?))"),
          "AC1: module m:safe? free-var path → #t (parity)");

    fs::remove_all(tmp);
}

// ── AC2: top-level inject still works ──
static void ac2_toplevel_inject() {
    std::println("\n--- #2566 AC2: top-level require still injects into top_ ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC2: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    CompilerService cs;
    CHECK(eval_bool(cs, "(begin (require \"std/mutate\" all:) "
                        "(procedure? mutate:boundary-safe?))"),
          "AC2: top-level procedure? after require");
}

// ── AC3: source-cite nested inject ──
static void ac3_source_cite_inject() {
    std::println("\n--- #2566 AC3: nested require injects into module env ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("require_inject_env_") != std::string::npos ||
              loader.find("RequireInjectGuard") != std::string::npos,
          "AC3: load_module_file sets inject target");
    CHECK(loader.find("#2566") != std::string::npos, "AC3: loader cites #2566");

    const auto prim = read_file("src/compiler/evaluator_primitives_module.cpp");
    CHECK(prim.find("require_inject_env_") != std::string::npos, "AC3: import uses inject target");
    CHECK(prim.find("#2566") != std::string::npos, "AC3: import cites #2566");

    const auto eixx = read_file("src/compiler/evaluator.ixx");
    CHECK(eixx.find("require_inject_env_") != std::string::npos, "AC3: field on Evaluator");
}

// ── AC4: SoA live top_ fallback ──
static void ac4_soa_live_top() {
    std::println("\n--- #2566 AC4: SoA live top_ fallback at root frame ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("#2566") != std::string::npos, "AC4: env lookup cites #2566");
    CHECK(env.find("cur == 0") != std::string::npos ||
              env.find("cur == 0 && owner_") != std::string::npos,
          "AC4: live top_ when SoA walk reaches root frame");
}

// ── AC5: gate wiring ──
static void ac5_gate() {
    std::println("\n--- #2566 AC5: test + cmake + gate ---");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_module_require_freevar_2566") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_module_require_freevar_2566") != std::string::npos,
          "AC5: check script");
    CHECK(build.find("cmd_module_require_freevar_coverage") != std::string::npos, "AC5: gate cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2566: module free-var resolve for required std bindings ===");
    ac1_module_freevar_parity();
    ac2_toplevel_inject();
    ac3_source_cite_inject();
    ac4_soa_live_top();
    ac5_gate();
    std::println("\n=== #2566: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
