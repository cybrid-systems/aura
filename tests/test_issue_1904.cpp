// @category: integration
// @reason: uses Evaluator + MutationBoundaryGuard + CompilerMetrics + CI linter
//
// test_issue_1904.cpp — Verify Issue #1904 acceptance criteria
// ("Complete remaining mutate:* primitive migration to
//  MutationBoundaryGuard + unified defuse_version bump invariants +
//  full rollback coverage (refine #213)").
//
// #1904 turns the existing observability counter family (#1252 /
// #1547 / #1618) into a hard migration gate:
//   1. MutationBoundaryGuard RAII is the single owner of the
//      workspace_mtx_ + defuse_version_ + panic-checkpoint +
//      rollback machinery.
//   2. Every mutate:* call site uses MutationBoundaryGuard
//      (try_acquire preferred; legacy ctor with quota soft-fail).
//   3. Legacy `std::unique_lock<std::shared_mutex>(ev.workspace_mtx_)`
//      + `ev.defuse_version_.fetch_add(...)` patterns are eliminated.
//   4. The CI linter (scripts/check_legacy_mutate_lock.py) fails
//      when either pattern reappears in evaluator_primitives_*.cpp.
//   5. (engine:metrics "query:mutation-guard-coverage") primitive
//      reports the coverage: -1 sentinel when legacy > 0,
//      10000 (basis points) when vacuously covered, basis points
//      (wrapped / (wrapped + legacy) * 10000) otherwise.
//
// This test exercises the public API surface only — verifying the
// accessor reachability, the primitive shape, the linter
// pass/fail logic (via --self-test mode), and the manual site
// observability (bumping the legacy counter directly to verify
// the -1 sentinel path). The full migration of ~13 legacy
// primitives is verified by the linter running on the
// evaluator_primitives_*.cpp files at the next ship cycle.
//
// Acceptance Criteria covered (mirrors #1904 body):
//   AC1: 4 #1904 accessors reachable (wrapped / legacy / try_acquire_total / try_acquire_reject)
//   AC2: fresh evaluator reports 10000 (vacuously covered) — no Guard wraps
//        and no legacy sites yet
//   AC3: running mutate:rebind bumps wrapped naturally; primitive still
//        returns 10000 because legacy=0
//   AC4: bumping mutation_legacy_manual_lock_total flips the
//        primitive to -1 regression sentinel
//   AC5: legacy sentinel takes precedence over basis-points branch
//        (legacy > 0 + wrapped > 0 → still -1)
//   AC6: scripts/check_legacy_mutate_lock.py self-test passes
//        (regex correctness: positive + negative fixtures)
//   AC7: scripts/check_legacy_mutate_lock.py detects a synthetic
//        legacy pattern in a temp file (integration with the linter)
//   AC8: mutation_boundary_primitives_wrapped counter monotonic under
//        repeated Mutate:rebind (every Guard ctor bumps once)
//   AC9: mutation_legacy_manual_lock_total stays 0 after running
//        a known-migrated mutate primitive (Guard path only)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_1904_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

// ── AC1: 4 #1904 accessors reachable ──
bool test_four_accessors_reachable() {
    std::println("\n--- AC1: 4 #1904 accessors reachable ---");
    CompilerService cs;
    const auto wrapped = cs.evaluator().get_mutation_boundary_primitives_wrapped();
    const auto legacy = cs.evaluator().get_mutation_legacy_manual_lock_total();
    const auto try_total = cs.evaluator().get_mutation_guard_try_acquire_total();
    const auto try_reject = cs.evaluator().get_mutation_guard_try_acquire_reject_total();
    CHECK(wrapped + legacy + try_total + try_reject == 0,
          "fresh evaluator: all 4 #1904 counters read as 0 (deterministic baseline)");
    return true;
}

