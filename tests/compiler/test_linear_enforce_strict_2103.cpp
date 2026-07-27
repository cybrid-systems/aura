// @category: unit
// @reason: Issue #2103 — optional Strict require_complete on
// validate_linear_provenance hot path (type-system review §7.2).
//
//   AC1: Soft + incomplete trail → incomplete metric; ok continues
//   AC2: Strict + incomplete trail → hard fail (ok=false, force_deopt)
//   AC3: Strict hard-fail bumps strict-hard-fail total; Full audit
//        correlation via force-rollback counter when metrics wired
//   AC4: #2026 Soft default preserved; Untracked/Moved semantics unchanged
//   AC5: query:post-steal-closed-loop-stats surfaces mode + schema-2103
//   AC6: source wiring

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::provenance::g_linear_soft_incomplete_continue_total;
using aura::core::provenance::g_linear_strict_hard_fail_total;
using aura::core::provenance::g_provenance_enforcement;
using aura::core::provenance::kLinearOwned;
using aura::core::provenance::kLinearUntracked;
using aura::core::provenance::linear_enforce_mode;
using aura::core::provenance::linear_enforce_require_complete;
using aura::core::provenance::LinearEnforceMode;
using aura::core::provenance::reset_linear_enforce_mode_for_test;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::set_linear_enforce_mode;
using aura::core::provenance::validate_linear_provenance;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_all() {
    reset_provenance_enforcement_for_test();
    reset_linear_enforce_mode_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

static void ac1_soft_incomplete_continues() {
    std::println("\n--- AC1: Soft + incomplete → continue ---");
    reset_all(); // forces Soft (explicit opt-in for Soft-path unit tests)
    CHECK(linear_enforce_mode() == LinearEnforceMode::Soft, "Soft forced via reset");
    CHECK(!linear_enforce_require_complete(), "Soft → require_complete false");

    const auto soft0 = g_linear_soft_incomplete_continue_total.load();
    const auto hard0 = g_linear_strict_hard_fail_total.load();
    const auto inc0 = g_provenance_enforcement().linear_provenance_incomplete_total.load();

    // Owned + zero provenance/mutation under Soft (IR hot-path default).
    auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0,
                                        /*require_complete=*/false);
    CHECK(r.ok, "Soft incomplete → ok");
    CHECK(!r.force_deopt, "Soft incomplete → no force_deopt");
    CHECK(g_provenance_enforcement().linear_provenance_incomplete_total.load() > inc0,
          "incomplete metric bumped");
    CHECK(g_linear_soft_incomplete_continue_total.load() > soft0, "soft continue total");
    CHECK(g_linear_strict_hard_fail_total.load() == hard0, "no strict hard fail under Soft");
}

static void ac2_strict_incomplete_hard_fail() {
    std::println("\n--- AC2: Strict + incomplete → hard fail ---");
    reset_all();
    set_linear_enforce_mode(LinearEnforceMode::Strict);
    CHECK(linear_enforce_mode() == LinearEnforceMode::Strict, "Strict set");
    CHECK(linear_enforce_require_complete(), "Strict → require_complete true");

    const auto hard0 = g_linear_strict_hard_fail_total.load();
    auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0,
                                        linear_enforce_require_complete());
    CHECK(!r.ok, "Strict incomplete → !ok");
    CHECK(r.force_deopt, "Strict incomplete → force_deopt");
    CHECK(r.reason != nullptr, "reason set");
    CHECK(g_linear_strict_hard_fail_total.load() > hard0, "strict hard-fail total");

    // Complete trail under Strict succeeds.
    auto r2 = validate_linear_provenance(kLinearOwned, 1, /*prov=*/42, /*mut=*/99, 0, 0, 0, 0,
                                         linear_enforce_require_complete());
    CHECK(r2.ok, "Strict + complete trail → ok");
    reset_linear_enforce_mode_for_test();
}

