// @category: integration
// @reason: tests the per_defuse_index header (no JIT required)

// test_per_defuse_index.cpp — Issue #411 fu1 follow-up #1
// scope-limited close: per-DefUseIndex caller tracking
// (replaces the global dep_caller_fn_ list with a
// per-DefUseIndex map so that invalidating one
// DefUseIndex only touches the callers of that specific
// index).
//
// Issue #411 fu1 follow-up #1's full scope is to make the
// per-symbol re-inference path the primary post-mutation
// path and to route it through DefUseIndex::query_def_use
// for O(uses) instead of the current O(n) per_symbol
// walk. This scope-limited close ships the per-DefUseIndex
// caller tracking infrastructure (the per-DefUseIndex
// map) plus the test that verifies the per-DefUseIndex
// isolation. The actual DefUseIndex routing in
// infer_flat_partial is a follow-up wiring (the indexed
// path will be the next commit in this series).
//
// Test cases:
//   AC1: fresh PerDefUseIndexTracker is empty
//   AC2: add_caller + get_callers for one DefUseIndex
//   AC3: per-DefUseIndex isolation — adding to one
//        DefUseIndex doesn't affect another
//   AC4: get_callers for unregistered DefUseIndex
//        returns empty
//   AC5: size_for_index reports the correct per-DefUseIndex
//        count
//   AC6: total_size sums across all DefUseIndexes
//   AC7: index_count reports the distinct count
//   AC8: clear() removes all state
//   AC9: DefUseIndex equality (FNV-1a hash on name)
//   AC10: copyable + movable

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <print>

#include "compiler/per_defuse_index.h"

namespace aura_per_defuse_index_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println("  FAIL: {}", msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; std::println("  PASS: {}  ({} = {})", msg, _a, _b); } \
    else          { ++g_failed; std::println("  FAIL: {}  ({} != {})", msg, _a, _b); } \
} while (0)

