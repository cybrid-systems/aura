// @category: unit
// @reason: Issue #2370 — real PerEval storm isolation for SpecJIT + shape
// version (no cross-eval specialization invalidation).
//
//   AC1: Soft / Global path — process-wide stamp path unchanged
//   AC2: two controllers; storm on A only clears A under PerEval
//   AC3: install stamps isolation epoch under PerEval; B survives A storm
//   AC4: query schema-2370 + per-eval metrics
//   AC5: source-cite + gate

#include "test_harness.hpp"

#include "compiler/aura_jit.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/shape_profiler.h"
#include "compiler/spec_jit_controller.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::shape::SpecJITController;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::jit::AuraJIT;
using aura::test::g_failed;
using aura::test::g_passed;

extern "C" void aura_set_storm_isolation_mode(int mode) noexcept;
extern "C" int aura_get_storm_isolation_mode(void) noexcept;
extern "C" void aura_set_storm_eval_context(void* eval_ptr) noexcept;
extern "C" std::uint64_t aura_specjit_storm_clear_total_v_read(void);
extern "C" std::uint64_t aura_specjit_per_eval_storm_clear_total_v_read(void);
extern "C" std::uint64_t aura_specjit_per_eval_storm_skip_foreign_total_v_read(void);

static int64_t stub_fn_a(int64_t*, uint32_t) {
    return 1;
}
static int64_t stub_fn_b(int64_t*, uint32_t) {
    return 2;
}

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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: Global path ──
static void ac1_global_soft() {
    std::println("\n--- AC1: Global isolation — process-wide stamp path ---");
    aura_set_storm_isolation_mode(0); // Global
    CHECK(aura_get_storm_isolation_mode() == 0, "AC1: mode Global");
    const auto v0 = aura::compiler::shape::current_global_shape_version();
    aura::compiler::shape::bump_shape_version_on_storm_enter();
    CHECK(aura::compiler::shape::current_global_shape_version() > v0, "AC1: global bump works");

    AuraJIT jit;
    SpecJITController ctl(jit);
    ctl.set_eval_owner(reinterpret_cast<void*>(0xA1));
    ctl.install_specialization("f", 1, &stub_fn_a);
    CHECK(ctl.has_specialization("f", 1), "AC1: specialization present under Global");
    // Soft: PerEval counters not required to advance.
}

// ── AC2/AC3: two controllers, PerEval ──
static void ac2_ac3_two_eval_isolation() {
    std::println("\n--- AC2/AC3: PerEval — storm on A does not clear B ---");
    aura_set_storm_isolation_mode(2); // PerEval
    CHECK(aura_get_storm_isolation_mode() == 2, "AC2: mode PerEval");

    AuraJIT jit;
    SpecJITController ctl_a(jit);
    SpecJITController ctl_b(jit);
    void* owner_a = reinterpret_cast<void*>(0xA100);
    void* owner_b = reinterpret_cast<void*>(0xB200);
    ctl_a.set_eval_owner(owner_a);
    ctl_b.set_eval_owner(owner_b);

    ctl_a.install_specialization("fa", 1, &stub_fn_a);
    ctl_b.install_specialization("fb", 1, &stub_fn_b);
    CHECK(ctl_a.has_specialization("fa", 1), "AC2: A has fa");
    CHECK(ctl_b.has_specialization("fb", 1), "AC2: B has fb");

    const auto clear0 = aura_specjit_storm_clear_total_v_read();
    const auto pe0 = aura_specjit_per_eval_storm_clear_total_v_read();
    const auto skip0 = aura_specjit_per_eval_storm_skip_foreign_total_v_read();
    const auto epoch_a0 = ctl_a.isolation_shape_epoch();
    const auto epoch_b0 = ctl_b.isolation_shape_epoch();
    const auto global_v0 = aura::compiler::shape::current_global_shape_version();

    // Storm as eval A: only A clears.
    aura_set_storm_eval_context(owner_a);
    ctl_a.on_deopt_storm();
    ctl_b.on_deopt_storm(); // foreign for B → skip

    CHECK(!ctl_a.has_specialization("fa", 1), "AC2: A cleared");
    CHECK(ctl_b.has_specialization("fb", 1), "AC2: B still has specialization");
    CHECK(ctl_a.isolation_shape_epoch() > epoch_a0, "AC3: A isolation epoch advanced");
    CHECK(ctl_b.isolation_shape_epoch() == epoch_b0, "AC3: B isolation epoch unchanged");
    CHECK(aura_specjit_storm_clear_total_v_read() == clear0 + 1, "AC2: global clear +1 (A only)");
    CHECK(aura_specjit_per_eval_storm_clear_total_v_read() == pe0 + 1, "AC2: per-eval clear +1");
    CHECK(aura_specjit_per_eval_storm_skip_foreign_total_v_read() == skip0 + 1,
          "AC2: foreign skip +1");
    // PerEval: process-global shape_version must not spuriously advance.
    CHECK(aura::compiler::shape::current_global_shape_version() == global_v0,
          "AC2: global shape_version not bumped under PerEval SpecJIT storm");

    // Re-install on A after storm — uses new isolation epoch.
    ctl_a.install_specialization("fa", 1, &stub_fn_a);
    CHECK(ctl_a.has_specialization("fa", 1), "AC3: A re-install after storm ok");
    CHECK(ctl_b.has_specialization("fb", 1), "AC3: B still valid after A re-install");

    // Storm as B: only B clears.
    aura_set_storm_eval_context(owner_b);
    ctl_a.on_deopt_storm(); // foreign → skip
    ctl_b.on_deopt_storm();
    CHECK(ctl_a.has_specialization("fa", 1), "AC2: A survives B storm");
    CHECK(!ctl_b.has_specialization("fb", 1), "AC2: B cleared by own storm");

    aura_set_storm_isolation_mode(0); // restore Global
    aura_set_storm_eval_context(nullptr);
}

