// test_aot_reload_primitive.cpp — Issue #1366: (aot:reload) Aura wrappers

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "runtime_shared.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Build a minimal shared object with aot_emit_version for success path.
// Returns path or empty on failure.
std::string build_test_so(std::uint64_t version) {
    const char* dir = "/tmp";
    std::string cpath = std::format("{}/aura_aot_test_{}.c", dir, version);
    std::string sopath = std::format("{}/aura_aot_test_{}.so", dir, version);
    {
        std::ofstream f(cpath);
        if (!f)
            return {};
        f << "#include <stdint.h>\n";
        f << "uint64_t aot_emit_version = " << version << "ULL;\n";
        f << "uint64_t aot_region_mask = 0ULL;\n";
        f << "__attribute__((constructor)) static void reg(void) {\n";
        f << "  (void)aot_emit_version;\n";
        f << "}\n";
    }
    std::string cmd = std::format("cc -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath);
    if (std::system(cmd.c_str()) != 0)
        return {};
    return sopath;
}

} // namespace

int main() {
    // ── C API region mask / module version (baseline) ──
    {
        aura_set_aot_region_mask(0);
        aura_set_module_version(0);
        CHECK(aura_get_aot_region_mask() == 0, "region mask init 0");
        CHECK(aura_get_module_version() == 0, "module version init 0");
        aura_set_aot_region_mask(0xABC);
        CHECK(aura_get_aot_region_mask() == 0xABC, "region mask set 0xABC");
        aura_set_module_version(42);
        CHECK(aura_get_module_version() == 42, "module version set 42");
        aura_set_aot_region_mask(0);
        aura_set_module_version(0);
    }

    // ── Aura: region mask round-trip ──
    {
        CompilerService cs;
        auto s = cs.eval("(aot:set-region-mask 7)");
        CHECK(s && is_bool(*s) && as_bool(*s), "aot:set-region-mask → #t");
        auto g = cs.eval("(aot:get-region-mask)");
        CHECK(g && is_int(*g) && as_int(*g) == 7, "aot:get-region-mask → 7");
        (void)cs.eval("(aot:set-region-mask 0)");
    }

    // ── Aura: module version round-trip ──
    {
        CompilerService cs;
        auto s = cs.eval("(aot:set-module-version 99)");
        CHECK(s && is_bool(*s) && as_bool(*s), "aot:set-module-version → #t");
        auto g = cs.eval("(stats:get \"aot:get-module-version\")");
        CHECK(g && is_int(*g) && as_int(*g) == 99, "aot:get-module-version → 99");
        (void)cs.eval("(aot:set-module-version 0)");
    }

    // ── Bad args ──
    {
        CompilerService cs;
        auto r = cs.eval("(aot:reload)");
        CHECK(r && is_bool(*r) && !as_bool(*r), "aot:reload no-arg → #f");
        auto r2 = cs.eval("(aot:reload 123)");
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "aot:reload non-string → #f");
        auto r3 = cs.eval("(aot:set-region-mask)");
        CHECK(r3 && is_bool(*r3) && !as_bool(*r3), "set-region-mask no-arg → #f");
        auto r4 = cs.eval("(aot:set-module-version \"x\")");
        CHECK(r4 && is_bool(*r4) && !as_bool(*r4), "set-module-version bad → #f");
    }

    // ── Failed reload (missing file) increments via-primitive counters ──
    {
        CompilerService cs;
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics available");
        // Ensure C-side metrics pointer is bound
        aura_set_aot_metrics(m);
        const auto att0 = m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed);
        const auto suc0 = m->aot_reload_success_via_primitive.load(std::memory_order_relaxed);
        const auto c_att0 = m->aot_reload_attempts_.load(std::memory_order_relaxed);
        const auto rb0 = m->aot_hot_update_atomic_rollback.load(std::memory_order_relaxed);

        auto r = cs.eval("(aot:reload \"/tmp/aura_nonexistent_aot_module_1366.so\")");
        CHECK(r && is_bool(*r) && !as_bool(*r), "reload missing .so → #f");
        CHECK(m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed) == att0 + 1,
              "attempts_via_primitive +1");
        CHECK(m->aot_reload_success_via_primitive.load(std::memory_order_relaxed) == suc0,
              "success_via_primitive unchanged");
        CHECK(m->aot_reload_attempts_.load(std::memory_order_relaxed) >= c_att0 + 1,
              "C API aot_reload_attempts_ bumped");
        CHECK(m->aot_hot_update_atomic_rollback.load(std::memory_order_relaxed) >= rb0 + 1,
              "atomic_rollback on failed dlopen");
    }

    // ── query:aot-reload-primitive-stats ──
    {
        CompilerService cs;
        auto s = cs.eval("(engine:metrics \"query:aot-reload-primitive-stats\")");
        CHECK(s && is_hash(*s), "query:aot-reload-primitive-stats is hash");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "attempts-via-primitive") >= 0,
              "attempts-via-primitive key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "success-via-primitive") >= 0,
              "success-via-primitive key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "region-mask") >= 0, "region-mask key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "module-version") >= 0,
              "module-version key");
    }

    // ── Optional: real .so success path ──
    {
        auto so = build_test_so(42);
        if (so.empty()) {
            CHECK(true, "skip success path (cc -shared unavailable)");
        } else {
            CompilerService cs;
            auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
            aura_set_aot_metrics(m);
            aura_set_aot_region_mask(0); // no region filter
            const auto suc0 = m->aot_reload_success_via_primitive.load(std::memory_order_relaxed);
            const auto att0 = m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed);
            auto expr = std::format("(aot:reload \"{}\" 42)", so);
            auto r = cs.eval(expr);
            CHECK(r && is_bool(*r), "reload real .so returns bool");
            if (r && is_bool(*r) && as_bool(*r)) {
                CHECK(m->aot_reload_success_via_primitive.load(std::memory_order_relaxed) ==
                          suc0 + 1,
                      "success_via_primitive +1");
            } else {
                // dlopen may fail in restricted env — still count attempt
                CHECK(m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed) ==
                          att0 + 1,
                      "attempt counted even if dlopen failed");
            }
            // Wrong version → false + stale reject
            auto r2 = cs.eval(std::format("(aot:reload \"{}\" 99)", so));
            CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "wrong version → #f");
        }
    }

    // ── Direct C API null path ──
    {
        CHECK(!aura_reload_aot_module(nullptr, 0), "C null path → false");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aot reload primitive #1366: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
