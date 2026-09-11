// @category: unit
// @reason: Issue #2377 — force single steal-complete entry (no weak
// legacy residual-less path under production).

#include "test_harness.hpp"

#include "compiler/mutation_hold_budget.h"
#include "core/gc_hooks.h"
#include "serve/fiber.h"
#include "serve/runtime_production_abi.h"
#include "serve/scheduler.h"
#include "serve/steal_safety.h"
#include "compiler/typed_mutation_audit.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" int aura_jit_ir_typed_entry_commit_readiness_ok(void);
extern "C" int aura_jit_linear_move_drop_elision_ok(void);
extern "C" int aura_jit_linear_post_mutate_enforce(std::uint32_t env_id);
extern "C" void aura_set_linear_post_mutate_enforce_fn(int (*fn)(void*, std::uint32_t),
                                                       void* user_data);
extern "C" void aura_jit_clear_linear_env_context(void);

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-defer-reason-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

// ── Issue #3654: unset linear_post_mutate_enforce callback is unsafe ──
//   AC1: Production/Full + no host callback → enforce returns 1
//   AC2: Soft / Off → 0 (pass-through)
//   AC3: registered callback still 0=safe / 1=unsafe (strong bridge only)
//   AC4: typed-entry and Move/Drop elision stay AND (not OR)
//   AC5: this suite + persist-rehydrate; no invent / docs / new query key
namespace {
static int s_linear_cb_hits = 0;
static int ac3654_cb_ok(void* /*user*/, std::uint32_t /*env*/) {
    ++s_linear_cb_hits;
    return 0;
}
static int ac3654_cb_bad(void* /*user*/, std::uint32_t /*env*/) {
    ++s_linear_cb_hits;
    return 1;
}
} // namespace

static void ac3654_linear_post_mutate_unset_fail_closed() {
    std::println("\n--- #3654: unset linear_post_mutate_enforce fail-closed ---");
    const auto br = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    const auto hdr = read_file("src/compiler/aura_jit_bridge.h");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(br.find("Issue #3654") != std::string::npos, "3654 AC5: strong cite");
    CHECK(br.find("production_hard_face_active()") != std::string::npos,
          "3654 AC1: Production/Full hard-face");
    CHECK(stub.find("Issue #3343 / #3654") != std::string::npos, "3654 AC2: stub cite");
    CHECK(hdr.find("Production/Full + no callback") != std::string::npos, "3654: header contract");

    aura_jit_clear_linear_env_context();
    s_linear_cb_hits = 0;
    aura_set_linear_post_mutate_enforce_fn(&ac3654_cb_ok, nullptr);
    (void)aura_jit_linear_post_mutate_enforce(1);
    const bool strong = s_linear_cb_hits > 0;
    aura_set_linear_post_mutate_enforce_fn(nullptr, nullptr);

    {
        auto& pda =
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active;
        const auto saved_pda = pda.load(std::memory_order_relaxed);
        const auto saved_st = aura::compiler::typed_audit::get_strategy();
        pda.store(0, std::memory_order_relaxed);
        aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Off);
        CHECK(aura_jit_linear_post_mutate_enforce(0) == 0,
              "3654 AC2: Soft unset callback pass-through");
        CHECK(aura_jit_linear_post_mutate_enforce(0xFFFFFFFFu) == 0,
              "3654 AC2: Soft unset + null env pass-through");
        pda.store(1, std::memory_order_relaxed);
        CHECK(aura_jit_linear_post_mutate_enforce(0) == 1,
              "3654 AC1: production unset callback unsafe");
        CHECK(aura_jit_linear_post_mutate_enforce(0xFFFFFFFFu) == 1,
              "3654 AC1: production unset + null env unsafe");
        pda.store(0, std::memory_order_relaxed);
        aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
        if (strong) {
            CHECK(aura_jit_linear_post_mutate_enforce(0) == 1,
                  "3654 AC1: Full strategy unset callback unsafe");
        }
        pda.store(saved_pda, std::memory_order_relaxed);
        aura::compiler::typed_audit::set_strategy(saved_st);
    }

    if (strong) {
        auto& pda =
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active;
        const auto saved_pda = pda.load(std::memory_order_relaxed);
        pda.store(1, std::memory_order_relaxed);
        s_linear_cb_hits = 0;
        aura_set_linear_post_mutate_enforce_fn(&ac3654_cb_ok, nullptr);
        CHECK(aura_jit_linear_post_mutate_enforce(1) == 0,
              "3654 AC3: registered callback safe stays 0");
        CHECK(s_linear_cb_hits >= 1, "3654 AC3: registered callback invoked");
        aura_set_linear_post_mutate_enforce_fn(&ac3654_cb_bad, nullptr);
        s_linear_cb_hits = 0;
        CHECK(aura_jit_linear_post_mutate_enforce(1) == 1,
              "3654 AC3: registered callback unsafe stays 1");
        pda.store(saved_pda, std::memory_order_relaxed);
    }
    aura_set_linear_post_mutate_enforce_fn(nullptr, nullptr);
    aura_jit_clear_linear_env_context();

    CHECK(tma.find("if (!linear_ir_fastpath_try_skip())") != std::string::npos &&
              tma.find("if (!cr.would_allow_commit)") != std::string::npos,
          "3654 AC4: elision still ANDs fast-path + live commit_readiness");
    CHECK(jit.find("CreateOr(is_unsafe, lin_unsafe)") != std::string::npos,
          "3654 AC4: #3616 anon still emits linear_post_mutate_enforce");
    CHECK(read_file("tests/serve/test_issue_3654.cpp").empty(), "3654 AC5: no invent");
    CHECK(read_file("docs/design/3654-jit-linear-post-mutate-unset.md").empty(),
          "3654 AC5: no docs/design");
    CHECK(br.find("schema-3654") == std::string::npos && br.find("g_3654_") == std::string::npos,
          "3654 AC5: no new query key");
}