// ── AC4: query ──
static void ac4_query() {
    std::println("\n--- AC4: query schema-2370 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2370") == 2370, "AC4: schema-2370");
    CHECK(href(cs, "issue-2370") == 2370, "AC4: issue-2370");
    CHECK(href(cs, "storm-isolation-per-eval-wired") == 1, "AC4: per-eval wired");
    CHECK(href(cs, "specjit-storm-clear-total") >= 0, "AC4: storm-clear-total");
    CHECK(href(cs, "specjit-per-eval-storm-clear-total") >= 0, "AC4: per-eval clear total");
    CHECK(href(cs, "specjit-per-eval-storm-skip-foreign-total") >= 0, "AC4: skip-foreign total");
}

// ── AC5: source + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- AC5: source-cite + gate ---");
    const auto sj = read_file("src/compiler/spec_jit_controller.cpp");
    const auto sjh = read_file("src/compiler/spec_jit_controller.h");
    const auto sp = read_file("src/compiler/shape_profiler.cpp");
    const auto hur = read_file("src/compiler/hot_update_registry.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script =
        read_file("scripts/coverage/checks/check_specjit_per_eval_storm_isolation_2370.py");
    CHECK(sj.find("Issue #2370") != std::string::npos, "AC5: #2370 in SpecJIT cpp");
    CHECK(sj.find("isolation_shape_epoch_") != std::string::npos, "AC5: isolation epoch");
    CHECK(sj.find("g_specjit_per_eval_storm_clear_total") != std::string::npos,
          "AC5: per-eval clear counter");
    CHECK(sj.find("g_specjit_per_eval_storm_skip_foreign_total") != std::string::npos,
          "AC5: foreign skip counter");
    CHECK(sjh.find("set_eval_owner") != std::string::npos, "AC5: set_eval_owner API");
    CHECK(sp.find("aura_get_storm_isolation_mode") != std::string::npos,
          "AC5: ShapeProfiler gates global bump");
    CHECK(hur.find("Issue #2370") != std::string::npos, "AC5: HotUpdate PerEval");
    CHECK(hur.find("aura_get_storm_eval_context") != std::string::npos, "AC5: TLS eval key");
    CHECK(mut.find("schema-2370") != std::string::npos, "AC5: query schema");
    CHECK(svc.find("set_eval_owner") != std::string::npos, "AC5: service binds owner");
    CHECK(cmake.find("test_specjit_per_eval_storm_isolation_2370") != std::string::npos,
          "AC5: cmake");
    CHECK(build.find("check_specjit_per_eval_storm_isolation_2370") != std::string::npos,
          "AC5: build script");
    CHECK(build.find("cmd_specjit_per_eval_storm_isolation_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(script.find("schema-2370") != std::string::npos, "AC5: coverage script");
}

} // namespace

int main() {
    std::println("test_specjit_per_eval_storm_isolation_2370");
    ac1_global_soft();
    ac2_ac3_two_eval_isolation();
    ac4_query();
    ac5_source_and_gate();
    if (g_failed)
        return 1;
    std::println("SpecJIT PerEval storm isolation #2370: OK ({} passed)", g_passed);
    return 0;
}
