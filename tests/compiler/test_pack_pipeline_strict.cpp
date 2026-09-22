// @category: unit
// @reason: Issue #3627 — pack-binary binding fixture. Compiled WITH
// AURA_PRODUCTION_PACK=1 (the same define as the production `aura`
// target), so apply_pipeline_strict_defaults is exercised through the
// pack arm: env unset → Forbidden regardless of dev_sandbox_off, while
// AURA_PIPELINE_STRICT operator overrides still win. The non-pack Soft
// face lives in test_tree_walker_fallback_strict.cpp (sandbox=off →
// Allow, #2213 AC2).

#include "compiler/pipeline_policy.hh"
#include "core/cpp26_contract_stats.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using aura::compiler::apply_pipeline_strict_defaults;
using aura::compiler::production_pipeline_strict;
using aura::compiler::reset_tree_walker_fallback_policy_for_test;
using aura::compiler::tree_walker_fallback_disposition;
using aura::compiler::tree_walker_fallback_policy;
using aura::compiler::TreeWalkerFallbackDisposition;
using aura::compiler::TreeWalkerFallbackPolicy;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, label)                                                                         \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::printf("  FAIL: %s\n", label);                                                    \
        }                                                                                          \
    } while (0)

void ac3627_default_and_reset() {
    std::printf("\n--- 3627 AC0: static default + reset_for_test stay Allow ---\n");
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,
          "static atomic default Allow before any apply");
    reset_tree_walker_fallback_policy_for_test();
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,
          "reset_for_test still Allow (unit Soft path unchanged)");
}

void ac3627_pack_env_unset() {
    std::printf("\n--- 3627 AC1: pack + env unset -> Forbidden, never TakeWalker ---\n");
    (void)unsetenv("AURA_PIPELINE_STRICT");
    (void)unsetenv("AURA_SANDBOX");
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "pack + sandbox=off + env unset -> Forbidden (#3627 AC1)");
    CHECK(production_pipeline_strict(), "pack binding implies pipeline strict");
    CHECK(tree_walker_fallback_disposition(true) == TreeWalkerFallbackDisposition::HardError,
          "needs -> HardError, never TakeWalker");
    CHECK(tree_walker_fallback_disposition(false) == TreeWalkerFallbackDisposition::ContinueIr,
          "happy path -> ContinueIr (policy load only, no extra branch)");
}

void ac3627_pack_ignores_dev_flag() {
    std::printf("\n--- 3627 AC2: pack ignores dev_sandbox_off entirely ---\n");
    (void)unsetenv("AURA_PIPELINE_STRICT");
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/false);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "pack + dev_off=false -> Forbidden (same strict binding)");
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "pack + dev_off=true -> Forbidden (sandbox knob does not unlock)");
}

void ac3627_operator_wins() {
    std::printf("\n--- 3627 AC3: AURA_PIPELINE_STRICT operator override wins in pack ---\n");
    (void)setenv("AURA_PIPELINE_STRICT", "allow", 1);
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,
          "env allow -> operator Soft in pack (#3627 AC3)");
    CHECK(tree_walker_fallback_disposition(true) == TreeWalkerFallbackDisposition::TakeWalker,
          "operator allow -> TakeWalker (explicit, not silent)");

    (void)setenv("AURA_PIPELINE_STRICT", "force-soa", 1);
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::ForceSoa,
          "env force-soa -> ForceSoa in pack");
    CHECK(tree_walker_fallback_disposition(true) == TreeWalkerFallbackDisposition::ContinueIr,
          "ForceSoa -> ContinueIr (no silent walker)");

    (void)setenv("AURA_PIPELINE_STRICT", "1", 1);
    apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden,
          "env 1 -> Forbidden in pack");
    (void)unsetenv("AURA_PIPELINE_STRICT");
    reset_tree_walker_fallback_policy_for_test();
}

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