// ── Issue #3619: no-edge force + still-held = production residual ──
//   AC1: production + latched + no-edge counter != 0 + snapshot held →
//        residual-zero reader returns 0 and try_acquire rejects with
//        AdmissionRejected: production-residual-sticky. Holder exit drops
//        held → reader returns 1 (sticky reset).
//   AC2: no unlock path added (thief-cannot-unlock unchanged — #3222 AC2
//        rows above; this change adds reads only).
//   AC3: Soft / unlatched → reader returns 1 (observe-only) with the same
//        counter + held.
//   AC4: no new metric / query key (reuse g_hold_budget_no_edge_force_
//        total + existing sticky bit).
//   AC6: source-cite the probe + residual-zero reader + try_acquire
//        sticky reject.
static void ac3619_no_edge_held_residual() {
    std::println("\n--- #3619: no-edge held residual → Ready/admit fail-closed ---");
    const auto sh = read_file("src/serve/steal_safety.h");
    const auto fcpp = read_file("src/serve/fiber.cpp");
    const auto bridge = read_file("src/compiler/fiber_bridge.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(sh.find("aura_mutation_hold_no_edge_still_held") != std::string::npos,
          "3619 AC6: residual-zero reader consults the probe");
    CHECK(sh.find("Issue #3619") != std::string::npos, "3619 AC6: reader fold cites #3619");
    CHECK(fcpp.find("g_hold_budget_no_edge_force_total.fetch_add") != std::string::npos,
          "3619 AC6: inbody-window no-edge bump");
    CHECK(fcpp.find("aura_mutation_hold_no_edge_still_held") != std::string::npos,
          "3619 AC6: strong probe def in fiber.cpp");
    CHECK(bridge.find("aura_mutation_hold_no_edge_still_held") != std::string::npos,
          "3619 AC6: weak stub (light link observe-only)");
    CHECK(mb.find("AdmissionRejected: production-residual-sticky") != std::string::npos,
          "3619 AC6: try_acquire sticky reject (existing bus)");
    CHECK(sh.find("g_3619_") == std::string::npos && sh.find("schema-3619") == std::string::npos,
          "3619 AC4: no new counter / query key");
    CHECK(read_file("tests/serve/test_issue_3619.cpp").empty(), "3619: no invent");
    CHECK(read_file("docs/design/3619-no-edge-held-residual.md").empty(), "3619: no docs/design");

    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define f (lambda (x) x))
")
)")
              .has_value(),
          "3619: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3619: eval");

    // AC3: unlatched + Soft → observe-only (reader 1 even with counter+held).
    aura::serve::set_production_multi_worker_latched_for_test(false);
    auto prod_save = aura::compiler::typed_audit::g_typed_mutation_audit_counters
                         .production_defaults_active.load(std::memory_order_relaxed);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
    aura::compiler::clear_hold_budget_no_edge_force_for_test();
    aura::compiler::g_hold_budget_no_edge_force_total.store(2, std::memory_order_relaxed);
    bool hold_ok = false;
    {
        aura::compiler::Evaluator::MutationBoundaryGuard g(cs.evaluator(), &hold_ok);
        CHECK(aura::serve::steal_safety_production_residual_zero_v_read() == 1,
              "3619 AC3: unlatched → reader observe-only (returns 1)");
    }

    // AC1: production + latched + no-edge + held → reader 0 + admit reject.
    // Guard ctor runs BEFORE the counter forces so the admit itself is
    // clean; the residual then refuse nested admits until held drops.
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        1, std::memory_order_relaxed);
    aura::serve::set_production_multi_worker_latched_for_test(true);
    {
        aura::compiler::Evaluator::MutationBoundaryGuard g(cs.evaluator(), &hold_ok);
        CHECK(aura::serve::steal_safety_production_residual_zero_v_read() == 0,
              "3619 AC1: no-edge + still held → residual-zero 0");
        auto gr = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
            cs.evaluator(), /*pending=*/1, &hold_ok);
        CHECK(!gr.has_value(), "3619 AC1: try_acquire rejected under residual sticky");
        if (!gr.has_value())
            std::println("  3619 AC1: reject msg={}", gr.error().message);
        // The #2701 budget arm may pre-empt the sticky arm here (the held
        // Guard IS the over-budget holder once no-edge forces fired — its
        // force-degrade is the remediation). AC1's observable: the sticky
        // production-residual bit is set and the admit is fail-closed.
        CHECK(aura::serve::steal_safety_production_residual_sticky_fail_v_read() != 0,
              "3619 AC1: production-residual sticky set");
    }
    // Holder exited → held drops → residual clears (sticky reset).
    CHECK(aura::serve::steal_safety_production_residual_zero_v_read() == 1,
          "3619 AC1: held drops → residual clears (sticky reset)");

    // Cleanup.
    aura::compiler::clear_hold_budget_no_edge_force_for_test();
    aura::serve::set_production_multi_worker_latched_for_test(false);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        prod_save, std::memory_order_relaxed);
}