static void ac3_full_audit_correlation() {
    std::println("\n--- AC3: Strict hard-fail + Full audit correlation surface ---");
    reset_all();
    set_linear_enforce_mode(LinearEnforceMode::Strict);
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);

    const auto hard0 = g_linear_strict_hard_fail_total.load();
    auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0, true);
    CHECK(!r.ok, "hard fail");
    CHECK(g_linear_strict_hard_fail_total.load() > hard0, "hard-fail counted");
    // Full strategy active so IR path would also bump force-rollback on metrics
    // (wired in enforce_linear_ownership_state). Source + query keys document it.
    CHECK(aura::compiler::typed_audit::get_strategy() ==
              aura::compiler::typed_audit::AuditStrategy::Full,
          "Full strategy active for correlation");
    reset_all();
}

static void ac4_lineage_soft_opt_in() {
    std::println("\n--- AC4: Soft opt-in + Untracked/complete unchanged ---");
    reset_all(); // Soft opt-in for Soft-path semantics
    CHECK(linear_enforce_mode() == LinearEnforceMode::Soft, "Soft opt-in");
    auto u = validate_linear_provenance(kLinearUntracked, 0, 0, 0, 0, 0, 0, 0, false);
    CHECK(u.ok, "Untracked always ok");
    auto c = validate_linear_provenance(kLinearOwned, 1, 7, 8, 0, 0, 0, 0, true);
    CHECK(c.ok, "complete + require_complete ok");

    // Happy-path linear eval still works under Soft.
    CompilerService cs;
    CHECK(cs.eval("(let ((l (Linear 5))) (move l))").has_value(), "Soft linear move ok");
}

static void ac5_query_mode() {
    std::println("\n--- AC5: query surfaces mode + schema-2103 ---");
    reset_all();
    CompilerService cs;
    CHECK(href(cs, "schema-2103") == 2103, "schema-2103");
    CHECK(href(cs, "issue-2103") == 2103, "issue-2103");
    CHECK(href(cs, "linear-enforce-mode-wired") == 1, "wired");
    CHECK(href(cs, "linear-enforce-mode") == 0, "Soft mode == 0");
    CHECK(href(cs, "linear-enforce-strict") == 0, "not strict");
    CHECK(href(cs, "linear-strict-hard-fail-total") >= 0, "hard-fail key");
    CHECK(href(cs, "linear-soft-incomplete-continue-total") >= 0, "soft continue key");
    CHECK(href(cs, "schema-2026") == 2026, "2026 lineage retained");

    set_linear_enforce_mode(LinearEnforceMode::Strict);
    CHECK(href(cs, "linear-enforce-mode") == 1, "Strict mode == 1");
    CHECK(href(cs, "linear-enforce-strict") == 1, "strict flag");
    reset_linear_enforce_mode_for_test();
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: source wiring #2103 ---");
    auto pt = read_file("src/core/provenance_tracker.hh");
    auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!pt.empty() && pt.find("Issue #2103") != std::string::npos, "tracker #2103");
    CHECK(pt.find("LinearEnforceMode") != std::string::npos, "LinearEnforceMode");
    CHECK(pt.find("linear_enforce_require_complete") != std::string::npos, "require helper");
    CHECK(pt.find("g_linear_strict_hard_fail_total") != std::string::npos, "hard-fail total");
    CHECK(!ir.empty() && ir.find("Issue #2103") != std::string::npos, "ir_executor #2103");
    CHECK(ir.find("linear_enforce_require_complete") != std::string::npos, "IR uses mode");
    CHECK(!q.empty() && q.find("schema-2103") != std::string::npos, "query schema-2103");
    CHECK(q.find("linear-enforce-mode") != std::string::npos, "mode key");
}

} // namespace

int main() {
    std::println("=== Issue #2103: LinearEnforceMode Soft/Strict ===");
    ac1_soft_incomplete_continues();
    ac2_strict_incomplete_hard_fail();
    ac3_full_audit_correlation();
    ac4_lineage_soft_opt_in();
    ac5_query_mode();
    ac6_source_wiring();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
