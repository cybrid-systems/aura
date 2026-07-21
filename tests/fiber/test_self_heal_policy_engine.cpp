// @category: integration
// @reason: Issue #1582 — policy-driven SelfHealing engine + graceful drain
// + quota/panic linkage + self_heal_success_total metrics.
//
//   AC1: policy engine API
//   AC2: quota / recoverable-panic auto heal or degrade
//   AC3: graceful drain observability
//   AC4: self_heal_success_total metrics
//   AC5: custom policy + hooks compose
//   AC6: panic-restore production path exercises engine (CompilerService)

#include "test_harness.hpp"

#include "core/self_healing_hooks.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <print>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

namespace sh = aura::core::self_heal;

static std::atomic<int> g_hook_hits{0};

static sh::PolicyResult custom_policy(const sh::HealErrorView& e) noexcept {
    if (e.kind == "custom")
        return {sh::HealAction::LimitedSelfMutate, true};
    return {sh::HealAction::None, false};
}

static void ac1_policy_engine_api() {
    std::println("\n--- AC1: policy engine API ---");
    sh::reset_self_heal_state_for_test();

    auto r_none = sh::default_self_heal_policy({.kind = "other", .message = "x", .code = 0});
    CHECK(r_none.action == sh::HealAction::None && !r_none.healed, "unknown kind → None");

    auto r_quota =
        sh::default_self_heal_policy({.kind = "quota-violation", .message = "memory", .code = 100});
    CHECK(r_quota.action == sh::HealAction::LimitedSelfMutate && r_quota.healed,
          "quota → LimitedSelfMutate");

    auto r_panic = sh::default_self_heal_policy(
        {.kind = "recoverable-panic", .message = "panic-restore", .code = 1});
    CHECK(r_panic.action == sh::HealAction::LimitedSelfMutate && r_panic.healed,
          "recoverable-panic → LimitedSelfMutate");

    CHECK(sh::is_quota_or_recoverable_panic(
              {.kind = "ResourceQuotaExceeded", .message = "", .code = 0}),
          "ResourceQuotaExceeded recognized");
}

static void ac2_quota_panic_auto() {
    std::println("\n--- AC2: quota/panic auto heal or degrade ---");
    sh::reset_self_heal_state_for_test();

    const auto s0 = sh::self_heal_success_total();
    // Simulate production quota-check trigger path.
    sh::trigger_self_healing({.kind = "quota-violation", .message = "fibers", .code = 10});
    CHECK(sh::self_heal_success_total() > s0, "success total advanced via trigger");
    CHECK(sh::self_heal_limited_mutate_total() >= 1, "limited mutate path");

    sh::clear_degraded_mode();
    const auto d0 = sh::self_heal_degrade_total();
    CHECK(sh::run_self_heal_engine(
              {.kind = "quota-violation", .message = "memory", .code = 2'000'000}),
          "high quota degrades");
    CHECK(sh::is_degraded_mode(), "degraded mode on");
    CHECK(sh::self_heal_degrade_total() > d0, "degrade total advanced");

    const auto s1 = sh::self_heal_success_total();
    CHECK(sh::run_self_heal_engine(
              {.kind = "recoverable-panic", .message = "panic-restore", .code = 0}),
          "panic heal");
    CHECK(sh::self_heal_success_total() > s1, "panic success counted");
}

static void ac3_graceful_drain() {
    std::println("\n--- AC3: graceful drain ---");
    sh::reset_self_heal_state_for_test();

    CHECK(!sh::is_graceful_drain_active(), "not draining initially");
    const auto r0 = sh::graceful_drain_requests();
    const auto g0 = sh::self_heal_graceful_drain_total();

    CHECK(sh::request_graceful_drain(/*reason=*/42), "request drain");
    CHECK(sh::is_graceful_drain_active(), "drain active");
    CHECK(sh::graceful_drain_requests() > r0, "requests advanced");
    CHECK(sh::self_heal_graceful_drain_total() > g0, "graceful_drain total advanced");

    CHECK(sh::request_graceful_drain(43), "second request ok");
    const auto c0 = sh::graceful_drain_completed();
    sh::complete_graceful_drain();
    CHECK(!sh::is_graceful_drain_active(), "drain completed");
    CHECK(sh::graceful_drain_completed() > c0, "completed counter advanced");

    CHECK(sh::run_self_heal_engine({.kind = "graceful-drain", .message = "shutdown", .code = 7}),
          "engine drain kind");
    CHECK(sh::is_graceful_drain_active(), "engine activated drain");
    sh::complete_graceful_drain();
}

static void ac4_metrics() {
    std::println("\n--- AC4: self_heal_success_total metrics ---");
    sh::reset_self_heal_state_for_test();
    CHECK(sh::self_heal_success_total() == 0, "reset success");
    CHECK(sh::self_heal_policy_runs() == 0, "reset policy runs");

    (void)sh::run_self_heal_engine({.kind = "quota-violation", .message = "time", .code = 1});
    (void)sh::run_self_heal_engine({.kind = "other", .message = "noop", .code = 0});
    CHECK(sh::self_heal_policy_runs() == 2, "policy runs = 2");
    CHECK(sh::self_heal_success_total() == 1, "only quota counted success");
    CHECK(sh::self_heal_triggers() == 2, "triggers = 2");
}

static void ac5_custom_policy_and_hooks() {
    std::println("\n--- AC5: custom policy + hooks ---");
    sh::reset_self_heal_state_for_test();
    g_hook_hits.store(0, std::memory_order_relaxed);

    sh::register_self_healing_hook([](const sh::HealErrorView& e) {
        if (e.kind == "custom") {
            g_hook_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    });
    sh::set_self_heal_policy_fn(&custom_policy);

    CHECK(sh::run_self_heal_engine({.kind = "custom", .message = "x", .code = 0}), "custom heals");
    CHECK(g_hook_hits.load() == 1, "hook fired");
    CHECK(sh::self_heal_success_total() >= 1, "success with custom policy");

    sh::clear_self_heal_policy();
    CHECK(sh::run_self_heal_engine({.kind = "quota-violation", .message = "memory", .code = 1}),
          "default policy after clear");
    sh::reset_self_heal_state_for_test();
}

static void ac6_panic_restore_production_path() {
    std::println("\n--- AC6: panic-restore production path ---");
    sh::reset_self_heal_state_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    auto& ev = cs.evaluator();
    const auto s0 = sh::self_heal_success_total();
    CHECK(ev.save_panic_checkpoint(), "save checkpoint");
    CHECK(cs.eval("(set-code \"(define x 2)\")").has_value(), "mutate after save");
    CHECK(ev.restore_panic_checkpoint(), "restore checkpoint");
    CHECK(sh::self_heal_success_total() > s0, "engine success via panic restore");
    CHECK(sh::self_heal_policy_runs() >= 1, "policy ran on restore");
    CHECK(true, "service path exercised");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::println("=== test_self_heal_policy_engine (#1582) ===");
    ac1_policy_engine_api();
    ac2_quota_panic_auto();
    ac3_graceful_drain();
    ac4_metrics();
    ac5_custom_policy_and_hooks();
    ac6_panic_restore_production_path();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
