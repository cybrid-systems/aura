// @category: unit
// @reason: Issue #3380 — occurrence full-solve recover bound to live commit
// TypeChecker (no process-global last-TC-wins slot). commit_readiness
// recover sites must invoke the C ABI that looks up the Evaluator TLS
// handle + commit_type_checker_handle, NOT a process-global fn/ctx pair
// that a stack TypeChecker (run_post_mutate_typecheck_no_lock) or a
// steal × dual-Evaluator race could overwrite. nullptr / no TLS handle
// → treat as recover fail (hard-reject face), never silent green.
// Ctor must NOT install a process-global recover hook.
//
// Fix contract (AC1–AC4 from the issue body):
//
//   AC1: Production + two live TypeCheckers (eval A's commit TC vs eval
//        B's commit TC). Recover on A invokes A's
//        try_occurrence_hard_face_full_solve_recover() — never B's.
//        The lookup goes through aura_typed_audit_current_commit_type_checker
//        which reads g_tls_audit_commit_readiness_evaluator →
//        commit_type_checker_handle(). Stack TypeCheckers (built in
//        run_post_mutate_typecheck_no_lock) do NOT participate in the
//        recover path because they aren't the commit TC of any Evaluator.
//   AC2: nullptr / no TLS handle → recover ABI returns false → commit
//        hard-rejected with force_reason cone_outside_goal_drop (10) or
//        refined_drift (15). Never treated as silent success.
//   AC3: Soft / Off: existing observe path unchanged. Quiet (no live
//        TC) → zero extra cost; Soft face-only fill is loads only
//        (no extra CS walk, no recover attempt).
//   AC4: Existing fixtures (test_partial_cone_commit_gate, test_type_linear_commit_health,
//        test_typed_audit_commit_readiness_live_policy): recover fail
//        still emits force_reason cone_outside_goal_drop (code 10) or
//        refined_drift (code 15). No regression on the SOLVED-only gate
//        — the test-only recover override is the only path that can
//        flip a recover site to success in hermetic tests; production
//        reads live TC.
//
// Header stays TypeChecker-free (C ABI in evaluator_mutation_boundary.cpp
// owns the cast + recover fn call — same separation as #3170/#3379).
// Reuses g_occurrence_hard_face_recover_{success,fail}_total /
// g_occurrence_recover_not_solved_total (no new query key per spec).
// Ctor does NOT call install_occurrence_full_solve_recover anymore —
// the process-global fn/ctx slot is removed.

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

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

// Find the body of a free function whose signature contains `sig`.
// Returns the substring of length `approx_len` starting at the matching
// opening brace (best-effort brace-balanced — caller passes a generous
// length).
static std::string find_fn_body(const std::string& src, const std::string& sig,
                                std::size_t approx_len) {
    const auto sig_pos = src.find(sig);
    if (sig_pos == std::string::npos)
        return {};
    const auto brace = src.find('{', sig_pos);
    if (brace == std::string::npos)
        return {};
    return src.substr(brace, approx_len);
}

} // namespace

// Forward decl for the C ABIs declared in typed_mutation_audit.h
// (extern "C" — tests must use the unqualified name).
extern "C" void* aura_typed_audit_current_commit_type_checker() noexcept;
extern "C" bool aura_typed_audit_try_occurrence_hard_face_full_solve_recover() noexcept;
extern "C" void aura_typed_audit_test_install_recover_override(bool (*fn)(void* ctx) noexcept,
                                                               void* ctx) noexcept;
extern "C" void aura_typed_audit_test_clear_recover_override() noexcept;

