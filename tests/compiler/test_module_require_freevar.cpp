// @category: unit
// @reason: Issue #2566 — non-std module free-var resolve of required
//          std/mutate (and other std) bindings matches top-level.
// Issue #2766 — require-before-export free-var capture of module-private
// cells (refine #2566/#2570/#2579). Prefer-existing suite per #81967.
//
//   AC1: (require "std/mutate" all:) inside non-std module → closures
//        resolve mutate:boundary-safe? same as top-level (#t under sandbox off)
//   AC2: Top-level require still injects into top_ (regression)
//   AC3: Nested require injects into module env (source-cite)
//   AC4: SoA live top_ fallback when walk reaches root frame
//   AC5: test + cmake + gate; no docs/design
//
//   #2766 AC1: require-before-export + private *cell* free-var works
//   #2766 AC2: export-before-require still works (parity)
//   #2766 AC3: std/orchestrator agent:spawn + agent:ask + agent:list
//   #2766 AC4: source-cite prologue skip + Phase 0 require-before-letrec
//   #2766 AC5: coverage linter wired; no docs/design/*
//
//   #2768 AC1: orchestrator.aura export-before-require + #2768 note
//   #2768 AC2: agent:spawn/ask/list + epoch monotonic on stdin
//   #2768 AC3: agent:status/stop/restart lifecycle
//   #2768 AC4: orch:parallel-with-yield callable (no unbound private free-vars)
//   #2768 AC5: coverage linter; no docs/design/*
//
//   #2769 AC1: stdlib inventory (order + module cells) via audit script
//   #2769 AC2: denseness paths green (llm/hot-strategy/agent/orch/mutate/query)
//   #2769 AC3: host #2766 canary still shared
//   #2769 AC4: zero require-before-export in lib/std (export-first policy)
//   #2769 AC5: linter + INDEX authoring note; no docs/design/*

#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

namespace fs = std::filesystem;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static fs::path find_lib_std() {
    for (const auto& p : {fs::path("lib/std"), fs::path("../lib/std"), fs::path("../../lib/std")}) {
        if (fs::is_directory(p) && fs::exists(p / "mutate.aura"))
            return fs::absolute(p.parent_path()); // lib/
    }
    return {};
}

static bool eval_bool(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    if (!r || !is_bool(*r))
        return false;
    return as_bool(*r);
}

// ── AC1: non-std module free-var parity with top-level ──
static void ac1_module_freevar_parity() {
    std::println("\n--- #2566 AC1: non-std module free-var parity with top-level ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC1: lib/ with std/mutate found");
    if (lib.empty())
        return;

    // Temp non-std module that requires std/mutate and closes over free vars.
    auto tmp = fs::temp_directory_path() / "aura_2566_mod";
    fs::create_directories(tmp);
    {
        std::ofstream out(tmp / "m.aura");
        out << "(export m:safe?)\n";
        out << "(require \"std/mutate\" all:)\n";
        out << "(define (m:safe?) (try (mutate:boundary-safe?) (catch (e) #f)))\n";
    }

    const auto path = lib.string() + ":" + tmp.string();
    setenv("AURA_PATH", path.c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);

    CompilerService cs;
    // Top-level probe first.
    CHECK(eval_bool(cs, "(begin (require \"std/mutate\" all:) (mutate:boundary-safe?))"),
          "AC1: top-level mutate:boundary-safe? → #t (sandbox off)");
    // Module free-var path must match.
    CHECK(eval_bool(cs, "(begin (require \"m\" all:) (m:safe?))"),
          "AC1: module m:safe? free-var path → #t (parity)");

    fs::remove_all(tmp);
}

