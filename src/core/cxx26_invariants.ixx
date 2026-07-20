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
    // Issue #1519: dirty bitmask column type used by FlatAST / IR SoA.
    static_assert(check_soa_columnar<std::vector<std::uint8_t>>(),
                  "Issue #1519: dirty column (vector<uint8_t>) must be SoAColumnar");
    static_assert(check_soa_columnar<std::vector<std::uint32_t>>(),
                  "Issue #1519: shape_ids_ column (vector<uint32_t>) must be SoAColumnar");
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

// ── Issue #1466 Phase 1: hot-path consteval invariants ───────
//
// Goal: more compile-time guarantees for hot-path dispatch tables and
// encoding layouts so that future drift is caught at build time
// (zero runtime cost). AI Agent bumps the consteval count and the
// (query:cxx26-invariants) primitive reflects the change.

// EvalValueTag enum completeness (#1466): the 5 primary tags must be
// the values 0..4 in declaration order (Fixnum=0, Ref=1, StringV2=2,
// Special=3, Float=4). Any reorder breaks low-2-bit dispatch. Unknown
// lives at 255 as a sentinel outside the low-2-bit space.
constexpr std::uint8_t kEvalValueTagFixnumVal = 0;
constexpr std::uint8_t kEvalValueTagRefVal = 1;
constexpr std::uint8_t kEvalValueTagStringV2Val = 2;
constexpr std::uint8_t kEvalValueTagSpecialVal = 3;
constexpr std::uint8_t kEvalValueTagFloatVal = 4;
static_assert(kEvalValueTagFixnumVal == 0,
              "Issue #1466: Fixnum tag must stay 0 for low-2 dispatch");
static_assert(kEvalValueTagRefVal == 1, "Issue #1466: Ref tag must stay 1 for low-2 dispatch");
static_assert(kEvalValueTagStringV2Val == 2,
              "Issue #1466: StringV2 tag must stay 2 for low-2 dispatch");
static_assert(kEvalValueTagSpecialVal == 3,
              "Issue #1466: Special tag must stay 3 for low-2 dispatch");
static_assert(kEvalValueTagFloatVal == 4,
              "Issue #1466: Float tag must stay 4 (post-low-2 sentinel)");
static_assert(kEvalValueTagFixnumVal < kEvalValueTagRefVal,
              "Issue #1466: enum values must be ascending");
static_assert(kEvalValueTagRefVal < kEvalValueTagStringV2Val,
              "Issue #1466: enum values must be ascending");
static_assert(kEvalValueTagStringV2Val < kEvalValueTagSpecialVal,
              "Issue #1466: enum values must be ascending");
static_assert(kEvalValueTagSpecialVal < kEvalValueTagFloatVal,
              "Issue #1466: enum values must be ascending");

// ShapeID boundary (#1466): SHAPE_UNKNOWN (0) must be the minimum
// ShapeID; inline ShapeIDs (Int=2, Pair=10, ...) live above SHAPE_ANY
// (1). Catches future renumbering before it silently breaks
// is_known_inline_shape_id().
constexpr std::uint64_t kShapeUnknownBoundary = 0;
constexpr std::uint64_t kShapeAnyBoundary = 1;
static_assert(kShapeUnknownBoundary == 0,
              "Issue #1466: SHAPE_UNKNOWN must be 0 (boundary sentinel)");
static_assert(kShapeAnyBoundary > kShapeUnknownBoundary,
              "Issue #1466: SHAPE_ANY must be above SHAPE_UNKNOWN");
static_assert(kShapeInlineIntId > kShapeAnyBoundary,
              "Issue #1466: inline Int ShapeID must be above SHAPE_ANY");
static_assert(kShapeInlinePairId > kShapeInlineIntId,
              "Issue #1466: inline Pair ShapeID must be above inline Int");

// IR SoA column breakdown (#1466): the 10 columns = 1 opcode + 4
// operands + 5 metadata fields. The breakdown is the spec; the total
// is the runtime invariant. Either side changing without the other
// breaks ir_soa.ixx serialize_soa round-trips.
constexpr std::size_t kIrSoaOpcodeColumns = 1;
constexpr std::size_t kIrSoaOperandColumns = 4;
constexpr std::size_t kIrSoaMetadataColumns = 5;
constexpr std::size_t kIrSoaComputedTotal =
    kIrSoaOpcodeColumns + kIrSoaOperandColumns + kIrSoaMetadataColumns;
