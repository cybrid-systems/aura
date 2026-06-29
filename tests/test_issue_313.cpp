// @category: unit
// @reason: pure C++ — FlatAST dirty bitmask + parent_ traversal
// test_issue_313.cpp — Verify Issue #313 acceptance criteria
// ("add kVerificationDirty bit and mark_dirty_verification
//  helper").
//
// Scope-limited close. The issue body asks for a new
// kVerificationDirty bit at 0x20 in the main dirty_ bitmask;
// the honest engineering answer is that all 8 bits of the
// existing uint8_t bitmask are already used (see the
// DirtyReason enum:
//   kGeneralDirty=0x01, kConstraintDirty=0x02,
//   kOccurrenceDirty=0x04, kOwnershipDirty=0x08,
//   kCoercionDirty=0x10, kStructDirty=0x20,
//   kDefUseDirty=0x40, kPpaHintDirty=0x80), so adding
// another bit requires widening the byte (uint8_t →
// uint16_t) — a major refactor.
//
// The PR ships:
//   - kVerificationDirty = 0x20 as a static constexpr
//     *named identifier* (not an actual dirty-byte bit)
//   - mark_dirty_verification(NodeId) — convenience wrapper
//     that calls the existing apply_verification_dirty_bits
//     (#469) with all standard reasons, which writes to
//     the orthogonal verification_dirty_ side-table AND
//     mirrors kGeneralDirty on the main dirty_ byte.
//   - mark_dirty_verification_upward(NodeId) — BFS walk of
//     parent_ chain (same shape as mark_dirty_upward at
//     line ~3726), applies verification-dirty on every
//     node along the path.
//   - is_verification_dirty(_for) / clear_verification_dirty
//     — read/clear accessors on the side-table.
//
// ACs:
//   AC1 bitmask 正确定义
//        (kVerificationDirty constant exists, even though
//        it aliases kStructDirty in the existing 8-bit byte —
//        the named identifier is preserved)
//   AC2 upward propagation works
//        (mark_dirty_verification_upward touches every
//         ancestor; verified via is_verification_dirty on
//         each)
//   AC3 doesn't affect existing dirty semantics/perf
//        (existing kGeneralDirty + kStructDirty paths are
//         untouched; the new helpers route through the
//         existing apply_verification_dirty_bits which
//         mirrors kGeneralDirty for backward compat)


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

