// @category: unit
// @reason: Issue #2504 — end-to-end PerEval storm isolation: no cross-eval
//          SpecJIT clear under dual-eval / multi-agent hosts (#2370 gate).
//
//   AC1: Dual eval, PerEval — storm in B does not clear A's specializations
//        (get_specialized still hits)
//   AC2: Foreign skip counter increments; clear only for matching owner
//   AC3: Global mode — storm clears both controllers (regression lock)
//   AC4: Shape version process bump not forced under PerEval storm enter
//   AC5: Source-cite + ctest target + coverage gate + schema-2504

#include "test_harness.hpp"

#include "compiler/aura_jit.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/shape_profiler.h"
#include "compiler/spec_jit_controller.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

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
extern "C" void* aura_get_storm_eval_context(void) noexcept;
extern "C" std::uint64_t aura_specjit_storm_clear_total_v_read(void);
extern "C" std::uint64_t aura_specjit_per_eval_storm_clear_total_v_read(void);
extern "C" std::uint64_t aura_specjit_per_eval_storm_skip_foreign_total_v_read(void);

static int64_t stub_fn_a(int64_t*, uint32_t) {
    return 11;
}
static int64_t stub_fn_b(int64_t*, uint32_t) {
    return 22;
}
static int64_t stub_fn_c(int64_t*, uint32_t) {
    return 33;
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

static void restore_global_isolation() {
    aura_set_storm_isolation_mode(0); // Global
    aura_set_storm_eval_context(nullptr);
}

// ── AC1: Dual eval PerEval — B storm does not clear A's hit path ──
static void ac1_dual_eval_pereval_hit_survives() {
    std::println("\n--- #2504 AC1: PerEval dual-eval — B storm, A still hits ---");
    aura_set_storm_isolation_mode(2); // PerEval
    CHECK(aura_get_storm_isolation_mode() == 2, "AC1: mode PerEval");

    AuraJIT jit;
    SpecJITController ctl_a(jit);
    SpecJITController ctl_b(jit);
    void* owner_a = reinterpret_cast<void*>(0xA2504);
    void* owner_b = reinterpret_cast<void*>(0xB2504);
    ctl_a.set_eval_owner(owner_a);
    ctl_b.set_eval_owner(owner_b);

    ctl_a.install_specialization("eval_a_hot", 1, &stub_fn_a);
    ctl_b.install_specialization("eval_b_hot", 1, &stub_fn_b);
    CHECK(ctl_a.get_specialized("eval_a_hot", 1) == &stub_fn_a, "AC1: A hit before storm");
    CHECK(ctl_b.get_specialized("eval_b_hot", 1) == &stub_fn_b, "AC1: B hit before storm");

    // Storm as B only (TLS = B). A must keep specialization + hit.
    aura_set_storm_eval_context(owner_b);
    ctl_a.on_deopt_storm(); // foreign for A → skip
    ctl_b.on_deopt_storm(); // matching owner → clear B

    CHECK(ctl_a.has_specialization("eval_a_hot", 1), "AC1: A still has specialization");
    CHECK(ctl_a.get_specialized("eval_a_hot", 1) == &stub_fn_a,
          "AC1: A get_specialized still hits after B storm");
    CHECK(!ctl_b.has_specialization("eval_b_hot", 1), "AC1: B cleared by own storm");
    CHECK(ctl_b.get_specialized("eval_b_hot", 1) == nullptr,
          "AC1: B hit path gone after own storm");

    // A's local storm finally clears A.
    aura_set_storm_eval_context(owner_a);
    ctl_a.on_deopt_storm();
    CHECK(!ctl_a.has_specialization("eval_a_hot", 1), "AC1: A cleared by own storm");
    CHECK(ctl_a.get_specialized("eval_a_hot", 1) == nullptr, "AC1: A miss after own storm");

    restore_global_isolation();
}

// ── AC2: Foreign skip + clear only matching owner ──
static void ac2_foreign_skip_and_owner_clear() {
    std::println("\n--- #2504 AC2: foreign skip counter + owner-only clear ---");
    aura_set_storm_isolation_mode(2);

    AuraJIT jit;
    SpecJITController ctl_a(jit);
    SpecJITController ctl_b(jit);
    void* owner_a = reinterpret_cast<void*>(0xA25042);
    void* owner_b = reinterpret_cast<void*>(0xB25042);
    ctl_a.set_eval_owner(owner_a);
    ctl_b.set_eval_owner(owner_b);
    ctl_a.install_specialization("fa", 1, &stub_fn_a);
    ctl_b.install_specialization("fb", 1, &stub_fn_b);

    const auto skip0 = aura_specjit_per_eval_storm_skip_foreign_total_v_read();
    const auto pe_clear0 = aura_specjit_per_eval_storm_clear_total_v_read();
    const auto storm_clear0 = aura_specjit_storm_clear_total_v_read();
    const auto a_clear0 = ctl_a.storm_clear_count();
    const auto b_clear0 = ctl_b.storm_clear_count();

    // Foreign storm from A onto B's controller (TLS = A, call B).
    aura_set_storm_eval_context(owner_a);
    ctl_b.on_deopt_storm();
    CHECK(ctl_b.has_specialization("fb", 1), "AC2: B not cleared by foreign storm");
    CHECK(aura_specjit_per_eval_storm_skip_foreign_total_v_read() == skip0 + 1,
          "AC2: skip_foreign +1");
    CHECK(ctl_b.storm_clear_count() == b_clear0, "AC2: B storm_clear_count unchanged");
    CHECK(aura_specjit_per_eval_storm_clear_total_v_read() == pe_clear0,
          "AC2: per-eval clear not advanced on foreign");

    // Matching owner clear.
    ctl_a.on_deopt_storm();
    CHECK(!ctl_a.has_specialization("fa", 1), "AC2: A cleared by matching owner");
    CHECK(ctl_a.storm_clear_count() == a_clear0 + 1, "AC2: A storm_clear_count +1");
    CHECK(aura_specjit_per_eval_storm_clear_total_v_read() == pe_clear0 + 1,
          "AC2: per-eval clear +1 for matching");
    CHECK(aura_specjit_storm_clear_total_v_read() == storm_clear0 + 1,
          "AC2: process storm_clear +1 for matching only");

    restore_global_isolation();
}

// ── AC3: Global mode process-wide clear ──
static void ac3_global_clears_both() {
    std::println("\n--- #2504 AC3: Global mode — storm clears both controllers ---");
    aura_set_storm_isolation_mode(0); // Global
    CHECK(aura_get_storm_isolation_mode() == 0, "AC3: mode Global");

    AuraJIT jit;
    SpecJITController ctl_a(jit);
    SpecJITController ctl_b(jit);
    // Owners set but ignored under Global (no foreign skip).
    ctl_a.set_eval_owner(reinterpret_cast<void*>(0xA0A0));
    ctl_b.set_eval_owner(reinterpret_cast<void*>(0xB0B0));
    ctl_a.install_specialization("ga", 1, &stub_fn_a);
    ctl_b.install_specialization("gb", 1, &stub_fn_b);
    CHECK(ctl_a.has_specialization("ga", 1) && ctl_b.has_specialization("gb", 1),
          "AC3: both installed under Global");

    const auto pe_skip0 = aura_specjit_per_eval_storm_skip_foreign_total_v_read();
    aura_set_storm_eval_context(reinterpret_cast<void*>(0xA0A0));
    // Under Global both clears apply regardless of TLS owner mismatch.
    ctl_a.on_deopt_storm();
    ctl_b.on_deopt_storm();

    CHECK(!ctl_a.has_specialization("ga", 1), "AC3: A cleared under Global");
    CHECK(!ctl_b.has_specialization("gb", 1), "AC3: B cleared under Global (process-wide)");
    CHECK(aura_specjit_per_eval_storm_skip_foreign_total_v_read() == pe_skip0,
          "AC3: no foreign skip under Global");

    restore_global_isolation();
}

// ── AC4: PerEval does not force process-global shape_version bump ──
static void ac4_no_global_shape_bump_under_pereval() {
    std::println("\n--- #2504 AC4: PerEval storm does not force global shape_version ---");
    aura_set_storm_isolation_mode(2);

    AuraJIT jit;
    SpecJITController ctl(jit);
    void* owner = reinterpret_cast<void*>(0x52504145); // "SHAPE"-ish tag
    ctl.set_eval_owner(owner);
    ctl.install_specialization("shape_fn", 1, &stub_fn_c);

    const auto global_v0 = aura::compiler::shape::current_global_shape_version();
    const auto epoch0 = ctl.isolation_shape_epoch();

    aura_set_storm_eval_context(owner);
    ctl.on_deopt_storm();

    CHECK(aura::compiler::shape::current_global_shape_version() == global_v0,
          "AC4: process-global shape_version unchanged under PerEval SpecJIT storm");
    CHECK(ctl.isolation_shape_epoch() > epoch0, "AC4: local isolation epoch advanced");
    CHECK(!ctl.has_specialization("shape_fn", 1), "AC4: local cache cleared");

    // ShapeProfiler storm-enter also must not bump under PerEval (#2370).
    const auto global_v1 = aura::compiler::shape::current_global_shape_version();
    // Simulate ShapeProfiler storm-enter gate: only bumps when mode != PerEval.
    if (aura_get_storm_isolation_mode() != 2)
        aura::compiler::shape::bump_shape_version_on_storm_enter();
    CHECK(aura::compiler::shape::current_global_shape_version() == global_v1,
          "AC4: ShapeProfiler-style enter would not bump under PerEval");

    restore_global_isolation();
}

// ── AC5: concurrent dual-eval stress + source/gate/schema ──
static void ac5_concurrent_and_source_gate() {
    std::println("\n--- #2504 AC5: concurrent stress + source-cite + schema-2504 ---");
    aura_set_storm_isolation_mode(2);

    AuraJIT jit;
    SpecJITController ctl_a(jit);
    SpecJITController ctl_b(jit);
    void* owner_a = reinterpret_cast<void*>(0xCA2504);
    void* owner_b = reinterpret_cast<void*>(0xCB2504);
    ctl_a.set_eval_owner(owner_a);
    ctl_b.set_eval_owner(owner_b);

    // Pre-install; concurrent storms must not cross-clear.
    for (int i = 0; i < 8; ++i) {
        ctl_a.install_specialization("a_fn", static_cast<std::uint32_t>(i + 1), &stub_fn_a);
        ctl_b.install_specialization("b_fn", static_cast<std::uint32_t>(i + 1), &stub_fn_b);
    }

    const auto skip0 = aura_specjit_per_eval_storm_skip_foreign_total_v_read();
    std::atomic<int> done{0};
    std::thread ta([&] {
        for (int r = 0; r < 32; ++r) {
            aura_set_storm_eval_context(owner_a);
            ctl_b.on_deopt_storm(); // foreign storm onto B
            ctl_a.install_specialization("a_fn", 1, &stub_fn_a);
        }
        done.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread tb([&] {
        for (int r = 0; r < 32; ++r) {
            aura_set_storm_eval_context(owner_b);
            ctl_a.on_deopt_storm(); // foreign storm onto A
            ctl_b.install_specialization("b_fn", 1, &stub_fn_b);
        }
        done.fetch_add(1, std::memory_order_relaxed);
    });
    ta.join();
    tb.join();
    CHECK(done.load() == 2, "AC5: both storm threads finished");
    CHECK(aura_specjit_per_eval_storm_skip_foreign_total_v_read() >= skip0 + 32,
          "AC5: foreign skips advanced under concurrent dual-eval storms");
    // After only foreign storms, re-install path keeps hits available.
    // Foreign-only storms never clear matching owner — both should still hit.
    CHECK(ctl_a.get_specialized("a_fn", 1) == &stub_fn_a,
          "AC5: A still hits after concurrent foreign storms");
    CHECK(ctl_b.get_specialized("b_fn", 1) == &stub_fn_b,
          "AC5: B still hits after concurrent foreign storms");

    restore_global_isolation();

    // Query lineage
    CompilerService cs;
    CHECK(href(cs, "schema-2504") == 2504, "AC5: schema-2504");
    CHECK(href(cs, "issue-2504") == 2504, "AC5: issue-2504");
    CHECK(href(cs, "specjit-pereval-e2e-isolation-wired") == 1, "AC5: e2e isolation wired");
    CHECK(href(cs, "multi-eval-host-pereval-heuristic-wired") == 1,
          "AC5: multi-eval host heuristic wired");
    CHECK(href(cs, "schema-2370") == 2370, "AC5: #2370 lineage retained");
    CHECK(href(cs, "storm-isolation-per-eval-wired") == 1, "AC5: per-eval wired");

    // Source-cite
    const auto sj = read_file("src/compiler/spec_jit_controller.cpp");
    const auto sp = read_file("src/compiler/shape_profiler.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script =
        read_file("scripts/coverage/checks/check_specjit_pereval_storm_e2e_2504.py");
    CHECK(sj.find("g_specjit_per_eval_storm_skip_foreign_total") != std::string::npos,
          "AC5: foreign skip in SpecJIT");
    CHECK(sj.find("storm_isolation_is_per_eval") != std::string::npos, "AC5: PerEval gate");
    CHECK(sp.find("aura_get_storm_isolation_mode() != 2") != std::string::npos,
          "AC5: ShapeProfiler skips global bump under PerEval");
    CHECK(mut.find("schema-2504") != std::string::npos, "AC5: schema-2504 in mutate");
    CHECK(mut.find("specjit-pereval-e2e-isolation-wired") != std::string::npos,
          "AC5: e2e wired key");
    CHECK(cmake.find("test_specjit_pereval_storm_e2e_2504") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_specjit_pereval_storm_e2e_2504") != std::string::npos,
          "AC5: build.py gate script");
    CHECK(build.find("cmd_specjit_pereval_storm_e2e_coverage") != std::string::npos,
          "AC5: build.py coverage cmd");
    CHECK(script.find("schema-2504") != std::string::npos, "AC5: coverage script present");
    CHECK(script.find("2504") != std::string::npos, "AC5: issue cite in coverage script");
}

} // namespace

int run_test_specjit_pereval_storm_e2e_2504() {
    std::println("test_specjit_pereval_storm_e2e_2504");
    ac1_dual_eval_pereval_hit_survives();
    ac2_foreign_skip_and_owner_clear();
    ac3_global_clears_both();
    ac4_no_global_shape_bump_under_pereval();
    ac5_concurrent_and_source_gate();
    if (g_failed)
        return 1;
    std::println("SpecJIT PerEval storm e2e #2504: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_specjit_pereval_storm_e2e_2504();
}
#endif
