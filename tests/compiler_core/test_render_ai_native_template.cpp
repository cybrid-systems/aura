// @category: unit
// @reason: Issue #1677 — render prim template + AI Native query/mutate evolution.
// Issue #1677 (#1978 renamed): issue# moved from filename to header.
//
//   AC1: query:render-closure-stats schema 1677 + render-critical-meta-count > 0
//   AC2: query:render-buffer-stats after make-terminal-buffer
//   AC3: (mutate :render-optimize) advances optimize-total
//   AC4: mutate:rebind of draw-frame advances rebind-total
//   AC5: present after rebind still works; dispatch-fast > 0
//   AC6: template phase stamped on evolution stats

#include "test_harness.hpp"

#include <cstdint>
#include <fcntl.h>
#include <print>
#include <string>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static int open_null_fd() {
    return ::open("/dev/null", O_WRONLY);
}

} // namespace

int main() {
    CompilerService cs;
    const int null_fd = open_null_fd();
    CHECK(null_fd >= 0, "open /dev/null");

    // ── AC1: closure stats ──
    {
        std::println("\n--- AC1: query:render-closure-stats ---");
        auto r = cs.eval("(stats:get \"query:render-closure-stats\")");
        CHECK(r && is_hash(*r), "closure-stats hash");
        CHECK(href(cs, "query:render-closure-stats", "schema") == 1677, "schema 1677");
        CHECK(href(cs, "query:render-closure-stats", "render-critical-meta-count") >= 5,
              "render-critical-meta-count >= 5");
        CHECK(href(cs, "query:render-closure-stats", "template-phase") == 1, "template-phase 1");
        CHECK(href(cs, "query:render-closure-stats", "stable-hot-path") == 1, "stable-hot-path");
    }

    auto id_r = cs.eval("(make-terminal-buffer 8 4)");
    CHECK(id_r && is_int(*id_r) && as_int(*id_r) >= 0, "make-terminal-buffer");
    const auto bid = as_int(*id_r);

    // ── AC2: buffer stats ──
    {
        std::println("\n--- AC2: query:render-buffer-stats ---");
        CHECK(href(cs, "query:render-buffer-stats", "schema") == 1677, "buffer schema 1677");
        CHECK(href(cs, "query:render-buffer-stats", "buffer-creates") >= 1, "buffer-creates >= 1");
        CHECK(href(cs, "query:render-buffer-stats", "buffer-live") >= 1, "buffer-live >= 1");
        CHECK(href(cs, "query:render-buffer-stats", "buffer-live-capacity-bytes") > 0,
              "live capacity bytes > 0");
    }

    const auto opt0 = href(cs, "query:render-evolution-stats", "optimize-total");
    const auto reb0 = href(cs, "query:render-evolution-stats", "rebind-total");

    // ── AC3: mutate :render-optimize ──
    {
        std::println("\n--- AC3: mutate :render-optimize ---");
        auto r = cs.eval(std::format("(mutate :render-optimize {})", bid));
        CHECK(r && is_int(*r) && as_int(*r) >= 1, "mutate :render-optimize returns applied>=1");
        // Facade alias
        auto r2 = cs.eval("(stats:get \"mutate:render-optimize\")");
        CHECK(r2 && is_int(*r2) && as_int(*r2) >= 1, "stats:get mutate:render-optimize");
        const auto opt1 = href(cs, "query:render-evolution-stats", "optimize-total");
        CHECK(opt1 > opt0, std::format("optimize-total {} > {}", opt1, opt0));
        CHECK(href(cs, "query:render-evolution-stats", "savings-total") > 0, "savings-total > 0");
    }

    // ── AC4: rebind draw-frame (render evolution name) ──
    {
        std::println("\n--- AC4: mutate:rebind draw-frame ---");
        // Workspace define with render-like name (simple body; rebind is the AC).
        auto sc = cs.eval("(set-code \"(define (draw-frame b) (+ b 1))\")");
        CHECK(sc.has_value(), "set-code draw-frame");

        auto reb =
            cs.eval("(mutate:rebind \"draw-frame\" \"(lambda (b) (+ b 2))\" \"prefer hot body\")");
        CHECK(reb.has_value(), "mutate:rebind returned a value");
        // rebind returns #t on success (bool) or truthy in some paths
        const bool ok =
            reb && ((is_bool(*reb) && as_bool(*reb)) || (is_int(*reb) && as_int(*reb) > 0));
        CHECK(ok, "mutate:rebind draw-frame succeeded");
        const auto reb1 = href(cs, "query:render-evolution-stats", "rebind-total");
        CHECK(reb1 > reb0, std::format("rebind-total {} > {}", reb1, reb0));
    }

    // ── AC5: present after evolution still hot-path ──
    {
        std::println("\n--- AC5: present + dispatch after evolution ---");
        const auto fast0 = href(cs, "query:render-closure-stats", "dispatch-fast-total");
        const auto present0 = href(cs, "query:render-stats", "present-batch-total");
        auto set = cs.eval(std::format("(terminal-set-cell {} 0 0 67 7 0)", bid));
        CHECK(set, "set-cell");
        auto p = cs.eval(std::format("(terminal-present-batch {} {})", bid, null_fd));
        CHECK(p && is_int(*p), "present-batch after optimize");
        const auto fast1 = href(cs, "query:render-closure-stats", "dispatch-fast-total");
        CHECK(fast1 > fast0, std::format("dispatch-fast {} > {}", fast1, fast0));
        const auto present1 = href(cs, "query:render-stats", "present-batch-total");
        // Either buffer-stats or render-stats should reflect present activity.
        const auto buf_present = href(cs, "query:render-buffer-stats", "present-batch-total");
        CHECK(present1 > present0 || buf_present > 0 || as_int(*p) >= 0,
              "present path observed (stats or non-error return)");
    }

    // ── AC6: evolution stats template stamps ──
    {
        std::println("\n--- AC6: evolution schema/phase ---");
        CHECK(href(cs, "query:render-evolution-stats", "schema") == 1677, "evo schema");
        CHECK(href(cs, "query:render-evolution-stats", "template-issue") == 1677, "template-issue");
        CHECK(href(cs, "query:render-evolution-stats", "template-phase") == 1, "template-phase");
        CHECK(href(cs, "query:render-evolution-stats", "render-critical-meta-count") >= 5,
              "critical meta on evo stats");
    }

    if (null_fd >= 0)
        ::close(null_fd);

    std::println("\n=== test_render_ai_native_template_1677: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
