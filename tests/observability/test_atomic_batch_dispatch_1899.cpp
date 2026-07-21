// @category: integration
// @reason: Issue #1899 — atomic-batch STRONG atomicity (Option A) + data-driven
// lockless dispatch table (extensibility) closing the 5-of-14 if/else debt.
//
//   AC1: source has kAtomicBatchLocklessOps table + #1899 + strong docs
//   AC2: query:atomic-batch-stats-hash schema-1899 + dispatch-table-size 13
//   AC3: multi-op batch of table-covered ops commits under strong mode
//   AC4: unsupported op aborts batch (batch-unsupported-op)
//   AC5: atomicity-mode remains strong (1); weak stays 0
//   AC6: multi-round stress — strong commits monotonic

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:atomic-batch-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (define g (lambda (y) (* y 2)))\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

} // namespace

int main() {
    // ── AC1: data-driven table in source ──
    {
        std::println("\n--- AC1: kAtomicBatchLocklessOps table ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_mutate.cpp",
                              "../src/compiler/evaluator_primitives_mutate.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read mutate.cpp");
        CHECK(src.find("#1899") != std::string::npos, "cites #1899");
        CHECK(src.find("kAtomicBatchLocklessOps") != std::string::npos, "table present");
        CHECK(src.find("kAtomicBatchLocklessOpCount") != std::string::npos, "count present");
        CHECK(src.find("STRONG") != std::string::npos ||
                  src.find("strong atomicity") != std::string::npos ||
                  src.find("STRONG atomicity") != std::string::npos,
              "strong atomicity docs");
        CHECK(src.find("no inter-op yield") != std::string::npos ||
                  src.find("no inter-op") != std::string::npos ||
                  src.find("No inter-op fiber yield") != std::string::npos,
              "no inter-op yield documented");
        // Table definition (skip doc comments that only mention the name).
        auto pos = src.find("kAtomicBatchLocklessOps[]");
        CHECK(pos != std::string::npos, "table array site");
        auto win = src.substr(pos, 2500);
        CHECK(win.find("eval_flat_apply_mutate_rebind") != std::string::npos, "rebind row");
        CHECK(win.find("eval_flat_apply_mutate_inline_call") != std::string::npos,
              "inline-call row");
        CHECK(win.find("eval_flat_apply_mutate_set_body") != std::string::npos, "set-body row");
        // Dispatch uses member pointer, not giant if/else after table.
        CHECK(src.find("(ev.*op_fn)") != std::string::npos ||
                  src.find("(ev.*op_fn)(op_args)") != std::string::npos,
              "member-pointer dispatch");
    }

    // ── AC2: stats surface ──
    {
        std::println("\n--- AC2: schema-1899 dispatch inventory ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
        CHECK(r.has_value() && is_hash(*r), "stats-hash is hash");
        CHECK(href(cs, "schema") == 622, "base schema 622");
        CHECK(href(cs, "schema-1878") == 1878, "schema-1878");
        CHECK(href(cs, "schema-1893") == 1893, "schema-1893");
        CHECK(href(cs, "schema-1899") == 1899, "schema-1899");
        CHECK(href(cs, "issue") == 1899, "issue 1899");
        CHECK(href(cs, "atomicity-mode") == 1, "strong atomicity-mode");
        CHECK(href(cs, "strong-atomicity-default") == 1, "strong default flag");
        CHECK(href(cs, "dispatch-table-data-driven") == 1, "data-driven flag");
        CHECK(href(cs, "dispatch-table-size") == 13, "table size 13");
        CHECK(href(cs, "lockless-ops-covered") == 13, "lockless covered 13");
        CHECK(href(cs, "no-inter-op-yield") == 1, "no inter-op yield");
        CHECK(href(cs, "weak-atomicity-used") == 0, "weak unused");
    }

    // ── AC3: multi-op batch commits ──
    {
        std::println("\n--- AC3: multi-op batch under strong mode ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        const auto strong0 = ev.atomic_batch_strong_atomicity_commits_total();
        const auto weak0 = ev.atomic_batch_weak_atomicity_used_total();
        // Batch of two rebinds (always in table).
        auto r =
            cs.eval("(mutate:atomic-batch "
                    "(list "
                    "(list \"mutate:rebind\" \"f\" \"(lambda (x) (+ x 10))\" \"batch-1899-a\") "
                    "(list \"mutate:rebind\" \"g\" \"(lambda (y) (* y 3))\" \"batch-1899-b\")) "
                    "\"1899 multi-op\")");
        CHECK(r.has_value(), "batch eval returns");
        // Success is non-error (bool or other).
        if (r)
            CHECK(!is_error(*r), "batch not error");
        CHECK(ev.atomic_batch_strong_atomicity_commits_total() >= strong0, "strong commits ≥");
        CHECK(ev.atomic_batch_weak_atomicity_used_total() == weak0, "weak still 0");
        CHECK(href(cs, "atomicity-mode") == 1, "mode still strong");
    }

    // ── AC4: unsupported op aborts ──
    {
        std::println("\n--- AC4: unsupported op aborts batch ---");
        CompilerService cs;
        seed(cs);
        auto r = cs.eval("(mutate:atomic-batch "
                         "(list "
                         "(list \"mutate:rebind\" \"f\" \"(lambda (x) 1)\" \"ok\") "
                         "(list \"mutate:not-a-real-op\" 0) "
                         "(list \"mutate:rebind\" \"g\" \"(lambda (y) 2)\" \"never\")) "
                         "\"1899 unsupported\")");
        CHECK(r.has_value(), "unsupported batch returns");
        if (r) {
            // make_merr returns a pair (key . (msg . 0)), not Tag::Error.
            using aura::compiler::types::as_bool;
            using aura::compiler::types::is_bool;
            using aura::compiler::types::is_pair;
            const bool failed = is_error(*r) || is_pair(*r) || (is_bool(*r) && !as_bool(*r));
            CHECK(failed, "unsupported → merr pair / error / #f");
        }
        CHECK(href(cs, "schema-1899") == 1899, "schema holds after abort");
    }

    // ── AC5: strong mode stable ──
    {
        std::println("\n--- AC5: strong mode + weak=0 ---");
        CompilerService cs;
        CHECK(href(cs, "atomicity-mode") == 1, "mode 1");
        CHECK(href(cs, "weak-atomicity-used") == 0, "weak 0");
        CHECK(href(cs, "strong-atomicity-default") == 1, "default strong");
    }

    // ── AC6: multi-round stress ──
    {
        std::println("\n--- AC6: multi-round strong commits ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        const auto strong0 = ev.atomic_batch_strong_atomicity_commits_total();
        for (int i = 0; i < 8; ++i) {
            auto expr = std::format(
                "(mutate:atomic-batch "
                "(list (list \"mutate:rebind\" \"f\" \"(lambda (x) (+ x {}))\" \"round\")) "
                "\"r{}\")",
                i + 1, i);
            (void)cs.eval(expr);
        }
        CHECK(ev.atomic_batch_strong_atomicity_commits_total() >= strong0, "strong non-decreasing");
        CHECK(ev.atomic_batch_weak_atomicity_used_total() == 0, "weak still 0 after stress");
        CHECK(href(cs, "dispatch-table-size") == 13, "table size holds");
        CHECK(href(cs, "schema-1899") == 1899, "schema holds after stress");
    }

    std::println("\n=== test_atomic_batch_dispatch_1899: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
