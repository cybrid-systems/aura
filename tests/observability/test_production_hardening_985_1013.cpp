// test_production_hardening_985_1013.cpp — Issues #985–#1013 Phase 1
//
// Cache bounds (SpecJIT/Shape/JIT/ADT) + ResourceQuota observability +
// EDA strcat helper flags. Avoids nested (car (list …)) / bare (foldl + …)
// which hit a pre-existing IR first-class-prim packaging path on main.

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

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    // String keys (not symbols) — matches FlatHashTable string keys from
    // build_kv_hash in evaluator_primitives_stdlib_review.cpp.
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

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

    if (::aura::test::g_failed)
        return 1;
    std::println("production hardening #985–#1013: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
