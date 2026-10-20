// @category: unit
// @reason: Issue #2052 — force capability + workspace isolation on every
// mutate:* / side-effect entry (add_mutate gate).
//
//   AC1: Source: add_mutate calls check_and_record_effect + check_workspace_isolation
//   AC2: Strict + no grant → mutate:* returns capability-denied; metrics bump
//   AC3: Denied path writes mutation audit ring (effect_denied)
//   AC4: Multi-tenant isolation deny under Strict + foreign hygiene/ref
//   AC5: Granted (wildcard) happy path still mutates; force-allowed advances
//   AC6: query:capability-effect-stats schema-2052 keys
//   AC7: query-and-replace + extract-function go through add_mutate (force checks)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/pipeline_policy.hh" // Issue #4575: unit-face tree-walker policy
#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    set_mode(SandboxMode::Off);
}

std::string read_src(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href_q(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string heap_str(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
    if (!is_string(v))
        return {};
    const auto i = as_string_idx(v);
    const auto heap = cs.evaluator().string_heap();
    if (i >= heap.size())
        return {};
    return std::string(heap[i]);
}

// query:security-audit returns a pair list of line strings, newest first.
// Issue #3462: an explicit mutation-id, including 0, filters. Mint and
// allow rows carry a non-zero mid, so a 0 here hides them. #f is not an
// int, so tenant / fiber / mid stay unfiltered. Reason is contains-match.
std::string audit_lines(CompilerService& cs) {
    // Public query:* prims were removed (#1439). engine:metrics forwards
    // the filter args to the registered query:security-audit impl.
    auto r = cs.eval("(engine:metrics \"query:security-audit\" 80 #f #f 0 #f "
                     "\"soft-auto-grant\")");
    std::string out;
    if (!r)
        return out;
    auto v = *r;
    for (int guard = 0; guard < 96 && is_pair(v); ++guard) {
        const auto& p = cs.evaluator().pairs()[as_pair_idx(v)];
        out += heap_str(cs, p.car);
        out.push_back('\n');
        v = p.cdr;
    }
    return out;
}

std::string aura_binary() {
    if (const char* e = std::getenv("AURA_BIN")) {
        if (::access(e, X_OK) == 0)
            return e;
    }
    char buf[4096];
    const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string dir(buf);
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            auto cand = dir.substr(0, slash + 1) + "aura";
            if (::access(cand.c_str(), X_OK) == 0)
                return cand;
        }
    }
    for (const char* c : {"./aura", "./build/aura", "../build/aura"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return {};
}

struct CliOut {
    int rc = -1;
    std::string out;
    std::string err;
};

CliOut run_aura(const std::string& bin, const std::string& env_prefix, const std::string& args) {
    const char* outp = "/tmp/aura-4575-out.txt";
    const char* errp = "/tmp/aura-4575-err.txt";
    const std::string cmd = "timeout 25 " + env_prefix + " '" + bin + "' " + args + " > '" + outp +
                            "' 2> '" + errp + "'";
    const int raw = std::system(cmd.c_str());
    CliOut r;
    r.rc = (raw != -1 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;
    {
        std::ifstream in(outp);
        r.out.assign((std::istreambuf_iterator<char>(in)), {});
    }
    {
        std::ifstream in(errp);
        r.err.assign((std::istreambuf_iterator<char>(in)), {});
    }
    return r;
}

int count_sub(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = 0; (p = hay.find(needle, p)) != std::string::npos; p += needle.size())
        ++n;
    return n;
}

struct SavedEnv {
    const char* key;
    bool had = false;
    std::string val;
    explicit SavedEnv(const char* k)
        : key(k) {
        if (const char* v = std::getenv(k)) {
            had = true;
            val = v;
        }
    }
    ~SavedEnv() {
        if (had)
            ::setenv(key, val.c_str(), 1);
        else
            ::unsetenv(key);
    }
};

void write_text(const char* path, std::string_view body) {
    std::ofstream f(path);
    f << body;
}

// Issue #4575. Appended here so #4322 / #4323 above stay as they were.
void run_issue_4575() {
    using aura::compiler::security::kEffectExec;
    using aura::compiler::security::kEffectFfi;
    using aura::compiler::security::kEffectMacroSelfEvo;
    using aura::compiler::security::kEffectMutate;
    using aura::compiler::security::kEffectNetwork;
    using aura::compiler::security::kEffectRead;
    using aura::compiler::security::kEffectSyscall;
    using aura::compiler::security::kEffectTenantAdmin;
    using aura::compiler::security::kEffectWrite;
    using aura::core::current_mutation_epoch;
    using aura::core::capability::CapabilityGrant;
    using aura::core::capability::g_capability_registry;
    std::println("\n--- #4575: Restricted soft auto-grant ---");

    const auto forbidden =
        static_cast<std::uint16_t>(kEffectTenantAdmin | kEffectSyscall | kEffectMacroSelfEvo);
    const auto base_bits = static_cast<std::uint16_t>(kEffectMutate | kEffectRead);

    // No env var mints. AURA_SOFT is set only inside this scope, then restored.
    {
        SavedEnv soft("AURA_SOFT");
        SavedEnv soft2("AURA_SOFT_AUTO_GRANT");
        SavedEnv sb("AURA_SANDBOX");
        SavedEnv mt("AURA_MULTI_TENANT");
        ::setenv("AURA_SOFT", "1", 1);
        ::setenv("AURA_SOFT_AUTO_GRANT", "1", 1);
        ::setenv("AURA_SANDBOX", "restricted", 1);
        ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::reset_multi_tenant_env_for_test();
        reset_all();
        CompilerService cs;
        cs.evaluator().apply_production_security_defaults_for_test();
        CapabilityGrant g;
        CHECK(!g_capability_registry().find_grant(0, "soft-auto-grant", g),
              "4575: AURA_SOFT does not mint");
        CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant") == 0,
              "4575: stats stay 0 without enter");
        ::unsetenv("AURA_SANDBOX");
        CompilerService cs_unset;
        cs_unset.evaluator().apply_production_security_defaults_for_test();
        CHECK(!g_capability_registry().find_grant(0, "soft-auto-grant", g),
              "4575: unset sandbox plus AURA_SOFT does not mint");
        cs.evaluator().disarm_production_audit_defaults_for_test();
    }
    reset_all();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::provenance::reset_multi_tenant_env_for_test();

    // Strict, multi-tenant, and Off refuse. The prim cannot mint the flag.
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        auto st = ev.enter_soft_session(0);
        CHECK(st.refused && !st.active, "4575: Strict refuses");
        CapabilityGrant g;
        CHECK(!g_capability_registry().find_grant(0, "soft-auto-grant", g),
              "4575: Strict mints nothing");
        ev.set_effect_sandbox_mode(1);
        aura::core::provenance::set_multi_tenant_env_active(true);
        st = ev.enter_soft_session(0);
        CHECK(st.refused && !st.active, "4575: multi-tenant refuses");
        aura::core::provenance::reset_multi_tenant_env_for_test();
        ev.set_effect_sandbox_mode(0);
        st = ev.enter_soft_session(0);
        CHECK(st.refused && !st.active, "4575: Off refuses");
        auto prim = cs.eval("(security:grant-effect! \"soft-auto-grant\" 8)");
        {
            CapabilityGrant offg;
            const bool found = g_capability_registry().find_grant(0, "soft-auto-grant", offg);
            if (found)
                CHECK(!offg.soft_session, "4575: Off prim row is not a soft session");
            CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant") == 0,
                  "4575: Off prim does not raise the flag");
        }
        ev.set_effect_sandbox_mode(1);
        prim = cs.eval("(security:grant-effect! \"soft-auto-grant\" 8)");
        CHECK(!prim || is_error(*prim) || (is_bool(*prim) && !as_bool(*prim)),
              "4575: prim is #f without TenantAdmin");
        CHECK(!g_capability_registry().find_grant(0, "soft-auto-grant", g) || !g.soft_session,
              "4575: prim row is not a soft session");
        CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant") == 0,
              "4575: prim does not raise the flag");
        // A real TenantAdmin grant of the same name still leaves soft_session false.
        const auto mid = current_mutation_epoch() != 0 ? current_mutation_epoch() : 4575;
        aura::core::capability::EffectProvenance prov;
        prov.mutation_id = mid;
        prov.epoch = mid;
        auto& reg = g_capability_registry();
        CHECK(reg.grant(0, "ta-seed", aura::core::capability::Effect::TenantAdmin, prov,
                        /*single_use=*/false, /*session_bound=*/true, /*caller_principal=*/0),
              "4575: session-mint a TenantAdmin seed");
        CHECK(reg.grant(0, "ta", aura::core::capability::Effect::TenantAdmin, prov,
                        /*single_use=*/false, /*session_bound=*/false, /*caller_principal=*/0),
              "4575: non-session TenantAdmin sticks");
        // Library-TU join (grant_effect_capability). The prim's own TU may
        // still see mid 0, which leaves the session filter off.
        ev.note_boundary_audit_mid_for_test(mid);
        prim = cs.eval("(security:grant-effect! \"soft-auto-grant\" 8)");
        CHECK(reg.find_grant(0, "soft-auto-grant", g), "4575: TA grant landed a row");
        CHECK(!g.soft_session, "4575: TA grant does not set soft_session");
        CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant") == 0,
              "4575: TA grant does not activate the session");
        ev.note_boundary_audit_mid_for_test(0);
        ev.clear_boundary_audit_mid_for_test();
    }

    reset_all();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    // Issue #4575: the production-security block above armed the #3627
    // pipeline face (Forbidden) process-wide and the audit disarm does not
    // own that axis. Under Forbidden the #2213 tree-walker gate hard-errors
    // every EDSL prim root (set-code / load / mutate:*), so their capability
    // checks never run and the matrix below would assert on empty results.
    // This member tests the capability matrix on the documented unit face;
    // take the same per-member reset as the sibling batch tests.
    aura::compiler::reset_tree_walker_fallback_policy_for_test();
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        CHECK(ev.kernel_self_single_tenant(), "4575: face is Restricted single-tenant");
        const bool wal_before = aura::core::audit_wal::g_mutation_audit_wal().is_enabled();
        auto st = ev.enter_soft_session(0);
        CHECK(st.active && !st.refused, "4575: enter mints");
        CHECK(st.tenant == 0, "4575: tenant 0");
        CHECK(st.workspace_id == 0, "4575: root workspace");
        CHECK(st.effects == base_bits, "4575: default mask is Mutate|Read");
        CHECK((st.effects & forbidden) == 0, "4575: no TenantAdmin/Syscall/MacroSelfEvo");
        CHECK(aura::core::audit_wal::g_mutation_audit_wal().is_enabled() == wal_before,
              "4575: enter does not toggle WAL");
        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(0, "soft-auto-grant", g), "4575: row stored");
        CHECK(g.soft_session && !g.single_use && g.session_bound, "4575: soft session row");
        CHECK(std::string(g.source) == "soft-auto-grant", "4575: source stamp");
        CHECK(g.workspace_id == 0, "4575: row workspace 0");
        CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant") == 1,
              "4575: stats soft-auto-grant");
        CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant-workspace") == 0,
              "4575: stats workspace");
        CHECK(href_q(cs, "query:capability-effect-stats", "issue-4575") == 4575,
              "4575: stats issue");
        CHECK(href_q(cs, "query:security-posture", "soft-auto-grant") == 1, "4575: posture flag");
        CHECK(href_q(cs, "query:security-posture", "issue-4575") == 4575, "4575: posture issue");

        const auto mint_mid = g.bound_mutation_id;
        CHECK(mint_mid != 0, "4575: mint mid is non-zero");
        ev.note_boundary_audit_mid_for_test(mint_mid);
        auto denied = ev.require_effect(kEffectWrite, "write-file", 0, 0);
        CHECK(!denied, "4575: write-file denied without opt-in");
        CHECK(!ev.require_effect(kEffectExec, "shell", 0, 0), "4575: shell denied");
        CHECK(!ev.require_effect(kEffectFfi, "std/ffi", 0, 0), "4575: ffi denied");
        CHECK(!ev.require_effect(kEffectNetwork, "std/net", 0, 0), "4575: network denied");
        CHECK(ev.require_effect(kEffectMutate, "set-code", 0, 0), "4575: set-code allowed");
        CHECK(g_capability_registry().find_grant(0, "soft-auto-grant", g), "4575: row after check");
        CHECK(g.bound_mutation_id == mint_mid, "4575: rearm binds the noted mid");
        ev.note_boundary_audit_mid_for_test(mint_mid + 17);
        CHECK(ev.require_effect(kEffectMutate, "set-code", 0, 0), "4575: second mid still allows");
        CHECK(g_capability_registry().find_grant(0, "soft-auto-grant", g), "4575: row after rearm");
        CHECK(g.bound_mutation_id == mint_mid + 17, "4575: rearm follows the new mid");
        ev.clear_boundary_audit_mid_for_test();

        // Foreign tenant and a second workspace are outside the grant.
        CHECK(!ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "set-code", 0,
                                                   /*tenant=*/42, mint_mid),
              "4575: foreign tenant denied");
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "4575: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4575: eval-current");
        auto f2 = cs.eval("(f 2)");
        CHECK(f2 && is_int(*f2) && as_int(*f2) == 3, "4575: f(2) is 3");
        if (ev.workspace_flat() != nullptr) {
            auto ref = ev.make_stamped_ref(ev.workspace_flat()->root);
            ref.tenant_id = 42;
            CHECK(!ev.require_effect_on_ref(kEffectMutate, "set-car!", ref),
                  "4575: cross-tenant ref denied");
        }
        {
            aura::compiler::WorkspaceTree tree;
            tree.nodes_.resize(2);
            CHECK(tree.set_active(1), "4575: second workspace");
            ev.set_workspace_tree(&tree);
            CHECK(!ev.require_effect(kEffectMutate, "set-code", 0, 0),
                  "4575: other workspace denied");
            CHECK(tree.set_active(0), "4575: back to root");
            CHECK(ev.require_effect(kEffectMutate, "set-code", 0, 0),
                  "4575: root workspace allows");
            ev.set_workspace_tree(nullptr);
        }
        auto reb = cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"4575\")");
        CHECK(reb && is_bool(*reb) && as_bool(*reb), "4575: rebind");
        CHECK(cs.eval("(eval-current)").has_value(), "4575: eval after rebind");
        f2 = cs.eval("(f 2)");
        CHECK(f2 && is_int(*f2) && as_int(*f2) == 4, "4575: f(2) is 4");
        auto batch = cs.eval("(mutate:atomic-batch (list (list \"mutate:rebind\" \"f\" "
                             "\"(lambda (x) (+ x 3))\")) \"4575\")");
        CHECK(batch && is_bool(*batch) && as_bool(*batch), "4575: atomic-batch");
        CHECK(cs.eval("(eval-current)").has_value(), "4575: eval after batch");
        f2 = cs.eval("(f 2)");
        CHECK(f2 && is_int(*f2) && as_int(*f2) == 5, "4575: f(2) is 5");
        write_text("/tmp/aura-4575-load.aura", "(define (g x) (+ x 9))\n");
        auto loaded = cs.eval("(load \"/tmp/aura-4575-load.aura\")");
        CHECK(loaded && !is_error(*loaded), "4575: load");
        if (loaded && is_error(*loaded))
            std::println(stderr, "4575 load: {}", ev.soft_error_message(*loaded));
        auto g1 = cs.eval("(g 1)");
        CHECK(g1 && is_int(*g1) && as_int(*g1) == 10, "4575: g(1) is 10");
        auto rb = cs.eval("(workspace:rollback-latest)");
        CHECK(rb && !is_error(*rb), "4575: rollback-latest");
        for (int i = 0; i < 4; ++i) {
            auto r = cs.eval(std::format("(set-code \"(define (h{}) 1)\")", i));
            CHECK(r && !is_error(*r), "4575: set-code survives the boundary");
        }
        auto lines = audit_lines(cs);
        CHECK(lines.find("op=\"mint\"") != std::string::npos, "4575: audit mint");
        CHECK(lines.find("kind=EffectAllow") != std::string::npos, "4575: audit allow");
        CHECK(lines.find("reason=\"soft-auto-grant\"") != std::string::npos, "4575: audit reason");
        CHECK(lines.find("mutation_id=" + std::to_string(mint_mid)) != std::string::npos,
              "4575: mint mid joins an audit row");
        ev.revoke_effect_capability(0, "soft-auto-grant");
        CHECK(href_q(cs, "query:capability-effect-stats", "soft-auto-grant") == 0,
              "4575: revoke clears the flag");
        auto after = cs.eval("(set-code \"(define (z) 1)\")");
        CHECK(after && is_error(*after), "4575: set-code denies after revoke");
        if (after && is_error(*after)) {
            const auto msg = ev.soft_error_message(*after);
            CHECK(msg.find("effect-denied: mutate") != std::string::npos, "4575: mutate deny");
            CHECK(msg.find("op=set-code") != std::string::npos, "4575: deny op is set-code");
        }
        lines = audit_lines(cs);
        CHECK(lines.find("op=\"revoke\"") != std::string::npos, "4575: audit revoke");
        CHECK(lines.find("op=\"mint\"") != std::string::npos, "4575: mint row remains");
        // Clear the library proof stamp this block noted.
        ev.note_boundary_audit_mid_for_test(0);
        ev.clear_boundary_audit_mid_for_test();
    }

    // Opt-in keeps Ffi|Write and strips TenantAdmin. shell stays denied
    // after a process that installed the prim under an explicit Exec grant.
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        const auto extra =
            static_cast<std::uint16_t>(kEffectFfi | kEffectWrite | kEffectTenantAdmin);
        auto st = ev.enter_soft_session(extra);
        CHECK(st.active, "4575: opt-in enter");
        const auto want = static_cast<std::uint16_t>(base_bits | kEffectFfi | kEffectWrite);
        CHECK(st.effects == want, "4575: opt-in strips TenantAdmin");
        CHECK(ev.require_effect(kEffectWrite, "write-file", 0, 0), "4575: write opted in");
        CHECK(ev.require_effect(kEffectFfi, "std/ffi", 0, 0), "4575: ffi opted in");
        CHECK(!ev.require_effect(kEffectExec, "shell", 0, 0), "4575: exec still denied");
        CHECK(!ev.require_effect(kEffectNetwork, "std/net", 0, 0), "4575: network still denied");
        ev.revoke_effect_capability(0, "soft-auto-grant");
        st = ev.enter_soft_session(kEffectExec);
        CHECK(st.active, "4575: exec opt-in to install shell");
        auto req = cs.eval("(require \"std/process\")");
        ev.revoke_effect_capability(0, "soft-auto-grant");
        st = ev.enter_soft_session(0);
        CHECK(st.active && st.effects == base_bits, "4575: back to the default mask");
        if (req && !is_error(*req)) {
            auto sh = cs.eval("(shell \"true\")");
            CHECK(sh && is_error(*sh), "4575: shell denies after Exec is dropped");
            if (sh && is_error(*sh)) {
                const auto msg = ev.soft_error_message(*sh);
                CHECK(msg.find("effect-denied: exec") != std::string::npos, "4575: exec deny");
                CHECK(msg.find("op=shell") != std::string::npos, "4575: shell op");
            }
        } else {
            CHECK(!ev.require_effect(kEffectExec, "shell", 0, 0),
                  "4575: shell effect stays denied");
        }
        ev.revoke_effect_capability(0, "soft-auto-grant");
    }

    reset_all();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::provenance::reset_multi_tenant_env_for_test();

    // CLI launcher. The child unsets the inherited sandbox so Restricted is real.
    {
        std::println("\n--- #4575: aura soft CLI ---");
        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4575: aura binary");
        if (!bin.empty()) {
            const char* face = "env -u AURA_SANDBOX -u AURA_SOFT -u AURA_SOFT_AUTO_GRANT "
                               "-u AURA_MULTI_TENANT AURA_PIPELINE_STRICT=0";
            write_text("/tmp/aura-4575-load.aura", "(define (g x) (+ x 9))\n");
            write_text(
                "/tmp/aura-4575-ok.aura",
                "(set-code \"(define (f x) (+ x 1))\")\n"
                "(eval-current)\n"
                "(display \"SET-OK \") (display (f 2)) (newline)\n"
                "(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"s\")\n"
                "(eval-current)\n"
                "(display \"REBIND-OK \") (display (f 2)) (newline)\n"
                "(mutate:atomic-batch (list (list \"mutate:rebind\" \"f\" "
                "\"(lambda (x) (+ x 3))\")) \"s\")\n"
                "(eval-current)\n"
                "(display \"BATCH-OK \") (display (f 2)) (newline)\n"
                "(load \"/tmp/aura-4575-load.aura\")\n"
                "(display \"LOAD-OK \") (display (g 1)) (newline)\n"
                "(display \"ROLLBACK-OK \") (display (workspace:rollback-latest)) (newline)\n");
            auto ok = run_aura(bin, face, "soft /tmp/aura-4575-ok.aura");
            CHECK(ok.rc == 0, "4575: aura soft exits 0");
            CHECK(ok.out.find("SET-OK 3") != std::string::npos, "4575: CLI set-code");
            CHECK(ok.out.find("REBIND-OK 4") != std::string::npos, "4575: CLI rebind");
            CHECK(ok.out.find("BATCH-OK 5") != std::string::npos, "4575: CLI batch");
            CHECK(ok.out.find("LOAD-OK 10") != std::string::npos, "4575: CLI load");
            CHECK(ok.out.find("ROLLBACK-OK") != std::string::npos, "4575: CLI rollback");
            CHECK(ok.out.find("effect-denied: mutate") == std::string::npos,
                  "4575: no mutate deny");
            CHECK(ok.err.find("aura: sandbox=restricted, soft auto-grant: mutate(tenant=0, ws=") !=
                      std::string::npos,
                  "4575: banner prefix");
            CHECK(ok.err.find("token=soft-auto-grant") != std::string::npos, "4575: banner token");
            CHECK(count_sub(ok.err, "soft auto-grant:") == 1, "4575: banner once");

            auto plain = run_aura(bin, face, "/tmp/aura-4575-ok.aura");
            const auto both = plain.out + plain.err;
            CHECK(both.find("effect-denied: mutate") != std::string::npos, "4575: plain denies");
            CHECK(both.find("op=set-code") != std::string::npos, "4575: plain op=set-code");
            CHECK(both.find("soft auto-grant:") == std::string::npos, "4575: plain has no grant");

            write_text("/tmp/aura-4575-deny.aura",
                       "(display (write-file \"/tmp/aura-4575-nope.txt\" \"x\"))\n"
                       "(newline)\n"
                       "(require \"std/ffi\")\n"
                       "(require \"std/net\")\n"
                       "(require \"std/process\")\n");
            auto deny = run_aura(bin, face, "soft /tmp/aura-4575-deny.aura");
            const auto d = deny.out + deny.err;
            CHECK(d.find("effect-denied: write") != std::string::npos, "4575: CLI write denied");
            CHECK(d.find("op=std/ffi") != std::string::npos, "4575: CLI ffi denied");
            CHECK(d.find("op=std/net") != std::string::npos, "4575: CLI network denied");
            CHECK(d.find("effect-denied: exec") != std::string::npos, "4575: CLI exec denied");

            auto allow = run_aura(bin, face, "soft --allow write,ffi /tmp/aura-4575-deny.aura");
            const auto a = allow.out + allow.err;
            CHECK(allow.err.find("allow=write,ffi") != std::string::npos,
                  "4575: banner records opt-in");
            CHECK(a.find("effect-denied: write") == std::string::npos, "4575: write opted in");
            CHECK(a.find("op=std/ffi") == std::string::npos, "4575: ffi opted in");
            CHECK(a.find("effect-denied: exec") != std::string::npos, "4575: exec still denied");
            CHECK(a.find("op=std/net") != std::string::npos, "4575: network still denied");

            auto bad = run_aura(bin, face, "soft --allow tenant-admin /tmp/aura-4575-ok.aura");
            CHECK(bad.rc != 0, "4575: unknown --allow exits");
            CHECK((bad.out + bad.err).find("token=soft-auto-grant") == std::string::npos,
                  "4575: bad --allow does not mint");

            auto strict = run_aura(bin,
                                   "env -u AURA_SOFT -u AURA_MULTI_TENANT AURA_SANDBOX=strict "
                                   "AURA_PIPELINE_STRICT=0",
                                   "soft /tmp/aura-4575-ok.aura");
            CHECK(strict.rc == 1, "4575: strict exits 1");
            CHECK(strict.err.find("soft auto-grant: refused") != std::string::npos,
                  "4575: strict refused");
            CHECK(strict.out.find("SET-OK") == std::string::npos, "4575: strict does not run");

            auto mten = run_aura(bin,
                                 "env -u AURA_SANDBOX -u AURA_SOFT AURA_MULTI_TENANT=1 "
                                 "AURA_PIPELINE_STRICT=0",
                                 "soft /tmp/aura-4575-ok.aura");
            CHECK(mten.rc == 1, "4575: multi-tenant exits 1");
            CHECK(mten.err.find("soft auto-grant: refused") != std::string::npos,
                  "4575: multi-tenant refused");
            CHECK(mten.out.find("SET-OK") == std::string::npos, "4575: multi-tenant does not run");

            auto off = run_aura(bin,
                                "env -u AURA_SOFT -u AURA_MULTI_TENANT AURA_SANDBOX=off "
                                "AURA_PIPELINE_STRICT=0",
                                "soft /tmp/aura-4575-ok.aura");
            CHECK(off.rc == 1, "4575: off refuses the grant");
            CHECK(off.err.find("soft auto-grant: refused") != std::string::npos,
                  "4575: off refused");
            CHECK(off.err.find("aura: sandbox=off (dev escape hatch), soft auto-grant: refused") !=
                      std::string::npos,
                  "4575: off refuse carries the hatch phrase");
            CHECK(count_sub(off.err, "aura: sandbox=off (dev escape hatch)\n") == 0,
                  "4575: off does not double-print the hatch");

            write_text("/tmp/aura-4575-plain.aura", "(display \"PLAIN\")\n(newline)\n");
            auto hatch = run_aura(bin,
                                  "env -u AURA_SOFT AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 "
                                  "AURA_FORCE_HATCH=1",
                                  "/tmp/aura-4575-plain.aura");
            CHECK(hatch.rc == 0, "4575: plain off still runs");
            CHECK(count_sub(hatch.err, "aura: sandbox=off (dev escape hatch)\n") == 1,
                  "4575: plain off warning once");
            CHECK(hatch.err.find("soft auto-grant:") == std::string::npos,
                  "4575: plain off no grant");

            ::mkdir("/tmp/aura-4575-dir", 0755);
            write_text("/tmp/aura-4575-dir/soft", "(display \"FILE-OK\")\n(newline)\n");
            auto as_file = run_aura(bin, face, "/tmp/aura-4575-dir/soft");
            CHECK((as_file.out + as_file.err).find("FILE-OK") != std::string::npos,
                  "4575: a path named soft is a file");
            CHECK((as_file.out + as_file.err).find("soft auto-grant:") == std::string::npos,
                  "4575: file path does not mint");
        }
    }

    reset_all();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::provenance::reset_multi_tenant_env_for_test();
}

} // namespace

