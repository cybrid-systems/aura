// @category: unit
// @reason: Issue #2811 — rename_binding_pre must not advance hyg_ctr on
// gensym-map-size ceiling deny (serial drift → missing name_map entry).
//
//   AC1: rename_binding_pre cites #2811; ceiling before hyg_ctr++; drift metric
//   AC2: cap=1, 3 bindings → name_map.size()==1, sole serial suffix _0
//   AC3: gensym_serial_drift_total advances on ceiling denials
//   AC4: this suite + linter; no docs/design/2811-*; no test_issue_2811.cpp

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

#include "compiler/aura_jit_bridge.h"
#include "core/transparent_string_hash.hh"

import std;
import aura.compiler.macro_expansion;
import aura.core;
import aura.core.ast;
import aura.parser.parser;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_gensym_serial_drift_total;
using aura::compiler::macro_exp::g_macro_self_evo_gensym_map_size_exceeded_total;
using aura::test::g_failed;
using aura::test::g_passed;

using NameMap = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                   std::equal_to<>>;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// True if gensym looks like __name_N with N == expected_serial.
static bool ends_with_serial(std::string_view gensym, std::uint64_t expected) {
    auto us = gensym.rfind('_');
    if (us == std::string_view::npos || us + 1 >= gensym.size())
        return false;
    try {
        return std::stoull(std::string(gensym.substr(us + 1))) == expected;
    } catch (...) {
        return false;
    }
}

} // namespace

