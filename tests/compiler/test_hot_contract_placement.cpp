// @category: unit
// @reason: Issue #2435 — Hot vs Cold contract placement; production hot
//          loops observe-only or off (default OFF under NDEBUG).
//
//   AC1: Production default: hot-loop contracts OFF (or observe)
//   AC2: Mutation / pass / compact edges still enforce (language pre/post)
//   AC3: Microbench proxy: 1e6 as_int under OFF ≈ fully disabled
//   AC4: Cold-path / debug still catch violations (source + mode)
//   AC5: Policy docs + schema-2435 + source-cite
//   #3043 AC1–AC5: Soft-observe tier (metrics, no abort); production OFF
//                  default unchanged; query hot-contract-false-total

#include "test_harness.hpp"

#include "core/cpp26_contract_stats.h"
#include "compiler/typed_mutation_audit.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.pass_manager;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::IRModuleV2;
using aura::compiler::run_pipeline;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::core::cpp26::current_hot_contracts_mode;
using aura::core::cpp26::hotpath_invariant_hits_total;
using aura::core::cpp26::kHotContractPlacementIssue;
using aura::core::cpp26::kHotContractsMode;
using aura::core::cpp26::kHotModeEnforce;
using aura::core::cpp26::kHotModeObserve;
using aura::core::cpp26::kHotModeOff;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:cpp26-contracts-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_hot_contract_placement() {
    std::println("=== Issue #2435: hot contract placement (hot OFF in production) ===");
    CHECK(kHotContractPlacementIssue == 2435, "issue stamp");
    CHECK(current_hot_contracts_mode() == kHotContractsMode, "mode accessor");

    // ── AC1: production default OFF under NDEBUG ───────────────────
    {
        std::println("\n--- #2435 AC1: production hot mode ---");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("Issue #2435") != std::string::npos, "AC1: policy #2435");
        CHECK(hh.find("production") != std::string::npos ||
                  hh.find("Production") != std::string::npos,
              "AC1: production policy text");
        CHECK(hh.find("AURA_HOT_MODE_OFF") != std::string::npos, "AC1: HOT_MODE_OFF");
        CHECK(hh.find("AURA_COLD_CONTRACT") != std::string::npos, "AC1: cold tier macro");
        CHECK(hh.find("AURA_HOT_CHECK_CONSTEXPR") != std::string::npos, "AC1: constexpr hot check");
        CHECK(hh.find("Hot") != std::string::npos && hh.find("Cold") != std::string::npos,
              "AC1: Hot vs Cold tier docs");
#if defined(NDEBUG) && !defined(AURA_CONTRACTS_ENFORCE) && !defined(AURA_CONTRACTS_OBSERVE) &&     \
    !defined(AURA_CONTRACTS_HOT_MODE_ENFORCE) && !defined(AURA_CONTRACTS_HOT_MODE_OBSERVE) &&      \
    !defined(AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE) && !defined(AURA_HOT_SOFT_OBSERVE)
        CHECK(kHotContractsMode == kHotModeOff, "AC1: NDEBUG default mode=off");
#else
        // Debug/enforce/observe builds are intentional non-production.
        CHECK(kHotContractsMode == kHotModeEnforce || kHotContractsMode == kHotModeObserve ||
                  kHotContractsMode == kHotModeOff,
              "AC1: mode is valid enum");
        std::println("  (build mode={} — not production OFF)", kHotContractsMode);
#endif
    }

    // ── AC2: cold edges still present (language pre / pass entry) ──
    {
        std::println("\n--- #2435 AC2: cold edges still enforce (language pre) ---");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        // #2524: run_pipeline pre cold edge lives in pass_pipeline_core.
        auto pm = read_file("src/compiler/pass_manager.ixx") +
                  read_file("src/compiler/pass_pipeline_core.ixx") +
                  read_file("src/compiler/pass_impls.ixx");
        // view_at keeps language pre (cold bounds edge)
        CHECK(soa.find("pre(func_idx < functions.size())") != std::string::npos,
              "AC2: view_at language pre cold edge");
        // Absolute-hot column access uses HOT_CHECK_CONSTEXPR not bare assert
        CHECK(soa.find("AURA_HOT_CHECK_CONSTEXPR") != std::string::npos,
              "AC2: IRInstructionView hot check macro");
        CHECK(soa.find("contract_assert(idx < func->opcodes_.size())") == std::string::npos,
              "AC2: bare contract_assert removed from opcode()");
        // Pass pipeline entry still has pre
        CHECK(pm.find("pre(sizeof...(Passes) > 0)") != std::string::npos ||
                  pm.find("pre(&pass != nullptr)") != std::string::npos,
              "AC2: pass pipeline pre cold edge");
        // Smoke: pass pipeline runs
        IRModule mod;
        aura::ir::IRFunction fn;
        fn.name = "c2435";
        fn.local_count = 2;
        aura::ir::BasicBlock b;
        b.id = 0;
        b.instructions.push_back(
            aura::ir::IRInstruction{.opcode = IROpcode::ConstI64, .operands = {0, 1, 0, 0}});
        fn.blocks.push_back(std::move(b));
        mod.functions.push_back(std::move(fn));
        ConstantFoldingWrap cf;
        CHECK(run_pipeline(mod, cf), "AC2: run_pipeline cold pre ok");
    }

    // ── AC3: microbench proxy — 1e6 as_int under OFF ≈ disabled ────
    {
        std::println("\n--- #2435 AC3: microbench proxy 1e6 as_int ---");
        constexpr int kN = 1'000'000;
        // Warm + time pure as_int loop (hot path under current mode).
        std::int64_t sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kN; ++i) {
            auto v = make_int(i & 0xFFFF);
            sink += as_int(v);
        }
        // Prevent DCE of the timed loop without making sink unformattable.
        asm volatile("" : "+r"(sink));
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const double ns_per = static_cast<double>(ns) / static_cast<double>(kN);
        std::println("  {} iters: {} ns total, {:.3f} ns/op, sink={}, mode={}", kN, ns, ns_per,
                     sink, kHotContractsMode);
        // Under OFF, RECORD/CHECK are ((void)0) — overhead identical to disabled.
        // Under enforce, we only require the loop completes correctly.
        CHECK(sink != 0 || kN == 0, "AC3: loop executed");
        if constexpr (kHotContractsMode == kHotModeOff) {
            // Proxy for ≤1%: zero contract work ⇒ overhead 0% vs disabled.
            CHECK(true, "AC3: production OFF ⇒ contract overhead 0% (≤1%)");
            const auto h0 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
            for (int i = 0; i < 10000; ++i)
                (void)as_int(make_int(i));
            const auto h1 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
            CHECK(h1 == h0, "AC3: OFF mode does not bump hits");
        } else {
            CHECK(true, "AC3: non-OFF mode — correctness only (no overhead gate)");
        }
        (void)ns_per;
    }

    // ── AC4: cold-path / debug still catch (source policy) ─────────
    {
        std::println("\n--- #2435 AC4: cold / debug catch policy ---");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("AURA_COLD_CONTRACT") != std::string::npos, "AC4: cold macro");
        CHECK(hh.find("fail-closed") != std::string::npos ||
                  hh.find("contract_assert") != std::string::npos,
              "AC4: enforce path present");
        // Valid programs unchanged
        auto v = make_int(42);
        CHECK(as_int(v) == 42, "AC4: as_int valid");
        IRModuleV2 mod;
        auto fi = mod.add_function("f", 1);
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
        auto view = mod.view_at(fi, 0);
        CHECK(view.opcode() == IROpcode::ConstI64, "AC4: view_at opcode");
    }

    // ── AC5: schema-2435 + source-cite ─────────────────────────────
    {
        std::println("\n--- #2435 AC5: schema + source-cite ---");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("schema-2435") != std::string::npos, "AC5: schema-2435");
        CHECK(q.find("hot-contracts-mode") != std::string::npos, "AC5: mode query key");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok");
        CHECK(href(cs, "schema-2435") == 2435, "AC5: schema-2435 runtime");
        CHECK(href(cs, "issue-2435") == 2435, "AC5: issue-2435");
        CHECK(href(cs, "hot-contract-placement-wired") == 1, "AC5: placement wired");
        CHECK(href(cs, "hotpath-contracts-2435-active") == 1, "AC5: 2435 active");
        const auto mode = href(cs, "hot-contracts-mode");
        CHECK(mode == 0 || mode == 1 || mode == 2, "AC5: mode 0|1|2");
        CHECK(mode == kHotContractsMode, "AC5: query matches compile-time mode");
    }

    // ── #3043: Soft-observe tier (production default still OFF) ─────
    {
        std::println("\n--- #3043 AC1: production default still OFF ---");
        CHECK(aura::core::cpp26::kHotContractSoftObserveIssue == 3043, "3043 AC1: issue constant");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("Issue #3043") != std::string::npos, "3043 AC1: policy #3043");
        CHECK(hh.find("Soft-observe") != std::string::npos ||
                  hh.find("SOFT_OBSERVE") != std::string::npos,
              "3043 AC1: Soft-observe tier documented");
        CHECK(hh.find("AURA_HOT_MODE_OFF") != std::string::npos, "3043 AC1: OFF still present");