// ── AC2: fresh evaluator reports 10000 (vacuously covered) ──
bool test_fresh_evaluator_vacuously_covered() {
    std::println("\n--- AC2: fresh evaluator -> query:mutation-guard-coverage = 10000 ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:mutation-guard-coverage\")");
    CHECK(r.has_value(), "primitive returns a value");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    const auto v = as_int(*r);
    CHECK(v == 10000, "no Guard wraps + no legacy sites yet -> vacuously 10000 coverage");
    return true;
}

// ── AC3: Guard-only path (no legacy) reports full coverage ──
// mutate:rebind naturally bumps mutation_boundary_primitives_wrapped via
// the Guard ctor (#1252). legacy stays 0, so the primitive still
// reports 10000.
bool test_guard_only_path_reports_full_coverage() {
    std::println("\n--- AC3: mutate:rebind (Guard path) -> still 10000 ---");
    CompilerService cs;
    auto r1 = cs.eval("(mutate:rebind \"a\" \"1\")");
    CHECK(r1.has_value(), "mutate:rebind returns");
    const auto wrapped = cs.evaluator().get_mutation_boundary_primitives_wrapped();
    CHECK(wrapped >= 1, "Guard ctor bumped mutation_boundary_primitives_wrapped by >= 1");
    auto r2 = cs.eval("(engine:metrics \"query:mutation-guard-coverage\")");
    CHECK(r2 && is_int(*r2), "primitive still int after Guard bump");
    if (!r2 || !is_int(*r2)) {
        ++g_failed;
        return false;
    }
    CHECK(as_int(*r2) == 10000, "wrapped > 0 + legacy = 0 -> still 10000 (no regression)");
    return true;
}