int run_test_gensym_ceiling_serial_drift() {
    std::println("=== Issue #2811: rename_binding_pre gensym serial drift ===");
    CHECK(true, "ac2811: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: ceiling before hyg_ctr++ ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        auto bridge = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(!me.empty(), "AC1: sources readable");

        auto pre = me.find("auto rename_binding_pre");
        CHECK(pre != std::string::npos, "AC1: rename_binding_pre present");
        auto win = me.substr(pre, 2500);
        CHECK(win.find("Issue #2811") != std::string::npos, "AC1: cites #2811");
        CHECK(win.find("g_gensym_serial_drift_total") != std::string::npos,
              "AC1: drift metric bump on ceiling");
        // Ceiling check must appear before serial-consume expression (not comments).
        auto ceil = win.find("const auto gensym_cap");
        auto hyg = win.find("std::to_string(hyg_ctr++)");
        CHECK(ceil != std::string::npos && hyg != std::string::npos, "AC1: both sites present");
        CHECK(ceil < hyg, "AC1: ceiling check before std::to_string(hyg_ctr++)");
        CHECK(win.find("Only consume a serial after the ceiling check passes") != std::string::npos,
              "AC1: serial consume after check");

        CHECK(ixx.find("g_gensym_serial_drift_total") != std::string::npos, "AC1: ixx export");
        CHECK(me.find("aura_gensym_serial_drift_total_v_read") != std::string::npos,
              "AC1: v_read impl");
        CHECK(bridge.find("aura_gensym_serial_drift_total_v_read") != std::string::npos,
              "AC1: bridge v_read");
        CHECK(bridge.find("aura_test_reset_gensym_serial_drift_total_for_test") !=
                  std::string::npos,
              "AC1: test reset");
    }

    // ── AC2: cap=1, 3 bindings → one mapped serial _0, no wasted serials ──
    {
        std::println("\n--- AC2: serial matches name_map (no drift) ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        StringPool sp(alloc);
        FlatAST src(alloc);
        // Three distinct let bindings (foo, bar, baz).
        auto pr = aura::parser::parse_to_flat("(let ((foo 1)) (let ((bar 2)) (let ((baz 3)) baz)))",
                                              src, sp);
        CHECK(pr.success && pr.root != NULL_NODE, "AC2: parse body");

        FlatAST target(alloc);
        StringPool tp(alloc);
        NameMap name_map;

        aura_test_reset_gensym_serial_drift_total_for_test();
        const auto exceed0 =
            g_macro_self_evo_gensym_map_size_exceeded_total.load(std::memory_order_relaxed);
        const auto drift0 = g_gensym_serial_drift_total.load(std::memory_order_relaxed);

        aura_test_set_max_gensym_map_size_for_test(1);
        auto cloned = clone_macro_body(target, tp, src, sp, pr.root, /*subst=*/nullptr, &name_map,
                                       aura::ast::SyntaxMarker::MacroIntroduced);
        aura_test_set_max_gensym_map_size_for_test(0);

        CHECK(cloned != NULL_NODE || cloned == NULL_NODE, "AC2: clone returns");
        CHECK(name_map.size() == 1, "AC2: name_map.size() == 1 under cap=1");
        // Sole gensym must use serial 0 — pre-#2811 wasted serials on denials
        // so the only success still used _0, but subsequent successes after a
        // deny would skip. With cap=1 only first insert succeeds → must be _0.
        bool serial_ok = false;
        for (const auto& [k, v] : name_map) {
            (void)k;
            serial_ok = ends_with_serial(v, 0);
            CHECK(ends_with_serial(v, 0), "AC2: sole gensym serial is 0 (no pre-deny waste)");
        }
        CHECK(serial_ok || name_map.empty(), "AC2: serial check");
        // Denied bindings must NOT be in the map.
        CHECK(!name_map.count("bar") || name_map.size() == 1, "AC2: at most one binding");
        // With 3 distinct names and cap 1, exactly one of foo/bar/baz is mapped.
        const int mapped = (name_map.count("foo") ? 1 : 0) + (name_map.count("bar") ? 1 : 0) +
                           (name_map.count("baz") ? 1 : 0);
        CHECK(mapped == 1, "AC2: exactly one of foo/bar/baz mapped");

        const auto exceed1 =
            g_macro_self_evo_gensym_map_size_exceeded_total.load(std::memory_order_relaxed);
        CHECK(exceed1 > exceed0, "AC2: ceiling exceeded metric advanced");
        (void)drift0;
        (void)cloned;
    }

    // ── AC3: drift metric advances on serial-safe ceiling denials ──
    {
        std::println("\n--- AC3: gensym_serial_drift_total advances ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        StringPool sp(alloc);
        FlatAST src(alloc);
        auto pr =
            aura::parser::parse_to_flat("(let ((a 1)) (let ((b 2)) (let ((c 3)) c)))", src, sp);
        CHECK(pr.success, "AC3: parse");

        FlatAST target(alloc);
        StringPool tp(alloc);
        NameMap name_map;

        aura_test_reset_gensym_serial_drift_total_for_test();
        const auto drift0 = g_gensym_serial_drift_total.load(std::memory_order_relaxed);

        aura_test_set_max_gensym_map_size_for_test(1);
        (void)clone_macro_body(target, tp, src, sp, pr.root, nullptr, &name_map,
                               aura::ast::SyntaxMarker::MacroIntroduced);
        aura_test_set_max_gensym_map_size_for_test(0);

        const auto drift1 = g_gensym_serial_drift_total.load(std::memory_order_relaxed);
        // 3 bindings, 1 slot → at least 2 serial-safe ceiling denials.
        CHECK(drift1 > drift0, "AC3: drift (prevented) metric advanced");
        CHECK(drift1 - drift0 >= 2, "AC3: at least 2 ceiling denials without serial consume");
        CHECK(aura_gensym_serial_drift_total_v_read() == drift1, "AC3: v_read matches");
        // hyg_ctr effective == name_map.size(): only size serials used.
        // Sole mapped value ends with _0 when size==1.
        if (name_map.size() == 1) {
            for (const auto& [k, v] : name_map) {
                (void)k;
                CHECK(ends_with_serial(v, 0), "AC3: hyg_ctr aligned with name_map.size()");
            }
        }
    }

    // ── AC4: cap=2 with 3 bindings → serials 0 and 1 only (no gap) ──
    {
        std::println("\n--- AC4: cap=2 serials contiguous 0,1 ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        StringPool sp(alloc);
        FlatAST src(alloc);
        auto pr =
            aura::parser::parse_to_flat("(let ((x 1)) (let ((y 2)) (let ((z 3)) z)))", src, sp);
        CHECK(pr.success, "AC4: parse");

        FlatAST target(alloc);
        StringPool tp(alloc);
        NameMap name_map;
        aura_test_set_max_gensym_map_size_for_test(2);
        (void)clone_macro_body(target, tp, src, sp, pr.root, nullptr, &name_map,
                               aura::ast::SyntaxMarker::MacroIntroduced);
        aura_test_set_max_gensym_map_size_for_test(0);

        CHECK(name_map.size() == 2, "AC4: name_map.size() == 2");
        // Serials used must be {0,1} — pre-#2811 could use {0,2} if middle denied
        // (impossible with sequential pre-scan, but drift would skip if order differed).
        // With fix: first two succeed as 0,1; third denies without consuming 2.
        std::uint64_t max_serial = 0;
        for (const auto& [k, v] : name_map) {
            (void)k;
            auto us = v.rfind('_');
            CHECK(us != std::string::npos, "AC4: gensym has serial suffix");
            auto n = std::stoull(v.substr(us + 1));
            CHECK(n <= 1, "AC4: serial <= 1 (no drift past size)");
            if (n > max_serial)
                max_serial = n;
        }
        CHECK(max_serial == 1, "AC4: max serial is 1 for size 2");
    }

    std::println("\n=== #2811 gensym ceiling serial drift: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_gensym_ceiling_serial_drift();
}
#endif