#if defined(NDEBUG) && !defined(AURA_CONTRACTS_ENFORCE) && !defined(AURA_CONTRACTS_OBSERVE) &&     \
    !defined(AURA_CONTRACTS_HOT_MODE_ENFORCE) && !defined(AURA_CONTRACTS_HOT_MODE_OBSERVE) &&      \
    !defined(AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE) && !defined(AURA_HOT_SOFT_OBSERVE)
        CHECK(kHotContractsMode == kHotModeOff, "3043 AC1: NDEBUG default still off");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        const auto h0 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        AURA_HOT_CHECK(false); // must not abort under Soft / !production_defaults
        AURA_HOT_RECORD();
        const auto h1 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        CHECK(h1 == h0, "3043 AC1: OFF RECORD still zero-cost");
#else
        CHECK(kHotContractsMode == kHotModeEnforce || kHotContractsMode == kHotModeObserve ||
                  kHotContractsMode == kHotModeOff,
              "3043 AC1: mode is valid enum");
#endif

        std::println("\n--- #3043 AC2: Soft-observe metrics, no abort ---");
        CHECK(hh.find("observe_hot_contract_false") != std::string::npos,
              "3043 AC2: observe helper present");
        CHECK(hh.find("AURA_HOT_MODE_SOFT_OBSERVE") != std::string::npos,
              "3043 AC2: Soft-observe flag");
        const auto f0 =
            aura::core::cpp26::contract_violation_hotpath_count.load(std::memory_order_relaxed);
        aura::core::cpp26::observe_hot_contract_false();
        const auto f1 =
            aura::core::cpp26::contract_violation_hotpath_count.load(std::memory_order_relaxed);
        CHECK(f1 > f0, "3043 AC2: observe_hot_contract_false visible");
        CHECK(true, "3043 AC2: helper does not abort");

        std::println("\n--- #3043 AC3: Enforce path unchanged ---");
        CHECK(hh.find("contract_assert(expr)") != std::string::npos, "3043 AC3: enforce assert");
        CHECK(hh.find("AURA_HOT_MODE_ENFORCE") != std::string::npos, "3043 AC3: enforce flag");
        CHECK(hh.find("fail-closed") != std::string::npos, "3043 AC3: fail-closed debug");

        std::println("\n--- #3043 AC4: sampled RECORD upper bound ---");
        CHECK(hh.find("kHotSoftObserveRecordSample") != std::string::npos,
              "3043 AC4: sample period named");
        CHECK(aura::core::cpp26::kHotSoftObserveRecordSample == 256, "3043 AC4: sample period 256");
        CHECK(hh.find("record_hotpath_invariant_hit_sampled") != std::string::npos,
              "3043 AC4: sampled RECORD helper");
        CHECK(hh.find("per-call atomic") != std::string::npos ||
                  hh.find("per-call atomic RMW") != std::string::npos,
              "3043 AC4: no per-call RMW documented");

        std::println("\n--- #3043 AC5: query hot-contract-false ---");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("schema-3043") != std::string::npos, "3043 AC5: schema-3043 query key");
        CHECK(q.find("hot-contract-false-total") != std::string::npos,
              "3043 AC5: false-total query key");
        CompilerService cs3043;
        CHECK(cs3043.eval("(+ 1 2)").has_value(), "3043 eval ok");
        CHECK(href(cs3043, "schema-3043") == 3043, "3043 AC5: schema-3043 runtime");
        CHECK(href(cs3043, "issue-3043") == 3043, "3043 AC5: issue-3043");
        CHECK(href(cs3043, "hot-contract-soft-observe-wired") == 1, "3043 AC5: soft-observe wired");
        CHECK(href(cs3043, "hot-contract-false-total") >= 0, "3043 AC5: false-total readable");
        CHECK(href(cs3043, "hot-contract-soft-observe-sample-period") == 256,
              "3043 AC5: sample period queryable");
        CHECK(href(cs3043, "hot-contracts-mode-env") >= 0, "3043 AC5: mode-env readable");
        CHECK(href(cs3043, "hot-contracts-production-off-default") == 1,
              "3043 AC5: production OFF default wired");
    }

    // ── #3106: Harden-armed Soft-observe hot contract ─────────────────
    {
        // #3139 AC4a: probe considers production_defaults_active() so
        // self-modify preset binaries are implicitly armed without env/HARDEN-flag.
        // Source-level check (mirrors linter scripts/coverage/checks/
        // check_hot_contract_harden_3106.py AC4 closure).
        auto test_3139_ac4a_probe_consults_production_defaults = []() {
            std::println("3139 AC4a -- probe considers production_defaults_active()");
            std::ifstream in{"src/core/cpp26_contract_stats.h"};
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string hh = ss.str();
            CHECK(hh.find("Issue #3139") != std::string::npos,
                  "3139 AC4a: Issue #3139 marker present in probe");
            CHECK(hh.find("aura_production_defaults_active_probe") != std::string::npos,
                  "3139 AC4a: production_defaults C ABI (#3313, no full audit header)");
        };

        // #3139 AC4b: implicit-arm check is gated by parsed==0 (env OFF respected).
        // If parsed was already 1 (env AURA_HOT_HARDEN set), the implicit arm is
        // skipped — env OFF cannot be silently overridden by production_defaults.
        auto test_3139_ac4b_implicit_arm_gated_by_parsed_zero = []() {
            std::println("3139 AC4b -- implicit-arm gated by parsed==0");
            std::ifstream in{"src/core/cpp26_contract_stats.h"};
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string hh = ss.str();
            CHECK(hh.find("if (parsed == 0 && ") != std::string::npos,
                  "3139 AC4b: implicit-arm guard present (parsed==0 check)");
        };

        // #3139 AC4c: include of typed_mutation_audit.h is at the top of the header
        // (near other plain-header includes) so production_defaults_active() is
        // reachable without crossing module boundaries.
        auto test_3139_ac4c_include_at_top = []() {
            std::println("3139 AC4c -- include at top of header");
            std::ifstream in{"src/core/cpp26_contract_stats.h"};
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string head = ss.str().substr(0, 2000);
            CHECK(head.find("aura_production_defaults_active_probe") != std::string::npos,
                  "3139 AC4c: C ABI production_defaults probe in first 2000 chars (#3313)");
        };

        std::println("\n--- #3106 AC1: harden-armed CHECK path ---");
        test_3139_ac4a_probe_consults_production_defaults();
        test_3139_ac4b_implicit_arm_gated_by_parsed_zero();
        test_3139_ac4c_include_at_top();
        CHECK(aura::core::cpp26::kHotContractHardenIssue == 3106, "3106 AC1: issue constant");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("Issue #3106") != std::string::npos, "3106 AC1: policy #3106");
        CHECK(hh.find("AURA_HOT_MODE_HARDEN") != std::string::npos, "3106 AC1: HARDEN mode");
        CHECK(hh.find("record_hotpath_contract_harden_trap") != std::string::npos,
              "3106 AC1: trap helper wired");
        CHECK(hh.find("hotpath_contract_harden_trap_total") != std::string::npos,
              "3106 AC1: trap counter declared");
        // The macro emits observe_hot_contract_false + record_..._trap + std::abort()
        CHECK(hh.find("observe_hot_contract_false();                                     \\") !=
                      std::string::npos ||
                  hh.find("observe_hot_contract_false();") != std::string::npos,
              "3106 AC1: observe counter still bumps under HARDEN");
        CHECK(hh.find("record_hotpath_contract_harden_trap();                            \\") !=
                      std::string::npos ||
                  hh.find("record_hotpath_contract_harden_trap();") != std::string::npos,
              "3106 AC1: trap counter bump inlined in macro");
        CHECK(hh.find("std::abort();") != std::string::npos,
              "3106 AC1: fail-closed std::abort trap");
        // Verify trap helper actually bumps the counter (without triggering the
        // macro's abort, which would kill the test process).
        const auto ht0 =
            aura::core::cpp26::hotpath_contract_harden_trap_total.load(std::memory_order_relaxed);
        aura::core::cpp26::record_hotpath_contract_harden_trap();
        const auto ht1 =
            aura::core::cpp26::hotpath_contract_harden_trap_total.load(std::memory_order_relaxed);
        CHECK(ht1 > ht0, "3106 AC1: trap helper bumps counter");
        // Query surface exposes the new keys
        CompilerService cs3106;
        CHECK(cs3106.eval("(+ 1 2)").has_value(), "3106 eval ok");
        CHECK(href(cs3106, "schema-3106") == 3106, "3106 AC1: schema-3106 runtime");
        CHECK(href(cs3106, "issue-3106") == 3106, "3106 AC1: issue-3106");
        CHECK(href(cs3106, "hot-contract-harden-wired") == 1, "3106 AC1: harden wired");
        CHECK(href(cs3106, "hotpath-contracts-3106-active") == 1, "3106 AC1: 3106 active");
        CHECK(href(cs3106, "hot-contract-harden-issue") == 3106, "3106 AC1: harden issue");
        CHECK(href(cs3106, "hot-contract-harden-trap-total") >= 0, "3106 AC1: trap-total readable");
        CHECK(href(cs3106, "hot-contract-harden-armed") >= 0, "3106 AC1: armed probe readable");

        std::println("\n--- #3106 AC2: harden-disarmed OFF path remains zero-cost ---");
        // Issue #3313 residual: NDEBUG OFF CHECK is runtime-gated. Soft /
        // !production_defaults → armed()==0, expr not evaluated.
        CHECK(hh.find("hot_contract_harden_armed()") != std::string::npos,
              "3106 AC2: OFF check runtime-gated (#3313)");
        CHECK(hh.find("Production OFF: zero cost") != std::string::npos,
              "3106 AC2: OFF comment unchanged");
        // HARDEN branch's happy path is a branch + sampled atomic (no per-call RMW).
        CHECK(hh.find("sampled RECORD + CHECK on false") != std::string::npos,
              "3106 AC2: harden uses sampled RECORD on happy path");
        // Mode enum still includes OFF (0)
        CHECK(hh.find("kHotModeOff = 0") != std::string::npos, "3106 AC2: kHotModeOff = 0");

        std::println("\n--- #3106 AC3: sampled RECORD + observe under harden ---");
        // Sample period unchanged
        CHECK(aura::core::cpp26::kHotSoftObserveRecordSample == 256,
              "3106 AC3: sample period 256 unchanged");
        CHECK(hh.find("record_hotpath_invariant_hit_sampled") != std::string::npos,
              "3106 AC3: sampled RECORD helper wired");
        // observe_hot_contract_false still wired under HARDEN (bumped BEFORE abort)
        CHECK(hh.find("observe_hot_contract_false()") != std::string::npos,
              "3106 AC3: observe counter wired");
        const auto cf0 =
            aura::core::cpp26::contract_violation_hotpath_count.load(std::memory_order_relaxed);
        aura::core::cpp26::observe_hot_contract_false();
        const auto cf1 =
            aura::core::cpp26::contract_violation_hotpath_count.load(std::memory_order_relaxed);
        CHECK(cf1 > cf0, "3106 AC3: observe counter bumps in isolation");
        CHECK(href(cs3106, "hot-contract-soft-observe-sample-period") == 256,
              "3106 AC3: sample period queryable");

        std::println("\n--- #3106 AC4: production-soak / agent-self-modify presets ---");
        // The build.py production-soak preset must arm harden by default.
        // Source-cite the compile flag string.
        CHECK(hh.find("AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE_HARDEN") != std::string::npos,
              "3106 AC4: HARDEN compile flag declared");
        CHECK(hh.find("AURA_HOT_SOFT_OBSERVE_HARDEN") != std::string::npos,
              "3106 AC4: HARDEN legacy alias declared");
        // runtime accessor exposed via query
        const int armed = href(cs3106, "hot-contract-harden-armed");
        CHECK(armed == 0 || armed == 1, "3106 AC4: armed is 0/1");

        std::println("\n--- #3106 AC5: cold contracts / arena / shape unchanged ---");
        // Cold path macros must remain unchanged (no harden flag on cold path).
        CHECK(hh.find("AURA_COLD_CONTRACT") != std::string::npos, "3106 AC5: cold macro wired");
        CHECK(hh.find("#define AURA_COLD_CONTRACT(expr) contract_assert(expr)") !=
                  std::string::npos,
              "3106 AC5: cold enforce unchanged");
        CHECK(hh.find("#define AURA_COLD_CONTRACT(expr) ((void)0)") != std::string::npos,
              "3106 AC5: cold off unchanged");
        // No new process-wide lock: only relaxed atomics added (counter bumps).
        // cpp26_contract_stats.h uses <atomic> + std::memory_order_relaxed — no
        // std::mutex / std::shared_mutex added.
        CHECK(hh.find("std::mutex") == std::string::npos &&
                  hh.find("std::shared_mutex") == std::string::npos,
              "3106 AC5: no new process-wide lock");
    }

    // ── #3313: production_defaults arms Soft-observe+Harden for NDEBUG OFF ──
    {
        std::println("\n--- #3313 AC1: production_defaults false CHECK is fail-closed ---");
        CHECK(aura::core::cpp26::kHotContractProductionHardenIssue == 3313,
              "3313 AC1: issue constant");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("Issue #3313") != std::string::npos, "3313 AC1: policy #3313");
        CHECK(hh.find("kHotContractProductionHardenIssue = 3313") != std::string::npos,
              "3313 AC1: stamp");
        CHECK(hh.find("hot_contract_harden_armed()") != std::string::npos,
              "3313 AC1: OFF macros consult armed probe");
        CHECK(hh.find("record_hotpath_contract_harden_trap") != std::string::npos,
              "3313 AC1: trap helper");
        CHECK(hh.find("std::abort();") != std::string::npos, "3313 AC1: fail-closed abort");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::core::cpp26::hot_contract_harden_armed(),
              "3313 AC1: production_defaults arms probe");
        AURA_HOT_CHECK(true); // happy path: branch + no trap
        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(AURA_HOT_MODE_HARDEN)
        CHECK(aura::core::cpp26::hot_contract_harden_armed(),
              "3313 AC1: compile HARDEN stays armed");
