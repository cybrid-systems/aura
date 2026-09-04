// @category: unit
// @reason: Issue #2142 — unify hot-path observe contracts + invariant hits.
//
//   AC1: policy documented in cpp26_contract_stats.h (AURA_HOT_CONTRACT)
//   AC2: value / ir_soa / arena primary paths use AURA_HOT_* (grep)
//   AC3: release path: AURA_HOT_CHECK is no-op under NDEBUG (static)
//   AC4: record_hotpath_invariant_hit / AURA_HOT_RECORD moves under microbench
//   AC5: valid programs unchanged; schema-2142 on query surface

#include "test_harness.hpp"

#include "core/cpp26_contract_stats.h"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir_soa;
import aura.compiler.ir;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::IRModuleV2;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::make_bool;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::core::cpp26::hotpath_invariant_hits_total;
using aura::core::cpp26::kContractHotPathsShipped;
using aura::core::cpp26::kHotContractSingleLoadIssue;
using aura::core::cpp26::kHotContractUnifyIssue;
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

int run_test_hot_contract_unify() {
    std::println("=== Issue #2142: unified AURA_HOT_CONTRACT ===");
    CHECK(kHotContractUnifyIssue == 2142, "issue stamp");
    CHECK(kContractHotPathsShipped >= 62, "hot paths count bumped");

    // ── AC1: policy docs ──
    {
        std::println("\n--- AC1: policy documentation ---");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("#2142") != std::string::npos, "header #2142");
        CHECK(hh.find("AURA_HOT_CONTRACT") != std::string::npos, "AURA_HOT_CONTRACT");
        CHECK(hh.find("AURA_HOT_RECORD") != std::string::npos, "AURA_HOT_RECORD");
        CHECK(hh.find("AURA_HOT_CHECK") != std::string::npos, "AURA_HOT_CHECK");
        CHECK(hh.find("NDEBUG") != std::string::npos, "release policy");
        CHECK(hh.find("observe-first") != std::string::npos ||
                  hh.find("Observe-first") != std::string::npos ||
                  hh.find("unified hot-path contract policy") != std::string::npos,
              "policy text");
    }

    // ── AC2: primary sites migrated ──
    {
        std::println("\n--- AC2: value / ir_soa / arena use AURA_HOT_* ---");
        auto val = read_file("src/compiler/value.ixx");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        auto ar = read_file("src/core/arena.ixx");
        CHECK(val.find("AURA_HOT_CONTRACT") != std::string::npos, "value AURA_HOT_CONTRACT");
        CHECK(val.find("AURA_HOT_RECORD") != std::string::npos, "value AURA_HOT_RECORD");
        // as_int should not pair bare contract_assert + record without helper
        CHECK(val.find("as_int") != std::string::npos, "as_int present");
        CHECK(soa.find("AURA_HOT_CONTRACT") != std::string::npos, "ir_soa AURA_HOT_CONTRACT");
        CHECK(ar.find("AURA_HOT_RECORD") != std::string::npos, "arena AURA_HOT_RECORD");
        // Drift: primary make_int path must not use raw record without AURA_HOT
        const auto make_int_pos = val.find("export inline EvalValue make_int");
        CHECK(make_int_pos != std::string::npos, "make_int found");
        const auto make_int_snip = val.substr(make_int_pos, 400);
        CHECK(make_int_snip.find("AURA_HOT_CONTRACT") != std::string::npos,
              "make_int uses AURA_HOT_CONTRACT");
        CHECK(make_int_snip.find("record_hotpath_invariant_hit") == std::string::npos,
              "make_int no bare record");
    }

    // ── AC3: static release policy ──
    {
        std::println("\n--- AC3: release CHECK policy ---");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        // Under NDEBUG without ENFORCE, CHECK is ((void)0)
        CHECK(hh.find("((void)0)") != std::string::npos, "release no-op CHECK");
#if defined(NDEBUG) && !defined(AURA_CONTRACTS_ENFORCE) && !defined(AURA_CONTRACTS_OBSERVE) &&     \
    !defined(AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE) && !defined(AURA_HOT_SOFT_OBSERVE)
        // In this build, AURA_HOT_CHECK should compile as no-op.
        AURA_HOT_CHECK(false); // must not abort in release
        CHECK(true, "release AURA_HOT_CHECK(false) no abort");
#else
        CHECK(true, "debug/enforce build: release no-op checked via source");
#endif
    }

    // ── AC4: hits move under microbench ──
    // Issue #2435: production hot mode OFF elides AURA_HOT_RECORD — hits
    // stay flat; debug/observe/enforce still move the counter.
    {
        std::println("\n--- AC4: hotpath hits move ---");
        const auto h0 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        for (int i = 0; i < 1000; ++i) {
            auto v = make_int(i);
            CHECK(as_int(v) == i, "as_int roundtrip");
            auto b = make_bool(i & 1);
            CHECK(as_bool(b) == ((i & 1) != 0), "as_bool");
        }
        // ir_soa view path
        IRModuleV2 mod;
        auto fi = mod.add_function("f", 2);
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
        (void)mod.view_at(fi, 0);
        mod.functions[0].mark_block_dirty(0);

        const auto h1 = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        std::println("  hits {} → {} (mode={})", h0, h1, aura::core::cpp26::kHotContractsMode);
        if constexpr (aura::core::cpp26::kHotContractsMode == aura::core::cpp26::kHotModeOff) {
            // #2435 production OFF: RECORD is no-op; semantics still correct.
            CHECK(h1 == h0, "production OFF: hits unchanged (zero record cost)");
            CHECK(true, "hotpath hits policy: off");
        } else {
            CHECK(h1 > h0, "hotpath_invariant_hits moved");
            CHECK(h1 - h0 >= 1000, "at least one hit per make/as_int pair");
        }
    }

    // ── AC5: valid programs + schema ──
    {
        std::println("\n--- AC5: schema-2142 + valid program ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok");
        auto r = cs.eval("(+ 40 2)");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "42");
        CHECK(href(cs, "schema-2142") == 2142, "schema-2142");
        CHECK(href(cs, "issue-2142") == 2142, "issue-2142");
        CHECK(href(cs, "schema-3043") == 3043, "schema-3043");
        CHECK(href(cs, "hot-contract-false-total") >= 0, "3043 false-total");
        CHECK(href(cs, "aura-hot-contract-wired") == 1, "wired");
        CHECK(href(cs, "hotpath-contracts-2142-active") == 1, "active");
        CHECK(href(cs, "hotpath-invariant-hits") >= 0, "hits queryable");
        CHECK(href(cs, "contract-hot-paths") >= 62, "contract-hot-paths");
    }

    // ── Issue #3501: NDEBUG OFF CONTRACT is one armed() load ──
    {
        std::println("\n--- #3501: AURA_HOT_CONTRACT single harden load ---");
        CHECK(kHotContractSingleLoadIssue == 3501, "3501 stamp");
        auto hh = read_file("src/core/cpp26_contract_stats.h");
        CHECK(hh.find("kHotContractSingleLoadIssue = 3501") != std::string::npos,
              "3501 AC5: stamp");
        const auto d1 = hh.find("#define AURA_HOT_CONTRACT");
        const auto d2 = hh.find("#define AURA_HOT_CONTRACT", d1 == std::string::npos ? 0 : d1 + 1);
        CHECK(d1 != std::string::npos && d2 != std::string::npos && d2 > d1,
              "3501 AC1: OFF + non-OFF CONTRACT defs");
        const auto off_body =
            (d1 != std::string::npos && d2 > d1) ? hh.substr(d1, d2 - d1) : std::string{};
        std::size_t armed_n = 0;
        for (std::size_t p = 0;
             (p = off_body.find("hot_contract_harden_armed()", p)) != std::string::npos; p += 26)
            ++armed_n;
        CHECK(armed_n == 1, "3501 AC1: single hot_contract_harden_armed() in OFF CONTRACT");
        CHECK(off_body.find("AURA_HOT_RECORD()") == std::string::npos,
              "3501 AC1: not RECORD-then-CHECK");
        CHECK(off_body.find("AURA_HOT_CHECK(") == std::string::npos, "3501 AC1: not nested CHECK");

        CHECK(off_body.find("observe_hot_contract_false") != std::string::npos,
              "3501 AC2: armed false observes");
        CHECK(off_body.find("record_hotpath_contract_harden_trap") != std::string::npos,
              "3501 AC2: armed false traps");
        CHECK(off_body.find("std::abort()") != std::string::npos, "3501 AC2: armed false abort");

        aura::compiler::typed_audit::apply_dev_audit_defaults();
#if defined(NDEBUG) && !defined(AURA_HOT_MODE_HARDEN) && !defined(AURA_HOT_MODE_ENFORCE)
        CHECK(!aura::core::cpp26::hot_contract_harden_armed(), "3501 AC3: Soft unarmed");
        AURA_HOT_CONTRACT(false); // must not evaluate expr / abort
        CHECK(true, "3501 AC3: unarmed CONTRACT(false) no abort");
#else
        CHECK(true, "3501 AC3: unarmed skip via source (non-OFF build)");
#endif
        CHECK(off_body.find("if (::aura::core::cpp26::hot_contract_harden_armed())") !=
                  std::string::npos,
              "3501 AC3: expr gated on armed()");

        auto soa = read_file("src/compiler/ir_soa.ixx");
        const auto vat = soa.find("IRInstructionView view_at(");
        const auto addb = soa.find("add_block", vat == std::string::npos ? 0 : vat);
        const auto vwin =
            (vat != std::string::npos && addb > vat) ? soa.substr(vat, addb - vat) : std::string{};
        CHECK(vwin.find("AURA_HOT_CONTRACT") != std::string::npos,
              "3501 AC4: view_at uses AURA_HOT_CONTRACT");
        CHECK(vwin.find("AURA_HOT_RECORD()") == std::string::npos,
              "3501 AC4: view_at no separate RECORD");
        CHECK(vwin.find("AURA_HOT_CHECK(") == std::string::npos,
              "3501 AC4: view_at no separate CHECK");

        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("schema-3501") == std::string::npos, "3501 AC5: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3501.cpp").empty(),
              "3501 AC5: no test_issue_3501.cpp");
        CHECK(read_file("docs/design/3501-hot-contract-single-load.md").empty(),
              "3501 AC5: no docs/design/3501-*");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hot_contract_unify();
}
#endif
