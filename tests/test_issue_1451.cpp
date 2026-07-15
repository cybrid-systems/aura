// @category: integration
// @reason: CompilerService governance prim (#1451)
//
// test_issue_1451.cpp — Issue #1451: Primitives Governance Policy +
// (primitive:validate-new) Agent-Proof check.
//
// ACs:
//   AC1: (primitive:validate-new "string-fake-zzz") → ok=#f, blocked=#t, category=string
//   AC2: (primitive:validate-new "query:fake-stats") → blocked stats
//   AC3: (primitive:validate-new "+") → already-registered=#t, ok=#f
//   AC4: (primitive:validate-new "agent-proof-free-name-1451") → ok=#t, requires-red-line=#t
//   AC5: (primitive:describe "primitive:validate-new") is registered
//   AC6: (engine:surface) still present (governance inventory)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1451_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;

std::int64_t hash_int(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -999;
    return as_int(*r);
}

bool hash_bool(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_bool(*r))
        return false;
    return as_bool(*r);
}

void ac1_blocked_string() {
    std::println("\n--- AC1: freeze-blocked string-* name ---");
    CompilerService cs;
    auto r = cs.eval("(primitive:validate-new \"string-fake-zzz-1451\")");
    CHECK(r.has_value() && is_hash(*r), "returns hash");
    CHECK(!hash_bool(cs, "(primitive:validate-new \"string-fake-zzz-1451\")", "ok"), "ok=#f");
    CHECK(hash_bool(cs, "(primitive:validate-new \"string-fake-zzz-1451\")", "blocked"),
          "blocked=#t");
    CHECK(hash_bool(cs, "(primitive:validate-new \"string-fake-zzz-1451\")", "prefer-stdlib"),
          "prefer-stdlib=#t");
    auto cat = cs.eval("(string=? (hash-ref (primitive:validate-new \"string-fake-zzz-1451\") "
                       "\"blocked-category\") \"string\")");
    CHECK(cat.has_value() && is_bool(*cat) && as_bool(*cat), "blocked-category=string");
}

void ac2_blocked_stats() {
    std::println("\n--- AC2: freeze-blocked *-stats name ---");
    CompilerService cs;
    const char* expr = "(primitive:validate-new \"query:agent-hallucinated-stats\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value() && is_hash(*r), "returns hash");
    CHECK(!hash_bool(cs, expr, "ok"), "ok=#f");
    CHECK(hash_bool(cs, expr, "blocked"), "blocked=#t");
    auto cat =
        cs.eval("(string=? (hash-ref (primitive:validate-new \"query:agent-hallucinated-stats\") "
                "\"blocked-category\") \"stats\")");
    CHECK(cat.has_value() && is_bool(*cat) && as_bool(*cat), "blocked-category=stats");
}

void ac3_already_registered() {
    std::println("\n--- AC3: already-registered core name ---");
    CompilerService cs;
    const char* expr = "(primitive:validate-new \"+\")";
    CHECK(hash_bool(cs, expr, "already-registered"), "already-registered=#t");
    CHECK(!hash_bool(cs, expr, "ok"), "ok=#f");
}

void ac4_free_name() {
    std::println("\n--- AC4: free name still requires red-line ---");
    CompilerService cs;
    const char* expr = "(primitive:validate-new \"agent-proof-free-name-1451\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value() && is_hash(*r), "returns hash");
    CHECK(hash_bool(cs, expr, "ok"), "ok=#t");
    CHECK(!hash_bool(cs, expr, "blocked"), "blocked=#f");
    CHECK(!hash_bool(cs, expr, "already-registered"), "already-registered=#f");
    CHECK(hash_bool(cs, expr, "requires-red-line"), "requires-red-line=#t");
    CHECK(hash_int(cs, expr, "schema") == 1, "schema=1");
}

void ac5_describe_registered() {
    std::println("\n--- AC5: primitive:validate-new is registered ---");
    CompilerService cs;
    auto slot = cs.evaluator().primitives().slot_for_name("primitive:validate-new");
    CHECK(slot < cs.evaluator().primitives().slot_count(), "registered on Primitives");
    auto d = cs.eval("(primitive:describe \"primitive:validate-new\")");
    CHECK(d.has_value() && !is_void(*d), "(primitive:describe …) non-void");
}

void ac6_surface_inventory() {
    std::println("\n--- AC6: engine:surface inventory still works ---");
    CompilerService cs;
    auto r = cs.eval("(engine:surface)");
    CHECK(r.has_value() && is_hash(*r), "(engine:surface) hash");
    CHECK(hash_int(cs, "(engine:surface)", "target-budget") == 420, "target-budget=420");
}

} // namespace aura_issue_1451_detail

int main() {
    using namespace aura_issue_1451_detail;
    std::println("=== Issue #1451: Primitives Governance + validate-new ===");
    ac1_blocked_string();
    ac2_blocked_stats();
    ac3_already_registered();
    ac4_free_name();
    ac5_describe_registered();
    ac6_surface_inventory();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
