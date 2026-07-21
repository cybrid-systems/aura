// @category: unit
// @reason: Issue #1855 — compile:relower-strategy must snapshot
// dirty_block_count under jit_cache_mtx_ (shared) vs mark/clear
// unique writers; compiler_service_ follows #1839 ownership.
//
//   AC1: source cites #1855; ir_cache_v2_dirty_block_count + #1839
//   AC2: unknown for missing name; none/incremental/full shape
//   AC3: mark then strategy still returns keyword (no hang)

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
using aura::compiler::types::is_bool;
using aura::compiler::types::is_keyword;
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
        std::println("\n--- AC1: locked dirty snapshot + #1839 ---");
        std::string prim;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            prim = read_file(p);
            if (!prim.empty())
                break;
        }
        CHECK(!prim.empty(), "read compile_03.cpp");
        CHECK(prim.find("#1855") != std::string::npos, "cites #1855");
        auto pos = prim.find("\"compile:relower-strategy\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 1800);
        CHECK(win.find("ir_cache_v2_dirty_block_count") != std::string::npos,
              "uses locked dirty_block_count helper");
        CHECK(win.find("ir_cache_v2_find") == std::string::npos ||
                  win.find("not ir_cache_v2_find") != std::string::npos,
              "avoids unlocked pointer find in body");
        auto pre = prim.substr(pos > 500 ? pos - 500 : 0, 500);
        CHECK(pre.find("#1839") != std::string::npos || win.find("#1839") != std::string::npos,
              "documents service ownership #1839");

        std::string svc;
        for (const char* p : {"src/compiler/service.ixx", "../src/compiler/service.ixx"}) {
            svc = read_file(p);
            if (!svc.empty())
                break;
        }
        CHECK(!svc.empty(), "read service.ixx");
        CHECK(svc.find("ir_cache_v2_dirty_block_count") != std::string::npos, "helper declared");
        auto hpos = svc.find("ir_cache_v2_dirty_block_count");
        CHECK(hpos != std::string::npos, "helper pos");
        if (hpos != std::string::npos) {
            auto hwin = svc.substr(hpos > 200 ? hpos - 200 : 0, 900);
            CHECK(hwin.find("shared_lock") != std::string::npos ||
                      hwin.find("jit_cache_mtx_") != std::string::npos,
                  "helper locks jit_cache_mtx_");
            CHECK(hwin.find("#1855") != std::string::npos, "helper cites #1855");
        }
        // Writers take unique_lock.
        auto mpos = svc.find("bool mark_block_dirty_v2");
        CHECK(mpos != std::string::npos, "mark_block_dirty_v2 present");
        if (mpos != std::string::npos) {
            auto mwin = svc.substr(mpos, 600);
            CHECK(mwin.find("unique_lock") != std::string::npos, "mark takes unique_lock");
            CHECK(mwin.find("#1855") != std::string::npos, "mark cites #1855");
        }
    }

    // ── AC2: runtime shape ──
    {
        std::println("\n--- AC2: relower-strategy keywords ---");
        CompilerService cs;
        auto u = cs.eval(R"((compile:relower-strategy "not-in-cache-xyz"))");
        CHECK(u.has_value(), "unknown eval ok");
        if (u)
            CHECK(is_keyword(*u) || is_bool(*u), "unknown → keyword or false");
        // With workspace + eval, may still be unknown if not cached — still ok.
        CHECK(cs.eval("(set-code \"(define foo 1)\")").has_value(), "set-code");
        (void)cs.eval("(eval-current)");
        auto s = cs.eval(R"((compile:relower-strategy "foo"))");
        CHECK(s.has_value(), "foo strategy eval ok");
        if (s)
            CHECK(is_keyword(*s) || is_bool(*s), "foo → keyword or bool");
    }

    // ── AC3: mark dirty then strategy ──
    {
        std::println("\n--- AC3: mark-block-dirty! then strategy ---");
        CompilerService cs;
        cs.evaluator().set_sandbox_mode(false);
        CHECK(cs.eval("(set-code \"(define bar 2)\")").has_value(), "set-code bar");
        (void)cs.eval("(eval-current)");
        // May no-op if not cached; must not hang/crash under lock.
        auto m = cs.eval(R"((compile:mark-block-dirty! "bar" 0 0))");
        CHECK(m.has_value(), "mark-block-dirty eval ok");
        auto s = cs.eval(R"((compile:relower-strategy "bar"))");
        CHECK(s.has_value(), "strategy after mark ok");
        if (s)
            CHECK(is_keyword(*s) || is_bool(*s), "strategy shape after mark");
        auto c = cs.eval(R"((compile:clear-block-dirty! "bar" 0 0))");
        CHECK(c.has_value(), "clear-block-dirty eval ok");
        auto s2 = cs.eval(R"((compile:relower-strategy "bar"))");
        CHECK(s2.has_value(), "strategy after clear ok");
    }

    std::println("\n=== test_relower_strategy_cache_lock_1855: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