void ac3666_pack_hot_check_compile_armed() {
    std::printf("\n--- #3666 AC1: pack CHECK is compile-true Harden, no cache load ---\n");
    CHECK(aura::core::cpp26::kHotContractProductionPackIssue == 3666, "3666 pack stamp");
    CHECK(aura::core::cpp26::kHotContractProductionPackCompileArmed,
          "3666 AC1: pack compile-armed constexpr");
    CHECK(aura::core::cpp26::hot_contract_harden_armed(),
          "3666 AC1: pack armed() is constant true");
    aura::core::cpp26::note_hot_contract_harden_armed(false);
    CHECK(aura::core::cpp26::hot_contract_harden_armed(),
          "3666 AC4: pack stays armed after note(false)");
    AURA_HOT_CHECK(true);
    AURA_HOT_CONTRACT(true);
    CHECK(true, "3666 AC1: pack CHECK/CONTRACT true does not abort");

    auto hh = read_file("src/core/cpp26_contract_stats.h");
    const auto pack = hh.find("#if defined(AURA_HOT_MODE_OFF) && defined(AURA_PRODUCTION_PACK)");
    CHECK(pack != std::string::npos, "3666 AC1: pack redefine present");
    const auto pack_end = hh.find("#endif", pack == std::string::npos ? 0 : pack);
    const auto pwin = (pack != std::string::npos && pack_end > pack)
                          ? hh.substr(pack, pack_end - pack)
                          : std::string{};
    CHECK(pwin.find("hot_contract_harden_armed()") == std::string::npos,
          "3666 AC1: pack macros do not call armed()");
    CHECK(pwin.find("hot_contract_harden_armed_cache") == std::string::npos,
          "3666 AC1: pack macros do not load cache");
    auto val = read_file("src/compiler/value.ixx");
    CHECK(val.find("AURA_HOT_CONTRACT(is_int(v))") != std::string::npos,
          "3666 AC1: as_int call site unchanged");
}

void ac3702_pack_happy_no_record() {
    std::printf("\n--- #3702 AC1: pack CONTRACT happy path has no sampled RECORD ---\n");
    CHECK(aura::core::cpp26::kHotContractPackHappyNoRecordIssue == 3702, "3702 stamp");
    auto hh = read_file("src/core/cpp26_contract_stats.h");
    CHECK(hh.find("kHotContractPackHappyNoRecordIssue = 3702") != std::string::npos,
          "3702: header stamp");
    const auto pack = hh.find("#if defined(AURA_HOT_MODE_OFF) && defined(AURA_PRODUCTION_PACK)");
    CHECK(pack != std::string::npos, "3702: pack redefine present");
    const auto pack_end = hh.find("#endif", pack == std::string::npos ? 0 : pack);
    const auto pwin = (pack != std::string::npos && pack_end > pack)
                          ? hh.substr(pack, pack_end - pack)
                          : std::string{};
    const auto cpos = pwin.find("#define AURA_HOT_CONTRACT");
    CHECK(cpos != std::string::npos, "3702 AC1: pack CONTRACT");
    const auto cbody = pwin.substr(cpos);
    CHECK(cbody.find("record_hotpath_invariant_hit_sampled") == std::string::npos,
          "3702 AC1: pack CONTRACT has no sampled RECORD");
    CHECK(cbody.find("AURA_HOT_RECORD") == std::string::npos,
          "3702 AC1: pack CONTRACT does not RECORD on success");
    CHECK(cbody.find("[[unlikely]]") != std::string::npos, "3702 AC1: unlikely abort");
    CHECK(cbody.find("std::abort()") != std::string::npos, "3702 AC2: false still abort");
    CHECK(cbody.find("record_hotpath_contract_harden_trap") != std::string::npos,
          "3702 AC2: false still trap counter");
    auto val = read_file("src/compiler/value.ixx");
    CHECK(val.find("AURA_HOT_CONTRACT(is_int(v))") != std::string::npos,
          "3702 AC1: as_int still CONTRACT");
    auto soa = read_file("src/compiler/ir_soa.ixx");
    const auto vat = soa.find("IRInstructionView view_at(");
    const auto addb = soa.find("add_block", vat == std::string::npos ? 0 : vat);
    const auto vwin =
        (vat != std::string::npos && addb > vat) ? soa.substr(vat, addb - vat) : std::string{};
    CHECK(vwin.find("AURA_HOT_CONTRACT") != std::string::npos, "3702 AC4: view_at same CONTRACT");
    CHECK(hh.find("schema-3702") == std::string::npos, "3702 AC5: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3702.cpp").empty(), "3702 AC5: no invent");
    CHECK(read_file("docs/design/3702-pack-happy-no-record.md").empty(),
          "3702 AC5: no docs/design");

    const auto h0 = aura::core::cpp26::hotpath_invariant_hits_total.load(std::memory_order_relaxed);
    for (int i = 0; i < 1024; ++i)
        AURA_HOT_CONTRACT(true);
