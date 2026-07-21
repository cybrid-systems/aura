// @category: unit
// @reason: Issue #1795 — query:sv-verification-closedloop-stats-hash must
// Issue #1795 (#1978 renamed): issue# moved from filename to header.
// allocate FlatHashTable capacity ≥ 2× keys (not create(8) for 7 fields).
//
//   AC1: source cites #1795; capacity ≥ 16 / 2×keys
//   AC2: no FlatHashTable::create(8) at closedloop-stats site
//   AC3: all 7 keys present via hash-ref
//   AC4: schema == 630

#include "test_harness.hpp"

#include <fstream>
#include <print>
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
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: capacity ≥ 2×keys ---");
        auto src = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                               "../src/compiler/evaluator_primitives_compile.cpp"});
        CHECK(!src.empty(), "read compile_04.cpp");
        CHECK(src.find("#1795") != std::string::npos, "cites #1795");
        auto pos = src.find("\"query:sv-verification-closedloop-stats-hash\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 2500);
        CHECK(win.find("FlatHashTable::create(8)") == std::string::npos,
              "no create(8) at closedloop site");
        CHECK(win.find("kClosedloopKeys") != std::string::npos ||
                  win.find("create(16)") != std::string::npos ||
                  win.find("cap = 16") != std::string::npos,
              "capacity ≥ 16");
        CHECK(win.find("kClosedloopKeys * 2") != std::string::npos ||
                  win.find("* 2") != std::string::npos,
              "scales with 2× keys");
    }

    // ── AC3/AC4: runtime ──
    {
        std::println("\n--- AC3/AC4: all 7 keys + schema 630 ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:sv-verification-closedloop-stats-hash\")");
        CHECK(r && is_hash(*r), "returns hash");

        const char* keys[] = {
            "feedback-to-mutate-cycles",
            "stable-ref-captures-in-sv",
            "verification-dirty-propagations",
            "reverify-success",
            "rollback-on-partial",
            "ppa-savings-total",
            "schema",
        };
        for (const char* k : keys) {
            auto v = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:sv-verification-closedloop-stats-hash\") "
                "\"{}\")",
                k));
            CHECK(v && is_int(*v), std::format("key {} present as int", k));
        }
        auto schema =
            cs.eval("(hash-ref (engine:metrics \"query:sv-verification-closedloop-stats-hash\") "
                    "\"schema\")");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 630, "schema == 630");
    }

    std::println("\n=== test_closedloop_stats_hash_cap_1795: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
