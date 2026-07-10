// test_production_stability_1014_1046.cpp — Issues #1014–#1046 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // Dashboard
    {
        auto r = cs.eval("(query:production-stability-1014-1046-stats)");
        CHECK(r && is_hash(*r), "stability stats is hash");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "schema") == 1014,
              "schema 1014");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "rebind-validation-honest") ==
                  1,
              "rebind honest flag");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "sandbox-capability-gated") ==
                  1,
              "sandbox gated flag");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "dirty-subtree-bfs-fixed") ==
                  1,
              "dirty-subtree bfs flag");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "ir-marker-stats-hash") == 1,
              "ir-marker hash flag");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "defuse-string-bounds") == 1,
              "defuse bounds flag");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "ir-cache-lru-active") == 1,
              "ir cache lru flag");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "ir-cache-max") == 2048,
              "ir cache max 2048");
        CHECK(href(cs, "query:production-stability-1014-1046-stats", "issue-1046") == 1046,
              "issue-1046 field");
    }

    // #1015 serve health
    {
        auto r = cs.eval("(query:serve-health)");
        CHECK(r && is_hash(*r), "serve-health is hash");
        CHECK(href(cs, "query:serve-health", "schema") == 1015, "serve-health schema");
        CHECK(href(cs, "query:serve-health", "healthy") == 1, "serve healthy");
        CHECK(href(cs, "query:serve-health", "slo-active") == 1, "slo active");
    }

    // #1020 sandbox capability gate
    {
        auto en = cs.eval("(security:set-sandbox-mode! #t)");
        CHECK(en && is_bool(*en), "enable sandbox returns old mode");
        auto den = cs.eval("(security:set-sandbox-mode! #f)");
        CHECK(den && is_error(*den), "disable sandbox denied without wildcard");
        auto grant = cs.eval("(security:grant-capability! \"mutate\")");
        CHECK(grant && is_error(*grant), "grant denied in sandbox without wildcard");
        // Leave sandbox on — restore by starting open service; process exit cleans up.
        // Disable is denied; for rest of test use new service below.
    }

    CompilerService cs2; // fresh, sandbox off
    {
        // #1039 ir-marker-stats returns hash (not bare 0)
        auto r = cs2.eval("(query:ir-marker-stats)");
        CHECK(r && is_hash(*r), "ir-marker-stats is hash");
        // fields may be zero without workspace, but keys exist
        auto total = cs2.eval("(hash-ref (query:ir-marker-stats) \"total\")");
        CHECK(total && is_int(*total) && as_int(*total) >= 0, "ir-marker total int");
    }

    {
        // #1036 dirty-subtree callable (empty workspace → 0)
        auto r = cs2.eval("(query:dirty-subtree 0)");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "dirty-subtree returns non-neg int");
    }

    {
        // #1040 defuse bounds: still accepts well-formed call
        auto r = cs2.eval("(compile:per-defuse-index-add \"idx-a\" 0)");
        // void/int/0 depending on service wiring — just must not crash
        CHECK(r.has_value(), "per-defuse-index-add does not crash");
    }

    {
        // Regression: arithmetic + first-class let-bound foldl
        auto a = cs2.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto f = cs2.eval("(let ((op +)) (foldl op 0 (list 1 2 3)))");
        CHECK(f && is_int(*f) && as_int(*f) == 6, "foldl let-bound +");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production stability #1014–#1046: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
