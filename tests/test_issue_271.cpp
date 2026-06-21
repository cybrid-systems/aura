// @category: integration
// @reason: uses CompilerService to verify incremental tag_arity_index

// test_issue_271.cpp — Issue #271: incremental tag_arity_index_
// maintenance across mutate without full O(N) rebuild.

#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_271_detail {

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

bool test_incremental_sync_after_mutate() {
    std::println("\n--- AC1: index stays valid after mutate (no full clear) ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    if (!set_source(cs, "(begin (define a (* 2 1)) (define b (* 2 2)))")) {
        ++g_failed;
        return false;
    }
    ev.force_build_tag_arity_index();
    const std::size_t entries_after_build = ev.tag_arity_index_entry_count();
    const auto gen_after_build = ev.tag_arity_index_synced_gen();
    CHECK(entries_after_build > 0, "index populated after initial build");

    if (!run_ok(cs, "(mutate:replace-pattern \"(* 2 ...)\" \"(+ ... ...)\" \"271\")")) {
        ++g_failed;
        return false;
    }

    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_size() > 0, "index buckets remain after mutate sync");
    CHECK(ev.tag_arity_index_entry_count() > 0, "index entries remain after mutate sync");
    CHECK(ev.tag_arity_index_synced_gen() >= gen_after_build,
          "synced generation advanced after structural mutate");

    auto count = run_int(cs, "(length (query:pattern \"(+ ...)\"))");
    CHECK(count == 2, "query:pattern finds post-mutate (+ ...) nodes via incremental index");
    return true;
}

bool test_query_pattern_after_multi_mutate() {
    std::println("\n--- AC2: query:pattern correct after query/mutate loop ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 1) (+ 2 2) (+ 3 3))")) {
        ++g_failed;
        return false;
    }
    auto before = run_int(cs, "(length (query:pattern \"(+ ... ...)\"))");
    CHECK(before == 3, "three (+ ...) matches before mutate");

    if (!run_ok(cs, "(mutate:replace-pattern \"(+ ... ...)\" \"(* ... ...)\" \"271\")")) {
        ++g_failed;
        return false;
    }
    auto after_plus = run_int(cs, "(length (query:pattern \"(+ ... ...)\"))");
    auto after_star = run_int(cs, "(length (query:pattern \"(* ... ...)\"))");
    CHECK(after_plus == 0, "no (+ ...) left after replace");
    CHECK(after_star == 3, "three (* ...) found after incremental index sync");
    return true;
}

bool test_no_rebuild_when_unchanged() {
    std::println("\n--- AC3: second build is no-op when flat unchanged ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    if (!set_source(cs, "(define x 1)")) {
        ++g_failed;
        return false;
    }
    ev.force_build_tag_arity_index();
    const std::size_t size1 = ev.tag_arity_index_synced_size();
    const auto gen1 = ev.tag_arity_index_synced_gen();
    const std::size_t entries1 = ev.tag_arity_index_entry_count();

    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_synced_size() == size1, "synced size unchanged on second build");
    CHECK(ev.tag_arity_index_synced_gen() == gen1, "synced gen unchanged on second build");
    CHECK(ev.tag_arity_index_entry_count() == entries1, "entry count unchanged on second build");
    return true;
}

bool test_invalidate_resets_sync_state() {
    std::println("\n--- AC4: invalidate clears sync metadata ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    if (!set_source(cs, "(define x 1)")) {
        ++g_failed;
        return false;
    }
    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_synced_size() > 0, "synced size set after build");
    ev.invalidate_tag_arity_index_for_test();
    CHECK(ev.tag_arity_index_synced_size() == 0, "synced size cleared after invalidate");
    CHECK(ev.tag_arity_index_synced_gen() == 0, "synced gen cleared after invalidate");
    return true;
}

int run_tests() {
    std::println("Issue #271 (incremental tag_arity_index maintenance)\n");
    test_incremental_sync_after_mutate();
    test_query_pattern_after_multi_mutate();
    test_no_rebuild_when_unchanged();
    test_invalidate_resets_sync_state();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_271_detail

int aura_issue_271_run() { return aura_issue_271_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_271_run(); }
#endif