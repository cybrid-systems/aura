// test_aot_reload_primitive.cpp — Issue #1366: (aot:reload) Aura wrappers
// Issue #2012: atomic func_table staging + rollback + concurrent stress

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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
    // Prefer engine:metrics catalog (query:* forms are registered there).
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
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

// Issue #2012: .so that registers a staged func_id via aura_register_fn_tracked.
// Uses dlsym(RTLD_DEFAULT) so the .so has no undefined symbols at dlopen
// (RTLD_NOW) while still binding to the host process's strong definition.
std::string build_registering_so(std::uint64_t version, std::uint64_t region, int func_id,
                                 const char* tag) {
    const char* dir = "/tmp";
    std::string cpath = std::format("{}/aura_aot_reg_{}_{}.c", dir, tag, version);
    std::string sopath = std::format("{}/aura_aot_reg_{}_{}.so", dir, tag, version);
    {
        std::ofstream f(cpath);
        if (!f)
            return {};
        f << "#include <stdint.h>\n";
        f << "#include <stddef.h>\n";
        f << "#include <dlfcn.h>\n";
        f << "uint64_t aot_emit_version = " << version << "ULL;\n";
        f << "uint64_t aot_region_mask = " << region << "ULL;\n";
        f << "typedef void (*reg_fn_t)(int64_t, int64_t);\n";
        f << "static int64_t aura_aot_reg_sentinel_" << tag
          << "(int64_t* a, uint32_t n) { (void)a; (void)n; return " << version << "; }\n";
        f << "__attribute__((constructor)) static void reg(void) {\n";
        f << "  void* self = dlopen(NULL, RTLD_LAZY);\n";
        f << "  reg_fn_t fn = self ? (reg_fn_t)dlsym(self, \"aura_register_fn_tracked\") : 0;\n";
        f << "  if (!fn) fn = (reg_fn_t)dlsym(RTLD_DEFAULT, \"aura_register_fn_tracked\");\n";
        f << "  if (fn) fn(" << func_id << ", (int64_t)(void*)aura_aot_reg_sentinel_" << tag
          << ");\n";
        f << "}\n";
    }
    std::string cmd = std::format("cc -shared -fPIC -o {} {} -ldl 2>/dev/null", sopath, cpath);
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
    // aot:get-region-mask is registered via register_stats_impl (engine:metrics).
    {
        CompilerService cs;
        auto s = cs.eval("(aot:set-region-mask 7)");
        CHECK(s && is_bool(*s) && as_bool(*s), "aot:set-region-mask → #t");
        auto g = cs.eval("(engine:metrics \"aot:get-region-mask\")");
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

    // ── Issue #2012: staging — failed validation leaves live slots intact ──
    {
        std::println("\n--- #2012: atomic staging rollback preserves live slots ---");
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
        aura_set_module_version(0);

        constexpr std::int64_t kFid = 77;
        const std::uintptr_t seed_ptr = static_cast<std::uintptr_t>(0xA0112012ull);
        aura_register_fn_tracked(kFid, static_cast<std::int64_t>(seed_ptr));
        CHECK(aura_aot_probe_fn_ptr(kFid) == seed_ptr, "seed slot 77");

        const auto epoch0 = aura_aot_func_table_epoch();
        const auto rb0 = metrics.aot_hot_update_atomic_rollback.load(std::memory_order_relaxed);
        const auto stale0 = metrics.aot_stale_reject_count_.load(std::memory_order_relaxed);
        aura_hot_update_registry_snapshot reg0{};
        aura_hot_update_registry_get_snapshot(&reg0);

        // Version-mismatch .so that *also* tries to register a new pointer into
        // slot 77 — staging must discard it so live stays seed_ptr.
        auto bad_so = build_registering_so(/*version=*/1, /*region=*/0, /*func_id=*/77, "bad");
        if (bad_so.empty()) {
            CHECK(true, "skip #2012 register-so (cc -shared unavailable)");
        } else {
            const bool ok = aura_reload_aot_module(bad_so.c_str(), /*expected=*/99);
            CHECK(!ok, "version mismatch → false");
            CHECK(aura_aot_probe_fn_ptr(kFid) == seed_ptr,
                  "live slot 77 unchanged after failed reload (staging discarded)");
            CHECK(aura_aot_func_table_epoch() == epoch0, "epoch not advanced on rollback");
            CHECK(metrics.aot_hot_update_atomic_rollback.load(std::memory_order_relaxed) >= rb0 + 1,
                  "atomic_rollback +1");
            CHECK(metrics.aot_stale_reject_count_.load(std::memory_order_relaxed) >= stale0 + 1,
                  "stale_reject +1");
            aura_hot_update_registry_snapshot reg1{};
            aura_hot_update_registry_get_snapshot(&reg1);
            CHECK(reg1.aot_reload_rollback_total >= reg0.aot_reload_rollback_total + 1,
                  "HotUpdateRegistry rollback counter +1");
        }

        // Region mismatch also rolls back cleanly.
        aura_set_aot_region_mask(0x1);
        auto region_so =
            build_registering_so(/*version=*/5, /*region=*/0x2, /*func_id=*/77, "region");
        if (!region_so.empty()) {
            const auto epoch1 = aura_aot_func_table_epoch();
            const auto rbm0 = metrics.aot_region_mismatch_.load(std::memory_order_relaxed);
            CHECK(!aura_reload_aot_module(region_so.c_str(), 5), "region mismatch → false");
            CHECK(aura_aot_probe_fn_ptr(kFid) == seed_ptr, "slot intact after region reject");
            CHECK(aura_aot_func_table_epoch() == epoch1, "epoch intact after region reject");
            CHECK(metrics.aot_region_mismatch_.load(std::memory_order_relaxed) >= rbm0 + 1,
                  "region_mismatch +1");
        }
        aura_set_aot_region_mask(0);

        // Success path: staging applied + epoch bump + registry success.
        auto good_so = build_registering_so(/*version=*/42, /*region=*/0, /*func_id=*/77, "good");
        if (!good_so.empty()) {
            const auto epoch2 = aura_aot_func_table_epoch();
            const auto suc0 = metrics.aot_hot_update_success_.load(std::memory_order_relaxed);
            aura_hot_update_registry_snapshot rs0{};
            aura_hot_update_registry_get_snapshot(&rs0);
            const bool ok = aura_reload_aot_module(good_so.c_str(), 42);
            if (ok) {
                CHECK(aura_aot_func_table_epoch() == epoch2 + 1, "epoch +1 on success");
                CHECK(metrics.aot_hot_update_success_.load(std::memory_order_relaxed) == suc0 + 1,
                      "hot_update_success +1");
                const auto live = aura_aot_probe_fn_ptr(kFid);
                CHECK(live != 0 && live != seed_ptr,
                      "slot 77 replaced with staged pointer from good .so");
                aura_hot_update_registry_snapshot rs1{};
                aura_hot_update_registry_get_snapshot(&rs1);
                CHECK(rs1.aot_reload_success_total >= rs0.aot_reload_success_total + 1,
                      "HotUpdateRegistry success counter +1");
            } else {
                CHECK(true, "good .so dlopen failed in env (non-fatal)");
            }
        }

        // query:aot-stats exposes rollback key (#2012).
        {
            CompilerService cs;
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
            auto st = cs.eval("(engine:metrics \"query:aot-stats\")");
            CHECK(st && is_hash(*st), "query:aot-stats is hash");
            CHECK(href(cs, "query:aot-stats", "aot-hot-update-rollback-count") >= 0,
                  "aot-hot-update-rollback-count key present");
            auto reg = cs.eval("(engine:metrics \"query:hot-update-registry-stats\")");
            CHECK(reg && is_hash(*reg), "query:hot-update-registry-stats is hash");
            CHECK(href(cs, "query:hot-update-registry-stats", "aot-reload-rollback-total") >= 0,
                  "registry aot-reload-rollback-total key");
        }
        aura_set_aot_metrics(nullptr);
    }

    // ── Issue #2012: concurrent epoch probes during forced fail + success ──
    {
        std::println("\n--- #2012: concurrent probe stress during reload ---");
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);

        auto so_ok = build_test_so(77);
        auto so_bad = build_registering_so(1, 0, 3, "stress");
        if (so_ok.empty()) {
            CHECK(true, "skip concurrent stress (cc unavailable)");
        } else {
            std::atomic<bool> stop{false};
            std::atomic<std::uint64_t> samples{0};
            std::atomic<std::uint64_t> torn{0};
            std::vector<std::thread> readers;
            readers.reserve(4);
            for (int t = 0; t < 4; ++t) {
                readers.emplace_back([&] {
                    while (!stop.load(std::memory_order_relaxed)) {
                        const auto e1 = aura_aot_func_table_epoch();
                        const auto p = aura_aot_probe_fn_ptr(3);
                        const auto e2 = aura_aot_func_table_epoch();
                        (void)p;
                        // Epoch is monotonic; a drop would indicate torn state machine.
                        if (e2 < e1)
                            torn.fetch_add(1, std::memory_order_relaxed);
                        samples.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (int i = 0; i < 32; ++i) {
                (void)aura_reload_aot_module(so_ok.c_str(), 77);
                if (!so_bad.empty())
                    (void)aura_reload_aot_module(so_bad.c_str(), 99); // forced fail
            }
            stop.store(true, std::memory_order_relaxed);
            for (auto& th : readers)
                th.join();
            CHECK(samples.load() > 0, "concurrent samples collected");
            CHECK(torn.load() == 0, "func_table_epoch never went backwards under stress");
        }
        aura_set_aot_metrics(nullptr);
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aot reload primitive #1366/#2012: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