// ── AC4: legacy > 0 -> -1 regression sentinel ──
bool test_legacy_sentinel_path() {
    std::println("\n--- AC4: legacy > 0 -> -1 regression sentinel ---");
    CompilerService cs;
    cs.evaluator().bump_mutation_legacy_manual_lock_total();
    auto r = cs.eval("(engine:metrics \"query:mutation-guard-coverage\")");
    CHECK(r && is_int(*r), "primitive int after legacy bump");
    if (!r || !is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(as_int(*r) == -1, "legacy > 0 -> -1 sentinel (grep-friendly regression marker)");
    return true;
}

// ── AC5: legacy sentinel takes precedence over wrapped-branch math ──
// Even with wrapped > 0 (via Guard ctor on mutate:rebind) AND legacy > 0,
// the primitive returns -1 because the legacy branch wins.
bool test_sentinel_precedence_over_basis_points() {
    std::println("\n--- AC5: legacy sentinel precedence over wrapped-branch math ---");
    CompilerService cs;
    // First bump legacy via the public helper.
    cs.evaluator().bump_mutation_legacy_manual_lock_total();
    // Then bump wrapped naturally via a Guard ctor on mutate:rebind.
    auto r1 = cs.eval("(mutate:rebind \"b\" \"2\")");
    CHECK(r1.has_value(), "mutate:rebind returns");
    const auto wrapped = cs.evaluator().get_mutation_boundary_primitives_wrapped();
    const auto legacy = cs.evaluator().get_mutation_legacy_manual_lock_total();
    CHECK(wrapped >= 1, "wrapped bumped via Guard ctor");
    CHECK(legacy >= 1, "legacy bumped via public helper");
    // Now check primitive: legacy > 0 wins.
    auto r2 = cs.eval("(engine:metrics \"query:mutation-guard-coverage\")");
    CHECK(r2 && is_int(*r2), "primitive int for sentinel-precedence branch");
    if (!r2 || !is_int(*r2)) {
        ++g_failed;
        return false;
    }
    CHECK(as_int(*r2) == -1,
          "legacy > 0 + wrapped > 0 -> -1 sentinel wins over wrapped-branch math");
    return true;
}

// ── AC6: linter self-test (regex correctness) ──
bool test_linter_self_test() {
    std::println("\n--- AC6: scripts/check_legacy_mutate_lock.py --self-test ---");
    const std::string cmd =
        "cd /home/dev/code/aura && python3 scripts/check_legacy_mutate_lock.py --self-test";
    const int rc = std::system(cmd.c_str());
    CHECK(rc == 0,
          "linter --self-test exits 0 (regex + ALLOW marker + non-mutex false-positive guards)");
    return rc == 0;
}

// ── AC7: linter detects a synthetic legacy pattern in a temp file ──
bool test_linter_detects_synthetic_violation() {
    std::println("\n--- AC7: linter detects synthetic legacy pattern ---");
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "test_issue_1904_legacy_fixture.cpp";
    {
        std::ofstream out(tmp);
        out << "// synthetic legacy fixture for test_issue_1904 AC7\n";
        out << "std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);\n";
        out << "ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);\n";
    }
    const std::string cmd = "cd /home/dev/code/aura && python3 scripts/check_legacy_mutate_lock.py "
                            "--files " +
                            tmp.string() + " > /dev/null 2>&1; echo $?";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    CHECK(pipe != nullptr, "popen succeeded");
    int rc = -1;
    if (pipe) {
        char buf[16] = {0};
        while (char* r = ::fgets(buf, sizeof(buf), pipe))
            (void)r;
        ::pclose(pipe);
        try {
            rc = std::stoi(buf);
        } catch (...) {
            rc = -1;
        }
    }
    std::filesystem::remove(tmp);
    CHECK(rc != 0, "linter exits non-zero on synthetic legacy pattern");
    return rc != 0;
}

// ── AC8: mutation_boundary_primitives_wrapped monotonic under mutate:rebind ──
bool test_wrapped_counter_monotonic_under_rebind() {
    std::println("\n--- AC8: wrapped counter monotonic under mutate:rebind ---");
    CompilerService cs;
    const auto before = cs.evaluator().get_mutation_boundary_primitives_wrapped();
    auto r = cs.eval(R"((begin
                          (mutate:rebind \"x\" \"42\")
                          (mutate:rebind \"y\" \"99\")))");
    CHECK(r.has_value(), "mutate:rebind batch returns");
    const auto after = cs.evaluator().get_mutation_boundary_primitives_wrapped();
    CHECK(after >= before + 1, "wrapped counter grows by >= 1 after one mutate:rebind");
    return true;
}

// ── AC9: migrated primitive leaves legacy counter at 0 ──
bool test_migrated_primitive_no_legacy_bump() {
    std::println("\n--- AC9: migrate:rebind leaves legacy counter at 0 ---");
    CompilerService cs;
    const auto before = cs.evaluator().get_mutation_legacy_manual_lock_total();
    auto r = cs.eval("(mutate:rebind \"z\" \"7\")");
    CHECK(r.has_value(), "mutate:rebind returns");
    const auto after = cs.evaluator().get_mutation_legacy_manual_lock_total();
    CHECK(after == before, "legacy counter unchanged (mutate:rebind uses Guard, not manual lock)");
    return true;
}

} // namespace aura_issue_1904_detail

int main() {
    using namespace aura_issue_1904_detail;
    std::println("=== Issue #1904: mutate:* primitive MutationBoundaryGuard full coverage ===");
    int rc = 0;
    rc |= !test_four_accessors_reachable();
    rc |= !test_fresh_evaluator_vacuously_covered();
    rc |= !test_guard_only_path_reports_full_coverage();
    rc |= !test_legacy_sentinel_path();
    rc |= !test_sentinel_precedence_over_basis_points();
    rc |= !test_linter_self_test();
    rc |= !test_linter_detects_synthetic_violation();
    rc |= !test_wrapped_counter_monotonic_under_rebind();
    rc |= !test_migrated_primitive_no_legacy_bump();
    std::println("\n=== Summary: passed={} failed={} ===", g_passed, g_failed);
    return rc == 0 ? 0 : 1;
}