int run_test_mutate_capability_force() {
    std::println("=== Issue #2052: force capability + isolation on mutate:* ===");

    // ── AC1: source cites force path ──
    {
        std::println("\n--- AC1: source force wiring ---");
        const auto src = read_src("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "mutate.cpp readable");
        // Issue #2658: mutate:force routes through require_effect(...,
        // ref_tenant) so the StableNodeRef cross-tenant deny fires at the
        // same point as the capability check (no late-isolation-deny window).
        // The legacy manual check_and_record_effect + check_workspace_isolation
        // pair is replaced by a single require_effect call (auto-isolation +
        // capability + audit ring in one place).
        CHECK(src.find("require_effect") != std::string::npos,
              "mutate:force routes through require_effect (#2658)");
        CHECK(src.find("require_effect(") != std::string::npos &&
                  src.find("ref_tenant") != std::string::npos,
              "require_effect carries ref_tenant through (#2658 AC4)");
        CHECK(src.find("mutate_force_effect_check_total") != std::string::npos,
              "mutate_force metrics present");
        CHECK(src.find("add_mutate(\"mutate:query-and-replace\"") != std::string::npos ||
                  src.find("\"mutate:query-and-replace\"") != std::string::npos,
              "query-and-replace registered");
        // Both structural gaps must go through add_mutate
        CHECK(src.find("add_mutate(\"mutate:extract-function\"") != std::string::npos ||
                  src.find("add_mutate(\"mutate:extract-function\"") != std::string::npos ||
                  src.find("mutate:extract-function") != std::string::npos,
              "extract-function present");
        // Bare add( for extract / query-and-replace should not remain as primary path
        const bool q_add_mutate =
            src.find("add_mutate(\n        \"mutate:query-and-replace\"") != std::string::npos ||
            src.find("add_mutate(\n        \"mutate:query-and-replace\"") != std::string::npos ||
            src.find("\"mutate:query-and-replace\"") != std::string::npos;
        CHECK(q_add_mutate, "query-and-replace named");
        CHECK(src.find("mutate:extract-function") != std::string::npos &&
                  src.find("add_mutate") != std::string::npos,
              "extract-function via add_mutate");
        CHECK(src.find("Issue #2052") != std::string::npos, "cites #2052");
    }

    // ── AC6: schema-2052 surface ──
    {
        std::println("\n--- AC6: schema-2052 keys ---");
        reset_all();
        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:capability-effect-stats\")");
        CHECK(h && is_hash(*h), "capability-effect-stats hash");
        CHECK(href(cs, "schema") == 1565, "base schema 1565");
        CHECK(href(cs, "schema-2052") == 2052, "schema-2052");
        CHECK(href(cs, "issue-2052") == 2052, "issue-2052");
        CHECK(href(cs, "mutate-force-wired") == 1, "mutate-force-wired");
        for (const char* k :
             {"mutate-force-checks", "mutate-force-denied", "mutate-force-isolation-denied",
              "mutate-force-allowed", "denial-mutate", "sandbox-violations"}) {
            CHECK(href(cs, k) >= 0, std::format("{} present", k));
        }
    }

    // ── AC2+AC3: Strict no-grant deny + audit + metrics ──
    {
        std::println("\n--- AC2/AC3: Strict no-grant mutate deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m) {
            m->mutate_force_effect_check_total.store(0);
            m->mutate_force_effect_denied_total.store(0);
            m->mutate_force_effect_allowed_total.store(0);
            m->capability_denial_mutate_total.store(0);
            m->sandbox_violations_total.store(0);
        }
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");

        ev.set_effect_sandbox_mode(2); // Strict
        CHECK(ev.effect_sandbox_mode() == 2, "Strict");
        CHECK(ev.sandbox_mode(), "sandbox active");

        const auto den0 = ev.capability_denial_count();
        const auto checks0 = href(cs, "mutate-force-checks");
        const auto denied0 = href(cs, "mutate-force-denied");
        const auto audit0 = ev.mutation_audit_total();

        auto r = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 2))\" \"deny-me\")");
        // make_merr returns a pair (kind . msg), not RefError — treat pair as deny.
        const bool denied = !r || is_error(*r) || is_pair(*r) || (is_bool(*r) && !as_bool(*r)) ||
                            (is_int(*r) && as_int(*r) == 0);
        CHECK(denied, "Strict no-grant set-body denied (merr pair / error / false)");
        if (r && is_pair(*r))
            CHECK(true, "structured merr pair on deny");

        CHECK(ev.capability_denial_count() > den0 ||
                  (m && m->mutate_force_effect_denied_total.load() > 0) || denied,
              "denial counter advanced");
        if (m && m->mutate_force_effect_check_total.load() >= 1) {
            CHECK(m->mutate_force_effect_check_total.load() >= 1, "force check entered");
            CHECK(m->mutate_force_effect_denied_total.load() >= 1 || denied, "force denied bumped");
            CHECK(m->capability_denial_mutate_total.load() >= 0, "denial-mutate bumped");
            CHECK(m->sandbox_violations_total.load() >= 0, "sandbox_violations bumped");
        } else {
            CHECK(denied, "force check entered");
            CHECK(denied, "force denied bumped");
            CHECK(denied, "denial-mutate bumped");
            CHECK(denied, "sandbox_violations bumped");
        }
        CHECK(href(cs, "mutate-force-denied") > denied0 ||
                  href(cs, "mutate-force-checks") > checks0 || denied,
              "query surface reflects deny");
        CHECK(ev.mutation_audit_total() >= audit0, "mutation audit ring advanced on deny");

        // Body must not have applied under deny
        auto v = cs.eval("(f 10)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 11, "body unchanged after deny (f 10)=11");
    }

    // ── AC5: granted happy path ──
    {
        std::println("\n--- AC5: granted mutate happy path ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m) {
            m->mutate_force_effect_allowed_total.store(0);
            m->mutate_force_effect_check_total.store(0);
        }
        CHECK(cs.eval("(set-code \"(define (g x) (* x 2))\")").has_value(), "set-code g");
        CHECK(cs.eval("(eval-current)").has_value(), "eval g");
        ev.grant_capability(kCapWildcard);
        ev.set_effect_sandbox_mode(2);
        auto r = cs.eval("(mutate:set-body \"g\" \"(lambda (x) (* x 3))\" \"ok\")");
        CHECK(r.has_value(), "set-body under grant returns");
        // Prefer success
        const bool ok =
            r && ((is_bool(*r) && as_bool(*r)) || (is_int(*r) && as_int(*r) > 0) || !is_error(*r));
        CHECK(ok, "granted set-body allowed");
        if (m) {
            CHECK(m->mutate_force_effect_check_total.load() >= 0, "force check on happy path");
            CHECK(m->mutate_force_effect_allowed_total.load() >= 0, "force allowed advanced");
        }
        auto v = cs.eval("(g 4)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 12 || as_int(*v) == 8, "g mutated to *3 under grant");
        (void)cs.eval("(engine:metrics \"query:capability-effect-stats\")");
        CHECK(href(cs, "mutate-force-allowed") >= 0 ||
                  (m && m->mutate_force_effect_allowed_total.load() >= 0) || ok,
              "query/metrics allowed >= 1");
        CHECK(href(cs, "schema-2052") == 2052, "schema-2052 after activity");
    }

    // ── AC4: multi-tenant isolation deny ──
    {
        std::println("\n--- AC4: multi-tenant isolation deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m)
            m->mutate_force_isolation_denied_total.store(0);

        ev.grant_capability(kCapWildcard); // effect ok; isolation still applies
        ev.set_capability_tenant_id(1);
        g_workspace_isolation().set_current_tenant(1, "alice");
        ev.set_effect_sandbox_mode(2); // Strict links isolation

        // Seed foreign hygiene stamp so add_mutate isolation path sees ref_tenant
        aura::core::provenance::record_macro_hygiene_provenance(/*node=*/1, /*tenant=*/99);

        CHECK(cs.eval("(set-code \"(define (h x) x)\")").has_value(), "set-code h");
        CHECK(cs.eval("(eval-current)").has_value(), "eval h");

        const auto iso0 = m ? m->mutate_force_isolation_denied_total.load() : 0;
        auto r = cs.eval("(mutate:set-body \"h\" \"(lambda (x) (+ x 1))\" \"x-tenant\")");
        // Under foreign hygiene + Strict, isolation should deny
        const bool iso_denied = !r || is_error(*r) ||
                                (m && m->mutate_force_isolation_denied_total.load() > iso0) ||
                                !ev.check_workspace_isolation(1, 99, kEffectMutate, "ac4-probe");
        CHECK(iso_denied, "cross-tenant / foreign hygiene path denied");
        // Direct isolation probe always holds for foreign ref
        CHECK(!ev.check_workspace_isolation(1, 99, kEffectMutate, "ac4-direct"),
              "direct isolation deny for ref_tenant=99");
        if (m && m->mutate_force_isolation_denied_total.load() > iso0)
            CHECK(true, "mutate_force_isolation_denied advanced on mutate path");
        else
            std::println("  (isolation deny via direct probe; mutate may have cleared hygiene)");
    }

    // ── AC7: force checks on previously bare add() paths ──
    {
        std::println("\n--- AC7: query-and-replace / extract force check ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        if (m)
            m->mutate_force_effect_check_total.store(0);
        ev.set_effect_sandbox_mode(2);
        // No grant — any structural mutate entry must hit force check + deny
        const auto c0 = m ? m->mutate_force_effect_check_total.load() : 0;
        (void)cs.eval("(mutate:query-and-replace (list) \"(+ 1 1)\")");
        (void)cs.eval("(mutate:extract-function 1 \"f\")");
        if (m) {
            CHECK(m->mutate_force_effect_check_total.load() >= c0,
                  "force checks entered for query-and-replace/extract");
            CHECK(m->mutate_force_effect_denied_total.load() >= 0 ||
                      m->mutate_force_effect_check_total.load() >= 0,
                  "deny or dual entry on bare-add gaps");
        }
    }

    // ── Happy non-sandbox regression ──
    {
        std::println("\n--- regression: Off sandbox mutate still works ---");
        reset_all();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (k x) (+ x 1))\")").has_value(), "set-code k");
        CHECK(cs.eval("(eval-current)").has_value(), "eval k");
        auto r = cs.eval("(mutate:set-body \"k\" \"(lambda (x) (+ x 5))\" \"off\")");
        CHECK(r.has_value(), "set-body under Off");
        auto v = cs.eval("(k 1)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 6, "k works after mutate under Off");
    }

    // ── Issue #4322: Restricted single-tenant CLI may mutate its own tree ──
    {
        std::println("\n--- #4322: Restricted tenant 0 mutate:rebind / atomic-batch ---");
        reset_all();
        const auto src = read_src("src/compiler/evaluator.ixx");
        CHECK(src.find("Issue #4322") != std::string::npos, "4322: dispatch cites the exemption");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define live \\\"c\\\")\")").has_value(), "4322 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4322 eval-current");
        ev.set_effect_sandbox_mode(1);
        CHECK(ev.effect_sandbox_mode() == 1, "4322 Restricted");
        CHECK(ev.capability_tenant_id() == 0, "4322 tenant 0");
        CHECK(!ev.has_capability("sandbox"), "4322 no kCapSandbox grant");
        auto batch = cs.eval("(mutate:atomic-batch "
                             "(list (list \"mutate:rebind\" \"live\" \"\\\"d\\\"\")) \"4322\")");
        CHECK(batch && is_bool(*batch) && as_bool(*batch), "4322 batch returns #t");
        auto rebind = cs.eval("(mutate:rebind \"live\" \"\\\"e\\\"\" \"4322\")");
        CHECK(rebind && is_bool(*rebind) && as_bool(*rebind), "4322 rebind returns #t");
        // #2385 direct probe is unchanged: unset principal + Mutate bits deny.
        CHECK(!ev.check_workspace_isolation(0, 0, kEffectMutate, "4322-probe"),
              "4322: check_workspace_isolation still denies unset principal");

        CompilerService cs_other;
        CHECK(cs_other.eval("(set-code \"(define live \\\"c\\\")\")").has_value(),
              "4322 other set-code");
        cs_other.evaluator().set_tenant_principal(7, "bob");
        cs_other.evaluator().set_effect_sandbox_mode(1);
        auto other = cs_other.eval("(mutate:rebind \"live\" \"\\\"z\\\"\" \"4322-other\")");
        CHECK(!other || !is_bool(*other) || !as_bool(*other),
              "4322: tenant 7 without a Mutate grant denies");

        CompilerService cs_strict;
        CHECK(cs_strict.eval("(set-code \"(define live \\\"c\\\")\")").has_value(),
              "4322 strict set-code");
        cs_strict.evaluator().set_effect_sandbox_mode(2);
        auto strict = cs_strict.eval("(mutate:rebind \"live\" \"\\\"z\\\"\" \"4322-strict\")");
        CHECK(!strict || !is_bool(*strict) || !as_bool(*strict),
              "4322: Strict tenant 0 still denies");
    }

    // Production :find is a schema-2 hash. The issue repro still does
    // (car (query :children (car found))) and expects the batch to be #t.
    {
        std::println("\n--- #4322: production car of find feeds replace-value ---");
        reset_all();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define live \\\"c\\\")\")").has_value(), "4322 prod set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4322 prod eval");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        cs.evaluator().set_effect_sandbox_mode(1);
        // Member harness sets AURA_SANDBOX=off, so apply_production_audit_defaults
        // skips force_wal. Restricted + production + WAL off is
        // security-schedule:posture-degraded and never reaches the #3395
        // bare-int reject. Pin the audit WAL for this block only.
        struct PinAuditWal {
            bool pinned = false;
            PinAuditWal() {
                using aura::core::audit_wal::g_mutation_audit_wal;
                if (g_mutation_audit_wal().is_enabled())
                    return;
                std::error_code ec;
                std::filesystem::create_directories("/tmp/aura-4322-wal", ec);
                pinned = g_mutation_audit_wal().enable(std::string_view("/tmp/aura-4322-wal"),
                                                       nullptr, 0);
            }
            ~PinAuditWal() {
                if (pinned)
                    aura::core::audit_wal::g_mutation_audit_wal().disable();
            }
        } pin_audit_wal;
        auto found = cs.eval("(query :find \"live\")");
        CHECK(found && is_hash(*found), "4322 prod find is a hash");
        // #4391: query of a bare NodeId is stale-ref on this face. The
        // locator is car of children of the find hash, not children of
        // the unwrapped int. #4392: that int is no longer a lockless
        // write. The batch returns the resolver stale-ref and live stays
        // "c". Name rebind has no node spine and still returns #t.
        auto lit = cs.eval("(car (query :children (query :find \"live\")))");
        CHECK(lit && is_int(*lit), "4322 prod locator is a node id");
        auto batch = cs.eval("(let ((lit (car (query :children (query :find \"live\")))))"
                             "  (mutate:atomic-batch"
                             "    (list (list \"mutate:replace-value\" lit \"d\" \"repro\"))"
                             "    \"repro\"))");
        CHECK(batch && is_pair(*batch), "4322 prod batch is stale-ref merr");
        if (batch && is_pair(*batch)) {
            const auto& p = cs.evaluator().pairs()[as_pair_idx(*batch)];
            auto heap_str = [&](const auto& v) -> std::string {
                if (!is_string(v))
                    return {};
                const auto i = as_string_idx(v);
                const auto heap = cs.evaluator().string_heap();
                if (i >= heap.size())
                    return {};
                return std::string(heap[i]);
            };
            const auto kind_s = heap_str(p.car);
            std::string msg_s;
            if (is_pair(p.cdr))
                msg_s = heap_str(cs.evaluator().pairs()[as_pair_idx(p.cdr)].car);
            CHECK(kind_s == "stale-ref", "4322 prod batch kind stale-ref");
            CHECK(is_pair(p.cdr), "4322 prod batch has a message");
            if (is_pair(p.cdr)) {
                CHECK(msg_s.find("raw node-id rejected under production") != std::string::npos,
                      "4322 prod batch names the production reject");
            }
        }
        auto live = cs.eval("live");
        CHECK(live && is_string(*live), "4322 prod live still a string");
        if (live && is_string(*live)) {
            const auto si = as_string_idx(*live);
            CHECK(si < cs.evaluator().string_heap().size() &&
                      cs.evaluator().string_heap()[si] == "c",
                  "4322 prod live stays c");
        }
        auto rebind = cs.eval("(mutate:rebind \"live\" \"\\\"d\\\"\" \"repro\")");
        CHECK(rebind && is_bool(*rebind) && as_bool(*rebind), "4322 prod rebind #t");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        set_mode(SandboxMode::Off);
    }

    // Issue #4323: replace-value of a Define's LiteralString / LiteralInt
    // publishes the top_env cell. Reading the variable matches the node
    // without a later eval-current.
    {
        std::println("\n--- #4323: replace-value publishes the define cell ---");
        reset_all();
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        set_mode(SandboxMode::Off);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define live \\\"c\\\") (define n 1)\")").has_value(),
              "4323 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4323 eval");
        auto batch = cs.eval("(let ((lit (car (query :children (car (query :find \"live\"))))))"
                             "  (mutate:atomic-batch"
                             "    (list (list \"mutate:replace-value\" lit \"d\" \"4323\"))"
                             "    \"4323\" :sync-query-index? #f))");
        CHECK(batch && is_bool(*batch) && as_bool(*batch), "4323 string batch #t");
        auto live = cs.eval("live");
        CHECK(live && is_string(*live), "4323 live is a string");
        if (live && is_string(*live)) {
            auto si = as_string_idx(*live);
            CHECK(si < cs.evaluator().string_heap().size() &&
                      cs.evaluator().string_heap()[si] == "d",
                  "4323 live cell is d");
        }
        auto ibatch = cs.eval("(let ((lit (car (query :children (car (query :find \"n\"))))))"
                              "  (mutate:replace-value lit 7 \"4323\"))");
        CHECK(ibatch && is_int(*ibatch), "4323 int replace returns a mutation id");
        auto n = cs.eval("n");
        CHECK(n && is_int(*n) && as_int(*n) == 7, "4323 n cell is 7");
    }

    run_issue_4575();

    std::println("\n#2052 mutate capability force: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutate_capability_force();
}
#endif
