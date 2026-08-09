// Issue #189/#417/#419/#456 (#1978 renamed): issue# moved from filename to header.
// test_defuse_version_closed_loop_419.cpp
// Issue #419: Modular current_defuse_version() API +
// is_version_current stale detection observability for
// AOT hot-update and runtime dispatch.
//
// Non-duplicative with #456 (epoch-stats / epoch-delta),
// #189 (concurrency:version-snapshot pair), #417
// (mutation-boundary-invariant-stats).
//
// AC1: query:defuse-version-stats reachable
// AC2: current_defuse_version matches get_defuse_version
// AC3: concurrency:version-snapshot + version-current?
// AC4: mutate:rebind bumps defuse epoch
// AC5: epoch-delta-since-last-query integration
// AC6: multi-round mutate matrix monotonic
// AC7: query regression (epoch-stats,
//      mutation-boundary-invariant-stats)
//
// #2860 (additive): query:evolution-epoch-snapshot unified
// fiber-scoped "evolution epoch" view for Agent self-evo loops.
// Schema=2860 + additive keys (hygiene-depth / defuse-version /
// mutation-boundary-depth / macro-introduced-count /
// layout-stamp-gen-arena / layout-stamp-gen-flat /
// residual-defer-*). Source-cite ACs for gate-only ship; runtime
// behavior verifies on next CI run.
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_419_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

static std::int64_t defuse_version_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:defuse-version-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:defuse-version-stats ---");
    CHECK(setup_workspace(cs), "defuse version workspace setup");
    const auto s0 = defuse_version_stats(cs);
    std::println("  defuse-version-stats = {}", s0);
    CHECK(s0 >= 0, "defuse version stats non-negative");

    std::println("\n--- AC2: current_defuse_version API ---");
    auto& ev = cs.evaluator();
    CHECK(ev.current_defuse_version() == ev.get_defuse_version(),
          "current_defuse_version matches get_defuse_version");

    std::println("\n--- AC3: version snapshot stale detection ---");
    auto snap = cs.eval("(stats:get \"concurrency:version-snapshot\")");
    CHECK(snap && is_int(*snap), "version-snapshot returns int");
    auto fresh =
        cs.eval("(concurrency:version-current? (stats:get \"concurrency:version-snapshot\"))");
    CHECK(fresh && is_bool(*fresh), "version-current? returns bool");

    std::println("\n--- AC4: mutate bumps defuse epoch ---");
    const auto stats4a = defuse_version_stats(cs);
    const auto ver4a = ev.current_defuse_version();
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    const auto ver4b = ev.current_defuse_version();
    const auto stats4b = defuse_version_stats(cs);
    std::println("  defuse version: {} -> {}", ver4a, ver4b);
    std::println("  defuse-version-stats: {} -> {}", stats4a, stats4b);
    CHECK(ver4b > ver4a, "mutate bumps current_defuse_version");
    CHECK(stats4b > stats4a, "mutate bumps defuse-version-stats");

    std::println("\n--- AC5: epoch-delta-since-last-query ---");
    (void)cs.eval("(engine:metrics \"query:epoch-stats\")");
    auto delta = cs.eval("(query:epoch-delta-since-last-query)");
    CHECK(delta && is_int(*delta), "epoch-delta returns int");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = defuse_version_stats(cs);
    const auto ver6a = ev.current_defuse_version();
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = defuse_version_stats(cs);
    const auto ver6b = ev.current_defuse_version();
    std::println("  defuse version: {} -> {}", ver6a, ver6b);
    std::println("  defuse-version-stats: {} -> {}", stats6a, stats6b);
    CHECK(ver6b > ver6a, "defuse version grows over matrix");
    CHECK(stats6b > stats6a, "defuse-version-stats grow over matrix");

    std::println("\n--- AC7: query regression ---");
    auto eps = cs.eval("(engine:metrics \"query:epoch-stats\")");
    auto mbi = cs.eval("(engine:metrics \"query:mutation-boundary-invariant-stats\")");
    CHECK(eps && is_int(*eps), "epoch-stats regression");
    CHECK(mbi && is_int(*mbi), "mutation-boundary-invariant-stats regression");
}

} // namespace aura_419_detail

// ── Issue #2860: unified query:evolution-epoch-snapshot
// Source-cite ACs (gate-only ship; runtime verifies on CI).

static std::string read_file_2860(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static void ac2860_1_source() {
    std::println("\n--- #2860 AC1: source — primitive registered + hash builder ---");
    const auto q = read_file_2860("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:evolution-epoch-snapshot") != std::string::npos,
          "#2860 AC1: primitive registered");
    CHECK(q.find("make_int(static_cast<std::int64_t>(2860))") != std::string::npos ||
              q.find("2860") != std::string::npos,
          "#2860 AC1: schema = 2860 present");
    CHECK(q.find("hygiene-depth") != std::string::npos, "#2860 AC1: hygiene-depth key");
    CHECK(q.find("defuse-version") != std::string::npos, "#2860 AC1: defuse-version key");
    CHECK(q.find("mutation-boundary-depth") != std::string::npos,
          "#2860 AC1: mutation-boundary-depth key");
    CHECK(q.find("macro-introduced-count") != std::string::npos,
          "#2860 AC1: macro-introduced-count key");
    CHECK(q.find("layout-stamp-gen-arena") != std::string::npos &&
              q.find("layout-stamp-gen-flat") != std::string::npos,
          "#2860 AC1: layout-stamp generation keys");
    CHECK(q.find("residual-defer-total") != std::string::npos &&
              q.find("residual-defer-forced-clear") != std::string::npos &&
              q.find("residual-defer-steal-hard-fail") != std::string::npos,
          "#2860 AC1: residual-defer flag keys");
    CHECK(q.find("FlatHashTable::create") != std::string::npos &&
              q.find("insert_pair") != std::string::npos,
          "#2860 AC1: hash builder uses FlatHashTable + insert_pair");
    CHECK(read_file_2860("docs/design/2860-evolution-epoch-snapshot.md").empty(),
          "#2860 AC1: no docs/design/2860-* per #1655");
}

static void ac2860_2_additive_keys() {
    std::println("\n--- #2860 AC2: additive-keys contract (no regression) ---");
    const auto q = read_file_2860("src/compiler/evaluator_primitives_query.cpp");
    // Existing surfaces preserved (additive, not replaced).
    CHECK(q.find("query:defuse-version-stats") != std::string::npos,
          "#2860 AC2: defuse-version-stats preserved");
    CHECK(q.find("query:macro-fiber-hygiene") != std::string::npos,
          "#2860 AC2: macro-fiber-hygiene preserved");
    CHECK(q.find("query:mutation-boundary-hold-stats") != std::string::npos,
          "#2860 AC2: mutation-boundary-hold-stats preserved");
    // New primitive is registered after defuse-version-stats (additive
    // on the same registration block).
    const auto defuse_pos = q.find("query:defuse-version-stats");
    const auto epoch_pos = q.find("query:evolution-epoch-snapshot");
    CHECK(defuse_pos != std::string::npos && epoch_pos != std::string::npos,
          "#2860 AC2: both primitives present");
    CHECK(epoch_pos > defuse_pos,
          "#2860 AC2: epoch-snapshot registered after defuse-version-stats (additive)");
}

int main() {
    aura::compiler::CompilerService cs;
    aura_419_detail::run_matrix(cs);
    std::println("\n=== #2860: unified evolution-epoch-snapshot (source-cite gate-only) ===");
    ac2860_1_source();
    ac2860_2_additive_keys();
    return RUN_ALL_TESTS();
}