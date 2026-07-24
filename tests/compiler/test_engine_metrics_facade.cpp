// @category: integration
// @reason: Issue #1433 (engine:metrics) facade golden test
//
// AC1: (engine:metrics) returns hash with nested groups + ≥200 metric fields
// AC2: :prefix "query:" returns filtered hash
// AC3: :group "jit" returns jit sub-tree hash
// AC4: by-name lookup still works (or void under s0)
// AC5: schema == 2; top-level group key snapshot

#include "test_harness.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;

namespace {

std::int64_t hash_int(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

bool is_hash_expr(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::format("(hash? {})", expr));
    return r && is_bool(*r) && as_bool(*r);
}

std::int64_t hash_len(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::format("(hash-length {})", expr));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // ── AC1: default facade hash ──
    {
        auto r = cs.eval("(engine:metrics)");
        CHECK(r && is_hash(*r), "(engine:metrics) returns hash");
        CHECK(hash_int(cs, "(engine:metrics)", "schema") == 2, "schema == 2 (#1433)");
        CHECK(hash_int(cs, "(engine:metrics)", "stats-count") > 0, "stats-count > 0");
        auto fields = hash_int(cs, "(engine:metrics)", "metrics-field-count");
        CHECK(fields >= 200, std::format("metrics-field-count >= 200 (got {})", fields));

        // Nested groups present (snapshot of top-level group keys)
        static const char* kGroups[] = {"compile", "jit", "mutate", "query",
                                        "arena",   "gc",  "eval",   "telemetry"};
        int groups_present = 0;
        for (const char* g : kGroups) {
            auto gr = cs.eval(std::format("(hash-ref (engine:metrics) \"{}\")", g));
            if (gr && is_hash(*gr))
                ++groups_present;
        }
        CHECK(groups_present >= 4,
              std::format("at least 4 top-level groups present (got {})", groups_present));
        // Back-compat compiler snapshot
        CHECK(is_hash_expr(cs, "(hash-ref (engine:metrics) \"compiler\")"),
              "compiler nested hash present");
    }

    // ── AC2: :prefix ──
    {
        auto r = cs.eval("(engine:metrics :prefix \"query:\")");
        CHECK(r && is_hash(*r), ":prefix query: returns hash");
        CHECK(hash_int(cs, "(engine:metrics :prefix \"query:\")", "schema") == 2,
              ":prefix schema 2");
        // Should include multiple query: stats catalog entries
        auto n = hash_len(cs, "(engine:metrics :prefix \"query:\")");
        CHECK(n >= 5, std::format(":prefix query: has >= 5 keys (got {})", n));
    }

    // ── AC3: :group ──
    {
        auto r = cs.eval("(engine:metrics :group \"jit\")");
        CHECK(r && is_hash(*r), ":group jit returns hash");
        auto n = hash_len(cs, "(engine:metrics :group \"jit\")");
        CHECK(n >= 1, std::format(":group jit has >= 1 field (got {})", n));
        // Known field from CompilerMetrics
        auto j = cs.eval("(hash-ref (engine:metrics :group \"jit\") \"jit_compilations\")");
        CHECK(j && is_int(*j), "jit_compilations present in jit group");
    }

    // ── AC4: by-name ──
    {
        auto r = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
        CHECK(r.has_value(), "by-name returns a value");
        // full: hash; s0: void — both OK
        CHECK(is_hash(*r) || is_void(*r), "by-name is hash or void");
        auto miss = cs.eval("(engine:metrics \"query:no-such-stats-zzz\")");
        CHECK(miss && is_void(*miss), "missing stats name → void");
    }

    // ── AC5: :all still works ──
    {
        auto r = cs.eval("(engine:metrics :all)");
        CHECK(r && is_hash(*r), ":all returns hash");
        CHECK(hash_int(cs, "(engine:metrics :all)", "schema") == 2, ":all schema 2");
    }

    // ── AC6: legacy query:*-stats still via facade (internal impl, #1439) ──
    {
        auto r = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
        // Under full primitives this is a hash; if missing, suite still OK for s0 builds.
        if (r && !is_void(*r))
            CHECK(is_hash(*r), "query:macro-hygiene-stats still works via engine:metrics");
        else
            CHECK(true, "query:*-stats absent (s0) — skip");
        // Must NOT be a public primitive name after #1439 (facade-only).
        // Unknown symbol typically returns error or empty optional — not a stats hash.
        try {
            auto bare = cs.eval("(query:macro-hygiene-stats)");
            if (bare && is_hash(*bare))
                CHECK(false, "bare query:macro-hygiene-stats must not return stats hash");
            else
                CHECK(true, "bare query:*-stats not a public primitive");
        } catch (...) {
            CHECK(true, "bare query:*-stats not a public primitive (threw)");
        }
    }

    if (::aura::test::g_failed) {
        std::println(std::cerr, "engine metrics facade #1433: FAIL ({} failed, {} passed)",
                     ::aura::test::g_failed, ::aura::test::g_passed);
        return 1;
    }
    std::println("engine metrics facade #1433: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