int run_test_steal_complete_strong_entry() {
    std::println("=== Issue #2377: steal-complete strong entry contract ===");

    // AC1: production path requires strong ABI (source + lock API)
    {
        std::println("\n--- AC1: production strong steal-complete ---");
        const auto wc = read_file("src/serve/worker.cpp");
        const auto fb = read_file("src/compiler/fiber_bridge.cpp");
        CHECK(wc.find("Issue #2377") != std::string::npos, "AC1: worker cites #2377");
        CHECK(wc.find("steal_snapshot_soft_production_locked") != std::string::npos,
              "AC1: worker production lock gate");
        CHECK(wc.find("std::abort()") != std::string::npos, "AC1: worker abort on null under prod");
        CHECK(wc.find("call_steal_complete") != std::string::npos, "AC1: call_steal_complete");
        CHECK(fb.find("Issue #2377") != std::string::npos, "AC1: fiber_bridge cites #2377");
        CHECK(fb.find("steal_snapshot_soft_production_locked") != std::string::npos,
              "AC1: weak stub production-aware");
        // Production lock round-trip exists (#2372 Soft + #2377 steal-complete).
        const bool saved = aura::serve::steal_snapshot_soft_production_locked();
        aura::serve::set_steal_snapshot_soft_production_locked(true);
        CHECK(aura::serve::steal_snapshot_soft_production_locked(), "AC1: lock on");
        aura::serve::set_steal_snapshot_soft_production_locked(false);
        CHECK(!aura::serve::steal_snapshot_soft_production_locked(), "AC1: lock off");
        aura::serve::set_steal_snapshot_soft_production_locked(saved);
    }

    // AC2: steal-complete transaction order in strong entry
    {
        std::println("\n--- AC2: strong entry transaction order ---");
        const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(fm.find("steal-complete transaction") != std::string::npos,
              "AC2: transaction documented");
        CHECK(fm.find("clear_gc_defer_for_evaluator") != std::string::npos, "AC2: Panic clear");
        CHECK(fm.find("force_clear_residual_defer_for_evaluator") != std::string::npos,
              "AC2: residual interlock");
        CHECK(fm.find("has_resume_layout_stamp") != std::string::npos, "AC2: LayoutStamp check");
        CHECK(fm.find("aura_evaluator_probe_linear_on_steal") != std::string::npos,
              "AC2: linear fold");
        // Order within on_steal_complete body only (file may cite helpers earlier).
        const auto fn = fm.find("extern \"C\" void aura_evaluator_on_steal_complete");
        CHECK(fn != std::string::npos, "AC2: strong entry present");
        // Window covers Panic clear → residual → LayoutStamp after
        // #3048/#3209 session-revoke lead-in + #3343 probe marker.
        const auto body = fm.substr(fn, 16000);
        const auto i_clear = body.find("clear_gc_defer_for_evaluator");
        const auto i_resid = body.find("force_clear_residual_defer_for_evaluator");
        const auto i_stamp = body.find("has_resume_layout_stamp");
        CHECK(i_clear != std::string::npos && i_resid != std::string::npos &&
                  i_stamp != std::string::npos && i_clear < i_resid && i_resid < i_stamp,
              "AC2: order Panic clear → residual → LayoutStamp");
    }

    // AC3: missing-entry metric API + Soft path allowed off production
    {
        std::println("\n--- AC3: missing-entry metric (sandbox path) ---");
        const auto before = aura::gc_hooks::steal_complete_entry_missing_total();
        aura::gc_hooks::bump_steal_complete_entry_missing_total();
        CHECK(aura::gc_hooks::steal_complete_entry_missing_total() == before + 1,
              "AC3: missing total bumps");
        const auto gh = read_file("src/core/gc_hooks.h");
        CHECK(gh.find("g_steal_complete_entry_missing_total") != std::string::npos,
              "AC3: counter in gc_hooks");
        CHECK(gh.find("Issue #2377") != std::string::npos, "AC3: cites #2377");
    }

    // AC4: zero-cost notes for residual 0 / stamp unset
    {
        std::println("\n--- AC4: zero-cost happy path documented ---");
        const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(fm.find("zero cost when") != std::string::npos ||
                  fm.find("Zero cost") != std::string::npos || fm.find("AC4") != std::string::npos,
              "AC4: zero-cost note present");
        CHECK(fm.find("defer_reasons_snapshot()") != std::string::npos, "AC4: residual load");
    }

    // AC5: query schema-2377 + source-cite
    {
        std::println("\n--- AC5: query schema-2377 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2377") == 2377, "AC5: schema-2377");
        CHECK(href(cs, "issue-2377") == 2377, "AC5: issue-2377");
        CHECK(href(cs, "steal-complete-strong-required-wired") == 1, "AC5: strong-required wired");
        CHECK(href(cs, "steal-complete-entry-missing-total") >= 0, "AC5: missing total key");
        CHECK(href(cs, "steal-complete-total") >= 0, "AC5: steal-complete-total retained");
        CHECK(href(cs, "schema-2203") == 2203, "AC5: 2203 retained");
        CHECK(href(cs, "schema-2314") == 2314 ||
                  href(cs, "residual-defer-steal-interlock-wired") == 1,
              "AC5: residual lineage present");

        const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(q.find("schema-2377") != std::string::npos, "AC5: query cites schema");
        CHECK(q.find("steal-complete-entry-missing-total") != std::string::npos,
              "AC5: query missing key");
    }

    // ── Issue #2955: production ABI self-check ──
    {
        std::println("\n=== Issue #2955: production ABI self-check ===");
        using aura::serve::aura_runtime_require_production_abi;
        using aura::serve::clear_production_abi_selfcheck_for_test;
        using aura::serve::g_production_abi_selfcheck_fail_total;
        using aura::serve::g_production_abi_selfcheck_ok_total;
        using aura::serve::production_abi_selfcheck_required;

        // AC2: Soft / no production_defaults → not required; no abort
        {
            std::println("\n--- #2955 AC2: Soft path no forced abort ---");
            clear_production_abi_selfcheck_for_test();
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(0, std::memory_order_relaxed);
            unsetenv("AURA_SANDBOX");
            CHECK(!production_abi_selfcheck_required(), "2955 AC2: Soft not required");
            const auto fail0 =
                g_production_abi_selfcheck_fail_total.load(std::memory_order_relaxed);
            CHECK(aura_runtime_require_production_abi(), "2955 AC2: Soft require returns true");
            CHECK(g_production_abi_selfcheck_fail_total.load(std::memory_order_relaxed) == fail0,
                  "2955 AC2: Soft no fail bump");
        }

        // AC2b: sandbox=off opts out even with production_defaults
        {
            std::println("\n--- #2955 AC2: sandbox=off opts out ---");
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            setenv("AURA_SANDBOX", "off", 1);
            CHECK(!production_abi_selfcheck_required(), "2955 AC2: sandbox=off not required");
            CHECK(aura_runtime_require_production_abi(), "2955 AC2: sandbox=off require ok");
            unsetenv("AURA_SANDBOX");
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(0, std::memory_order_relaxed);
        }

        // AC3: full production link (this binary links strong evaluator + fiber)
        // with production_defaults → ok path when markers present
        {
            std::println("\n--- #2955 AC3: full link self-check ok ---");
            clear_production_abi_selfcheck_for_test();
            unsetenv("AURA_SANDBOX");
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            CHECK(production_abi_selfcheck_required(), "2955 AC3: production requires check");
            // Strong markers present in this link unit (evaluator_fiber_mutation + fiber).
            CHECK(aura_abi_strong_steal_complete_v() == 1, "2955 AC3: steal-complete strong");
            CHECK(aura_abi_strong_fiber_eval_id_v() == 1, "2955 AC3: fiber eval-id strong");
            CHECK(aura_abi_strong_mutation_held_v() == 1, "2955 AC3: mutation held strong");
            CHECK(aura_abi_strong_mutation_depth_from_ptr_v() == 1,
                  "2955 AC3: depth-from-ptr strong");
            const auto ok0 = g_production_abi_selfcheck_ok_total.load(std::memory_order_relaxed);
            CHECK(aura_runtime_require_production_abi(), "2955 AC3: require ok");
            CHECK(g_production_abi_selfcheck_ok_total.load(std::memory_order_relaxed) == ok0 + 1,
                  "2955 AC3: ok_total +1");
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(0, std::memory_order_relaxed);
            clear_production_abi_selfcheck_for_test();
        }

        // AC1 source: production required + missing marker → abort (source-cite)
        {
            std::println("\n--- #2955 AC1: fail path source (abort on missing strong) ---");
            const auto cpp = read_file("src/serve/runtime_production_abi.cpp");
            const auto main_c = read_file("src/main.cpp");
            CHECK(cpp.find("std::abort()") != std::string::npos, "2955 AC1: abort on fail");
            CHECK(cpp.find("production ABI self-check failed") != std::string::npos,
                  "2955 AC1: FATAL message");
            CHECK(main_c.find("aura_runtime_require_production_abi") != std::string::npos,
                  "2955 AC1: main calls self-check after production defaults");
            CHECK(main_c.find("apply_production_security_defaults") != std::string::npos,
                  "2955 AC1: production defaults before self-check");
            // Order: defaults then self-check
            const auto dpos = main_c.find("apply_production_security_defaults");
            const auto cpos = main_c.find("aura_runtime_require_production_abi");
            CHECK(dpos != std::string::npos && cpos != std::string::npos && dpos < cpos,
                  "2955 AC1: defaults before self-check order");
        }

        // AC4/AC5: query + source
        {
            std::println("\n--- #2955 AC4–AC5: query + source-cite ---");
            CompilerService cs;
            CHECK(href(cs, "schema-2955") == 2955, "2955 AC4: schema-2955");
            CHECK(href(cs, "issue-2955") == 2955, "2955 AC4: issue-2955");
            CHECK(href(cs, "production-abi-selfcheck-wired") == 1, "2955 AC4: wired sentinel");
            CHECK(href(cs, "production-abi-selfcheck-ok-total") >= 0, "2955 AC4: ok-total");
            CHECK(href(cs, "production-abi-selfcheck-fail-total") >= 0, "2955 AC4: fail-total");
            CHECK(href(cs, "schema-2377") == 2377, "2955 AC4: schema-2377 preserved");

            const auto hh = read_file("src/serve/runtime_production_abi.h");
            const auto cpp = read_file("src/serve/runtime_production_abi.cpp");
            const auto fb = read_file("src/compiler/fiber_bridge.cpp");
            const auto build = read_file("build.py");
            const auto lint =
                read_file("scripts/coverage/checks/check_production_abi_selfcheck_2955.py");
            CHECK(hh.find("Issue #2955") != std::string::npos, "2955 AC5: header cites #2955");
            CHECK(cpp.find("Issue #2955") != std::string::npos ||
                      cpp.find("#2955") != std::string::npos,
                  "2955 AC5: cpp cites #2955");
            CHECK(fb.find("aura_abi_strong_steal_complete_v") != std::string::npos,
                  "2955 AC5: weak markers in fiber_bridge");
            CHECK(build.find("check_production_abi_selfcheck_2955") != std::string::npos,
                  "2955 AC5: build.py wires linter");
            CHECK(!lint.empty(), "2955 AC5: linter present");
            CHECK(read_file("docs/design/2955-production-abi-selfcheck.md").empty(),
                  "2955 AC6: no docs/design/");
            CHECK(read_file("tests/serve/test_issue_2955.cpp").empty(),
                  "2955 AC6: no invent test file");
        }

        // ── Issue #3098: production multi-worker Ready must refuse Soft fall-through.
        // AND-s strong ABI + production_defaults_active. Reuses existing
        // production_abi_selfcheck_* counters + bit 4 (defaults missing).
        {
            std::println("\n=== Issue #3098: production multi-worker Ready refuses Soft ===");
            using aura::serve::aura_runtime_require_production_multi_worker;
            using aura::serve::g_production_abi_selfcheck_last_fail_bits;
            using aura::serve::kProductionAbiSelfcheckFailBitDefaults;

            // AC1 source-cite: bit 4 reserved for #3098 defaults missing.
            CHECK(kProductionAbiSelfcheckFailBitDefaults == (1ull << 4),
                  "3098 AC1: bit 4 reserved for defaults missing");

            // AC2: production_defaults=1 + sandbox unset + strong markers →
            // multi_worker returns true + ok_total +1 + last_fail_bits = 0.
            {
                std::println("\n--- #3098 AC2: production multi-worker ok path ---");
                aura::serve::clear_steal_safety_transaction_for_test();
                clear_production_abi_selfcheck_for_test();
                unsetenv("AURA_SANDBOX");
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(1, std::memory_order_relaxed);
                const auto ok0 =
                    g_production_abi_selfcheck_ok_total.load(std::memory_order_relaxed);
                const auto fail0 =
                    g_production_abi_selfcheck_fail_total.load(std::memory_order_relaxed);
                CHECK(aura_runtime_require_production_multi_worker(),
                      "3098 AC2: production + ABI ok → multi_worker returns true");
                CHECK(g_production_abi_selfcheck_ok_total.load(std::memory_order_relaxed) ==
                          ok0 + 1,
                      "3098 AC2: ok_total +1");
                CHECK(g_production_abi_selfcheck_fail_total.load(std::memory_order_relaxed) ==
                          fail0,
                      "3098 AC2: fail_total unchanged (no abort)");
                CHECK(g_production_abi_selfcheck_last_fail_bits.load(std::memory_order_relaxed) ==
                          0,
                      "3098 AC2: last_fail_bits cleared to 0 on success");
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(0, std::memory_order_relaxed);
                clear_production_abi_selfcheck_for_test();
            }

            // AC4 source-cite: existing counters reused + bit 4 + new function in main.cpp.
            {
                std::println("\n--- #3098 AC4 source-cite ---");
                const auto main_c = read_file("src/main.cpp");
                const auto hh = read_file("src/serve/runtime_production_abi.h");
                const auto cpp = read_file("src/serve/runtime_production_abi.cpp");
                CHECK(cpp.find("aura_runtime_require_production_multi_worker") != std::string::npos,
                      "3098 AC4: cpp defines multi_worker");
                CHECK(cpp.find("kProductionAbiSelfcheckFailBitDefaults") != std::string::npos,
                      "3098 AC4: cpp uses bit 4 for defaults missing");
                CHECK(cpp.find("multi-worker Ready self-check failed (#3098") != std::string::npos,
                      "3098 AC4: cpp FATAL message cites #3098");
                CHECK(hh.find("aura_runtime_require_production_multi_worker") != std::string::npos,
                      "3098 AC4: hh declares multi_worker");
                CHECK(hh.find("kProductionAbiSelfcheckFailBitDefaults") != std::string::npos,
                      "3098 AC4: hh defines bit 4 constant");
                CHECK(main_c.find("aura_runtime_require_production_multi_worker_c") !=
                          std::string::npos,
                      "3098 AC4: main.cpp wires multi-worker Ready check after ABI check");
                // Order: single-worker ABI check → multi-worker Ready check
                const auto single_pos = main_c.find("aura_runtime_require_production_abi_c");
                const auto multi_pos =
                    main_c.find("aura_runtime_require_production_multi_worker_c");
                CHECK(single_pos != std::string::npos && multi_pos != std::string::npos &&
                          single_pos < multi_pos,
                      "3098 AC4: single-worker ABI check before multi-worker Ready check");
            }

            // AC5 source-cite: C-linkage accessor present (issue body #2955 lineage).
            {
                std::println("\n--- #3098 AC5 source-cite ---");
                const auto hh = read_file("src/serve/runtime_production_abi.h");
                CHECK(hh.find("aura_runtime_require_production_multi_worker_c") !=
                          std::string::npos,
                      "3098 AC5: C-linkage accessor declared");
                CHECK(read_file("tests/serve/test_issue_3098.cpp").empty(),
                      "3098 AC5: no invent test file (extend #81967 lineage)");
            }
        }

        // ── Issue #3476: Scheduler::run welds Ready before WorkerThread::start.
        {
            std::println("\n=== Issue #3476: Scheduler::run production Ready weld ===");
            const auto sched_cpp = read_file("src/serve/scheduler.cpp");
            const auto run_pos = sched_cpp.find("void Scheduler::run()");
            CHECK(run_pos != std::string::npos, "3476 AC6: Scheduler::run found");
            const auto start_pos = sched_cpp.find("w->start()", run_pos);
            const auto multi_pos =
                sched_cpp.find("aura_runtime_require_production_multi_worker", run_pos);
            const auto abi_pos = sched_cpp.find("aura_runtime_require_production_abi()", run_pos);
            const auto size_pos = sched_cpp.find("workers_.size() > 1", run_pos);
            CHECK(size_pos != std::string::npos && size_pos < start_pos,
                  "3476 AC1: workers_.size()>1 gate before start");
            CHECK(multi_pos != std::string::npos && multi_pos < start_pos,
                  "3476 AC1: multi-worker Ready before WorkerThread::start");
            CHECK(abi_pos != std::string::npos && abi_pos < start_pos,
                  "3476 AC3: single-worker uses require_production_abi before start");
            CHECK(sched_cpp.find("Issue #3476") != std::string::npos, "3476 AC6: scheduler cites");
            CHECK(sched_cpp.find("g_3476_") == std::string::npos, "3476 AC4: no new g_3476_*");
            CHECK(read_file("docs/design/3476-scheduler-ready-weld.md").empty(),
                  "3476 AC6: no docs/design");
            CHECK(read_file("tests/serve/test_issue_3476.cpp").empty(), "3476 AC5: no invent");

            // AC3 live: single-worker Soft does not abort (require_production_abi).
            {
                std::println("\n--- #3476 AC3: single-worker Soft run ---");
                aura::serve::clear_production_abi_selfcheck_for_test();
                aura::compiler::typed_audit::apply_dev_audit_defaults();
                aura::serve::Scheduler one(1);
                std::thread t([&one]() { one.run(); });
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                CHECK(aura_runtime_multi_worker_production_latched() == 0,
                      "3476 AC3: single-worker does not latch multi-worker");
                one.stop();
                t.join();
            }

            // AC2 live: production + workers>1 Ready latches for the process.
            {
                std::println("\n--- #3476 AC2: production multi-worker run latches ---");
                aura::serve::clear_steal_safety_transaction_for_test();
                aura::serve::clear_production_abi_selfcheck_for_test();
                unsetenv("AURA_SANDBOX");
                aura::compiler::typed_audit::apply_production_audit_defaults();
                aura::serve::Scheduler many(2);
                std::thread t([&many]() { many.run(); });
                int latched = 0;
                for (int i = 0; i < 80 && latched == 0; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    latched = aura_runtime_multi_worker_production_latched();
                }
                CHECK(latched == 1, "3476 AC2: Ready latched after Scheduler::run");
                many.stop();
                t.join();
                aura::compiler::typed_audit::apply_dev_audit_defaults();
                aura::serve::clear_production_abi_selfcheck_for_test();
            }
        }

        // ── Issue #3195: multi-worker residual-zero sticky + Soft misconfig ──
        {
            std::println("\n=== Issue #3195: multi-worker residual-zero sticky ===");
            using aura::serve::clear_production_abi_selfcheck_for_test;
            using aura::serve::clear_steal_safety_transaction_for_test;
            using aura::serve::g_steal_safety_residual_boundary_unsafe_total;
            using aura::serve::steal_safety_production_residual_sticky_fail_v_read;
            using aura::serve::steal_safety_production_residual_zero_v_read;

            // AC2: Soft / single-worker (latch unset): residual is metric-only.
            {
                std::println("\n--- #3195 AC2: Soft / unlatched pass-through ---");
                clear_steal_safety_transaction_for_test();
                clear_production_abi_selfcheck_for_test();
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(0, std::memory_order_relaxed);
                g_steal_safety_residual_boundary_unsafe_total.store(1, std::memory_order_relaxed);
                CHECK(steal_safety_production_residual_zero_v_read() == 1,
                      "3195 AC2: Soft unlatched residual_zero pass-through");
                CHECK(steal_safety_production_residual_sticky_fail_v_read() == 0,
                      "3195 AC2: Soft unlatched sticky stays 0");
                g_steal_safety_residual_boundary_unsafe_total.store(0, std::memory_order_relaxed);
            }

            // AC1: latched multi-worker + named residual → SSOT 0 + sticky 1,
            // even after production_defaults is flipped Soft (I3/I6).
            {
                std::println("\n--- #3195 AC1: latched residual fail-closed ---");
                clear_steal_safety_transaction_for_test();
                clear_production_abi_selfcheck_for_test();
                unsetenv("AURA_SANDBOX");
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(1, std::memory_order_relaxed);
                CHECK(aura::serve::aura_runtime_require_production_multi_worker(),
                      "3195 AC1: multi-worker Ready ok (clean residual)");
                CHECK(aura::serve::g_production_multi_worker_latched.load(
                          std::memory_order_relaxed) == 1,
                      "3195 AC1: Ready latches multi-worker");
                g_steal_safety_residual_boundary_unsafe_total.store(1, std::memory_order_relaxed);
                CHECK(steal_safety_production_residual_zero_v_read() == 0,
                      "3195 AC1: BoundaryUnsafe fail-closes residual_zero");
                CHECK(steal_safety_production_residual_sticky_fail_v_read() == 1,
                      "3195 AC1: sticky set on residual under latch");
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(0, std::memory_order_relaxed);
                CHECK(steal_safety_production_residual_zero_v_read() == 0,
                      "3195 AC1: Soft-misconfig still fail-closed");
                CHECK(steal_safety_production_residual_sticky_fail_v_read() == 1,
                      "3195 AC1: sticky survives Soft flip");
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(0, std::memory_order_relaxed);
                clear_steal_safety_transaction_for_test();
                clear_production_abi_selfcheck_for_test();
            }

            // AC3/AC4 source-cite: bit 5 + no new counters.
            {
                std::println("\n--- #3195 AC3/AC4 source-cite ---");
                const auto hh = read_file("src/serve/runtime_production_abi.h");
                const auto cpp = read_file("src/serve/runtime_production_abi.cpp");
                CHECK(aura::serve::kProductionAbiSelfcheckFailBitResidualSticky == (1ull << 5),
                      "3195 AC3: bit 5 reserved for residual sticky");
                CHECK(hh.find("kProductionAbiSelfcheckFailBitResidualSticky") != std::string::npos,
                      "3195 AC3: header bit 5");
                CHECK(cpp.find("g_steal_safety_production_residual_sticky_fail_wired") !=
                          std::string::npos,
                      "3195 AC3: Ready requires sticky wired");
                CHECK(cpp.find("#3195") != std::string::npos, "3195 AC3: cpp cites #3195");
                CHECK(read_file("tests/serve/test_issue_3195.cpp").empty(),
                      "3195 AC5: no invent test file");
            }
        }
    }

    // ── Issue #3343: production weak probe_linear_on_steal fail-closed ──
    {
        std::println("\n=== Issue #3343: production weak linear-on-steal fail-closed ===");
        const auto fb = read_file("src/compiler/fiber_bridge.cpp");
        const auto wc = read_file("src/serve/worker.cpp");
        const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        const auto hh = read_file("src/serve/runtime_production_abi.h");
        const auto cpp = read_file("src/serve/runtime_production_abi.cpp");
        const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_production_weak_abi_commit_readiness_3343.py");
        const auto build = read_file("build.py");

        CHECK(fb.find("Issue #3343") != std::string::npos, "3343 AC1: fiber_bridge cites #3343");
        CHECK(fb.find("aura_evaluator_probe_linear_on_steal") != std::string::npos,
              "3343 AC1: weak probe present");
        {
            const auto p =
                fb.find("__attribute__((weak, used)) void aura_evaluator_probe_linear_on_steal");
            CHECK(p != std::string::npos, "3343 AC1: weak used probe");
            const auto body = fb.substr(p, 900);
            CHECK(body.find("steal_snapshot_soft_production_locked") != std::string::npos,
                  "3343 AC1: weak probe production-aware");
            CHECK(body.find("std::abort()") != std::string::npos, "3343 AC1: weak probe aborts");
        }
        {
            const auto p = wc.find("static inline void call_probe_linear_on_steal");
            CHECK(p != std::string::npos, "3343 AC1: worker call_probe");
            const auto body = wc.substr(p, 1100);
            CHECK(body.find("steal_snapshot_soft_production_locked") != std::string::npos,
                  "3343 AC1: worker null-ref production lock");
            CHECK(body.find("std::abort()") != std::string::npos, "3343 AC1: worker null abort");
            CHECK(body.find("Issue #3343") != std::string::npos, "3343 AC1: worker cites #3343");
        }
        CHECK(fm.find("extern \"C\" void aura_evaluator_probe_linear_on_steal()") !=
                  std::string::npos,
              "3343 AC2: strong probe def");
        CHECK(fm.find("probe_and_repin_linear_on_steal") != std::string::npos,
              "3343 AC3: strong probe re-pins");
        CHECK(fm.find("note_escape_gate_clear_on_steal") != std::string::npos,
              "3343 AC3: steal-complete clears escape");
        CHECK(aura_abi_strong_probe_linear_on_steal_v() == 1,
              "3343 AC2: this link unit has strong probe marker");
        CHECK(aura::serve::kProductionAbiSelfcheckFailBitProbeLinear == (1ull << 7),
              "3343 AC2: fail bit 7");
        CHECK(hh.find("kProductionAbiSelfcheckFailBitProbeLinear") != std::string::npos,
              "3343 AC2: header bit 7");
        CHECK(cpp.find("aura_abi_strong_probe_linear_on_steal_v() == 0") != std::string::npos,
              "3343 AC2: self-check requires probe marker");
        CHECK(aura::serve::kProductionAbiSelfcheckFailBitTypedEntry == (1ull << 8),
              "3419 AC2: fail bit 8");
        CHECK(hh.find("kProductionAbiSelfcheckFailBitTypedEntry") != std::string::npos,
              "3419 AC2: header bit 8");
        CHECK(cpp.find("aura_abi_strong_ir_typed_entry_v() == 0") != std::string::npos,
              "3419 AC2: self-check requires typed-entry marker");
        CHECK(aura_abi_strong_ir_typed_entry_v() == 1,
              "3419 AC2: this link unit has strong typed-entry marker");
        CHECK(stub.find("Issue #3343") != std::string::npos, "3343 AC4: JIT stub cites #3343");
        {
            auto& pda = aura::compiler::typed_audit::g_typed_mutation_audit_counters
                            .production_defaults_active;
            const auto saved = pda.load(std::memory_order_relaxed);
            pda.store(0, std::memory_order_relaxed);
            CHECK(aura_jit_ir_typed_entry_commit_readiness_ok() == 1,
                  "3343 AC4: Soft stub allows IR entry");
            CHECK(aura_jit_linear_move_drop_elision_ok() == 1,
                  "3343 AC4: Soft stub allows elision");
            CHECK(aura_jit_linear_post_mutate_enforce(0) == 0,
                  "3343 AC4: Soft stub post-mutate pass-through");
            pda.store(1, std::memory_order_relaxed);
            CHECK(aura_jit_ir_typed_entry_commit_readiness_ok() == 0,
                  "3343 AC1: production stub refuses IR entry");
            CHECK(aura_jit_linear_move_drop_elision_ok() == 0,
                  "3343 AC1: production stub blocks elision");
            CHECK(aura_jit_linear_post_mutate_enforce(0) == 1,
                  "3343 AC1: production stub post-mutate unsafe");
            pda.store(saved, std::memory_order_relaxed);
        }
        CHECK(!lint.empty() && lint.find("3343") != std::string::npos, "3343 AC5: linter");
        CHECK(build.find("check_production_weak_abi_commit_readiness_3343") != std::string::npos,
              "3343 AC5: build.py");
        CHECK(read_file("docs/design/3343-production-weak-abi-commit-readiness.md").empty(),
              "3343 AC5: no docs/design");
        CHECK(read_file("tests/serve/test_issue_3343.cpp").empty(), "3343 AC5: no invent");
        CHECK(cpp.find("schema-3343") == std::string::npos, "3343 AC5: no schema-3343");
    }

    ac3619_no_edge_held_residual();
    ac3654_linear_post_mutate_unset_fail_closed();
    std::println(
        "\n=== #2377 + #2955 + #3098 + #3195 + #3343 + #3654 results: {} passed, {} failed ===",
        g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_complete_strong_entry();
}
#endif
