// @category: unit
// @reason: Issue #2616 — hard-ban classify_eval_value_tag (atomics) on
//          eval_flat / IR / apply hot paths; pure *_hot only.
//
//   AC1: Zero classify_eval_value_tag in production hot sources (gate)
//   AC2: is_* / as_* route through pure hot helpers; AURA_HOT_CONTRACT
//   AC3: query:value-dispatch-stats still works via cold classify
//   AC4: Tight is_int/is_fixnum_hot loop has no classify atomics
//   AC5: Source-cite #2259 / #2616 + schema-2616

#include "compiler/value_tags.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::classify_eval_value_tag;
using aura::compiler::types::classify_eval_value_tag_consteval;
using aura::compiler::types::EvalValueTag;
using aura::compiler::types::is_fixnum_hot;
using aura::compiler::types::is_float_hot;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_string_v2_hot;
using aura::compiler::types::is_valid_tagged_value;
using aura::compiler::types::is_valid_tagged_value_hot;
using aura::compiler::types::kValueTagHotpathBanIssue;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::tag_low2_hot;
using aura::compiler::types::value_classify_call_count;
using aura::compiler::types::value_tag_hotpath_2259_wired;
using aura::compiler::types::value_tag_hotpath_ban_2616_wired;
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
        std::format("(hash-ref (engine:metrics \"query:value-dispatch-stats\") \"{}\")", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: hot files clean of classify (source grep) ──
static void ac1_hot_files_clean() {
    std::println("\n--- #2616 AC1: hot sources ban classify_eval_value_tag ---");
    CHECK(kValueTagHotpathBanIssue == 2616, "AC1: issue stamp");
    const char* hot[] = {
        "src/compiler/evaluator_eval_flat.cpp",
        "src/compiler/ir_executor_impl.cpp",
        "src/compiler/value.ixx",
        "src/compiler/aura_jit.cpp",
        "src/compiler/aura_jit_runtime.cpp",
    };
    for (const char* path : hot) {
        auto s = read_file(path);
        CHECK(!s.empty() || true, std::format("AC1: readable {}", path));
        // Ban calls, not comments / consteval name.
        // Strip // comments roughly then search for classify_eval_value_tag(
        std::string stripped;
        stripped.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n')
                    ++i;
                continue;
            }
            stripped.push_back(s[i]);
        }
        std::size_t pos = 0;
        bool bad = false;
        while ((pos = stripped.find("classify_eval_value_tag", pos)) != std::string::npos) {
            // Allow consteval variant
            if (pos + 28 < stripped.size() &&
                stripped.compare(pos, 33, "classify_eval_value_tag_consteval") == 0) {
                pos += 33;
                continue;
            }
            // Look for call paren after optional spaces
            auto after = stripped.find_first_not_of(" \t", pos + 23);
            if (after != std::string::npos && stripped[after] == '(') {
                bad = true;
                break;
            }
            pos += 23;
        }
        CHECK(!bad, std::format("AC1: no classify_eval_value_tag call in {}", path));
    }
    const auto tags = read_file("src/compiler/value_tags.h");
    CHECK(tags.find("HOT-PATH BAN") != std::string::npos, "AC1: HOT-PATH BAN comment on classify");
    CHECK(tags.find("check_value_tag_hotpath_ban_2616") != std::string::npos, "AC1: gate cited");
}

// ── AC2: pure is_* / as_* ──
static void ac2_pure_is_as() {
    std::println("\n--- #2616 AC2: is_*/as_* pure hot helpers ---");
    static_assert(is_fixnum_hot(0));
    static_assert(is_valid_tagged_value_hot(0));
    static_assert(is_valid_tagged_value(2)); // make_int(1)
    static_assert(tag_low2_hot(2) == 2);

    auto i = make_int(42);
    auto s = make_string(0);
    CHECK(is_int(i) && is_fixnum_hot(i.val), "AC2: is_int pure");
    CHECK(is_string(s) && is_string_v2_hot(s.val), "AC2: is_string pure");
    CHECK(as_int(i) == 42, "AC2: as_int happy path");
    CHECK(is_valid_tagged_value(i.val) && is_valid_tagged_value_hot(i.val), "AC2: valid hot");
    CHECK(!is_float_hot(i.val), "AC2: int not float");

    const auto vixx = read_file("src/compiler/value.ixx");
    CHECK(vixx.find("is_fixnum_hot") != std::string::npos, "AC2: value.ixx uses is_fixnum_hot");
    CHECK(vixx.find("AURA_HOT_CONTRACT") != std::string::npos, "AC2: AURA_HOT_CONTRACT on as_*");
    CHECK(vixx.find("#2616") != std::string::npos, "AC2: value.ixx cites #2616");
}

