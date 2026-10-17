// test_stdlib_production_review_923_940.cpp — Issues #923–#940 Phase 1
// Issue #923/#926/#932/#936/#940 (#1978 renamed): issue# moved from filename to header.
//
// Validates stdlib production-review observability surface + PrimMeta tiers
// + list-sort native path registration.

#include "test_harness.hpp"

#include <atomic>
#include <bit>
#include <cstdint>
#include <thread>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(aura::test::aura_href_expr(q, key));
    if (!r || !is_int(*r))
        return -1; // the read loop raised (e.g. no stable snapshot within max-tries)
    return as_int(*r);
}

} // namespace

// #4556: a native writer thread updates 64 f64 fields under a seqlock
// (odd while writing). Every field of one generation holds the same value,
// so a torn snapshot shows up as sum != 64 * first element. Returns the
// torn count, or -1 when the run failed / the writer never advanced.
alignas(8) static std::uint64_t g_shm_4556[1 + 64] = {};

static std::int64_t seqlock_torn_reads_4556(CompilerService& cs) {
    std::atomic<bool> stop{false};
    std::thread writer([&stop] {
        std::atomic_ref<std::uint64_t> seq(g_shm_4556[0]);
        double gen = 0.0;
        while (!stop.load(std::memory_order_relaxed)) {
            seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            gen += 1.0;
            const auto bits = std::bit_cast<std::uint64_t>(gen);
            for (int k = 1; k <= 64; ++k)
                std::atomic_ref<std::uint64_t>(g_shm_4556[k])
                    .store(bits, std::memory_order_relaxed);
            seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            // A data plane publishes a stats block periodically, not in a
            // tight loop; without a gap the reader can starve (bounded tries).
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });
    const auto addr = reinterpret_cast<std::uintptr_t>(&g_shm_4556[0]);
    // Install the c-* host prims the way (require "std/ffi") does.
    if (is_error(cs.evaluator().ensure_std_host_prims("std/ffi"))) {
        stop.store(true);
        writer.join();
        return -2;
    }
    auto r = cs.eval(std::format(
        "(let* ((shm (c-opaque {})) (seq (c-view shm 0 1 'u64 520))"
        "       (vals (c-view shm 8 64 'f64 520)) (snap (make-f64vector 64 0.0)))"
        "  (let loop ((i 0) (torn 0) (first -1.0) (last -1.0))"
        "    (if (< i 5000)"
        "        (begin (c-view-read-consistent! vals snap seq)"
        "               (let ((v (f64vector-ref snap 0)))"
        "                 (loop (+ i 1)"
        "                       (if (= (f64vector-sum snap) (* 64.0 v)) torn (+ torn 1))"
        "                       (if (< first 0.0) v first) v)))"
        "        (if (> last first) torn -1))))",
        addr));
    stop.store(true);
    writer.join();
    if (!r || !is_int(*r))
        return -1; // the read loop raised (e.g. no stable snapshot within max-tries)
    return as_int(*r);
}

int run_test_stdlib_production_review() {
    CompilerService cs;

    // #923: list-sort primitive registered
    {
        auto r = cs.eval("(list-sort (list 3 1 2))");
        CHECK(r && is_pair(*r), "list-sort returns a list");
        // car should be 1 after sort
        auto car = cs.eval("(car (list-sort (list 3 1 2)))");
        CHECK(car && is_int(*car) && as_int(*car) == 1, "list-sort ascending");
    }

    // #4549: Floats keep their type, strings sort, comparators apply, and an
    // unsupported list is an error — never a silent ().
    {
        auto f = cs.eval("(car (list-sort (list 2.5 1.7 3.9)))");
        CHECK(f && is_float(*f) && as_float(*f) == 1.7, "4549: list-sort keeps Floats");
        auto m = cs.eval("(equal? (list-sort (list 3 1.5 2)) (list 1.5 2 3))");
        CHECK(m && is_bool(*m) && as_bool(*m), "4549: mixed Int/Float numeric order");
        auto s = cs.eval("(equal? (list-sort (list \"b\" \"a\" \"c\")) (list \"a\" \"b\" \"c\"))");
        CHECK(s && is_bool(*s) && as_bool(*s), "4549: list-sort strings");
        auto c = cs.eval("(equal? (list-sort (list 1 3 2) >) (list 3 2 1))");
        CHECK(c && is_bool(*c) && as_bool(*c), "4549: list-sort comparator");
        auto e = cs.eval("(try (begin (list-sort (list 1 \"x\")) #f) (catch (e) #t))");
        CHECK(e && is_bool(*e) && as_bool(*e), "4549: mixed kinds raise");
    }

    // #926 / #923–#940: production review dashboard
    {
        auto r = cs.eval("(engine:metrics \"query:stdlib-production-review-stats\")");
        CHECK(r && is_hash(*r), "query:stdlib-production-review-stats is hash");
        CHECK(href(cs, "query:stdlib-production-review-stats", "schema") == 923,
              "schema field 923");
        CHECK(href(cs, "query:stdlib-production-review-stats", "issue-940") == 940,
              "issue-940 field present");
        CHECK(href(cs, "query:stdlib-production-review-stats", "registry-domain-peels") >= 1,
              "registry peels counted (#932)");
    }

    // #926 tier histogram
    {
        auto r = cs.eval("(engine:metrics \"query:primitive-tier-stats\")");
        CHECK(r && is_hash(*r), "query:primitive-tier-stats is hash");
        CHECK(href(cs, "query:primitive-tier-stats", "schema") == 926, "tier schema 926");
        CHECK(href(cs, "query:primitive-tier-stats", "slots") > 0, "has registry slots");
    }

    // Audit bump for each issue
    for (int issue = 923; issue <= 940; ++issue) {
        auto r = cs.eval(std::format("(stdlib:audit-bump {})", issue));
        CHECK(r && is_bool(*r) && as_bool(*r),
              std::format("stdlib:audit-bump {} ok", issue).c_str());
    }
    CHECK(href(cs, "query:stdlib-production-review-stats", "self-evo-safety") >= 1,
          "audit bump visible for #936");

    // list-sort bumps iterative sort counter
    CHECK(href(cs, "query:stdlib-production-review-stats", "list-iterative-sorts") >= 1,
          "list-sort bumped iterative-sorts");

    // #4556: seqlock reads of a concurrently written native block never tear.
    // Runs on cs, last: a second CompilerService in this process would leave
    // cs's stdlib-review prims bound to the other (destroyed) Evaluator.
    {
        const auto torn = seqlock_torn_reads_4556(cs);
        CHECK(torn == 0, "4556: c-view-read-consistent! never returns a torn snapshot");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("stdlib production review #923–#940: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_stdlib_production_review();
}
#endif