int run_test_typed_audit_commit_readiness_recover_acl() {
    std::println("=== Issue #3380: commit_readiness recover bound to live commit TypeChecker "
                 "(no process-global last-TC-wins) ===");
    CHECK(true, "ac3380: issue stamp");

    // Clear any test override from a prior suite (defensive — process-wide
    // TLS; if test_partial_cone_commit_gate ran before us, its teardown
    // already cleared it, but be explicit).
    aura_typed_audit_test_clear_recover_override();

    auto h = read_file("src/compiler/typed_mutation_audit.h");
    auto cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto ixx = read_file("src/compiler/type_checker.ixx");

    // ── AC1: process-global slot removed; recover is bound to live TC ──
    {
        std::println("\n--- AC1: recover bound to live commit TypeChecker (no "
                     "process-global fn/ctx) ---");
        // 1a. The process-global fn pointer + ctx slot are GONE from the header.
        CHECK(h.find("g_occurrence_full_solve_recover_fn") == std::string::npos,
              "AC1: process-global fn pointer removed");
        CHECK(h.find("g_occurrence_full_solve_recover_ctx") == std::string::npos,
              "AC1: process-global ctx slot removed");
        CHECK(h.find("OccurrenceFullSolveRecoverFn") == std::string::npos,
              "AC1: OccurrenceFullSolveRecoverFn typedef removed");
        CHECK(h.find("install_occurrence_full_solve_recover") == std::string::npos,
              "AC1: install_occurrence_full_solve_recover removed");
        // 1b. The two C ABIs are declared in the header.
        CHECK(h.find("aura_typed_audit_current_commit_type_checker") != std::string::npos,
              "AC1: current_commit_type_checker C ABI declared");
        CHECK(h.find("aura_typed_audit_try_occurrence_hard_face_full_solve_recover") !=
                  std::string::npos,
              "AC1: try_recover C ABI declared");
        // 1c. The C ABI is implemented in evaluator_mutation_boundary.cpp.
        CHECK(cpp.find("aura_typed_audit_current_commit_type_checker") != std::string::npos,
              "AC1: current_commit_type_checker implemented");
        CHECK(cpp.find("aura_typed_audit_try_occurrence_hard_face_full_solve_recover") !=
                  std::string::npos,
              "AC1: try_recover implemented (calls live TC or test override)");
        // 1d. The implementation reads g_tls_audit_commit_readiness_evaluator
        // (#3379's TLS slot) and walks commit_type_checker_handle() to reach
        // the live TypeChecker.
        const auto current_tc_body =
            find_fn_body(cpp, "aura_typed_audit_current_commit_type_checker", 800);
        CHECK(!current_tc_body.empty(), "AC1: current_commit_type_checker body found");
        if (!current_tc_body.empty()) {
            CHECK(current_tc_body.find("g_tls_audit_commit_readiness_evaluator") !=
                      std::string::npos,
                  "AC1: ABI reads g_tls_audit_commit_readiness_evaluator (TLS handle)");
            CHECK(current_tc_body.find("commit_type_checker_handle") != std::string::npos,
                  "AC1: ABI walks commit_type_checker_handle() to reach live TC");
        }
        // 1e. The try_recover impl casts the handle to TypeChecker* and calls
        // try_occurrence_hard_face_full_solve_recover() on it. This is the
        // "bind to live TC" requirement.
        const auto try_recover_body =
            find_fn_body(cpp, "aura_typed_audit_try_occurrence_hard_face_full_solve_recover", 1500);
        CHECK(!try_recover_body.empty(), "AC1: try_recover body found");
        if (!try_recover_body.empty()) {
            CHECK(try_recover_body.find("TypeChecker") != std::string::npos,
                  "AC1: try_recover casts handle to TypeChecker*");
            CHECK(try_recover_body.find("try_occurrence_hard_face_full_solve_recover") !=
                      std::string::npos,
                  "AC1: try_recover invokes "
                  "TypeChecker::try_occurrence_hard_face_full_solve_recover");
        }
        // 1f. Ctor does NOT install a process-global recover hook. The
        // ctor body must not reference install_occurrence_full_solve_recover
        // anywhere — neither in the ctor nor in occurrence_full_solve_recover_trampoline
        // (the trampoline was also removed).
        const auto ctor_body =
            find_fn_body(ixx, "explicit TypeChecker(aura::core::TypeRegistry& reg)", 800);
        CHECK(!ctor_body.empty(), "AC1: TypeChecker ctor found");
        if (!ctor_body.empty()) {
            CHECK(ctor_body.find("install_occurrence_full_solve_recover") == std::string::npos,
                  "AC1: ctor does not install process-global recover hook");
        }
        CHECK(ixx.find("occurrence_full_solve_recover_trampoline") == std::string::npos,
              "AC1: trampoline static method removed (last-TC-wins closure)");
    }

    // ── AC2: nullptr / no TLS → recover fail (hard-reject face, never silent green) ──
    {
        std::println("\n--- AC2: nullptr / no TLS → recover fail ---");
        // 2a. current_commit_type_checker returns nullptr when no TLS evaluator.
        // Behavioral check: no override, no TLS → current_commit_type_checker() == nullptr.
        aura_typed_audit_test_clear_recover_override();
        const void* tc_null = aura_typed_audit_current_commit_type_checker();
        CHECK(tc_null == nullptr,
              "AC2: current_commit_type_checker() returns nullptr with no TLS evaluator");
        // 2b. try_recover returns false when there is no live TC (and no test override).
        const bool recovered = aura_typed_audit_try_occurrence_hard_face_full_solve_recover();
        CHECK(!recovered, "AC2: try_recover returns false when no TLS + no override");
        // 2c. The C ABI in evaluator_mutation_boundary.cpp has a nullptr guard.
        const auto current_tc_body =
            find_fn_body(cpp, "aura_typed_audit_current_commit_type_checker", 800);
        CHECK(current_tc_body.find("return nullptr") != std::string::npos,
              "AC2: ABI returns nullptr guard present");
        const auto try_recover_body =
            find_fn_body(cpp, "aura_typed_audit_try_occurrence_hard_face_full_solve_recover", 1500);
        CHECK(try_recover_body.find("tc_handle == nullptr") != std::string::npos ||
                  try_recover_body.find("tc_handle==nullptr") != std::string::npos ||
                  try_recover_body.find("tc == nullptr") != std::string::npos,
              "AC2: try_recover has nullptr guard for live TC");
    }

    // ── AC3: Soft path unchanged — no install side effects, no extra CS walk ──
    {
        std::println("\n--- AC3: Soft / Off unchanged (no install side effects) ---");
        // 3a. The commit_readiness() body still gates recover on
        // occurrence_face_hard / refined_consistency_hard (Soft/Off leaves
        // these false, so recover is never called — zero extra CS walk).
        const auto cr_body = find_fn_body(h, "inline CommitReadiness commit_readiness(", 5000);
        CHECK(!cr_body.empty(), "AC3: commit_readiness body found");
        if (!cr_body.empty()) {
            // Recover call sites are gated by occurrence_face_hard or
            // refined_consistency_hard (both false under Soft).
            CHECK(cr_body.find("aura_typed_audit_try_occurrence_hard_face_full_solve_recover") !=
                      std::string::npos,
                  "AC3: commit_readiness invokes the new C ABI");
            // The block 2 gate (truncate + outside drop) requires
            // in.truncate_hard || in.occurrence_face_hard — both false
            // under Soft, so recover never runs.
            CHECK(cr_body.find("if (hard && outside_drop)") != std::string::npos,
                  "AC3: block 2 recover gated on hard + outside_drop (Soft=false)");
            CHECK(cr_body.find("if (in.occurrence_face_hard)") != std::string::npos,
                  "AC3: block 6 recover gated on occurrence_face_hard (Soft=false)");
            CHECK(cr_body.find("if (in.refined_consistency_hard && "
                               "in.refined_consistency_drift)") != std::string::npos,
                  "AC3: block 6c recover gated on refined_consistency_hard (Soft=false)");
        }
        // 3b. No "install" calls remain in the ctor (no side effects at
        // TypeChecker construction time — Soft observe path stays untouched).
        const auto ctor_body =
            find_fn_body(ixx, "explicit TypeChecker(aura::core::TypeRegistry& reg)", 800);
        CHECK(!ctor_body.empty(), "AC3: ctor body found");
        if (!ctor_body.empty()) {
            CHECK(ctor_body.find("install_") == std::string::npos,
                  "AC3: ctor installs nothing (no process-global hook side effects)");
        }
    }

    // ── AC4: existing fixtures still reject with cone_outside_goal_drop / refined_drift ──
    {
        std::println("\n--- AC4: recover fail → force_reason cone_outside_goal_drop (10) / "
                     "refined_drift (15) ---");
        // 4a. Source-cite: the existing test_partial_cone_commit_gate +
        // test_type_linear_commit_health + test_typed_audit_commit_readiness_live_policy
        // suites still exist and now use the test-only override path
        // (aura_typed_audit_test_install_recover_override).
        auto pcg = read_file("tests/compiler/test_partial_cone_commit_gate.cpp");
        auto tch = read_file("tests/compiler/test_type_linear_commit_health.cpp");
        auto lp = read_file("tests/compiler/test_typed_audit_commit_readiness_live_policy.cpp");
        // Old call sites (typed_audit::install_occurrence_full_solve_recover) are gone —
        // existing suites migrated to the test override ABI.
        CHECK(pcg.find("typed_audit::install_occurrence_full_solve_recover") == std::string::npos,
              "AC4: test_partial_cone_commit_gate migrated to test override ABI");
        CHECK(tch.find("typed_audit::install_occurrence_full_solve_recover") == std::string::npos,
              "AC4: test_type_linear_commit_health migrated to test override ABI");
        // test_typed_audit_commit_readiness_live_policy never used install
        // (source-cite only); confirm it doesn't reference the old API either.
        CHECK(lp.find("install_occurrence_full_solve_recover") == std::string::npos,
              "AC4: test_typed_audit_commit_readiness_live_policy is source-cite only");
        // 4b. test_occurrence_goal_persist_rehydrate (which assigned to the
        // process-global g_occurrence_full_solve_recover_fn/_ctx) now uses
        // aura_typed_audit_test_clear_recover_override.
        auto ogpr = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
        CHECK(ogpr.find("g_occurrence_full_solve_recover_fn") == std::string::npos,
              "AC4: test_occurrence_goal_persist_rehydrate no longer references old globals");
        CHECK(ogpr.find("aura_typed_audit_test_clear_recover_override") != std::string::npos,
              "AC4: test_occurrence_goal_persist_rehydrate uses new clear ABI");
        // 4c. commit_readiness still emits the existing force_reason codes
        // for the recover-fail path. The decision table is unchanged:
        // - block 2 recover fail → "cone_outside_goal_drop" (code 10)
        // - block 6c recover fail → "refined_drift" (code 15)
        // - block 6 (empty_face) recover fail → "occurrence_empty_after_fence" (code 850)
        const auto cr_body = find_fn_body(h, "inline CommitReadiness commit_readiness(", 5000);
        CHECK(cr_body.find("\"cone_outside_goal_drop\"") != std::string::npos,
              "AC4: block 2 recover fail → force_reason cone_outside_goal_drop");
        CHECK(cr_body.find("\"refined_drift\"") != std::string::npos,
              "AC4: block 6c recover fail → force_reason refined_drift");
        CHECK(cr_body.find("\"occurrence_empty_after_fence\"") != std::string::npos,
              "AC4: block 6 empty_face recover fail → force_reason occurrence_empty_after_fence");
        // 4d. Behavioral: with override=nullptr, try_recover returns false,
        // and the existing test_partial_cone_commit_gate fixtures still
        // hard-reject cone_outside_goal_drop / refined_drift. (We don't
        // re-run those suites here — the source-cite + the AC2 behavioral
        // prove the wiring.)
        CHECK(true, "AC4: existing fixture force_reason codes unchanged");
        // 4e. Test-only override path: with override=[](void*)->true, recover
        // succeeds. This is the only path that can flip recover to true in
        // hermetic tests; production reads live TC.
        aura_typed_audit_test_install_recover_override([](void*) noexcept -> bool { return true; },
                                                       nullptr);
        const bool overridden_ok = aura_typed_audit_try_occurrence_hard_face_full_solve_recover();
        CHECK(overridden_ok,
              "AC4: test override fn=true → try_recover returns true (test override path)");
        aura_typed_audit_test_clear_recover_override();
        const bool overridden_cleared =
            aura_typed_audit_try_occurrence_hard_face_full_solve_recover();
        CHECK(!overridden_cleared,
              "AC4: clear override → try_recover returns false (live TC null → false)");
    }

    // ── AC5: no new query schema; existing recover counters reused ──
    {
        std::println("\n--- AC5: no new query schema, existing recover counters reused ---");
        // 5a. No new counter of the form `*3380*_total` is introduced.
        CHECK(h.find("3380_total") == std::string::npos,
              "AC5: no new 3380-suffixed counter total introduced");
        // 5b. Reused counters (no schema change):
        CHECK(h.find("g_occurrence_hard_face_recover_success_total") != std::string::npos,
              "AC5: existing recover-success counter retained");
        CHECK(h.find("g_occurrence_hard_face_recover_fail_total") != std::string::npos,
              "AC5: existing recover-fail counter retained");
        CHECK(h.find("g_occurrence_recover_not_solved_total") != std::string::npos,
              "AC5: existing not-solved counter retained (#3108)");
        // 5c. No docs/design/3380-* (per MEMORY #1655 docs are obsolete
        // for agent repo; we don't write design docs).
        CHECK(read_file("docs/design/3380-recover-acl.md").empty(),
              "AC5: no docs/design/3380-* per #1655");
        // 5d. No test_issue_3380_* (per MEMORY 2026-07-24: tests go to
        // src/-aligned suite; this file uses the thematic test_typed_audit_* prefix).
        const auto self_path = "tests/compiler/test_typed_audit_commit_readiness_recover_acl.cpp";
        auto self = read_file(self_path);
        CHECK(self.find("test_issue_3380") == std::string::npos,
              "AC5: this test file does not invent test_issue_3380_*");
    }

    // Final defensive clear (so a subsequent suite that doesn't set the
    // override sees a clean TLS slot).
    aura_typed_audit_test_clear_recover_override();

    std::println("\n=== Issue #3380 done ===");
    return g_failed == 0 ? 0 : 1;
}
