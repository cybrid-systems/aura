// test_domain_production_sweep.cpp — Domain suite: production sweep/hardening/safety
//
// Wave 2 (#root_test_classification): collapses per-range production_* binaries'
// schema + field-presence gates into one suite. Behavioral extras remain in
// EXCLUDE_FROM_ALL root test_production_*.cpp (on-demand ninja).
//
// Cases: domain/cases/production_sweep_cases.hpp

#include "test_harness.hpp"
#include "domain/cases/production_sweep_cases.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::prod::kProdCases;
using aura::test::prod::kProdCasesCount;
using aura::test::prod::ProdCase;

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void run_prod_cases(CompilerService& cs) {
    std::println("\n=== Domain suite: production field-list schemas ({}) ===", kProdCasesCount);
    for (std::size_t i = 0; i < kProdCasesCount; ++i) {
        const ProdCase& c = kProdCases[i];
        auto h = cs.eval(aura::test::aura_call_expr(c.query));
        CHECK(h && is_hash(*h), std::format("{} returns hash ({})", c.query, c.source_stem));
        // #1261–#1265 lineage may report schema 1625 or 1261.
        const auto got = href(cs, c.query, "schema");
        const bool schema_ok = (got == c.schema) ||
                               (c.schema == 1625 && (got == 1625 || got == 1261)) ||
                               (c.schema == 1261 && (got == 1625 || got == 1261));
        CHECK(schema_ok,
              std::format("{} schema == {} (got {}, {})", c.query, c.schema, got, c.source_stem));
        for (std::size_t f = 0; f < c.n_fields; ++f) {
            const char* key = c.fields[f];
            auto v = cs.eval(
                std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(c.query), key));
            CHECK(v.has_value(),
                  std::format("{} field '{}' present ({})", c.query, key, c.source_stem));
        }
    }
}

} // namespace

int aura_issue_domain_production_sweep_run() {
    CompilerService cs;
    run_prod_cases(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_domain_production_sweep_run();
}
#endif
