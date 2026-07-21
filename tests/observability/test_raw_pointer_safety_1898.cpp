// @category: integration
// @reason: Issue #1898 — systemic raw non-owning pointer TOCTOU safety:
// pin/generation for compiler_service_ / type_registry_ / workspace_flat_,
// WorkspaceFlatPin RAII (shared_lock), revalidation soft-fail metrics.
//
//   AC1: pin API + generation stamps in evaluator.ixx
//   AC2: with_compiler_service_pin / WorkspaceFlatPin used in compile stats
//   AC3: query:raw-pointer-safety-stats schema 1898
//   AC4: pin_workspace_flat holds pointer under shared_lock (runtime)
//   AC5: pin_compiler_service revalidate happy path
//   AC6: set_compiler_service bumps generation

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:raw-pointer-safety-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

} // namespace

int main() {
    // ── AC1: source API ──
    {
        std::println("\n--- AC1: pin API + generation stamps ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator.ixx");
        CHECK(src.find("#1898") != std::string::npos, "cites #1898");
        CHECK(src.find("struct RawPointerPin") != std::string::npos, "RawPointerPin");
        CHECK(src.find("pin_compiler_service") != std::string::npos, "pin_compiler_service");
        CHECK(src.find("compiler_service_pin_valid") != std::string::npos, "pin_valid service");
        CHECK(src.find("pin_type_registry") != std::string::npos, "pin_type_registry");
        CHECK(src.find("class WorkspaceFlatPin") != std::string::npos, "WorkspaceFlatPin");
        CHECK(src.find("pin_workspace_flat") != std::string::npos, "pin_workspace_flat");
        CHECK(src.find("compiler_service_gen_") != std::string::npos, "service gen field");
        CHECK(src.find("workspace_flat_gen_") != std::string::npos, "workspace gen field");
        CHECK(src.find("type_registry_gen_") != std::string::npos, "registry gen field");
    }

    // ── AC2: compile sites use pin helpers ──
    {
        std::println("\n--- AC2: with_compiler_service_pin + WorkspaceFlatPin ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile.cpp");
        CHECK(src.find("with_compiler_service_pin") != std::string::npos, "service pin helper");
        CHECK(src.find("pin_workspace_flat") != std::string::npos, "workspace pin used");
        CHECK(src.find("query:raw-pointer-safety-stats") != std::string::npos, "stats query");
        CHECK(src.find("raw_pointer_uaf_prevented_total") != std::string::npos, "uaf metric");
        // High-risk sites migrated.
        auto ir = src.find("\"compile:ir-stats\"");
        CHECK(ir != std::string::npos, "ir-stats present");
        CHECK(src.substr(ir, 800).find("with_compiler_service_pin") != std::string::npos,
              "ir-stats uses service pin");
        auto rel = src.find("\"compile:relower-strategy\"");
        CHECK(rel != std::string::npos, "relower present");
        CHECK(src.substr(rel, 1200).find("with_compiler_service_pin") != std::string::npos,
              "relower uses service pin");
    }

    // ── AC3: stats surface ──
    {
        std::println("\n--- AC3: query:raw-pointer-safety-stats ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:raw-pointer-safety-stats\")");
        CHECK(r.has_value() && is_hash(*r), "stats is hash");
        CHECK(href(cs, "schema") == 1898, "schema 1898");
        CHECK(href(cs, "issue") == 1898, "issue 1898");
        CHECK(href(cs, "pin-api-wired") == 1, "pin-api-wired");
        CHECK(href(cs, "workspace-flat-pin-raii") == 1, "workspace pin raii");
        CHECK(href(cs, "service-pin-wired") == 1, "service pin wired");
        CHECK(href(cs, "type-registry-pin-wired") == 1, "registry pin wired");
        CHECK(href(cs, "raw-pointer-uaf-prevented") >= 0, "uaf key");
        CHECK(href(cs, "compiler_service_pin_reject_total") >= 0, "service reject key");
        CHECK(href(cs, "workspace-flat-generation") >= 0, "ws gen key");
    }

    // ── AC4: WorkspaceFlatPin runtime ──
    {
        std::println("\n--- AC4: pin_workspace_flat under shared_lock ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        const auto pins_before = m ? load_u64(m->workspace_flat_pin_total) : 0;
        {
            auto pin = ev.pin_workspace_flat();
            CHECK(static_cast<bool>(pin), "pin non-null after set-code");
            CHECK(pin.get() == ev.workspace_flat(), "pin matches workspace_flat()");
            CHECK(pin->size() >= 1, "flat has nodes");
        }
        // Stats path bumps pin counter.
        (void)cs.eval("(engine:metrics \"compile:invalidations-stats\")");
        (void)cs.eval("(engine:metrics \"compile:ast-ops-stats\")");
        if (m) {
            CHECK(load_u64(m->workspace_flat_pin_total) >= pins_before + 2,
                  "stats bumped workspace_flat_pin_total");
        }
        CHECK(href(cs, "workspace-flat-pin-total") >= 2, "query reflects pins");
    }

    // ── AC5: service pin happy path ──
    {
        std::println("\n--- AC5: pin_compiler_service revalidate ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto pin = ev.pin_compiler_service();
        CHECK(static_cast<bool>(pin), "service pin non-null");
        CHECK(ev.compiler_service_pin_valid(pin), "pin still valid");
        CHECK(pin.ptr == ev.compiler_service(), "ptr matches");
        (void)cs.eval("(set-code \"(define y 2)\")");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(engine:metrics \"compile:ir-stats\")");
        CHECK(r.has_value(), "ir-stats eval ok");
        // void or hash depending on whether IR was compiled.
        if (r)
            CHECK(is_void(*r) || is_hash(*r), "ir-stats shape");
        auto cache = cs.eval("(engine:metrics \"query:compiler-cache-stats\")");
        CHECK(cache.has_value() && (is_pair(*cache) || is_int(*cache)), "cache-stats ok");
    }

    // ── AC6: generation bumps on rebind ──
    {
        std::println("\n--- AC6: set_compiler_service bumps generation ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto g0 = ev.compiler_service_generation();
        auto pin0 = ev.pin_compiler_service();
        CHECK(ev.compiler_service_pin_valid(pin0), "valid before rebind");
        // Rebind to same pointer still bumps gen (detect any set_*).
        void* svc = ev.compiler_service();
        ev.set_compiler_service(svc);
        const auto g1 = ev.compiler_service_generation();
        CHECK(g1 > g0, "generation increased on set_compiler_service");
        CHECK(!ev.compiler_service_pin_valid(pin0), "old pin invalid after rebind");
        auto pin1 = ev.pin_compiler_service();
        CHECK(ev.compiler_service_pin_valid(pin1), "new pin valid");
        // Restore (no-op same pointer) for service health.
        ev.set_compiler_service(svc);
        CHECK(href(cs, "schema") == 1898, "schema holds");
        CHECK(href(cs, "compiler-service-generation") >= static_cast<std::int64_t>(g1),
              "query shows generation");
    }

    std::println("\n=== test_raw_pointer_safety_1898: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
