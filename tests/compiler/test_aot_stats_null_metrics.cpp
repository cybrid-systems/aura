// @category: unit
// @reason: Issue #1843 — compile:aot-stats must early-check
// Issue #1835/#1843 (#1978 renamed): issue# moved from filename to header.
// compiler_metrics_ (no per-field m? ternaries) and still return
// a zero-filled hash + schema when metrics are unset.
//
//   AC1: source cites #1843; early !m branch; no m ? load pattern
//   AC2: CompilerService path returns hash with schema 1516
//   AC3: bare Evaluator (no metrics) returns zero hash via service unset

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: early null check, no per-field m? ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp");
        CHECK(src.find("#1843") != std::string::npos, "cites #1843");
        auto pos = src.find("\"compile:aot-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 2800);
        CHECK(win.find("if (!m)") != std::string::npos, "early !m check");
        CHECK(win.find("zero_kv") != std::string::npos ||
                  win.find("make_int(0)") != std::string::npos,
              "zero-filled null path");
        // Fragile pre-#1843 pattern should not remain on loads.
        CHECK(win.find("m ? load(m->") == std::string::npos, "no m ? load ternaries");
        CHECK(win.find("schema") != std::string::npos, "schema key");
    }

    // ── AC2: service path ──
    {
        std::println("\n--- AC2: CompilerService aot-stats hash ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:aot-stats\")");
        if (!r)
            r = cs.eval("(compile:aot-stats)");
        CHECK(r && is_hash(*r), "returns hash");
        auto sch = cs.eval("(hash-ref (engine:metrics \"compile:aot-stats\") \"schema\")");
        if (!sch || !is_int(*sch))
            sch = cs.eval("(hash-ref (compile:aot-stats) \"schema\")");
        CHECK(sch && is_int(*sch) && as_int(*sch) == 1516, "schema=1516");
    }

    // ── AC3: null metrics path ──
    {
        std::println("\n--- AC3: null metrics still yields zero hash ---");
        // Use CompilerService then clear metrics on the evaluator —
        // under quiescence (#1835 contract allows set after eval done).
        CompilerService cs;
        cs.evaluator().set_compiler_metrics(nullptr);
        auto r = cs.eval("(engine:metrics \"compile:aot-stats\")");
        if (!r)
            r = cs.eval("(compile:aot-stats)");
        CHECK(r && is_hash(*r), "null metrics still returns hash (zeros)");
        auto ir =
            cs.eval("(hash-ref (engine:metrics \"compile:aot-stats\") \"per-function-ir-emits\")");
        if (!ir || !is_int(*ir))
            ir = cs.eval("(hash-ref (compile:aot-stats) \"per-function-ir-emits\")");
        CHECK(ir && is_int(*ir) && as_int(*ir) == 0, "zeros when no metrics");
    }

    std::println("\n=== test_aot_stats_null_metrics_1843: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