#if defined(AURA_HOT_MODE_OFF)
    CHECK(aura::core::cpp26::hotpath_invariant_hits_total.load(std::memory_order_relaxed) == h0,
          "3702 AC1: pack happy path RECORD count stays 0");
#else
    (void)h0;
    CHECK(true, "3702 AC1: source-cite (Debug pack is ENFORCE, not NDEBUG OFF)");
#endif
}

static std::size_t count_hot_unbox_macros(std::string_view win) {
    std::size_t n = 0;
    for (const char* key : {"AURA_HOT_CONTRACT(", "AURA_HOT_CHECK("}) {
        for (std::size_t p = 0; (p = win.find(key, p)) != std::string::npos; ++p)
            ++n;
    }
    return n;
}

void ac3770_one_contract_per_unbox() {
    std::printf("\n--- #3770 AC1: as_int / as_bool / as_string one hot contract ---\n");
    auto val = read_file("src/compiler/value.ixx");
    CHECK(val.find("Issue #3770") != std::string::npos, "3770 AC1: cite");
    const auto i0 = val.find("export inline std::int64_t as_int(");
    const auto i1 = val.find("export inline EvalValue make_bool(");
    const auto b0 = val.find("export inline bool as_bool(");
    const auto b1 = val.find("export inline EvalValue make_void(");
    const auto s0 = val.find("export inline std::uint64_t as_string_idx(");
    const auto s1 = val.find("export inline EvalValue make_string_v2(");
    const auto iwin =
        (i0 != std::string::npos && i1 > i0) ? val.substr(i0, i1 - i0) : std::string{};
    const auto bwin =
        (b0 != std::string::npos && b1 > b0) ? val.substr(b0, b1 - b0) : std::string{};
    const auto swin =
        (s0 != std::string::npos && s1 > s0) ? val.substr(s0, s1 - s0) : std::string{};
    CHECK(count_hot_unbox_macros(iwin) == 1, "3770 AC1: as_int exactly one CONTRACT/CHECK");
    CHECK(iwin.find("AURA_HOT_CONTRACT(is_int(v))") != std::string::npos,
          "3770 AC1: as_int CONTRACT(is_int)");
    CHECK(iwin.find("AURA_HOT_CHECK") == std::string::npos, "3770 AC1: as_int no stacked CHECK");
    CHECK(count_hot_unbox_macros(bwin) == 1, "3770 AC1: as_bool exactly one CONTRACT/CHECK");
    CHECK(count_hot_unbox_macros(swin) == 1, "3770 AC1: as_string_idx exactly one CONTRACT/CHECK");
    CHECK(swin.find("AURA_HOT_CONTRACT(is_string(v))") != std::string::npos,
          "3770 AC1: as_string_idx CONTRACT(is_string)");
    CHECK(swin.find("AURA_HOT_CHECK") == std::string::npos,
          "3770 AC1: as_string_idx no stacked CHECK");

    auto hh = read_file("src/core/cpp26_contract_stats.h");
    const auto pack = hh.find("#if defined(AURA_HOT_MODE_OFF) && defined(AURA_PRODUCTION_PACK)");
    const auto pack_end = hh.find("#endif", pack == std::string::npos ? 0 : pack);
    const auto pwin = (pack != std::string::npos && pack_end > pack)
                          ? hh.substr(pack, pack_end - pack)
                          : std::string{};
    const auto cpos = pwin.find("#define AURA_HOT_CONTRACT");
    const auto cbody = (cpos != std::string::npos) ? pwin.substr(cpos) : std::string{};
    CHECK(cbody.find("record_hotpath_invariant_hit_sampled") == std::string::npos,
          "3770 AC2: pack happy path still no sampled RECORD");
    CHECK(iwin.find("AURA_HOT_RECORD") == std::string::npos, "3770 AC2: as_int no RECORD");
    CHECK(cbody.find("std::abort()") != std::string::npos, "3770 AC3: ENFORCE/pack still abort");
    CHECK(val.find("schema-3770") == std::string::npos, "3770 AC3: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3770.cpp").empty(), "3770 AC3: no invent");
    CHECK(read_file("docs/design/3770-as-int-one-contract.md").empty(), "3770 AC3: no docs/design");
}

