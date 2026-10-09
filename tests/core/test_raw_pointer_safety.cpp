// @category: integration
// @reason: Issue #1898 — systemic raw non-owning pointer TOCTOU safety:
// Issue #1898 (#1978 renamed): issue# moved from filename to header.
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
#include "compiler/security_defaults.hh"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.type;

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

// Issue #4404: TypeRegistry::compact must fail closed on a stale
// generation, and the primitive must refuse while a MutationBoundary
// is live without bumping the pin generation.
static std::string read_rel(const char* rel) {
    for (const char* prefix : {"", "../", "../../"}) {
        auto s = read_file((std::string(prefix) + rel).c_str());
        if (!s.empty())
            return s;
    }
    return {};
}

static void ac4404_type_registry_compact_fence() {
    std::println("\n--- #4404: type-registry compact generation fence ---");

    const auto mem = read_rel("src/compiler/evaluator_primitives_memory.cpp");
    const auto timpl = read_rel("src/core/type_impl.cpp");
    const auto tixx = read_rel("src/core/type.ixx");
    const auto eixx = read_rel("src/compiler/evaluator.ixx");
    CHECK(!mem.empty() && !timpl.empty() && !tixx.empty() && !eixx.empty(), "4404 sources");
    if (mem.empty() || timpl.empty() || tixx.empty() || eixx.empty())
        return;

    const auto add = mem.find("add(\"type-registry-compact\"");
    CHECK(add != std::string::npos, "4404 compact primitive");
    if (add == std::string::npos)
        return;
    const auto win = mem.substr(add, 700);
    const auto null_at = win.find("if (!ev.type_registry_)");
    const auto gate_at = null_at == std::string::npos
                             ? std::string::npos
                             : win.find("with_arena_compact_idle", null_at);
    const auto note_at = gate_at == std::string::npos
                             ? std::string::npos
                             : win.find("note_type_registry_content_compacted", gate_at);
    CHECK(null_at != std::string::npos && gate_at != std::string::npos &&
              note_at != std::string::npos,
          "4404 idle gate wraps compact");
    CHECK(null_at < gate_at && gate_at < note_at, "4404 null check then gate then bump");
    const auto helper = mem.find("auto with_arena_compact_idle");
    CHECK(helper != std::string::npos && helper < add, "4404 helper is above the primitive");
    if (helper != std::string::npos && helper < add) {
        CHECK(mem.substr(helper, add - helper).find("note_type_registry_content_compacted") ==
                  std::string::npos,
              "4404 idle return does not bump");
    }

    auto window_has = [&](const char* fn, const char* needle) {
        const auto at = timpl.find(fn);
        return at != std::string::npos && timpl.substr(at, 480).find(needle) != std::string::npos;
    };
    CHECK(window_has("TypeRegistry::tag_of", "id_current"), "4404 tag_of generation");
    CHECK(window_has("TypeRegistry::name_of", "id_current"), "4404 name_of generation");
    CHECK(window_has("TypeRegistry::func_of", "id_current"), "4404 func_of generation");
    CHECK(window_has("TypeRegistry::lookup_type", "current_generation()"),
          "4404 lookup_type generation");
    CHECK(window_has("TypeRegistry::tag_of", "std::shared_lock lock(type_registry_mutex_)"),
          "4404 shared reader");
    CHECK(window_has("TypeRegistry::compact()", "std::lock_guard lock(type_registry_mutex_)"),
          "4404 exclusive compact");
    CHECK(timpl.find("next_generation_.fetch_add(1, std::memory_order_acq_rel)") !=
              std::string::npos,
          "4404 registry generation publish");
    CHECK(tixx.find("class TypeRegistryLock") != std::string::npos, "4404 one mutex adapter");
    CHECK(tixx.find("std::shared_mutex mu_") != std::string::npos, "4404 shared_mutex");
    const auto note = eixx.find("note_type_registry_content_compacted");
    CHECK(note != std::string::npos, "4404 pin bump method");
    if (note != std::string::npos) {
        const auto note_win = eixx.substr(note, 500);
        CHECK(note_win.find("type_registry_gen_.fetch_add(1, std::memory_order_release)") !=
                  std::string::npos,
              "4404 bumps type_registry_gen_");
        CHECK(note_win.find("invalidate_persistent_typechecker()") != std::string::npos,
              "4404 invalidates persistent checker");
    }

    {
        aura::core::TypeRegistry reg;
        const auto g0 = reg.generation();
        CHECK(g0 >= 1, "4404 generation starts at least 1");
        auto user = reg.register_type(aura::core::TypeTag::RECORD, "User4404");
        auto fn = reg.register_func({reg.int_type()}, reg.bool_type());
        CHECK(reg.name_of(user) == "User4404", "4404 name before compact");
        CHECK(reg.lookup_type("User4404") == user, "4404 lookup before compact");
        CHECK(reg.func_of(fn) != nullptr, "4404 func before compact");
        CHECK(reg.tag_of(fn) == aura::core::TypeTag::FUNC, "4404 func tag before compact");
        const auto reclaimed = reg.compact();
        CHECK(reclaimed >= 1, "4404 compact reclaims the user type");
        CHECK(reg.generation() == g0 + 1, "4404 compact bumps generation");
        CHECK(user.valid(), "4404 TypeId::valid ignores registry generation");
        CHECK(reg.tag_of(user) == aura::core::TypeTag::DYNAMIC, "4404 stale tag");
        CHECK(reg.name_of(user) == "<invalid>", "4404 stale name");
        CHECK(reg.func_of(fn) == nullptr, "4404 stale func");
        CHECK(reg.tag_of(fn) == aura::core::TypeTag::DYNAMIC, "4404 stale func tag");
        CHECK(!reg.lookup_type("User4404").valid(), "4404 stale lookup");
        const auto fresh = reg.int_type();
        CHECK(fresh.generation == reg.generation(), "4404 fresh int_type generation");
        CHECK(reg.tag_of(fresh) == aura::core::TypeTag::INT, "4404 fresh int tag");
        CHECK(reg.lookup_type("Int") == reg.int_type(), "4404 Int lookup is int_type");
        const auto g1 = reg.generation();
        CHECK(reg.compact() == 0, "4404 second compact reclaims 0");
        CHECK(reg.generation() == g1 + 1, "4404 zero-reclaim compact still bumps");
        CHECK(reg.tag_of(reg.int_type()) == aura::core::TypeTag::INT, "4404 int survives");
    }

    if (const char* e = std::getenv("AURA_SANDBOX")) {
        if (std::string_view(e) == "off")
            ::unsetenv("AURA_SANDBOX");
    }
    aura::compiler::security::apply_production_security_defaults();
    const auto mode = aura::core::sandbox::g_sandbox_mode_atomic().load(std::memory_order_acquire);
    CHECK(mode != static_cast<std::uint8_t>(aura::core::sandbox::SandboxMode::Off),
          "4404 sandbox is not off");
    CHECK(aura::core::lifetime::g_general_object_pin_required_pref.load(
              std::memory_order_relaxed) == 1,
          "4404 general-object pin required");
    CHECK(aura::compiler::typed_audit::production_defaults_active(), "4404 production defaults");

    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* treg = static_cast<aura::core::TypeRegistry*>(ev.ensure_type_registry());
        CHECK(treg != nullptr, "4404 registry");
        if (!treg)
            return;
        auto user = treg->register_type(aura::core::TypeTag::RECORD, "User4404");
        const auto reg_gen0 = treg->generation();
        const auto pin_gen0 = ev.type_registry_generation();
        auto pin = ev.pin_type_registry();
        CHECK(ev.type_registry_pin_valid(pin), "4404 pin valid before compact");
        CHECK(ev.ensure_typechecker() != nullptr, "4404 persistent checker");
        const auto creates0 = ev.persistent_typechecker_create_total();
        auto* m = metrics_of(cs);
        const auto inv0 = m ? load_u64(m->typecheck_persistent_invalidate_total) : 0;

        auto r = cs.eval("(type-registry-compact)");
        CHECK(r && is_int(*r), "4404 compact eval is int");
        if (!(r && is_int(*r)))
            return;
        CHECK(as_int(*r) >= 1, "4404 eval reclaimed user type");
        CHECK(ev.type_registry_generation() == pin_gen0 + 1, "4404 pin generation increased");
        CHECK(!ev.type_registry_pin_valid(pin), "4404 old pin invalid");
        CHECK(treg->generation() == reg_gen0 + 1, "4404 registry generation increased");
        CHECK(treg->tag_of(user) == aura::core::TypeTag::DYNAMIC, "4404 eval stale tag");
        CHECK(treg->name_of(user) == "<invalid>", "4404 eval stale name");
        CHECK(!treg->lookup_type("User4404").valid(), "4404 eval stale lookup");
        CHECK(treg->int_type().generation == treg->generation(), "4404 eval fresh int generation");
        CHECK(treg->tag_of(treg->int_type()) == aura::core::TypeTag::INT, "4404 eval fresh int");
        CHECK(treg->lookup_type("Int") == treg->int_type(), "4404 eval Int lookup");
        if (m) {
            CHECK(load_u64(m->typecheck_persistent_invalidate_total) > inv0,
                  "4404 persistent checker invalidated");
        }
        (void)ev.ensure_typechecker();
        CHECK(ev.persistent_typechecker_create_total() > creates0, "4404 checker recreated");

        const auto reg_gen1 = treg->generation();
        const auto pin_gen1 = ev.type_registry_generation();
        auto r2 = cs.eval("(type-registry-compact)");
        CHECK(r2 && is_int(*r2), "4404 second compact is int");
        CHECK(treg->generation() > reg_gen1, "4404 second compact bumps registry generation");
        CHECK(ev.type_registry_generation() > pin_gen1, "4404 second compact bumps pin generation");
    }

    {
        // cs.eval under an outermost Guard inverts Mutate→Workspace lock
        // order and never reaches the primitive (#3269). Drive the slot so
        // the idle gate is what returns 0.
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* treg = static_cast<aura::core::TypeRegistry*>(ev.ensure_type_registry());
        CHECK(treg != nullptr, "4404 idle registry");
        if (!treg)
            return;
        const auto reg_gen = treg->generation();
        const auto pin_gen = ev.type_registry_generation();
        bool ok = true;
        {
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
            CHECK(gr.has_value() && ok, "4404 guard");
            CHECK(ev.any_active_mutation_boundary(), "4404 boundary live");
            if (gr.has_value()) {
                auto& prims = ev.primitives();
                const auto slot = prims.slot_for_name("type-registry-compact");
                auto fnopt = prims.slot_lookup_fast(slot);
                CHECK(fnopt.has_value(), "4404 compact slot");
                if (fnopt) {
                    auto paused = (*fnopt)(std::span<const aura::compiler::types::EvalValue>{});
                    CHECK(is_int(paused) && as_int(paused) == 0, "4404 idle compact returns 0");
                }
            }
        }
        CHECK(treg->generation() == reg_gen, "4404 idle keeps registry generation");
        CHECK(ev.type_registry_generation() == pin_gen, "4404 idle keeps pin generation");
    }
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

    ac4404_type_registry_compact_fence();

    std::println("\n=== test_raw_pointer_safety_1898: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
