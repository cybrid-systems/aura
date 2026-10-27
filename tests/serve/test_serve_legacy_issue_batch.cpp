// test_serve_legacy_issue_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include "serve/serve_async.h"

#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

extern int run_test_issue_1990();
extern int run_test_issue_1991();
extern int run_test_issue_1992();
extern int run_test_issue_1993();
extern int run_test_mutate_mailbox_starvation_throttle();
extern int run_test_spawn_quota_no_leak();

// ── Issue #4702: serve exec chunk with several top-level defines ────
//
// The serve exec command takes a whole chunk of Aura source. A chunk
// with SEVERAL top-level forms used to be evaluated as ONE program: the
// compiler lowers a multi-form program (Begin root) to IR, where the
// top-level defines become IR-local cells that die with the IR module —
// so "(define a1 1)\n(define b1 2)" bound nothing in the session env and
// the NEXT exec failed with "unbound variable: a1" (only function defines
// survived: the compiler re-registers Lambda defines in the env
// separately). The serve path now splits the chunk into its top-level
// forms and evaluates them one at a time — the shared helpers
// aura::serve::detail::split_top_level_forms + eval_top_level_forms that
// serve_async.cpp and main.cpp use — and an unbalanced chunk is rejected
// instead of being accepted silently (it used to swallow the forms after
// it).
//
// AC1: the chunk splitter recovers every top-level form of a chunk
// AC2: an unbalanced chunk is rejected — no form is evaluated
// AC3: every top-level define of one exec chunk persists in the session
//      env (the next exec sees the bindings)
// AC4: a function define of the same chunk stays callable
static int test_issue_4702_exec_chunk_forms() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    (void)g_passed;
    (void)g_failed;

    // ── AC1: the splitter recovers every top-level form ──
    {
        auto split = aura::serve::detail::split_top_level_forms(
            "(define a1 1)\n(define b1 2)\n(define (f1) 42)");
        CHECK(split.ok(), "AC1: a balanced chunk splits without an error");
        CHECK(split.forms.size() == 3, "AC1: three top-level forms recovered");
        CHECK(split.forms.size() == 3 && split.forms[0].find("(define a1 1)") != std::string::npos,
              "AC1: the first form is the first define");
        CHECK(split.forms.size() == 3 &&
                  split.forms[2].find("(define (f1) 42)") != std::string::npos,
              "AC1: the last form is the function define");
        // A delimiter inside a string / character literal / comment is not
        // a delimiter (the #4622 shape).
        auto lit = aura::serve::detail::split_top_level_forms(
            "(display \"(\")\n(display #\\()\n(display 1) ; )\n");
        CHECK(lit.ok(), "AC1: literals and comments do not unbalance the chunk");
        CHECK(lit.forms.size() == 3, "AC1: three forms around the literals");
    }

    // ── AC2: an unbalanced chunk is rejected ──
    {
        auto unclosed = aura::serve::detail::split_top_level_forms("(define a1 1)\n(define (bad");
        CHECK(!unclosed.ok(), "AC2: an unclosed '(' is rejected");
        CHECK(unclosed.forms.empty(), "AC2: a rejected chunk yields no form to evaluate");
        CHECK(!unclosed.ok() && unclosed.error.find("parse error") != std::string::npos,
              "AC2: the rejection carries a parse error");
        auto extra = aura::serve::detail::split_top_level_forms("(define a1 1))");
        CHECK(!extra.ok(), "AC2: an extra ')' is rejected");
    }

    // ── AC3 / AC4: the defines of one chunk persist ──
    {
        aura::compiler::CompilerService cs;
        auto split = aura::serve::detail::split_top_level_forms("(define a1 1)\n(define b1 2)");
        CHECK(split.ok(), "AC3: the define chunk splits cleanly");
        // The serve exec path: split the chunk, then evaluate the forms in
        // order (aura::serve::detail::eval_top_level_forms).
        auto chunk = aura::serve::detail::eval_top_level_forms(
            split.forms, [&](std::string_view form) { return cs.exec_with_cache(form); });
        CHECK(chunk.has_value(), "AC3: the chunk evaluates");
        CHECK(chunk.has_value() && (*chunk).has_value(), "AC3: the chunk result is a value");
        // Every top-level define of the chunk is bound in the session env:
        // the NEXT exec sees them.
        auto read = cs.exec_with_cache("(list a1 b1)");
        CHECK(read.has_value(), "AC3: the next exec sees the defines of the chunk");
        if (read.has_value()) {
            auto text = aura::compiler::format_value(
                *read, cs.evaluator().primitives().string_heap(), cs.evaluator().pairs(), 0,
                &cs.evaluator().primitives(), cs.evaluator().keyword_table(),
                cs.evaluator().vector_heap(), cs.evaluator().symbol_table());
            CHECK(text == "(1 2)", "AC3: both defines of the chunk are bound (1 2)");
        }
        // A function define of the same chunk stays callable.
        auto fnsplit = aura::serve::detail::split_top_level_forms("(define (f4702) 42)");
        auto fn = aura::serve::detail::eval_top_level_forms(
            fnsplit.forms, [&](std::string_view form) { return cs.exec_with_cache(form); });
        CHECK(fn.has_value() && (*fn).has_value(), "AC4: the function define evaluates");
        auto call = cs.exec_with_cache("(f4702)");
        CHECK(call.has_value(), "AC4: the function define is callable");
    }
    return 0;
}

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_serve_legacy_issue_batch (7 members) ===");

    std::println("\n──── test_issue_1990 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1990() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1990 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1990 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1991 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1991() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1991 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1991 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1992 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1992() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1992 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1992 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1993 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1993() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1993 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1993 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_mailbox_starvation_throttle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_mailbox_starvation_throttle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_mailbox_starvation_throttle ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_mailbox_starvation_throttle ({} checks)", g_passed);
    }

    std::println("\n──── test_spawn_quota_no_leak ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_spawn_quota_no_leak() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_spawn_quota_no_leak ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_spawn_quota_no_leak ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_4702_exec_chunk_forms ────");
    g_passed = 0;
    g_failed = 0;
    if (test_issue_4702_exec_chunk_forms() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_4702_exec_chunk_forms ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_4702_exec_chunk_forms ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
