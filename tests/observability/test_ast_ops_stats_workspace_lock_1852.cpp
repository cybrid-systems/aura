// @category: unit
// @reason: Issue #1852 — compile:ast-ops-stats must shared_lock
// workspace_mtx_ for a single coherent workspace_flat() counter
// snapshot (pre-#1852 double unlocked read vs #1729 swap → UAF /
// mixed workspace totals). Sibling of #1851.
//
//   AC1: source cites #1852; shared_lock + workspace_mtx_; single load
//   AC2: engine:metrics returns hash with expected keys
//   AC3: set-code interleave still returns (no hang)

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
using aura::compiler::types::is_void;
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
        std::println("\n--- AC1: shared_lock on ast-ops-stats ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_00.cpp");
        CHECK(src.find("#1852") != std::string::npos, "cites #1852");
        auto pos = src.find("\"compile:ast-ops-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        // Span local build_hash lambda + locked snapshot.
        auto win = src.substr(pos, 3600);
        CHECK(win.find("shared_lock") != std::string::npos, "uses shared_lock");
        CHECK(win.find("workspace_mtx_") != std::string::npos, "locks workspace_mtx_");
        CHECK(win.find("workspace_flat") != std::string::npos, "reads workspace_flat");
        CHECK(win.find("children_call_count") != std::string::npos, "reads children counter");
        CHECK(win.find("dirty_upward_fast_fixed_point_count") != std::string::npos,
              "reads fast fixed-point counter");
        // Single call-site load under the lock (not two separate ifs).
        // Count only `ev.workspace_flat()` — comments may mention the name.
        auto lock_pos = win.find("shared_lock");
        CHECK(lock_pos != std::string::npos, "shared_lock in window");
        if (lock_pos != std::string::npos) {
            auto after = win.substr(lock_pos);
            std::size_t n = 0;
            for (std::size_t i = 0; (i = after.find("ev.workspace_flat()", i)) != std::string::npos;
                 i += 18)
                ++n;
            CHECK(n == 1, std::format("single ev.workspace_flat() after lock (got {})", n));
        }
        auto pre = src.substr(pos > 500 ? pos - 500 : 0, 500);
        CHECK(pre.find("#1729") != std::string::npos || win.find("#1729") != std::string::npos,
              "pairs with #1729");
        CHECK(pre.find("#1851") != std::string::npos || win.find("#1851") != std::string::npos,
              "cites sibling #1851");
    }

    // ── AC2: metrics hash shape ──
    {
        std::println("\n--- AC2: engine:metrics ast-ops-stats ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"compile:ast-ops-stats\")");
        CHECK(r.has_value(), "metrics returns");
        if (r)
            CHECK(is_hash(*r) || is_void(*r), "hash or void");
        if (r && is_hash(*r)) {
            auto c = cs.eval(
                "(hash-ref (engine:metrics \"compile:ast-ops-stats\") \"children-call-count\")");
            CHECK(c && is_int(*c) && as_int(*c) >= 0, "children-call-count >= 0");
            auto f = cs.eval("(hash-ref (engine:metrics \"compile:ast-ops-stats\") "
                             "\"dirty-upward-fast-fixed-point-hits\")");
            CHECK(f && is_int(*f) && as_int(*f) >= 0, "fast-fixed-point-hits >= 0");
        }
    }

    // ── AC3: after set-code / interleave ──
    {
        std::println("\n--- AC3: metrics after set-code ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(engine:metrics \"compile:ast-ops-stats\")");
        CHECK(r.has_value(), "metrics after set-code");
        for (int i = 0; i < 20; ++i) {
            CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(),
                  std::format("set-code iter {}", i));
            auto m = cs.eval("(engine:metrics \"compile:ast-ops-stats\")");
            CHECK(m.has_value(), std::format("metrics iter {}", i));
        }
    }

    std::println("\n=== test_ast_ops_stats_workspace_lock_1852: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
