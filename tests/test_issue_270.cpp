// @category: integration
// @reason: uses CompilerService to verify StableNodeRef + atomic-batch apply

// test_issue_270.cpp — Issue #270: end_id snapshot + StableNodeRef
// in mutate:replace-pattern / mutate:query-and-replace.
//
// Verifies multi-match apply in a single primitive call (would fail
// without atomic batch + stable refs when the second match is skipped).


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_270_detail {

static bool run_ok(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r)
        return false;
    auto& v = *r;
    if (aura::compiler::types::is_bool(v))
        return aura::compiler::types::as_bool(v);
    if (aura::compiler::types::is_void(v))
        return false;
    return true;
}

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string string_value(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r)
        return "";
    auto& v = *r;
    if (!aura::compiler::types::is_string(v))
        return "";
    auto idx = aura::compiler::types::as_string_idx(v);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return "";
    return std::string(heap[idx]);
}

static bool set_source(aura::compiler::CompilerService& cs, std::string_view src) {
    std::string cmd = "(set-code \"";
    for (char c : src) {
        if (c == '\\' || c == '"')
            cmd += '\\';
        cmd += c;
    }
    cmd += "\")";
    return run_ok(cs, cmd);
}

bool test_replace_pattern_multi_match() {
    std::println("\n--- AC1: replace-pattern applies all matches in one call ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (define a (* 2 1)) (define b (* 2 2)) (define c (* 2 3)))")) {
        ++g_failed;
        return false;
    }
    if (!run_ok(cs, "(mutate:replace-pattern \"(* 2 ...)\" \"(+ ... ...)\" \"270\")")) {
        ++g_failed;
        return false;
    }
    auto src = string_value(cs, "(current-source :workspace)");
    CHECK(src.find("(* 2") == std::string::npos, "no (* 2 ...) left after multi replace");
    CHECK(src.find("(define a (+ 1))") != std::string::npos,
          "first define doubled via (+ ... ...)");
    CHECK(src.find("(define b (+ 2))") != std::string::npos, "second define doubled");
    CHECK(src.find("(define c (+ 3))") != std::string::npos, "third define doubled");
    return true;
}

bool test_query_and_replace_multi_match() {
    std::println("\n--- AC2: query-and-replace applies all predicate matches ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin 1 2 3)")) {
        ++g_failed;
        return false;
    }
    if (!run_ok(cs, "(mutate:query-and-replace (query:where :tag \"LiteralInt\") \"99\")")) {
        ++g_failed;
        return false;
    }
    auto src = string_value(cs, "(current-source :workspace)");
    CHECK(src.find("(begin 99 99 99)") != std::string::npos,
          "all three literal ints replaced with 99");
    return true;
}

bool test_replace_pattern_wildcard_multi() {
    std::println("\n--- AC3: wildcard replace-pattern multi-match ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (/ 10 2) (/ 20 2) (/ 30 2))")) {
        ++g_failed;
        return false;
    }
    if (!run_ok(cs, "(mutate:replace-pattern \"(/ ... 2)\" \"(* ... 0.5)\")")) {
        ++g_failed;
        return false;
    }
    auto src = string_value(cs, "(current-source :workspace)");
    CHECK(src.find("(/ ") == std::string::npos, "no (/ ...) forms left");
    CHECK(src.find("(* 10 0.5)") != std::string::npos, "first wildcard replace ok");
    CHECK(src.find("(* 20 0.5)") != std::string::npos, "second wildcard replace ok");
    CHECK(src.find("(* 30 0.5)") != std::string::npos, "third wildcard replace ok");
    return true;
}

int run_tests() {
    std::println("Issue #270 (StableNodeRef + atomic batch in replace primitives)\n");
    test_replace_pattern_multi_match();
    test_query_and_replace_multi_match();
    test_replace_pattern_wildcard_multi();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_270_detail

int aura_issue_270_run() {
    return aura_issue_270_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_270_run();
}
#endif