// @category: unit
// @reason: Issue #2410 — meta(NodeTag) bounds-checked; OOB returns sentinel.
//
//   AC1: meta(NodeTag{}) returns well-defined sentinel (no UB)
//   AC2: meta(0xFF) returns sentinel
//   AC3: valid tags 0x01..Class return correct metadata
//   AC4: ASan/TSan clean (no OOB)
//   AC5: static_assert / compile-time validation (source-cite)

#include "test_harness.hpp"

#include <cstdint>
#include <print>

import std;
import aura.core.ast;

namespace {

using aura::ast::is_valid_node_tag;
using aura::ast::kNodeMeta;
using aura::ast::kNodeTagMax;
using aura::ast::meta;
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_node_meta_bounds_2410() {
    std::println("=== Issue #2410: meta(NodeTag) bounds check ===");

    // ── AC1 default-constructed tag = 0 ────────────────────────────
    {
        std::println("\n--- #2410 AC1: meta(NodeTag{{}}) sentinel ---");
        NodeTag zero{};
        CHECK(static_cast<std::uint32_t>(zero) == 0, "AC1: default NodeTag is 0");
        CHECK(!is_valid_node_tag(zero), "AC1: 0 is not a valid tag");
        const auto& m = meta(zero);
        CHECK(m.name == "LiteralInt", "AC1: sentinel name LiteralInt");
        CHECK(m.tag == NodeTag::LiteralInt, "AC1: sentinel tag LiteralInt");
        // Must not crash / must be same as kNodeMeta[0]
        CHECK(&m == &kNodeMeta[0], "AC1: sentinel is kNodeMeta[0] address");
    }

    // ── AC2 high invalid tag ───────────────────────────────────────
    {
        std::println("\n--- #2410 AC2: meta(0xFF) sentinel ---");
        auto bad = static_cast<NodeTag>(0xFF);
        CHECK(!is_valid_node_tag(bad), "AC2: 0xFF invalid");
        const auto& m = meta(bad);
        CHECK(m.name == "LiteralInt", "AC2: sentinel name");
        CHECK(&m == &kNodeMeta[0], "AC2: sentinel address");
        // Just past Class
        auto past = static_cast<NodeTag>(kNodeTagMax + 1);
        CHECK(!is_valid_node_tag(past), "AC2: Class+1 invalid");
        CHECK(&meta(past) == &kNodeMeta[0], "AC2: Class+1 sentinel");
    }

    // ── AC3 all valid tags ─────────────────────────────────────────
    {
        std::println("\n--- #2410 AC3: valid tags preserve metadata ---");
        CHECK(kNodeTagMax == static_cast<std::uint32_t>(NodeTag::Class), "AC3: kNodeTagMax");
        CHECK(kNodeMeta.size() == static_cast<std::size_t>(NodeTag::Class),
              "AC3: table size == Class");
        CHECK(meta(NodeTag::LiteralInt).has_int, "AC3: LiteralInt has_int");
        CHECK(meta(NodeTag::Call).fixed_children == 1, "AC3: Call fixed_children");
        CHECK(meta(NodeTag::Call).has_var_children, "AC3: Call has_var_children");
        CHECK(meta(NodeTag::Lambda).has_params, "AC3: Lambda has_params");
        CHECK(meta(NodeTag::IfExpr).fixed_children == 3, "AC3: IfExpr children");
        CHECK(meta(NodeTag::LiteralFloat).has_float, "AC3: LiteralFloat has_float");
        CHECK(meta(NodeTag::Class).name == "Class", "AC3: Class name");
        CHECK(meta(NodeTag::Class).tag == NodeTag::Class, "AC3: Class tag");
        CHECK(meta(NodeTag::MacroDef).name == "MacroDef", "AC3: MacroDef");
        CHECK(meta(NodeTag::DefineType).name == "DefineType", "AC3: DefineType");
        // Gap entry still addressable as raw index 0x0C (see #2411 is_gap)
        CHECK(is_valid_node_tag(static_cast<NodeTag>(0x0C)), "AC3: gap index in range");
        CHECK(meta(static_cast<NodeTag>(0x0C)).name == "<gap>", "AC3: gap name");
        CHECK(meta(static_cast<NodeTag>(0x0C)).is_gap, "AC3: gap is_gap");
        // Spot-check index mapping for Class
        CHECK(&meta(NodeTag::Class) == &kNodeMeta[static_cast<std::size_t>(NodeTag::Class) - 1],
              "AC3: Class maps to last table slot");
    }

    // ── AC4 ASan-safe: no OOB on random invalid ───────────────────
    {
        std::println("\n--- #2410 AC4: fuzz invalid tags (no OOB) ---");
        for (std::uint32_t v : {0u, 0x24u, 0x80u, 0xFEu, 0xFFu, 0x100u}) {
            auto t = static_cast<NodeTag>(v);
            if (v >= 1 && v <= kNodeTagMax)
                continue;
            CHECK(!is_valid_node_tag(t), "AC4: invalid tag rejected");
            CHECK(&meta(t) == &kNodeMeta[0], "AC4: invalid → sentinel");
        }
    }

    // ── AC5 compile-time table size (runtime anchors + helpers) ───
    {
        std::println("\n--- #2410 AC5: table size / is_valid_node_tag ---");
        CHECK(kNodeMeta.size() == static_cast<std::size_t>(NodeTag::Class),
              "AC5: table size == Class");
        CHECK(is_valid_node_tag(NodeTag::LiteralInt), "AC5: LiteralInt valid");
        CHECK(is_valid_node_tag(NodeTag::Class), "AC5: Class valid");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_node_meta_bounds_2410();
}
#endif
