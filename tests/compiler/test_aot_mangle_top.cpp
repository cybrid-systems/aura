// test_aot_mangle_top.cpp — Issue #1369 / #2015:
// __top__ always includes _vN; per-function version probe;
// full `_eN_lN` parse + env/linear stale detection.

#include "test_harness.hpp"
#include "compiler/aot_mangle.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/runtime_shared.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <string>

import std;

using aura::compiler::aot_link_name;
using aura::compiler::aot_mangle_has_version_suffix;
using aura::compiler::aot_mangle_version_is_stale;
using aura::compiler::aot_mangle_version_is_stale_detail;
using aura::compiler::aot_parse_full_version_suffix;
using aura::compiler::aot_parse_version_suffix;
using aura::compiler::AotVersionSuffix;
using aura::compiler::CompilerMetrics;
using aura::compiler::mangle_aot_name;

// Build a minimal .so with aot_top_fn_version + aot_fn_versions table.
static bool write_probe_so(const std::string& path, std::uint64_t top_ver,
                           std::uint64_t other_ver) {
    const std::string c_path = path + ".c";
    {
        std::ofstream f(c_path);
        if (!f)
            return false;
        f << "#include <stdint.h>\n";
        f << "const unsigned long long aot_emit_version = " << top_ver << "ULL;\n";
        f << "const unsigned long long aot_top_fn_version = " << top_ver << "ULL;\n";
        f << "const unsigned aot_fn_versions_len = 2;\n";
        f << "const unsigned long long aot_fn_versions[] = {" << top_ver << "ULL, " << other_ver
          << "ULL};\n";
        f << "const char* const aot_fn_version_names[] = {\"__top__\", \"helper\"};\n";
    }
    std::string cmd = "cc -shared -fPIC -o " + path + " " + c_path + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    std::remove(c_path.c_str());
    return rc == 0;
}

