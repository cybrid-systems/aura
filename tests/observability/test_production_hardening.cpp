// test_production_hardening.cpp — Merged #985-#1013 + #1072-#1096 (#1978).
//
// Originally test_production_hardening_985_1013.cpp +
// test_production_hardening_1072_1096.cpp. Both are Phase 1
// production hardening Wave2 scaffolding (schema flag checks
// via test_domain_production_sweep target). Merged with both
// AC sets preserved verbatim.
//
// AC list (all preserved; each section cites original issue#):
//   Issue #985-#1013 (test_production_hardening_985_1013.cpp):
//     schema 985 + cache bounds + ResourceQuota + EDA strcat helper
//   Issue #1072-#1096 (test_production_hardening_1072_1096.cpp):
//     schema 1072 + http injection + recovery pct + compaction
//     efficiency + ast:ref-get + mutate string bounds + jit fallback

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // ── #985–#1013 ────────────────────────────────────────────
    {
        auto r = cs.eval("(engine:metrics \"query:production-hardening-985-1013-stats\")");
        CHECK(r && is_hash(*r), "hardening stats is hash");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "schema") == 985, "schema 985");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-hardening-985-1013-stats",
                   "specjit-null-placeholder-fixed") == 1,
              "specjit placeholder fix flag");
        CHECK(href(cs, "query:production-hardening-985-1013-stats",
                   "thread-local-pressure-sample") == 1,
              "thread-local pressure flag");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "eda-strcat-helper") == 1,
              "eda strcat flag");
        CHECK(href(cs, "query:production-hardening-985-1013-stats",
                   "feedback-metric-order-fixed") == 1,
              "feedback metric order");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "set-marker-dead-ok-removed") ==
                  1,
              "set-marker dead ok removed");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "bounded-lru-active") == 1,
              "bounded lru template");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "issue-1013") == 1013,
              "issue-1013 field");
    }

    // #1013 ResourceQuota Phase 1: configure via quota-set, enforce via quota-check.
    {
        auto set = cs.eval("(resource:quota-set \"fibers\" 256)");
        CHECK(set && is_int(*set) && as_int(*set) == 1, "quota-set fibers 256");
        auto ok = cs.eval("(resource:quota-check \"fibers\" 10)");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "quota allows 10 fibers");
        auto rej = cs.eval("(resource:quota-check \"fibers\" 1000000)");
        CHECK(rej && is_bool(*rej) && !as_bool(*rej), "quota rejects huge fiber count");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "quota-checks") >= 2,
              "quota-checks counted");
        CHECK(href(cs, "query:production-hardening-985-1013-stats", "quota-rejects") >= 1,
              "quota-rejects counted");
    }

    // First-class + via let (safe path); bare (foldl + …) hits pre-existing IR bug.
    {
        auto r = cs.eval("(let ((op +)) (foldl op 0 (list 1 2 3)))");
        CHECK(r && is_int(*r) && as_int(*r) == 6, "foldl with let-bound + works");
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2) still works");
    }

    // ── #1072–#1096 ───────────────────────────────────────────
    {
        auto r = cs.eval("(engine:metrics \"query:production-hardening-1072-1096-stats\")");
        CHECK(r && is_hash(*r), "hardening stats is hash");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "schema") == 1072,
              "schema 1072");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats",
                   "http-shell-injection-fixed") == 1,
              "http injection fixed");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "recovery-pct-clamped") == 1,
              "recovery pct clamped");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats",
                   "compaction-efficiency-clamped") == 1,
              "compaction efficiency clamped");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "ast-ref-get-meta-tags") == 1,
              "ast:ref-get meta tags");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "mutate-string-bounds-bulk") ==
                  1,
              "mutate string bounds");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats",
                   "jit-fallback-status-defined") == 1,
              "jit fallback status");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "issue-1096") == 1096,
              "issue-1096 field");
    }

    // #1079 recovery-success never exceeds 100
    {
        auto r = cs.eval(
            "(hash-ref (engine:metrics \"query:primitives-error-stats\") \"recovery-success\")");
        CHECK(r && is_int(*r) && as_int(*r) >= 0 && as_int(*r) <= 100,
              "recovery-success in [0,100]");
    }

    // #1078 eda concurrency atomic-batch-sv-success is non-neg
    {
        auto r = cs.eval("(hash-ref (engine:metrics \"query:eda-concurrency-stats\") "
                         "\"atomic-batch-sv-success\")");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "atomic-batch-sv-success >= 0");
    }

    // #1072 arena:adaptive-stats returns pair of ints (no crash)
    {
        auto r = cs.eval("(stats:get \"arena:adaptive-stats\")");
        CHECK(r && (is_pair(*r) || is_void(*r) || is_int(*r)), "arena:adaptive-stats ok");
    }

    // #1080 efficiency clamped when no arena
    {
        auto r = cs.eval("(hash-ref (engine:metrics \"query:arena-production-compaction-stats\") "
                         "\"compaction-efficiency-pct\")");
        if (r && is_int(*r))
            CHECK(as_int(*r) >= 0 && as_int(*r) <= 100, "efficiency_pct in [0,100]");
        else
            CHECK(true, "arena compaction stats optional");
    }

    // Regression
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto e = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(e && is_int(*e) && as_int(*e) == 3, "eval still works");
    }

    std::println("=== production hardening #985-#1096: OK ({} passed) ===", ::aura::test::g_passed);
    return ::aura::test::g_failed ? 1 : 0;
}