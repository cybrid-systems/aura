// test_stdlib_production_review_923_940.cpp — Issues #923–#940 Phase 1
// Issue #923/#926/#932/#936/#940 (#1978 renamed): issue# moved from filename to header.
//
// Validates stdlib production-review observability surface + PrimMeta tiers
// + list-sort native path registration.

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // #923: list-sort primitive registered
    {
        auto r = cs.eval("(list-sort (list 3 1 2))");
        CHECK(r && is_pair(*r), "list-sort returns a list");
        // car should be 1 after sort
        auto car = cs.eval("(car (list-sort (list 3 1 2)))");
        CHECK(car && is_int(*car) && as_int(*car) == 1, "list-sort ascending");
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

    if (::aura::test::g_failed)
        return 1;
    std::println("stdlib production review #923–#940: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