bool test_empty_tracker() {
    std::println("\n--- AC1: fresh PerDefUseIndexTracker is empty ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    CHECK_EQ(t.total_size(), 0u, "fresh tracker total_size == 0");
    CHECK_EQ(t.index_count(), 0u, "fresh tracker index_count == 0");
    return true;
}

bool test_add_and_get_for_one_index() {
    std::println("\n--- AC2: add_caller + get_callers for one DefUseIndex ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    using aura::compiler::per_defuse_index::Caller;
    t.add_caller(DefUseIndex{"foo"}, Caller{"caller1_for_foo"});
    auto callers = t.get_callers(DefUseIndex{"foo"});
    CHECK_EQ(callers.size(), 1u, "foo has 1 caller");
    if (!callers.empty())
        CHECK_EQ(callers[0].location, std::string{"caller1_for_foo"},
                 "caller location matches");
    return true;
}

bool test_per_index_isolation() {
    std::println("\n--- AC3: per-DefUseIndex isolation ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    using aura::compiler::per_defuse_index::Caller;
    // Populate defuse_index_1 with 2 callers, defuse_index_2
    // with 1 caller. Verify that each DefUseIndex's caller
    // list is independent — adding to one doesn't leak into
    // the other. This is the core property of the
    // per-DefUseIndex approach vs. the pre-existing global
    // list.
    t.add_caller(DefUseIndex{"foo"}, Caller{"caller1_for_foo"});
    t.add_caller(DefUseIndex{"foo"}, Caller{"caller2_for_foo"});
    t.add_caller(DefUseIndex{"bar"}, Caller{"caller1_for_bar"});

    auto foo_callers = t.get_callers(DefUseIndex{"foo"});
    auto bar_callers = t.get_callers(DefUseIndex{"bar"});

    CHECK_EQ(foo_callers.size(), 2u, "foo has exactly 2 callers (per-index isolation)");
    CHECK_EQ(bar_callers.size(), 1u, "bar has exactly 1 caller (per-index isolation)");
    if (foo_callers.size() == 2) {
        CHECK_EQ(foo_callers[0].location, std::string{"caller1_for_foo"},
                 "foo[0] is caller1_for_foo");
        CHECK_EQ(foo_callers[1].location, std::string{"caller2_for_foo"},
                 "foo[1] is caller2_for_foo");
    }
    if (bar_callers.size() == 1) {
        CHECK_EQ(bar_callers[0].location, std::string{"caller1_for_bar"},
                 "bar[0] is caller1_for_bar");
    }
    return true;
}

bool test_get_callers_for_unregistered_index() {
    std::println("\n--- AC4: get_callers for unregistered DefUseIndex returns empty ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    t.add_caller(DefUseIndex{"foo"}, {"x"});
    auto missing = t.get_callers(DefUseIndex{"missing"});
    CHECK_EQ(missing.size(), 0u, "unregistered index returns empty caller list");
    return true;
}

bool test_size_for_index() {
    std::println("\n--- AC5: size_for_index reports the correct per-index count ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    t.add_caller(DefUseIndex{"foo"}, {"c1"});
    t.add_caller(DefUseIndex{"foo"}, {"c2"});
    t.add_caller(DefUseIndex{"foo"}, {"c3"});
    t.add_caller(DefUseIndex{"bar"}, {"c1"});
    CHECK_EQ(t.size_for_index(DefUseIndex{"foo"}), 3u, "foo has 3 callers");
    CHECK_EQ(t.size_for_index(DefUseIndex{"bar"}), 1u, "bar has 1 caller");
    CHECK_EQ(t.size_for_index(DefUseIndex{"missing"}), 0u,
             "missing has 0 callers");
    return true;
}

bool test_total_size() {
    std::println("\n--- AC6: total_size sums across all indexes ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    t.add_caller(DefUseIndex{"foo"}, {"c1"});
    t.add_caller(DefUseIndex{"foo"}, {"c2"});
    t.add_caller(DefUseIndex{"bar"}, {"c1"});
    t.add_caller(DefUseIndex{"baz"}, {"c1"});
    t.add_caller(DefUseIndex{"baz"}, {"c2"});
    CHECK_EQ(t.total_size(), 5u, "total_size == 5 (sum across 3 indexes)");
    return true;
}

bool test_index_count() {
    std::println("\n--- AC7: index_count reports the distinct count ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    t.add_caller(DefUseIndex{"foo"}, {"c1"});
    t.add_caller(DefUseIndex{"foo"}, {"c2"});  // same index
    t.add_caller(DefUseIndex{"bar"}, {"c1"});  // new index
    CHECK_EQ(t.index_count(), 2u, "index_count == 2 (foo + bar, dedup'd)");
    return true;
}

bool test_clear() {
    std::println("\n--- AC8: clear() removes all state ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    t.add_caller(DefUseIndex{"foo"}, {"c1"});
    t.add_caller(DefUseIndex{"bar"}, {"c1"});
    CHECK(t.total_size() > 0, "tracker non-empty before clear");
    t.clear();
    CHECK_EQ(t.total_size(), 0u, "total_size == 0 after clear");
    CHECK_EQ(t.index_count(), 0u, "index_count == 0 after clear");
    auto callers = t.get_callers(DefUseIndex{"foo"});
    CHECK_EQ(callers.size(), 0u, "foo's caller list empty after clear");
    return true;
}

bool test_equality() {
    std::println("\n--- AC9: DefUseIndex equality (FNV-1a hash on name) ---");
    using aura::compiler::per_defuse_index::DefUseIndex;
    DefUseIndex a{"foo"};
    DefUseIndex b{"foo"};
    DefUseIndex c{"bar"};
    CHECK(a == b, "DefUseIndex with same name are equal");
    CHECK(!(a == c), "DefUseIndex with different names are unequal");
    // Hash consistency
    std::hash<DefUseIndex> hasher;
    CHECK_EQ(hasher(a), hasher(b), "FNV-1a hash is consistent for equal keys");
    CHECK(hasher(a) != hasher(c), "FNV-1a hash differs for different names");
    return true;
}

bool test_copyable_and_movable() {
    std::println("\n--- AC10: copyable + movable ---");
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t;
    using aura::compiler::per_defuse_index::DefUseIndex;
    t.add_caller(DefUseIndex{"foo"}, {"c1"});
    // Copy ctor
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t2(t);
    CHECK_EQ(t2.total_size(), 1u, "copy ctor preserves state");
    // Move ctor
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t3(std::move(t2));
    CHECK_EQ(t3.total_size(), 1u, "move ctor preserves state");
    // Copy assignment
    aura::compiler::per_defuse_index::PerDefUseIndexTracker t4;
    t4 = t3;
    CHECK_EQ(t4.total_size(), 1u, "copy assignment preserves state");
    return true;
}

}  // namespace aura_per_defuse_index_detail

int main() {
    using namespace aura_per_defuse_index_detail;
    std::println("=== Issue #411 fu1 follow-up #1: per-DefUseIndex tracking (scope-limited) ===");
    test_empty_tracker();
    test_add_and_get_for_one_index();
    test_per_index_isolation();
    test_get_callers_for_unregistered_index();
    test_size_for_index();
    test_total_size();
    test_index_count();
    test_clear();
    test_equality();
    test_copyable_and_movable();
    std::println("\n=== Summary: {}/{} passed, {}/{} failed ===",
                 g_passed, g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