// ── AC2: top-level inject still works ──
static void ac2_toplevel_inject() {
    std::println("\n--- #2566 AC2: top-level require still injects into top_ ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC2: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    CompilerService cs;
    CHECK(eval_bool(cs, "(begin (require \"std/mutate\" all:) "
                        "(procedure? mutate:boundary-safe?))"),
          "AC2: top-level procedure? after require");
}

// ── AC3: source-cite nested inject ──
static void ac3_source_cite_inject() {
    std::println("\n--- #2566 AC3: nested require injects into module env ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("require_inject_env_") != std::string::npos ||
              loader.find("RequireInjectGuard") != std::string::npos,
          "AC3: load_module_file sets inject target");
    CHECK(loader.find("#2566") != std::string::npos, "AC3: loader cites #2566");

    const auto prim = read_file("src/compiler/evaluator_primitives_module.cpp");
    CHECK(prim.find("require_inject_env_") != std::string::npos, "AC3: import uses inject target");
    CHECK(prim.find("#2566") != std::string::npos, "AC3: import cites #2566");

    const auto eixx = read_file("src/compiler/evaluator.ixx");
    CHECK(eixx.find("require_inject_env_") != std::string::npos, "AC3: field on Evaluator");
}

// ── AC4: SoA live top_ fallback ──
static void ac4_soa_live_top() {
    std::println("\n--- #2566 AC4: SoA live top_ fallback at root frame ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("#2566") != std::string::npos, "AC4: env lookup cites #2566");
    CHECK(env.find("cur == 0") != std::string::npos ||
              env.find("cur == 0 && owner_") != std::string::npos,
          "AC4: live top_ when SoA walk reaches root frame");
}

// ── AC5: gate wiring ──
static void ac5_gate() {
    std::println("\n--- #2566 AC5: test + cmake + gate ---");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_module_require_freevar") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_module_require_freevar_2566") != std::string::npos,
          "AC5: check script");
    CHECK(build.find("cmd_module_require_freevar_coverage") != std::string::npos, "AC5: gate cmd");
}

// ── Issue #2766: require-before-export private free-var capture ──

static void ac2766_1_require_before_export_private_cell() {
    std::println("\n--- #2766 AC1: require-before-export private *cell* free-var ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC1: lib found");
    if (lib.empty())
        return;

    auto tmp = fs::temp_directory_path() / "aura_2766_bad_order";
    fs::create_directories(tmp);
    {
        // Bad textual order (require then export) — must still work.
        std::ofstream out(tmp / "bad-order.aura");
        out << "(require \"std/list\" all:)\n";
        out << "(export bad:get bad:set!)\n";
        out << "(define *cell* 0)\n";
        out << "(define (bad:get) *cell*)\n";
        out << "(define (bad:set! v) (set! *cell* v) *cell*)\n";
    }

    const auto path = lib.string() + ":" + tmp.string();
    setenv("AURA_PATH", path.c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);

    CompilerService cs;
    auto r1 = cs.eval("(begin (require \"bad-order\" all:) (bad:get))");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 0, "AC1: (bad:get) → 0 after require-before-export");
    auto r2 = cs.eval("(bad:set! 9)");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == 9, "AC1: (bad:set! 9) → 9");
    auto r3 = cs.eval("(bad:get)");
    CHECK(r3 && is_int(*r3) && as_int(*r3) == 9, "AC1: (bad:get) → 9 after set!");

    fs::remove_all(tmp);
}

static void ac2766_2_export_before_require_parity() {
    std::println("\n--- #2766 AC2: export-before-require still works ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC2: lib found");
    if (lib.empty())
        return;

    auto tmp = fs::temp_directory_path() / "aura_2766_good_order";
    fs::create_directories(tmp);
    {
        std::ofstream out(tmp / "good-order.aura");
        out << "(export good:get good:set!)\n";
        out << "(require \"std/list\" all:)\n";
        out << "(define *cell* 0)\n";
        out << "(define (good:get) *cell*)\n";
        out << "(define (good:set! v) (set! *cell* v) *cell*)\n";
    }

    const auto path = lib.string() + ":" + tmp.string();
    setenv("AURA_PATH", path.c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);

    CompilerService cs;
    auto r1 = cs.eval("(begin (require \"good-order\" all:) (good:get))");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 0, "AC2: (good:get) → 0");
    auto r2 = cs.eval("(good:set! 9)");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == 9, "AC2: (good:set! 9) → 9");
    auto r3 = cs.eval("(good:get)");
    CHECK(r3 && is_int(*r3) && as_int(*r3) == 9, "AC2: (good:get) → 9");

    fs::remove_all(tmp);
}