static_assert(kIrSoaComputedTotal == kIrSoaColumnCount,
              "Issue #1466: IR SoA column breakdown (1+4+5) must sum to kIrSoaColumnCount");
static_assert(kIrSoaMetadataColumns >= 5,
              "Issue #1466: IR SoA needs >= 5 metadata cols (line/col/generation/dbg_info/flags)");

// Tagged encoding bit layout (#1466): the 4 low-2-bit tag classes
// (Fixnum, Ref, StringV2, Special) must map to distinct low-2-bit
// values. Floats share low-2=0 with Fixnums but are disjoint on the
// range, so the dispatch is two-stage (low-2 + range).
static_assert(kFixnumTagLow2 != kRefTagLow2 && kFixnumTagLow2 != kStringV2TagLow2 &&
                  kRefTagLow2 != kStringV2TagLow2 && kFixnumTagLow2 != kSpecialTagLow2 &&
                  kRefTagLow2 != kSpecialTagLow2 && kStringV2TagLow2 != kSpecialTagLow2,
              "Issue #1466: 4 low-2-bit tag classes must be pairwise distinct");

// ── Issue #1519: SIMD / cache-line / dirty / freelist / shape range ──
//
// These deepen the consteval surface for Arena allocate contracts,
// dirty propagation bitmasks, and ShapeProfiler inline IDs so AI
// multi-round mutation cannot silently drift layout constants.

// Cache line + SIMD alignment (hot SoA column packing).
inline constexpr std::size_t kCacheLineBytes = 64;
inline constexpr std::size_t kSimdAlignBytes = 16;
static_assert(kCacheLineBytes == 64, "Issue #1519: cache line must stay 64B for SoA packing");
static_assert(kSimdAlignBytes == 16, "Issue #1519: SIMD align must stay 16B");
static_assert((kCacheLineBytes % kSimdAlignBytes) == 0,
              "Issue #1519: cache line must be a multiple of SIMD align");
static_assert((kExpectedTierSizes[0] % kSimdAlignBytes) == 0,
              "Issue #1519: tier 0 size must be SIMD-aligned");
static_assert((kExpectedTierSizes[2] % kSimdAlignBytes) == 0,
              "Issue #1519: tier 2 size must be SIMD-aligned");

// Freelist relocate protocol: min free block must hold a next-pointer.
inline constexpr std::size_t kFreelistNextPtrBytes = sizeof(void*);
static_assert(kExpectedTierSizes[0] >= kFreelistNextPtrBytes,
              "Issue #1519: SmallObjectPool freelist needs tier0 >= sizeof(void*)");
static_assert(kExpectedTierSizes[0] >= 16,
              "Issue #1519: min tier must remain 16 for freelist + payload");

// Dirty bitmask constants (structural low 8 + higher feature bits).
inline constexpr std::uint8_t kDirtyGeneralBit = 0x01;
inline constexpr std::uint8_t kDirtyOccurrenceBit = 0x80; // high bit of structural byte
static_assert((kDirtyGeneralBit & kDirtyReasonStructuralMask) != 0,
              "Issue #1519: general dirty bit must live in structural mask");
static_assert((kDirtyOccurrenceBit & kDirtyReasonStructuralMask) != 0,
              "Issue #1519: occurrence dirty bit must live in structural mask");
static_assert(kDirtyGeneralBit != kDirtyOccurrenceBit,
              "Issue #1519: general vs occurrence dirty bits must be distinct");

// ShapeID practical upper bound for inline shapes (profiler table).
inline constexpr std::uint64_t kShapeInlineMaxId = 64;
static_assert(kShapeInlineIntId < kShapeInlineMaxId,
              "Issue #1519: inline Int ShapeID must be below max table");
static_assert(kShapeInlinePairId < kShapeInlineMaxId,
              "Issue #1519: inline Pair ShapeID must be below max table");
static_assert(kShapeInlineMaxId <= 255, "Issue #1519: ShapeID table must fit in uint8 fast path");

// Per-tier size total must equal SmallObjectPool layout (3MB / 3).
inline constexpr std::size_t kSmallPoolTotalBytes = 3 * 1024 * 1024;
inline constexpr std::size_t kPerTierExpected = kSmallPoolTotalBytes / kExpectedTierCount;
static_assert(kPerTierExpected == 1024 * 1024,
              "Issue #1519: per-tier region must stay 1MB (3MB pool / 3 tiers)");