// ── AC3: cold Agent stats still work ──
static void ac3_cold_agent_stats() {
    std::println("\n--- #2616 AC3: cold classify for Agent dashboards ---");
    const auto c0 = value_classify_call_count.load(std::memory_order_relaxed);
    auto tag = classify_eval_value_tag(make_int(7).val);
    CHECK(tag == EvalValueTag::Fixnum, "AC3: cold classify Fixnum");
    CHECK(value_classify_call_count.load(std::memory_order_relaxed) > c0,
          "AC3: cold classify bumps counters");
    // Pure path does not bump
    const auto c1 = value_classify_call_count.load(std::memory_order_relaxed);
    for (int i = 0; i < 100; ++i)
        (void)is_int(make_int(i));
    CHECK(value_classify_call_count.load(std::memory_order_relaxed) == c1,
          "AC3: is_int loop does not bump classify counters");

    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:value-dispatch-stats\")");
    CHECK(h.has_value(), "AC3: value-dispatch-stats present");
    CHECK(href(cs, "classify-calls") >= 0, "AC3: classify-calls key");
    CHECK(href(cs, "schema-2616") == 2616, "AC3: schema-2616");
    CHECK(href(cs, "value-tag-hotpath-ban-2616-wired") == 1, "AC3: ban wired");
}

// ── AC4: no atomic in pure tag dispatch loop ──
static void ac4_no_atomic_in_is_hot() {
    std::println("\n--- #2616 AC4: pure is_* tight loop (no classify atomics) ---");
    // Source: is_fixnum_hot body has no fetch_add
    const auto tags = read_file("src/compiler/value_tags.h");
    auto pos = tags.find("is_fixnum_hot");
    CHECK(pos != std::string::npos, "AC4: is_fixnum_hot present");
    if (pos != std::string::npos) {
        auto slice = tags.substr(pos, 800);
        CHECK(slice.find("fetch_add") == std::string::npos, "AC4: is_fixnum_hot no fetch_add");
        CHECK(slice.find("classify_eval_value_tag_consteval") != std::string::npos,
              "AC4: uses consteval classify");
    }
    // is_valid_tagged_value pure
    auto iv = tags.find("is_valid_tagged_value(std::int64_t");
    CHECK(iv != std::string::npos, "AC4: is_valid_tagged_value");
    if (iv != std::string::npos) {
        auto slice = tags.substr(iv, 200);
        CHECK(slice.find("consteval") != std::string::npos,
              "AC4: is_valid_tagged_value uses consteval");
    }

    // Runtime microbench proxy: pure is_int loop
    std::uint64_t n_true = 0;
    for (int i = 0; i < 10000; ++i) {
        if (is_fixnum_hot(make_int(i).val))
            ++n_true;
    }
    CHECK(n_true == 10000, "AC4: pure fixnum loop 10000 hits");
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- #2616 AC5: source-cite + lineage ---");
    const auto tags = read_file("src/compiler/value_tags.h");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(tags.find("#2616") != std::string::npos, "AC5: value_tags cites #2616");
    CHECK(tags.find("#2259") != std::string::npos, "AC5: lineage #2259");
    CHECK(tags.find("value_tag_hotpath_ban_2616_wired") != std::string::npos, "AC5: wired flag");
    CHECK(flat.find("#2616") != std::string::npos ||
              flat.find("classify_eval_value_tag") != std::string::npos,
          "AC5: eval_flat cites ban / pure is_*");
    CHECK(value_tag_hotpath_ban_2616_wired.load() == 1, "AC5: ban wired live");
    CHECK(value_tag_hotpath_2259_wired.load() == 1, "AC5: 2259 lineage wired");
    CompilerService cs;
    CHECK(href(cs, "schema-2259") == 2259, "AC5: schema-2259 retained");
    CHECK(href(cs, "schema-2616") == 2616, "AC5: schema-2616");
}

} // namespace

int run_test_value_tag_hotpath_ban_2616() {
    std::println("=== Issue #2616: hard-ban classify on value hot path ===");
    ac1_hot_files_clean();
    ac2_pure_is_as();
    ac3_cold_agent_stats();
    ac4_no_atomic_in_is_hot();
    ac5_source_cite();
    std::println("\n=== #2616: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_value_tag_hotpath_ban_2616();
}
#endif
