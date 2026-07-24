// test_aot_mangle_top.cpp — Issue #1369:
// __top__ always includes _vN; per-function version probe.

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
using aura::compiler::aot_parse_version_suffix;
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

    if (::aura::test::g_failed)
        return 1;
    std::println("aot mangle top #1369: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