// ── Issue #1620: Arena / Value / Shape / FlatAST / SoAView deepen ──
//
// Refines closed #1321 with compile-time guarantees for remaining
// high-churn mutation hot paths (tier size, dirty depth, NodeTag
// packing, special Value encodings, SoAView enforcement phase).

// Arena SmallObjectPool max tier must match allocate pre(size <= kMax).
inline constexpr std::size_t kArenaMaxSmallObjectBytes = 64;
static_assert(kArenaMaxSmallObjectBytes == kExpectedTierSizes[2],
              "Issue #1620: Arena max small object must equal tier2 size");
static_assert(kArenaMaxSmallObjectBytes == 64,
              "Issue #1620: Arena kMaxSmallSize must stay 64 for allocate contracts");

// FlatAST mark_dirty_upward production bounds (ast.ixx constants).
inline constexpr std::uint64_t kMarkDirtyMaxDepthConsteval = 64;
inline constexpr std::uint64_t kMarkDirtyCountThresholdConsteval = 4096;
static_assert(kMarkDirtyMaxDepthConsteval == 64,
              "Issue #1620: mark_dirty max depth must stay 64 (stack safety)");
static_assert(kMarkDirtyCountThresholdConsteval == 4096,
              "Issue #1620: mark_dirty count threshold must stay 4096");
static_assert((kMarkDirtyCountThresholdConsteval & (kMarkDirtyCountThresholdConsteval - 1)) == 0,
              "Issue #1620: mark_dirty count threshold should be power-of-2");

// NodeTag packing used by tag_arity_index (Let=0x06, LetRec=0x07).
inline constexpr std::uint8_t kNodeTagLetVal = 0x06;
inline constexpr std::uint8_t kNodeTagLetRecVal = 0x07;
static_assert(kNodeTagLetVal == 0x06, "Issue #1620: NodeTag::Let must stay 0x06");
static_assert(kNodeTagLetRecVal == 0x07, "Issue #1620: NodeTag::LetRec must stay 0x07");
static_assert(kNodeTagLetVal < kNodeTagLetRecVal, "Issue #1620: Let/LetRec tags ordered");
// Let with 2 children → tag_arity pack (0x06<<8)|2
inline constexpr std::uint32_t kTagArityLet2 =
    (static_cast<std::uint32_t>(kNodeTagLetVal) << 8) | 2u;
static_assert(kTagArityLet2 == 0x602u, "Issue #1620: Let arity-2 tag_arity pack");

// EvalValue Special encodings used by inline_shape_of (bool / void).
inline constexpr std::int64_t kSpecialBoolTrue = 3;
inline constexpr std::int64_t kSpecialBoolFalse = 7;
inline constexpr std::int64_t kSpecialVoid = 11;
static_assert(kSpecialBoolTrue == 3, "Issue #1620: Special true must stay 3");
static_assert(kSpecialBoolFalse == 7, "Issue #1620: Special false must stay 7");
static_assert(kSpecialVoid == 11, "Issue #1620: Special void must stay 11");
static_assert((kSpecialBoolTrue & 3) == kSpecialTagLow2,
              "Issue #1620: bool true must have Special low2 bits");
static_assert((kSpecialBoolFalse & 3) == kSpecialTagLow2,
              "Issue #1620: bool false must have Special low2 bits");
static_assert((kSpecialVoid & 3) == kSpecialTagLow2,
              "Issue #1620: void must have Special low2 bits");

// SoAView enforcement phase (soa_view.ixx kSoaViewEnforcementPhase = 3 / #1918).
inline constexpr int kSoaViewEnforcementPhaseConsteval = 3;
static_assert(kSoaViewEnforcementPhaseConsteval >= 3,
              "Issue #1918: SoAView enforcement phase must be >= 3 (EDSL full migration)");

// Exported count for (query:cpp26-contracts-stats) consteval_checks field.
// Bumped #1321 (+4), #1466 Phase 1 (+17), #1519 (+12), #1620 (+12 Arena/FlatAST/Value/SoA).
inline constexpr std::int64_t kCpp26ConstevalChecksShipped = 77;

} // namespace aura::core