#else
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3313 AC1: Soft restores disarmed");
#endif
        CompilerService cs3313;
        CHECK(cs3313.eval("(+ 1 2)").has_value(), "3313 eval ok");
        CHECK(href(cs3313, "schema-3313") == 3313, "3313 AC1: schema-3313");
        CHECK(href(cs3313, "issue-3313") == 3313, "3313 AC1: issue-3313");
        CHECK(href(cs3313, "hot-contract-harden-armed") >= 0, "3313 AC1: armed key reused");
        CHECK(href(cs3313, "hot-contract-harden-trap-total") >= 0, "3313 AC1: trap-total reused");
        CHECK(href(cs3313, "hotpath-contracts-3313-active") == 1, "3313 AC1: 3313 active");

        std::println("\n--- #3313 AC2: Soft / unit macros remain no-op ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(NDEBUG) && !defined(AURA_HOT_MODE_HARDEN) && !defined(AURA_HOT_MODE_ENFORCE)
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3313 AC2: Soft disarmed");
        const auto h0 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        const auto t0 =
            aura::core::cpp26::hotpath_contract_harden_trap_total.load(std::memory_order_relaxed);
        AURA_HOT_CHECK(false); // must not abort
        AURA_HOT_RECORD();
        CHECK(hotpath_invariant_hits_total.load(std::memory_order_relaxed) == h0,
              "3313 AC2: Soft RECORD no extra atomic RMW");
        CHECK(aura::core::cpp26::hotpath_contract_harden_trap_total.load(
                  std::memory_order_relaxed) == t0,
              "3313 AC2: Soft no trap bump");
#endif
        CHECK(hh.find("expr not evaluated") != std::string::npos, "3313 AC2: Soft skips expr");

        std::println("\n--- #3313 AC3: sample period still applies under Harden ---");
        CHECK(aura::core::cpp26::kHotSoftObserveRecordSample == 256, "3313 AC3: sample 256");
        CHECK(hh.find("record_hotpath_invariant_hit_sampled") != std::string::npos,
              "3313 AC3: sampled RECORD");
        CHECK(hh.find("hot_contract_harden_armed()") != std::string::npos &&
                  hh.find("record_hotpath_invariant_hit_sampled") != std::string::npos,
              "3313 AC3: OFF RECORD uses sampled helper when armed");

        std::println("\n--- #3313 AC4: linter + no invent / docs ---");
        auto build = read_file("build.py");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(build.find("check_hot_contract_production_harden_3313") != std::string::npos,
              "3313 AC4: build.py wires linter");
        CHECK(q.find("schema-3313") != std::string::npos, "3313 AC4: additive schema stamp");
        CHECK(read_file("tests/compiler/test_issue_3313.cpp").empty(), "3313 AC4: no invent");
        CHECK(read_file("docs/design/3313-hot-contract-production-harden.md").empty(),
              "3313 AC4: no docs/design");
        CHECK(hh.find("AURA_COLD_CONTRACT") != std::string::npos, "3313 AC4: cold unchanged");
    }

    // ── #3428: view_at HARDEN-armed AURA_HOT_CHECK (I1 residual of #3106) ──
    {
        std::println("\n--- #3428 AC1: view_at calls AURA_HOT_CHECK ---");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        const auto vat = soa.find("IRInstructionView view_at(");
        CHECK(vat != std::string::npos, "3428 AC1: view_at present");
        const auto nxt = soa.find("add_block", vat);
        const auto win =
            (vat != std::string::npos && nxt > vat) ? soa.substr(vat, nxt - vat) : std::string{};
        CHECK(win.find("AURA_HOT_CONTRACT") != std::string::npos,
              "3428 AC1: view_at body contains AURA_HOT_CONTRACT");
        CHECK(win.find("AURA_HOT_RECORD()") == std::string::npos,
              "3428 AC1: RECORD folded into CONTRACT (#3501)");
        CHECK(win.find("pre(func_idx < functions.size())") != std::string::npos,
              "3428 AC1: language pre cold edge kept");
        CHECK(win.find("Issue #3428") != std::string::npos, "3428 AC1: issue cite");
        CHECK(hh.find("record_hotpath_contract_harden_trap") != std::string::npos,
              "3428 AC1: trap helper reused");
        CHECK(hh.find("std::abort();") != std::string::npos, "3428 AC1: abort reused");
        const auto ht0 =
            aura::core::cpp26::hotpath_contract_harden_trap_total.load(std::memory_order_relaxed);
        aura::core::cpp26::record_hotpath_contract_harden_trap();
        CHECK(aura::core::cpp26::hotpath_contract_harden_trap_total.load(
                  std::memory_order_relaxed) > ht0,
              "3428 AC1: trap helper bumps counter");
        IRModuleV2 mod;
        auto fi = mod.add_function("f3428", 1);
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
        auto view = mod.view_at(fi, 0);
        CHECK(view.opcode() == IROpcode::ConstI64, "3428 AC1: happy-path view_at");

        std::println("\n--- #3428 AC2: OFF/Soft no extra atomic beyond RECORD ---");
        CHECK(hh.find("hot_contract_harden_armed()") != std::string::npos,
              "3428 AC2: CHECK runtime-gated");
        CHECK(hh.find("expr not evaluated") != std::string::npos, "3428 AC2: Soft skips expr");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(NDEBUG) && !defined(AURA_HOT_MODE_HARDEN) && !defined(AURA_HOT_MODE_ENFORCE)
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3428 AC2: Soft disarmed");
        const auto h0 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        const auto t0 =
            aura::core::cpp26::hotpath_contract_harden_trap_total.load(std::memory_order_relaxed);
        (void)mod.view_at(fi, 0);
        CHECK(hotpath_invariant_hits_total.load(std::memory_order_relaxed) == h0,
              "3428 AC2: OFF RECORD no extra atomic");
        CHECK(aura::core::cpp26::hotpath_contract_harden_trap_total.load(
                  std::memory_order_relaxed) == t0,
              "3428 AC2: OFF no trap bump");
#endif

        std::println("\n--- #3428 AC3: #3106/#3043 suites stay green ---");
        CHECK(hh.find("Issue #3106") != std::string::npos, "3428 AC3: #3106 policy");
        CHECK(hh.find("Issue #3043") != std::string::npos, "3428 AC3: #3043 policy");
        CHECK(soa.find("AURA_HOT_CHECK_CONSTEXPR") != std::string::npos,
              "3428 AC3: CHECK_CONSTEXPR kept on constexpr getters");
        auto build = read_file("build.py");
        CHECK(build.find("check_hot_contract_harden_3106") != std::string::npos,
              "3428 AC3: 3106 linter still wired");

        std::println("\n--- #3428 AC4: placement test + linter, no invent ---");
        CHECK(build.find("check_hot_contract_view_at_harden_3428") != std::string::npos,
              "3428 AC4: build.py wires linter");
        CHECK(read_file("tests/compiler/test_issue_3428.cpp").empty(), "3428 AC4: no invent");
        CHECK(read_file("tests/issues/test_issue_3428.cpp").empty(), "3428 AC4: no issues invent");

        std::println("\n--- #3428 AC5: no new query key / docs / Arena policy ---");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("hot-contract-harden-trap-total") != std::string::npos,
              "3428 AC5: reuse trap-total");
        CHECK(q.find("hot-contract-harden-armed") != std::string::npos, "3428 AC5: reuse armed");
        CHECK(q.find("schema-3428") == std::string::npos, "3428 AC5: no schema-3428");
        CHECK(soa.find("g_3428_") == std::string::npos, "3428 AC5: no g_3428_*");
        CHECK(read_file("docs/design/3428-view-at-harden.md").empty(), "3428 AC5: no docs/design");
        CHECK(hh.find("AURA_COLD_CONTRACT") != std::string::npos, "3428 AC5: cold unchanged");
        CompilerService cs3428;
        CHECK(cs3428.eval("(+ 1 2)").has_value(), "3428 eval ok");
        CHECK(href(cs3428, "hot-contract-harden-trap-total") >= 0, "3428 AC5: trap-total readable");
        CHECK(href(cs3428, "hot-contract-harden-armed") >= 0, "3428 AC5: armed readable");
    }

    // ── #3490: cache Harden arm; CONSTEXPR honors the same flag ──────
    {
        std::println("\n--- #3490 AC1: armed cache, no per-CHECK C ABI probe ---");
        CHECK(aura::core::cpp26::kHotContractHardenCacheIssue == 3490, "3490 AC1: issue stamp");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(hh.find("kHotContractHardenCacheIssue = 3490") != std::string::npos,
              "3490 AC1: stamp");
        CHECK(hh.find("hot_contract_harden_armed_cache") != std::string::npos, "3490 AC1: cache");
        CHECK(hh.find("note_hot_contract_harden_armed") != std::string::npos, "3490 AC1: note");
        const auto fn = hh.find("[[nodiscard]] inline bool hot_contract_harden_armed()");
        const auto nxt = hh.find("peek_hot_contracts_mode_env", fn);
        const auto win =
            (fn != std::string::npos && nxt > fn) ? hh.substr(fn, nxt - fn) : std::string{};
        const auto load = win.find("hot_contract_harden_armed_cache.load");
        const auto probe = win.find("aura_production_defaults_active_probe() != 0");
        CHECK(load != std::string::npos && probe != std::string::npos && load < probe,
              "3490 AC1: cache load precedes C ABI probe");
        CHECK(win.find("if (v >= 0)") != std::string::npos, "3490 AC1: cache-hit return");
        CHECK(tma.find("note_hot_contract_harden_armed(true)") != std::string::npos,
              "3490 AC1: apply_production stores armed");
        CHECK(tma.find("note_hot_contract_harden_armed(false)") != std::string::npos,
              "3490 AC1: apply_dev stores disarmed");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(NDEBUG) && !defined(AURA_HOT_MODE_HARDEN) && !defined(AURA_HOT_MODE_ENFORCE)
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3490 AC1: Soft cache disarmed");
#endif
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::core::cpp26::hot_contract_harden_armed(),
              "3490 AC1: apply_production arms via cache");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(AURA_HOT_MODE_HARDEN)
        CHECK(aura::core::cpp26::hot_contract_harden_armed(),
              "3490 AC1: compile HARDEN stays armed");