static std::size_t count_substr(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = 0; (p = s.find(needle, p)) != std::string::npos; ++p)
        ++n;
    return n;
}

void ac4007_pack_deletes_aos_factory() {
    std::printf("\n--- #4007: pack deletes AoS named factory; residual call sites 0 ---\n");
    auto opt = read_file("src/compiler/optimization_passes.ixx");
    CHECK(opt.find("kProductionSoaFactoryIssue = 4007") != std::string::npos, "4007: stamp");
    const auto pack = opt.find("#if defined(AURA_PRODUCTION_PACK)");
    CHECK(pack != std::string::npos, "4007: pack ifdef");
    const auto pack_end = opt.find("#else", pack == std::string::npos ? 0 : pack);
    const auto pwin = (pack != std::string::npos && pack_end > pack)
                          ? opt.substr(pack, pack_end - pack)
                          : std::string{};
    CHECK(pwin.find("run_default_optimization_pipeline(aura::ir::IRModule&) = delete") !=
              std::string::npos,
          "4007: AoS factory = delete under pack");
    CHECK(opt.find("run_default_optimization_pipeline(::aura::compiler::IRModuleV2&") !=
              std::string::npos,
          "4007: V2 factory overload present");
    CHECK(opt.find("run_production_soa_pure_wrap_pack") != std::string::npos,
          "4007: factory is PureWrap pack");
    auto svc = read_file("src/compiler/service.ixx");
    auto lowering = read_file("src/compiler/lowering_impl.cpp");
    auto impls = read_file("src/compiler/pass_impls.ixx");
    CHECK(count_substr(svc, "run_default_optimization_pipeline(") == 0,
          "4007: service residual AoS factory calls = 0");
    CHECK(count_substr(lowering, "run_default_optimization_pipeline(") == 0,
          "4007: lowering residual AoS factory calls = 0");
    CHECK(count_substr(impls, "run_default_optimization_pipeline(") == 0,
          "4007: pass_impls residual AoS factory calls = 0");
    CHECK(svc.find("kDefaultOptPipelineAosDeleted") != std::string::npos,
          "4007: production TU static_assert");
    CHECK(opt.find("schema-4007") == std::string::npos, "4007: no new query key");
    CHECK(read_file("tests/compiler/test_issue_4007.cpp").empty(), "4007: no invent");
    CHECK(read_file("docs/design/4007-soa-factory.md").empty(), "4007: no docs/design");
}

} // namespace

int main() {
    std::printf("=== Issue #3627: pack-binary binding (AURA_PRODUCTION_PACK fixture) ===\n");
    ac3627_default_and_reset();
    ac3627_pack_env_unset();
    ac3627_pack_ignores_dev_flag();
    ac3627_operator_wins();
    ac3666_pack_hot_check_compile_armed();
    ac3702_pack_happy_no_record();
    ac3770_one_contract_per_unbox();
    ac4007_pack_deletes_aos_factory();
    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
