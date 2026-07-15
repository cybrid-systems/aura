// test_issue_1402.cpp — Issue #1402: Primitive security-tier
// enforcement assert (kPrimSecPrivileged → capability gate contract).
//
// Background: primitives_meta.h:18-26 declares tier constants
// (kPrimSecSafe/Sandboxed/Privileged). The enforcement is currently
// hardcoded per-primitive (security:* primitives check kCapWildcard
// in evaluator_primitives_security.cpp; fiber:spawn checks kCapFiber
// in evaluator_primitives_messaging.cpp). The tier metadata itself
// is declarative — no dispatch-time check uses security_level.
//
// Fix (Option 1, security assertion): the hardcoded enforcement IS
// in place for the kPrimSecPrivileged primitives. This contract test
// locks the invariant: in sandbox_mode without the required
// capability, Privileged primitives return merr / std::unexpected.
//
// ACs:
//   AC1: security:set-sandbox-mode! in sandboxed mode without
//        kCapWildcard → merr (capability denied)
//   AC2: security:grant-capability! in sandboxed mode without
//        kCapWildcard → merr (capability denied)
//   AC3: fiber:spawn in sandboxed mode without kCapFiber/kCapWildcard
//        → merr (capability denied)
//   AC4: After granting kCapWildcard, the same primitives work
//        (backward compat — sandbox admin not over-denied)
//
// cs.eval() returns std::expected<EvalValue, Diagnostic>. Cap denied
// manifests as either !r (std::unexpected) or r->is_error() (merr).

#include "test_harness.hpp"
import std;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace test_issue_1402_detail {

static void run_ac1_set_sandbox_mode_no_wildcard(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: security:set-sandbox-mode! without kCapWildcard ---");
    cs.evaluator().set_sandbox_mode(true);
    // Try to disable sandbox (change from true → false) — requires kCapWildcard.
    auto r = cs.eval("(security:set-sandbox-mode! #f)");
    const bool denied = !r || aura::compiler::types::is_error(*r);
    CHECK(denied, "security:set-sandbox-mode! #f in sandboxed mode without kCapWildcard → merr");
    // Sandbox mode should still be true (the deny didn't change state).
    auto mode = cs.eval("(stats:get \"security:sandbox-mode?\")");
    CHECK(mode && aura::compiler::types::is_bool(*mode) && aura::compiler::types::as_bool(*mode),
          "sandbox_mode still true after denied set-sandbox-mode! #f");
    cs.evaluator().set_sandbox_mode(false); // cleanup
}

static void run_ac2_grant_capability_no_wildcard(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: security:grant-capability! without kCapWildcard ---");
    cs.evaluator().set_sandbox_mode(true);
    auto r = cs.eval("(security:grant-capability! \"fiber\")");
    const bool denied = !r || aura::compiler::types::is_error(*r);
    CHECK(denied, "security:grant-capability! \"fiber\" in sandboxed mode without "
                  "kCapWildcard → merr");
    cs.evaluator().set_sandbox_mode(false); // cleanup
}

static void run_ac3_fiber_spawn_no_cap(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: fiber:spawn without kCapFiber/kCapWildcard ---");
    cs.evaluator().set_sandbox_mode(true);
    // Define a no-op closure to spawn.
    cs.eval("(define spawn-test-closure (lambda () 42))");
    auto r = cs.eval("(fiber:spawn spawn-test-closure)");
    const bool denied = !r || aura::compiler::types::is_error(*r);
    CHECK(denied, "fiber:spawn in sandboxed mode without kCapFiber/kCapWildcard → merr");
    cs.evaluator().set_sandbox_mode(false); // cleanup
}

static void run_ac4_with_wildcard_works(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: with kCapWildcard, Privileged primitives work ---");
    cs.evaluator().set_sandbox_mode(true);
    // Issue #1402: kCapWildcard constant = "*" (security_capabilities.h:12).
    // Use the literal string directly — avoids needing to import the
    // security module just for the constant.
    cs.evaluator().grant_capability("*");
    // Now set-sandbox-mode! should succeed (sandboxed → open).
    auto r1 = cs.eval("(security:set-sandbox-mode! #f)");
    const bool r1_denied = !r1 || (r1 && aura::compiler::types::is_error(*r1));
    CHECK(!r1_denied, "security:set-sandbox-mode! #f with kCapWildcard succeeds (no merr)");
    auto mode = cs.eval("(stats:get \"security:sandbox-mode?\")");
    CHECK(mode && aura::compiler::types::is_bool(*mode) && !aura::compiler::types::as_bool(*mode),
          "sandbox_mode now false after kCapWildcard-granted set-sandbox-mode! #f");
    // Cleanup
    cs.evaluator().set_sandbox_mode(false);
}

} // namespace test_issue_1402_detail

int aura_issue_1402_run() {
    using namespace test_issue_1402_detail;
    std::println("=== Issue #1402: kPrimSecPrivileged capability gate contract ===");
    {
        aura::compiler::CompilerService cs;
        run_ac1_set_sandbox_mode_no_wildcard(cs);
        run_ac2_grant_capability_no_wildcard(cs);
        run_ac3_fiber_spawn_no_cap(cs);
        run_ac4_with_wildcard_works(cs);
    }
    std::println("\nResults: {}/{} passed, {}/{} failed", ::aura::test::g_passed,
                 ::aura::test::g_passed + ::aura::test::g_failed, ::aura::test::g_failed,
                 ::aura::test::g_passed + ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1402_run();
}
#endif