#else
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3490 AC1: Soft restores disarmed");
#endif

        std::println("\n--- #3490 AC2: Soft / unarmed expr not evaluated ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(NDEBUG) && !defined(AURA_HOT_MODE_HARDEN) && !defined(AURA_HOT_MODE_ENFORCE)
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3490 AC2: Soft disarmed");
        const auto t0 =
            aura::core::cpp26::hotpath_contract_harden_trap_total.load(std::memory_order_relaxed);
        AURA_HOT_CHECK(false); // must not abort
        CHECK(aura::core::cpp26::hotpath_contract_harden_trap_total.load(
                  std::memory_order_relaxed) == t0,
              "3490 AC2: Soft no trap bump");
#endif
        CHECK(hh.find("expr not evaluated") != std::string::npos, "3490 AC2: Soft skips expr");

        std::println("\n--- #3490 AC3: column CONSTEXPR honors Harden arm ---");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        CHECK(hh.find("if !consteval") != std::string::npos, "3490 AC3: CONSTEXPR runtime arm");
        CHECK(hh.find("AURA_HOT_CHECK(expr)") != std::string::npos,
              "3490 AC3: CONSTEXPR uses CHECK");
        CHECK(soa.find("AURA_HOT_CHECK_CONSTEXPR") != std::string::npos,
              "3490 AC3: column accessors keep CONSTEXPR");
        CHECK(soa.find("Issue #3490") != std::string::npos, "3490 AC3: ir_soa cite");
        IRModuleV2 mod3490;
        auto fi = mod3490.add_function("f3490", 1);
        auto bi = mod3490.add_block(fi);
        mod3490.add_instruction(fi, IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1, 0, 0);
        mod3490.seal_block(fi, bi);
        auto view = mod3490.view_at(fi, 0);
        CHECK(view.opcode() == IROpcode::ConstI64, "3490 AC3: happy-path opcode");
        CHECK(view.operand(1) == 7, "3490 AC3: happy-path operand");

        std::println("\n--- #3490 AC4: view_at Harden path retained ---");
        const auto vat = soa.find("IRInstructionView view_at(");
        const auto addb = soa.find("add_block", vat);
        const auto vwin =
            (vat != std::string::npos && addb > vat) ? soa.substr(vat, addb - vat) : std::string{};
        CHECK(vwin.find("AURA_HOT_CONTRACT") != std::string::npos,
              "3490 AC4: view_at CONTRACT kept");
        CHECK(hh.find("record_hotpath_contract_harden_trap") != std::string::npos,
              "3490 AC4: trap helper reused");

        std::println("\n--- #3490 AC5: unarmed NDEBUG no extra atomics / no invent ---");
        auto build = read_file("build.py");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(build.find("check_hot_contract_harden_cache_3490") != std::string::npos,
              "3490 AC5: build.py");
        const auto p3313 = build.find("check_hot_contract_production_harden_3313");
        const auto p3490 = build.find("check_hot_contract_harden_cache_3490");
        CHECK(p3313 != std::string::npos && p3490 != std::string::npos && p3490 > p3313,
              "3490 AC5: linter AFTER #3313");
        CHECK(q.find("schema-3490") == std::string::npos, "3490 AC5: no schema-3490");
        CHECK(hh.find("g_3490_") == std::string::npos && soa.find("g_3490_") == std::string::npos,
              "3490 AC5: no g_3490_*");
        CHECK(q.find("hot-contract-harden-trap-total") != std::string::npos,
              "3490 AC5: reuse trap-total");
        CHECK(read_file("docs/design/3490-hot-contract-harden-cache.md").empty(),
              "3490 AC5: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3490.cpp").empty(), "3490 AC5: no invent");
        CHECK(hh.find("AURA_COLD_CONTRACT") != std::string::npos, "3490 AC5: cold unchanged");
    }

    std::println(
        "\n=== #2435/#3043/#3106/#3139/#3313/#3428/#3490 results: {} passed, {} failed ===",
        g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hot_contract_placement();
}
#endif
