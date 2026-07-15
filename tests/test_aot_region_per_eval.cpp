// test_aot_region_per_eval.cpp — Issue #1367: per-evaluator AOT region isolation

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "runtime_shared.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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

} // namespace

int main() {
    // Reset process default state
    aura_set_aot_region_mask(0);
    aura_set_module_version(0);

    // ── Two Evaluators, independent region masks ──
    {
        Evaluator a;
        Evaluator b;
        const auto map0 = aura_aot_state_map_size();

        aura_set_aot_region_mask_for_eval(&a, 1);
        aura_set_aot_region_mask_for_eval(&b, 2);
        CHECK(aura_get_aot_region_mask_for_eval(&a) == 1, "eval A region=1");
        CHECK(aura_get_aot_region_mask_for_eval(&b) == 2, "eval B region=2");
        CHECK(aura_get_aot_region_mask() == 0, "default region still 0");
        CHECK(aura_aot_state_map_size() >= map0 + 2, "map has ≥2 eval entries");

        // Module versions independent
        aura_set_module_version_for_eval(&a, 10);
        aura_set_module_version_for_eval(&b, 20);
        CHECK(aura_get_module_version_for_eval(&a) == 10, "eval A module_ver=10");
        CHECK(aura_get_module_version_for_eval(&b) == 20, "eval B module_ver=20");
        CHECK(aura_get_module_version() == 0, "default module_ver still 0");

        // Defuse per-eval (0 falls back to global)
        aura_set_aot_defuse_version(100);
        CHECK(aura_get_aot_defuse_version_for_eval(&a) == 100, "defuse falls back to global");
        aura_set_aot_defuse_version_for_eval(&a, 55);
        CHECK(aura_get_aot_defuse_version_for_eval(&a) == 55, "eval A defuse=55");
        CHECK(aura_get_aot_defuse_version_for_eval(&b) == 100, "eval B still global 100");
        aura_set_aot_defuse_version(0);
    }

    // ── Cleanup removes map entries ──
    {
        const auto before = aura_aot_state_map_size();
        {
            Evaluator tmp;
            aura_set_aot_region_mask_for_eval(&tmp, 99);
            CHECK(aura_get_aot_region_mask_for_eval(&tmp) == 99, "tmp region set");
            CHECK(aura_aot_state_map_size() >= before + 1, "map grew for tmp");
        } // ~Evaluator → aura_cleanup_aot_state
        CHECK(aura_aot_state_map_size() == before, "map size restored after dtor");
        // Explicit cleanup is no-op if already gone
        Evaluator ghost;
        void* p = &ghost;
        aura_set_aot_region_mask_for_eval(p, 1);
        aura_cleanup_aot_state(p);
        CHECK(aura_get_aot_region_mask_for_eval(p) == 0, "after cleanup, recreated state is 0");
        aura_cleanup_aot_state(p);
    }

    // ── Backward compat global API ──
    {
        aura_set_aot_region_mask(0xDEAD);
        CHECK(aura_get_aot_region_mask() == 0xDEAD, "global set/get");
        CHECK(aura_get_aot_region_mask_for_eval(nullptr) == 0xDEAD, "nullptr == default");
        aura_set_module_version(7);
        CHECK(aura_get_module_version() == 7, "global module version");
        aura_set_aot_region_mask(0);
        aura_set_module_version(0);
    }

    // ── Aura primitives are per-CompilerService evaluator ──
    {
        CompilerService cs_a;
        CompilerService cs_b;
        (void)cs_a.eval("(aot:set-region-mask 11)");
        (void)cs_b.eval("(aot:set-region-mask 22)");
        auto ga = cs_a.eval("(aot:get-region-mask)");
        auto gb = cs_b.eval("(aot:get-region-mask)");
        CHECK(ga && is_int(*ga) && as_int(*ga) == 11, "Aura CS A region=11");
        CHECK(gb && is_int(*gb) && as_int(*gb) == 22, "Aura CS B region=22");

        (void)cs_a.eval("(aot:set-module-version 3)");
        (void)cs_b.eval("(aot:set-module-version 4)");
        auto va = cs_a.eval("(aot:get-module-version)");
        auto vb = cs_b.eval("(aot:get-module-version)");
        CHECK(va && is_int(*va) && as_int(*va) == 3, "Aura CS A module=3");
        CHECK(vb && is_int(*vb) && as_int(*vb) == 4, "Aura CS B module=4");
    }

    // ── Reload uses per-eval region (wrong region rejects) ──
    {
        // Build SO with region 1 if possible
        const char* cpath = "/tmp/aura_aot_region_1367.c";
        const char* sopath = "/tmp/aura_aot_region_1367.so";
        {
            std::ofstream f(cpath);
            f << "#include <stdint.h>\n"
                 "uint64_t aot_emit_version = 1ULL;\n"
                 "uint64_t aot_region_mask = 1ULL;\n"
                 "__attribute__((constructor)) static void reg(void) {}\n";
        }
        if (std::system(
                std::format("cc -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath).c_str()) == 0) {
            Evaluator eval;
            aura_set_aot_region_mask_for_eval(&eval, 2); // host expects region 2
            bool ok = aura_reload_aot_module_for_eval(&eval, sopath, 1);
            CHECK(!ok, "region mismatch rejects reload");
            aura_set_aot_region_mask_for_eval(&eval, 1); // match binary
            ok = aura_reload_aot_module_for_eval(&eval, sopath, 1);
            CHECK(ok, "matching region accepts reload");
            aura_cleanup_aot_state(&eval);
        } else {
            CHECK(true, "skip region reload SO test (cc unavailable)");
        }
    }

    // ── query exposes map size ──
    {
        CompilerService cs;
        auto s = cs.eval("(engine:metrics \"query:aot-reload-primitive-stats\")");
        CHECK(s && is_hash(*s), "stats hash");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "per-eval-state-map-size") >= 0,
              "per-eval-state-map-size key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "per-eval-region-sets") >= 0,
              "per-eval-region-sets key");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aot region per-eval #1367: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
