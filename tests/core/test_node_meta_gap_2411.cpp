// @category: unit
// @reason: Issue #2411 — kNodeMeta gap entry is_gap + tag 0x0C; full tag/name check.
//
//   AC1: gap entry tag is 0x0C sentinel and is_gap == true
//   AC2: validate_node_meta covers all non-gap tag/name consistency (source-cite)
//   AC3: meta(LiteralInt) is the real 0x01 entry (not the gap)
//   AC4: valid tags unchanged (name/tag/flags spot-check)
//   AC5: ASan/TSan clean (no OOB; addressable gap)

#include "test_harness.hpp"

#include "reflect/node_tag_names.hh"

#include <cstdint>
#include <print>

import std;
import aura.core.ast;

namespace {

using aura::ast::is_valid_node_tag;
using aura::ast::kNodeMeta;
using aura::ast::kNodeTagGapIndex;
using aura::ast::kNodeTagMax;
using aura::ast::kNodeTagNames;
using aura::ast::meta;
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_node_meta_gap_2411() {
    std::println("=== Issue #2411: kNodeMeta gap is_gap + full consistency ===");

    // ── AC1 gap sentinel ───────────────────────────────────────────
    {
        std::println("\n--- #2411 AC1: gap is_gap + tag 0x0C ---");
        CHECK(kNodeTagGapIndex == 11, "AC1: gap index 11");
        const auto& g = kNodeMeta[kNodeTagGapIndex];
        CHECK(g.is_gap, "AC1: is_gap true");
        CHECK(g.tag == static_cast<NodeTag>(0x0C), "AC1: tag is 0x0C");
        CHECK(g.name == "<gap>", "AC1: name <gap>");
        CHECK(g.tag != NodeTag::LiteralInt, "AC1: not LiteralInt spoof");
        CHECK(&meta(static_cast<NodeTag>(0x0C)) == &g, "AC1: meta(0x0C) is gap slot");
        CHECK(meta(static_cast<NodeTag>(0x0C)).is_gap, "AC1: meta(0x0C).is_gap");
    }

    // ── AC2 full-table tag/name (runtime mirror of consteval) ─────
    {
        std::println("\n--- #2411 AC2: all non-gap tag/name consistency ---");
        std::size_t gap_count = 0;
        for (std::size_t i = 0; i < kNodeMeta.size(); ++i) {
            const auto& e = kNodeMeta[i];
            if (i == kNodeTagGapIndex) {
                CHECK(e.is_gap, "AC2: only gap has is_gap");
                ++gap_count;
                continue;
            }
            CHECK(!e.is_gap, "AC2: non-gap is_gap false");
            CHECK(e.tag == static_cast<NodeTag>(i + 1), "AC2: tag == i+1");
            CHECK(e.name == kNodeTagNames[i], "AC2: name matches table");
        }
        CHECK(gap_count == 1, "AC2: exactly one gap");
    }

    // ── AC3 LiteralInt is not the gap ──────────────────────────────
    {
        std::println("\n--- #2411 AC3: meta(LiteralInt) is real 0x01 entry ---");
        const auto& li = meta(NodeTag::LiteralInt);
        CHECK(!li.is_gap, "AC3: LiteralInt not gap");
        CHECK(li.tag == NodeTag::LiteralInt, "AC3: tag LiteralInt");
        CHECK(li.name == "LiteralInt", "AC3: name LiteralInt");
        CHECK(li.has_int, "AC3: has_int");
        CHECK(&li == &kNodeMeta[0], "AC3: address is slot 0");
        CHECK(&li != &kNodeMeta[kNodeTagGapIndex], "AC3: not gap address");
    }

    // ── AC4 valid tags unchanged ───────────────────────────────────
    {
        std::println("\n--- #2411 AC4: valid tags preserve metadata ---");
        CHECK(meta(NodeTag::Call).fixed_children == 1, "AC4: Call fixed_children");
        CHECK(meta(NodeTag::Call).has_var_children, "AC4: Call has_var_children");
        CHECK(!meta(NodeTag::Call).is_gap, "AC4: Call not gap");
        CHECK(meta(NodeTag::Lambda).has_params, "AC4: Lambda has_params");
        CHECK(meta(NodeTag::MacroDef).name == "MacroDef", "AC4: MacroDef");
        CHECK(meta(NodeTag::DefineType).name == "DefineType", "AC4: DefineType");
        CHECK(meta(NodeTag::Class).tag == NodeTag::Class, "AC4: Class tag");
        CHECK(meta(NodeTag::Class).name == "Class", "AC4: Class name");
        CHECK(kNodeMeta.size() == static_cast<std::size_t>(kNodeTagMax), "AC4: size");
        CHECK(is_valid_node_tag(NodeTag::LiteralInt), "AC4: LiteralInt range-valid");
    }

    // ── AC5 addressable without crash ──────────────────────────────
    {
        std::println("\n--- #2411 AC5: gap addressable / no OOB ---");
        for (std::size_t i = 0; i < kNodeMeta.size(); ++i) {
            (void)kNodeMeta[i].is_gap;
            (void)kNodeMeta[i].name;
        }
        CHECK(true, "AC5: full table walk");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_node_meta_gap_2411();
}
#endif
