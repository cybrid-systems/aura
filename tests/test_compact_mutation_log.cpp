// test_compact_mutation_log.cpp — Issue #1362: compact committed mutation log

#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.service;
import aura.compiler.value;

using aura::ast::FlatAST;
using aura::ast::MutationStatus;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Minimal FlatAST with one literal node for mutation targets
struct Fixture {
    aura::ast::ASTArena arena;
    FlatAST flat;
    NodeId lit = NULL_NODE;

    Fixture()
        : flat(arena.allocator()) {
        lit = flat.add_literal(1);
        flat.root = lit;
    }

    void add_committed(int n) {
        for (int i = 0; i < n; ++i) {
            (void)flat.add_mutation(lit, "tweak", "Int", "Int", "c");
        }
    }

    void add_then_rollback(int n) {
        for (int i = 0; i < n; ++i) {
            auto mid = flat.add_mutation_with_rollback(lit, "tweak", "Int", "Int", "rb",
                                                       MutationStatus::Committed, 0, 1, 2, true);
            (void)flat.rollback(mid);
        }
    }
};

} // namespace

int main() {
    // ── compact no-op when small ──
    {
        Fixture f;
        f.add_committed(50);
        CHECK(f.flat.mutation_count() == 50, "50 committed");
        auto d = f.flat.compact_mutation_log(1000, true);
        CHECK(d == 0, "compact no-op when size <= keep_recent");
        CHECK(f.flat.mutation_count() == 50, "size unchanged");
    }

    // ── 10K committed → compact(1000) → size ~1000 ──
    {
        Fixture f;
        f.add_committed(10000);
        CHECK(f.flat.mutation_count() == 10000, "10K before compact");
        const auto ops0 = f.flat.mutation_log_compact_ops();
        const auto rec0 = f.flat.mutation_log_compacted_records();
        auto dropped = f.flat.compact_mutation_log(1000, true);
        CHECK(dropped == 9000, "dropped 9000 committed");
        CHECK(f.flat.mutation_count() == 1000, "keep 1000 recent");
        CHECK(f.flat.mutation_log_compact_ops() == ops0 + 1, "compact ops +1");
        CHECK(f.flat.mutation_log_compacted_records() == rec0 + 9000, "compacted +9000");
        CHECK(f.flat.mutation_count() < 5000, "log size bounded < 5K");
    }

    // ── keep RolledBack when keep_all_rolledback=true ──
    {
        Fixture f;
        // 5K committed (old) + 1K rolledback + 1K recent committed
        f.add_committed(5000);
        f.add_then_rollback(1000);
        f.add_committed(1000);
        const auto before = f.flat.mutation_count();
        CHECK(before == 7000, "7000 total before compact");
        // keep_recent=1000 keeps last 1000 (recent committed);
        // older region has 5K committed (drop) + 1K rolledback (keep)
        auto dropped = f.flat.compact_mutation_log(1000, true);
        CHECK(dropped == 5000, "dropped 5K old committed");
        // remaining: 1000 RolledBack + 1000 recent = 2000
        CHECK(f.flat.mutation_count() == 2000, "1000 rolledback + 1000 recent");
        std::size_t rb = 0, cm = 0;
        for (const auto& rec : f.flat.all_mutations()) {
            if (rec.status == MutationStatus::RolledBack)
                ++rb;
            else
                ++cm;
        }
        CHECK(rb == 1000, "all RolledBack preserved");
        CHECK(cm == 1000, "only recent Committed kept");
    }

    // ── keep_all_rolledback=false drops RolledBack too ──
    {
        Fixture f;
        f.add_committed(2000);
        f.add_then_rollback(500);
        f.add_committed(500);
        auto dropped = f.flat.compact_mutation_log(500, false);
        // drop_to = 3000-500 = 2500; all first 2500 dropped
        CHECK(dropped == 2500, "drop 2500 without preserving RB");
        CHECK(f.flat.mutation_count() == 500, "only keep_recent remain");
    }

    // ── auto-compact at threshold on add_mutation ──
    {
        Fixture f;
        // Push past auto threshold (10K) — auto keep 1000
        f.add_committed(static_cast<int>(FlatAST::kMutationLogAutoCompactThreshold) + 50);
        CHECK(f.flat.mutation_count() <= FlatAST::kMutationLogAutoCompactKeepRecent + 50,
              "auto-compact bounded log after >10K adds");
        CHECK(f.flat.mutation_log_compact_ops() >= 1, "auto-compact bumped ops");
        CHECK(f.flat.mutation_count() < 5000, "auto log size < 5K");
    }

    // ── many cycles stay bounded ──
    {
        Fixture f;
        for (int cycle = 0; cycle < 50; ++cycle) {
            f.add_committed(500);
            (void)f.flat.compact_mutation_log(200, true);
        }
        CHECK(f.flat.mutation_count() <= 200, "after 50 cycles size <= keep_recent");
        CHECK(f.flat.mutation_log_compacted_records() >= 50ull * 300, "many records reclaimed");
    }

    // ── Aura primitives ──
    {
        CompilerService cs;
        // Need a workspace for mutation-log-compact
        (void)cs.eval("(set-code \"(define x 1)\")");
        auto c0 = cs.eval("(mutation-count)");
        CHECK(c0 && is_int(*c0), "mutation-count works");

        // Force some mutations if possible via mutate
        (void)cs.eval("(mutate:tweak-literal 0 1)");
        auto d = cs.eval("(mutation-log-compact 1000)");
        CHECK(d && is_int(*d), "mutation-log-compact returns int");

        // #553 owns query:mutation-log-stats (int sum). Compaction
        // metrics ship under a distinct name so int regression stays.
        auto s = cs.eval("(query:mutation-log-compact-stats)");
        CHECK(s && is_hash(*s), "query:mutation-log-compact-stats is hash");
        auto ls = href(cs, "query:mutation-log-compact-stats", "log-size");
        CHECK(ls >= 0, "log-size key");
        auto co = href(cs, "query:mutation-log-compact-stats", "compact-ops");
        CHECK(co >= 0, "compact-ops key");
        auto cr = href(cs, "query:mutation-log-compact-stats", "compacted-records");
        CHECK(cr >= 0, "compacted-records key");
        auto th = href(cs, "query:mutation-log-compact-stats", "auto-threshold");
        CHECK(th == static_cast<std::int64_t>(FlatAST::kMutationLogAutoCompactThreshold),
              "auto-threshold key == 10000");
        auto legacy = cs.eval("(query:mutation-log-stats)");
        CHECK(legacy && is_int(*legacy), "query:mutation-log-stats remains int (#553)");
    }

    // no workspace
    {
        CompilerService cs;
        auto d = cs.eval("(mutation-log-compact)");
        CHECK(d && is_int(*d) && as_int(*d) == 0, "compact without workspace → 0");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("compact mutation log #1362: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