int main() {
    // ── mangle always includes _vN for __top__ ──
    {
        auto m0 = mangle_aot_name("__top__", 0, 0);
        CHECK(m0 == "__top___v0", "__top__ v0 → __top___v0");
        auto m7 = mangle_aot_name("__top__", 0, 7);
        CHECK(m7 == "__top___v7", "__top__ v7 → __top___v7");
        CHECK(aot_link_name("__top__", 0, 0) == "__top__", "link name stays __top__");
        CHECK(aot_link_name("__top__", 0, 7) == "__top__", "link name stays __top__ even v7");
    }

    // ── non-top always versioned ──
    {
        CHECK(mangle_aot_name("my_fn", 5, 0) == "my_fn_5_v0", "my_fn v0");
        CHECK(mangle_aot_name("my_fn", 5, 7) == "my_fn_5_v7", "my_fn v7");
        CHECK(aot_link_name("my_fn", 5, 7) == "my_fn_5_v7", "link == mangle for non-top");
    }

    // ── parse / stale helpers ──
    {
        std::uint64_t v = 99;
        CHECK(aot_parse_version_suffix("__top___v0", &v) && v == 0, "parse __top___v0");
        CHECK(aot_parse_version_suffix("my_fn_5_v7", &v) && v == 7, "parse my_fn_5_v7");
        CHECK(!aot_parse_version_suffix("__top__", &v), "bare __top__ has no suffix");
        CHECK(aot_mangle_has_version_suffix("__top___v0"), "has suffix");
        CHECK(!aot_mangle_version_is_stale("__top___v7", 7), "v7 matches expected 7");
        CHECK(aot_mangle_version_is_stale("__top___v7", 8), "v7 stale vs expected 8");
        CHECK(aot_mangle_version_is_stale("__top__", 0), "missing suffix → stale");
    }

    // ── C API parse wrappers ──
    {
        std::uint64_t v = 0;
        CHECK(aura_aot_parse_version_suffix("fn_0_v42", &v) && v == 42, "C parse v42");
        CHECK(!aura_aot_mangle_version_is_stale("fn_0_v42", 42), "C not stale");
        CHECK(aura_aot_mangle_version_is_stale("fn_0_v42", 1), "C stale");
    }

    // ── two binaries same emit_version, different top body epochs ──
    // (demonstrable via mangle identity without full emit)
    {
        auto top_a = mangle_aot_name("__top__", 0, 10);
        auto top_b = mangle_aot_name("__top__", 0, 11);
        CHECK(top_a != top_b, "different __top__ epochs → different mangled ids");
        CHECK(aot_mangle_version_is_stale(top_a, 11), "top_a stale vs epoch 11");
        CHECK(!aot_mangle_version_is_stale(top_b, 11), "top_b matches epoch 11");
    }

    // ── dlopen probe: aot_top_fn_version + table ──
    {
        const char* so_path = "/tmp/aura_aot_fn_version_probe_1369.so";
        if (write_probe_so(so_path, 10, 20)) {
            void* h = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
            CHECK(h != nullptr, "dlopen probe so");
            if (h) {
                CompilerMetrics metrics;
                aura_set_aot_metrics(&metrics);
                const auto stale0 = metrics.aot_fn_version_probe_stale_total.load();

                CHECK(aura_aot_probe_fn_version(h, "__top__") == 10, "probe __top__ → 10");
                CHECK(aura_aot_probe_fn_version(h, "helper") == 20, "probe helper → 20");
                CHECK(aura_aot_probe_fn_version(h, "missing") == 10,
                      "unknown name falls back to aot_emit_version=10");

                CHECK(!aura_aot_fn_version_is_stale(h, "__top__", 10), "top not stale");
                CHECK(aura_aot_fn_version_is_stale(h, "__top__", 99), "top stale vs 99");
                CHECK(metrics.aot_fn_version_probe_stale_total.load() == stale0 + 1,
                      "stale probe metric +1");
                CHECK(aura_aot_fn_version_is_stale(h, "helper", 1), "helper stale vs 1");

                aura_set_aot_metrics(nullptr);
                ::dlclose(h);
            }
            std::remove(so_path);
        } else {
            std::println("  (skip dlopen probe: failed to build probe .so)");
        }
    }

    // ── null handle ──
    {
        CHECK(aura_aot_probe_fn_version(nullptr, "__top__") == ~std::uint64_t{0},
              "null handle → missing");
        CHECK(!aura_aot_fn_version_is_stale(nullptr, "__top__", 0),
              "null handle + expected 0 → not stale (legacy trust)");
        CHECK(aura_aot_fn_version_is_stale(nullptr, "__top__", 5),
              "null handle + expected 5 → stale");
    }

    // ── Issue #2015: full `_eN_lN` parse + env/linear stale ──
    {
        std::println("\n--- #2015: full env/linear suffix parse + stale ---");
        // Mangle emits `_e_l` when either non-zero.
        auto m = mangle_aot_name("cap", 0, /*defuse=*/7, /*env=*/5, /*linear=*/1);
        CHECK(m.find("_v7") != std::string::npos, "mangle has _v7");
        CHECK(m.find("_e5") != std::string::npos, "mangle has _e5");
        CHECK(m.find("_l1") != std::string::npos, "mangle has _l1");
        CHECK(m == "cap_0_v7_e5_l1", "exact full suffix shape");

        // Parse full: `_v7_e5_l1`
        AotVersionSuffix full{};
        CHECK(aot_parse_full_version_suffix(m, &full), "full parse ok");
        CHECK(full.has_defuse && full.defuse_version == 7, "defuse 7");
        CHECK(full.has_env_linear && full.env_frame_version == 5, "env 5");
        CHECK(full.linear_state == 1, "linear 1");

        // Defuse-only parse still works with trailing _e_l (bugfix).
        std::uint64_t v = 0;
        CHECK(aot_parse_version_suffix("cap_0_v7_e5_l1", &v) && v == 7,
              "defuse parse ignores trailing _e_l");
        CHECK(aot_parse_version_suffix("__top___v0_e0_l1", &v) && v == 0, "v0 with _e_l");

        // Absent env/linear: pure defuse name
        AotVersionSuffix bare{};
        CHECK(aot_parse_full_version_suffix("my_fn_5_v3", &bare), "bare parse");
        CHECK(bare.defuse_version == 3 && !bare.has_env_linear, "no env/linear");

        // Stale: defuse matches, env differs → stale
        CHECK(!aot_mangle_version_is_stale(m, 7, 5, 1), "full match not stale");
        CHECK(aot_mangle_version_is_stale(m, 7, 9, 1), "env drift → stale");
        CHECK(aot_mangle_version_is_stale(m, 7, 5, 2), "linear drift → stale");
        CHECK(aot_mangle_version_is_stale(m, 8, 5, 1), "defuse drift → stale");
        // Host expects env/linear but mangled has none → stale
        CHECK(aot_mangle_version_is_stale("fn_0_v7", 7, 5, 0), "host env vs bare mangle");
        // Pure defuse path still works (expected env/linear 0, bare mangle)
        CHECK(!aot_mangle_version_is_stale("fn_0_v7", 7), "legacy defuse-only ok");

        bool d_stale = false, e_stale = false, l_stale = false;
        CHECK(aot_mangle_version_is_stale_detail(m, 7, 9, 1, &d_stale, &e_stale, &l_stale),
              "detail stale");
        CHECK(!d_stale && e_stale && !l_stale, "only env flag");

        // C API full parse + stale_ex
        std::uint64_t cd = 0, ce = 0;
        std::uint8_t cl = 0;
        CHECK(aura_aot_parse_full_version_suffix("cap_0_v7_e5_l1", &cd, &ce, &cl), "C full parse");
        CHECK(cd == 7 && ce == 5 && cl == 1, "C full components");
        CHECK(!aura_aot_mangle_version_is_stale_ex("cap_0_v7_e5_l1", 7, 5, 1), "C ex match");
        CHECK(aura_aot_mangle_version_is_stale_ex("cap_0_v7_e5_l1", 7, 6, 1), "C ex env stale");

        // Metric bump on env/linear drift via C stale_ex
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        const auto drift0 = metrics.aot_env_frame_version_drift_prevented.load();
        CHECK(aura_aot_mangle_version_is_stale_ex("cap_0_v7_e5_l1", 7, 99, 1), "C drift");
        CHECK(metrics.aot_env_frame_version_drift_prevented.load() >= drift0 + 1,
              "env drift metric +1");
        aura_set_aot_metrics(nullptr);

        // Defaults (0,0) still produce pre-#1640 suffix shape
        CHECK(mangle_aot_name("x", 0, 3) == "x_0_v3", "3-arg defaults no _e_l");
    }

    // ── Issue #2015: dlopen probe env/linear via stale_ex ──
    {
        const char* so_path = "/tmp/aura_aot_fn_env_linear_2015.so";
        {
            std::ofstream f(std::string(so_path) + ".c");
            f << "#include <stdint.h>\n";
            f << "const unsigned long long aot_emit_version = 10ULL;\n";
            f << "const unsigned long long aot_top_fn_version = 10ULL;\n";
            f << "const unsigned long long aot_env_frame_version = 5ULL;\n";
            f << "const unsigned long long aot_linear_state = 1ULL;\n";
            f << "const unsigned aot_fn_versions_len = 0;\n";
        }
        std::string cmd =
            std::string("cc -shared -fPIC -o ") + so_path + " " + so_path + ".c 2>/dev/null";
        if (std::system(cmd.c_str()) == 0) {
            void* h = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
            CHECK(h != nullptr, "dlopen env/linear so");
            if (h) {
                CompilerMetrics metrics;
                aura_set_aot_metrics(&metrics);
                const auto drift0 = metrics.aot_env_frame_version_drift_prevented.load();
                // defuse matches, env matches
                CHECK(!aura_aot_fn_version_is_stale_ex(h, "__top__", 10, 5, 1), "ex match");
                // defuse matches, env drifted
                CHECK(aura_aot_fn_version_is_stale_ex(h, "__top__", 10, 9, 1), "ex env stale");
                CHECK(metrics.aot_env_frame_version_drift_prevented.load() >= drift0 + 1,
                      "dlopen env drift metric");
                // linear drift
                CHECK(aura_aot_fn_version_is_stale_ex(h, "__top__", 10, 5, 2), "ex linear stale");
                // legacy defuse-only still works
                CHECK(!aura_aot_fn_version_is_stale(h, "__top__", 10), "legacy not stale");
                aura_set_aot_metrics(nullptr);
                ::dlclose(h);
            }
            std::remove(so_path);
            std::remove((std::string(so_path) + ".c").c_str());
        } else {
            std::println("  (skip dlopen env/linear probe: cc failed)");
        }
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aot mangle top #1369/#2015: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
