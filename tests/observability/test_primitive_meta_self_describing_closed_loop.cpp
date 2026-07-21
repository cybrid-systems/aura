// Issue #478/#480/#560/#583 (#1978 renamed): issue# moved from filename to header.
// test_primitive_meta_self_describing_closed_loop_480.cpp
// Issue #480: Self-describing primitives metadata + Agent extension kit.
//
// Non-duplicative with #583 (primitives-stats registry hot-path),
// #478 (primitive-error-stats pair), #560 (stats:list meta).
//
// AC1: query:primitive-meta-stats reachable
// AC2: primitive:describe returns meta for known builtin (+)
// AC3: query:primitive-list-with-meta returns non-empty list
// AC4: describe/list bumps meta stats counters
// AC5: documented meta count > 0 (builtin annotations)
// AC6: multi-round matrix — meta stats monotonic
// AC7: query regression (primitives-stats, primitive-error-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_480_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t meta_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:primitive-meta-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:primitive-meta-stats ---");
    const auto s0 = meta_stats(cs);
    const auto slots = cs.evaluator().get_primitive_slot_count();
    std::println("  primitive-meta-stats = {} registry_slots = {}", s0, slots);
    CHECK(s0 > 0, "primitive-meta-stats > 0");
    CHECK(slots > 0, "registry has primitives");

    std::println("\n--- AC2: primitive:describe known builtin ---");
    const auto desc0 = cs.evaluator().get_primitive_describe_count();
    auto desc = cs.eval("(primitive:describe \"+\")");
    const auto desc1 = cs.evaluator().get_primitive_describe_count();
    CHECK(desc && is_pair(*desc), "primitive:describe \"+\" returns pair");
    CHECK(desc1 > desc0, "describe bumps primitive_describe_count");
    auto arity = cs.eval("(car (primitive:describe \"+\"))");
    CHECK(arity && is_int(*arity), "describe arity is int");

    std::println("\n--- AC3: query:primitive-list-with-meta ---");
    const auto list0 = cs.evaluator().get_primitive_list_meta_count();
    auto lst = cs.eval("(engine:metrics \"query:primitive-list-with-meta\")");
    const auto list1 = cs.evaluator().get_primitive_list_meta_count();
    CHECK(lst && is_pair(*lst), "primitive-list-with-meta returns non-empty list");
    CHECK(list1 > list0, "list-with-meta bumps list_meta_count");

    std::println("\n--- AC4: meta stats bumped by describe/list ---");
    const auto stats4a = meta_stats(cs);
    (void)cs.eval("(primitive:describe \"not\")");
    (void)cs.eval("(engine:metrics \"query:primitive-list-with-meta\")");
    const auto stats4b = meta_stats(cs);
    std::println("  meta-stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b > stats4a, "describe+list bumps primitive-meta-stats");

    std::println("\n--- AC5: documented meta count ---");
    const auto documented = cs.evaluator().get_primitive_documented_meta_count();
    std::println("  documented_meta = {}", documented);
    CHECK(documented > 0, "builtin primitives have doc metadata");

    std::println("\n--- AC6: multi-round matrix monotonic ---");
    const auto stats6a = meta_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(primitive:describe \"-\")");
        (void)cs.eval("(primitive:describe \"not\")");
    }
    const auto stats6b = meta_stats(cs);
    std::println("  meta-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "meta-stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto ps = cs.eval("(engine:metrics \"query:primitives-stats\")");
    auto pes = cs.eval("(engine:metrics \"query:primitive-error-stats\")");
    CHECK(ps && is_int(*ps), "primitives-stats regression");
    CHECK(pes && is_pair(*pes), "primitive-error-stats regression");
}

} // namespace aura_480_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_480_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}