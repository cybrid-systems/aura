// @category: unit
// @reason: pure C++ — NodeTag enum + kNodeMeta + tag_dispatch
// test_issue_310.cpp — Verify Issue #310 acceptance criteria
// ("feat(ast): add minimal SV structural NodeTags (Interface,
//  Modport) to NodeTag enum").
//
// P3 / good-first-issue. The PR adds 2 new NodeTag values
// (Interface, Modport) and corresponding kNodeMeta entries,
// plus a mirrored update to src/reflect/tag_dispatch.hh's Tag
// enum + TAG_COUNT constant. No behavior change.
//
// ACs:
//   AC1: 编译通过，无 warning
//        (verified by the build; this test doesn't check
//        for warnings directly)
//   AC2: NodeTag::Interface / NodeTag::Modport 在测试中
//        正确使用
//        (direct enum checks below)
//   AC3: kNodeMeta 表保持一致性
//        (array size matches the highest tag value; meta
//        accessor returns the correct entry for both new
//        tags)
//   AC4: 现有所有测试通过
//        (covered by the existing ctest suite; this test
//        only adds new checks, doesn't modify any existing
//        test)


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

#include "reflect/tag_dispatch.hh"

namespace aura_issue_310_detail {
#define CHECK_EQ_LOCAL(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (!(_a == _b)) { \
        std::println("  FAIL: {} (got {} expected {} line {})", msg, _a, _b, __LINE__); \
        ++g_failed; \
    } else { \
        std::println("  PASS: {}", msg); \
        ++g_passed; \
    } \
} while (0)

// ═══════════════════════════════════════════════════════════════
// AC2: new tags are usable as enum values
// ═══════════════════════════════════════════════════════════════

bool test_new_tags_usable() {
    std::println("\n--- AC2: NodeTag::Interface / NodeTag::Modport usable ---");
    using aura::ast::NodeTag;
    // Store in a variable to confirm the enum values are real.
    NodeTag iface = NodeTag::Interface;
    NodeTag mport = NodeTag::Modport;
    CHECK(iface != NodeTag::LiteralInt, "Interface is distinct from LiteralInt");
    CHECK(mport != NodeTag::LiteralInt, "Modport is distinct from LiteralInt");
    CHECK(iface != mport, "Interface is distinct from Modport");
    // Tag values per the issue spec.
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(iface), std::uint32_t{0x1B},
             "Interface tag value is 0x1B");
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(mport), std::uint32_t{0x1C},
             "Modport tag value is 0x1C");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: kNodeMeta table is consistent
// ═══════════════════════════════════════════════════════════════

bool test_kNodeMeta_consistency() {
    std::println("\n--- AC3: kNodeMeta table consistency ---");
    using aura::ast::kNodeMeta;
    using aura::ast::meta;
    using aura::ast::NodeTag;
    // Array size matches the highest tag value: tags go from
    // 0x01 to 0x1C, so the array should have 0x1C = 28
    // entries.
    CHECK_EQ_LOCAL(kNodeMeta.size(), std::size_t{28},
             "kNodeMeta has 28 entries (tags 0x01-0x1C)");
    // meta() returns the correct entries for both new tags.
    auto mi = meta(NodeTag::Interface);
    CHECK_EQ_LOCAL(mi.name, std::string_view{"Interface"},
             "meta(Interface).name is \"Interface\"");
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(mi.tag), std::uint32_t{0x1B},
             "meta(Interface).tag is 0x1B");
    CHECK(mi.has_var_children,
          "Interface has var_children (declarative body)");
    auto mm = meta(NodeTag::Modport);
    CHECK_EQ_LOCAL(mm.name, std::string_view{"Modport"},
             "meta(Modport).name is \"Modport\"");
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(mm.tag), std::uint32_t{0x1C},
             "meta(Modport).tag is 0x1C");
    CHECK(mm.has_var_children,
          "Modport has var_children (port direction list)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3 (bonus): tag_dispatch.hh mirrors the NodeTag enum
// ═══════════════════════════════════════════════════════════════

bool test_tag_dispatch_mirror() {
    std::println("\n--- AC3 (bonus): tag_dispatch.hh mirror ---");
    using aura::reflect::tag_dispatch::Tag;
    CHECK_EQ_LOCAL(static_cast<std::uint8_t>(Tag::Interface), std::uint8_t{0x1B},
             "tag_dispatch::Tag::Interface is 0x1B");
    CHECK_EQ_LOCAL(static_cast<std::uint8_t>(Tag::Modport), std::uint8_t{0x1C},
             "tag_dispatch::Tag::Modport is 0x1C");
    // TAG_COUNT is one past the highest tag value.
    CHECK_EQ_LOCAL(static_cast<std::uint8_t>(Tag::TAG_COUNT), std::uint8_t{0x1D},
             "tag_dispatch::TAG_COUNT is 0x1D (one past max)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #310 (SV structural NodeTags) ═══\n");
    test_new_tags_usable();
    test_kNodeMeta_consistency();
    test_tag_dispatch_mirror();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_310_detail

int aura_issue_310_run() { return aura_issue_310_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_310_run(); }
#endif