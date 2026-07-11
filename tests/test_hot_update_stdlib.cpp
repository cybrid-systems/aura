// test_hot_update_stdlib.cpp — Issue #1370: lib/std/hot-update stdlib

#include "test_harness.hpp"

#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

namespace {

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool eval_bool(CompilerService& cs, const char* expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    // ── Files present ──
    CHECK(file_exists("lib/std/hot-update.aura"), "hot-update.aura");
    CHECK(file_exists("lib/std/hot-update/reload.aura"), "reload.aura");
    CHECK(file_exists("lib/std/hot-update/region.aura"), "region.aura");
    CHECK(file_exists("lib/std/hot-update/monitor.aura"), "monitor.aura");
    CHECK(file_exists("lib/std/hot-update.aura-type"), "hot-update.aura-type");
    CHECK(file_exists("lib/std/tests/test_hot_update_reload.aura"), "test reload aura");
    CHECK(file_exists("lib/std/tests/test_hot_update_region.aura"), "test region aura");
    CHECK(file_exists("lib/std/tests/test_hot_update_monitor.aura"), "test monitor aura");

    {
        auto idx = read_file("lib/std/INDEX.aura");
        CHECK(idx.find("hot-update") != std::string::npos, "INDEX lists hot-update");
        CHECK(idx.find("hot-update/reload") != std::string::npos, "INDEX lists reload");
        CHECK(idx.find("hot-update/region") != std::string::npos, "INDEX lists region");
        CHECK(idx.find("hot-update/monitor") != std::string::npos, "INDEX lists monitor");
    }

    {
        auto main = read_file("lib/std/hot-update.aura");
        CHECK(main.find("hot-update:reload") != std::string::npos, "export reload");
        CHECK(main.find("hot-update:health") != std::string::npos, "export health");
        CHECK(main.find("aot:reload") != std::string::npos, "uses aot:reload");
    }

    // ── require + basic API ──
    {
        CompilerService cs;
        auto r = cs.eval("(require \"std/hot-update\" all:)");
        CHECK(r.has_value(), "require std/hot-update");

        CHECK(eval_bool(cs, "(= (hot-update:config-version (hot-update:default-config)) 0)"),
              "default version 0");
        CHECK(eval_bool(cs, "(= (hot-update:config-retries (hot-update:default-config)) 3)"),
              "default retries 3");

        // Missing module → #f (retry still returns false)
        auto miss = cs.eval("(hot-update:reload \"/tmp/aura_hu_missing_1370.so\")");
        CHECK(miss && is_bool(*miss) && !as_bool(*miss), "reload missing → #f");

        CHECK(eval_bool(cs, "(begin (hot-update:set-region! 9) (= (hot-update:get-region) 9))"),
              "region set/get");
        CHECK(eval_bool(cs, "(begin (hot-update:set-module-version! 5) "
                            "(= (hot-update:get-module-version) 5))"),
              "module version set/get");

        auto health = cs.eval("(hot-update:health)");
        CHECK(health && is_string(*health), "health returns string");
        if (health && is_string(*health)) {
            auto heap = cs.evaluator().string_heap();
            auto idx = as_string_idx(*health);
            CHECK(idx < heap.size(), "health string on heap");
            if (idx < heap.size()) {
                const auto& s = heap[idx];
                CHECK(s == "unknown" || s == "healthy" || s == "warning" || s == "degraded",
                      "health label valid");
            }
        }

        auto stats = cs.eval("(hot-update:stats)");
        CHECK(stats.has_value(), "hot-update:stats");
        auto rstats = cs.eval("(hot-update:reload-stats)");
        CHECK(rstats.has_value(), "hot-update:reload-stats");
    }

    // ── region module ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/hot-update/region\" all:)").has_value(), "require region");
        CHECK(eval_bool(cs, "(begin (region:clear!) (= (region:get) 0))"), "clear region");
        CHECK(eval_bool(cs, "(> (region:own-mask 7) 0)"), "own-mask non-zero");
        CHECK(eval_bool(cs, "(begin (region:isolate! 3) "
                            "(= (region:get) (region:own-mask 3)))"),
              "isolate! matches own-mask");
        CHECK(eval_bool(cs, "(region:compatible? 0 1)"), "compatible mine=0");
        CHECK(eval_bool(cs, "(not (region:compatible? 2 3))"), "incompatible different");
    }

    // ── reload strategies ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/hot-update/reload\" all:)").has_value(), "require reload");
        CHECK(eval_bool(cs, "(not ((make-manual-reload \"/tmp/nope_1370.so\")))"),
              "manual reload missing → #f");
        CHECK(eval_bool(cs, "(let ((o (make-once-reload \"/tmp/nope_1370.so\"))) "
                            "(begin (o \"reload\") (o \"done?\")))"),
              "once reload marks done?");
    }

    // ── monitor ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/hot-update/monitor\" all:)").has_value(), "require monitor");
        auto r =
            cs.eval("(let* ((hits 0)"
                    "       (m (make-hot-update-monitor (lambda (s) (set! hits (+ hits 1)) s))))"
                    "  (monitor:run-n m 2)"
                    "  (monitor:stop m)"
                    "  (and (= hits 2) (not (monitor:running? m))))");
        CHECK(r && is_bool(*r) && as_bool(*r), "monitor run-n + stop");
    }

    // ── stdlib .aura tests (load and expect #t) ──
    {
        CompilerService cs;
        auto r = cs.eval("(load \"lib/std/tests/test_hot_update_reload.aura\")");
        CHECK(r.has_value(), "load test_hot_update_reload.aura");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "test_hot_update_reload → #t");
    }
    {
        CompilerService cs;
        auto r = cs.eval("(load \"lib/std/tests/test_hot_update_region.aura\")");
        CHECK(r.has_value(), "load test_hot_update_region.aura");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "test_hot_update_region → #t");
    }
    {
        CompilerService cs;
        auto r = cs.eval("(load \"lib/std/tests/test_hot_update_monitor.aura\")");
        CHECK(r.has_value(), "load test_hot_update_monitor.aura");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "test_hot_update_monitor → #t");
    }

    // ── INDEX discoverability ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/INDEX\" all:)").has_value(), "require INDEX");
        auto help = cs.eval("(stdlib:help \"hot-update\")");
        CHECK(help.has_value(), "stdlib:help hot-update");
        auto pref = cs.eval("(stdlib:by-prefix \"hot-update\")");
        CHECK(pref.has_value(), "stdlib:by-prefix hot-update");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("hot-update stdlib #1370: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
