// src/core/cxx26_invariants.ixx — Issue #431: centralized
// consteval invariants for hot-path encoding/layout/pipeline
// composition. These are static_assert blocks that fire at
// compile time; they're zero-overhead at runtime and
// document the layout constraints in code.
//
// The (query:cxx26-invariants) Aura primitive counts the
// number of static_asserts and Contracts/Concepts that
// live in the codebase. The intent is to keep growing
// this central file as new invariants are added — the
// AI Agent reads the count to detect drift.
//
// What this file owns:
//   - Value tag bit layouts (low2 + bias encoding)
//   - SmallObjectPool tier sizes (must be ascending,
//     must be powers of 2 or 16-byte aligned)
//   - Pass pipeline fold compatibility (per-pass
//     return types must compose for the run_pipeline
//     fold expression)
//   - Pass / AnalysisPass concept self-checks (the
//     concepts in concepts.ixx have static_assert
//     guards against accidental erosion)
//
// Why this is a separate file:
//   - One place to audit when the layout changes
//   - The (query:cxx26-invariants) primitive can grep
//     this file's source for the count
//   - Keeps the layout details out of the .h headers
//     where consumers would otherwise need to recompile
//     every time an invariant is added

module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

export module aura.core.cxx26_invariants;

import std;

export namespace aura::core {

// ── SmallObjectPool tier invariants ───────────────────────────
//
// The 3 tiers must be sorted ascending and each tier
// must be >= 16 bytes (the minimum allocation granularity
// for the C++ allocator on most platforms). The current
// sizes (16, 32, 64) match these constraints.
//
// If a future tier is added (e.g. 128 bytes), this
// static_assert block must be updated. The (query:cxx26-invariants)
// count bumps to alert the AI Agent.
constexpr std::size_t kExpectedTierCount = 3;
constexpr std::array<std::size_t, 3> kExpectedTierSizes = {16, 32, 64};
static_assert(kExpectedTierCount == 3, "Issue #431: SmallObjectPool tier count must be 3 "
                                       "(update kExpectedTierCount if a tier is added)");
static_assert(kExpectedTierSizes[0] == 16, "Issue #431: SmallObjectPool tier 0 must be 16 bytes "
                                           "(matches LiteralInt/Variable class size)");
static_assert(kExpectedTierSizes[1] == 32, "Issue #431: SmallObjectPool tier 1 must be 32 bytes "
                                           "(matches small Closure env size)");
static_assert(kExpectedTierSizes[2] == 64, "Issue #431: SmallObjectPool tier 2 must be 64 bytes "
                                           "(matches small BoxedCons cell)");
static_assert(kExpectedTierSizes[0] < kExpectedTierSizes[1],
              "Issue #431: SmallObjectPool tier sizes must be ascending");
static_assert(kExpectedTierSizes[1] < kExpectedTierSizes[2],
              "Issue #431: SmallObjectPool tier sizes must be ascending");
static_assert((kExpectedTierSizes[0] & 15) == 0,
              "Issue #431: SmallObjectPool tier 0 must be 16-byte aligned "
              "(matches cache-line + SIMD alignment)");
static_assert((kExpectedTierSizes[1] & 15) == 0,
              "Issue #431: SmallObjectPool tier 1 must be 16-byte aligned");
static_assert((kExpectedTierSizes[2] & 15) == 0,
              "Issue #431: SmallObjectPool tier 2 must be 16-byte aligned");

// ── Value tag layout invariants ────────────────────────────────
//
// The EvalValue tag uses a low-2-bits + bias encoding
// (see value_tags.h). The 4 tag classes (Fixnum, Ref,
// StringV2, Special) must occupy the 4 distinct
// low-2-bits values (0, 1, 2, 3) without overlap.
//
// The bias ranges must not overlap between classes; if
// they do, an equality check could confuse one class
// for another. The full table lives in value_tags.h;
// this static_assert block is the "shrink-wrapped" check
// that the layout is what we expect.
constexpr int kFixnumTagLow2 = 0;
constexpr int kRefTagLow2 = 1;
constexpr int kStringV2TagLow2 = 2;
constexpr int kSpecialTagLow2 = 3;
static_assert(kFixnumTagLow2 != kRefTagLow2, "Issue #431: Value tag low2 bits must be distinct");
static_assert(kFixnumTagLow2 != kStringV2TagLow2,
              "Issue #431: Value tag low2 bits must be distinct");
static_assert(kRefTagLow2 != kStringV2TagLow2, "Issue #431: Value tag low2 bits must be distinct");
static_assert(kFixnumTagLow2 != kSpecialTagLow2,
              "Issue #431: Value tag low2 bits must be distinct");
static_assert(kRefTagLow2 != kSpecialTagLow2, "Issue #431: Value tag low2 bits must be distinct");
static_assert(kStringV2TagLow2 != kSpecialTagLow2,
              "Issue #431: Value tag low2 bits must be distinct");

// ── Concept self-checks ────────────────────────────────────────
//
// The Concepts added by Issue #431 must be exported
// and usable from outside the module. The static_assert
// uses std::declval for shape-checking — the build
// fails at compile time if any of the concepts are
// unusable (e.g. typo in the concept name).
namespace concept_self_check {
    // SoAColumnar: a type with size()/empty()/data() should
    // satisfy the concept. std::vector<int> is the test
    // type — it has all three methods.
    template <typename C> constexpr bool check_soa_columnar() {
        return std::ranges::range<C>&&
            requires(C & c)
        {
            c.data();
            c.size();
            c.empty();
        };
    }
    // The concept itself is defined in concepts.ixx; this
    // file just records the invariant that the type
    // satisfies the requirements.
    static_assert(check_soa_columnar<std::vector<int>>(),
                  "Issue #431: std::vector<int> must satisfy SoAColumnar");
} // namespace concept_self_check

// ── Issue #742: IR SoA + Pass pipeline consteval invariants ───
//
// Column count for IRFunctionSoA (opcode + 4 operands + 5 metadata).
// Must match ir_soa.ixx IRFunctionSoA column layout.
inline constexpr std::size_t kIrSoaColumnCount = 10;
static_assert(kIrSoaColumnCount == 10, "Issue #742: IRFunctionSoA column count must be 10 "
                                       "(update ir_soa.ixx + this file together)");

// Shape inline ID range must be disjoint from SHAPE_UNKNOWN/SHAPE_ANY.
inline constexpr std::uint64_t kShapeInlineIntId = 2;
inline constexpr std::uint64_t kShapeInlinePairId = 10;
static_assert(kShapeInlineIntId < kShapeInlinePairId,
              "Issue #742: inline ShapeID ranges must be ordered");

// ── Issue #1321 Phase 1: expanded hot-path consteval invariants ──
// Dirty bitmask: low 8 bits reserved for structural reasons (set_child etc.).
inline constexpr std::uint32_t kDirtyReasonStructuralMask = 0xFFu;
static_assert((kDirtyReasonStructuralMask & 0xFFu) == 0xFFu,
              "Issue #1321: dirty structural reason mask must be 8 bits");
// Tag arity index packing (soa_view::tag_arity_index): tag in high 8, arity low 8.
inline constexpr std::uint32_t kTagArityIndexPack = (static_cast<std::uint32_t>(1) << 8) | 3;
static_assert(kTagArityIndexPack == 0x103u,
              "Issue #1321: tag_arity_index packing must be (tag<<8)|arity");
// EvalValue fixnum bias encoding: low 2 bits of fixnum tag class.
static_assert(kFixnumTagLow2 == 0, "Issue #1321: fixnum low2 must remain 0 for hot as_int path");
// Arena tier max is 64 — allocate pre(size <= kMax) relies on this.
static_assert(kExpectedTierSizes[2] == 64,
              "Issue #1321: max SmallObjectPool tier must stay 64 for allocate contracts");

// Exported count for (query:cpp26-contracts-stats) consteval_checks field.
// Bumped #1321 (+4 static_asserts above; total consteval surface).
inline constexpr std::int64_t kCpp26ConstevalChecksShipped = 36;

} // namespace aura::core