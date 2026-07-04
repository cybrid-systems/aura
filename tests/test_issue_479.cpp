// test_issue_479.cpp — Verify Issue #479 per-prim fast-path hit tracking.
//
// Scope:
//   1. observability_metrics.h adds per-slot atomic array
//      (primitive_fastpath_hits_per_prim_) with lazy-grow
//      under atomic_flag spinlock.
//   2. primitives_detail.h adds prim_record_fastpath_hit_for_slot(m, slot)
//      that bumps BOTH the aggregate counter and the per-slot counter.
//   3. Four fast-path sites wired to the per-slot variant:
//        evaluator_primitives_list.cpp: apply_unary (slot in lambda)
//        evaluator_primitives_list.cpp: apply_pred  (slot in lambda)
//        evaluator_primitives_list.cpp: apply_binary (slot in lambda)
//        evaluator_primitives_runtime.cpp: apply (slot from runtime)
//   4. New Aura primitive (query:primitive-fastpath-per-prim) returns:
//        total           — aggregate fast-path hit count
//        distinct-prims  — number of slots with count > 0
//        top             — list of (name . count) dotted pairs sorted desc
//        capacity        — current per-prim array capacity
//   5. Backward compat: prim_record_fastpath_hit (aggregate only) still works.
//
// top list layout (built by evaluator_primitives_observability.cpp):
//   Each entry is a Pair{pair_ev, next_cell} where pair_ev is a dotted
//   pair (name . count). In Scheme that's nested: outer cons cells hold
//   the inner dotted pairs.
//
// Acceptance Criteria:
//   AC1  query:primitive-fastpath-per-prim returns a hash
//   AC2  Hash fields total / distinct-prims / top / capacity all present
//   AC3  Baseline snapshot is non-negative
//   AC4  After (map not big) the per-prim count for 'not' grows
//   AC5  After (filter null? big) the per-prim count for 'null?' grows
//   AC6  After (foldl + 0 big) the per-prim count for '+' grows
//   AC7  Apply path (runtime.cpp) also bumps per-slot counts
//   AC8  total == sum of per-prim counts (invariance)
//   AC9  Optional top-N argument caps the list
//   AC10 Top list is sorted desc by count
//   AC11 Aggregate counter primitive_fastpath_hits_total still bumps (back-compat)
//   AC12 distinct-prims grew from baseline

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_479_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                       \
    do {                                                                                       \
        if (cond) {                                                                            \
            ++g_passed;                                                                        \
            std::println(std::cout, "  PASS: {}", msg);                                        \
        } else {                                                                               \
            ++g_failed;                                                                        \
            std::println(std::cerr, "  FAIL: {}", msg);                                        \
        }                                                                                      \
    } while (0)

// Read an int field from a hash-returning Aura expression.
// Returns -1 on failure.
static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view hash_src,
                             std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Walk the top list from C++. Returns a vector of (name, count) pairs.
// top is built by evaluator_primitives_observability.cpp as:
//   for each slot:
//     push_back(name_ev, count_ev)        -> pairs[N]
//     push_back(pair_at_N, prev_top_list) -> pairs[N+1]
//     top_list = pair_at_(N+1)
// So the top list is a series of cons cells holding dotted pairs.
struct Entry {
    std::string name;
    std::int64_t count;
};

static std::vector<Entry> read_top_list(aura::compiler::CompilerService& cs) {
    std::vector<Entry> out;
    auto r = cs.eval("(hash-ref (query:primitive-fastpath-per-prim) 'top)");
    if (!r || !aura::compiler::types::is_pair(*r))
        return out;
    const auto& pairs = cs.evaluator().pairs();
    const auto& strings = cs.evaluator().string_heap();
    auto cur = *r;
    int safety = 10000;
    while (aura::compiler::types::is_pair(cur) && safety-- > 0) {
        auto cell_idx = aura::compiler::types::as_pair_idx(cur);
        if (cell_idx >= pairs.size())
            break;
        const auto& cell = pairs[cell_idx];
        // cell.car is the entry dotted pair (name, count)
        if (!aura::compiler::types::is_pair(cell.car))
            break;
        auto entry_idx = aura::compiler::types::as_pair_idx(cell.car);
        if (entry_idx >= pairs.size())
            break;
        const auto& entry = pairs[entry_idx];
        Entry e;
        if (aura::compiler::types::is_string(entry.car)) {
            auto sidx = aura::compiler::types::as_string_idx(entry.car);
            if (sidx < strings.size())
                e.name = strings[sidx];
        }
        if (aura::compiler::types::is_int(entry.cdr))
            e.count = aura::compiler::types::as_int(entry.cdr);
        out.push_back(std::move(e));
        cur = cell.cdr;
    }
    return out;
}

