// @category: unit
// @reason: Issue #2796 — atomic-batch abort paths must not call
// linear_post_mutate_enforce_all after rollback (pollutes
// linear_invariant_fail / invariant_violations_caught with rollback noise).
//
//   AC1: abort_batch_workspace cites #2796; no enforce_all on abort paths
//   AC2: unsupported-op / batch-failed leave audit fail counters unchanged
//   AC3: multi-fail storm still keeps fail counters flat
//   AC4: this suite + linter; no docs/design/2796-*; no test_issue_2796.cpp

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
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

static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

struct AuditSnap {
    std::uint64_t linear_fail = 0;
    std::uint64_t inv_caught = 0;
    std::uint64_t composite_fail = 0;
};

static AuditSnap snap_audit() {
    return {
        g_typed_mutation_audit_counters.linear_invariant_fail.load(std::memory_order_relaxed),
        g_typed_mutation_audit_counters.invariant_violations_caught.load(std::memory_order_relaxed),
        g_typed_mutation_audit_counters.composite_invariant_fail_total.load(
            std::memory_order_relaxed),
    };
}

static bool audit_unchanged(const AuditSnap& a, const AuditSnap& b) {
    return a.linear_fail == b.linear_fail && a.inv_caught == b.inv_caught &&
           a.composite_fail == b.composite_fail;
}

} // namespace

int run_test_atomic_batch_rollback_metric_noise() {
    std::println("=== Issue #2796: atomic-batch rollback metric noise ===");
    CHECK(true, "ac2796: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: abort path skips linear_post_mutate_enforce_all ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!mut.empty(), "AC1: mutate readable");
        auto pos = mut.find("add_mutate(\"mutate:atomic-batch\"");
        if (pos == std::string::npos)
            pos = mut.find("mutate:atomic-batch");
        CHECK(pos != std::string::npos, "AC1: atomic-batch present");
        auto win = mut.substr(pos, 22000);
        CHECK(win.find("Issue #2796") != std::string::npos, "AC1: cites #2796");
        CHECK(win.find("abort_batch_workspace") != std::string::npos, "AC1: abort helper");
        // Helper body must not call enforce_all (only comments may mention it).
        auto hpos = win.find("auto abort_batch_workspace");
        CHECK(hpos != std::string::npos, "AC1: helper pos");
        auto hend = win.find("while (is_pair(op_list))", hpos);
        if (hend == std::string::npos)
            hend = hpos + 800;
        auto hbody = win.substr(hpos, hend - hpos);
        // Strip comments: no live call of enforce_all in helper.
        CHECK(hbody.find("linear_post_mutate_enforce_all()") != std::string::npos,
              "AC1: comment documents skip");
        // Live call would look like (void)ev.linear_post_mutate_enforce_all();
        CHECK(hbody.find("(void)ev.linear_post_mutate_enforce_all()") == std::string::npos,
              "AC1: no live (void)ev.linear_post_mutate_enforce_all() in helper");
        CHECK(win.find("abort_batch_workspace()") != std::string::npos, "AC1: helper used");
    }

    // ── AC2: single fail leaves audit counters flat ──
    {
        std::println("\n--- AC2: unsupported-op + batch-failed metrics unchanged ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");

        const auto before = snap_audit();

        // Unsupported sub-op → batch-unsupported-op abort path.
        auto u =
            cs.eval("(mutate:atomic-batch (list (list \"mutate:not-a-real-op\" \"x\" \"1\")))");
        CHECK(u.has_value(), "AC2: unsupported returns");
        CHECK(is_pair(*u), "AC2: unsupported merr");
        CHECK(merr_kind(cs, *u) == "batch-unsupported-op", "AC2: kind unsupported-op");

        // Sub-op unexpected → batch-failed abort path.
        auto b = cs.eval("(mutate:atomic-batch "
                         "(list (list \"mutate:rebind\" \"f\" \"(lambda () 99)\") "
                         "      (list \"mutate:rebind\" \"no-such-2796\" \"(lambda () 0)\")))");
        CHECK(b.has_value() && is_pair(*b), "AC2: batch-failed merr");
        CHECK(merr_kind(cs, *b) == "batch-failed", "AC2: kind batch-failed");

        const auto after = snap_audit();
        CHECK(audit_unchanged(before, after),
              "AC2: linear_invariant_fail / invariant_violations_caught / "
              "composite_invariant_fail_total unchanged after abort paths");
        // f still original.
        auto f = cs.eval("(begin (eval-current) (f))");
        CHECK(f && is_int(*f) && as_int(*f) == 1, "AC2: f still 1");
    }

    // ── AC3: multi-fail storm ──
    {
        std::println("\n--- AC3: 50 failed batches keep fail counters flat ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define g (lambda () 2))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        const auto before = snap_audit();
        for (int i = 0; i < 50; ++i) {
            auto r =
                cs.eval("(mutate:atomic-batch (list (list \"mutate:not-a-real-op\" \"x\" \"1\")))");
            CHECK(r.has_value(), "AC3: each fail returns");
        }
        const auto after = snap_audit();
        CHECK(audit_unchanged(before, after), "AC3: audit fail counters flat after 50 fails");
    }

    std::println("\n=== #2796 atomic-batch rollback metric noise: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_atomic_batch_rollback_metric_noise();
}
#endif