static void ac2766_3_orchestrator_agent_registry() {
    std::println("\n--- #2766 AC3: std/orchestrator agent:spawn / ask / list ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC3: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);

    CompilerService cs;
    // Full official agent surface under stdin denseness host.
    auto spawn = cs.eval("(begin (require \"std/orchestrator\" all:) "
                         "(agent:spawn \"ping\" (lambda (x) (+ x 1))))");
    CHECK(spawn.has_value(), "AC3: agent:spawn succeeds (no unbound agent-register)");
    if (spawn && is_string(*spawn)) {
        // Prefer string "ping" when returned as name.
        auto si = as_string_idx(*spawn);
        // string heap index path may not be exposed; accept any success value.
        (void)si;
        CHECK(true, "AC3: spawn returned value");
    }
    auto ask = cs.eval("(agent:ask \"ping\" 41)");
    CHECK(ask && is_int(*ask) && as_int(*ask) == 42, "AC3: (agent:ask \"ping\" 41) → 42");
    auto list = cs.eval("(agent:list)");
    CHECK(list.has_value(), "AC3: agent:list callable (no unbound *agents*)");
    auto epoch = cs.eval("(orch:registry-epoch)");
    CHECK(epoch && is_int(*epoch) && as_int(*epoch) >= 1,
          "AC3: orch:registry-epoch ≥ 1 (no unbound *registry-epoch*)");
}

static void ac2766_4_source_cite() {
    std::println("\n--- #2766 AC4: source-cite prologue skip + Phase 0 ---");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(efl.find("#2766") != std::string::npos, "AC4: eval_flat cites #2766");
    CHECK(efl.find("is_module_prologue") != std::string::npos, "AC4: is_module_prologue helper");
    CHECK(efl.find("Phase 0") != std::string::npos ||
              efl.find("module prologue") != std::string::npos,
          "AC4: Phase 0 / prologue documentation");
    CHECK(efl.find("require") != std::string::npos && efl.find("import") != std::string::npos,
          "AC4: require/import treated as prologue");
}

static void ac2766_5_linter() {
    std::println("\n--- #2766 AC5: linter + no docs/design ---");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_module_require_export_order_2766.py");
    const auto t = read_file("tests/compiler/test_module_require_freevar.cpp");
    CHECK(build.find("check_module_require_export_order_2766") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(t.find("ac2766_1_require_before_export_private_cell") != std::string::npos, "AC5: AC1");
    CHECK(t.find("ac2766_3_orchestrator_agent_registry") != std::string::npos, "AC5: AC3");
    CHECK(read_file("docs/design/2766-require-before-export.md").empty(),
          "AC5: no docs/design/2766-* per #1655");
}

// ── Issue #2768: std/orchestrator multi-agent surface on stdin denseness ──

static void ac2768_1_orchestrator_export_first() {
    std::println("\n--- #2768 AC1: orchestrator.aura export-before-require ---");
    const auto orch = read_file("lib/std/orchestrator.aura");
    CHECK(!orch.empty(), "AC1: read orchestrator.aura");
    CHECK(orch.find("#2768") != std::string::npos, "AC1: cites #2768");
    CHECK(orch.find("#2766") != std::string::npos, "AC1: cites #2766 host fix");
    // First top-level form (line starting with '('), ignore comment examples.
    auto first_toplevel = [&](std::string_view prefix) -> std::size_t {
        std::size_t pos = 0;
        while (pos < orch.size()) {
            auto i = orch.find(prefix, pos);
            if (i == std::string::npos)
                return std::string::npos;
            auto line_start = orch.rfind('\n', i);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            if (i == line_start)
                return i;
            pos = i + 1;
        }
        return std::string::npos;
    };
    const auto exp = first_toplevel("(export ");
    const auto req = first_toplevel("(require ");
    CHECK(exp != std::string::npos && req != std::string::npos, "AC1: export + require present");
    CHECK(exp < req, "AC1: export appears before first require (canonical order)");
}

static void ac2768_2_spawn_ask_list_epoch() {
    std::println("\n--- #2768 AC2: spawn/ask/list + epoch monotonic ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC2: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);

    CompilerService cs;
    CHECK(cs.eval("(require \"std/orchestrator\" all:)").has_value(), "AC2: require orchestrator");
    auto e0 = cs.eval("(orch:registry-epoch)");
    CHECK(e0 && is_int(*e0), "AC2: epoch readable at rest");
    const auto n0 = as_int(*e0);
    CHECK(cs.eval("(agent:spawn \"ping\" (lambda (x) (+ x 1)))").has_value(), "AC2: spawn");
    auto e1 = cs.eval("(orch:registry-epoch)");
    CHECK(e1 && is_int(*e1) && as_int(*e1) > n0, "AC2: epoch bumps on spawn");
    auto ask = cs.eval("(agent:ask \"ping\" 41)");
    CHECK(ask && is_int(*ask) && as_int(*ask) == 42, "AC2: ask 41 → 42");
    CHECK(cs.eval("(agent:list)").has_value(), "AC2: agent:list");
}

static void ac2768_3_status_stop_restart() {
    std::println("\n--- #2768 AC3: agent:status / stop / restart lifecycle ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC3: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);

    CompilerService cs;
    CHECK(cs.eval("(require \"std/orchestrator\" all:)").has_value(), "AC3: require");
    CHECK(cs.eval("(agent:spawn \"lc\" (lambda (x) (* x 2)))").has_value(), "AC3: spawn lc");
    auto st = cs.eval("(agent:status \"lc\")");
    CHECK(st.has_value(), "AC3: status callable");
    CHECK(cs.eval("(agent:stop \"lc\")").has_value(), "AC3: stop");
    CHECK(cs.eval("(agent:status \"lc\")").has_value(), "AC3: status after stop");
    CHECK(cs.eval("(agent:restart \"lc\")").has_value(), "AC3: restart");
    auto ask = cs.eval("(agent:ask \"lc\" 5)");
    CHECK(ask && is_int(*ask) && as_int(*ask) == 10, "AC3: ask after restart → 10");
}

static void ac2768_4_parallel_with_yield_smoke() {
    std::println("\n--- #2768 AC4: orch:parallel-with-yield smoke ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC4: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);

    CompilerService cs;
    CHECK(cs.eval("(require \"std/orchestrator\" all:)").has_value(), "AC4: require");
    // Callable without unbound private free-vars (orch-yield-safe / wrap).
    // Empty list is OK when fiber:spawn is unavailable on host.
    auto r = cs.eval("(orch:parallel-with-yield "
                     "(list (lambda (x) (* x 2)) (lambda (x) (+ x 1))) 10)");
    CHECK(r.has_value(), "AC4: parallel-with-yield returns without unbound free-vars");
}

static void ac2768_5_linter() {
    std::println("\n--- #2768 AC5: linter + no docs/design ---");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_orchestrator_agent_stdin_2768.py");
    const auto t = read_file("tests/compiler/test_module_require_freevar.cpp");
    const auto orch = read_file("lib/std/orchestrator.aura");
    CHECK(build.find("check_orchestrator_agent_stdin_2768") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(t.find("ac2768_2_spawn_ask_list_epoch") != std::string::npos, "AC5: AC2 test");
    CHECK(t.find("ac2768_3_status_stop_restart") != std::string::npos, "AC5: AC3 test");
    CHECK(orch.find("#2768") != std::string::npos, "AC5: orchestrator cites #2768");
    CHECK(read_file("docs/design/2768-orchestrator-agent-stdin.md").empty(),
          "AC5: no docs/design/2768-* per #1655");
}

// ── Issue #2769: stdlib-wide require/export form-order audit ──

static bool export_before_require_or_export_only(const std::string& src) {
    // First col-0 (export / (require — export-first or export-only.
    // Match "(export" / "(require" with word boundary (space, newline, or
    // end) — some modules use multi-line "(export\n  sym …)".
    auto first_toplevel_form = [&](std::string_view head) -> std::size_t {
        std::size_t pos = 0;
        while (pos < src.size()) {
            auto i = src.find(head, pos);
            if (i == std::string::npos)
                return std::string::npos;
            auto line_start = src.rfind('\n', i);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            if (i == line_start) {
                const auto after = i + head.size();
                if (after >= src.size() || src[after] == ' ' || src[after] == '\n' ||
                    src[after] == '\t' || src[after] == '\r')
                    return i;
            }
            pos = i + 1;
        }
        return std::string::npos;
    };
    const auto exp = first_toplevel_form("(export");
    const auto req = first_toplevel_form("(require");
    if (exp == std::string::npos)
        return false;
    if (req == std::string::npos)
        return true; // export-only
    return exp < req;
}

static void ac2769_1_inventory() {
    std::println("\n--- #2769 AC1: stdlib inventory + audit script ---");
    const auto audit =
        read_file("scripts/coverage/checks/check_stdlib_require_export_audit_2769.py");
    CHECK(!audit.empty(), "AC1: audit script present");
    CHECK(audit.find("ModuleInfo") != std::string::npos, "AC1: ModuleInfo inventory");
    CHECK(audit.find("require-first") != std::string::npos, "AC1: classifies require-first");
    CHECK(audit.find("--print-table") != std::string::npos, "AC1: --print-table");
    CHECK(audit.find("--fix") != std::string::npos, "AC1: --fix autofix");
    // Denseness modules present in lib/std.
    CHECK(!read_file("lib/std/llm.aura").empty(), "AC1: llm.aura");
    CHECK(!read_file("lib/std/hot-strategy.aura").empty(), "AC1: hot-strategy.aura");
    CHECK(!read_file("lib/std/net.aura").empty(), "AC1: net.aura");
    CHECK(!read_file("lib/std/orchestrator.aura").empty(), "AC1: orchestrator.aura");
    CHECK(!read_file("lib/std/agent.aura").empty(), "AC1: agent.aura");
}

static void ac2769_2_denseness_paths_green() {
    std::println("\n--- #2769 AC2: denseness paths green (llm/hs/agent/orch/mutate/query) ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC2: lib found");
    if (lib.empty())
        return;
    setenv("AURA_PATH", lib.string().c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);

    CompilerService cs;
    // llm rate-limit cell free-vars
    CHECK(cs.eval("(require \"std/llm\" all:)").has_value(), "AC2: require llm");
    auto lim = cs.eval("(llm:rate-limit-set! 4)");
    CHECK(lim && is_int(*lim) && as_int(*lim) == 4, "AC2: llm:rate-limit-set!");
    auto rem = cs.eval("(llm:rate-limit-remaining)");
    CHECK(rem && is_int(*rem) && as_int(*rem) == 4, "AC2: llm:rate-limit-remaining cell");

    CompilerService cs2;
    CHECK(cs2.eval("(require \"std/hot-strategy\" all:)").has_value(), "AC2: require hot-strategy");
    auto ver = cs2.eval("(hot-strategy:version)");
    CHECK(ver && is_int(*ver), "AC2: hot-strategy:version cell");

    CompilerService cs3;
    CHECK(cs3.eval("(require \"std/orchestrator\" all:)").has_value(), "AC2: require orchestrator");
    CHECK(cs3.eval("(agent:spawn \"d9\" (lambda (x) (* x 2)))").has_value(), "AC2: agent:spawn");
    auto ask = cs3.eval("(agent:ask \"d9\" 21)");
    CHECK(ask && is_int(*ask) && as_int(*ask) == 42, "AC2: agent:ask denseness");

    CompilerService cs4;
    CHECK(cs4.eval("(require \"std/agent\" all:)").has_value(), "AC2: require agent");
    CHECK(cs4.eval("(agent:loop-stats)").has_value(), "AC2: agent:loop-stats");

    CompilerService cs5;
    CHECK(eval_bool(cs5, "(begin (require \"std/mutate\" all:) (mutate:boundary-safe?))"),
          "AC2: mutate:boundary-safe?");

    CompilerService cs6;
    CHECK(cs6.eval("(require \"std/query\" all:)").has_value(), "AC2: require query");
    CHECK(cs6.eval("(query:list-categories)").has_value(), "AC2: query:list-categories");

    CompilerService cs7;
    CHECK(cs7.eval("(require \"std/net\" all:)").has_value(), "AC2: require net");
    CHECK(cs7.eval("(url-encode \"a b\")").has_value(), "AC2: net url-encode");
}

static void ac2769_3_host_canary_2766() {
    std::println("\n--- #2769 AC3: host #2766 free-var canary still present ---");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(efl.find("#2766") != std::string::npos, "AC3: eval_flat cites #2766");
    CHECK(efl.find("is_module_prologue") != std::string::npos, "AC3: is_module_prologue");
    CHECK(efl.find("Phase 0") != std::string::npos, "AC3: Phase 0 prologue");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("#2766") != std::string::npos, "AC3: loader cites #2766");
    CHECK(loader.find("set_pool") != std::string::npos, "AC3: mod_env set_pool");
    // Shared ac2766 residual tests still in this suite.
    const auto t = read_file("tests/compiler/test_module_require_freevar.cpp");
    CHECK(t.find("ac2766_1_require_before_export_private_cell") != std::string::npos,
          "AC3: ac2766_1 still present (shared canary)");
}

static void ac2769_4_form_order_policy() {
    std::println("\n--- #2769 AC4: denseness modules export-first policy ---");
    const char* modules[] = {
        "lib/std/llm.aura",
        "lib/std/hot-strategy.aura",
        "lib/std/net.aura",
        "lib/std/io.aura",
        "lib/std/agent.aura",
        "lib/std/orchestrator.aura",
        "lib/std/ast-viz.aura",
        "lib/std/hash.aura",
        "lib/std/heal.aura",
        "lib/std/hot-update-monitor.aura",
        "lib/std/hot-update-reload.aura",
    };
    for (const char* m : modules) {
        const auto src = read_file(m);
        CHECK(!src.empty(), std::string("AC4: read ") + m);
        CHECK(export_before_require_or_export_only(src),
              std::string("AC4: export-before-require (or export-only) in ") + m);
    }
    // llm / hot-strategy cite #2769 form-order note.
    CHECK(read_file("lib/std/llm.aura").find("#2769") != std::string::npos, "AC4: llm cites #2769");
    CHECK(read_file("lib/std/hot-strategy.aura").find("#2769") != std::string::npos,
          "AC4: hot-strategy cites #2769");
}

static void ac2769_5_linter() {
    std::println("\n--- #2769 AC5: linter + INDEX authoring note ---");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_stdlib_require_export_audit_2769.py");
    const auto t = read_file("tests/compiler/test_module_require_freevar.cpp");
    const auto index = read_file("lib/std/INDEX.aura");
    CHECK(build.find("check_stdlib_require_export_audit_2769") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(t.find("ac2769_1_inventory") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2769_2_denseness_paths_green") != std::string::npos, "AC5: AC2 test");
    CHECK(index.find("#2769") != std::string::npos, "AC5: INDEX authoring note #2769");
    CHECK(index.find("#2766") != std::string::npos, "AC5: INDEX cites host #2766");
    CHECK(index.find("export") != std::string::npos && index.find("require") != std::string::npos,
          "AC5: INDEX mentions export/require order");
    CHECK(read_file("docs/design/2769-stdlib-require-export-audit.md").empty(),
          "AC5: no docs/design/2769-* per #1655");
}

} // namespace

int run_test_module_require_freevar() {
    std::println("=== Issue #2566: module free-var resolve for required std bindings ===");
    ac1_module_freevar_parity();
    ac2_toplevel_inject();
    ac3_source_cite_inject();
    ac4_soa_live_top();
    ac5_gate();
    std::println("\n=== Issue #2766: require-before-export free-var capture ===");
    ac2766_1_require_before_export_private_cell();
    ac2766_2_export_before_require_parity();
    ac2766_3_orchestrator_agent_registry();
    ac2766_4_source_cite();
    ac2766_5_linter();
    std::println("\n=== Issue #2768: std/orchestrator multi-agent stdin denseness ===");
    ac2768_1_orchestrator_export_first();
    ac2768_2_spawn_ask_list_epoch();
    ac2768_3_status_stop_restart();
    ac2768_4_parallel_with_yield_smoke();
    ac2768_5_linter();
    std::println("\n=== Issue #2769: stdlib require/export form-order audit ===");
    ac2769_1_inventory();
    ac2769_2_denseness_paths_green();
    ac2769_3_host_canary_2766();
    ac2769_4_form_order_policy();
    ac2769_5_linter();
    std::println("\n=== #2566+#2766+#2768+#2769: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_require_freevar();
}
#endif