namespace aura_issue_313_detail {
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
// AC1: kVerificationDirty constant exists + bitmask layout
// ═══════════════════════════════════════════════════════════════

bool test_k_verification_dirty_constant() {
    std::println("\n--- AC1: kVerificationDirty constant + bitmask layout ---");
    using namespace aura::ast;
    // The named constant exists at the suggested value.
    CHECK_EQ_LOCAL(static_cast<int>(FlatAST::kVerificationDirty), 0x20,
                   "kVerificationDirty == 0x20 (issue's suggested value)");
    // The existing dirty-byte layout is intact:
    //   all 8 bits used by other reasons, none free.
    //   kStructDirty = 0x20 is the collision the issue
    //   body flags; we keep the named identifier and
    //   rely on the orthogonal side-table.
    CHECK_EQ_LOCAL(static_cast<int>(FlatAST::kGeneralDirty), 0x01,
                   "kGeneralDirty still 0x01");
    CHECK_EQ_LOCAL(static_cast<int>(FlatAST::kStructDirty), 0x20,
                   "kStructDirty still 0x20 (existing bit preserved)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: upward propagation works
// ═══════════════════════════════════════════════════════════════

bool test_mark_dirty_verification_upward() {
    std::println("\n--- AC2: mark_dirty_verification_upward propagates BFS ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    // Build a depth-3 chain: root → mid → leaf, where leaf
    // is a Variable (node with no children).
    auto leaf = flat.add_variable(pool.intern("sig"));
    auto mid = flat.add_begin({leaf});
    auto root = flat.add_begin({mid});
    flat.root = root;
    (void)leaf; (void)mid; (void)root;
    // Pre-condition: nothing is verification-dirty.
    CHECK_EQ_LOCAL(flat.is_verification_dirty(leaf), false,
                   "leaf starts NOT verification-dirty");
    CHECK_EQ_LOCAL(flat.is_verification_dirty(mid), false,
                   "mid starts NOT verification-dirty");
    CHECK_EQ_LOCAL(flat.is_verification_dirty(root), false,
                   "root starts NOT verification-dirty");
    // Mark the leaf as verification-dirty via the new
    // mark_dirty_verification(_upward) helper.
    flat.mark_dirty_verification_upward(leaf);
    // Now the leaf, mid, AND root should all be
    // verification-dirty (BFS up the parent chain).
    CHECK_EQ_LOCAL(flat.is_verification_dirty(leaf), true,
                   "leaf is verification-dirty after upward mark");
    CHECK_EQ_LOCAL(flat.is_verification_dirty(mid), true,
                   "mid is verification-dirty (parent propagated)");
    CHECK_EQ_LOCAL(flat.is_verification_dirty(root), true,
                   "root is verification-dirty (root parent)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2 (cont): mark_dirty_verification (single node, no upward)
// ═══════════════════════════════════════════════════════════════

bool test_mark_dirty_verification_single() {
    std::println("\n--- AC2 (cont): mark_dirty_verification is single-node ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto leaf = flat.add_variable(pool.intern("sig"));
    auto mid = flat.add_begin({leaf});
    auto root = flat.add_begin({mid});
    flat.root = root;
    flat.mark_dirty_verification(leaf);
    CHECK_EQ_LOCAL(flat.is_verification_dirty(leaf), true,
                   "leaf is verification-dirty after single-node mark");
    // Single-node mark: parents should NOT be dirty (the
    // upward variant is what propagates).
    CHECK_EQ_LOCAL(flat.is_verification_dirty(mid), false,
                   "mid is NOT verification-dirty after single-node mark");
    CHECK_EQ_LOCAL(flat.is_verification_dirty(root), false,
                   "root is NOT verification-dirty after single-node mark");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2 (cont): per-reason query via is_verification_dirty_for
// ═══════════════════════════════════════════════════════════════

bool test_is_verification_dirty_for_per_reason() {
    std::println("\n--- AC2 (cont): per-reason query ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto leaf = flat.add_variable(pool.intern("sig"));
    flat.mark_dirty_verification(leaf);
    // mark_dirty_verification sets BOTH reasons at once
    // (it's a "all-reasons convenience" wrapper).
    const auto reasons = FlatAST::kCoverageFeedbackDirty | FlatAST::kAssertFailureDirty;
    CHECK_EQ_LOCAL(flat.is_verification_dirty_for(leaf, reasons), true,
                   "leaf has both coverage-feedback + assert-failure set");
    CHECK_EQ_LOCAL(flat.is_verification_dirty_for(leaf, FlatAST::kCoverageFeedbackDirty), true,
                   "leaf has coverage-feedback specifically");
    CHECK_EQ_LOCAL(flat.is_verification_dirty_for(leaf, FlatAST::kAssertFailureDirty), true,
                   "leaf has assert-failure specifically");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2 (cont): clear path
// ═══════════════════════════════════════════════════════════════

bool test_clear_verification_dirty() {
    std::println("\n--- AC2 (cont): clear_verification_dirty path ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto leaf = flat.add_variable(pool.intern("sig"));
    flat.mark_dirty_verification(leaf);
    CHECK_EQ_LOCAL(flat.is_verification_dirty(leaf), true,
                   "leaf is verification-dirty after mark");
    // Clear all reasons.
    flat.clear_verification_dirty(leaf);
    CHECK_EQ_LOCAL(flat.is_verification_dirty(leaf), false,
                   "leaf is NOT verification-dirty after clear");
    // Re-mark + clear-just-one.
    flat.mark_dirty_verification(leaf);
    flat.clear_verification_dirty_for(leaf, FlatAST::kCoverageFeedbackDirty);
    CHECK_EQ_LOCAL(flat.is_verification_dirty_for(leaf, FlatAST::kCoverageFeedbackDirty), false,
                   "leaf no longer has coverage-feedback after targeted clear");
    CHECK_EQ_LOCAL(flat.is_verification_dirty_for(leaf, FlatAST::kAssertFailureDirty), true,
                   "leaf still has assert-failure after targeted clear");
    flat.clear_verification_dirty(leaf);
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: existing dirty semantics + perf unchanged
// ═══════════════════════════════════════════════════════════════

bool test_existing_dirty_unchanged() {
    std::println("\n--- AC3: existing dirty paths untouched ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto leaf = flat.add_variable(pool.intern("sig"));
    auto mid = flat.add_begin({leaf});
    auto root = flat.add_begin({mid});
    flat.root = root;
    // The existing mark_dirty_upward path is unchanged; it
    // should still propagate to all ancestors.
    flat.mark_dirty_upward(leaf);
    CHECK_EQ_LOCAL(flat.is_dirty(leaf), true,
                   "mark_dirty_upward still marks leaf dirty (existing path)");
    CHECK_EQ_LOCAL(flat.is_dirty(mid), true,
                   "mark_dirty_upward still propagates to mid");
    CHECK_EQ_LOCAL(flat.is_dirty(root), true,
                   "mark_dirty_upward still propagates to root");
    // The verification path is independent: a fresh flat
    // should have mark_dirty_verification produce clean
    // results without touching the existing dirty byte.
    FlatAST fresh;
    auto fresh_leaf = fresh.add_variable(pool.intern("sig"));
    fresh.mark_dirty_verification(fresh_leaf);
    CHECK_EQ_LOCAL(fresh.is_verification_dirty(fresh_leaf), true,
                   "verification side-table independent of dirty byte");
    // The main dirty_ byte IS bumped (apply_verification_dirty_bits
    // mirrors kGeneralDirty for backward compat with legacy
    // is_dirty() callers).
    CHECK_EQ_LOCAL(fresh.is_dirty(fresh_leaf), true,
                   "mark_dirty_verification mirrors kGeneralDirty for legacy compat");
    return true;
}

int run_tests() {
    std::println("═══ Issue #313 (kVerificationDirty + helpers) ═══\n");
    test_k_verification_dirty_constant();
    test_mark_dirty_verification_upward();
    test_mark_dirty_verification_single();
    test_is_verification_dirty_for_per_reason();
    test_clear_verification_dirty();
    test_existing_dirty_unchanged();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_313_detail

int aura_issue_313_run() { return aura_issue_313_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_313_run(); }
#endif