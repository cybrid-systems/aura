// @category: unit
// @reason: Issue #1671 — lookup_stats_impl uses C++20 heterogeneous
// Issue #1671 (#1978 renamed): issue# moved from filename to header.
// find(string_view) so hot-path stats catalog walks do not allocate a
// temporary std::string key per name.
//
// Paired note (transparent hash migration repair): production maps now use
// aura::core::TransparentStringHash (single string_view overload) shared via
// src/core/transparent_string_hash.hh. This suite remains the regression
// gate for heterogeneous stats-catalog lookup after the batch-3 include/
// map-template repairs (Primitives table_/hot_map_/name_to_slot_ + JIT).
//
//   AC1: engine:metrics by-name resolves a registered stats impl
//   AC2: engine:metrics :all returns hash with stats-count > 0
//   AC3: engine:metrics :prefix "query:" returns hash
//   AC4: stats:get path resolves (catalog lookup)
//   AC5: repeated by-name lookups remain stable (no crash / void regression)

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t hash_int(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_by_name() {
    std::println("\n--- AC1: engine:metrics by-name ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"typecheck-status\")");
    CHECK(r.has_value(), "by-name returns a value");
    if (r)
        CHECK(!is_void(*r), "typecheck-status not void (impl registered)");
}

static void ac2_all() {
    std::println("\n--- AC2: engine:metrics :all ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics :all)");
    CHECK(r && is_hash(*r), ":all returns hash");
    const auto n = hash_int(cs, "(engine:metrics :all)", "stats-count");
    CHECK(n > 0, "stats-count > 0");
    std::println("  stats-count={}", n);
}

static void ac3_prefix() {
    std::println("\n--- AC3: engine:metrics :prefix ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics :prefix \"query:\")");
    CHECK(r && is_hash(*r), ":prefix query: returns hash");
    CHECK(hash_int(cs, "(engine:metrics :prefix \"query:\")", "schema") == 2, "schema == 2");
}

static void ac4_stats_get() {
    std::println("\n--- AC4: stats:get ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"typecheck-status\")");
    CHECK(r.has_value(), "stats:get returns");
    if (r)
        CHECK(!is_void(*r), "stats:get non-void for known name");
}

static void ac5_repeated_lookups() {
    std::println("\n--- AC5: repeated by-name lookups ---");
    CompilerService cs;
    constexpr int kIters = 200;
    int ok = 0;
    for (int i = 0; i < kIters; ++i) {
        auto r = cs.eval("(engine:metrics \"typecheck-status\")");
        if (r && !is_void(*r))
            ++ok;
        auto r2 = cs.eval("(engine:metrics \"jit:intrinsic-count\")");
        if (r2 && !is_void(*r2))
            ++ok;
    }
    CHECK(ok == kIters * 2, "all repeated lookups resolved");
    std::println("  resolved {}/{}", ok, kIters * 2);
}

} // namespace

int main() {
    std::println("=== Issue #1671: lookup_stats_impl heterogeneous find ===");
    ac1_by_name();
    ac2_all();
    ac3_prefix();
    ac4_stats_get();
    ac5_repeated_lookups();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