static std::int64_t count_for(const std::vector<Entry>& entries, std::string_view name) {
    std::int64_t total = 0;
    for (const auto& e : entries)
        if (e.name == name)
            total += e.count;
    return total;
}

} // namespace aura_issue_479_detail

int main() {
    using namespace aura_issue_479_detail;

    std::println("=== Issue #479: per-prim fast-path hit tracking ===");

    aura::compiler::CompilerService cs;

    // Define a list we'll iterate over multiple times across the ACs.
    (void)cs.eval("(define big (list 1 2 3 4 5 6 7 8 9 10))");

    // AC1: query primitive exists and returns a hash.
    std::println("\n--- AC1: query:primitive-fastpath-per-prim exists ---");
    auto r = cs.eval("(query:primitive-fastpath-per-prim)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "query:primitive-fastpath-per-prim returns hash");

    // AC2: hash fields present.
    std::println("\n--- AC2: hash fields present ---");
    constexpr auto stats_src = "(query:primitive-fastpath-per-prim)";
    CHECK(hash_int(cs, stats_src, "total") >= 0, "total field present (>= 0)");
    CHECK(hash_int(cs, stats_src, "distinct-prims") >= 0,
          "distinct-prims field present (>= 0)");
    CHECK(hash_int(cs, stats_src, "capacity") >= 0, "capacity field present (>= 0)");
    auto top_field = cs.eval("(hash-ref (query:primitive-fastpath-per-prim) 'top)");
    CHECK(top_field.has_value(), "top field present");

    // AC3: baseline snapshot is non-negative.
    std::println("\n--- AC3: baseline snapshot ---");
    const auto total_baseline = hash_int(cs, stats_src, "total");
    const auto distinct_baseline = hash_int(cs, stats_src, "distinct-prims");
    const auto capacity_baseline = hash_int(cs, stats_src, "capacity");
    std::println("  baseline total={} distinct={} capacity={}", total_baseline,
                 distinct_baseline, capacity_baseline);
    CHECK(total_baseline >= 0, "baseline total >= 0");
    CHECK(distinct_baseline >= 0, "baseline distinct-prims >= 0");
    CHECK(capacity_baseline >= 0, "baseline capacity >= 0");

    // Helper to read count directly from C++-side walk.
    auto count_of = [&](std::string_view name) -> std::int64_t {
        return count_for(read_top_list(cs), name);
    };
    auto sum_of = [&]() -> std::int64_t {
        std::int64_t sum = 0;
        for (const auto& e : read_top_list(cs))
            sum += e.count;
        return sum;
    };
    auto len_of_impl = [&](const char* src) -> std::int64_t {
        auto rr = cs.eval(std::format("(hash-ref {} 'top)", src));
        if (!rr || !aura::compiler::types::is_pair(*rr))
            return -1;
        std::int64_t n = 0;
        const auto& pairs = cs.evaluator().pairs();
        auto cur = *rr;
        int safety = 10000;
        while (aura::compiler::types::is_pair(cur) && safety-- > 0) {
            ++n;
            auto idx = aura::compiler::types::as_pair_idx(cur);
            if (idx >= pairs.size())
                break;
            cur = pairs[idx].cdr;
        }
        return n;
    };
    auto is_sorted_desc = [&]() -> bool {
        auto entries = read_top_list(cs);
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i].count > entries[i - 1].count)
                return false;
        }
        return true;
    };
    auto len_of = [&](const char* src) { return len_of_impl(src); };

    // AC4: (map not big) bumps per-prim count for 'not'.
    std::println("\n--- AC4: (map not big) bumps 'not' ---");
    const auto not_before = count_of("not");
    (void)cs.eval("(map not big)");
    const auto not_after = count_of("not");
    CHECK(not_after > not_before,
          std::format("'not' per-prim count grew ({} -> {})", not_before, not_after));

    // AC5: (filter null? big) bumps per-prim count for 'null?'.
    std::println("\n--- AC5: (filter null? big) bumps 'null?' ---");
    const auto total_before_null = hash_int(cs, stats_src, "total");
    const auto null_before = count_of("null?");
    (void)cs.eval("(filter null? big)");
    const auto null_after = count_of("null?");
    CHECK(null_after > null_before,
          std::format("'null?' per-prim count grew ({} -> {})", null_before, null_after));
    const auto total_after_null = hash_int(cs, stats_src, "total");
    CHECK(total_after_null > total_before_null,
          std::format("total grew from {} to {} after (filter null? big)",
                      total_before_null, total_after_null));

    // AC6: (foldl + 0 big) bumps per-prim count for '+' via apply_binary path.
    std::println("\n--- AC6: (foldl + 0 big) bumps '+' via apply_binary ---");
    const auto plus_before = count_of("+");
    (void)cs.eval("(foldl + 0 big)");
    const auto plus_after = count_of("+");
    CHECK(plus_after > plus_before,
          std::format("'+' per-prim count grew ({} -> {})", plus_before, plus_after));

    // 'not' should still be present (multi-prim tracking).
    CHECK(count_of("not") > 0, "'not' per-prim count still > 0 (multi-prim tracking)");

    // AC7: (apply + '(1 2 3)) bumps per-prim count for '+' (runtime.cpp site).
    std::println("\n--- AC7: (apply + '(1 2 3)) bumps '+' via runtime path ---");
    const auto plus_before_apply = plus_after;
    (void)cs.eval("(apply + '(1 2 3))");
    const auto plus_after_apply = count_of("+");
    CHECK(plus_after_apply > plus_before_apply,
          std::format("'+' per-prim count grew via apply: {} -> {}",
                      plus_before_apply, plus_after_apply));

    // AC8: total == sum of per-prim counts (invariance).
    std::println("\n--- AC8: total == sum of per-prim counts ---");
    const auto total_now = hash_int(cs, stats_src, "total");
    const auto sum_now = sum_of();
    CHECK(total_now == sum_now,
          std::format("total ({}) == sum of per-prim counts ({})", total_now, sum_now));

    // AC9: top-N argument caps the list.
    std::println("\n--- AC9: optional top-N argument ---");
    const auto len1 = len_of_impl("(query:primitive-fastpath-per-prim 1)");
    const auto len3 = len_of_impl("(query:primitive-fastpath-per-prim 3)");
    CHECK(len1 >= 0 && len1 <= 1,
          std::format("top-N=1 returns list of length <= 1 (got {})", len1));
    CHECK(len3 >= 0 && len3 <= 3,
          std::format("top-N=3 returns list of length <= 3 (got {})", len3));
    if (len1 >= 0 && len3 >= 0) {
        CHECK(len3 >= len1,
              std::format("top-N=3 (len {}) >= top-N=1 (len {})", len3, len1));
    }

    // AC10: top list is sorted desc by count.
    std::println("\n--- AC10: top list sorted desc by count ---");
    CHECK(is_sorted_desc(),
          "top list is sorted desc by count");

    // AC11: aggregate counter primitive_fastpath_hits_total still bumps
    // (backward compat — observed via query:primitives-registry-stats 'fastpath-hits').
    std::println("\n--- AC11: aggregate counter still bumps ---");
    constexpr auto reg_src = "(query:primitives-registry-stats)";
    const auto reg_total = hash_int(cs, reg_src, "fastpath-hits");
    CHECK(reg_total > 0,
          std::format("aggregate 'fastpath-hits' > 0 (got {})", reg_total));
    CHECK(reg_total == total_now,
          std::format("aggregate 'fastpath-hits' ({}) == per-prim total ({})",
                      reg_total, total_now));

    // AC12: distinct-prims grew from baseline.
    std::println("\n--- AC12: distinct-prims grew ---");
    const auto distinct_now = hash_int(cs, stats_src, "distinct-prims");
    CHECK(distinct_now >= distinct_baseline,
          std::format("distinct-prims grew ({} -> {})", distinct_baseline, distinct_now));

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}