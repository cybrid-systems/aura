// Issue #221: include the PersistentChildVector header in the
// module's global module fragment (same trick as #220's
// gap_buffer.hh). The global fragment is processed BEFORE the
// module's purview, so the std includes in
// persistent_child_vector.hh don't conflict with `import std;`.
module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "core/persistent_child_vector.hh"
#include "core/cpp26_contract_stats.h"
#include <contracts>
#include <shared_mutex>

export module aura.core.ast;
import std;
import aura.core.type;
import aura.core.mutation;
import aura.core.concepts;
export import aura.core.mutation;

namespace aura::ast {

// ── Common type aliases ──────────────────────────────────────
// NodeId / NULL_NODE / SymId come from aura.core.mutation (#275).
export constexpr SymId INVALID_SYM = ~0u;

// ── Issue #217 Cycle 5/7: AST types are reflection-ready ─────
//
// The AST types below (SourceLocation, ParsedPhase, NodeMeta,
// NodeView, MutationRecord, Patch, MatchClauseInfo) are plain
// POD structs that the C++26 P2996 reflection
// (reflect_members<T>()) sees automatically — no explicit
// REFLECT_MEMBERS annotation is required. The reflection-driven
// serialization (auto_serialize<T>(buf, obj) /
// auto_deserialize<T>(buf, pos)) works for the simple types
// (SourceLocation, Patch) out of the box.
//
// Cycle 7 updates:
//   - std::span<T, N> is now a first-class MemberKind
//     (MemberKind::Span, is_std_span trait). Cycle 6
//     added the basic support; Cycle 7 fixed the
//     serialize side to use elem_size from MemberInfo
//     (correct for non-char element types).
//   - NodeView's serialize side now correctly writes
//     the byte count for std::span<const NodeId>
//     children, std::span<const SymId> params, etc.
//   - The DESERIALIZE side still hardcodes std::span<const
//     char> for the destination type. For non-char
//     element types, the caller must re-interpret the
//     deserialized bytes. This is documented as a
//     follow-up limitation (Cycle 8).
//
// For the still-pending types (MutationRecord, MatchClauseInfo):
//   - Cycle 9 verification: MutationRecord-like struct
//     (5 std::string fields + 2 enum fields + 10 POD fields)
//     roundtrips correctly. All 5 string fields preserve
//     their values, all enums preserve, all bool preserve,
//     all uint32/uint64 preserve. 25 new checks pass in
//     test_issue_217.cpp Test 13. This unblocks the
//     actual MutationRecord migration in src/core/ast.ixx.
//   - Cycle 10 verification: MatchClauseInfo-like struct
//     (2 std::vector<SymId> + 1 bool) roundtrips correctly.
//     All 3 fields preserve their values through
//     serialize/deserialize. 18 new checks pass in
//     test_issue_217.cpp Test 14. This unblocks the
//     actual MatchClauseInfo migration in src/core/ast.ixx.
//
// The test in tests/test_issue_217.cpp verifies the
// reflection infrastructure works for the simple AST types
// (SourceLocation, Patch, NodeViewLike with std::span<const
// char>) and documents the limitations for the more complex
// types.

// ── Source location ────────────────────────────
export struct SourceLocation {
    std::uint32_t line = 0, column = 0, file = 0;
};
// ── Syntax marker (Trees-That-Grow / hygienic macros) ──────
export enum class SyntaxMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
    BoolLiteral = 2, // #t / #f
};


// ── Phase tag (Trees-That-Grow) ──────────────────────────────
export struct ParsedPhase {
    static constexpr std::uint32_t id = 0;
};

// ── Node tags ────────────────────────────────────────────────
export enum class NodeTag : std::uint32_t {
    LiteralInt = 0x01,
    Variable = 0x02,
    Call = 0x03,
    IfExpr = 0x04,
    Lambda = 0x05,
    Let = 0x06,
    LetRec = 0x07,
    Define = 0x08,
    Begin = 0x09,
    Set = 0x0A,
    Quote = 0x0B,
    LiteralString = 0x0D,
    MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
    LiteralFloat = 0x11,
    Pair = 0x12,
    DefineType = 0x13,
    DefineModule = 0x14,
    Export = 0x15,
    Linear = 0x16,
    Move = 0x17,
    Borrow = 0x18,
    MutBorrow = 0x19,
    Drop = 0x1A,
    // Issue #310: minimal SV structural tags. The Interface
    // declares a port bundle (SystemVerilog `interface` form);
    // the Modport declares a directional view over an
    // interface's signals (`modport` form). These tags are
    // the AST-level foundation — no builder, query, or emit
    // is added in this issue. Follow-up issues will wire the
    // constructors + side-table population + lowering hooks.
    Interface = 0x1B,
    Modport = 0x1C,
    // Issue #694: SVA structural tags for verification-driven
    // self-evolution (property/sequence/assert/covergroup/coverpoint).
    Property = 0x1D,
    Sequence = 0x1E,
    Assert = 0x1F,
    Covergroup = 0x20,
    Coverpoint = 0x21,
    // Issue #496: SV constraint + class tags for verification-driven
    // randomization and OOP containers (native FlatAST SoA).
    Constraint = 0x22,
    Class = 0x23,
};

// Issue #402: FlatAST summary flags. Bit-set computed eagerly
// in add_node (O(1) per add) so needs_tree_walker_fallback
// can fast-path trivial expressions (all bits == 0) without
// scanning every node. The full scan remains as the slow
// path for expressions that touch at least one of these flags.
//
// Each bit corresponds to ONE node-tag class (or
// node-int_value / node-sym_id pattern) that the slow-path
// scan looks for. Bits are OR-ed into summary_flags_ on
// add_node; reset() / clear() clears the whole field.
export enum class SummaryFlag : std::uint32_t {
    None = 0,
    HasMacroDef = 1u << 0,          // NodeTag::MacroDef → tree-walker fallback
    HasDefineType = 1u << 1,        // NodeTag::DefineType → tree-walker fallback
    HasDefineModule = 1u << 2,      // NodeTag::DefineModule → tree-walker fallback
    HasLambdaDotted = 1u << 3,      // Lambda with int_value!=0 (dotted rest param)
    HasTypeAnnotVar = 1u << 4,      // TypeAnnotation with int_value!=0 (3-arg form)
    HasSet = 1u << 5,               // NodeTag::Set (set! special form)
    HasKeywordVar = 1u << 6,        // Variable with sym_id starting with ':'
    HasQueryOrMutateCall = 1u << 7, // Call with callee sym_id starts_with "query:" / "mutate:"
    HasTreeWalkerCall = 1u << 8, // Call with callee sym_id in tree_walker_only set (slow-path only)
    HasUserBindingVar = 1u << 9, // Variable sym_id in user_bindings_ (cross-cutting with state)
    HasUnresolvedVar = 1u << 10, // Variable sym_id not in primitives + not in ir_cache_
};


// ── StringPool — arena-backed string interner ──────────────────
//
// Interns strings into a contiguous character buffer and returns
// SymId (std::uint32_t offset into the buffer). The hash table uses
// open addressing with linear probing for cache-friendliness.
//
// All storage is arena-allocated via pmr vector. reset() reclaims
// everything.
//
// Thread safety (Issue #1861): intern() mutates buf_ / hash_tbl_ with
// no internal lock. Concurrent intern (or intern vs resolve that
// races a rehash) on the same StringPool is undefined. Shared pools
// (workspace / canonical) must be serialized by the owner — e.g.
// workspace_mtx_ writers, or single-fiber eval that alone binds
// into Envs holding that pool. resolve() / find_by_name() are const
// reads and are safe only while no concurrent intern runs.
//
export class StringPool {
public:
    explicit StringPool(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : buf_(alloc)
        , hash_tbl_(alloc) {
        // Reserve 0 as invalid sentinel
        buf_.push_back('\0');
        // Initialize hash table (power of 2, start small)
        rehash(64);
    }

    // Intern a string — returns a stable SymId.
    // Not thread-safe (Issue #1861); see class comment.
    SymId intern(std::string_view s) {
        if (s.empty())
            return 0;

        auto hash = hash_str(s);
        auto mask = hash_capacity_ - 1;
        auto idx = hash & mask;

        while (hash_tbl_[idx] != INVALID_SYM) {
            auto existing = hash_tbl_[idx];
            auto view = resolve(existing);
            if (view == s)
                return existing;    // already interned
            idx = (idx + 1) & mask; // linear probe
        }

        // Not found — intern
        auto offset = static_cast<SymId>(buf_.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back('\0'); // null terminator

        hash_tbl_[idx] = offset;

        // Grow if load factor > 0.5
        ++entry_count_;
        if (entry_count_ * 2 > hash_capacity_)
            rehash(hash_capacity_ * 2);

        return offset;
    }

    // Resolve a SymId back to string_view
    std::string_view resolve(SymId id) const {
        if (id >= buf_.size())
            return {};
        return std::string_view(buf_.data() + id);
    }

    // Issue #372: reverse name → SymId lookup.
    //
    // Reuses the existing hash_tbl_ probe loop (same FNV-1a
    // hash + linear probe as `intern`) so we don't need a
    // side-table — buf_ may grow/realloc, but the SymId
    // offsets in hash_tbl_ stay valid, so we just walk the
    // probe chain and compare names.
    //
    // Empty name maps to SymId 0 (the leading '\0' sentinel
    // — matches `intern("")`). Names not present in the pool
    // return std::nullopt; callers should distinguish "pool
    // has no such name" from "pool has it at offset 0".
    //
    // Per-pool lookup is the foundation for cross-layer
    // (cross-pool) queries: callers do `pool.intern(name)` (or
    // the resolved SymId from a captured ref) → walk the AST
    // of that pool's flat. We don't try to unify SymIds
    // across pools (that's deferred — see #372 follow-ups).
    [[nodiscard]] std::optional<SymId> find_by_name(std::string_view s) const noexcept {
        if (s.empty())
            return SymId{0};
        auto hash = hash_str(s);
        auto mask = hash_capacity_ - 1;
        auto idx = hash & mask;
        while (hash_tbl_[idx] != INVALID_SYM) {
            auto existing = hash_tbl_[idx];
            if (resolve(existing) == s)
                return existing;
            idx = (idx + 1) & mask;
        }
        return std::nullopt;
    }

    // Total bytes used for string data
    std::size_t data_size() const { return buf_.size(); }

    // Issue #187 (P0): observability for the hash table. 0.0 = empty,
    // 1.0 = fully packed. load_factor (entries / capacity) is the
    // standard metric. We expose it alongside the raw counts so the
    // auto-compact trigger (Issue #187) can decide when to rehash.
    [[nodiscard]] std::size_t entry_count() const noexcept { return entry_count_; }
    [[nodiscard]] std::size_t hash_capacity() const noexcept { return hash_capacity_; }
    [[nodiscard]] double load_factor() const noexcept {
        return hash_capacity_ == 0
                   ? 0.0
                   : static_cast<double>(entry_count_) / static_cast<double>(hash_capacity_);
    }
    [[nodiscard]] std::size_t hash_table_bytes() const noexcept {
        return hash_tbl_.size() * sizeof(SymId);
    }
    // Sum of byte lengths of all interned strings (excludes the
    // trailing NUL per string but includes the leading \0 sentinel).
    [[nodiscard]] std::size_t string_bytes_total() const noexcept {
        std::size_t total = 1; // leading \0
        for (std::uint32_t i = 0; i < hash_capacity_; ++i) {
            if (hash_tbl_[i] != INVALID_SYM) {
                total += resolve(hash_tbl_[i]).size() + 1; // +1 for NUL
            }
        }
        return total;
    }
    // Total memory footprint of this StringPool (buf_ + hash_tbl_ +
    // entry_count_ + hash_capacity_ bookkeeping). For the
    // observability layer to report.
    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return data_size() + hash_table_bytes() + sizeof(*this);
    }
    // Fragmentation ratio of buf_ = (data_size - string_bytes_total)
    // / data_size. 0.0 = perfectly packed, 1.0 = entirely dead bytes.
    // After reset()+re-intern, the leading \0 + the most-recently-
    // appended strings dominate; older strings can leave gaps if the
    // pool has been reset-then-rebuilt. In typical steady state
    // (no reset), buf_ grows monotonically and fragmentation ~0.
    [[nodiscard]] double buf_fragmentation() const noexcept {
        std::size_t ds = data_size();
        return ds == 0 ? 0.0
                       : static_cast<double>(ds - string_bytes_total()) / static_cast<double>(ds);
    }

    // Issue #187 (P0): conservative compact() that rebuilds the
    // hash table at the smallest power-of-2 capacity that still
    // holds all live entries (load factor 0.5-0.6). SymIds (which
    // are offsets into buf_) are NOT remapped — buf_ is monotonic
    // (strings are only appended, never removed), so SymIds stay
    // valid. This compact only reclaims hash_tbl_ memory, not
    // buf_ memory.
    //
    // Returns bytes reclaimed (from hash_tbl_ shrink).
    [[nodiscard]] std::size_t compact() noexcept {
        std::size_t before = hash_table_bytes();
        if (entry_count_ == 0) {
            // Nothing to keep — reset to minimum.
            rehash(64);
        } else {
            // Pick smallest power-of-2 capacity with load <= 0.5.
            std::uint32_t target = 64;
            while (target / 2 < entry_count_)
                target *= 2;
            if (target < hash_capacity_) {
                rehash(target);
            }
        }
        std::size_t after = hash_table_bytes();
        std::size_t saved = (before > after) ? (before - after) : 0;
        return saved;
    }

    // Reset all state
    void reset() {
        buf_.clear();
        buf_.push_back('\0');
        hash_tbl_.clear();
        hash_capacity_ = 0;
        entry_count_ = 0;
        rehash(64);
    }

private:
    static std::uint64_t hash_str(std::string_view s) {
        // FNV-1a
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (auto c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001B3ull;
        }
        return h;
    }

    void rehash(std::uint32_t new_cap) {
        auto old_tbl = std::move(hash_tbl_);
        auto old_cap = hash_capacity_;

        hash_capacity_ = new_cap;
        hash_tbl_.assign(new_cap, INVALID_SYM);
        entry_count_ = 0;

        for (std::uint32_t i = 0; i < old_cap; ++i) {
            if (old_tbl[i] != INVALID_SYM) {
                auto hash = hash_str(resolve(old_tbl[i]));
                auto mask = hash_capacity_ - 1;
                auto idx = hash & mask;
                while (hash_tbl_[idx] != INVALID_SYM)
                    idx = (idx + 1) & mask;
                hash_tbl_[idx] = old_tbl[i];
                ++entry_count_;
            }
        }
    }

    // Backing store: contiguous null-terminated strings
    std::pmr::vector<char> buf_;

    // Open-addressing hash table (SymId → offset into buf_)
    std::pmr::vector<SymId> hash_tbl_;
    std::uint32_t hash_capacity_ = 0;
    std::uint32_t entry_count_ = 0;
};

// ── Node metadata (constexpr table for reflection/validation) ──
export struct NodeMeta {
    NodeTag tag;
    std::string_view name;
    std::uint8_t fixed_children; // how many children this tag has
    bool has_var_children;       // variable-length children (Call args)
    bool has_string;             // has a name/id string (Variable/Let/Define)
    bool has_int;                // has an int64 value (LiteralInt)
    bool has_float;              // has a double value (LiteralFloat)
    bool has_params;             // has param list (Lambda)
};

// Tag-to-metadata mapping, indexed by `tag - 1`.
// Tags must be sequential starting from 1 (LiteralInt = 0x01).
// Gap at 0x0C is filled with a sentinel.
export constexpr std::array<NodeMeta, 35> kNodeMeta = {{
    {NodeTag::LiteralInt, "LiteralInt", 0, false, false, true, false, false},       // 0x01
    {NodeTag::Variable, "Variable", 0, false, true, false, false, false},           // 0x02
    {NodeTag::Call, "Call", 1, true, false, false, false, false},                   // 0x03
    {NodeTag::IfExpr, "IfExpr", 3, false, false, false, false, false},              // 0x04
    {NodeTag::Lambda, "Lambda", 1, false, false, false, false, true},               // 0x05
    {NodeTag::Let, "Let", 2, false, true, false, false, false},                     // 0x06
    {NodeTag::LetRec, "LetRec", 2, false, true, false, false, false},               // 0x07
    {NodeTag::Define, "Define", 1, false, true, false, false, false},               // 0x08
    {NodeTag::Begin, "Begin", 0, true, false, false, false, false},                 // 0x09
    {NodeTag::Set, "Set", 1, false, true, false, false, false},                     // 0x0A
    {NodeTag::Quote, "Quote", 1, false, false, false, false, false},                // 0x0B
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false, false},           // 0x0C (gap)
    {NodeTag::LiteralString, "LiteralString", 0, false, true, false, false, false}, // 0x0D
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false,
     false}, // 0x0E (MacroDef — Expr* only)
    {NodeTag::TypeAnnotation, "TypeAnnotation", 1, false, true, false, false, false}, // 0x0F
    {NodeTag::Coercion, "Coercion", 1, false, true, false, false, false},             // 0x10
    {NodeTag::LiteralFloat, "LiteralFloat", 0, false, false, false, true, false},     // 0x11
    {NodeTag::Pair, "Pair", 2, false, false, false, false, false},                    // 0x12
    {NodeTag::LiteralInt, "<gap>", 0, false, false, false, false, false},             // 0x13 (gap)
    {NodeTag::DefineModule, "DefineModule", 0, true, false, false, false, false},     // 0x14
    {NodeTag::Export, "Export", 0, true, false, false, false, false},                 // 0x15
    {NodeTag::Linear, "Linear", 1, false, false, false, false, false},                // 0x16
    {NodeTag::Move, "Move", 1, false, false, false, false, false},                    // 0x17
    {NodeTag::Borrow, "Borrow", 1, false, false, false, false, false},                // 0x18
    {NodeTag::MutBorrow, "MutBorrow", 1, false, false, false, false, false},          // 0x19
    {NodeTag::Drop, "Drop", 1, false, false, false, false, false},                    // 0x1A
    // Issue #310: SV structural tags. Same shape as
    // DefineModule/Export (0 fixed children, var_children,
    // no per-node flags) — the structural payload (interface
    // name + port list / modport name + port directions)
    // will be populated via side-tables when the builder
    // methods land in a follow-up.
    {NodeTag::Interface, "Interface", 0, true, false, false, false, false},   // 0x1B
    {NodeTag::Modport, "Modport", 0, true, false, false, false, false},       // 0x1C
    {NodeTag::Property, "Property", 1, false, true, false, false, false},     // 0x1D
    {NodeTag::Sequence, "Sequence", 1, false, true, false, false, false},     // 0x1E
    {NodeTag::Assert, "Assert", 1, false, true, false, false, false},         // 0x1F
    {NodeTag::Covergroup, "Covergroup", 0, true, false, false, false, false}, // 0x20
    {NodeTag::Coverpoint, "Coverpoint", 0, true, false, false, false, false}, // 0x21
    {NodeTag::Constraint, "Constraint", 0, true, false, false, false, false}, // 0x22
    {NodeTag::Class, "Class", 0, true, false, false, false, false},           // 0x23
}};


export constexpr const NodeMeta& meta(NodeTag tag) {
    return kNodeMeta[static_cast<std::size_t>(tag) - 1];
}

// Compile-time validation: check known entries, skip gap sentinels
consteval bool validate_node_meta() {
    // Gap entries (0x0C, 0x0E in tag space → indices 11, 13)
    if (kNodeMeta[11].name != "<gap>")
        return false;
    if (kNodeMeta[13].name != "<gap>")
        return false;
    if (meta(NodeTag::LiteralInt).name != "LiteralInt")
        return false;
    if (meta(NodeTag::Call).fixed_children != 1)
        return false;
    if (meta(NodeTag::Call).has_var_children != true)
        return false;
    if (meta(NodeTag::Lambda).has_params != true)
        return false;
    if (meta(NodeTag::IfExpr).fixed_children != 3)
        return false;
    if (meta(NodeTag::Let).has_string != true)
        return false;
    if (meta(NodeTag::LiteralInt).has_int != true)
        return false;
    if (meta(NodeTag::Begin).has_var_children != true)
        return false;
    if (meta(NodeTag::Coercion).name != "Coercion")
        return false;
    if (meta(NodeTag::LiteralFloat).name != "LiteralFloat")
        return false;
    if (meta(NodeTag::LiteralFloat).has_float != true)
        return false;
    return true;
}
static_assert(validate_node_meta(), "kNodeMeta misaligned with NodeTag enum");

// ── NodeView — lightweight non-owning read view ────────────────
export struct NodeView {
    NodeId id = NULL_NODE; // node index in the FlatAST
    NodeTag tag = NodeTag::LiteralInt;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    SymId sym_id = INVALID_SYM;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    // Issue #73 Phase 2: per-view TypeId cache. 0 = DYNAMIC/unknown.
    // Populated by FlatAST::get (from SoA column) and MappedCache::get
    // (from cache column), so callers reading a NodeView don't have to
    // look up the SoA column separately.
    std::uint32_t type_id = 0;
    std::span<const NodeId> children;
    std::span<const SymId> params;
    std::span<const NodeId> param_annotations; // annotation node IDs, may be NULL_NODE
    SyntaxMarker marker = SyntaxMarker::User;

    bool has_int() const { return tag == NodeTag::LiteralInt; }
    bool has_float() const { return tag == NodeTag::LiteralFloat; }
    bool has_name() const { return sym_id != INVALID_SYM; }
    NodeId child(std::uint32_t i) const { return children[i]; }
};

// MutationRecord / rollback types live in aura.core.mutation (#275).

// ── Patch — AI mutation descriptor ─────────────────────────────
export struct Patch {
    NodeId node = NULL_NODE;
    std::uint32_t field_offset = 0;
    std::uint64_t new_value = 0;
};

// ── Match clause metadata (for exhaustiveness checking) ──
// Maps let-node-id → list of constructor SymIds used in match clauses.
// If a wildcard pattern (_) is used, stores a single INVALID_SYM entry.
//
// `used_constructors` collects patterns like (Ctor args...) — definite.
// `candidate_constructors` collects bare-identifier patterns like `Nil` —
// these are ambiguous (could be a constructor or a variable binding) and
// are resolved by the type checker against the actual subject type.
export struct MatchClauseInfo {
    std::vector<SymId> used_constructors;
    std::vector<SymId> candidate_constructors;
    bool has_wildcard = false;
    // Issue #260: set by typecheck / post-mutation exhaustiveness pass.
    bool exhaustiveness_checked = false;
    // Normalized subject TypeId.index from last exhaustiveness check (0 = unknown).
    std::uint32_t subject_type_id = 0;
};

// ── Issue #222: OwnedSharedMutex wrapper ──────────────────────
//
// std::shared_mutex is neither copyable nor movable, so a class
// containing it directly has its implicit copy/move ctors
// deleted. This wrapper stores the mutex inline and defines the
// copy/move semantics we want for FlatAST:
//
//   - Copy ctor: placement-new a FRESH shared_mutex. Each copy
//     gets its own mutex (independent mutation isolation).
//   - Copy assign: no-op (the destination keeps its own mutex;
//     only the data members are overwritten).
//   - Move ctor / move assign: destination gets a fresh mutex;
//     the source keeps its own until destroyed (lock state is
//     not transferred — same discipline as the old unique_ptr
//     move, which left the moved-from FlatAST with nullptr).
//
// Issue #300 follow-up #1: the previous unique_ptr<std::shared_mutex>
// allocated the mutex on the heap. ~FlatAST ran ~OwnedSharedMutex
// (freeing that heap block) before ~children_, and ASAN caught a
// PCV shared_ptr control block UAF — the freed mutex block was
// reused with a corrupted use_count. Inline storage removes the
// extra heap free entirely.
//
// Used as the type of `FlatAST::structural_mtx_`.
class OwnedSharedMutex {
public:
    OwnedSharedMutex() noexcept { construct(); }
    ~OwnedSharedMutex() { destroy(); }

    // Copy: fresh mutex (independent isolation).
    OwnedSharedMutex(const OwnedSharedMutex&) noexcept { construct(); }
    // Move: fresh mutex in the destination; source keeps its own.
    OwnedSharedMutex(OwnedSharedMutex&&) noexcept { construct(); }
    // Copy-assign: keep our own mutex (the data being copied
    // doesn't include the mutex state).
    OwnedSharedMutex& operator=(const OwnedSharedMutex&) noexcept { return *this; }
    // Move-assign: keep our own mutex.
    OwnedSharedMutex& operator=(OwnedSharedMutex&&) noexcept { return *this; }

    std::shared_mutex& get() noexcept { return *mutex_ptr(); }
    const std::shared_mutex& get() const noexcept { return *mutex_ptr(); }
    // Like get() but returns a non-const reference even through
    // a const OwnedSharedMutex. Needed because shared_lock /
    // unique_lock require a non-const mutex reference to acquire
    // (the lock state is part of the mutex). The const_cast is
    // safe here because acquiring a lock is a "logical const"
    // operation: it doesn't modify the protected data.
    std::shared_mutex& mutable_get() const noexcept {
        return *const_cast<std::shared_mutex*>(mutex_ptr());
    }

private:
    alignas(std::shared_mutex) std::byte storage_[sizeof(std::shared_mutex)];

    std::shared_mutex* mutex_ptr() noexcept {
        return std::launder(reinterpret_cast<std::shared_mutex*>(storage_));
    }
    const std::shared_mutex* mutex_ptr() const noexcept {
        return std::launder(reinterpret_cast<const std::shared_mutex*>(storage_));
    }
    void construct() { std::construct_at(mutex_ptr()); }
    void destroy() { std::destroy_at(mutex_ptr()); }
};

// Issue #261: observability POD for NodeId lifecycle / fragmentation.
export struct NodeLifecycleStats {
    std::size_t total_slots = 0;
    std::size_t live_nodes = 0;
    std::size_t free_slots = 0;
    double fragmentation_ratio = 0.0;
};

// Issue #263: post-restore consistency report (generation + span validity).
export struct PostRestoreReport {
    std::size_t violations = 0;
    std::uint16_t generation = 0;
    std::size_t live_nodes = 0;
    std::size_t free_slots = 0;
};

// ── FlatAST — SoA flat index-based AST ─────────────────────────

// ═══════════════════════════════════════════════════════════════
// Generic AST traversal helpers (Phase A hoisting)
//
// Originally lived in aura.compiler.query. Moved here in the
// Phase A hoisting commit so aura.core.ast can also use them
// (aura.compiler.query imports aura.core.ast, so the helpers
// couldn't live in query if ast wanted to use them — would
// create a cycle).
//
// Re-exported from aura.compiler.query via `export using` for
// backward compat with all existing call sites in compiler/.
//
// Each helper is constrained by aura::core::ASTContainer (and
// optionally AuraInvocable for visitor/predicate params). Zero
// runtime overhead: template-inlined at -O3.
// ═══════════════════════════════════════════════════════════════

// ── Generic AST traversal helper (ASTContainer-constrained) ──────────
//
// Walks every child of `root` in `ast` and invokes `vis(child_id)`
// for each one. Constrained by aura::core::ASTContainer so it works
// for any AST that exposes get/children/tag — not just FlatAST.
// This is the first place in the query module that uses the
// core concept (Phase 3 follow-up #1, applied here).
//
// Two-parameter form: walk_children<Id, C, Visitor>(ast, root, vis)
// where Id is the node handle type, C is the AST container type
// (constrained via ASTContainer<C, Id>), and Visitor is the
// per-child callable. The visitor receives each child Id by value.
//
// Why a free template instead of a method on ASTIndex?
//   - Demonstrates that the ASTContainer concept actually compiles
//     against a real query-layer consumer.
//   - Lets future AST types (e.g., a cursor AST or a serialized
//     read-only AST) plug into the same traversal logic without
//     inheriting from FlatAST.
export template <typename Id, typename C, typename Visitor>
    requires aura::core::ASTContainer<C, Id> && std::invocable<Visitor&, Id>
constexpr std::size_t walk_children(C& ast, Id root, Visitor&& vis) {
    std::size_t count = 0;
    // Issue #1520: prefer children_columnar / children_safe when available
    // so hot walks pin PCV storage and bump columnar metrics.
    if constexpr (requires { ast.children_columnar(root); }) {
        auto safe = ast.children_columnar(root);
        for (auto child : safe) {
            vis(static_cast<Id>(child));
            ++count;
        }
    } else {
        for (auto child : ast.children(root)) {
            vis(static_cast<Id>(child));
            ++count;
        }
    }
    return count;
}

// ── count_nodes_with_predicate — recursive DFS counter ────────────
//
// Phase D helper. Walks the subtree rooted at `root`, invoking
// `pred(node_id)` for each node (including root). Returns the
// count of nodes for which pred returns a truthy value.
//
// Precondition: `root` must be a valid node id. If you have a
// possibly-null root, check `root != NULL_NODE` (or the
// appropriate sentinel for your Id type) before calling —
// the helper does NOT handle null roots gracefully (passing a
// null root would crash on `ast.children(root)` since
// ASTContainer doesn't require an `is_valid` accessor).
//
// Zero overhead:
//   - Recursive DFS, no allocations.
//   - Pred invoked via template-inlined operator().
//   - Compiles to the same code as a hand-written recursive
//     counter at -O3.
//
// Used by (future) compile:* stats primitives that need to
// count nodes matching some criteria.
export template <typename Id, typename C, typename P>
    requires aura::core::ASTContainer<C, Id> && aura::core::AuraInvocable<P&, Id>
[[nodiscard]] constexpr std::size_t count_nodes_with_predicate(C& ast, Id root, P&& pred) {
    std::size_t count = 0;
    if (static_cast<bool>(pred(root)))
        ++count;
    for (auto child : ast.children(root)) {
        count += count_nodes_with_predicate(ast, static_cast<Id>(child), pred);
    }
    return count;
}

// ── find_first_node_with — recursive DFS first-match finder ──────
//
// Phase D helper. Walks the subtree rooted at `root` in pre-order
// DFS (root first, then children left-to-right). Returns the
// first node id for which `pred(node_id)` is truthy, or
// std::nullopt if no node matches.
//
// Precondition: same as count_nodes_with_predicate — `root`
// must be a valid node id.
//
// Zero overhead: same as count_nodes_with_predicate. The
// std::optional<Id> return is a small wrapper (1 byte tag +
// Id payload) and short-circuits as soon as a match is found
// (no full traversal).
export template <typename Id, typename C, typename P>
    requires aura::core::ASTContainer<C, Id> && aura::core::AuraInvocable<P&, Id>
[[nodiscard]] constexpr std::optional<Id> find_first_node_with(C& ast, Id root, P&& pred) {
    if (static_cast<bool>(pred(root)))
        return root;
    for (auto child : ast.children(root)) {
        if (auto found = find_first_node_with(ast, static_cast<Id>(child), pred)) {
            return found;
        }
    }
    return std::nullopt;
}

// ── walk_ancestors — parent-chain upward walk ──────────────
//
// Phase D.5 helper. Walks the parent chain starting at
// `start`, invoking `vis(id)` for each ancestor (including
// `start` itself, mirroring walk_env_frames in evaluator.ixx).
// Stops when:
//   - `vis(id)` returns false (caller-initiated early stop), or
//   - `parent_of(cur)` returns `cur` (self-loop termination).
//
// Self-loop termination is the standard "termination" pattern
// for FlatAST: parent_of(any valid node) returns the actual
// parent (or NULL_NODE for roots), and parent_of(NULL_NODE)
// returns NULL_NODE — same as input — so the loop ends.
//
// Preconditions:
//   - `start` must be a valid node id (not the null sentinel).
//     Callers should check `start != NULL_NODE` first if it
//     may be null.
//   - The AST's parent_of() must eventually return a self-
//     loop to terminate. FlatAST satisfies this.
//
// Zero overhead:
//   - Single while loop, no recursion, no allocation.
//   - vis invoked via template-inlined operator().
//   - Self-loop check is one compare per step.
//   - At -O3, generates identical assembly to a hand-written
//     while loop with a self-loop exit.
//
// Used by (future): scope chain analysis, name resolution
// (find enclosing scope), GC root marking from a node.
export template <typename Id, typename C, typename V>
    requires aura::core::ASTContainer<C, Id> &&
             requires(C& c, Id id) {
                 { c.parent_of(id) } -> std::convertible_to<Id>;
             } && aura::core::AuraInvocable<V&, Id>
constexpr std::size_t walk_ancestors(C& ast, Id start, V&& vis) {
    using NC = aura::core::NullIdCheck<Id>;
    std::size_t count = 0;
    Id cur = start;
    while (!NC::is_null(cur)) {
        if (!static_cast<bool>(vis(cur)))
            return count;
        ++count;
        cur = ast.parent_of(cur);
    }
    return count;
}

// ── count_nodes_with_tag — tag-based DFS counter ────────────────
//
// Phase A.2 helper. DFS from root, count nodes whose tag
// equals `tag`. Thin wrapper over count_nodes_with_predicate
// for the common case of "count all Calls" / "count all
// Variables" / etc. — needed by (compile:*) stats primitives.
//
// Zero overhead: at -O3, the wrapper inlines into a single
// recursive DFS with a tag compare per node. Same codegen
// as a hand-written `count_nodes_with_predicate` with a
// tag-equality lambda.
//
// Precondition: same as count_nodes_with_predicate — root
// must be a valid node id.
export template <typename Id, typename C, typename Tag>
    requires aura::core::ASTContainer<C, Id>
[[nodiscard]] constexpr std::size_t count_nodes_with_tag(C& ast, Id root, Tag tag) {
    // The tag type is whatever ast.tag() returns (NodeTag for
    // FlatAST). We capture it by value in the lambda so the
    // recursive call passes it through without re-loading.
    return count_nodes_with_predicate<Id>(ast, root,
                                          [&ast, tag](Id id) { return ast.tag(id) == tag; });
}

export class FlatAST {
    // Issue #1431 follow-up (Race #2): FlatAST parser race. Two
    // threads concurrently calling CompilerService::eval share
    // the same workspace_flat_ FlatAST and race on add_node /
    // reset_node_slot push_backs, corrupting std::pmr memory_resource
    // and surfacing as SIGSEGV at pmr::memory_resource::allocate
    // (called from push_back via _M_realloc_append via the
    // polymorphic_allocator inside the NodeTag vector). TypeRegistry
    // Race #1 (the previous fix) used to crash first, hiding this
    // one. recursive_mutex because reset_node_slot is invoked from
    // inside add_node on the recycled-slot path.
    mutable std::recursive_mutex flatast_mutex_;

public:
public:
    // Issue #261 / #1299: node_gen_[id] == 0 marks a recycled (free-list)
    // or ghost-orphan slot. Public so query:* can skip freed ghosts after
    // mutation rollback (Phase 1 #1299/#1300).
    [[nodiscard]] bool is_free_slot(NodeId id) const noexcept {
        return id >= node_gen_.size() || node_gen_[id] == 0;
    }

private:
    void reset_node_slot(NodeId id, NodeTag tag, SyntaxMarker m) {
        tag_[id] = tag;
        int_val_[id] = 0;
        float_val_[id] = 0.0;
        sym_id_[id] = INVALID_SYM;
        children_[id] = PersistentChildVector<NodeId>{};
        param_begin_[id] = 0;
        param_count_[id] = 0;
        cap_require_count_[id] = 0;
        line_[id] = 0;
        col_[id] = 0;
        marker_[id] = m;
        type_id_[id] = 0;
        dirty_[id] = 0;
        if (id < ppa_dirty_.size())
            ppa_dirty_[id] = 0;
        // Issue #437: reset verify_dirty_ alongside ppa_dirty_
        if (id < verify_dirty_.size())
            verify_dirty_[id] = 0;
        // Issue #469: reset verification_dirty_ alongside the
        // other dirty columns. Populated by
        // apply_verification_dirty_bits (from
        // (verify:parse-coverage-feedback) /
        // (verify:parse-assert-failure)).
        if (id < verification_dirty_.size())
            verification_dirty_[id] = 0;
        // Issue #290: reset macro_dirty_ alongside the other
        // dirty columns. Populated by apply_macro_dirty_bits
        // (called from clone_macro_body for newly expanded
        // subtrees and from self-evolution loops that touch
        // macro-introduced nodes).
        if (id < macro_dirty_.size())
            macro_dirty_[id] = 0;
        error_kind_[id] = 0;
        if (id < value_cache_.size())
            value_cache_[id] = kNotCached;
        node_first_mutation_[id] = 0;
        // Issue #320: reset the per-node epoch column
        // (the slot is recycled; the epoch starts at 0
        // again, same semantics as a fresh node).
        if (id < last_seen_epoch_.size())
            last_seen_epoch_[id] = 0;
        // Issue #339: reset occ_stale_ alongside
        // last_seen_epoch_ (the slot is recycled; the
        // staleness starts at 0 again, same as a
        // fresh node).
        if (id < occ_stale_.size())
            occ_stale_[id] = 0;
        parent_[id] = NULL_NODE;
        // Issue #1689: recycled slot has no incoming edges.
        if (!incoming_parent_index_dirty_ && id < incoming_parent_edges_.size())
            incoming_parent_edges_[id].clear();
        node_gen_[id] = generation_;
    }

    [[nodiscard]] std::vector<bool> mark_live_nodes() const {
        std::vector<bool> live(size(), false);
        if (size() == 0)
            return live;
        std::vector<NodeId> queue;
        auto visit = [&](NodeId id) {
            if (id == NULL_NODE || id >= size() || is_free_slot(id) || live[id])
                return;
            live[id] = true;
            queue.push_back(id);
        };
        if (root != NULL_NODE)
            visit(root);
        for (const auto& [lid, _] : region_by_lambda_id_)
            visit(lid);
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            for (auto cid : children(queue[qi])) {
                if (cid != NULL_NODE)
                    visit(cid);
            }
        }
        return live;
    }

    // Issue #402: compute the summary-flag bits set by adding
    // a node of the given tag with default int_value=0 and
    // INVALID_SYM. Bits that depend on int_value (Lambda dotted
    // / TypeAnnotation var-form) or sym_id (keyword Variable,
    // query: / mutate: Call, user-binding Variable) are set
    // lazily when those values are written (see set_int_value,
    // set_sym_id) — this helper covers the tag-only path that
    // add_node takes for fresh nodes.
    [[nodiscard]] static constexpr std::uint32_t summary_flags_for_tag(NodeTag tag) noexcept {
        switch (tag) {
            case NodeTag::MacroDef:
                return static_cast<std::uint32_t>(SummaryFlag::HasMacroDef);
            case NodeTag::DefineType:
                return static_cast<std::uint32_t>(SummaryFlag::HasDefineType);
            case NodeTag::DefineModule:
                return static_cast<std::uint32_t>(SummaryFlag::HasDefineModule);
            case NodeTag::Set:
                return static_cast<std::uint32_t>(SummaryFlag::HasSet);
            default:
                return 0;
        }
    }

    // Issue #402: lazy bits set by set_int_value when the
    // value is non-zero (Lambda dotted / TypeAnnotation
    // var-form). Returns the bit(s) to OR-in, or 0 if the
    // tag + int_value combination doesn't trigger any flag.
    [[nodiscard]] static constexpr std::uint32_t
    summary_flags_for_int_value(NodeTag tag, std::int64_t v) noexcept {
        if (v == 0)
            return 0;
        switch (tag) {
            case NodeTag::Lambda:
                return static_cast<std::uint32_t>(SummaryFlag::HasLambdaDotted);
            case NodeTag::TypeAnnotation:
                return static_cast<std::uint32_t>(SummaryFlag::HasTypeAnnotVar);
            default:
                return 0;
        }
    }

    // Issue #402: lazy bits set by set_sym_id when the sym_id
    // is non-empty. Covers keyword Variables (':foo'),
    // query:/mutate: Call callees, and unresolved Variables
    // (root-level bare references). The user-binding /
    // tree-walker-only bits depend on cross-cutting state
    // (user_bindings_, ir_cache_) and are NOT set here — the
    // caller (needs_tree_walker_fallback) handles those via
    // the slow-path scan if it must.
    [[nodiscard]] static std::uint32_t
    summary_flags_for_sym_id(NodeTag tag, const std::string_view& name) noexcept {
        if (name.empty())
            return 0;
        switch (tag) {
            case NodeTag::Variable:
                if (name[0] == ':')
                    return static_cast<std::uint32_t>(SummaryFlag::HasKeywordVar);
                // Unresolved bare variables depend on runtime state
                // (primitives / ir_cache_), so we don't flag here.
                return 0;
            case NodeTag::Call: {
                if (name.starts_with("query:") || name.starts_with("mutate:"))
                    return static_cast<std::uint32_t>(SummaryFlag::HasQueryOrMutateCall);
                return 0;
            }
            default:
                return 0;
        }
    }

    // Issue #402: public accessors + mutator for the summary
    // bit-set. The fast-path in needs_tree_walker_fallback
    // calls summary_has() once per expression; the slow-path
    // scan calls summary_recompute() after every structural
    // mutation to keep the bit-set in sync.
public:
    [[nodiscard]] std::uint32_t summary_flags() const noexcept { return summary_flags_; }
    [[nodiscard]] bool summary_has(SummaryFlag flag) const noexcept {
        return (summary_flags_ & static_cast<std::uint32_t>(flag)) != 0;
    }
    // OR-in additional bits (used by add_node + the lazy
    // set_int_value / set_sym_id sites).
    void summary_add_flags(std::uint32_t bits) noexcept { summary_flags_ |= bits; }
    // Clear (used by reset() and the heavy recompute path).
    void summary_clear() noexcept { summary_flags_ = 0; }
    // Issue #402: rebuild the bit-set from scratch by walking
    // the entire tag_ column. O(n) but only called from test
    // code or after heavy structural mutations that may have
    // invalidated the eager-update assumptions. NEVER call
    // this on the fast path of every eval — the whole point
    // of #402 is to avoid that.
    void summary_recompute() {
        std::uint32_t f = 0;
        for (std::size_t i = 0; i < tag_.size(); ++i) {
            if (i < node_gen_.size() && node_gen_[i] == 0)
                continue; // free slot, skip
            const auto t = tag_[i];
            f |= summary_flags_for_tag(t);
            f |= summary_flags_for_int_value(t, int_val_[i]);
            // Note: sym_id-based bits (keyword / query: /
            // mutate:) require pool resolution and are NOT
            // recomputed here. They are caught by the
            // fast-path subtree walk (which has the pool via
            // its caller).
        }
        summary_flags_ = f;
    }

    [[nodiscard]] NodeId add_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
        // Issue #1431 follow-up Race #2: serialize FlatAST node
        // allocation across threads. Two CompilerService::eval
        // callers share workspace_flat_ and race on the per-node
        // push_backs (tag_ / int_val_ / float_val_ / sym_id_ /
        // children_ / etc.), corrupting std::pmr memory_resource
        // and crashing at pmr::memory_resource::allocate from
        // _M_realloc_append. recursive_mutex because reset_node_slot
        // is invoked on the recycled-slot path (free_list_.back()).
        std::lock_guard<std::recursive_mutex> lock(flatast_mutex_);
        // Issue #402: tag-based summary flags. Update eagerly
        // before allocation so a fast-path consumer (next
        // add_node's caller) sees the correct bit-set.
        summary_flags_ |= summary_flags_for_tag(tag);
        // Issue #399: pre-reserve the per-node "dirty"
        // side-table columns to the upcoming size. The
        // push_back(0) calls below would otherwise trigger
        // occasional 2x reallocations during AI-driven
        // structural mutations; reserving up-front makes
        // them O(1) size updates and keeps the mark_dirty
        // hot path free of any dirty_.resize() call. Also
        // keeps the invariant dirty_.size() == tag_.size()
        // explicit (not just implicit from push_back
        // ordering).
        const auto upcoming_size = static_cast<std::size_t>(tag_.size()) + 1;
        dirty_.reserve(upcoming_size);
        ppa_dirty_.reserve(upcoming_size);
        verify_dirty_.reserve(upcoming_size);
        verification_dirty_.reserve(upcoming_size);
        macro_dirty_.reserve(upcoming_size);
        if (!free_list_.empty()) {
            auto id = free_list_.back();
            free_list_.pop_back();
            reset_node_slot(id, tag, m);
            node_slot_reuse_count_.fetch_add(1, std::memory_order_relaxed);
            return id;
        }
        auto id = static_cast<NodeId>(tag_.size());
        tag_.push_back(tag);
        int_val_.push_back(0);
        float_val_.push_back(0.0);
        sym_id_.push_back(INVALID_SYM);
        // Issue #220: init the per-node children_ entry (uses
        // the outer's allocator, which is the default for now).
        children_.emplace_back();
        param_begin_.push_back(0);
        param_count_.push_back(0);
        cap_require_count_.push_back(0);
        line_.push_back(0);
        col_.push_back(0);
        marker_.push_back(m);
        // Issue #367: provenance column (parallel to marker_).
        // 0 = no provenance. Stamped by hygiene / mutation
        // primitives later via set_provenance().
        provenance_.push_back(0);
        type_id_.push_back(0);
        // Issue #412: parallel type_cache_gen_ column. 0 = no
        // cache entry yet (matches type_id_ == 0 semantics).
        type_cache_gen_.push_back(0);
        // Issue #412 follow-up #1: parallel
        // type_cache_binding_gen_ column. 0 = no binding
        // context (e.g. a top-level expression that
        // doesn't depend on any binding). Bumped on
        // structural changes to the binding the cache
        // entry is for.
        type_cache_binding_gen_.push_back(0);
        dirty_.push_back(0);
        ppa_dirty_.push_back(0);
        // Issue #437: verify_dirty_ column. Mirrors ppa_dirty_'s
        // push_back(0) pattern; populated by apply_verify_dirty_bits.
        verify_dirty_.push_back(0);
        // Issue #469: verification_dirty_ column. Mirrors
        // verify_dirty_'s push_back(0) pattern; populated by
        // apply_verification_dirty_bits (from the
        // verify:parse-coverage-feedback /
        // verify:parse-assert-failure primitives).
        verification_dirty_.push_back(0);
        // Issue #290: macro_dirty_ column. Mirrors
        // verification_dirty_'s pattern; populated by
        // apply_macro_dirty_bits (called from clone_macro_body
        // and self-evolution loops). 2 bits defined:
        //   kMacroExpansion = 0x01
        //   kMacroSelfModify = 0x02
        macro_dirty_.push_back(0);
        // Issue #390: per-node schema cache. 0 = no
        // schema (matches type_id_ == 0 semantics).
        // Populated by clone_macro_body from the
        // source node's schema_cache_ (or type_id_
        // as a fallback). The schema cache is a
        // pre-computed type for macro-introduced
        // nodes that the type checker can use as a
        // cache hit signal — avoids re-inferring
        // types for nodes whose schema was already
        // determined by the macro definition.
        schema_cache_.push_back(0);
        // Issue #79: per-node error kind (0 = no error, non-zero = ErrorKind).
        // Populated by the type-checker and runtime evaluator; queryable via
        // the AuraQuery `(has-error? N)` clause.
        error_kind_.push_back(0);
        // Issue #339: per-node occurrence-narrowing
        // staleness column. New nodes start fresh
        // (0 = not stale).
        occ_stale_.push_back(0);
        // value cache initialized lazily (not in arena — module-level vector)
        if (id >= static_cast<NodeId>(value_cache_.size()))
            value_cache_.resize(id + 1, kNotCached);
        else
            value_cache_[id] = kNotCached;
        node_first_mutation_.push_back(0);
        // Issue #320: per-node epoch tracking. New
        // nodes start at 0 (which compares != any real
        // mutation_epoch_ >= 1, so the first
        // synthesize_flat after a mutation will treat
        // them as "stale" once and then settle into a
        // stable state).
        last_seen_epoch_.push_back(0);
        parent_.push_back(NULL_NODE);
        // Issue #1689: keep inverted index parallel to node columns when valid.
        if (!incoming_parent_index_dirty_) {
            if (incoming_parent_edges_.size() < tag_.size())
                incoming_parent_edges_.resize(tag_.size());
            // New id is at tag_.size()-1 after push above; ensure slot exists.
            if (incoming_parent_edges_.size() <= static_cast<std::size_t>(id))
                incoming_parent_edges_.resize(static_cast<std::size_t>(id) + 1);
            incoming_parent_edges_[id].clear();
        }
        node_gen_.push_back(generation_);
        return id;
    }

    // SoA storage (all pmr::vector = arena allocated)
    std::pmr::vector<NodeTag> tag_;
    std::pmr::vector<std::int64_t> int_val_;
    std::pmr::vector<double> float_val_;
    std::pmr::vector<SymId> sym_id_;
    // Issue #220/221: per-node children. Each node has its own
    // PersistentChildVector<NodeId> (a COW / immutable vector
    // defined in src/core/persistent_child_vector.hh). The
    // accessors (children(), set_child(), insert_child(),
    // remove_child(), get().children) read through the PCV
    // span. The add_X methods build a per-node list from their
    // child NodeIds and store it via the PCV range constructor
    // (one allocation per node, no per-element COW).
    //
    // The PCV's storage is reference-counted via
    // std::shared_ptr, so back-references (e.g. a closure that
    // captured the pre-mutation children list) stay valid. This
    // is the foundation for #221 slice 3/5 (#177 rollback).
    // Heap std::vector (not pmr/arena): pmr::vector realloc was
    // leaving aliased PCV slots sharing one control block (#300).
    std::vector<PersistentChildVector<NodeId>> children_;
    // Issue #222: shared_mutex that guards structural mutations
    // (set_child / insert_child / remove_child). Acquired
    // exclusively via begin_structural_mutation() (the public
    // RAII guard) by every mutator. Readers can either:
    //   (a) Acquire a shared lock for the duration of their
    //       read (via try_acquire_reader_lock()), or
    //   (b) Read lock-free and verify generation_ after the
    //       read (the existing pattern; safe because PCV is
    //       immutable + COW).
    // The mutex ensures that (COW-mutation + parent_ update
    // + generation bump + dirty mark) is atomic w.r.t. other
    // mutators AND readers that explicitly acquire the lock.
    // PCV's COW semantics mean that readers NOT holding the
    // lock still see consistent data (they read the old
    // shared_ptr to the pre-mutation storage).
    //
    // Wrapped in OwnedSharedMutex because std::shared_mutex is
    // neither copyable nor movable, which would otherwise delete
    // FlatAST's implicit copy/move constructors (needed by
    // WorkspaceTree::ensure_local_flat's `*new_flat = *parent_flat_`
    // path). The wrapper allocates a FRESH mutex on copy (each
    // FlatAST instance gets its own mutex — correct for
    // independent mutation isolation), and the move constructor
    // transfers ownership (the moved-from FlatAST keeps its
    // mutex as nullptr; the moved-to owns it).
    OwnedSharedMutex structural_mtx_;
    // Issue #1783: shared_mutex for marker_ / provenance_ columns.
    // Metadata-only — does NOT bump generation_ (unlike
    // StructuralMutationGuard). Writers take exclusive lock via
    // begin_metadata_mutation(); readers take shared via
    // try_acquire_metadata_reader_lock(). Serializes cross-fiber
    // syntax:set-marker / set-provenance / propagate-marker vs
    // syntax-marker / get-provenance without invalidating StableNodeRef.
    OwnedSharedMutex metadata_mtx_;
    std::pmr::vector<NodeId> parent_;
    // Issue #1689: inverted multi-parent edge index.
    // For each child NodeId, list of (parent, child_index) edges that
    // reference it. Enables O(deg) parent lookup for remove-node (vs O(N*C)).
    // Mutable: rebuild may run from const collect/lookup paths.
    // When dirty, next lookup rebuilds from children_; locked structural
    // mutates keep the index fresh via incremental updates.
    mutable std::pmr::vector<std::pmr::vector<std::pair<NodeId, std::uint32_t>>>
        incoming_parent_edges_;
    mutable bool incoming_parent_index_dirty_ = true;
    mutable std::atomic<std::uint64_t> incoming_parent_index_rebuilds_{0};
    mutable std::atomic<std::uint64_t> incoming_parent_index_lookups_{0};
    mutable std::atomic<std::uint64_t> incoming_parent_index_hits_{0};
    std::pmr::vector<std::uint32_t> param_begin_;
    std::pmr::vector<std::uint32_t> param_count_;
    std::pmr::vector<std::uint32_t> cap_require_count_;
    std::pmr::vector<SymId> param_data_;
    std::pmr::vector<NodeId> param_annot_data_; // per-param annotation node IDs (NULL_NODE = none)
    // Source location (line/col, 1-based)
    std::pmr::vector<std::uint32_t> line_;
    std::pmr::vector<std::uint32_t> col_;
    // Type information (L6.5+): type_id per node, 0 = DYNAMIC
    std::pmr::vector<SyntaxMarker> marker_;
    // Issue #367: per-node provenance id (0 = no provenance).
    // Refers to the side-table in MarkerProvenanceTable; the
    // table stores macro_def_id + expansion_id + mutation_id
    // for tracing "where did this node come from?" for
    // AI-agent auditability. Adding a column (rather than
    // fields on Node) keeps the per-node struct size
    // unchanged and lets us add provenance at any time
    // without touching every FlatAST consumer.
    std::pmr::vector<std::uint32_t> provenance_;
    std::pmr::vector<std::uint8_t> dirty_;
    // Issue #277: per-node PPA dirty bitmask (orthogonal to DirtyReason
    // which is full at 8 bits). OR'd with kPpaHintDirty on dirty_ when set.
    std::pmr::vector<std::uint8_t> ppa_dirty_;
    // Issue #437: per-node verification dirty bitmask (orthogonal to
    // DirtyReason which is full at 8 bits). Bit definitions:
    //   0x01 = Assertion (assertion failed / SVA property violated)
    //   0x02 = Coverage (coverage hole detected)
    //   0x04 = SVA (SVA property / sequence affected)
    //   0x08 = FormalCounterexample (formal proof counterexample)
    // OR'd with kGeneralDirty on dirty_ when set so legacy
    // is_dirty() callers still see "this node needs work".
    std::pmr::vector<std::uint8_t> verify_dirty_;
    // Issue #469: verification_dirty_ column. Mirrors
    // verify_dirty_'s pattern; populated by
    // apply_verification_dirty_bits (called from
    // (verify:parse-coverage-feedback) /
    // (verify:parse-assert-failure)). 2 bits defined:
    //   kCoverageFeedbackDirty = 0x01
    //   kAssertFailureDirty = 0x02
    std::pmr::vector<std::uint8_t> verification_dirty_;
    // Issue #290: macro_dirty_ column. Mirrors
    // verification_dirty_'s pattern; populated by
    // apply_macro_dirty_bits (called from clone_macro_body
    // after a hygienic macro expansion, and from
    // self-evolution loops that touch macro-introduced
    // nodes). 2 bits defined:
    //   kMacroExpansion = 0x01 (a freshly cloned macro body)
    //   kMacroSelfModify = 0x02 (a self-evolution step
    //                            mutated a macro-introduced
    //                            node)
    std::pmr::vector<std::uint8_t> macro_dirty_;
    // Issue #339: per-node occurrence-narrowing staleness
    // column. When a mutation affects a predicate or a
    std::pmr::vector<std::uint32_t> type_id_;
    // Issue #412: per-node type cache generation. Parallel to
    // type_id_; stores the type_cache_generation_ at the time
    // the cache entry was populated. On cache hit, the
    // type-checker compares this against the current
    // type_cache_generation_ — if they differ, the entry was
    // cached before a structural mutation that may have
    // invalidated the type, so we re-infer. This augments the
    // existing reg_.free_vars(tid).empty() check by catching
    // cases where the free_vars check is too permissive (e.g.,
    // a polymorphic type that still has TYPE_VAR children but
    // the binding structure didn't change). 0 = no cache
    // entry yet (matches type_id_ == 0 semantics).
    std::pmr::vector<std::uint32_t> type_cache_gen_;
    // Issue #412 follow-up #1: per-binding generation
    // column. Parallel to type_cache_gen_; stores the
    // gen of THE SPECIFIC BINDING the cache entry is
    // for (0 = no binding context, e.g. a top-level
    // expression that doesn't depend on any binding).
    // The per-binding gen is bumped only when THAT
    // specific binding's structure changes (a separate
    // bump from the global type_cache_generation_ which
    // bumps on every dirty event). On cache hit, the
    // check is: global gen matches AND per-binding gen
    // matches → entry is fresh. This is finer-grained
    // than the global gen alone (which over-invalidates
    // when a different binding mutates).
    std::pmr::vector<std::uint32_t> type_cache_binding_gen_;
    // Issue #390: per-node schema cache column.
    // Parallel to type_id_; stores the pre-computed
    // type for nodes whose schema was determined by
    // macro definition (clone_macro_body). 0 = no
    // schema (the type checker will infer normally).
    // Non-zero = the type checker can use this as a
    // cache hit signal (avoids re-inference).
    std::pmr::vector<std::uint32_t> schema_cache_;
    // Issue #412 follow-up #1: per-binding generation
    // map. SymId → uint32_t. Each binding has its own
    // gen counter that bumps only when THAT binding's
    // structure changes (e.g. mutate:rebind on a
    // top-level define). Bumping on every dirty event
    // would be over-invalidating (the current global
    // gen behavior). The per-binding gen rescues cache
    // entries that don't depend on the mutated binding
    // from being re-inferred.
    //
    // Non-pmr / non-atomic — std::pmr allocator's
    // uses_allocator_args + std::atomic + std::tuple
    // don't compose well. The mutation primitives
    // synchronize via the mutation lock
    // (enter_mutation_boundary), so concurrent access
    // isn't a concern. The map is small (only the
    // bindings that have been touched are in it), so
    // std::unordered_map's default allocator is fine.
    // COW path creates a fresh map on the clone so
    // mutations on the clone don't affect the parent.
    struct BindingGenMap {
        std::unordered_map<aura::ast::SymId, std::uint32_t> gens;
        BindingGenMap() = default;
    };
    std::shared_ptr<BindingGenMap> binding_gens_ = std::make_shared<BindingGenMap>();
    // Issue #79: per-node error kind, 0 = no error, non-zero = ErrorKind
    // enum value. Populated by the type-checker and runtime evaluator;
    // queryable via the AuraQuery `(has-error? N)` clause.
    std::pmr::vector<std::uint8_t> error_kind_;
    // Issue #447: tag+arity pre-index for query:pattern.
    // Index: (NodeTag, arity) → list of NodeIds matching.
    // arity is a 16-bit value (high byte = min children,
    // low byte = max children; both 0xFF for "any").
    // Issue #1371: hash map (tag, arity) → vector<NodeId>
    // for O(1) find_by_tag_arity (was linear vector scan).
    // Built lazily on first query:pattern / ensure call.
    //
    // Issue #1636 / #1609 / #1501: MacroIntroduced hygiene is **not**
    // packed into TagArityKey. Evaluator maintains a parallel
    // tag_arity_index_user_ (user-marker only) served when
    // skip_macro_introduced=true — same hot-path O(1) win without
    // exploding key space or delta-pack complexity.
    struct TagArityKey {
        std::uint32_t tag;
        std::uint16_t arity_min;
        std::uint16_t arity_max;
        bool operator==(const TagArityKey& o) const noexcept {
            return tag == o.tag && arity_min == o.arity_min && arity_max == o.arity_max;
        }
    };
    struct TagArityKeyHash {
        std::size_t operator()(const TagArityKey& k) const noexcept {
            // FNV-1a style mix of tag + arity range.
            std::uint64_t h = 14695981039346656037ull;
            auto mix = [&](std::uint64_t x) noexcept {
                h ^= x;
                h *= 1099511628211ull;
            };
            mix(static_cast<std::uint64_t>(k.tag));
            mix(static_cast<std::uint64_t>(k.arity_min));
            mix(static_cast<std::uint64_t>(k.arity_max));
            return static_cast<std::size_t>(h);
        }
    };
    using TagArityIndexMap =
        std::pmr::unordered_map<TagArityKey, std::pmr::vector<NodeId>, TagArityKeyHash>;
    TagArityIndexMap tag_arity_index_;
    // AST size() after last full rebuild or successful delta sync.
    // Enables append-only delta: [built_size, size()).
    std::size_t tag_arity_index_built_size_ = 0;
    // Issue #1503: per-node packed (tag<<32|arity) key last inserted
    // into tag_arity_index_. ~0 = not indexed. Enables O(1) remove
    // for dirty-node incremental patch (vs full O(N) rebuild).
    static constexpr std::uint64_t kTagArityNodeKeyNone = ~std::uint64_t{0};
    std::pmr::vector<std::uint64_t> tag_arity_node_key_;
    // Issue #1503: when dirty_count * 100 > size * pct, prefer full
    // rebuild over per-dirty patch (default 25%). Agent-tunable.
    std::uint8_t tag_arity_index_full_rebuild_threshold_pct_ = 25;
    // Issue #447: index hit/miss counters (stats-only,
    // relaxed-ordering). Exposed via (query:query-stats)
    // primitive.
    mutable std::atomic<std::uint64_t> tag_arity_index_hits_{0};
    mutable std::atomic<std::uint64_t> tag_arity_index_misses_{0};
    mutable std::atomic<std::uint64_t> tag_arity_index_rebuilds_{0};
    // Issue #554: rebuild timing + delta update counters.
    //   - tag_arity_index_rebuild_time_us_  (# lifetime
    //     microseconds spent in rebuild_tag_arity_index)
    //   - tag_arity_index_delta_hits_       (# of times
    //     the incremental delta path was taken vs full
    //     rebuild — the AI Agent reads this to measure
    //     the incremental-savings optimization)
    mutable std::atomic<std::uint64_t> tag_arity_index_rebuild_time_us_{0};
    mutable std::atomic<std::uint64_t> tag_arity_index_delta_hits_{0};
    // Issue #1503: full rebuilds chosen by dirty-fraction threshold
    // vs incremental dirty-node patches.
    mutable std::atomic<std::uint64_t> tag_arity_index_threshold_full_rebuilds_{0};
    mutable std::atomic<std::uint64_t> tag_arity_index_incremental_patches_{0};
    // Issue #547: dirty flag + mark counter for the
    // tag_arity_index. mark_dirty_upward + structural mutate
    // paths set this flag; rebuild_tag_arity_index() /
    // rebuild_tag_arity_index_delta() clear it. Callers use
    // ensure_tag_arity_index() to choose delta vs full rebuild.
    // Bump counter (tag_arity_index_dirty_marks_) is stats-
    // only and exposed via (query:pattern-index-stats).
    mutable std::atomic<bool> tag_arity_index_dirty_{false};
    mutable std::atomic<std::uint64_t> tag_arity_index_dirty_marks_{0};
    // Module-level eval result cache (int64_t = EvalValue serialization).
    // Used by Evaluator::eval_flat for incremental evaluation (Issue #32b).
    // Indexed by NodeId. Zero = not cached. Stored at module level (not arena)
    // because the evaluator outlives individual arena scopes.
    // Issue #67: pmr::vector (was std::vector) — uses the same
    // polymorphic_allocator as the rest of the FlatAST's SoA storage.
    // When the FlatAST is destroyed, the pmr vectors' heap buffers
    // are released along with the rest of the arena's memory.
    // This eliminates the need for free_persistent_state() at process
    // exit.
    std::pmr::vector<std::int64_t> value_cache_;
    std::pmr::vector<MutationRecord> mutation_log_;
    // Issue #1419: provenance context stamped into new MutationRecords
    // (author / parent chain / composite transaction). Defaults 0.
    std::uint64_t mutation_author_fingerprint_ = 0;
    std::uint64_t mutation_parent_mutation_id_ = 0;
    std::uint64_t mutation_composite_transaction_id_ = 0;
    // Issue #282: Occurrence Typing provenance log. Each entry
    // is captured when synthesize_flat_if applies a narrowing
    // (predicate → refined type in a branch). Exposed via
    // (query:provenance-of var-name) and all_narrowings() /
    // narrowing_count() accessors. The log is cleared on
    // full FlatAST reset, same lifecycle as mutation_log_.
    std::pmr::vector<NarrowingRecord> narrowing_log_;
    // Issue #639: count of narrowing records marked stale by
    // invalidate_narrowings_in_subtree (post-mutate invalidation).
    std::uint64_t narrow_invalidation_post_mutate_ = 0;
    // Issue #402: bit-set of fallback-relevant node tags.
    // Computed eagerly in add_node so the fast-path in
    // needs_tree_walker_fallback can skip the O(n)
    // scan when no fallback-relevant tag is present
    // (the common case for `(+ 1 2)`-style expressions).
    // The bit-set is conservative: a 0 means "no tag
    // in the slow-scan set is present", but unresolved
    // Variables / query:/mutate: Calls still need
    // verification — those are caught by the subtree
    // walk (also added in #402).
    std::uint32_t summary_flags_ = 0;
    std::pmr::vector<std::uint32_t> node_first_mutation_;
    // Issue #320: per-node epoch tracking. Records the
    // last mutation_epoch_ at which this node was
    // touched (marked dirty or structurally mutated).
    // The TypeChecker's synthesize_flat cache uses this
    // to detect which specific nodes need re-inference
    // (per-node invalidation) vs the coarse whole-cache
    // gate that #168 ships. The column is a
    // parallel-array SoA vector; live nodes always
    // carry a valid epoch (starts at 0, which
    // compares != any real mutation_epoch_ >= 1).
    std::pmr::vector<std::uint64_t> last_seen_epoch_;
    // Issue #339: per-node occurrence-narrowing
    // staleness column. (Declared after last_seen_epoch_
    // to match the init-list order in all 3 ctors;
    // -Wreorder compliance.) When a mutation affects a
    // predicate or a narrowed var, the type checker
    // marks the affected if-nodes' occurrence context
    // as stale (the dirty_ byte is full with 8
    // DirtyReason bits; this column is the orthogonal
    // side-table, like verify_dirty_ /
    // verification_dirty_ / macro_dirty_). The narrow
    // helper: 1 = stale (must re-analyze before use),
    // 0 = fresh.
    std::pmr::vector<std::uint8_t> occ_stale_;
    std::uint64_t next_mutation_id_ = 1;
    std::uint16_t generation_ = 1;
    std::pmr::vector<std::uint16_t> node_gen_;
    // Issue #261: free-list of recycled NodeId slots. Slots on the
    // free list have node_gen_[id] == 0 (tombstone). add_node()
    // pops from here before bump-appending new slots.
    std::pmr::vector<NodeId> free_list_;
    // Issue #255: reference stability observability counters.
    // The reference stability mechanism (generation_ + node_gen_
    // + StableNodeRef) is a candidate for std::meta-based
    // refactor (P2996); until that lands in a compiler, these
    // counters let users audit how often the mechanism fires.
    // Atomic so concurrent mutates (lockless path) and reader
    // is_valid checks don't race. Exposed via accessors below
    // for service.ixx to accumulate into CompilerMetrics.
    // Mutable so const is_valid() overloads can bump them
    // (the increment is observability, not a logical state
    // change — the validation result is the same).
    mutable std::atomic<std::uint64_t> bump_generation_count_{0};
    mutable std::atomic<std::uint64_t> is_valid_check_count_{0};
    mutable std::atomic<std::uint64_t> stable_ref_invalidations_{0};
    // Issue #392: scoped / per-subtree generation bumping for
    // reduced StableRef over-invalidation. subtree_bump_count_
    // tracks lifetime total of bump_generation_subtree() calls.
    // subtree_gen_ is the per-top-level-Define generation map
    // (only meaningful for Define roots; other slots stay at 0).
    mutable std::atomic<std::uint64_t> subtree_bump_count_{0};
    std::pmr::vector<std::uint16_t> subtree_gen_;
    // Issue #457: generation_ / node_gen_ lifecycle
    // observability counters. All stats-only (relaxed
    // ordering). Bumped in bump_generation() (wrap
    // detection), is_valid() (stale access), and
    // StableNodeRef validation (invalidation).
    // Exposed via the (query:stable-ref-stats) primitive.
    //
    // Issue #1964 cycle 2d: generation_ + wrap_epoch_ +
    // subtree_gen_ are per-AST fields (each FlatAST instance
    // has its own). They do NOT migrate to the global
    // WorkspaceEpoch::Generation / Wrap / Subtree atomics
    // (those are process-global and architecturally distinct).
    // Per-AST semantics are intentional — each FlatAST tracks
    // its own reference stability state. The WorkspaceEpoch
    // type (src/core/workspace_epoch.hh) provides the
    // vocabulary for cycle 2b-style migrations; per-AST fields
    // remain. See docs/agent-safety-mechanisms-simplification.md
    // for the full invariant table.
    mutable std::atomic<std::uint64_t> generation_wrap_count_{0};
    mutable std::atomic<std::uint64_t> node_gen_stale_access_count_{0};
    mutable std::atomic<std::uint64_t> atomic_batch_commits_{0};
    // Issue #368: wrap_epoch_ is bumped each time generation_
    // (uint16_t) wraps back to 1. Captured in StableNodeRef at
    // make_ref() time and checked in is_valid(). With uint16_t
    // generation_, wrap happens every 65535 structural
    // mutations; without the epoch check, a ref captured before
    // the first wrap could become a false positive after a
    // SECOND wrap returns generation_ to its prior value
    // (~130K mutates in). epoch_ is atomic uint32_t: needs
    // ~4 billion * 65535 = ~2.6e14 mutates to wrap, which is
    // well outside the lifetime of any realistic workspace.
    mutable std::atomic<std::uint32_t> wrap_epoch_{0};
    // Issue #369: per-category counters for the structural
    // rollback dispatcher. Bumped in
    // try_rollback_structural_child_op on success vs.
    // best-effort skip. Exposed via (ast:generation-stats) so
    // AI agents can find structural ops that still rely on
    // legacy add_mutation() and miss the children_ restore.
    mutable std::atomic<std::uint64_t> structural_rollback_success_{0};
    mutable std::atomic<std::uint64_t> structural_rollback_besteffort_{0};
    // Issue #1282: set when generation_ wraps; consumed by
    // restamp_all_node_generations / maybe_auto_restamp_on_wrap so
    // live node_gen_ is restamped without manual agent intervention.
    mutable std::atomic<bool> auto_restamp_pending_{false};
    mutable std::atomic<std::uint64_t> auto_restamp_on_wrap_count_{0};
    // Issue #1281: children topology restore via PCV snapshot count.
    mutable std::atomic<std::uint64_t> children_topology_restore_count_{0};
    // Issue #1502: parent_ column restored (snapshot or rebuild-from-children)
    // after failed atomic-batch / MutationBoundary. Pairs with
    // children_topology_restore_count_ for full topology fidelity.
    mutable std::atomic<std::uint64_t> parent_topology_restore_count_{0};
    // Issue #1299/#1300: orphan ghost nodes freed on rollback.
    mutable std::atomic<std::uint64_t> ghost_orphan_nodes_freed_{0};
    // Issue #370: lifetime-safe view counter. Bumped in
    // children_safe(NodeId). Mirrors children_call_count_ for
    // the raw children(NodeId) accessor. AI agents can use
    // (children-call-count + children-safe-view-count) to
    // gauge how often their code crosses mutation boundaries
    // with cached views.
    mutable std::atomic<std::uint64_t> children_safe_view_count_{0};
    // Issue #1520: columnar children + region dense lookup metrics.
    mutable std::atomic<std::uint64_t> children_column_soa_hits_{0};
    mutable std::atomic<std::uint64_t> pcv_pin_count_{0};
    mutable std::atomic<std::uint64_t> region_dense_hits_{0};
    mutable std::atomic<std::uint64_t> region_map_lookups_{0};
    // Issue #678: parent_safe_view(NodeId) counter — mirrors
    // parent_of_call_count_ for generation-tagged parent access.
    mutable std::atomic<std::uint64_t> parent_safe_view_count_{0};
    // Issue #256: AST operation observability counters.
    // The hand-written AST operations (children, parent_of,
    // mark_dirty_upward) are candidates for std::meta-based
    // auto-generation once P2996 lands in a compiler. Until
    // then, these counters let users audit how often each
    // operation fires:
    // - children_call_count_: total children() calls
    //   (reads of the per-node children_ SoA column)
    // - parent_of_call_count_: total parent_of() calls
    //   (reads of the parent_ SoA column)
    // - mark_dirty_upward_call_count_: total
    //   mark_dirty_upward() invocations
    // - mark_dirty_total_nodes_: total nodes touched across
    //   all mark_dirty_upward() calls (sum of queue sizes).
    //   Divided by mark_dirty_upward_call_count_ gives the
    //   average depth of dirty propagation per mutation —
    //   a key metric for the std::meta refactor (if depth
    //   is high, the refactor should batch or skip).
    // Mutable so const children() / parent_of() overloads
    // can bump them (the increment is observability, not
    // a logical state change — the returned span is the same).
    mutable std::atomic<std::uint64_t> children_call_count_{0};
    mutable std::atomic<std::uint64_t> parent_of_call_count_{0};
    mutable std::atomic<std::uint64_t> mark_dirty_upward_call_count_{0};
    mutable std::atomic<std::uint64_t> mark_dirty_total_nodes_{0};
    // Issue #336: counter for the mark_dirty_upward_fast
    // early-exit hits (when the parent already has the
    // target reason bits). Surfaced via
    // (compile:dirty-fast-stats).
    mutable std::atomic<std::uint64_t> dirty_upward_fast_fixed_point_hits_{0};
    // Issue #471: SV-scale observability for dirty
    // propagation perf. The max_depth_observed_ tracks
    // the deepest mark_dirty_upward traversal seen on
    // this FlatAST (lifetime). The early_exit_count_ is
    // bumped when the early-exit fixed-point check
    // fires (parent already has the reason bits). The
    // ratio early_exit / call_count gives the early-exit
    // rate; max_depth gives the worst-case traversal.
    // Exposed via (query:dirty-propagation-stats).
    mutable std::atomic<std::uint64_t> mark_dirty_early_exit_count_{0};
    mutable std::atomic<std::uint64_t> mark_dirty_max_depth_observed_{0};
    // Issue #1251: early-exit truncations when depth/count bounds hit.
    mutable std::atomic<std::uint64_t> mark_dirty_truncated_count_{0};
    // Issue #1345: stop-at Define/Interface/Module boundary prune hits.
    mutable std::atomic<std::uint64_t> mark_dirty_boundary_prune_count_{0};
    // Issue #1651: calls to children_stable_span_view (zero-copy span-return alternative
    // to children_stable's std::vector allocation). Bumped in the new method below;
    // exposes the AI Agent hot-path `copy-avoided` count via (query:dirty-stats)
    // composition (no new primitive per #1632 "原语最小化"). Pairs with the existing
    // mark_dirty_early_exit_count_ (#1251) which covers the parallel dirty-side
    // zero-allocation optimization surface.
    mutable std::atomic<std::uint64_t> children_stable_span_calls_total_{0};
    // Issue #1348: auto soft-compact on atomic batch commit.
    mutable std::atomic<std::uint64_t> auto_compact_on_commit_count_{0};
    // Issue #1465: AST-level dirty short-circuit API surface.
    // is_subtree_dirty_node(NodeId) — O(1) check on a single node's
    //   dirty_ bit. Foundation for downstream per-subtree short-circuit
    //   in query/lower/eval hot paths.
    // dirty_nodes_in_range(NodeId start, NodeId end) — counts dirty
    //   nodes in [start, end). Useful for batch-query invocations
    //   that want to skip a range when count == 0.
    // Returns false / 0 if the AST is not yet dirty-tracked
    // (dirty_ column not built). Both are const and thread-safe.
    bool is_subtree_dirty_node(NodeId id) const noexcept {
        if (id == NULL_NODE || id >= size())
            return false;
        if (dirty_.empty())
            return false; // not built
        return dirty_[static_cast<std::size_t>(id)];
    }
    std::size_t dirty_nodes_in_range(NodeId start, NodeId end) const noexcept {
        if (start >= end || dirty_.empty())
            return 0;
        const auto s = static_cast<std::size_t>(start);
        const auto e = static_cast<std::size_t>(end);
        const auto cap = dirty_.size();
        const auto hi = (e > cap) ? cap : e;
        if (s >= hi)
            return 0;
        std::size_t count = 0;
        for (std::size_t i = s; i < hi; ++i)
            if (dirty_[i])
                ++count;
        return count;
    }
    mutable std::atomic<std::uint64_t> live_nodes_threshold_warn_count_{0};
    // Configurable soft-compact policy (#1348).
    std::size_t compaction_free_list_threshold_ = 1024;
    std::size_t max_live_nodes_warn_ = 1'000'000;
    // Issue #1251: rollback_to_size triggered soft compaction.
    mutable std::atomic<std::uint64_t> rollback_compaction_triggered_{0};
    // Issue #1319 Phase 1: structural mutate counters (insert/remove child).
    mutable std::atomic<std::uint64_t> structural_mutate_insert_total_{0};
    mutable std::atomic<std::uint64_t> structural_mutate_erase_total_{0};
    // Issue #1301: mutation_log_ suffix records dropped after rollback.
    mutable std::atomic<std::uint64_t> mutation_log_compacted_records_{0};
    mutable std::atomic<std::uint64_t> mutation_log_compact_ops_{0};
    // Issue #1355: render-hotpath lightweight checkpoints (field-only side log).
    mutable std::atomic<std::uint64_t> lightweight_total_{0};
    mutable std::atomic<std::uint64_t> lightweight_commit_total_{0};
    mutable std::atomic<std::uint64_t> lightweight_rollback_total_{0};
    mutable std::atomic<std::uint64_t> lightweight_records_total_{0};
    // Issue #412: per-FlatAST type cache generation counter.
    // Bumped by mark_dirty_upward() and by the explicit
    // bump_type_cache_generation() accessor. The
    // type-checker stores the current gen in
    // type_cache_gen_[id] when populating type_id_[id], and
    // compares on cache hit. 32-bit is enough for the
    // theoretical max (~4B dirty events per FlatAST lifetime
    // is far beyond any realistic mutation count).
    mutable std::atomic<std::uint32_t> type_cache_generation_{0};
    // Issue #412 follow-up #1: per-binding gen bump
    // counter. Bumped on every bump_binding_gen() call
    // (when a mark_dirty_upward targets a binding node).
    // Lifetime total; plumbed to CompilerMetrics via the
    // snapshot for observability.
    mutable std::atomic<std::uint64_t> binding_gen_bumps_total_{0};
    // Issue #413: mutation_log-integrated invalidation.
    // Records the (mutation_id, SymId) pair when
    // mark_dirty_upward bumps the per-binding gen. Lets
    // users trace "which mutation invalidated this
    // binding's cache entry" via
    // (compile:mutation-log-invalidation-trace
    // mutation-id).
    //
    // Non-pmr / non-atomic — same constraint as
    // binding_gens_ (std::pmr + atomic + tuple don't
    // compose). Mutation access is serialized via the
    // mutation lock.
    struct InvalidationRecord {
        aura::ast::SymId sym;
        std::uint64_t mutation_id = 0;
        std::uint32_t binding_gen_at_bump = 0;
    };
    std::pmr::vector<InvalidationRecord> invalidation_trace_;
    // Issue #261: NodeId lifecycle observability counters.
    mutable std::atomic<std::uint64_t> node_recycle_total_{0};
    mutable std::atomic<std::uint64_t> node_slot_reuse_count_{0};
    mutable std::atomic<std::uint64_t> node_compact_total_{0};
    // Issue #497: soft compaction + stale-ref auto-refresh observability.
    mutable std::atomic<std::uint64_t> soft_compact_count_{0};
    mutable std::atomic<std::uint64_t> stale_ref_auto_refresh_count_{0};
    // Issue #437: verification-dirty observability counters.
    // Bumped by apply_verify_dirty_bits. Stats-only
    // (relaxed-ordering). Exposed via the
    // (query:verify-dirty-stats) primitive.
    mutable std::atomic<std::uint64_t> verify_assertion_dirty_total_{0};
    mutable std::atomic<std::uint64_t> verify_coverage_dirty_total_{0};
    mutable std::atomic<std::uint64_t> verify_sva_dirty_total_{0};
    mutable std::atomic<std::uint64_t> verify_formal_cex_dirty_total_{0};
    // Issue #469: verification_dirty_ observability counters.
    // Bumped by apply_verification_dirty_bits. Stats-only
    // (relaxed-ordering). Exposed via the
    // (query:verification-loop-stats) primitive.
    mutable std::atomic<std::uint64_t> verification_coverage_feedback_total_{0};
    mutable std::atomic<std::uint64_t> verification_assert_failure_total_{0};
    // Issue #469: structured-mutate success/failure counters
    // (used by (query:verification-loop-stats) to compute
    // mutate_success_rate).
    mutable std::atomic<std::uint64_t> sv_mutate_attempts_total_{0};
    mutable std::atomic<std::uint64_t> sv_mutate_success_total_{0};
    mutable std::atomic<std::uint64_t> verify_loop_cycles_total_{0};
    // Issue #290: macro_dirty_ observability counters. Bumped
    // by apply_macro_dirty_bits. Stats-only (relaxed-ordering).
    // Exposed via the (compile:macro-dirty-stats) primitive.
    mutable std::atomic<std::uint64_t> macro_expansion_dirty_total_{0};
    mutable std::atomic<std::uint64_t> macro_self_modify_dirty_total_{0};

public:
    // Issue #437 / #1840: per-reason verify-dirty stat accessors.
    // Public so the (query:verify-dirty-stats) / SEVA primitives
    // can read them from evaluator_primitives_compile.cpp.
    // Issue #1840: acquire-load so concurrent apply_verify_dirty_bits
    // (fetch_add relaxed) is visible to stats readers (pre-#1840
    // used relaxed → SEVA readers could observe stale totals
    // indefinitely under weak orderings).
    [[nodiscard]] std::uint64_t verify_assertion_dirty_total() const noexcept {
        return verify_assertion_dirty_total_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t verify_coverage_dirty_total() const noexcept {
        return verify_coverage_dirty_total_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t verify_sva_dirty_total() const noexcept {
        return verify_sva_dirty_total_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t verify_formal_cex_dirty_total() const noexcept {
        return verify_formal_cex_dirty_total_.load(std::memory_order_acquire);
    }
    // Issue #1840: consistent 4-counter view for SEVA multi-field
    // reads (seva:fix-reset-bugs / query:seva-audit-log). Same
    // double-check acquire pattern as CompilerMetrics type-cache
    // snapshot (#1797) — no writer mutex on hot apply_verify_dirty_bits.
    struct VerifyDirtyTotalsSnapshot {
        std::uint64_t assertion = 0;
        std::uint64_t coverage = 0;
        std::uint64_t sva = 0;
        std::uint64_t formal_cex = 0;
    };
    [[nodiscard]] VerifyDirtyTotalsSnapshot snapshot_verify_dirty_totals() const noexcept {
        VerifyDirtyTotalsSnapshot s;
        for (int attempt = 0; attempt < 16; ++attempt) {
            s.assertion = verify_assertion_dirty_total_.load(std::memory_order_acquire);
            s.coverage = verify_coverage_dirty_total_.load(std::memory_order_acquire);
            s.sva = verify_sva_dirty_total_.load(std::memory_order_acquire);
            s.formal_cex = verify_formal_cex_dirty_total_.load(std::memory_order_acquire);
            if (verify_assertion_dirty_total_.load(std::memory_order_acquire) == s.assertion &&
                verify_coverage_dirty_total_.load(std::memory_order_acquire) == s.coverage &&
                verify_sva_dirty_total_.load(std::memory_order_acquire) == s.sva &&
                verify_formal_cex_dirty_total_.load(std::memory_order_acquire) == s.formal_cex) {
                return s;
            }
        }
        return s; // best-effort after retries
    }
    // Issue #469: verification_dirty_ stat accessors.
    [[nodiscard]] std::uint64_t verification_coverage_feedback_total() const noexcept {
        return verification_coverage_feedback_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t verification_assert_failure_total() const noexcept {
        return verification_assert_failure_total_.load(std::memory_order_relaxed);
    }
    // Issue #469: structured-mutate stat accessors.
    [[nodiscard]] std::uint64_t sv_mutate_attempts_total() const noexcept {
        return sv_mutate_attempts_total_.load(std::memory_order_relaxed);
    }
    // Issue #290: macro_dirty_ stat accessors.
    [[nodiscard]] std::uint64_t macro_expansion_dirty_total() const noexcept {
        return macro_expansion_dirty_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t macro_self_modify_dirty_total() const noexcept {
        return macro_self_modify_dirty_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t sv_mutate_success_total() const noexcept {
        return sv_mutate_success_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t verify_loop_cycles_total() const noexcept {
        return verify_loop_cycles_total_.load(std::memory_order_relaxed);
    }
    // Issue #469: bump helpers for the structured-mutate
    // counters (called from the (mutate:sv-add-coverpoint) /
    // (mutate:sv-weaken-property) primitives).
    void bump_sv_mutate_attempt() noexcept {
        sv_mutate_attempts_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_sv_mutate_success() noexcept {
        sv_mutate_success_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_verify_loop_cycle() noexcept {
        verify_loop_cycles_total_.fetch_add(1, std::memory_order_relaxed);
    }

public:
    // Issue #437: verify_dirty_ accessor (public, used by
    // the (compile:verify-dirty?) primitive).
    [[nodiscard]] std::uint8_t verify_dirty(NodeId id) const noexcept {
        if (id >= verify_dirty_.size())
            return 0;
        return verify_dirty_[id];
    }
    // Issue #469: verification_dirty_ accessor is defined
    // later in this file (after apply_verification_dirty_bits
    // is declared). See the second declaration.
    // Issue #456: public accessor for the main dirty_ byte
    // (used by (query:dirty-subtree) to check each node's
    // dirty bitmask against a reason filter).
    [[nodiscard]] std::uint8_t dirty(NodeId id) const noexcept {
        if (id >= dirty_.size())
            return 0;
        return dirty_[id];
    }
    // Issue #447 / #1371 / #1503: tag+arity pre-index for query:pattern.
    // Full rebuild of the (tag, arity) → vector<NodeId> hash map
    // over all live nodes. Idempotent. Stats counters bumped.
    [[nodiscard]] static std::uint64_t pack_tag_arity_node_key(std::uint32_t tag,
                                                               std::size_t arity) noexcept {
        return (static_cast<std::uint64_t>(tag) << 32) | static_cast<std::uint64_t>(arity);
    }
    [[nodiscard]] TagArityKey unpack_tag_arity_node_key(std::uint64_t packed) const noexcept {
        return TagArityKey{static_cast<std::uint32_t>(packed >> 32),
                           static_cast<std::uint16_t>(packed & 0xFFFFu),
                           static_cast<std::uint16_t>(packed & 0xFFFFu)};
    }
    void tag_arity_index_remove_id(NodeId id) noexcept {
        if (id >= tag_arity_node_key_.size())
            return;
        const auto packed = tag_arity_node_key_[id];
        if (packed == kTagArityNodeKeyNone)
            return;
        const TagArityKey key = unpack_tag_arity_node_key(packed);
        auto it = tag_arity_index_.find(key);
        if (it != tag_arity_index_.end()) {
            auto& bucket = it->second;
            bucket.erase(std::remove(bucket.begin(), bucket.end(), id), bucket.end());
            if (bucket.empty())
                tag_arity_index_.erase(it);
        }
        tag_arity_node_key_[id] = kTagArityNodeKeyNone;
    }
    void tag_arity_index_insert_id(NodeId id) noexcept {
        if (id >= size())
            return;
        if (is_free_slot(id))
            return;
        const auto tag = static_cast<std::uint32_t>(tag_[id]);
        const std::size_t ar = children_[id].size();
        const auto packed = pack_tag_arity_node_key(tag, ar);
        if (id >= tag_arity_node_key_.size())
            tag_arity_node_key_.resize(static_cast<std::size_t>(id) + 1, kTagArityNodeKeyNone);
        // Already indexed under same key — no-op.
        if (tag_arity_node_key_[id] == packed)
            return;
        if (tag_arity_node_key_[id] != kTagArityNodeKeyNone)
            tag_arity_index_remove_id(id);
        const TagArityKey key{tag, static_cast<std::uint16_t>(ar), static_cast<std::uint16_t>(ar)};
        tag_arity_index_[key].push_back(id);
        tag_arity_node_key_[id] = packed;
    }
    // Issue #1503: re-key a single node (remove old bucket, insert new).
    void patch_tag_arity_index_node(NodeId id) noexcept {
        if (id == NULL_NODE || id >= size() || tag_arity_index_.empty())
            return;
        tag_arity_index_remove_id(id);
        tag_arity_index_insert_id(id);
        tag_arity_index_incremental_patches_.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<std::size_t>(id) + 1 > tag_arity_index_built_size_)
            tag_arity_index_built_size_ = static_cast<std::size_t>(id) + 1;
    }
    void rebuild_tag_arity_index() noexcept {
        tag_arity_index_.clear();
        // Issue #554: time the rebuild so (query:pattern-
        // index-stats) can report the lifetime
        // microseconds spent. The AI Agent reads this to
        // compute avg_rebuild_us = time_us / rebuilds and
        // detect latency spikes.
        auto t0 = std::chrono::steady_clock::now();
        const std::size_t n = size();
        tag_arity_node_key_.assign(n, kTagArityNodeKeyNone);
        for (NodeId id = 0; id < n; ++id) {
            if (is_free_slot(id))
                continue;
            const auto tag = static_cast<std::uint32_t>(tag_[id]);
            const std::size_t ar = children_[id].size();
            const TagArityKey key{tag, static_cast<std::uint16_t>(ar),
                                  static_cast<std::uint16_t>(ar)};
            // Issue #1371: O(1) bucket insert via hash map.
            tag_arity_index_[key].push_back(id);
            tag_arity_node_key_[id] = pack_tag_arity_node_key(tag, ar);
        }
        tag_arity_index_built_size_ = n;
        tag_arity_index_rebuilds_.fetch_add(1, std::memory_order_relaxed);
        // Issue #554: record elapsed microseconds for the
        // (query:pattern-index-stats) primitive's AI Agent
        // observability. Stats-only (relaxed-ordering).
        auto t1 = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        tag_arity_index_rebuild_time_us_.fetch_add(static_cast<std::uint64_t>(us),
                                                   std::memory_order_relaxed);
        // Issue #547: clear the dirty flag — the index is
        // now in sync with the AST.
        tag_arity_index_dirty_.store(false, std::memory_order_release);
    }
    // Issue #1371: incremental append of [start_id, end_id)
    // into the hash map. Safe when those node ids are new
    // (append-only growth). Does NOT remove stale entries for
    // mutated in-place nodes — callers that change arity of
    // existing ids must fall back to rebuild_tag_arity_index()
    // or patch_tag_arity_index_dirty_nodes() (#1503).
    void rebuild_tag_arity_index_delta(NodeId start_id, NodeId end_id) noexcept {
        auto t0 = std::chrono::steady_clock::now();
        const std::size_t n = size();
        if (end_id > n)
            end_id = static_cast<NodeId>(n);
        if (tag_arity_node_key_.size() < n)
            tag_arity_node_key_.resize(n, kTagArityNodeKeyNone);
        for (NodeId id = start_id; id < end_id; ++id)
            tag_arity_index_insert_id(id);
        if (static_cast<std::size_t>(end_id) > tag_arity_index_built_size_)
            tag_arity_index_built_size_ = static_cast<std::size_t>(end_id);
        tag_arity_index_delta_hits_.fetch_add(1, std::memory_order_relaxed);
        auto t1 = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        tag_arity_index_rebuild_time_us_.fetch_add(static_cast<std::uint64_t>(us),
                                                   std::memory_order_relaxed);
        tag_arity_index_dirty_.store(false, std::memory_order_release);
    }
    // Issue #1503: patch all currently dirty nodes (+ append
    // tail) without a full rebuild. O(N) dirty-bit scan + O(dirty)
    // hash updates. Prefer over full rebuild when dirty fraction
    // is below tag_arity_index_full_rebuild_threshold_pct_.
    void patch_tag_arity_index_dirty_nodes() noexcept {
        auto t0 = std::chrono::steady_clock::now();
        const std::size_t n = size();
        if (tag_arity_node_key_.size() < n)
            tag_arity_node_key_.resize(n, kTagArityNodeKeyNone);
        for (NodeId id = 0; id < n; ++id) {
            if (static_cast<std::size_t>(id) >= tag_arity_index_built_size_ || is_dirty(id))
                patch_tag_arity_index_node(id);
        }
        tag_arity_index_built_size_ = n;
        tag_arity_index_delta_hits_.fetch_add(1, std::memory_order_relaxed);
        auto t1 = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        tag_arity_index_rebuild_time_us_.fetch_add(static_cast<std::uint64_t>(us),
                                                   std::memory_order_relaxed);
        tag_arity_index_dirty_.store(false, std::memory_order_release);
    }
    // Issue #1371 / #1503: refresh index if dirty.
    // Policy:
    //   1. empty → full rebuild
    //   2. append-only growth with no dirty below built_size → delta append
    //   3. dirty fraction > threshold_pct → full rebuild (threshold counter)
    //   4. else → incremental dirty-node patch
    void ensure_tag_arity_index() noexcept {
        if (!tag_arity_index_dirty_.load(std::memory_order_acquire)) {
            // First build: empty map with clean dirty flag.
            if (tag_arity_index_.empty() && size() > 0)
                rebuild_tag_arity_index();
            return;
        }
        const std::size_t n = size();
        if (tag_arity_index_.empty()) {
            rebuild_tag_arity_index();
            return;
        }
        // Count dirty among already-built ids (cheap byte scan).
        std::size_t dirty_n = 0;
        const std::size_t scan_n = std::min(n, tag_arity_index_built_size_);
        for (NodeId id = 0; id < static_cast<NodeId>(scan_n); ++id) {
            if (is_dirty(id))
                ++dirty_n;
        }
        // Index dirty with no dirty bits and no growth: unknown
        // in-place change (mark_tag_arity_index_dirty alone) → full rebuild.
        if (dirty_n == 0 && n <= tag_arity_index_built_size_) {
            rebuild_tag_arity_index();
            return;
        }
        // Append-only growth with no in-place dirties → O(new) delta.
        if (dirty_n == 0 && n > tag_arity_index_built_size_) {
            rebuild_tag_arity_index_delta(static_cast<NodeId>(tag_arity_index_built_size_),
                                          static_cast<NodeId>(n));
            return;
        }
        const std::uint8_t pct = tag_arity_index_full_rebuild_threshold_pct_;
        const bool prefer_full =
            scan_n > 0 && dirty_n * 100 > scan_n * static_cast<std::size_t>(pct);
        if (prefer_full) {
            tag_arity_index_threshold_full_rebuilds_.fetch_add(1, std::memory_order_relaxed);
            rebuild_tag_arity_index();
            return;
        }
        // Issue #1503: sparse dirty set → per-node re-key patch.
        patch_tag_arity_index_dirty_nodes();
    }
    // Issue #1371 / #1503: structural dirty + index policy. Marks the
    // index dirty (like mark_dirty_upward) and, when the index is warm,
    // immediately patches the seed node (append or in-place re-key).
    void mark_dirty_upward_with_index_update(NodeId id) {
        mark_dirty_upward(id);
        if (id == NULL_NODE || id >= size())
            return;
        if (tag_arity_index_.empty())
            return; // not built yet — ensure will full-rebuild
        // Issue #1503: live patch for append and in-place arity/tag.
        patch_tag_arity_index_node(id);
        if (tag_arity_index_built_size_ >= size()) {
            // All nodes indexed; clear dirty so ensure is a no-op
            // when only this path ran. Concurrent dirties may re-set.
            // Leave dirty if other mark_dirty_upward calls flipped it.
            // Conservative: keep dirty true so ensure still validates.
        }
    }
    void set_tag_arity_index_full_rebuild_threshold_pct(std::uint8_t pct) noexcept {
        tag_arity_index_full_rebuild_threshold_pct_ = pct == 0 ? 1 : (pct > 100 ? 100 : pct);
    }
    [[nodiscard]] std::uint8_t tag_arity_index_full_rebuild_threshold_pct() const noexcept {
        return tag_arity_index_full_rebuild_threshold_pct_;
    }
    [[nodiscard]] std::uint64_t tag_arity_index_threshold_full_rebuilds() const noexcept {
        return tag_arity_index_threshold_full_rebuilds_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t tag_arity_index_incremental_patches() const noexcept {
        return tag_arity_index_incremental_patches_.load(std::memory_order_relaxed);
    }

    // Issue #447 / #1371: find all NodeIds matching (tag,
    // arity_min, arity_max). Returns a copy of the
    // bucket. O(1) hash lookup. Bumps hit/miss counter.
    // Callers should call ensure_tag_arity_index() /
    // rebuild_tag_arity_index() first when the index may
    // be dirty (query:pattern does this).
    [[nodiscard]] std::pmr::vector<NodeId>
    find_by_tag_arity(std::uint32_t tag, std::uint16_t arity_min, std::uint16_t arity_max) const {
        const TagArityKey key{tag, arity_min, arity_max};
        auto it = tag_arity_index_.find(key);
        if (it != tag_arity_index_.end()) {
            tag_arity_index_hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
        tag_arity_index_misses_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    // Issue #447: query-stats accessors.
    [[nodiscard]] std::uint64_t tag_arity_index_hits() const noexcept {
        return tag_arity_index_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t tag_arity_index_misses() const noexcept {
        return tag_arity_index_misses_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t tag_arity_index_rebuilds() const noexcept {
        return tag_arity_index_rebuilds_.load(std::memory_order_relaxed);
    }
    // Issue #554: rebuild timing + delta update counters.
    [[nodiscard]] std::uint64_t tag_arity_index_rebuild_time_us() const noexcept {
        return tag_arity_index_rebuild_time_us_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t tag_arity_index_delta_hits() const noexcept {
        return tag_arity_index_delta_hits_.load(std::memory_order_relaxed);
    }
    // Issue #554: bump helper for delta hits — call from
    // the (follow-up) incremental delta-rebuild path.
    void bump_tag_arity_index_delta_hits() const noexcept {
        tag_arity_index_delta_hits_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t tag_arity_index_size() const noexcept {
        return tag_arity_index_.size();
    }
    // Issue #547: dirty-flag hook. mark_tag_arity_index_dirty()
    // is called from mutate:* paths (and the follow-up will
    // wire mark_dirty_upward). tag_arity_index_dirty() lets
    // callers short-circuit queries when the index is stale
    // (rebuild before serving the query). Stats counter
    // tag_arity_index_dirty_marks() exposes the lifetime
    // # of dirty marks for (query:pattern-index-stats).
    void mark_tag_arity_index_dirty() const noexcept {
        tag_arity_index_dirty_.store(true, std::memory_order_release);
        tag_arity_index_dirty_marks_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool tag_arity_index_dirty() const noexcept {
        return tag_arity_index_dirty_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t tag_arity_index_dirty_marks() const noexcept {
        return tag_arity_index_dirty_marks_.load(std::memory_order_relaxed);
    }
    // Issue #437: apply verify-dirty bits to a node.
    void apply_verify_dirty_bits(NodeId id, std::uint8_t verify_reasons) {
        if (verify_reasons == 0)
            return;
        if (id >= verify_dirty_.size())
            verify_dirty_.resize(id + 1, 0);
        // Detect newly-set bits (for per-reason counters).
        const auto newly_set = verify_reasons & ~verify_dirty_[id];
        verify_dirty_[id] |= verify_reasons;
        if (newly_set & kAssertionDirty)
            verify_assertion_dirty_total_.fetch_add(1, std::memory_order_relaxed);
        if (newly_set & kCoverageDirty)
            verify_coverage_dirty_total_.fetch_add(1, std::memory_order_relaxed);
        if (newly_set & kSvaDirty)
            verify_sva_dirty_total_.fetch_add(1, std::memory_order_relaxed);
        if (newly_set & kFormalCounterexampleDirty)
            verify_formal_cex_dirty_total_.fetch_add(1, std::memory_order_relaxed);
        mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
    }
    // Issue #469: verification_dirty_ accessor (public, used
    // by the (query:verification-loop-stats) primitive).
    [[nodiscard]] std::uint8_t verification_dirty(NodeId id) const noexcept {
        if (id >= verification_dirty_.size())
            return 0;
        return verification_dirty_[id];
    }
    // Issue #469: apply verification-dirty bits to a node.
    // Mirrors apply_verify_dirty_bits (from #437) but for
    // the verification_dirty_ column (separate column to
    // avoid collision with the VerifyDirtyReason bits).
    void apply_verification_dirty_bits(NodeId id, std::uint8_t reasons) {
        if (reasons == 0)
            return;
        if (id >= verification_dirty_.size())
            verification_dirty_.resize(id + 1, 0);
        // Detect newly-set bits (for per-reason counters).
        const auto newly_set = reasons & ~verification_dirty_[id];
        verification_dirty_[id] |= reasons;
        if (newly_set & kCoverageFeedbackDirty)
            verification_coverage_feedback_total_.fetch_add(1, std::memory_order_relaxed);
        if (newly_set & kAssertFailureDirty)
            verification_assert_failure_total_.fetch_add(1, std::memory_order_relaxed);
        mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
    }

    // Issue #313: mark_dirty_verification helper. Convenience
    // wrapper around apply_verification_dirty_bits() that
    // applies all standard verification reasons at once
    // (coverage-feedback + assert-failure). Mirrors
    // mark_ppa_dirty() (PPA-style API).
    //
    // Note on the bit value: the issue body suggests 0x20 for
    // kVerificationDirty in the main dirty_ bitmask, but all 8
    // bits of that byte are already used by kGeneralDirty (0x01),
    // kConstraintDirty (0x02), kOccurrenceDirty (0x04),
    // kOwnershipDirty (0x08), kCoercionDirty (0x10),
    // kStructDirty (0x20), kDefUseDirty (0x40), and
    // kPpaHintDirty (0x80) — see the DirtyReason enum. The
    // honest close is therefore to keep the orthogonal
    // verification_dirty_ side-table (set by #469) as the
    // canonical storage for per-reason verification state, and
    // use the constant kVerificationDirty as a *named
    // identifier* (not an actual bit value) so that callers
    // can document the verification concern without conflicting
    // with existing dirty-bit assignments.
    //
    // Follow-up issue: a bitmask-widening refactor (uint8_t →
    // uint16_t or wider) could consolidate the per-reason state
    // into a single column. That's a separate, well-scoped
    // change touching the storage vector, is_dirty / is_dirty_for
    // accessors, and all mark_* helpers.
    static constexpr std::uint8_t kVerificationDirty = 0x20;

    // Mark a node as verification-dirty: set all verification
    // reasons on the orthogonal verification_dirty_ side-table
    // + mirror kGeneralDirty on the main dirty_ byte (via
    // apply_verification_dirty_bits).
    void mark_dirty_verification(NodeId id) {
        apply_verification_dirty_bits(
            id, static_cast<std::uint8_t>(kCoverageFeedbackDirty | kAssertFailureDirty));
    }

    // Issue #320: per-node epoch accessors + helper.
    //
    // The last_seen_epoch_ column records the
    // mutation_epoch_ at which this node was last
    // touched. The TypeChecker's synthesize_flat cache
    // will use this to do per-node invalidation
    // (rather than the coarse whole-cache gate that
    // #168 ships). For now (this PR is the
    // foundation), the column is plumbed but the
    // synthesize_flat cache still uses the coarse gate.
    // The follow-up issue wires the per-node check.
    //
    // The accessor returns 0 for out-of-range (which
    // compares != any real mutation_epoch_ >= 1, so
    // a never-touched node will look "stale" once
    // after the first real epoch and then settle).

    // Read-only accessor (public, used by the type
    // checker + tests).
    [[nodiscard]] std::uint64_t last_seen_epoch(NodeId id) const noexcept {
        if (id >= last_seen_epoch_.size())
            return 0;
        return last_seen_epoch_[id];
    }

    // Stamp the node with the current mutation epoch.
    // Called from mark_dirty() so any future read sees
    // the updated epoch. The size of the column grows
    // lazily (matches the pattern of other side-tables
    // like verify_dirty_).
    void stamp_last_seen_epoch(NodeId id, std::uint64_t epoch) noexcept {
        if (id >= last_seen_epoch_.size())
            last_seen_epoch_.resize(id + 1, 0);
        last_seen_epoch_[id] = epoch;
    }

    // Convenience: stamp a node as having-seen the
    // current global epoch. The caller is expected to
    // pass the epoch from the global mutation counter
    // (e.g. CompilerService::mutation_epoch_). This
    // helper exists so the call site doesn't have to
    // know about the column details.
    void stamp_last_seen_epoch_to_current(NodeId id, std::uint64_t current_epoch) noexcept {
        stamp_last_seen_epoch(id, current_epoch);
    }

    // Issue #339: per-node occurrence-staleness
    // accessors + helpers. The occ_stale_ column
    // records whether the node's occurrence-narrowing
    // information is fresh (0) or stale (1) after a
    // mutation. Stale nodes must re-run
    // analyze_predicate_flat before their narrowing
    // is trusted.
    //
    // The narrowing is "stale" when the type checker
    // (post-mutation) can't confirm the predicate
    // still holds — either because the predicate
    // itself mutated or because the bound variable's
    // type changed in a way that's no longer a
    // sub-type of the refined type.
    //
    // Public read accessor: returns 0/1 (or 0 for
    // out-of-range, consistent with the other
    // per-node dirty accessors).
    [[nodiscard]] std::uint8_t is_occurrence_stale(NodeId id) const noexcept {
        if (id >= occ_stale_.size())
            return 0;
        return occ_stale_[id];
    }
    // Mark a node as stale (after mutation or after
    // validate_occurrence_narrowing() decides the
    // refined type is no longer compatible). The
    // caller can pass the mutation_id for
    // provenance tracking.
    void mark_occurrence_stale(NodeId id) noexcept {
        if (id >= occ_stale_.size())
            occ_stale_.resize(id + 1, 0);
        occ_stale_[id] = 1;
    }
    // Clear the staleness bit (after a successful
    // re-analysis via analyze_predicate_flat).
    void clear_occurrence_stale(NodeId id) noexcept {
        if (id < occ_stale_.size())
            occ_stale_[id] = 0;
    }
    // Count of how many nodes are currently stale
    // (for observability).
    [[nodiscard]] std::size_t occurrence_stale_count() const noexcept {
        std::size_t n = 0;
        for (auto v : occ_stale_)
            if (v)
                ++n;
        return n;
    }

    // Walk the parent_ chain upward (BFS, the same shape as
    // mark_dirty_upward at line ~3726) and apply
    // verification-dirty to each ancestor. The BFS logic is
    // duplicated locally rather than funneling through
    // mark_dirty_upward to avoid triggering the
    // type_cache_generation_, mark_tag_arity_index_dirty, +
    // per-binding-gen bump path — those cost meaningful work
    // for nodes whose verification state changed but whose
    // type is unaffected. The verification_dirty_ column is
    // the canonical source of "verification needs re-eval"
    // and `is_verification_dirty(id)` reads it directly. If
    // the broader mark_dirty_upward side effects are wanted
    // alongside verification, callers can chain the two.
    void mark_dirty_verification_upward(NodeId id) {
        // Match the mark_dirty_upward observability signal so
        // monitoring tooling sees a single upward-walk per
        // verification event.
        mark_dirty_upward_call_count_.fetch_add(1, std::memory_order_relaxed);
        std::deque<NodeId> queue;
        queue.push_back(id);
        while (!queue.empty()) {
            auto nid = queue.front();
            queue.pop_front();
            apply_verification_dirty_bits(
                nid, static_cast<std::uint8_t>(kCoverageFeedbackDirty | kAssertFailureDirty));
            auto p = parent_[nid];
            if (p != NULL_NODE)
                queue.push_back(p);
        }
    }

    // Issue #313: read-only accessor for the verification-
    // dirty side-table. Returns the OR'd set of verification
    // reasons set on this node (kCoverageFeedbackDirty |
    // kAssertFailureDirty | ...). 0 means "clean from a
    // verification perspective".
    bool is_verification_dirty(NodeId id) const {
        return id < verification_dirty_.size() && verification_dirty_[id] != 0;
    }
    bool is_verification_dirty_for(NodeId id, std::uint8_t verify_mask) const {
        return id < verification_dirty_.size() && (verification_dirty_[id] & verify_mask) != 0;
    }
    void clear_verification_dirty(NodeId id) {
        if (id < verification_dirty_.size())
            verification_dirty_[id] = 0;
    }
    void clear_verification_dirty_for(NodeId id, std::uint8_t verify_mask) {
        if (id < verification_dirty_.size())
            verification_dirty_[id] &= static_cast<std::uint8_t>(~verify_mask);
    }


    // Issue #290: macro_dirty_ bit definitions. OR'd into the
    // macro_dirty_ column when clone_macro_body or a
    // self-evolution step touches a node. Setting any macro bit
    // also ORs kGeneralDirty on dirty_ for backward-compat with
    // legacy is_dirty() callers (type checker, lowering).
    enum MacroDirtyReason : std::uint8_t {
        kMacroExpansion = 0x01,  // clone_macro_body produced this subtree
        kMacroSelfModify = 0x02, // self-evolution step touched a macro-introduced node
    };
    // Issue #290: macro_dirty_ accessor (public, used by the
    // (compile:macro-dirty?) primitive).
    [[nodiscard]] std::uint8_t macro_dirty(NodeId id) const noexcept {
        if (id >= macro_dirty_.size())
            return 0;
        return macro_dirty_[id];
    }
    // Issue #290: apply macro-dirty bits to a node. Called from
    // clone_macro_body after a hygienic macro expansion
    // (kMacroExpansion) and from self-evolution loops that touch
    // macro-introduced nodes (kMacroSelfModify). Mirrors the
    // verification-column pattern (#469) but lives in its own
    // column to avoid collision with the VerifyDirtyReason bits
    // (4 bits used).
    void apply_macro_dirty_bits(NodeId id, std::uint8_t reasons) {
        if (reasons == 0)
            return;
        if (id >= macro_dirty_.size())
            macro_dirty_.resize(id + 1, 0);
        const auto newly_set = reasons & ~macro_dirty_[id];
        macro_dirty_[id] |= reasons;
        if (newly_set & kMacroExpansion)
            macro_expansion_dirty_total_.fetch_add(1, std::memory_order_relaxed);
        if (newly_set & kMacroSelfModify)
            macro_self_modify_dirty_total_.fetch_add(1, std::memory_order_relaxed);
        mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
    }
    // Issue #290: bulk-clear all macro_dirty_ bits across the
    // flat. Called from the (compile:clear-macro-dirty!) primitive
    // and from full-reset paths. Walks every live node.
    void clear_macro_dirty_all() noexcept {
        for (auto& b : macro_dirty_)
            b = 0;
    }
    // Issue #290: count nodes with any macro_dirty_ bit set.
    // Used by the (compile:macro-dirty-count) primitive.
    [[nodiscard]] std::size_t macro_dirty_count() const noexcept {
        std::size_t n = 0;
        for (auto b : macro_dirty_)
            if (b != 0)
                ++n;
        return n;
    }

public:
    // Low-level raw node creation (for advanced mutation).
    // Creates a minimal node with the given tag and default fields.
    // Children must be set up manually via set_child/insert_child.
    [[nodiscard]] NodeId add_raw_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
        return add_node(tag, m);
    }

    std::vector<MatchClauseInfo> match_info_;
    // Issue #150 Phase 1: region annotation side-tables.
    // region_by_sym_ is keyed by the define's SymId (the
    // function's bound name). region_by_lambda_id_ is keyed by
    // the lambda's NodeId (for anonymous lambdas that don't
    // have a bound name). Both are populated by the parser
    // (evaluator_eval_flat.cpp) when it sees
    // (performance-region ...) / (evolution-region ...) wrappers.
    // Lowering reads them via get_function_region_for_sym /
    // get_function_region_for_lambda to set
    // IRFunction::region accordingly.
    std::pmr::unordered_map<SymId, std::uint8_t> region_by_sym_;
    std::pmr::unordered_map<NodeId, std::uint8_t> region_by_lambda_id_;
    // Issue #1520: dense SoA side-tables for region lookups (index =
    // SymId/NodeId, 0 = unset, else region+1). Cap keeps memory bounded.
    static constexpr std::size_t kRegionDenseCap = 65536;
    std::pmr::vector<std::uint8_t> region_by_sym_dense_;
    std::pmr::vector<std::uint8_t> region_by_lambda_dense_;
    // Issue #255: explicit custom move constructor. The
    // 4 std::atomic observability members (added for #255)
    // make the implicit move ctor deleted by some compiler
    // versions — the default move tries to copy-construct
    // each atomic, which is deleted. We implement a custom
    // move ctor that does a memberwise move: std::pmr vectors
    // + unordered_maps move naturally; std::atomic values are
    // std::move'd (which on libstdc++ does an rcu-style value
    // transfer). The user-declared destructor would otherwise
    // inhibit implicit move generation, so this is mandatory.
    FlatAST(FlatAST&& other) noexcept
        : tag_(std::move(other.tag_))
        , int_val_(std::move(other.int_val_))
        , float_val_(std::move(other.float_val_))
        , sym_id_(std::move(other.sym_id_))
        , children_(std::move(other.children_))
        , parent_(std::move(other.parent_))
        , incoming_parent_edges_(std::move(other.incoming_parent_edges_))
        , incoming_parent_index_dirty_(other.incoming_parent_index_dirty_)
        , incoming_parent_index_rebuilds_(other.incoming_parent_index_rebuilds_.load())
        , incoming_parent_index_lookups_(other.incoming_parent_index_lookups_.load())
        , incoming_parent_index_hits_(other.incoming_parent_index_hits_.load())
        , param_begin_(std::move(other.param_begin_))
        , param_count_(std::move(other.param_count_))
        , cap_require_count_(std::move(other.cap_require_count_))
        , param_data_(std::move(other.param_data_))
        , param_annot_data_(std::move(other.param_annot_data_))
        , line_(std::move(other.line_))
        , col_(std::move(other.col_))
        // Issue #1893: hygiene / dirty metadata SoA columns (must track
        // declaration order: marker_ → provenance_ → dirty_ family).
        , marker_(std::move(other.marker_))
        , provenance_(std::move(other.provenance_))
        , dirty_(std::move(other.dirty_))
        , ppa_dirty_(std::move(other.ppa_dirty_))
        , verify_dirty_(std::move(other.verify_dirty_))
        , verification_dirty_(std::move(other.verification_dirty_))
        , macro_dirty_(std::move(other.macro_dirty_))
        , type_id_(std::move(other.type_id_))
        , type_cache_gen_(std::move(other.type_cache_gen_))
        , type_cache_binding_gen_(std::move(other.type_cache_binding_gen_))
        , binding_gens_(std::move(other.binding_gens_))
        , error_kind_(std::move(other.error_kind_))
        , value_cache_(std::move(other.value_cache_))
        , mutation_log_(std::move(other.mutation_log_))
        // Issue #300 follow-up #2: regression from e5f559bf
        // ("fix warning") which dropped node_first_mutation_
        // from the move-ctor init list. Default-construction
        // used the new_delete_resource, mixing with the
        // arena-allocated columns during ~FlatAST() and
        // producing a heap-use-after-free on the binding_gens_
        // shared_ptr control block (the default-ctor'd
        // column's heap buffer lived past the arena shrink in
        // (arena:defrag), then died during teardown in an
        // order that poisoned the next column's destructor).
        // Restoring the init-list entry matches declaration
        // order (node_first_mutation_ declared before
        // last_seen_epoch_) and keeps the same pmr allocator.
        , node_first_mutation_(std::move(other.node_first_mutation_))
        // Issue #320: per-node epoch tracking column
        // (SoA parallel to mutation_log_).
        , last_seen_epoch_(std::move(other.last_seen_epoch_))
        // Issue #339: per-node occurrence-staleness column.
        , occ_stale_(std::move(other.occ_stale_))
        , next_mutation_id_(other.next_mutation_id_)
        , generation_(other.generation_)
        , node_gen_(std::move(other.node_gen_))
        , free_list_(std::move(other.free_list_))
        , bump_generation_count_(other.bump_generation_count_.load())
        , is_valid_check_count_(other.is_valid_check_count_.load())
        , stable_ref_invalidations_(other.stable_ref_invalidations_.load())
        , generation_wrap_count_(other.generation_wrap_count_.load())
        , node_gen_stale_access_count_(other.node_gen_stale_access_count_.load())
        , atomic_batch_commits_(other.atomic_batch_commits_.load())
        , children_safe_view_count_(other.children_safe_view_count_.load())
        , children_column_soa_hits_(other.children_column_soa_hits_.load())
        , pcv_pin_count_(other.pcv_pin_count_.load())
        , region_dense_hits_(other.region_dense_hits_.load())
        , region_map_lookups_(other.region_map_lookups_.load())
        , children_call_count_(other.children_call_count_.load())
        , parent_of_call_count_(other.parent_of_call_count_.load())
        , mark_dirty_upward_call_count_(other.mark_dirty_upward_call_count_.load())
        , mark_dirty_total_nodes_(other.mark_dirty_total_nodes_.load())
        , mark_dirty_early_exit_count_(other.mark_dirty_early_exit_count_.load())
        , mark_dirty_max_depth_observed_(other.mark_dirty_max_depth_observed_.load())
        , mark_dirty_truncated_count_(other.mark_dirty_truncated_count_.load())
        , mark_dirty_boundary_prune_count_(other.mark_dirty_boundary_prune_count_.load())
        , auto_compact_on_commit_count_(other.auto_compact_on_commit_count_.load())
        , live_nodes_threshold_warn_count_(other.live_nodes_threshold_warn_count_.load())
        , compaction_free_list_threshold_(other.compaction_free_list_threshold_)
        , max_live_nodes_warn_(other.max_live_nodes_warn_)
        , rollback_compaction_triggered_(other.rollback_compaction_triggered_.load())
        , node_recycle_total_(other.node_recycle_total_.load())
        , node_slot_reuse_count_(other.node_slot_reuse_count_.load())
        , node_compact_total_(other.node_compact_total_.load())
        , soft_compact_count_(other.soft_compact_count_.load())
        , stale_ref_auto_refresh_count_(other.stale_ref_auto_refresh_count_.load())
        , verification_coverage_feedback_total_(other.verification_coverage_feedback_total_.load())
        , verification_assert_failure_total_(other.verification_assert_failure_total_.load())
        , sv_mutate_attempts_total_(other.sv_mutate_attempts_total_.load())
        , sv_mutate_success_total_(other.sv_mutate_success_total_.load())
        , verify_loop_cycles_total_(other.verify_loop_cycles_total_.load())
        , macro_expansion_dirty_total_(other.macro_expansion_dirty_total_.load())
        , macro_self_modify_dirty_total_(other.macro_self_modify_dirty_total_.load())
        , match_info_(std::move(other.match_info_))
        , region_by_sym_(std::move(other.region_by_sym_))
        , region_by_lambda_id_(std::move(other.region_by_lambda_id_))
        , region_by_sym_dense_(std::move(other.region_by_sym_dense_))
        , region_by_lambda_dense_(std::move(other.region_by_lambda_dense_))
        , root(other.root)
        , bump_generation_suppressed_(other.bump_generation_suppressed_)
        , atomic_batch_bumps_saved_(other.atomic_batch_bumps_saved_) {}
    FlatAST& operator=(FlatAST&& other) noexcept {
        if (this != &other) {
            tag_ = std::move(other.tag_);
            int_val_ = std::move(other.int_val_);
            float_val_ = std::move(other.float_val_);
            sym_id_ = std::move(other.sym_id_);
            children_ = std::move(other.children_);
            parent_ = std::move(other.parent_);
            incoming_parent_edges_ = std::move(other.incoming_parent_edges_);
            incoming_parent_index_dirty_ = other.incoming_parent_index_dirty_;
            incoming_parent_index_rebuilds_.store(other.incoming_parent_index_rebuilds_.load());
            incoming_parent_index_lookups_.store(other.incoming_parent_index_lookups_.load());
            incoming_parent_index_hits_.store(other.incoming_parent_index_hits_.load());
            param_begin_ = std::move(other.param_begin_);
            param_count_ = std::move(other.param_count_);
            cap_require_count_ = std::move(other.cap_require_count_);
            param_data_ = std::move(other.param_data_);
            param_annot_data_ = std::move(other.param_annot_data_);
            line_ = std::move(other.line_);
            col_ = std::move(other.col_);
            // Issue #1893: hygiene / dirty metadata SoA columns.
            marker_ = std::move(other.marker_);
            provenance_ = std::move(other.provenance_);
            dirty_ = std::move(other.dirty_);
            ppa_dirty_ = std::move(other.ppa_dirty_);
            verify_dirty_ = std::move(other.verify_dirty_);
            verification_dirty_ = std::move(other.verification_dirty_);
            macro_dirty_ = std::move(other.macro_dirty_);
            type_id_ = std::move(other.type_id_);
            type_cache_gen_ = std::move(other.type_cache_gen_);
            type_cache_binding_gen_ = std::move(other.type_cache_binding_gen_);
            binding_gens_ = std::move(other.binding_gens_);
            error_kind_ = std::move(other.error_kind_);
            node_gen_ = std::move(other.node_gen_);
            free_list_ = std::move(other.free_list_);
            value_cache_ = std::move(other.value_cache_);
            mutation_log_ = std::move(other.mutation_log_);
            node_first_mutation_ = std::move(other.node_first_mutation_);
            next_mutation_id_ = other.next_mutation_id_;
            generation_ = other.generation_;
            bump_generation_count_.store(other.bump_generation_count_.load());
            is_valid_check_count_.store(other.is_valid_check_count_.load());
            stable_ref_invalidations_.store(other.stable_ref_invalidations_.load());
            atomic_batch_commits_.store(other.atomic_batch_commits_.load());
            verification_coverage_feedback_total_.store(
                other.verification_coverage_feedback_total_.load());
            verification_assert_failure_total_.store(
                other.verification_assert_failure_total_.load());
            sv_mutate_attempts_total_.store(other.sv_mutate_attempts_total_.load());
            sv_mutate_success_total_.store(other.sv_mutate_success_total_.load());
            verify_loop_cycles_total_.store(other.verify_loop_cycles_total_.load());
            macro_expansion_dirty_total_.store(other.macro_expansion_dirty_total_.load());
            macro_self_modify_dirty_total_.store(other.macro_self_modify_dirty_total_.load());
            generation_wrap_count_.store(other.generation_wrap_count_.load());
            node_gen_stale_access_count_.store(other.node_gen_stale_access_count_.load());
            children_call_count_.store(other.children_call_count_.load());
            parent_of_call_count_.store(other.parent_of_call_count_.load());
            mark_dirty_upward_call_count_.store(other.mark_dirty_upward_call_count_.load());
            mark_dirty_total_nodes_.store(other.mark_dirty_total_nodes_.load());
            mark_dirty_early_exit_count_.store(other.mark_dirty_early_exit_count_.load());
            mark_dirty_max_depth_observed_.store(other.mark_dirty_max_depth_observed_.load());
            mark_dirty_truncated_count_.store(other.mark_dirty_truncated_count_.load());
            mark_dirty_boundary_prune_count_.store(other.mark_dirty_boundary_prune_count_.load());
            auto_compact_on_commit_count_.store(other.auto_compact_on_commit_count_.load());
            live_nodes_threshold_warn_count_.store(other.live_nodes_threshold_warn_count_.load());
            compaction_free_list_threshold_ = other.compaction_free_list_threshold_;
            max_live_nodes_warn_ = other.max_live_nodes_warn_;
            rollback_compaction_triggered_.store(other.rollback_compaction_triggered_.load());
            node_recycle_total_.store(other.node_recycle_total_.load());
            node_slot_reuse_count_.store(other.node_slot_reuse_count_.load());
            node_compact_total_.store(other.node_compact_total_.load());
            soft_compact_count_.store(other.soft_compact_count_.load());
            stale_ref_auto_refresh_count_.store(other.stale_ref_auto_refresh_count_.load());
            match_info_ = std::move(other.match_info_);
            region_by_sym_ = std::move(other.region_by_sym_);
            region_by_lambda_id_ = std::move(other.region_by_lambda_id_);
            region_by_sym_dense_ = std::move(other.region_by_sym_dense_);
            region_by_lambda_dense_ = std::move(other.region_by_lambda_dense_);
            children_column_soa_hits_.store(other.children_column_soa_hits_.load());
            pcv_pin_count_.store(other.pcv_pin_count_.load());
            region_dense_hits_.store(other.region_dense_hits_.load());
            region_map_lookups_.store(other.region_map_lookups_.load());
            root = other.root;
            bump_generation_suppressed_ = other.bump_generation_suppressed_;
            atomic_batch_bumps_saved_ = other.atomic_batch_bumps_saved_;
        }
        return *this;
    }
    // Issue #255: explicit copy constructor + copy assignment.
    // Declaring a move ctor/assignment implicitly deletes the
    // copy versions, but evaluator_env.cpp has 3 copy-assign
    // sites (workspace COW, local-flat initialization, etc.).
    // Copy is rare in hot paths but must compile.
    FlatAST(const FlatAST& other)
        : tag_(other.tag_)
        , int_val_(other.int_val_)
        , float_val_(other.float_val_)
        , sym_id_(other.sym_id_)
        , children_(other.children_)
        , parent_(other.parent_)
        , incoming_parent_edges_(other.incoming_parent_edges_)
        , incoming_parent_index_dirty_(other.incoming_parent_index_dirty_)
        , incoming_parent_index_rebuilds_(other.incoming_parent_index_rebuilds_.load())
        , incoming_parent_index_lookups_(other.incoming_parent_index_lookups_.load())
        , incoming_parent_index_hits_(other.incoming_parent_index_hits_.load())
        , param_begin_(other.param_begin_)
        , param_count_(other.param_count_)
        , cap_require_count_(other.cap_require_count_)
        , param_data_(other.param_data_)
        , param_annot_data_(other.param_annot_data_)
        , line_(other.line_)
        , col_(other.col_)
        , marker_(other.marker_)
        , provenance_(other.provenance_) // Issue #1893: was missing from copy path
        , dirty_(other.dirty_)
        , ppa_dirty_(other.ppa_dirty_)
        , verify_dirty_(other.verify_dirty_)
        , verification_dirty_(other.verification_dirty_)
        , macro_dirty_(other.macro_dirty_)
        , type_id_(other.type_id_)
        , type_cache_gen_(other.type_cache_gen_)
        , type_cache_binding_gen_(other.type_cache_binding_gen_)
        , schema_cache_(other.schema_cache_)
        , binding_gens_(other.binding_gens_)
        , error_kind_(other.error_kind_)
        , value_cache_(other.value_cache_)
        , mutation_log_(other.mutation_log_)
        , narrowing_log_(other.narrowing_log_)
        , node_first_mutation_(other.node_first_mutation_)
        // Issue #320: per-node epoch tracking column.
        , last_seen_epoch_(other.last_seen_epoch_)
        // Issue #339: per-node occurrence-staleness
        // column. (Declared after last_seen_epoch_ in
        // the class; init-list order must match the
        // declaration order to silence -Wreorder.)
        , occ_stale_(other.occ_stale_)
        , next_mutation_id_(other.next_mutation_id_)
        , generation_(other.generation_)
        , node_gen_(other.node_gen_)
        , free_list_(other.free_list_)
        , bump_generation_count_(other.bump_generation_count_.load())
        , is_valid_check_count_(other.is_valid_check_count_.load())
        , stable_ref_invalidations_(other.stable_ref_invalidations_.load())
        , generation_wrap_count_(other.generation_wrap_count_.load())
        , node_gen_stale_access_count_(other.node_gen_stale_access_count_.load())
        , atomic_batch_commits_(other.atomic_batch_commits_.load())
        // Declaration order: children_safe_view_count_ / #1520 metrics
        // precede children_call_count_ and verify_loop_cycles_total_.
        , children_safe_view_count_(other.children_safe_view_count_.load())
        , children_column_soa_hits_(other.children_column_soa_hits_.load())
        , pcv_pin_count_(other.pcv_pin_count_.load())
        , region_dense_hits_(other.region_dense_hits_.load())
        , region_map_lookups_(other.region_map_lookups_.load())
        , children_call_count_(other.children_call_count_.load())
        , parent_of_call_count_(other.parent_of_call_count_.load())
        , mark_dirty_upward_call_count_(other.mark_dirty_upward_call_count_.load())
        , mark_dirty_total_nodes_(other.mark_dirty_total_nodes_.load())
        , mark_dirty_truncated_count_(other.mark_dirty_truncated_count_.load())
        , mark_dirty_boundary_prune_count_(other.mark_dirty_boundary_prune_count_.load())
        , auto_compact_on_commit_count_(other.auto_compact_on_commit_count_.load())
        , live_nodes_threshold_warn_count_(other.live_nodes_threshold_warn_count_.load())
        , compaction_free_list_threshold_(other.compaction_free_list_threshold_)
        , max_live_nodes_warn_(other.max_live_nodes_warn_)
        , rollback_compaction_triggered_(other.rollback_compaction_triggered_.load())
        , node_recycle_total_(other.node_recycle_total_.load())
        , node_slot_reuse_count_(other.node_slot_reuse_count_.load())
        , node_compact_total_(other.node_compact_total_.load())
        , soft_compact_count_(other.soft_compact_count_.load())
        , stale_ref_auto_refresh_count_(other.stale_ref_auto_refresh_count_.load())
        , verification_coverage_feedback_total_(other.verification_coverage_feedback_total_.load())
        , verification_assert_failure_total_(other.verification_assert_failure_total_.load())
        , sv_mutate_attempts_total_(other.sv_mutate_attempts_total_.load())
        , sv_mutate_success_total_(other.sv_mutate_success_total_.load())
        , verify_loop_cycles_total_(other.verify_loop_cycles_total_.load())
        , macro_expansion_dirty_total_(other.macro_expansion_dirty_total_.load())
        , macro_self_modify_dirty_total_(other.macro_self_modify_dirty_total_.load())
        , match_info_(other.match_info_)
        , region_by_sym_(other.region_by_sym_)
        , region_by_lambda_id_(other.region_by_lambda_id_)
        , region_by_sym_dense_(other.region_by_sym_dense_)
        , region_by_lambda_dense_(other.region_by_lambda_dense_)
        , root(other.root)
        , bump_generation_suppressed_(other.bump_generation_suppressed_)
        , atomic_batch_bumps_saved_(other.atomic_batch_bumps_saved_) {}
    FlatAST& operator=(const FlatAST& other) {
        if (this != &other) {
            tag_ = other.tag_;
            int_val_ = other.int_val_;
            float_val_ = other.float_val_;
            sym_id_ = other.sym_id_;
            children_ = other.children_;
            parent_ = other.parent_;
            incoming_parent_edges_ = other.incoming_parent_edges_;
            incoming_parent_index_dirty_ = other.incoming_parent_index_dirty_;
            incoming_parent_index_rebuilds_.store(other.incoming_parent_index_rebuilds_.load());
            incoming_parent_index_lookups_.store(other.incoming_parent_index_lookups_.load());
            incoming_parent_index_hits_.store(other.incoming_parent_index_hits_.load());
            param_begin_ = other.param_begin_;
            param_count_ = other.param_count_;
            cap_require_count_ = other.cap_require_count_;
            param_data_ = other.param_data_;
            param_annot_data_ = other.param_annot_data_;
            line_ = other.line_;
            col_ = other.col_;
            // Issue #1893: hygiene / dirty metadata SoA — required for
            // capture_workspace_snapshot / atomic-batch :snapshot? restore
            // to preserve MacroIntroduced + provenance after rollback.
            marker_ = other.marker_;
            provenance_ = other.provenance_;
            dirty_ = other.dirty_;
            ppa_dirty_ = other.ppa_dirty_;
            verify_dirty_ = other.verify_dirty_;
            verification_dirty_ = other.verification_dirty_;
            macro_dirty_ = other.macro_dirty_;
            type_id_ = other.type_id_;
            type_cache_gen_ = other.type_cache_gen_;
            type_cache_binding_gen_ = other.type_cache_binding_gen_;
            binding_gens_ = other.binding_gens_;
            schema_cache_ = other.schema_cache_;
            error_kind_ = other.error_kind_;
            node_gen_ = other.node_gen_;
            free_list_ = other.free_list_;
            value_cache_ = other.value_cache_;
            mutation_log_ = other.mutation_log_;
            node_first_mutation_ = other.node_first_mutation_;
            next_mutation_id_ = other.next_mutation_id_;
            generation_ = other.generation_;
            bump_generation_count_.store(other.bump_generation_count_.load());
            is_valid_check_count_.store(other.is_valid_check_count_.load());
            stable_ref_invalidations_.store(other.stable_ref_invalidations_.load());
            atomic_batch_commits_.store(other.atomic_batch_commits_.load());
            verification_coverage_feedback_total_.store(
                other.verification_coverage_feedback_total_.load());
            verification_assert_failure_total_.store(
                other.verification_assert_failure_total_.load());
            sv_mutate_attempts_total_.store(other.sv_mutate_attempts_total_.load());
            sv_mutate_success_total_.store(other.sv_mutate_success_total_.load());
            verify_loop_cycles_total_.store(other.verify_loop_cycles_total_.load());
            macro_expansion_dirty_total_.store(other.macro_expansion_dirty_total_.load());
            macro_self_modify_dirty_total_.store(other.macro_self_modify_dirty_total_.load());
            children_call_count_.store(other.children_call_count_.load());
            parent_of_call_count_.store(other.parent_of_call_count_.load());
            mark_dirty_upward_call_count_.store(other.mark_dirty_upward_call_count_.load());
            mark_dirty_total_nodes_.store(other.mark_dirty_total_nodes_.load());
            mark_dirty_truncated_count_.store(other.mark_dirty_truncated_count_.load());
            mark_dirty_boundary_prune_count_.store(other.mark_dirty_boundary_prune_count_.load());
            auto_compact_on_commit_count_.store(other.auto_compact_on_commit_count_.load());
            live_nodes_threshold_warn_count_.store(other.live_nodes_threshold_warn_count_.load());
            compaction_free_list_threshold_ = other.compaction_free_list_threshold_;
            max_live_nodes_warn_ = other.max_live_nodes_warn_;
            rollback_compaction_triggered_.store(other.rollback_compaction_triggered_.load());
            node_recycle_total_.store(other.node_recycle_total_.load());
            node_slot_reuse_count_.store(other.node_slot_reuse_count_.load());
            node_compact_total_.store(other.node_compact_total_.load());
            soft_compact_count_.store(other.soft_compact_count_.load());
            stale_ref_auto_refresh_count_.store(other.stale_ref_auto_refresh_count_.load());
            match_info_ = other.match_info_;
            region_by_sym_ = other.region_by_sym_;
            region_by_lambda_id_ = other.region_by_lambda_id_;
            region_by_sym_dense_ = other.region_by_sym_dense_;
            region_by_lambda_dense_ = other.region_by_lambda_dense_;
            children_column_soa_hits_.store(other.children_column_soa_hits_.load());
            pcv_pin_count_.store(other.pcv_pin_count_.load());
            region_dense_hits_.store(other.region_dense_hits_.load());
            region_map_lookups_.store(other.region_map_lookups_.load());
            root = other.root;
            bump_generation_suppressed_ = other.bump_generation_suppressed_;
            atomic_batch_bumps_saved_ = other.atomic_batch_bumps_saved_;
        }
        return *this;
    }
    explicit FlatAST(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : tag_(alloc)
        , int_val_(alloc)
        , float_val_(alloc)
        , sym_id_(alloc)
        , children_()
        , parent_(alloc)
        , incoming_parent_edges_(alloc)
        , param_begin_(alloc)
        , param_count_(alloc)
        , cap_require_count_(alloc)
        , param_data_(alloc)
        , param_annot_data_(alloc)
        , line_(alloc)
        , col_(alloc)
        // Issue #300: these pmr columns must use the arena alloc passed
        // to FlatAST (not the default new_delete_resource).
        , marker_(alloc)
        , dirty_(alloc)
        , ppa_dirty_(alloc)
        , verify_dirty_(alloc)
        , verification_dirty_(alloc)
        , macro_dirty_(alloc)
        , type_id_(alloc)
        , type_cache_gen_(alloc)
        , type_cache_binding_gen_(alloc)
        , schema_cache_(alloc)
        , error_kind_(alloc)
        // Issue #1371: tag_arity hash map (declared before value_cache_).
        , tag_arity_index_(alloc)
        , value_cache_(alloc)
        , mutation_log_(alloc)
        , narrowing_log_(alloc)
        // Issue #300 follow-up #2: regression from e5f559bf
        // ("fix warning"). node_first_mutation_ must use the
        // arena allocator (alloc), not the default
        // new_delete_resource. Same root cause as the move
        // ctor fix above — removing it from the init list
        // caused default-construction with the wrong
        // allocator and produced a heap-use-after-free on
        // the binding_gens_ shared_ptr control block during
        // ~FlatAST() after (arena:defrag) shrank the arena.
        // Restoring the init-list entry matches declaration
        // order (node_first_mutation_ declared before
        // last_seen_epoch_).
        , node_first_mutation_(alloc)
        // Issue #320: per-node epoch tracking column.
        , last_seen_epoch_(alloc)
        // Issue #339: per-node occurrence-staleness column.
        , occ_stale_(alloc)
        , node_gen_(alloc)
        , free_list_(alloc) {}

    // Issue #300 follow-up #1: release the children_ column before
    // the rest of ~FlatAST runs. Aliased PCV slots can share one
    // heap control block with a corrupted use_count (ASAN UAF after
    // arena:defrag). Swap the column out first so the member dtor is
    // a no-op, then dedupe-abandon aliases on the detached vector.
    void release_children_for_teardown() {
        std::vector<PersistentChildVector<NodeId>> doomed;
        doomed.swap(children_);
        std::unordered_set<const void*> seen;
        for (auto& pcv : doomed) {
            const void* id = pcv.storage_identity();
            if (id == nullptr)
                continue;
            if (!seen.insert(id).second)
                pcv.abandon_storage();
        }
    }

    ~FlatAST() { release_children_for_teardown(); }

    // ── Builders ───────────────────────────────────────────────

    // Set parent for all children of the given node
    void link_children(NodeId id) {
        // Issue #220/221: walk the per-node PCV. The PCV's
        // iterators are const (the vector is immutable from
        // the outside), so this is read-only iteration.
        for (auto cid : children_[id]) {
            if (cid != NULL_NODE)
                parent_[cid] = id;
        }
        // Issue #1689: bulk parent rewrite — reindex on next lookup.
        mark_incoming_parent_index_dirty();
    }

    [[nodiscard]] NodeId add_literal_float(double val) {
        auto id = add_node(NodeTag::LiteralFloat);
        float_val_[id] = val;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_literalstring(SymId name) {
        auto id = add_node(NodeTag::LiteralString);
        sym_id_[id] = name;
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_literal(std::int64_t val) {
        auto id = add_node(NodeTag::LiteralInt);
        int_val_[id] = val;
        // Issue #402: literal int doesn't trigger any
        // fallback-relevant flag (LiteralInt is IR-native).
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_variable(SymId name) {
        auto id = add_node(NodeTag::Variable);
        sym_id_[id] = name;
        // Issue #402: lazy sym_id bit deferred. Keyword
        // Variables (':foo') need tree-walker fallback, but
        // resolving the sym_id requires the pool, which is
        // out of scope here. The fast-path's subtree walk
        // covers the keyword check (it walks the root
        // subtree and applies the existing per-NodeTag /
        // per-sym_id logic without paying the full-flat
        // iteration cost).
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_call(NodeId func, std::span<const NodeId> args) {
        auto id = add_node(NodeTag::Call);
        children_[id] =
            PersistentChildVector<NodeId>(1 + args.size(), [&](std::size_t i) -> NodeId {
                if (i == 0)
                    return func;
                return args[i - 1];
            });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_if(NodeId cond, NodeId then_b, NodeId else_b) {
        auto id = add_node(NodeTag::IfExpr);
        children_[id] = PersistentChildVector<NodeId>(3, [&](std::size_t i) -> NodeId {
            return (i == 0 ? cond : (i == 1 ? then_b : else_b));
        });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_lambda(std::span<const SymId> params, NodeId body,
                                    bool dotted = false) {
        return add_lambda(params, {}, body, dotted);
    }
    [[nodiscard]] NodeId add_lambda(std::span<const SymId> params, std::span<const NodeId> annots,
                                    NodeId body, bool dotted = false) {
        auto id = add_node(NodeTag::Lambda);
        int_val_[id] = dotted ? 1 : 0; // store dotted flag
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        // Store annotations (or NULL_NODE if not provided)
        param_annot_data_.resize(param_annot_data_.size() + params.size(), aura::ast::NULL_NODE);
        for (std::size_t i = 0; i < params.size() && i < annots.size(); ++i)
            param_annot_data_[pstart + i] = annots[i];
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return body; });
        link_children(id);
        return id;
    }

    // Issue #1266: set params on an existing Lambda (used by
    // mutate:inline-call clone path — add_lambda with empty params
    // then copy params after body wire-up).
    void set_lambda_params(NodeId id, std::span<const SymId> params,
                           std::span<const NodeId> annots = {}) {
        if (id >= param_begin_.size() || id >= param_count_.size())
            return;
        if (id >= tag_.size() || tag_[id] != NodeTag::Lambda)
            return;
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_annot_data_.resize(param_annot_data_.size() + params.size(), NULL_NODE);
        for (std::size_t i = 0; i < params.size() && i < annots.size(); ++i)
            param_annot_data_[pstart + i] = annots[i];
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
    }

    [[nodiscard]] NodeId add_let(SymId name, NodeId val, NodeId body) {
        auto id = add_node(NodeTag::Let);
        sym_id_[id] = name;
        children_[id] = PersistentChildVector<NodeId>(
            2, [&](std::size_t i) -> NodeId { return (i == 0 ? val : body); });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_letrec(SymId name, NodeId val, NodeId body) {
        auto id = add_node(NodeTag::LetRec);
        sym_id_[id] = name;
        children_[id] = PersistentChildVector<NodeId>(
            2, [&](std::size_t i) -> NodeId { return (i == 0 ? val : body); });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_define(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Define);
        sym_id_[id] = name;
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return val; });
        link_children(id);
        return id;
    }

    // Issue #150 Phase 1: region annotation side-table.
    // Maps either a define's name (SymId) or a lambda's
    // NodeId (for anonymous lambdas) to the user's
    // performance-region / evolution-region hint. The
    // lowering pass (FlatFnBuilder in lowering_impl.cpp)
    // reads this table to set IRFunction::region on the
    // corresponding IRFunction. The side-table is keyed by
    // either a SymId (for defines) or a NodeId (for anonymous
    // lambdas) — we use two parallel maps to keep the
    // namespaces separate.
    //
    // The parser populates this via set_function_region(sym, r)
    // for a define, and set_function_region(node_id, r) for a
    // lambda. The lowering pass queries via
    // get_function_region_for_sym(sym) and
    // get_function_region_for_lambda(node_id) — both return
    // std::optional<std::uint8_t>; nullopt means no annotation.
    void set_function_region(SymId name, std::uint8_t region) {
        region_by_sym_[name] = region;
        // Issue #1520: dense SoA side-table for hot SymId lookups.
        // Encoding: 0 = unset, region+1 stored (region is 0..254).
        if (static_cast<std::size_t>(name) < kRegionDenseCap) {
            if (region_by_sym_dense_.size() <= static_cast<std::size_t>(name))
                region_by_sym_dense_.resize(static_cast<std::size_t>(name) + 1, 0);
            region_by_sym_dense_[static_cast<std::size_t>(name)] =
                static_cast<std::uint8_t>(region + 1);
        }
    }
    // Overload tag: pass a 0 literal to disambiguate. We
    // use a sentinel parameter (an unused int) so the two
    // overloads don't collide on the same uint32_t underlying
    // type. Callers use set_function_region_sym and
    // set_function_region_lambda explicitly.
    void set_function_region_lambda(NodeId lambda_id, std::uint8_t region) {
        region_by_lambda_id_[lambda_id] = region;
        // Issue #1520: dense side-table for hot lambda NodeId lookups.
        if (static_cast<std::size_t>(lambda_id) < kRegionDenseCap) {
            if (region_by_lambda_dense_.size() <= static_cast<std::size_t>(lambda_id))
                region_by_lambda_dense_.resize(static_cast<std::size_t>(lambda_id) + 1, 0);
            region_by_lambda_dense_[static_cast<std::size_t>(lambda_id)] =
                static_cast<std::uint8_t>(region + 1);
        }
    }
    [[nodiscard]] std::optional<std::uint8_t> get_function_region_for_sym(SymId name) const {
        // Issue #1520: prefer dense columnar path (no hash).
        if (static_cast<std::size_t>(name) < region_by_sym_dense_.size()) {
            const auto enc = region_by_sym_dense_[static_cast<std::size_t>(name)];
            if (enc != 0) {
                region_dense_hits_.fetch_add(1, std::memory_order_relaxed);
                return static_cast<std::uint8_t>(enc - 1);
            }
        }
        region_map_lookups_.fetch_add(1, std::memory_order_relaxed);
        auto it = region_by_sym_.find(name);
        if (it == region_by_sym_.end())
            return std::nullopt;
        return it->second;
    }
    [[nodiscard]] std::optional<std::uint8_t>
    get_function_region_for_lambda(NodeId lambda_id) const {
        if (static_cast<std::size_t>(lambda_id) < region_by_lambda_dense_.size()) {
            const auto enc = region_by_lambda_dense_[static_cast<std::size_t>(lambda_id)];
            if (enc != 0) {
                region_dense_hits_.fetch_add(1, std::memory_order_relaxed);
                return static_cast<std::uint8_t>(enc - 1);
            }
        }
        region_map_lookups_.fetch_add(1, std::memory_order_relaxed);
        auto it = region_by_lambda_id_.find(lambda_id);
        if (it == region_by_lambda_id_.end())
            return std::nullopt;
        return it->second;
    }

    // Issue #150 Phase 3: mutation impact analysis helper.
    // Given a SymId (the name that was mutated), find all
    // Define nodes whose value subtree references that
    // sym. Returns the list of Define node IDs whose
    // functions are potentially affected by a
    // mutate:rebind on the given name.
    //
    // Conservative MVP: direct reference only. A function
    // that calls another function which uses the mutated
    // name is NOT flagged (transitive analysis is a
    // follow-up). This is a safe underestimate: a function
    // that DOES directly reference the mutated sym is
    // flagged, and the caller of such a function MAY also
    // need invalidation but isn't flagged. The follow-up
    // (Phase 3b) would walk the call graph from these
    // results to find the transitive set.
    [[nodiscard]] std::pmr::vector<aura::ast::NodeId> defines_referencing_sym(SymId sym) const {
        std::pmr::vector<aura::ast::NodeId> result(
            std::pmr::polymorphic_allocator<aura::ast::NodeId>{});
        for (aura::ast::NodeId i = 0; i < size(); ++i) {
            if (i >= size())
                break;
            auto v = get(i);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            if (v.sym_id == sym)
                continue; // the mutated Define itself
            if (v.children.empty())
                continue;
            if (subtree_uses_sym(v.child(0), sym)) {
                result.push_back(i);
            }
        }
        return result;
    }

    // Recursive helper: does the subtree rooted at `root`
    // contain a Variable node whose sym_id == `sym`?
    //
    // Phase A hoisting migration: now uses
    // aura::ast::find_first_node_with<Id, C, P> from the
    // generic AST traversal helpers (originally defined in
    // aura.compiler.query, hoisted here to break the ast ↔
    // query import cycle). Same semantics — walks the subtree
    // recursively, returns true on first matching Variable
    // node. Zero behavior change at -O3 (template-inlined).
    bool subtree_uses_sym(aura::ast::NodeId root, SymId sym) const {
        if (root == aura::ast::NULL_NODE || root >= size())
            return false;
        auto found =
            find_first_node_with<std::uint32_t>(*this, root, [this, sym](aura::ast::NodeId id) {
                auto v = this->get(id);
                return v.tag == aura::ast::NodeTag::Variable && v.sym_id == sym;
            });
        return found.has_value();
    }

    // Issue #372: name-based Define lookup.
    //
    // Walks the AST subtree rooted at `root_` (or an explicit
    // root override) and returns the first Define node whose
    // sym_id matches the given name in the supplied pool.
    //
    // The pool is a separate argument (rather than a stored
    // member) because FlatAST is the AST data, not the
    // symbol-table — multiple Flats can share a single
    // StringPool (e.g. parent layer → child layer after COW
    // clone), or one Flat can be parsed against multiple
    // pools across its lifetime. The caller is responsible
    // for passing the pool that this Flat was parsed against
    // (workspace_flat_'s companion workspace_pool_).
    //
    // Returns std::nullopt if the name isn't interned in the
    // pool, the root is NULL_NODE, or no Define with that
    // sym_id exists in the reachable subtree.
    [[nodiscard]] std::optional<aura::ast::NodeId>
    find_define_by_name(const StringPool& pool, std::string_view name,
                        std::optional<aura::ast::NodeId> search_root = std::nullopt) const {
        const auto sym = pool.find_by_name(name);
        if (!sym)
            return std::nullopt;
        const auto start = search_root.value_or(root);
        if (start == aura::ast::NULL_NODE || start >= size())
            return std::nullopt;
        return find_first_node_with<std::uint32_t>(
            *this, start, [this, sym_id = *sym](aura::ast::NodeId id) {
                auto v = this->get(id);
                return v.tag == aura::ast::NodeTag::Define && v.sym_id == sym_id;
            });
    }

    // Issue #1685 / #1687: re-resolve a Define after parse_to_flat appends
    // into this FlatAST. Prefer `preferred` when it is still a live matching
    // Define in [0, size_before_parse); otherwise scan that range only so a
    // parse-appended full "(define name ...)" form is not mistaken for the
    // original binding (and free-list recycle at lower ids is detected).
    [[nodiscard]] NodeId resolve_define_after_parse(SymId sym, NodeId preferred,
                                                    std::size_t size_before_parse) const {
        const auto limit = std::min(size_before_parse, static_cast<std::size_t>(size()));
        if (preferred != NULL_NODE && static_cast<std::size_t>(preferred) < limit &&
            !is_free_slot(preferred)) {
            auto v = get(preferred);
            if (v.tag == NodeTag::Define && v.sym_id == sym)
                return preferred;
        }
        for (NodeId id = 0; static_cast<std::size_t>(id) < limit; ++id) {
            if (is_free_slot(id))
                continue;
            auto v = get(id);
            if (v.tag == NodeTag::Define && v.sym_id == sym)
                return id;
        }
        return NULL_NODE;
    }

    // Issue #1687: after resolve_define_after_parse, re-derive the Lambda
    // child of a (define (name ...) ...) form. Returns NULL_NODE if the
    // Define is missing or its value child is not a Lambda.
    [[nodiscard]] NodeId resolve_lambda_child_of_define(NodeId define_id) const {
        if (define_id == NULL_NODE || define_id >= size() || is_free_slot(define_id))
            return NULL_NODE;
        auto v = get(define_id);
        if (v.tag != NodeTag::Define || v.children.size() != 1)
            return NULL_NODE;
        auto lambda_id = v.child(0);
        if (lambda_id >= size() || is_free_slot(lambda_id))
            return NULL_NODE;
        if (get(lambda_id).tag != NodeTag::Lambda)
            return NULL_NODE;
        return lambda_id;
    }

    [[nodiscard]] NodeId add_define_type(SymId name, std::span<const SymId> params,
                                         std::span<const NodeId> ctors) {
        auto id = add_node(NodeTag::DefineType);
        sym_id_[id] = name;
        // Store type params in param_data_
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_annot_data_.resize(param_annot_data_.size() + params.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        children_[id] = PersistentChildVector<NodeId>(ctors.begin(), ctors.end());
        return id;
    }

    [[nodiscard]] NodeId add_begin(NodeId* exprs, std::uint32_t count) {
        auto id = add_node(NodeTag::Begin);
        // Build the N-element PCV directly from the exprs array via
        // the fill-constructor (single allocation, no temp vector).
        children_[id] =
            PersistentChildVector<NodeId>(count, [&](std::size_t i) -> NodeId { return exprs[i]; });
        link_children(id);
        return id;
    }
    [[nodiscard]] NodeId add_begin(std::span<const NodeId> exprs) {
        auto id = add_node(NodeTag::Begin);
        children_[id] = PersistentChildVector<NodeId>(
            exprs.size(), [&](std::size_t i) -> NodeId { return exprs[i]; });
        link_children(id);
        return id;
    }

    // Export: (export sym1 sym2 ...) — children = Variable nodes
    [[nodiscard]] NodeId add_define_module(SymId name, std::span<const SymId> type_params) {
        auto id = add_node(NodeTag::DefineModule);
        sym_id_[id] = name;
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), type_params.begin(), type_params.end());
        param_annot_data_.resize(param_annot_data_.size() + type_params.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(type_params.size());
        return id;
    }
    [[nodiscard]] NodeId add_define_module(SymId name, std::span<const SymId> type_params,
                                           std::span<const SymId> cap_require) {
        auto id = add_node(NodeTag::DefineModule);
        sym_id_[id] = name;
        // type params first
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), type_params.begin(), type_params.end());
        param_annot_data_.resize(param_annot_data_.size() + type_params.size(), NULL_NODE);
        // capability requirements after type params (no separator needed, we store count)
        param_data_.insert(param_data_.end(), cap_require.begin(), cap_require.end());
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(type_params.size());
        cap_require_count_[id] = static_cast<std::uint32_t>(cap_require.size());
        return id;
    }

    [[nodiscard]] NodeId add_export(std::span<const NodeId> syms) {
        auto id = add_node(NodeTag::Export);
        children_[id] = PersistentChildVector<NodeId>(
            syms.size(), [&](std::size_t i) -> NodeId { return syms[i]; });
        link_children(id);
        return id;
    }

    // Issue #311: SV `interface` builder. Mirrors add_export's
    // shape (PersistentChildVector of body NodeIds) + stores
    // the interface name in `sym_id_` (same shape as
    // add_define_module's param_data_ side-table would, but
    // body items are NodeIds — signal declarations, nested
    // modport nodes, etc. — rather than bare symbols). Uses
    // the PCV fill-constructor pattern to preserve copy-on-
    // write semantics, and calls link_children() to wire up
    // parent_[child] = id for each body item.
    //
    // The body items can be any NodeId (Define for signal
    // decls, Modport for nested modports, Begin for module
    // bodies, etc.). The builder doesn't constrain what the
    // body contains — the parser / EDSL surface is
    // responsible for that.
    [[nodiscard]] NodeId add_interface(SymId name, std::span<const NodeId> body) {
        auto id = add_node(NodeTag::Interface);
        sym_id_[id] = name;
        children_[id] = PersistentChildVector<NodeId>(
            body.size(), [&](std::size_t i) -> NodeId { return body[i]; });
        link_children(id);
        return id;
    }

    // Issue #311: SV `modport` builder. Uses the param_data_
    // side-table (same shape as add_define_module's
    // type_params) to store the port-direction list as a
    // flat symbol vector with offset/count indices. The
    // direction information (input/output/inout) is captured
    // implicitly via the port symbol's name (a future parser
    // surface will decode e.g. "input data" -> {data, INPUT}
    // and emit the modport accordingly; the builder just
    // records the symbol list for now).
    //
    // Like add_define_module, no children_ is set (a modport
    // is a leaf construct carrying only the port list); the
    // param_data_ side-table is the canonical payload.
    [[nodiscard]] NodeId add_modport(SymId name, std::span<const SymId> ports) {
        auto id = add_node(NodeTag::Modport);
        sym_id_[id] = name;
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), ports.begin(), ports.end());
        param_annot_data_.resize(param_annot_data_.size() + ports.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(ports.size());
        return id;
    }

    // Issue #694: SVA builders. Property/Sequence store the expr as a
    // LiteralString child; Coverpoint uses param_data_ for bins (like
    // Modport); Covergroup stores Coverpoint children; Assert references
    // a Property child.
    [[nodiscard]] NodeId add_property(SymId name, SymId expr_sym) {
        auto expr_id = add_literalstring(expr_sym);
        auto id = add_node(NodeTag::Property);
        sym_id_[id] = name;
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t) -> NodeId { return expr_id; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_sequence(SymId name, SymId expr_sym) {
        auto expr_id = add_literalstring(expr_sym);
        auto id = add_node(NodeTag::Sequence);
        sym_id_[id] = name;
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t) -> NodeId { return expr_id; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_coverpoint(SymId var, std::span<const SymId> bins) {
        auto id = add_node(NodeTag::Coverpoint);
        sym_id_[id] = var;
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), bins.begin(), bins.end());
        param_annot_data_.resize(param_annot_data_.size() + bins.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(bins.size());
        return id;
    }

    [[nodiscard]] NodeId add_covergroup(SymId name, std::span<const NodeId> coverpoints) {
        auto id = add_node(NodeTag::Covergroup);
        sym_id_[id] = name;
        children_[id] = PersistentChildVector<NodeId>(
            coverpoints.size(), [&](std::size_t i) -> NodeId { return coverpoints[i]; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_assert(SymId name, NodeId property_id) {
        auto id = add_node(NodeTag::Assert);
        sym_id_[id] = name;
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t) -> NodeId { return property_id; });
        link_children(id);
        return id;
    }

    // Issue #496: SV constraint builder. Expressions stored in param_data_
    // (same side-table shape as Coverpoint bins / Modport ports).
    [[nodiscard]] NodeId add_constraint(SymId name, std::span<const SymId> expressions) {
        auto id = add_node(NodeTag::Constraint);
        sym_id_[id] = name;
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), expressions.begin(), expressions.end());
        param_annot_data_.resize(param_annot_data_.size() + expressions.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(expressions.size());
        return id;
    }

    // Issue #496: SV class builder. Optional base class stored as the first
    // param_data_ entry when base_sym != INVALID_SYM; body items are children_.
    [[nodiscard]] NodeId add_class(SymId name, SymId base_sym, std::span<const NodeId> body) {
        auto id = add_node(NodeTag::Class);
        sym_id_[id] = name;
        if (base_sym != INVALID_SYM) {
            auto pstart = static_cast<std::uint32_t>(param_data_.size());
            param_data_.push_back(base_sym);
            param_annot_data_.push_back(NULL_NODE);
            param_begin_[id] = pstart;
            param_count_[id] = 1;
        }
        children_[id] = PersistentChildVector<NodeId>(
            body.size(), [&](std::size_t i) -> NodeId { return body[i]; });
        link_children(id);
        return id;
    }

    void append_param(NodeId id, SymId val) {
        if (id >= param_begin_.size())
            return;
        param_data_.push_back(val);
        param_annot_data_.push_back(NULL_NODE);
        ++param_count_[id];
    }

    [[nodiscard]] NodeId add_set(SymId name, NodeId val) {
        auto id = add_node(NodeTag::Set);
        sym_id_[id] = name;
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return val; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_macrodef(SymId name, const std::vector<SymId>& params, NodeId body,
                                      bool dotted = false, bool hygienic = false,
                                      bool preserved = false) {
        auto id = add_node(NodeTag::MacroDef);
        sym_id_[id] = name;
        // Issue #120: dotted in bit 0, hygienic in bit 1.
        // Issue #230 #2: bit 2 = preserved. When set, this macro
        // uses the env-binding expansion path (params bound in a
        // child env, no AST subst) instead of the AST-subst
        // path. The env-binding path is what symbol-generating
        // macros like define-struct need: the body can reference
        // the user's actual struct name and field list as
        // Variables and get the literal values back. The `&`
        // sigil in the param list is just a marker for
        // readability — if the macro is `define-hygienic-macro*`
        // (preserved), ALL params use the env-binding semantics.
        int_val_[id] = (preserved ? 4 : 0) | (hygienic ? 2 : 0) | (dotted ? 1 : 0);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return body; });
        // Issue #484 follow-up: link_children so the body's parent_
        // points to this MacroDef. Without this, the macro body
        // is an orphan (parent_ = NULL), which causes query:pattern
        // to exclude it (orphan-skip after #484). The test
        // test_issue_140 AC2.3 and AC4.2 depend on the macro body
        // being queryable as User-marker code.
        link_children(id);
        auto pstart = static_cast<std::uint32_t>(param_data_.size());
        param_data_.insert(param_data_.end(), params.begin(), params.end());
        param_annot_data_.resize(param_annot_data_.size() + params.size(), NULL_NODE);
        param_begin_[id] = pstart;
        param_count_[id] = static_cast<std::uint32_t>(params.size());
        return id;
    }

    // Issue #120: query the hygienic flag on a MacroDef node.
    // Encoded in bit 1 of int_val_ (dotted is bit 0).
    bool is_hygienic_macrodef(NodeId id) const {
        if (id >= int_val_.size())
            return false;
        return (int_val_[id] & 2) != 0;
    }
    bool is_dotted_macrodef(NodeId id) const {
        if (id >= int_val_.size())
            return false;
        return (int_val_[id] & 1) != 0;
    }
    // Issue #230 #2: query the preserved flag (set by
    // `define-hygienic-macro*`). When true, the macro uses
    // env-binding expansion (params bound in a child env)
    // instead of AST substitution.
    bool is_preserved_macrodef(NodeId id) const {
        if (id >= int_val_.size())
            return false;
        return (int_val_[id] & 4) != 0;
    }

    [[nodiscard]] NodeId add_quote(NodeId val) {
        auto id = add_node(NodeTag::Quote);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return val; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_pair(NodeId car, NodeId cdr) {
        auto id = add_node(NodeTag::Pair);
        children_[id] = PersistentChildVector<NodeId>(
            2, [&](std::size_t i) -> NodeId { return (i == 0 ? car : cdr); });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_type_annotation(SymId type_name, NodeId inner,
                                             SymId var_sym = INVALID_SYM) {
        auto id = add_node(NodeTag::TypeAnnotation);
        sym_id_[id] = type_name;
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        if (var_sym != INVALID_SYM) {
            int_val_[id] = static_cast<std::int64_t>(var_sym);
        }
        link_children(id);
        return id;
    }

    bool has_var_annot(NodeId id) const { return id < size() && int_val_[id] != 0; }
    SymId var_annot_sym(NodeId id) const { return static_cast<SymId>(int_val_[id]); }

    [[nodiscard]] NodeId add_coercion(NodeId inner, std::uint32_t type_id) {
        auto id = add_node(NodeTag::Coercion);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        type_id_[id] = type_id;
        link_children(id);
        return id;
    }
    [[nodiscard]] NodeId add_coercion(NodeId inner, std::uint32_t type_tag, std::uint32_t type_id) {
        auto id = add_node(NodeTag::Coercion);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        int_val_[id] = static_cast<std::int64_t>(type_tag);
        type_id_[id] = type_id;
        link_children(id);
        return id;
    }
    // ── M4 Linear ownership builders ───────────────────────────
    [[nodiscard]] NodeId add_linear(NodeId inner) {
        auto id = add_node(NodeTag::Linear);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_move(NodeId inner) {
        auto id = add_node(NodeTag::Move);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_borrow(NodeId inner) {
        auto id = add_node(NodeTag::Borrow);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_mut_borrow(NodeId inner) {
        auto id = add_node(NodeTag::MutBorrow);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_drop(NodeId inner) {
        auto id = add_node(NodeTag::Drop);
        children_[id] =
            PersistentChildVector<NodeId>(1, [&](std::size_t i) -> NodeId { return inner; });
        link_children(id);
        return id;
    }

    // ── Access ─────────────────────────────────────────────────

    NodeView get(NodeId id) const {
        // Issue #1620: hot-path SoA column access invariant probe
        // (zero cost under release observe; Agents track hits).
        if (id < tag_.size()) {
            aura::core::cpp26::record_hotpath_invariant_hit();
            // In-bounds NodeId → primary SoA columns must cover it.
            contract_assert(id < int_val_.size());
            contract_assert(id < sym_id_.size());
        }
        if (id >= tag_.size()) {
            // Defensive: stale or invalid NodeId. Return a default
            // NodeView (empty spans, NULL_NODE-like values). The
            // parser can produce invalid NodeIds during
            // quasiquote expansion (Issue #219 regression) when a
            // previously-captured NodeView's underlying buffer
            // moved. Without this check, the next access would
            // crash on tag_[id] / child_data_[begin] / etc.
            return NodeView{
                .id = id,
                .tag = static_cast<NodeTag>(0),
                .int_value = 0,
                .float_value = 0.0,
                .sym_id = INVALID_SYM,
                .line = 0,
                .col = 0,
                .type_id = 0u,
                .children = {},
                .params = {},
                .param_annotations = {},
                .marker = SyntaxMarker::User,
            };
        }
        return NodeView{
            .id = id,
            .tag = tag_[id],
            .int_value = int_val_[id],
            .float_value = float_val_[id],
            .sym_id = sym_id_[id],
            .line = id < line_.size() ? line_[id] : 0,
            .col = id < col_.size() ? col_[id] : 0,
            // Issue #73 Phase 2: populate type_id on the view so
            // callers can read it without a separate flat.type_id()
            // lookup. Same value as flat.type_id(id).
            .type_id = id < type_id_.size() ? type_id_[id] : 0u,
            .children = std::span<const NodeId>(children_[id].data(), children_[id].size()),
            .params = std::span(param_data_.data() + param_begin_[id], param_count_[id]),
            .param_annotations =
                std::span(param_annot_data_.data() + param_begin_[id], param_count_[id]),
            .marker = id < marker_.size() ? marker_[id] : SyntaxMarker::User,
        };
    }

    // ── Location ──────────────────────────────────────────────
    void set_loc(NodeId id, std::uint32_t line, std::uint32_t col) pre(id < line_.size())
        pre(id < col_.size()) {
        line_[id] = line;
        col_[id] = col;
    }
    std::uint32_t line(NodeId id) const { return line_[id]; }
    std::uint32_t col(NodeId id) const { return col_[id]; }

    // Direct field access (for mutation)
    NodeTag& tag(NodeId id) { return tag_[id]; }
    std::int64_t& int_val(NodeId id) { return int_val_[id]; }
    SymId& sym_id(NodeId id) { return sym_id_[id]; }

    // Const overloads so ASTContainer<const FlatAST, Id>
    // is satisfied (concept requires `ast.tag(id)` callable
    // on const ast). Used by find_first_node_with<Id, const
    // FlatAST, P> when called from const methods like
    // FlatAST::subtree_uses_sym.
    // Issue #1321: hot SoA column accessors — contract pre(id valid) in debug
    // so AI mutation corruption is caught early (release: no-op).
    [[nodiscard]] NodeTag tag(NodeId id) const noexcept {
        contract_assert(id < tag_.size());
        return tag_[id];
    }
    [[nodiscard]] std::int64_t int_val(NodeId id) const noexcept {
        contract_assert(id < int_val_.size());
        return int_val_[id];
    }
    [[nodiscard]] SymId sym_id(NodeId id) const noexcept {
        contract_assert(id < sym_id_.size());
        return sym_id_[id];
    }

    // ── Parent access ──────────────────────────────────────────
    NodeId parent_of(NodeId id) const {
        // Issue #256: bump the call counter (lifetime total).
        parent_of_call_count_.fetch_add(1, std::memory_order_relaxed);
        return id < parent_.size() ? parent_[id] : NULL_NODE;
    }

    // Issue #1689: mark inverted parent-edge index stale (bulk topology
    // changes). Next collect/lookup rebuilds O(N+E).
    void mark_incoming_parent_index_dirty() const noexcept { incoming_parent_index_dirty_ = true; }

    // Issue #1689: rebuild child→[(parent,idx)...] from children_ SoA.
    void rebuild_incoming_parent_index() const {
        const auto n = size();
        incoming_parent_edges_.assign(n, {});
        for (NodeId id = 0; id < n; ++id) {
            if (is_free_slot(id) || id >= children_.size())
                continue;
            const auto& ch = children_[id];
            for (std::size_t ci = 0; ci < ch.size(); ++ci) {
                auto cid = ch[static_cast<std::uint32_t>(ci)];
                if (cid == NULL_NODE || cid >= n)
                    continue;
                incoming_parent_edges_[cid].emplace_back(id, static_cast<std::uint32_t>(ci));
            }
        }
        incoming_parent_index_dirty_ = false;
        incoming_parent_index_rebuilds_.fetch_add(1, std::memory_order_relaxed);
    }

    void ensure_incoming_parent_index() const {
        if (incoming_parent_index_dirty_)
            rebuild_incoming_parent_index();
    }

    // Issue #1689: O(deg) collect of all (parent, child_index) edges to
    // `target`, sorted for safe multi-remove (same parent → higher index first).
    [[nodiscard]] std::vector<std::pair<NodeId, std::uint32_t>>
    collect_incoming_parent_edges(NodeId target) const {
        incoming_parent_index_lookups_.fetch_add(1, std::memory_order_relaxed);
        ensure_incoming_parent_index();
        incoming_parent_index_hits_.fetch_add(1, std::memory_order_relaxed);
        std::vector<std::pair<NodeId, std::uint32_t>> edges;
        if (target == NULL_NODE || target >= incoming_parent_edges_.size())
            return edges;
        const auto& src = incoming_parent_edges_[target];
        edges.assign(src.begin(), src.end());
        std::ranges::sort(edges, [](const auto& a, const auto& b) {
            if (a.first != b.first)
                return a.first < b.first;
            return a.second > b.second;
        });
        return edges;
    }

    [[nodiscard]] std::uint64_t incoming_parent_index_rebuilds() const noexcept {
        return incoming_parent_index_rebuilds_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t incoming_parent_index_lookups() const noexcept {
        return incoming_parent_index_lookups_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t incoming_parent_index_hits() const noexcept {
        return incoming_parent_index_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool incoming_parent_index_dirty() const noexcept {
        return incoming_parent_index_dirty_;
    }

    // ── Child field access ─────────────────────────────────────

    std::span<const NodeId> children(NodeId id) const {
        // Issue #256: bump the call counter (lifetime total).
        // The early-return on out-of-range still bumps — we
        // want to count how often callers probe with bad ids
        // (cheap error metric).
        children_call_count_.fetch_add(1, std::memory_order_relaxed);
        if (id >= children_.size())
            return {};
        // WARNING (Issue #370): the returned std::span borrows
        // the underlying storage. If callers cache this span
        // across mutations (set_child / insert_child /
        // remove_child / rollback_to_size), the span WILL
        // dangle when the storage's last shared_ptr is
        // released. Use children_safe(id) for lifetime-pinned
        // views; reserve raw children(id) for single-statement
        // use within the same mutation boundary.
        return std::span<const NodeId>(children_[id].data(), children_[id].size());
    }

    // Issue #370: lifetime-pinned accessor. Returns a
    // SafePCVSpan<NodeId> that bundles the std::span with the
    // underlying storage shared_ptr. As long as the
    // SafePCVSpan is alive, the storage stays alive — so the
    // returned span stays valid across mutate operations +
    // rollback. One atomic refcount bump per call (amortized
    // over all reads via the same handle).
    // Issue #370/#678: lifetime-pinned children accessor.
    [[nodiscard]] SafePCVSpan<NodeId> children_safe_view(NodeId id) const {
        children_safe_view_count_.fetch_add(1, std::memory_order_relaxed);
        pcv_pin_count_.fetch_add(1, std::memory_order_relaxed);
        children_column_soa_hits_.fetch_add(1, std::memory_order_relaxed);
        if (id >= children_.size())
            return {};
        auto keep = share_storage(children_[id]);
        std::span<const NodeId> sp(children_[id].data(), children_[id].size());
        return SafePCVSpan<NodeId>(sp, std::move(keep));
    }

    [[nodiscard]] SafePCVSpan<NodeId> children_safe(NodeId id) const {
        return children_safe_view(id);
    }

    // Issue #1520 / #1624: preferred columnar children accessor (alias of
    // children_safe with explicit SoA-path metrics). Hot paths
    // (query:pattern, mark_dirty_upward children walk, walk_children)
    // should use this instead of raw children() spans.
    [[nodiscard]] SafePCVSpan<NodeId> children_columnar(NodeId id) const {
        return children_safe_view(id);
    }

    // Issue #1624: contract-guarded single-child read via columnar path
    // (SafePCVSpan / PCV data). Prefer over raw children()[i] on hot paths.
    [[nodiscard]] NodeId get_child(NodeId id, std::uint32_t idx) const
        pre(id == NULL_NODE || id < children_.size()) {
        if (id == NULL_NODE || id >= children_.size())
            return NULL_NODE;
        auto col = children_columnar(id);
        if (idx >= col.size())
            return NULL_NODE;
        return col[idx];
    }

    // Issue #1520: zero-overhead columnar children metrics (for Agents).
    [[nodiscard]] std::uint64_t children_column_soa_hits() const noexcept {
        return children_column_soa_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t pcv_pin_count() const noexcept {
        return pcv_pin_count_.load(std::memory_order_relaxed);
    }
    // Issue #1624: DOD migration progress (columnar hits as progress units).
    [[nodiscard]] std::uint64_t soa_dod_migration_progress() const noexcept {
        return children_column_soa_hits_.load(std::memory_order_relaxed);
    }
    // pcv_columnar hit rate in basis points: columnar / (columnar+raw) * 10000
    [[nodiscard]] std::uint64_t pcv_columnar_hit_rate_bp() const noexcept {
        const auto col = children_column_soa_hits_.load(std::memory_order_relaxed);
        const auto raw = children_call_count_.load(std::memory_order_relaxed);
        const auto den = col + raw;
        if (den == 0)
            return 0;
        return (col * 10000) / den;
    }
    [[nodiscard]] std::uint64_t region_dense_hits() const noexcept {
        return region_dense_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t region_map_lookups() const noexcept {
        return region_map_lookups_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t map_indirection_miss_total() const noexcept {
        // "Miss" = still paid unordered_map cost (dense miss → map).
        return region_map_lookups_.load(std::memory_order_relaxed);
    }

    // Mutable overload (for code paths that need to write through
    // the children list, e.g. fixup_deltas in ast_impl.cpp).
    // PCV is immutable; this returns a const span. For
    // mutation, use insert_child / remove_child / set_child
    // (which return a new PCV and assign back to children_[id]).
    std::span<const NodeId> children_mutable(NodeId id) {
        if (id >= children_.size())
            return {};
        return std::span<const NodeId>(children_[id].data(), children_[id].size());
    }

    // ── Issue #222: structural mutation guard ────────────────────
    //
    // RAII wrapper that holds an exclusive lock on structural_mtx_
    // for its lifetime. On destruction:
    //   1. Bumps generation_ (invalidates StableNodeRef).
    //   2. Releases the exclusive lock.
    //
    // The lock + generation bump is the atomicity guarantee for
    // structural mutations. A reader that wants to see consistent
    // state across (read generation → read children_) can:
    //   - capture generation_ before the read
    //   - read children_
    //   - verify generation_ unchanged after the read
    //   - if changed, retry (with the new generation)
    //
    // Or, use try_acquire_reader_lock() for a longer-lived read
    // transaction.
    //
    // The set_child / insert_child / remove_child methods acquire
    // this guard internally. Callers who need to apply a multi-step
    // mutation atomically (e.g. swap two children in a single
    // "transaction") can acquire the guard explicitly:
    //
    //   {
    //     auto guard = ast.begin_structural_mutation();
    //     auto a = ast.children(id_a);
    //     ast.set_child(id_a, 0, ...);
    //     ast.set_child(id_b, 0, ...);
    //     // guard's dtor bumps generation_ once, releases the lock.
    //   }
    //
    // Move-only. The dtor is the single point that releases the
    // lock + bumps the generation, so even exception paths are safe.
    class StructuralMutationGuard {
    public:
        StructuralMutationGuard() noexcept = default;
        explicit StructuralMutationGuard(FlatAST* ast) noexcept
            : ast_(ast)
            , lock_() {
            if (ast_)
                lock_ = std::unique_lock<std::shared_mutex>(ast->structural_mtx_.get());
        }
        ~StructuralMutationGuard() {
            if (ast_) {
                // Issue #250: count suppressed bumps in the
                // FlatAST's atomic_batch_bumps_saved_ counter.
                // The actual bump is still done by FlatAST's
                // bump_generation() method, which short-circuits
                // when bump_generation_suppressed_ is set.
                if (ast_->bump_generation_suppressed_) {
                    ++ast_->atomic_batch_bumps_saved_;
                }
                ast_->bump_generation();
            }
            // unique_lock's dtor releases the lock.
        }
        StructuralMutationGuard(const StructuralMutationGuard&) = delete;
        StructuralMutationGuard& operator=(const StructuralMutationGuard&) = delete;
        StructuralMutationGuard(StructuralMutationGuard&& o) noexcept
            : ast_(o.ast_)
            , lock_(std::move(o.lock_)) {
            o.ast_ = nullptr;
        }
        StructuralMutationGuard& operator=(StructuralMutationGuard&& o) noexcept {
            if (this != &o) {
                // Release any current lock first.
                if (ast_)
                    ast_->bump_generation();
                ast_ = o.ast_;
                lock_ = std::move(o.lock_);
                o.ast_ = nullptr;
            }
            return *this;
        }
        // Returns true if the guard holds a valid lock.
        [[nodiscard]] explicit operator bool() const noexcept {
            return ast_ != nullptr && lock_.owns_lock();
        }

    private:
        FlatAST* ast_ = nullptr;
        std::unique_lock<std::shared_mutex> lock_;
    };

    // Acquire an exclusive lock on structural_mtx_ for the
    // duration of the returned guard's lifetime. Used by
    // set_child / insert_child / remove_child internally;
    // callers can also acquire it explicitly for multi-step
    // atomic mutations.
    [[nodiscard]] StructuralMutationGuard begin_structural_mutation() {
        return StructuralMutationGuard(this);
    }

    // Acquire a SHARED (reader) lock on structural_mtx_.
    // The returned guard's lifetime is the duration of the
    // reader's transaction. Use for long-lived reads that
    // need a consistent view of children_ across multiple
    // calls. For short reads (one children(id) call), the
    // PCV's COW semantics + generation_ check are sufficient.
    class ReaderLockGuard {
    public:
        ReaderLockGuard() noexcept = default;
        explicit ReaderLockGuard(const FlatAST* ast) noexcept
            : ast_(ast)
            , lock_() {
            if (ast_)
                lock_ = std::shared_lock<std::shared_mutex>(ast->structural_mtx_.mutable_get());
        }
        ~ReaderLockGuard() = default;
        ReaderLockGuard(const ReaderLockGuard&) = delete;
        ReaderLockGuard& operator=(const ReaderLockGuard&) = delete;
        ReaderLockGuard(ReaderLockGuard&& o) noexcept
            : ast_(o.ast_)
            , lock_(std::move(o.lock_)) {
            o.ast_ = nullptr;
        }
        ReaderLockGuard& operator=(ReaderLockGuard&& o) noexcept {
            if (this != &o) {
                ast_ = o.ast_;
                lock_ = std::move(o.lock_);
                o.ast_ = nullptr;
            }
            return *this;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return ast_ != nullptr && lock_.owns_lock();
        }

    private:
        const FlatAST* ast_ = nullptr;
        std::shared_lock<std::shared_mutex> lock_;
    };
    [[nodiscard]] ReaderLockGuard try_acquire_reader_lock() const { return ReaderLockGuard(this); }

    // Issue #1783: exclusive lock on metadata_mtx_ (marker_ /
    // provenance_). Unlike StructuralMutationGuard, does NOT bump
    // generation_ — marker/provenance are observational metadata.
    class MetadataWriteGuard {
    public:
        MetadataWriteGuard() noexcept = default;
        explicit MetadataWriteGuard(FlatAST* ast) noexcept
            : ast_(ast)
            , lock_() {
            if (ast_)
                lock_ = std::unique_lock<std::shared_mutex>(ast->metadata_mtx_.get());
        }
        ~MetadataWriteGuard() = default;
        MetadataWriteGuard(const MetadataWriteGuard&) = delete;
        MetadataWriteGuard& operator=(const MetadataWriteGuard&) = delete;
        MetadataWriteGuard(MetadataWriteGuard&& o) noexcept
            : ast_(o.ast_)
            , lock_(std::move(o.lock_)) {
            o.ast_ = nullptr;
        }
        MetadataWriteGuard& operator=(MetadataWriteGuard&& o) noexcept {
            if (this != &o) {
                ast_ = o.ast_;
                lock_ = std::move(o.lock_);
                o.ast_ = nullptr;
            }
            return *this;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return ast_ != nullptr && lock_.owns_lock();
        }

    private:
        FlatAST* ast_ = nullptr;
        std::unique_lock<std::shared_mutex> lock_;
    };
    [[nodiscard]] MetadataWriteGuard begin_metadata_mutation() { return MetadataWriteGuard(this); }

    // Issue #1783: shared (reader) lock on metadata_mtx_.
    class MetadataReadGuard {
    public:
        MetadataReadGuard() noexcept = default;
        explicit MetadataReadGuard(const FlatAST* ast) noexcept
            : ast_(ast)
            , lock_() {
            if (ast_)
                lock_ = std::shared_lock<std::shared_mutex>(ast->metadata_mtx_.mutable_get());
        }
        ~MetadataReadGuard() = default;
        MetadataReadGuard(const MetadataReadGuard&) = delete;
        MetadataReadGuard& operator=(const MetadataReadGuard&) = delete;
        MetadataReadGuard(MetadataReadGuard&& o) noexcept
            : ast_(o.ast_)
            , lock_(std::move(o.lock_)) {
            o.ast_ = nullptr;
        }
        MetadataReadGuard& operator=(MetadataReadGuard&& o) noexcept {
            if (this != &o) {
                ast_ = o.ast_;
                lock_ = std::move(o.lock_);
                o.ast_ = nullptr;
            }
            return *this;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return ast_ != nullptr && lock_.owns_lock();
        }

    private:
        const FlatAST* ast_ = nullptr;
        std::shared_lock<std::shared_mutex> lock_;
    };
    [[nodiscard]] MetadataReadGuard try_acquire_metadata_reader_lock() const {
        return MetadataReadGuard(this);
    }

    // Issue #222 slice 3/3: _locked() variants of the structural
    // mutators. Caller MUST hold the structural mutation lock
    // (e.g. is inside a begin_structural_mutation() scope). Used
    // for multi-step atomic mutations where the caller wants to
    // batch several set_child / insert_child / remove_child calls
    // under a single lock + single generation bump. Without these,
    // calling set_child inside begin_structural_mutation would
    // double-lock the non-recursive std::shared_mutex and deadlock.
    //
    // The body is identical to the public version except for the
    // guard acquisition.
    // Issue #1624: Contracts on structural child mutation — pre(id valid).
    // post: children count preserved (with_set), dirty bits via mark paths.
    // Issue #1689: helpers for incremental inverted parent-edge index.
    void incoming_index_ensure_size(NodeId need) const {
        if (need == NULL_NODE)
            return;
        if (incoming_parent_edges_.size() <= static_cast<std::size_t>(need))
            incoming_parent_edges_.resize(static_cast<std::size_t>(need) + 1);
    }
    void incoming_index_remove_edge(NodeId child, NodeId parent, std::uint32_t idx) const {
        if (child == NULL_NODE || child >= incoming_parent_edges_.size())
            return;
        auto& edges = incoming_parent_edges_[child];
        edges.erase(
            std::remove_if(edges.begin(), edges.end(),
                           [&](const auto& e) { return e.first == parent && e.second == idx; }),
            edges.end());
    }
    void incoming_index_add_edge(NodeId child, NodeId parent, std::uint32_t idx) const {
        if (child == NULL_NODE)
            return;
        incoming_index_ensure_size(child);
        incoming_parent_edges_[child].emplace_back(parent, idx);
    }
    void incoming_index_shift_parent_indices(NodeId parent, std::uint32_t from_idx,
                                             int delta) const {
        // Adjust exactly one edge per child slot under `parent` at index ci
        // (match e.second == ci). Matching >= from_idx would double-count when
        // the same child NodeId appears in multiple slots.
        const auto& list = children_[parent];
        for (std::uint32_t ci = from_idx; ci < list.size(); ++ci) {
            auto c = list[ci];
            if (c == NULL_NODE || c >= incoming_parent_edges_.size())
                continue;
            for (auto& e : incoming_parent_edges_[c]) {
                if (e.first == parent && e.second == ci) {
                    if (delta < 0 && e.second == 0)
                        break;
                    e.second = static_cast<std::uint32_t>(static_cast<int>(e.second) + delta);
                    break;
                }
            }
        }
    }

    void set_child_locked(NodeId id, std::uint32_t idx, NodeId child) pre(id < children_.size()) {
        contract_assert(id < children_.size());
        const auto& list = children_[id];
        if (idx >= list.size())
            return;
        const auto old_size = list.size();
        auto old_cid = list[idx];
        if (old_cid != NULL_NODE && old_cid < parent_.size())
            parent_[old_cid] = NULL_NODE;
        children_[id] = list.with_set(idx, child);
        contract_assert(children_[id].size() == old_size);
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
        // Issue #1689: incremental inverted index (when valid).
        if (!incoming_parent_index_dirty_) {
            if (old_cid != NULL_NODE)
                incoming_index_remove_edge(old_cid, id, idx);
            if (child != NULL_NODE)
                incoming_index_add_edge(child, id, idx);
        }
        add_mutation_child_op(id, idx, old_cid, child, "structural-set-child");
    }
    void insert_child_locked(NodeId id, std::uint32_t idx, NodeId child) {
        const auto& list = children_[id];
        auto pos = std::min(static_cast<std::uint32_t>(list.size()), idx);
        // Issue #1689: shift indices of edges at/after pos before insert.
        if (!incoming_parent_index_dirty_ && pos < list.size())
            incoming_index_shift_parent_indices(id, pos, /*delta=*/+1);
        children_[id] = list.with_insert(pos, child);
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
        if (!incoming_parent_index_dirty_ && child != NULL_NODE)
            incoming_index_add_edge(child, id, pos);
        add_mutation_child_op(id, pos, NULL_NODE, child, "structural-insert-child");
        // Issue #1319 Phase 1: count structural inserts; full GapBuffer-backed
        // children_ column is progressive (PCV COW retained for snapshot/rollback).
        structural_mutate_insert_total_.fetch_add(1, std::memory_order_relaxed);
    }
    void remove_child_locked(NodeId id, std::uint32_t idx) {
        const auto& list = children_[id];
        if (idx < list.size()) {
            auto cid = list[idx];
            // Issue #1689: drop edge and shift higher indices before erase.
            if (!incoming_parent_index_dirty_) {
                if (cid != NULL_NODE)
                    incoming_index_remove_edge(cid, id, idx);
                if (idx + 1 < list.size())
                    incoming_index_shift_parent_indices(id, idx + 1, /*delta=*/-1);
            }
            if (cid != NULL_NODE && cid < parent_.size())
                parent_[cid] = NULL_NODE;
            children_[id] = list.with_erase(idx);
            add_mutation_child_op(id, idx, cid, NULL_NODE, "structural-remove-child");
            structural_mutate_erase_total_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void set_child(NodeId id, std::uint32_t idx, NodeId child) pre(id < children_.size()) {
        // Issue #222: acquire the structural mutation guard. The
        // guard's dtor bumps generation_ + releases the lock.
        // Issue #1624: Contracts on public structural mutate entry.
        StructuralMutationGuard guard(this);
        set_child_locked(id, idx, child);
    }

    // Insert a child at position idx (0 = first, child_count = append)
    // Shifts all subsequent children and updates child_begin_ for later nodes.
    void insert_child(NodeId id, std::uint32_t idx, NodeId child) {
        // Issue #222: acquire the structural mutation guard.
        StructuralMutationGuard guard(this);
        insert_child_locked(id, idx, child);
    }

    // Remove a child at position idx by replacing with NULL_NODE
    void remove_child(NodeId id, std::uint32_t idx) {
        // Issue #222: acquire the structural mutation guard.
        StructuralMutationGuard guard(this);
        remove_child_locked(id, idx);
    }

    // ── Bulk ───────────────────────────────────────────────────

    // Issue #221: capture a snapshot of children_ for rollback
    // (#177 MutationCheckpoint integration). The returned vector
    // contains PCV copies that share the underlying storage with
    // children_ (PCV COW); the snapshot is O(1) per node.
    // Returns std::pmr::vector to match children_'s allocator.
    std::vector<PersistentChildVector<NodeId>> snapshot_children() const {
        return children_; // vector copy ctor; each PCV is shared_ptr copy
    }

    // Issue #221: restore children_ from a pre-captured snapshot.
    // The passed-in vector is moved (its shared_ptrs are now bound
    // to children_; back-references to the old PCVs in the
    // snapshot are released as the snapshot goes out of scope).
    //
    // Issue #1502: after reinstalling children_, rebuild parent_
    // from the restored child lists so children_/parent_ topology
    // cannot diverge when MutationRecord inverse ops partially
    // fail or when restore runs without a full inverse walk.
    void restore_children(std::vector<PersistentChildVector<NodeId>>&& snapshot) {
        // Issue #487: pad the snapshot up to children_'s current
        // size before the move. Without padding, if the in-flight
        // mutation added nodes (e.g. set-code inside a MutationBoundary
        // guard, or any primitive that grew children_ before the
        // rollback fires), the snapshot would be smaller than the
        // current children_. The subsequent move would shrink
        // children_, and any access to children_[id] for id >=
        // snapshot.size() (e.g. a destructor that walks every node)
        // would trigger std::vector::operator[]'s debug-mode
        // assertion. This was crashing test_issue_192 tests 3.1 +
        // 4.1 (the atomic-batch bad-op path).
        //
        // The padding is cheap (empty PCVs are zero-sized; the
        // per-node COW means a moved-empty PCV is a single
        // shared_ptr that doesn't allocate).
        // Issue #1299/#1300: nodes allocated during the failed mutation
        // (id >= pre-mutation size) become orphan "ghosts" if we only
        // pad children_ with empty PCVs. Free those slots so queries
        // and restamp_all_node_generations skip them.
        const std::size_t pre_size = snapshot.size();
        const std::size_t post_size = children_.size();
        if (snapshot.size() < children_.size()) {
            snapshot.resize(children_.size());
        }
        children_ = std::move(snapshot);
        if (post_size > pre_size)
            free_orphan_nodes_from(static_cast<NodeId>(pre_size));
        // Issue #1281: every PCV topology restore is a fidelity event.
        children_topology_restore_count_.fetch_add(1, std::memory_order_relaxed);
        // Issue #1502: full parent_ topology restore from children_.
        rebuild_parent_links_from_children();
        bump_generation();
        // Issue #1282: if a wrap was observed mid-mutation, restamp now.
        maybe_auto_restamp_on_wrap();
    }

    // Issue #1502: capture parent_ SoA column for strong topology
    // rollback (pairs with snapshot_children). O(n) vector copy.
    std::pmr::vector<NodeId> snapshot_parent() const { return parent_; }

    // Issue #1502: restore parent_ from a pre-captured snapshot.
    // Pads if the live flat grew during a failed mutation (same
    // ghost-node safety as restore_children). Prefer
    // rebuild_parent_links_from_children() after restore_children
    // for fidelity; this path supports dual restore (snapshot +
    // rebuild) and fine-grained checkpoints.
    void restore_parent(std::pmr::vector<NodeId>&& snapshot) {
        if (snapshot.size() < parent_.size())
            snapshot.resize(parent_.size(), NULL_NODE);
        parent_ = std::move(snapshot);
        parent_topology_restore_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #1502: recompute parent_[cid] = id for every live
    // children_[id] entry. Clears parent_ first so detached /
    // reattached nodes cannot keep a stale back-link after
    // children_ restore without a matching inverse MutationRecord.
    // O(nodes + edges). Safe under structural / workspace lock.
    void rebuild_parent_links_from_children() {
        if (parent_.size() < size())
            parent_.resize(size(), NULL_NODE);
        for (NodeId id = 0; id < parent_.size(); ++id)
            parent_[id] = NULL_NODE;
        const std::size_t n = std::min(children_.size(), parent_.size());
        for (NodeId id = 0; id < static_cast<NodeId>(n); ++id) {
            for (NodeId cid : children_[id]) {
                if (cid != NULL_NODE && cid < parent_.size())
                    parent_[cid] = id;
            }
        }
        parent_topology_restore_count_.fetch_add(1, std::memory_order_relaxed);
        // Issue #1689: parent_ rebuild is bulk — refresh inverted index too.
        rebuild_incoming_parent_index();
    }

    // Issue #1299/#1300: mark nodes [begin, size()) as free (node_gen_=0)
    // and push onto free_list_. Used after rollback when mutations added
    // nodes that are no longer reachable after children_ restore.
    std::size_t free_orphan_nodes_from(NodeId begin) {
        std::size_t freed = 0;
        for (NodeId id = begin; id < size(); ++id) {
            if (id >= node_gen_.size())
                break;
            if (node_gen_[id] == 0)
                continue; // already free
            node_gen_[id] = 0;
            free_list_.push_back(id);
            ++freed;
        }
        if (freed > 0) {
            node_recycle_total_.fetch_add(freed, std::memory_order_relaxed);
            ghost_orphan_nodes_freed_.fetch_add(freed, std::memory_order_relaxed);
        }
        return freed;
    }

    [[nodiscard]] std::uint64_t ghost_orphan_nodes_freed() const noexcept {
        return ghost_orphan_nodes_freed_.load(std::memory_order_relaxed);
    }

    // Issue #266: capture / restore sym_id_ for fine-grained rollback
    // of bulk rename operations (mutate:rename-symbol).
    std::pmr::vector<SymId> snapshot_sym_id() const { return sym_id_; }
    void restore_sym_id(std::pmr::vector<SymId>&& snapshot) {
        sym_id_ = std::move(snapshot);
        bump_generation();
    }

    // Issue #266: Lambda param columns used by rename-symbol.
    struct ParamColumnsSnapshot {
        std::pmr::vector<SymId> param_data;
        std::pmr::vector<std::uint32_t> param_begin;
        std::pmr::vector<std::uint32_t> param_count;
    };
    ParamColumnsSnapshot snapshot_param_columns() const {
        return {param_data_, param_begin_, param_count_};
    }
    void restore_param_columns(ParamColumnsSnapshot&& snapshot) {
        param_data_ = std::move(snapshot.param_data);
        param_begin_ = std::move(snapshot.param_begin);
        param_count_ = std::move(snapshot.param_count);
        bump_generation();
    }

    // Issue #1893: hygiene + dirty metadata SoA snapshot for atomic-batch
    // rollback without a full FlatAST deep-copy (:snapshot? #t path).
    // Captures marker_ / provenance_ / dirty_ / macro_dirty_ so
    // MacroIntroduced + expansion provenance survive partial-fail rollback.
    struct MetadataColumnsSnapshot {
        std::pmr::vector<SyntaxMarker> marker;
        std::pmr::vector<std::uint32_t> provenance;
        std::pmr::vector<std::uint8_t> dirty;
        std::pmr::vector<std::uint8_t> macro_dirty;
    };
    [[nodiscard]] MetadataColumnsSnapshot snapshot_metadata_columns() const {
        return {marker_, provenance_, dirty_, macro_dirty_};
    }
    void restore_metadata_columns(MetadataColumnsSnapshot&& snapshot) noexcept {
        marker_ = std::move(snapshot.marker);
        provenance_ = std::move(snapshot.provenance);
        dirty_ = std::move(snapshot.dirty);
        macro_dirty_ = std::move(snapshot.macro_dirty);
    }

    // ── Issue #261: NodeId lifecycle / SoA compaction ────────────
    //
    // recycle_dead_nodes() marks unreachable slots (not reachable
    // from root via children, plus region_by_lambda_id_ roots) as
    // free (node_gen_=0) and pushes them onto free_list_ for reuse
    // by add_node(). Does NOT shrink the SoA columns — use
    // compact_nodes() for densification + cache locality.
    //
    // compact_nodes() rebuilds dense 0..live-1 SoA columns,
    // remaps all NodeId references, clears free_list_, and bumps
    // generation_ (invalidates all StableNodeRefs).
    [[nodiscard]] std::size_t recycle_dead_nodes() {
        auto live = mark_live_nodes();
        std::size_t recycled = 0;
        for (NodeId id = 0; id < size(); ++id) {
            if (is_free_slot(id))
                continue;
            if (!live[id]) {
                node_gen_[id] = 0;
                free_list_.push_back(id);
                ++recycled;
            }
        }
        if (recycled > 0)
            node_recycle_total_.fetch_add(recycled, std::memory_order_relaxed);
        return recycled;
    }

    // Issue #497: recycle dead slots without full SoA remap or
    // generation bump (avoids invalidating all StableNodeRefs).
    [[nodiscard]] std::size_t compact_nodes_soft() {
        const auto recycled = recycle_dead_nodes();
        if (recycled > 0)
            soft_compact_count_.fetch_add(1, std::memory_order_relaxed);
        return recycled;
    }

    [[nodiscard]] std::size_t compact_nodes() {
        auto live = mark_live_nodes();
        std::size_t live_count = 0;
        for (bool is_live : live) {
            if (is_live)
                ++live_count;
        }
        const auto old_size = size();
        if (live_count == 0) {
            clear();
            if (old_size > 0)
                node_compact_total_.fetch_add(old_size, std::memory_order_relaxed);
            return old_size;
        }
        if (live_count == old_size)
            return 0;

        std::vector<NodeId> old_to_new(old_size, NULL_NODE);
        NodeId next = 0;
        for (NodeId id = 0; id < old_size; ++id) {
            if (live[id])
                old_to_new[id] = next++;
        }

        auto remap = [&](NodeId id) -> NodeId {
            if (id == NULL_NODE || id >= old_to_new.size())
                return NULL_NODE;
            return old_to_new[id];
        };

        StructuralMutationGuard guard(this);

        auto alloc = tag_.get_allocator();
        std::pmr::vector<NodeTag> new_tag(alloc);
        std::pmr::vector<std::int64_t> new_int_val(alloc);
        std::pmr::vector<double> new_float_val(alloc);
        std::pmr::vector<SymId> new_sym_id(alloc);
        std::vector<PersistentChildVector<NodeId>> new_children;
        std::pmr::vector<NodeId> new_parent(alloc);
        std::pmr::vector<std::uint32_t> new_param_begin(alloc);
        std::pmr::vector<std::uint32_t> new_param_count(alloc);
        std::pmr::vector<std::uint32_t> new_cap_require_count(alloc);
        std::pmr::vector<std::uint32_t> new_line(alloc);
        std::pmr::vector<std::uint32_t> new_col(alloc);
        std::pmr::vector<SyntaxMarker> new_marker(alloc);
        std::pmr::vector<std::uint8_t> new_dirty(alloc);
        std::pmr::vector<std::uint8_t> new_ppa_dirty(alloc);
        // Issue #437: COW scratch for verify_dirty_
        std::pmr::vector<std::uint8_t> new_verify_dirty(alloc);
        // Issue #469: COW scratch for verification_dirty_
        std::pmr::vector<std::uint8_t> new_verification_dirty(alloc);
        // Issue #290: COW scratch for macro_dirty_
        std::pmr::vector<std::uint8_t> new_macro_dirty(alloc);
        std::pmr::vector<std::uint32_t> new_type_id(alloc);
        // Issue #412: COW scratch for type_cache_gen_.
        std::pmr::vector<std::uint32_t> new_type_cache_gen(alloc);
        // Issue #412 follow-up #1: COW scratch for
        // per-binding cache gen.
        std::pmr::vector<std::uint32_t> new_type_cache_binding_gen(alloc);
        std::pmr::vector<std::uint8_t> new_error_kind(alloc);
        std::pmr::vector<std::uint32_t> new_node_first_mutation(alloc);
        std::pmr::vector<std::uint16_t> new_node_gen(alloc);
        std::pmr::vector<std::int64_t> new_value_cache(alloc);

        new_tag.reserve(live_count);
        new_int_val.reserve(live_count);
        new_float_val.reserve(live_count);
        new_sym_id.reserve(live_count);
        new_children.reserve(live_count);
        new_parent.reserve(live_count);
        new_param_begin.reserve(live_count);
        new_param_count.reserve(live_count);
        new_cap_require_count.reserve(live_count);
        new_line.reserve(live_count);
        new_col.reserve(live_count);
        new_marker.reserve(live_count);
        new_dirty.reserve(live_count);
        new_ppa_dirty.reserve(live_count);
        new_type_id.reserve(live_count);
        new_type_cache_gen.reserve(live_count);
        new_type_cache_binding_gen.reserve(live_count);
        new_error_kind.reserve(live_count);
        new_node_first_mutation.reserve(live_count);
        new_node_gen.reserve(live_count);
        new_value_cache.reserve(live_count);

        for (NodeId id = 0; id < old_size; ++id) {
            if (!live[id])
                continue;
            new_tag.push_back(tag_[id]);
            new_int_val.push_back(int_val_[id]);
            new_float_val.push_back(float_val_[id]);
            new_sym_id.push_back(sym_id_[id]);
            std::vector<NodeId> remapped_children;
            remapped_children.reserve(children_[id].size());
            for (auto cid : children_[id]) {
                if (cid != NULL_NODE)
                    remapped_children.push_back(remap(cid));
            }
            new_children.emplace_back(remapped_children.begin(), remapped_children.end());
            new_parent.push_back(remap(parent_[id]));
            new_param_begin.push_back(param_begin_[id]);
            new_param_count.push_back(param_count_[id]);
            new_cap_require_count.push_back(cap_require_count_[id]);
            new_line.push_back(line_[id]);
            new_col.push_back(col_[id]);
            new_marker.push_back(marker_[id]);
            new_dirty.push_back(dirty_[id]);
            if (id < ppa_dirty_.size())
                new_ppa_dirty.push_back(ppa_dirty_[id]);
            else
                new_ppa_dirty.push_back(0);
            // Issue #437: COW the verify_dirty_ bitmask.
            if (id < verify_dirty_.size())
                new_verify_dirty.push_back(verify_dirty_[id]);
            else
                new_verify_dirty.push_back(0);
            // Issue #469: COW the verification_dirty_ bitmask.
            if (id < verification_dirty_.size())
                new_verification_dirty.push_back(verification_dirty_[id]);
            else
                new_verification_dirty.push_back(0);
            // Issue #290: COW the macro_dirty_ bitmask.
            if (id < macro_dirty_.size())
                new_macro_dirty.push_back(macro_dirty_[id]);
            else
                new_macro_dirty.push_back(0);
            new_type_id.push_back(type_id_[id]);
            // Issue #412: parallel cache gen. After COW we
            // reset to 0 so the next type-checker pass will
            // re-populate both fields via
            // set_type_with_gen().
            new_type_cache_gen.push_back(0);
            // Issue #412 follow-up #1: parallel
            // per-binding cache gen. Also reset to 0
            // after COW; the next type-checker pass will
            // re-populate via set_type_with_binding_gen().
            new_type_cache_binding_gen.push_back(0);
            new_error_kind.push_back(error_kind_[id]);
            new_node_first_mutation.push_back(node_first_mutation_[id]);
            new_node_gen.push_back(generation_);
            if (id < value_cache_.size())
                new_value_cache.push_back(value_cache_[id]);
            else
                new_value_cache.push_back(kNotCached);
        }

        tag_ = std::move(new_tag);
        int_val_ = std::move(new_int_val);
        float_val_ = std::move(new_float_val);
        sym_id_ = std::move(new_sym_id);
        children_ = std::move(new_children);
        parent_ = std::move(new_parent);
        param_begin_ = std::move(new_param_begin);
        param_count_ = std::move(new_param_count);
        cap_require_count_ = std::move(new_cap_require_count);
        line_ = std::move(new_line);
        col_ = std::move(new_col);
        marker_ = std::move(new_marker);
        dirty_ = std::move(new_dirty);
        ppa_dirty_ = std::move(new_ppa_dirty);
        // Issue #437: COW the verify_dirty_ column
        verify_dirty_ = std::move(new_verify_dirty);
        // Issue #469: COW the verification_dirty_ column
        verification_dirty_ = std::move(new_verification_dirty);
        // Issue #290: COW the macro_dirty_ column
        macro_dirty_ = std::move(new_macro_dirty);
        type_id_ = std::move(new_type_id);
        type_cache_gen_ = std::move(new_type_cache_gen);
        // Issue #412 follow-up #1: COW the
        // per-binding cache gen column. The
        // binding_gens_ map is NOT COW'd — the new flat
        // starts with an empty map and only entries
        // that the clone's mutations touch are added.
        // This ensures mutations on the clone don't
        // invalidate the parent's cache.
        type_cache_binding_gen_ = std::move(new_type_cache_binding_gen);
        // Issue #412 follow-up #1: COW the
        // binding_gens_ map. The clone gets a fresh
        // empty map (the COW contract is that mutations
        // on the clone don't affect the parent). The
        // existing entries will be re-built lazily as
        // the clone's mutations bump them.
        binding_gens_ = std::make_shared<BindingGenMap>();
        error_kind_ = std::move(new_error_kind);
        node_first_mutation_ = std::move(new_node_first_mutation);
        node_gen_ = std::move(new_node_gen);
        value_cache_ = std::move(new_value_cache);
        // Issue #490: proactive rebuild after compact remap so
        // query:tag-arity-count / find_by_tag_arity avoid a
        // lazy O(n) spike on the next query call.
        rebuild_tag_arity_index();

        root = remap(root);

        std::pmr::unordered_map<NodeId, std::uint8_t> new_region_by_lambda(alloc);
        for (const auto& [lid, region] : region_by_lambda_id_) {
            auto nlid = remap(lid);
            if (nlid != NULL_NODE)
                new_region_by_lambda[nlid] = region;
        }
        region_by_lambda_id_ = std::move(new_region_by_lambda);

        std::vector<MatchClauseInfo> new_match_info(live_count);
        for (NodeId id = 0; id < old_size; ++id) {
            if (!live[id])
                continue;
            auto new_id = old_to_new[id];
            if (has_match_info(id))
                new_match_info[new_id] = match_info_[id];
        }
        match_info_ = std::move(new_match_info);

        for (auto& rec : mutation_log_) {
            rec.target_node = remap(rec.target_node);
            rec.parent_id = remap(rec.parent_id);
        }

        free_list_.clear();

        const auto reclaimed = old_size - live_count;
        node_compact_total_.fetch_add(reclaimed, std::memory_order_relaxed);
        return reclaimed;
    }

    [[nodiscard]] NodeLifecycleStats node_lifecycle_stats() const noexcept {
        NodeLifecycleStats stats;
        stats.total_slots = size();
        stats.free_slots = free_list_.size();
        auto live = mark_live_nodes();
        for (bool is_live : live) {
            if (is_live)
                ++stats.live_nodes;
        }
        if (stats.total_slots > 0) {
            const auto dead = stats.total_slots - stats.live_nodes;
            stats.fragmentation_ratio =
                static_cast<double>(dead) / static_cast<double>(stats.total_slots);
        }
        return stats;
    }

    std::uint64_t node_recycle_total() const noexcept {
        return node_recycle_total_.load(std::memory_order_relaxed);
    }
    std::uint64_t node_slot_reuse_count() const noexcept {
        return node_slot_reuse_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t node_compact_total() const noexcept {
        return node_compact_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t soft_compact_count() const noexcept {
        return soft_compact_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t stale_ref_auto_refresh_count() const noexcept {
        return stale_ref_auto_refresh_count_.load(std::memory_order_relaxed);
    }
    void record_stale_ref_auto_refresh() noexcept {
        stale_ref_auto_refresh_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1346: lock-free StableNodeRef validate path counter.
    mutable std::atomic<std::uint64_t> lockfree_stable_ref_validate_count_{0};
    void record_lockfree_stable_ref_validate() noexcept {
        lockfree_stable_ref_validate_count_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t lockfree_stable_ref_validate_count() const noexcept {
        return lockfree_stable_ref_validate_count_.load(std::memory_order_relaxed);
    }
    // Issue #1345 / #1346 observability accessors.
    [[nodiscard]] std::uint64_t mark_dirty_boundary_prune_count() const noexcept {
        return mark_dirty_boundary_prune_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t auto_compact_on_commit_count() const noexcept {
        return auto_compact_on_commit_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t live_nodes_threshold_warn_count() const noexcept {
        return live_nodes_threshold_warn_count_.load(std::memory_order_relaxed);
    }
    void set_compaction_free_list_threshold(std::size_t n) noexcept {
        compaction_free_list_threshold_ = n == 0 ? 1 : n;
    }
    void set_max_live_nodes_warn(std::size_t n) noexcept { max_live_nodes_warn_ = n == 0 ? 1 : n; }
    [[nodiscard]] std::size_t compaction_free_list_threshold() const noexcept {
        return compaction_free_list_threshold_;
    }
    [[nodiscard]] std::size_t max_live_nodes_warn() const noexcept { return max_live_nodes_warn_; }

    void clear() {
        tag_.clear();
        int_val_.clear();
        float_val_.clear();
        sym_id_.clear();
        children_.clear();
        parent_.clear();
        param_begin_.clear();
        param_count_.clear();
        cap_require_count_.clear();
        param_data_.clear();
        param_annot_data_.clear();
        line_.clear();
        col_.clear();
        marker_.clear();
        // Issue #402: clear summary flags alongside the
        // per-node columns. The next add_node will rebuild
        // the bit-set from scratch.
        summary_flags_ = 0;
        dirty_.clear();
        ppa_dirty_.clear();
        // Issue #437: clear verify_dirty_ alongside ppa_dirty_
        verify_dirty_.clear();
        // Issue #469: clear verification_dirty_ alongside the
        // other dirty columns. Populated by
        // apply_verification_dirty_bits (from
        // (verify:parse-coverage-feedback) /
        // (verify:parse-assert-failure)).
        verification_dirty_.clear();
        // Issue #290: clear macro_dirty_ alongside the other
        // dirty columns. Populated by
        // apply_macro_dirty_bits (from clone_macro_body and
        // self-evolution).
        macro_dirty_.clear();
        // Issue #447: clear the tag+arity index on full
        // reset. The next query:pattern call will rebuild
        // it lazily.
        tag_arity_index_.clear();
        tag_arity_index_built_size_ = 0;
        type_id_.clear();
        mutation_log_.clear();
        // Issue #282: clear narrowing provenance on FlatAST reset.
        narrowing_log_.clear();
        node_first_mutation_.clear();
        node_gen_.clear();
        free_list_.clear();
        next_mutation_id_ = 1;
        generation_ = 1;
        match_info_.clear();
        root = NULL_NODE;
    }

    std::size_t size() const { return tag_.size(); }
    bool empty() const { return tag_.empty(); }

    // Issue #402: walk only the root subtree (iterative
    // DFS, bounded by max_nodes as a safety net). Returns
    // the number of nodes visited. The visitor is called
    // for every node in the subtree, in pre-order.
    //
    // Why subtree-only: needs_tree_walker_fallback was
    // iterating over the ENTIRE flat (all historical
    // defines, macros, etc.) on every eval. For typical
    // expressions like `(+ 1 2)`, the root subtree has 4
    // nodes vs flat.size() of 100+ — a 25x+ reduction in
    // loop iterations per eval. Historical nodes that
    // contain MacroDef / DefineType / etc. don't affect
    // the current eval's fallback decision; only the
    // reachable-from-root subgraph does.
    template <typename Visitor>
    std::size_t walk_subtree(NodeId root, Visitor&& visit, std::size_t max_nodes = 1024) const {
        if (root == NULL_NODE || root >= tag_.size())
            return 0;
        std::size_t visited = 0;
        // Iterative DFS using an explicit stack (avoids
        // recursion blow-up for deep ASTs).
        std::vector<NodeId> stack;
        stack.push_back(root);
        while (!stack.empty() && visited < max_nodes) {
            auto id = stack.back();
            stack.pop_back();
            if (id == NULL_NODE || id >= tag_.size())
                continue;
            if (is_free_slot(id))
                continue;
            visit(id);
            ++visited;
            auto v = get(id);
            for (auto c : v.children)
                stack.push_back(c);
        }
        return visited;
    }

    // ── Issue #217 Cycle 14 (P2): FlatAST SoA custom serialize ─
    //
    // The production FlatAST has 23 PRIVATE SoA columns
    // (tag_/int_val_/float_val_/sym_id_/child_begin_/
    // child_count_/child_data_/parent_/param_begin_/
    // param_count_/cap_require_count_/param_data_/
    // param_annot_data_/line_/col_/marker_/dirty_/type_id_/
    // error_kind_/value_cache_/node_first_mutation_/
    // node_gen_). The generic reflect_members<T>() can't
    // see private members, so the generic auto_serialize
    // path doesn't work. These custom methods iterate the
    // SoA columns directly.
    //
    // Issue #220: the children_ field is a per-node
    // std::pmr::vector<NodeId>, so the wire format replaces
    // the 3 legacy child_* columns (child_begin_ + child_count_ +
    // child_data_) with 2 new columns (per-node count + flat
    // children). This is the same column count (22 → 22) but
    // the structure is different. Old v1 cache files won't
    // roundtrip with the new format.
    //
    // Wire format (matches tests/test_issue_217.cpp Test 18/19):
    //   [u32 format_version = 2]  (v1 still readable)
    //   [u32 num_nodes]
    //   For each of 22 SoA columns (fixed order, see below):
    //     [u32 count]
    //     [count * sizeof(elem) raw bytes]
    //   [u32 next_mutation_id_ (low 32 bits)]
    //   [u16 generation_]
    //   [u16 reserved]
    //
    // The 22 SoA columns in order:
    //   1. tag_           (u32 = NodeTag)
    //   2. int_val_       (i64)
    //   3. float_val_     (f64)
    //   4. sym_id_        (u32 = SymId)
    //   5. child_count_per_node_ (u32 per node)  ← NEW (#220)
    //   6. child_data     (u32 = NodeId, flat concatenation)  ← NEW (#220)
    //   7. parent_        (u32 = NodeId)
    //   8. param_begin_   (u32)
    //   9. param_count_   (u32)
    //  10. cap_require_count_ (u32)
    //  11. param_data_    (u32 = SymId)
    //  12. param_annot_data_ (u32 = NodeId)
    //  13. line_         (u32)
    //  14. col_          (u32)
    //  15. marker_        (u8 = SyntaxMarker)
    //  16. dirty_         (u8)
    //  17. type_id_       (u32)
    //  18. error_kind_    (u8)
    //  19. value_cache_   (i64)
    //  20. node_first_mutation_ (u32)
    //  21. node_gen_      (u16)
    //
    // The legacy 3 columns (child_begin_ + child_count_ +
    // child_data_ as a flat child array) are replaced by
    // child_count_per_node_ + child_data.
    //
    // v2 additions (Issue #269 — hand-inlined; no reflect.hh in module):
    //   - mutation_log_ (vector<MutationRecord>)
    //   - match_info_ (vector<MatchClauseInfo>, 3 wire fields)
    //   - region_by_sym_ / region_by_lambda_id_ (manual map)
    //   - root (NodeId scalar)

    static void wire_write_string(std::vector<char>& buf, std::string_view s) {
        std::uint32_t len = static_cast<std::uint32_t>(s.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&len), reinterpret_cast<char*>(&len) + 4);
        buf.insert(buf.end(), s.begin(), s.end());
    }

    static void wire_write_vec_u32(std::vector<char>& buf, const std::vector<SymId>& v) {
        std::uint32_t sz = static_cast<std::uint32_t>(v.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&sz), reinterpret_cast<char*>(&sz) + 4);
        if (!v.empty()) {
            buf.insert(buf.end(), reinterpret_cast<const char*>(v.data()),
                       reinterpret_cast<const char*>(v.data()) + v.size() * sizeof(SymId));
        }
    }

    static void wire_write_match_clause_info(std::vector<char>& buf, const MatchClauseInfo& m) {
        wire_write_vec_u32(buf, m.used_constructors);
        wire_write_vec_u32(buf, m.candidate_constructors);
        buf.push_back(m.has_wildcard ? '\1' : '\0');
    }

    static void
    wire_write_map_u32_u8(std::vector<char>& buf,
                          const std::pmr::unordered_map<std::uint32_t, std::uint8_t>& m) {
        std::uint32_t count = static_cast<std::uint32_t>(m.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&count), reinterpret_cast<char*>(&count) + 4);
        for (const auto& [k, v] : m) {
            buf.insert(buf.end(), reinterpret_cast<const char*>(&k),
                       reinterpret_cast<const char*>(&k) + 4);
            buf.insert(buf.end(), reinterpret_cast<const char*>(&v),
                       reinterpret_cast<const char*>(&v) + 1);
        }
    }

    static std::string wire_read_string(const std::vector<char>& buf, std::size_t& pos) {
        std::uint32_t len;
        std::memcpy(&len, &buf[pos], 4);
        pos += 4;
        std::string s(buf.data() + pos, buf.data() + pos + len);
        pos += len;
        return s;
    }

    static std::vector<SymId> wire_read_vec_u32(const std::vector<char>& buf, std::size_t& pos) {
        std::uint32_t sz;
        std::memcpy(&sz, &buf[pos], 4);
        pos += 4;
        std::vector<SymId> v(sz);
        if (sz > 0) {
            std::memcpy(v.data(), &buf[pos], sz * sizeof(SymId));
            pos += sz * sizeof(SymId);
        }
        return v;
    }

    static MatchClauseInfo wire_read_match_clause_info(const std::vector<char>& buf,
                                                       std::size_t& pos) {
        MatchClauseInfo m;
        m.used_constructors = wire_read_vec_u32(buf, pos);
        m.candidate_constructors = wire_read_vec_u32(buf, pos);
        m.has_wildcard = buf[pos++] != 0;
        return m;
    }

    static void wire_read_map_u32_u8(const std::vector<char>& buf, std::size_t& pos,
                                     std::pmr::unordered_map<std::uint32_t, std::uint8_t>& m) {
        std::uint32_t count;
        std::memcpy(&count, &buf[pos], 4);
        pos += 4;
        m.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t k;
            std::uint8_t v;
            std::memcpy(&k, &buf[pos], 4);
            pos += 4;
            std::memcpy(&v, &buf[pos], 1);
            pos += 1;
            m[k] = v;
        }
    }

    void serialize_soa(std::vector<char>& buf) const {
        // Format version (v2 includes side-data fields)
        std::uint32_t version = 2;
        buf.insert(buf.end(), reinterpret_cast<char*>(&version),
                   reinterpret_cast<char*>(&version) + 4);
        // Num nodes (informational; per-node columns derive their size)
        std::uint32_t num_nodes = static_cast<std::uint32_t>(tag_.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&num_nodes),
                   reinterpret_cast<char*>(&num_nodes) + 4);

        // Helper: serialize a pmr::vector<T> as count + raw bytes
        auto write_column = [&buf](const auto& col) {
            std::uint32_t count = static_cast<std::uint32_t>(col.size());
            buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                       reinterpret_cast<char*>(&count) + 4);
            if (!col.empty()) {
                buf.insert(
                    buf.end(), reinterpret_cast<const char*>(col.data()),
                    reinterpret_cast<const char*>(col.data()) +
                        col.size() *
                            sizeof(
                                typename std::remove_reference<decltype(col)>::type::value_type));
            }
        };
        // 19 SoA columns + 2 children columns (per-node count +
        // flat children) = 21 columns total. The legacy
        // child_begin_/child_count_/child_data_ are gone (see
        // children_ field which is the new source of truth).
        write_column(tag_);
        write_column(int_val_);
        write_column(float_val_);
        write_column(sym_id_);
        // Issue #220: write the per-node children as two
        // columns. (1) per-node count, (2) flat concatenation
        // of all children. The reader reconstructs children_
        // from these.
        {
            std::vector<std::uint32_t> child_counts(num_nodes);
            std::uint32_t total_children = 0;
            for (NodeId i = 0; i < num_nodes; ++i) {
                child_counts[i] = static_cast<std::uint32_t>(children_[i].size());
                total_children += child_counts[i];
            }
            write_column(child_counts);
            std::vector<NodeId> flat_children;
            flat_children.reserve(total_children);
            for (NodeId i = 0; i < num_nodes; ++i) {
                flat_children.insert(flat_children.end(), children_[i].begin(), children_[i].end());
            }
            write_column(flat_children);
        }
        write_column(parent_);
        write_column(param_begin_);
        write_column(param_count_);
        write_column(cap_require_count_);
        write_column(param_data_);
        write_column(param_annot_data_);
        write_column(line_);
        write_column(col_);
        write_column(marker_);
        write_column(dirty_);
        // Issue #437: serialize verify_dirty_ alongside ppa_dirty_
        // (the serializer writes the columns in lockstep, so we
        // need to add it here to keep binary compat). Insert
        // before type_id_ to match the read order.
        write_column(verify_dirty_);
        write_column(type_id_);
        write_column(error_kind_);
        write_column(value_cache_);
        write_column(node_first_mutation_);
        write_column(node_gen_);
        // Scalars
        buf.insert(buf.end(), reinterpret_cast<const char*>(&next_mutation_id_),
                   reinterpret_cast<const char*>(&next_mutation_id_) + 4);
        buf.insert(buf.end(), reinterpret_cast<const char*>(&generation_),
                   reinterpret_cast<const char*>(&generation_) + 2);
        // 2 bytes padding (v1 compat; v2 side-data follows)
        std::uint16_t reserved = 0;
        buf.insert(buf.end(), reinterpret_cast<const char*>(&reserved),
                   reinterpret_cast<const char*>(&reserved) + 2);

        // v2 side-data (Issue #269)
        {
            std::uint32_t log_count = static_cast<std::uint32_t>(mutation_log_.size());
            buf.insert(buf.end(), reinterpret_cast<char*>(&log_count),
                       reinterpret_cast<char*>(&log_count) + 4);
            for (const auto& rec : mutation_log_)
                mutation::wire_write_mutation_record(buf, rec);
        }
        {
            std::uint32_t mi_count = static_cast<std::uint32_t>(match_info_.size());
            buf.insert(buf.end(), reinterpret_cast<char*>(&mi_count),
                       reinterpret_cast<char*>(&mi_count) + 4);
            for (const auto& mi : match_info_)
                wire_write_match_clause_info(buf, mi);
        }
        wire_write_map_u32_u8(buf, region_by_sym_);
        wire_write_map_u32_u8(buf, region_by_lambda_id_);
        buf.insert(buf.end(), reinterpret_cast<const char*>(&root),
                   reinterpret_cast<const char*>(&root) + 4);
        // Issue #277: optional v2 extension — PPA dirty column.
        write_column(ppa_dirty_);
        // Issue #290: optional v2 extension — macro_dirty_
        // column. Appended after ppa_dirty_ so older readers
        // (which stop at the EOF after ppa_dirty_) skip it
        // gracefully (read_column checks size).
        write_column(macro_dirty_);
    }

    // Static (no instance needed). Returns a freshly-constructed
    // FlatAST populated from the wire format. The FlatAST uses
    // a default std::pmr::polymorphic_allocator (no arena) — if
    // the caller needs arena-backed deserialization, they should
    // pass an allocator to the FlatAST constructor after the
    // call.
    //
    // v1 omits side-data (mutation_log_, match_info_, region_by_*,
    // root stay default). v2 includes all five fields.
    static FlatAST deserialize_soa(const std::vector<char>& buf, std::size_t& pos) {
        FlatAST ast;
        std::uint32_t version;
        std::memcpy(&version, &buf[pos], 4);
        pos += 4;
        if (version != 1 && version != 2) {
            pos = buf.size();
            return ast;
        }
        const bool is_v2 = (version == 2);
        std::uint32_t num_nodes;
        std::memcpy(&num_nodes, &buf[pos], 4);
        pos += 4;

        auto read_column = [&buf, &pos](auto& col) {
            using T = typename std::remove_reference<decltype(col)>::type::value_type;
            std::uint32_t count;
            std::memcpy(&count, &buf[pos], 4);
            pos += 4;
            col.resize(count);
            // Issue #219: GapBuffer's `data()` is not contiguous
            // when the gap is in the middle. compact() moves the
            // gap to the end so data() returns a contiguous
            // pointer. For pmr::vector this is a no-op (the
            // compiler optimizes it away for trivial types).
            if constexpr (requires { col.compact(); }) {
                col.compact();
            }
            if (count > 0) {
                std::memcpy(col.data(), &buf[pos], count * sizeof(T));
                pos += count * sizeof(T);
            }
        };
        read_column(ast.tag_);
        read_column(ast.int_val_);
        read_column(ast.float_val_);
        read_column(ast.sym_id_);
        // Issue #220: read the per-node children columns and
        // populate ast.children_ from them. The legacy
        // child_begin_/child_count_/child_data_ columns are
        // gone (the children_ field is the new source of
        // truth, populated by all add_X methods).
        {
            std::vector<std::uint32_t> child_counts;
            read_column(child_counts);
            std::vector<NodeId> flat_children;
            read_column(flat_children);
            ast.children_.resize(num_nodes);
            std::size_t offset = 0;
            for (NodeId i = 0; i < num_nodes; ++i) {
                auto count = child_counts[i];
                // Issue #221: build each per-node PCV from the
                // flat column via the range constructor.
                ast.children_[i] = PersistentChildVector<NodeId>(
                    flat_children.begin() + offset, flat_children.begin() + offset + count);
                offset += count;
            }
        }
        read_column(ast.parent_);
        read_column(ast.param_begin_);
        read_column(ast.param_count_);
        read_column(ast.cap_require_count_);
        read_column(ast.param_data_);
        read_column(ast.param_annot_data_);
        read_column(ast.line_);
        read_column(ast.col_);
        read_column(ast.marker_);
        read_column(ast.dirty_);
        // Issue #437: verify_dirty_ column (u8 per node), written by
        // serialize_soa since #437. Hand-built v1 fixtures must
        // include it between dirty_ and type_id_.
        read_column(ast.verify_dirty_);
        read_column(ast.type_id_);
        read_column(ast.error_kind_);
        read_column(ast.value_cache_);
        read_column(ast.node_first_mutation_);
        read_column(ast.node_gen_);
        std::memcpy(&ast.next_mutation_id_, &buf[pos], 4);
        pos += 4;
        std::memcpy(&ast.generation_, &buf[pos], 2);
        pos += 2;
        pos += 2; // reserved

        if (is_v2) {
            std::uint32_t log_count;
            std::memcpy(&log_count, &buf[pos], 4);
            pos += 4;
            ast.mutation_log_.clear();
            ast.mutation_log_.reserve(log_count);
            for (std::uint32_t i = 0; i < log_count; ++i)
                ast.mutation_log_.push_back(mutation::wire_read_mutation_record(buf, pos));

            std::uint32_t mi_count;
            std::memcpy(&mi_count, &buf[pos], 4);
            pos += 4;
            ast.match_info_.resize(mi_count);
            for (std::uint32_t i = 0; i < mi_count; ++i)
                ast.match_info_[i] = wire_read_match_clause_info(buf, pos);

            wire_read_map_u32_u8(buf, pos, ast.region_by_sym_);
            wire_read_map_u32_u8(buf, pos, ast.region_by_lambda_id_);
            std::memcpy(&ast.root, &buf[pos], 4);
            pos += 4;
            // Issue #277: backward-compatible optional PPA dirty column.
            if (pos + 4 <= buf.size()) {
                read_column(ast.ppa_dirty_);
            }
            // Issue #290: backward-compatible optional
            // macro_dirty_ column. Appended after ppa_dirty_;
            // older serialized blobs (pre-#290) stop here, so
            // the guard `pos + 4 <= buf.size()` is sufficient.
            if (pos + 4 <= buf.size()) {
                read_column(ast.macro_dirty_);
            }
        }
        return ast;
    }

    // ── Marker access ─────────────────────────────────────────

    void set_marker(NodeId id, SyntaxMarker m)
        // Issue #144: markers are a hygiene signal used by
        // query:pattern and mutate:replace-subtree (Issue #140,
        // #142). A silent no-op on stale id would let a
        // macro-introduced node appear user-written.
        pre(id < marker_.size()) {
        marker_[id] = m;
    }
    SyntaxMarker marker(NodeId id) const {
        return id < marker_.size() ? marker_[id] : SyntaxMarker::User;
    }

    // Issue #397: centralized hygiene check. Returns true iff
    // the node at `id` was introduced by macro expansion (or by
    // a structural transformation that respects the hygiene
    // contract, such as mutate:extract-function). Out-of-bounds
    // ids default to false (the marker() accessor returns User
    // for those). Used by query:pattern + mutate:replace-subtree
    // (and other primitives that need to distinguish user-written
    // from macro-introduced code).
    //
    // Hygiene contract:
    //   - MacroIntroduced nodes are query-visible (they appear
    //     in (query:defines), (query:children), etc.) so the user
    //     can introspect the expanded body.
    //   - MacroIntroduced nodes are mutation-protected by default
    //     (mutate:replace-subtree returns a hygiene error if you
    //     try to overwrite one). This is provenance tracking +
    //     hygiene, not full α-renaming at expansion time.
    [[nodiscard]] bool is_macro_introduced(NodeId id) const noexcept {
        return marker(id) == SyntaxMarker::MacroIntroduced;
    }

    // ── Dirty tracking (incremental compilation) ───────────────
    //
    // Issue #188: per-node dirty bitmask. The dirty_ column is
    // repurposed as a bitmask where each bit represents a different
    // reason for invalidation. This lets the type checker do
    // *targeted* re-analysis (only re-run occurrence-narrowing when
    // a predicate changed, only re-validate ownership when a Linear
    // binding changed, etc.) instead of the coarse "re-infer
    // everything dirty" pass.
    //
    // The old single-bit semantics are preserved: `is_dirty(id)` is
    // `true` iff ANY bit is set, so existing callers see no behavior
    // change. New callers can use `is_dirty_for(id, reason)` for
    // targeted re-analysis.

    // Issue #188: dirty-reason bits. Each can be OR'd together when
    // a single mutation triggers multiple concerns. Bit 0 (kGeneral)
    // is the backward-compatible "this node needs re-inference" bit.
    enum DirtyReason : std::uint8_t {
        kGeneralDirty = 0x01,    // node type must be re-inferred
        kConstraintDirty = 0x02, // constraints involving this var changed
        kOccurrenceDirty = 0x04, // occurrence-narrowing affected
        kOwnershipDirty = 0x08,  // Linear/Move/Borrow state changed
        kCoercionDirty = 0x10,   // deferred coercion needs re-apply
        // Issue #262: infra dirty bits for precise incremental paths.
        kStructDirty = 0x20,  // structural shape changed (children/parent)
        kDefUseDirty = 0x40,  // def-use / caller graph may be stale
        kPpaHintDirty = 0x80, // PPA-hint metadata needs refresh
    };

    // Issue #277: PPA-specific dirty bits stored in the orthogonal
    // ppa_dirty_ column (DirtyReason uint8_t is full). Setting any
    // PPA bit also ORs kPpaHintDirty on dirty_ for backward-compat
    // with dirty:counts "ppa-hint" aggregation.
    enum PpaDirtyReason : std::uint8_t {
        kTimingDirty = 0x01, // timing closure stale
        kPowerDirty = 0x02,  // power estimate stale
        kAreaDirty = 0x04,   // area estimate stale
        kBackendHint = 0x08, // Verilog/HW backend should re-emit
    };

    // Issue #277: OR PPA bits into ppa_dirty_ and mirror kPpaHintDirty
    // on dirty_ so legacy dirty:counts aggregation stays accurate.
    void apply_ppa_dirty_bits(NodeId id, std::uint8_t ppa_reasons) {
        if (ppa_reasons == 0)
            return;
        if (id >= ppa_dirty_.size())
            ppa_dirty_.resize(id + 1, 0);
        ppa_dirty_[id] |= ppa_reasons;
        mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty | kPpaHintDirty));
    }

    // Issue #437: verification-specific dirty bits stored in the
    // orthogonal verify_dirty_ column. Setting any verify bit also
    // ORs kGeneralDirty on dirty_ for backward-compat.
    enum VerifyDirtyReason : std::uint8_t {
        kAssertionDirty = 0x01,            // assertion failed
        kCoverageDirty = 0x02,             // coverage hole detected
        kSvaDirty = 0x04,                  // SVA property/sequence affected
        kFormalCounterexampleDirty = 0x08, // formal proof counterexample
    };

    // Issue #469: verification_dirty_ enum (separate from
    // VerifyDirtyReason so we can use 2 bits without
    // collision with the #437 / #455 / #458 bits).
    enum VerificationDirtyReason : std::uint8_t {
        kCoverageFeedbackDirty = 0x01, // coverage hole from external tool
        kAssertFailureDirty = 0x02,    // assertion failure from external tool
    };

    // Issue #437: OR verify bits into verify_dirty_ and mirror
    // kGeneralDirty on dirty_ so legacy is_dirty() callers still
    // see "this node needs work". The public definition is
    // in the public section below.
    // (No forward declaration needed; the public definition
    //  at line ~775 is reachable from the dirty_observability
    //  path via class-internal lookup.)

    // Issue #469: OR verification bits into
    // verification_dirty_ and mirror kGeneralDirty on dirty_.
    // Defined in the public section below alongside the
    // other Issue #437/469 accessors.

    // Issue #188: mark a node dirty for one or more specific reasons.
    // The `kGeneralDirty` bit is set automatically so existing
    // is_dirty() callers still see "this node needs work".
    //
    // Issue #399: pre-reserve capacity for all the per-node
    // "dirty" side-table columns (dirty_, ppa_dirty_,
    // verify_dirty_, verification_dirty_, macro_dirty_) so
    // that mark_dirty's resize() fallback is a no-op in the
    // hot path. The automatic reserve in add_node keeps
    // dirty_ in lockstep with tag_ for the normal growth
    // path; this public helper lets external code (workspace
    // bulk-loaders, snapshot/clone paths) reserve up-front
    // for known-large ASTs without paying the amortized
    // 2x reallocations.
    //
    // Idempotent: a second call with a smaller n is a no-op
    // (std::vector::reserve only grows). The reservation is
    // amortized O(1) across N add_node calls when called
    // once with the final size.
    void reserve_dirty(std::size_t n) noexcept {
        dirty_.reserve(n);
        ppa_dirty_.reserve(n);
        verify_dirty_.reserve(n);
        verification_dirty_.reserve(n);
        macro_dirty_.reserve(n);
    }

    // Issue #302: pre-Contract added so a NodeId out-of-bounds for
    // the current tag_ column is caught at the boundary instead of
    // silently growing dirty_ to ~4G (which would happen for
    // NULL_NODE = 0xFFFFFFFF on a 32-bit NodeId). Note: NULL_NODE
    // is 0 by default for small ASTs (it can be redefined), so the
    // contract catches genuine OOB like passing a stale NodeId from
    // a released child.
    void mark_dirty(NodeId id, std::uint8_t reasons = kGeneralDirty) pre(id < tag_.size()) {
        if (id >= dirty_.size())
            dirty_.resize(id + 1, 0);
        dirty_[id] |= reasons;
        // Issue #1519: post — requested reason bits are set after stamp.
        contract_assert(reasons == 0 || (dirty_[id] & reasons) == reasons);
        aura::core::cpp26::record_hotpath_invariant_hit();
        clear_cached_value(id); // invalidate result cache
        // Issue #1455: kOccurrenceDirty implies the occurrence-
        // stale column — keep the two signals in lockstep so
        // resolve_if_predicate_occurrence force-reanalyzes and
        // safe-falls back even when only mark_dirty (not the
        // set_occurrence_dirty hook) stamped the bit.
        if ((reasons & static_cast<std::uint8_t>(kOccurrenceDirty)) != 0)
            mark_occurrence_stale(id);
        // Issue #320: stamp the per-node epoch with the
        // current mutation epoch (if known). The
        // synthesize_flat cache will compare this against
        // cache_epoch_ to decide per-node invalidation
        // (follow-up wiring). For now (this PR is the
        // foundation), the column is populated but not
        // consulted.
        //
        // The mark_dirty signature doesn't take an
        // explicit epoch (callers don't always have one
        // handy). The stamp uses a separate helper
        // stamp_last_seen_epoch() that the higher-level
        // mark_dirty_upward() / typed_mutate paths call
        // with the current global mutation_epoch_.
        // Here we just bump the column by 1 from the
        // previous value to give a "touched" signal for
        // tests + introspection (the value isn't yet
        // meaningful for the cache check; that's a
        // follow-up).
        if (id < last_seen_epoch_.size())
            last_seen_epoch_[id] += 1;
    }

    // Issue #320: mark_dirty_for_reinfer helper.
    // Combines mark_dirty + explicit epoch stamp. The
    // caller passes the current global mutation_epoch_
    // (from CompilerService::mutation_epoch_). The
    // synthesize_flat cache check (follow-up wiring)
    // will compare this against cache_epoch_ to decide
    // per-node invalidation.
    //
    // For now (this PR is the foundation), this helper
    // exists so typed_mutate / mark_dirty_upward can
    // opt into the per-node epoch path when the global
    // epoch is available. Callers that don't have the
    // global epoch handy can fall back to plain
    // mark_dirty (which still bumps the column by 1).
    void mark_dirty_for_reinfer(NodeId id, std::uint64_t current_epoch) {
        mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
        stamp_last_seen_epoch(id, current_epoch);
    }
    void mark_subtree_dirty(NodeId id, std::uint8_t reasons = kGeneralDirty,
                            std::uint8_t ppa_reasons = 0) {
        mark_dirty(id, reasons);
        apply_ppa_dirty_bits(id, ppa_reasons);
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE)
                mark_subtree_dirty(c, reasons, ppa_reasons);
        }
    }
    // Issue #188: backward-compatible single-bit semantics — true if
    // any dirty bit is set. The pre-#188 callers that asked "is this
    // node dirty?" still get the right answer.
    bool is_dirty(NodeId id) const { return id < dirty_.size() && dirty_[id] != 0; }

    // Issue #337: flat views over SoA columns. C++23
    // std::span gives a non-owning, bounds-checked
    // view of contiguous memory; combined with
    // std::views::zip (C++23), the caller can iterate
    // multiple columns in lockstep without
    // per-element overhead. The view is invalidated
    // by any add_node / reset_all / push_back call
    // (the underlying vector may reallocate); callers
    // that hold a view across mutations should
    // re-acquire it.
    //
    // The views are the foundation for the C++23
    // modernization issue; concrete pass-level
    // adoption (query:pattern, mark_dirty_upward_fast,
    // IRFunctionSoA scans) is follow-up work.
    [[nodiscard]] std::span<const std::uint8_t> dirty_view() const noexcept {
        return std::span<const std::uint8_t>(dirty_.data(), dirty_.size());
    }
    [[nodiscard]] std::span<const std::uint8_t> ppa_dirty_view() const noexcept {
        return std::span<const std::uint8_t>(ppa_dirty_.data(), ppa_dirty_.size());
    }
    // Issue #320: per-node epoch column view.
    [[nodiscard]] std::span<const std::uint64_t> last_seen_epoch_view() const noexcept {
        return std::span<const std::uint64_t>(last_seen_epoch_.data(), last_seen_epoch_.size());
    }
    // Issue #456: dirty column accessor (the main dirty_).
    [[nodiscard]] std::span<const std::uint8_t> verify_dirty_view() const noexcept {
        return std::span<const std::uint8_t>(verify_dirty_.data(), verify_dirty_.size());
    }
    [[nodiscard]] std::span<const std::uint8_t> verification_dirty_view() const noexcept {
        return std::span<const std::uint8_t>(verification_dirty_.data(),
                                             verification_dirty_.size());
    }

    // Issue #346: mutation_log view (most-recent first).
    // Non-owning span over the log. The vector grows
    // unbounded (no eviction); for production runs
    // with many mutations, the agent can sample via
    // (query:mutations-since <last_id>) instead of
    // walking the whole log.
    [[nodiscard]] std::span<const MutationRecord> mutation_log_view() const noexcept {
        return std::span<const MutationRecord>(mutation_log_.data(), mutation_log_.size());
    }
    // Issue #188: targeted check — true if a specific reason bit
    // (or any of the bits in the reason mask) is set. Lets the type
    // checker say "this node's occurrence narrowing is stale but
    // ownership is fine" and re-narrow only.
    bool is_dirty_for(NodeId id, std::uint8_t reason_mask) const {
        return id < dirty_.size() && (dirty_[id] & reason_mask) != 0;
    }
    // Issue #188: return the full dirty bitmask (for diagnostics).
    std::uint8_t dirty_reasons(NodeId id) const { return id < dirty_.size() ? dirty_[id] : 0; }
    void clear_dirty(NodeId id) {
        if (id < dirty_.size())
            dirty_[id] = 0;
    }
    // Issue #188: clear specific reason bits (leaves others set).
    // Used after a targeted re-analysis pass so the bit for the
    // resolved concern is cleared but other stale reasons remain.
    void clear_dirty_for(NodeId id, std::uint8_t reason_mask) {
        if (id < dirty_.size())
            dirty_[id] &= static_cast<std::uint8_t>(~reason_mask);
    }
    // Issue #188: read-only view of the dirty column for
    // observability/aggregation. Used by the (dirty:counts)
    // primitive to walk all nodes in O(n) without a per-node
    // accessor call.
    [[nodiscard]] const std::pmr::vector<std::uint8_t>& dirty_column() const noexcept {
        return dirty_;
    }
    // Issue #277: PPA dirty accessors (orthogonal column).
    void mark_ppa_dirty(NodeId id, std::uint8_t ppa_reasons) {
        apply_ppa_dirty_bits(id, ppa_reasons);
    }
    bool is_ppa_dirty_for(NodeId id, std::uint8_t ppa_mask) const {
        return id < ppa_dirty_.size() && (ppa_dirty_[id] & ppa_mask) != 0;
    }
    std::uint8_t ppa_dirty_reasons(NodeId id) const {
        return id < ppa_dirty_.size() ? ppa_dirty_[id] : 0;
    }
    void clear_ppa_dirty(NodeId id) {
        if (id < ppa_dirty_.size())
            ppa_dirty_[id] = 0;
    }
    void clear_ppa_dirty_for(NodeId id, std::uint8_t ppa_mask) {
        if (id < ppa_dirty_.size())
            ppa_dirty_[id] &= static_cast<std::uint8_t>(~ppa_mask);
    }
    [[nodiscard]] const std::pmr::vector<std::uint8_t>& ppa_dirty_column() const noexcept {
        return ppa_dirty_;
    }
    // Issue #190: read-only view of the marker column for
    // observability/aggregation. Used by the (syntax-marker-counts)
    // primitive to walk all nodes in O(n).
    [[nodiscard]] const std::pmr::vector<SyntaxMarker>& marker_column() const noexcept {
        return marker_;
    }

    // Issue #367: per-node provenance accessor + setter.
    // provenance_id is an index into the FlatAST's own
    // MarkerProvenanceTable (so it's per-FlatAST — no cross-AST
    // lookup required). 0 means "no provenance recorded".
    void set_provenance(NodeId id, std::uint32_t prov_id) noexcept {
        if (id < provenance_.size())
            provenance_[id] = prov_id;
    }
    [[nodiscard]] std::uint32_t provenance(NodeId id) const noexcept {
        if (id >= provenance_.size())
            return 0;
        return provenance_[id];
    }
    [[nodiscard]] const std::pmr::vector<std::uint32_t>& provenance_column() const noexcept {
        return provenance_;
    }

    // ── Node validation (NodeMeta invariants) ─────────────────
    // Checks a single node against its NodeMeta invariants.
    // Returns a description of the first violation, or empty string if valid.
    // If fail_on_error is true, asserts on violation.
    std::string validate_node(NodeId id, bool fail_on_error = true) const;

    // Validate all nodes in the FlatAST. Returns total violations found.
    // If fail_on_error is true, asserts on first violation.
    std::size_t validate_all_nodes(bool fail_on_error = true) const;

    // Validation note type (for non-fatal reporting)
    struct ValidationError {
        NodeId node;
        std::string message;
        std::string expected;
        std::string actual;
    };
    // Validate all nodes, populating errors vector instead of asserting.
    std::size_t validate_all_nodes(std::vector<ValidationError>& errors) const;

    // Issue #263: post-restore consistency check. Verifies generation_
    // / node_gen_ alignment, parent/child bidirectional integrity, and
    // that all child spans reference live nodes. Populates `errors` when
    // non-null. Returns violation count (0 = consistent).
    [[nodiscard]] PostRestoreReport
    validate_post_restore(std::vector<ValidationError>* errors = nullptr) const;

    // ── Value result cache (for incremental eval) ────────────
    // Stores the last EvalValue result for each node.
    // kNotCached = not yet evaluated or cache invalidated.
    // When a node is marked dirty, its cache is cleared automatically.
    static constexpr std::int64_t kNotCached = 0x7FFFFFFFFFFFFFFFLL; // INT64_MAX as sentinel
    std::int64_t get_cached_value(NodeId id) const {
        return id < static_cast<NodeId>(value_cache_.size()) ? value_cache_[id] : kNotCached;
    }
    void set_cached_value(NodeId id, std::int64_t val) {
        if (id >= static_cast<NodeId>(value_cache_.size()))
            value_cache_.resize(static_cast<std::size_t>(id) + 1, kNotCached);
        value_cache_[id] = val;
    }
    void clear_cached_value(NodeId id) {
        if (id < static_cast<NodeId>(value_cache_.size()))
            value_cache_[id] = kNotCached;
    }

    // ── Match clause metadata ────────────────────────────────
    void set_match_info(NodeId id, MatchClauseInfo info) {
        if (id >= match_info_.size())
            match_info_.resize(id + 1);
        match_info_[id] = std::move(info);
    }
    bool has_match_info(NodeId id) const {
        if (id >= match_info_.size())
            return false;
        const auto& mi = match_info_[id];
        return !mi.used_constructors.empty() || !mi.candidate_constructors.empty() ||
               mi.has_wildcard;
    }
    const MatchClauseInfo* get_match_info(NodeId id) const {
        if (!has_match_info(id))
            return nullptr;
        return &match_info_[id];
    }
    // Propagate dirty upward: mark this node AND all ancestors dirty
    // Uses parent_ SoA column for O(depth) traversal (iterative, no recursion)
    // Issue #188: optional `reasons` parameter propagates the bitmask
    // from the leaf to all ancestors. Default is kGeneralDirty for
    // backward compatibility with the 30+ callers that don't yet
    // classify their mutations.
    // Issue #1251: production bounds for dirty propagation under
    // large-scale AI multi-round editing. Configurable via these
    // constants; Agent can observe truncations via counters.
    static constexpr std::uint64_t kMarkDirtyMaxDepth = 64;
    static constexpr std::uint64_t kMarkDirtyCountThreshold = 4096;

    void mark_dirty_upward(const NodeId id, std::uint8_t reasons = kGeneralDirty,
                           std::uint8_t ppa_reasons = 0)
        // Issue #273: node must be in-bounds; NULL_NODE would resize dirty_ to ~4G.
        // Issue #1466: post-condition ensures the call counter is bumped
        // (every dirty cascade invocation is accounted for in metrics).
        // Zero release cost under observe semantic.
        pre(id < tag_.size())
            post(mark_dirty_upward_call_count_.load(std::memory_order_relaxed) > 0) {
        // Issue #1620: dirty cascade is a core mutation hot path —
        // probe invariant hit for Agents (pairs with Contracts pre/post).
        aura::core::cpp26::record_hotpath_invariant_hit();
        contract_assert(kMarkDirtyMaxDepth == 64);
        // Issue #256: bump the call counter + track total nodes
        // touched. The ratio (total_nodes / call_count) gives
        // the average dirty-propagation depth per mutation —
        // the key metric for whether the std::meta refactor is
        // worth it.
        mark_dirty_upward_call_count_.fetch_add(1, std::memory_order_relaxed);
        // Issue #693: SV structural / SVA feedback nodes propagate
        // verify_dirty_ upward for targeted sv_ir re-emit hints.
        bool propagate_sva_verify = false;
        if (id < tag_.size()) {
            const auto src_tag = tag_[id];
            propagate_sva_verify = (src_tag == NodeTag::Interface || src_tag == NodeTag::Modport ||
                                    src_tag == NodeTag::Property || src_tag == NodeTag::Sequence ||
                                    src_tag == NodeTag::Assert || src_tag == NodeTag::Covergroup ||
                                    src_tag == NodeTag::Coverpoint ||
                                    src_tag == NodeTag::Constraint || src_tag == NodeTag::Class);
        }
        if (!propagate_sva_verify && id < verify_dirty_.size())
            propagate_sva_verify = (verify_dirty_[id] & kSvaDirty) != 0;
        std::uint64_t touched = 0;
        bool truncated = false;
        std::deque<NodeId> queue;
        queue.push_back(id);
        while (!queue.empty()) {
            // Issue #1251: bound depth/count to avoid p99 latency spikes
            // on pathological parent chains / SoC-scale ASTs.
            if (touched >= kMarkDirtyMaxDepth || touched >= kMarkDirtyCountThreshold) {
                truncated = true;
                mark_dirty_truncated_count_.fetch_add(1, std::memory_order_relaxed);
                // Still stamp the current chain top so Define-level
                // subtree_gen consumers observe invalidation.
                if (!queue.empty()) {
                    auto top = queue.front();
                    mark_dirty(top, reasons);
                    if (top < tag_.size())
                        bump_generation_subtree(top);
                }
                break;
            }
            auto nid = queue.front();
            queue.pop_front();
            mark_dirty(nid, reasons);
            apply_ppa_dirty_bits(nid, ppa_reasons);
            if (propagate_sva_verify)
                apply_verify_dirty_bits(nid, kSvaDirty);
            ++touched;
            auto p = parent_[nid];
            if (p != NULL_NODE)
                queue.push_back(p);
        }
        (void)truncated;
        // Issue #471: track max traversal depth. The
        // max-depth is the deepest BFS level reached in
        // this call. Atomic max — CAS loop.
        {
            const std::uint64_t depth = touched;
            std::uint64_t cur = mark_dirty_max_depth_observed_.load(std::memory_order_relaxed);
            while (depth > cur) {
                if (mark_dirty_max_depth_observed_.compare_exchange_weak(cur, depth))
                    break;
            }
        }
        mark_dirty_total_nodes_.fetch_add(touched, std::memory_order_relaxed);
        // Issue #547: mark the tag_arity_index dirty so the
        // next (query:pattern) call knows to rebuild (or
        // patch) the index. mark_tag_arity_index_dirty()
        // bumps the dirty_marks counter (stats).
        mark_tag_arity_index_dirty();
        // Issue #1503: when the index is already warm, live-patch
        // the cascade seed so arity/tag re-keys stay O(1) and
        // ensure_tag_arity_index can take the incremental path
        // instead of a surprise full O(N) rebuild on large ASTs.
        if (!tag_arity_index_.empty() && id < size())
            patch_tag_arity_index_node(id);
        // Issue #412: bump the type cache generation. Every
        // mark_dirty_upward() call invalidates ALL cached
        // type_id_ entries (they were computed against an
        // older binding/predicate context). Cache entries
        // captured at the current gen will be re-checked
        // on the next hit and recomputed if the gen
        // diverges. See set_type_with_gen() and
        // synthesize_flat()'s cache hit path.
        type_cache_generation_.fetch_add(1, std::memory_order_relaxed);
        // Issue #412 follow-up #1: per-binding gen. If the
        // target node is a binding (Define/Let/LetRec)
        // with a valid sym_id, bump THAT binding's gen
        // (not just the global gen). This is the
        // per-binding granular invalidation signal: the
        // global gen invalidates ALL cache entries (over-
        // invalidating), the per-binding gen only
        // invalidates cache entries that depend on this
        // specific binding. For non-binding targets (sub-
        // expression mutations), only the global gen
        // bumps (no binding to bump).
        if (id < tag_.size()) {
            auto tgv = get(id);
            if ((tgv.tag == NodeTag::Define || tgv.tag == NodeTag::Let ||
                 tgv.tag == NodeTag::LetRec) &&
                tgv.sym_id != INVALID_SYM) {
                bump_binding_gen(tgv.sym_id);
                // Issue #413: record the (mutation_id,
                // SymId) pair so users can trace
                // invalidation back to the mutation.
                // The most recent mutation_id is
                // next_mutation_id_ - 1 (the counter
                // was bumped in add_mutation / add_subtree
                // before mark_dirty_upward was called).
                if (next_mutation_id_ > 1) {
                    const std::uint64_t mid = next_mutation_id_ - 1;
                    invalidation_trace_.push_back({
                        .sym = tgv.sym_id,
                        .mutation_id = mid,
                        .binding_gen_at_bump = binding_gen(tgv.sym_id),
                    });
                    invalidation_trace_records_total_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // Issue #639: invalidate narrowing provenance in the
        // mutated subtree. Any mark_dirty_upward may affect
        // predicate/if-context bindings downstream.
        (void)invalidate_narrowings_in_subtree(
            id, type_cache_generation_.load(std::memory_order_relaxed));
    }

    // Issue #336: optimized variant of mark_dirty_upward.
    // Early-exits the upward walk when a parent already
    // has the target reason bits set (leverages the
    // bitmask as a fixed-point check). This is the
    // "fine-grained" optimization the issue asks for:
    // when the parent already has the bits, the BFS
    // stops there (no need to mark the grandparent
    // again — it inherits the bits through the parent
    // anyway, since any analysis that checks
    // `is_dirty_for(p, mask)` walks up via parent_).
    //
    // Win: in deep ASTs with many small mutations
    // (common in AI self-modification loops), the
    // average propagation depth drops because we don't
    // re-mark ancestors that are already dirty for
    // these reasons.
    //
    // The early-exit check uses the existing
    // is_dirty_for(id, mask) which is a single
    // 8-bit AND — much cheaper than the BFS step
    // it skips (queue.push_back + pop_front + mark_dirty
    // + apply_ppa_dirty_bits + per-call counter bump).
    //
    // The stats counter dirty_upward_fast_fixed_point_hits_
    // tracks how many times the early-exit fired (for
    // perf benchmarking). On workloads with lots of
    // repeated small mutations in deep ASTs, this
    // should fire often.
    // Issue #1345: optional max_depth (-1 = default kMarkDirtyMaxDepth)
    // and stop_at_boundary (Define/Interface/Module/Modport prune).
    void mark_dirty_upward_fast(const NodeId id, std::uint8_t reasons = kGeneralDirty,
                                std::uint8_t ppa_reasons = 0, int max_depth = -1,
                                bool stop_at_boundary = false) pre(id < tag_.size()) {
        mark_dirty_upward_call_count_.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t touched = 0;
        std::uint64_t fixed_point_hits = 0;
        // max_depth < 0 → unlimited (must not silently use kMarkDirtyMaxDepth).
        const bool limit_depth = max_depth >= 0;
        const std::uint64_t depth_cap =
            limit_depth ? static_cast<std::uint64_t>(max_depth) : UINT64_MAX;
        std::deque<NodeId> queue;
        queue.push_back(id);
        while (!queue.empty()) {
            if (limit_depth && touched >= depth_cap) {
                mark_dirty_truncated_count_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            auto nid = queue.front();
            queue.pop_front();
            // Issue #336: if the node is already dirty
            // for ALL the target reasons, skip the
            // mark (the bitmask is idempotent under OR).
            if (!is_dirty_for(nid, reasons)) {
                mark_dirty(nid, reasons);
                apply_ppa_dirty_bits(nid, ppa_reasons);
                ++touched;
            }
            // Issue #1345: configurable boundary prune — stop
            // ascending at module/interface/define roots so
            // large SoC ASTs do not re-dirty the entire tree.
            if (stop_at_boundary && nid < tag_.size()) {
                const auto t = tag_[nid];
                if (t == NodeTag::Define || t == NodeTag::Interface || t == NodeTag::DefineModule ||
                    t == NodeTag::Modport) {
                    mark_dirty_boundary_prune_count_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }
            auto p = parent_[nid];
            if (p == NULL_NODE)
                continue;
            // Issue #336: early-exit when the parent
            // already has all the target reason bits
            // set. The parent's parents will inherit
            // the bits through it (any analysis that
            // checks the parent will see "dirty for
            // these reasons" and propagate further
            // itself if needed).
            if (is_dirty_for(p, reasons)) {
                ++fixed_point_hits;
                // Issue #471: also bump the lifetime
                // mark_dirty_early_exit_count_ for
                // (query:dirty-propagation-stats).
                mark_dirty_early_exit_count_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            queue.push_back(p);
        }
        // Issue #471: track max depth seen on fast path
        // (same atomic max as the plain mark_dirty_upward).
        {
            const std::uint64_t depth = touched;
            std::uint64_t cur = mark_dirty_max_depth_observed_.load(std::memory_order_relaxed);
            while (depth > cur) {
                if (mark_dirty_max_depth_observed_.compare_exchange_weak(cur, depth))
                    break;
            }
        }
        mark_dirty_total_nodes_.fetch_add(touched, std::memory_order_relaxed);
        dirty_upward_fast_fixed_point_hits_.fetch_add(fixed_point_hits, std::memory_order_relaxed);
        mark_tag_arity_index_dirty();
        // Issue #1503: live-patch seed when index is warm (same as mark_dirty_upward).
        if (!tag_arity_index_.empty() && id < size())
            patch_tag_arity_index_node(id);
        type_cache_generation_.fetch_add(1, std::memory_order_relaxed);
        // Issue #1455: fast upward path must invalidate
        // NarrowingRecords + occ_stale_ the same way as
        // mark_dirty_upward (predicate-affecting mutates
        // often use _fast).
        (void)invalidate_narrowings_in_subtree(
            id, type_cache_generation_.load(std::memory_order_relaxed));
    }

    // Issue #336: clear specific bits on a node and
    // all descendants. The pre-existing clear_dirty_for
    // (line ~3747) handles the single-node case; this
    // is the range variant for re-analysis passes
    // that have already propagated the cleared bits'
    // new value upward.
    void clear_dirty_for_subtree(NodeId id, std::uint8_t reason_mask) {
        if (id < dirty_.size())
            dirty_[id] &= static_cast<std::uint8_t>(~reason_mask);
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE)
                clear_dirty_for_subtree(c, reason_mask);
        }
    }

    // Issue #336: counter for the fast-path early-exit
    // hits. Surfaced via (compile:dirty-fast-stats)
    // for observability.
    [[nodiscard]] std::uint64_t dirty_upward_fast_fixed_point_count() const noexcept {
        return dirty_upward_fast_fixed_point_hits_.load(std::memory_order_relaxed);
    }

    // Issue #262: propagate dirty upward but stop at `stop_at` (exclusive).
    // Marks `id` and ancestors until (but not including) `stop_at`.
    // If `stop_at` is NULL_NODE, behaves like mark_dirty_upward.
    void mark_dirty_upward_until(NodeId id, std::uint8_t reasons, NodeId stop_at,
                                 std::uint8_t ppa_reasons = 0) {
        mark_dirty_upward_call_count_.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t touched = 0;
        auto cur = id;
        while (cur != NULL_NODE && cur != stop_at) {
            mark_dirty(cur, reasons);
            apply_ppa_dirty_bits(cur, ppa_reasons);
            ++touched;
            cur = parent_[cur];
        }
        mark_dirty_total_nodes_.fetch_add(touched, std::memory_order_relaxed);
        // Issue #412: see mark_dirty_upward() for the rationale.
        // Bump the type cache generation here too — the
        // until-stop variant is used by structural mutations
        // that want to invalidate the cache for a subtree
        // but preserve ancestor caches (e.g., re-typing a
        // single function body without re-typing its
        // callers). Same gen bump as the unbounded variant.
        type_cache_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #262: mark def-use entry nodes and propagate the combined
    // reason mask (including kDefUseDirty) upward through ancestors.
    // Used when a mutation affects a known set of caller/use sites.
    void mark_dirty_defuse_entries(std::span<const NodeId> entries, std::uint8_t reasons,
                                   std::uint8_t ppa_reasons = 0) {
        auto combined = static_cast<std::uint8_t>(reasons | kDefUseDirty);
        for (auto entry : entries) {
            if (entry < size())
                mark_dirty_upward(entry, combined, ppa_reasons);
        }
    }

    // Check if any node in a subtree (including the root) is dirty
    bool has_dirty_subtree(NodeId id) const {
        if (is_dirty(id))
            return true;
        auto v = get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE && has_dirty_subtree(c))
                return true;
        }
        return false;
    }

    void clear_all_dirty() {
        std::fill(dirty_.begin(), dirty_.end(), 0);
        std::fill(ppa_dirty_.begin(), ppa_dirty_.end(), 0);
        // DO NOT clear value_cache_ here! The value cache persists across
        // eval-current calls so that non-dirty subtrees keep their cached
        // results. Only mark_dirty() (called on mutation) clears individual
        // cache entries. This enables subtree-level incremental re-evaluation:
        // after eval-current, the cache is populated. On the next call, clean
        // nodes return cached results immediately (see eval_flat cache check).
        // When a mutation marks nodes dirty, their cache entries are cleared
        // by mark_dirty() → clear_cached_value().
    }

    // ── Mutation audit ──────────────────────────────────────────

    // Issue #1419: compound provenance context stamped into every
    // new MutationRecord. 0 = system / no parent / no composite
    // (backward compatible with pre-#1412 records). Set by
    // Evaluator::set_current_agent_fingerprint and
    // TypedTransactionGuard for atomic multi-mutate.
    void set_mutation_author_fingerprint(std::uint64_t fp) noexcept {
        mutation_author_fingerprint_ = fp;
    }
    [[nodiscard]] std::uint64_t mutation_author_fingerprint() const noexcept {
        return mutation_author_fingerprint_;
    }
    void set_mutation_parent_mutation_id(std::uint64_t id) noexcept {
        mutation_parent_mutation_id_ = id;
    }
    [[nodiscard]] std::uint64_t mutation_parent_mutation_id() const noexcept {
        return mutation_parent_mutation_id_;
    }
    void set_mutation_composite_transaction_id(std::uint64_t id) noexcept {
        mutation_composite_transaction_id_ = id;
    }
    [[nodiscard]] std::uint64_t mutation_composite_transaction_id() const noexcept {
        return mutation_composite_transaction_id_;
    }

    // Record a mutation on a node. Returns the mutation_id.
    std::uint64_t add_mutation(NodeId node, std::string_view op_name, std::string_view old_type,
                               std::string_view new_type, std::string_view summary,
                               MutationStatus status = MutationStatus::Committed) {
        return add_mutation_with_rollback(node, op_name, old_type, new_type, summary, status, 0, 0,
                                          0, false);
    }

    // Record a mutation with rollback data (field_offset + old/new_value).
    // Issue #1696: node may be NULL_NODE for multi-node ops
    // (replace-pattern, rename-symbol). NULL_NODE is ~0u, not 0 — bare
    // 0 is a real NodeId and must not stand in for "whole-tree / N-sites".
    // When node is NULL_NODE, skip mark_dirty_upward and node_first_mutation_
    // (no single target; callers already dirtied individual sites).
    std::uint64_t add_mutation_with_rollback(const NodeId node, std::string_view op_name,
                                             std::string_view old_type, std::string_view new_type,
                                             std::string_view summary, MutationStatus status,
                                             std::uint32_t field_offset, std::uint64_t old_value,
                                             std::uint64_t new_value, bool has_rollback)
        pre(node == NULL_NODE || node < tag_.size()) post(r : r >= 1) {
        const std::uint64_t mid = next_mutation_id_++;
        // Issue #1355: under render lightweight checkpoint, field-level
        // records go to a side stack (not mutation_log_) so hot-path
        // frames do not inflate the durable log.
        if (render_lightweight_active_ && has_rollback && !lightweight_frames_.empty()) {
            MutationRecord rec = mutation::create_mutation_record({
                .mutation_id = mid,
                .target_node = node,
                .operator_name = op_name,
                .old_type_str = old_type,
                .new_type_str = new_type,
                .summary = summary,
                .status = status,
                .field_offset = field_offset,
                .old_value = old_value,
                .new_value = new_value,
                .has_rollback_data = has_rollback,
                .author_fingerprint = mutation_author_fingerprint_,
                .parent_mutation_id = mutation_parent_mutation_id_,
                .composite_transaction_id = mutation_composite_transaction_id_,
            });
            lightweight_frames_.back().push_back(std::move(rec));
            lightweight_records_total_.fetch_add(1, std::memory_order_relaxed);
            if (node != NULL_NODE)
                mark_dirty_upward(node);
            return mid;
        }
        mutation_log_.push_back(mutation::create_mutation_record({
            .mutation_id = mid,
            .target_node = node,
            .operator_name = op_name,
            .old_type_str = old_type,
            .new_type_str = new_type,
            .summary = summary,
            .status = status,
            .field_offset = field_offset,
            .old_value = old_value,
            .new_value = new_value,
            .has_rollback_data = has_rollback,
            .author_fingerprint = mutation_author_fingerprint_,
            .parent_mutation_id = mutation_parent_mutation_id_,
            .composite_transaction_id = mutation_composite_transaction_id_,
        }));
        if (node != NULL_NODE) {
            mark_dirty_upward(node);
            if (node < node_first_mutation_.size() && node_first_mutation_[node] == 0)
                node_first_mutation_[node] = static_cast<std::uint32_t>(mutation_log_.size());
        }
        // Issue #1362: auto-compact committed prefix when log is huge
        maybe_auto_compact_mutation_log();
        return mid;
    }

    // ── Issue #1355: render hot-path lightweight checkpoints ──
    void begin_render_lightweight_checkpoint() noexcept {
        lightweight_frames_.emplace_back();
        render_lightweight_active_ = true;
        lightweight_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void commit_render_lightweight_checkpoint() noexcept {
        if (lightweight_frames_.empty()) {
            render_lightweight_active_ = false;
            return;
        }
        lightweight_frames_.pop_back(); // discard side log (committed)
        render_lightweight_active_ = !lightweight_frames_.empty();
        lightweight_commit_total_.fetch_add(1, std::memory_order_relaxed);
    }

    // Roll back field mutations stored in the current lightweight frame.
    std::size_t rollback_render_lightweight_checkpoint() noexcept {
        if (lightweight_frames_.empty()) {
            render_lightweight_active_ = false;
            return 0;
        }
        auto& frame = lightweight_frames_.back();
        std::size_t count = 0;
        const bool prev_defer = defer_rollback_restamp_;
        defer_rollback_restamp_ = true;
        for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
            if (it->status != MutationStatus::Committed)
                continue;
            if (try_rollback_record(*it).has_value())
                ++count;
        }
        defer_rollback_restamp_ = prev_defer;
        if (count > 0)
            restamp_all_node_generations();
        lightweight_frames_.pop_back();
        render_lightweight_active_ = !lightweight_frames_.empty();
        lightweight_rollback_total_.fetch_add(1, std::memory_order_relaxed);
        return count;
    }

    // Commit all nested lightweight frames (frame boundary auto-commit).
    std::size_t commit_all_render_lightweight_checkpoints() noexcept {
        std::size_t n = 0;
        while (!lightweight_frames_.empty()) {
            commit_render_lightweight_checkpoint();
            ++n;
        }
        return n;
    }

    [[nodiscard]] bool render_lightweight_active() const noexcept {
        return render_lightweight_active_;
    }
    [[nodiscard]] std::size_t render_lightweight_depth() const noexcept {
        return lightweight_frames_.size();
    }
    [[nodiscard]] std::uint64_t lightweight_total() const noexcept {
        return lightweight_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t lightweight_commit_total() const noexcept {
        return lightweight_commit_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t lightweight_rollback_total() const noexcept {
        return lightweight_rollback_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t lightweight_records_total() const noexcept {
        return lightweight_records_total_.load(std::memory_order_relaxed);
    }

    // Issue #142: record a subtree-level mutation (e.g. mutate:replace-subtree).
    // The target_node here is the NEW subtree's root. The old_subtree_source is
    // kept verbatim so rollback can re-parse and re-attach without needing
    // a generation-aware node lookup.
    std::uint64_t add_mutation_subtree(NodeId target_node, NodeId parent_id,
                                       std::uint32_t child_idx, std::string_view old_subtree_source,
                                       std::string_view op_name, std::string_view summary) {
        const std::uint64_t mid = next_mutation_id_++;
        mutation_log_.push_back(mutation::create_subtree_mutation_record({
            .mutation_id = mid,
            .target_node = target_node,
            .parent_id = parent_id,
            .child_idx = child_idx,
            .old_subtree_source = old_subtree_source,
            .operator_name = op_name,
            .summary = summary,
            .author_fingerprint = mutation_author_fingerprint_,
            .parent_mutation_id = mutation_parent_mutation_id_,
            .composite_transaction_id = mutation_composite_transaction_id_,
        }));
        if (target_node != NULL_NODE)
            mark_dirty_upward(target_node);
        if (parent_id != NULL_NODE)
            mark_dirty_upward(parent_id);
        if (target_node < node_first_mutation_.size() && node_first_mutation_[target_node] == 0)
            node_first_mutation_[target_node] = static_cast<std::uint32_t>(mutation_log_.size());
        // Issue #1362: auto-compact committed prefix when log is huge
        maybe_auto_compact_mutation_log();
        return mid;
    }

    // Issue #222 slice 2/3: record a structural child-list mutation
    // (set_child / insert_child / remove_child). Captures parent,
    // child_idx, old child NodeId, new child NodeId. Rollback is via
    // the #221 children_snapshot (capture in the #177 MutationCheckpoint)
    // + rollback_to_size on the mutation log; has_rollback=true signals
    // that the caller has set up a snapshot for rollback.
    //
    // Calls mark_dirty_upward(parent) so the parent + ancestors are
    // marked dirty for incremental re-eval (#148) / dirty-aware
    // caching (#188). The "structural-" prefix on op_name distinguishes
    // these from typed_mutate's "replace-type" / "replace-value" kinds.
    std::uint64_t add_mutation_child_op(NodeId parent, std::uint32_t child_idx, NodeId old_child,
                                        NodeId new_child, std::string_view op_name) {
        return add_mutation_with_rollback(
            parent, op_name, "", "", op_name, MutationStatus::Committed, child_idx,
            static_cast<std::uint64_t>(old_child), static_cast<std::uint64_t>(new_child),
            /*has_rollback=*/true);
    }

    // Issue #369: convenience wrapper for Aura-level mutate
    // primitives that mutate the children_ column. Remaps the
    // wrapper-op name (e.g. "remove-node") to a canonical
    // "structural-X-child" name so that
    // try_rollback_structural_child_op (which dispatches on the
    // canonical name) can roll back the children_ SoA column.
    // Wrapper primitives should prefer this over flat.add_mutation()
    // for any op that modifies the children_ column.
    std::uint64_t add_structural_mutation_log_entry(NodeId parent, std::uint32_t child_idx,
                                                    NodeId old_child, NodeId new_child,
                                                    std::string_view op_name) {
        std::string canonical;
        if (op_name == "remove-node" || op_name == "remove-child" ||
            op_name == "structural-remove-child")
            canonical = "structural-remove-child";
        else if (op_name == "insert-child" || op_name == "structural-insert-child")
            canonical = "structural-insert-child";
        else if (op_name == "set-body" || op_name == "set-child" ||
                 op_name == "structural-set-child")
            canonical = "structural-set-child";
        else
            canonical = std::string(
                op_name); // pass through; try_rollback_structural_child_op may still skip
        return add_mutation_child_op(parent, child_idx, old_child, new_child, canonical);
    }

    // Get mutation history for a specific node (0 == no history)
    // Get mutation history for a specific node (filters from log, O(n) in log size)
    std::pmr::vector<MutationRecord> mutation_history(NodeId node) const {
        std::pmr::vector<MutationRecord> result;
        for (auto& rec : mutation_log_) {
            if (rec.target_node == node)
                result.push_back(rec);
        }
        return result;
    }

    // Total number of mutations recorded
    std::size_t mutation_count() const { return mutation_log_.size(); }

    // Issue #1408: Count of mutations that are still Committed (i.e.
    // not yet RolledBack). The mutation_log_ retains RolledBack records
    // for audit (see Issue #1301 follow-up `mutation_log_compacted_records_`),
    // so the raw mutation_count() is not a useful "how many effects
    // are currently applied" metric. Use this for atomic-multi-mutate
    // tests that need to verify "0 new committed mutations" after a
    // TypedTransactionGuard dtor rollback.
    std::size_t committed_mutation_count() const {
        std::size_t n = 0;
        for (const auto& rec : mutation_log_) {
            if (rec.status == MutationStatus::Committed)
                ++n;
        }
        return n;
    }

    // Issue #1408: Physically erase all mutation records with
    // mutation_id >= since_id from the log. Used by TypedTransactionGuard's
    // dtor to implement strict atomic-batch rollback: even when
    // try_rollback_record() can't undo some ops (pre-#1441 rebind lacked
    // rollback data), this method removes the record from the log so the
    // "0 applied" AC holds (committed_mutation_count() returns to its
    // pre-batch value). Issue #1441: rebind now has try_rollback_rebind_op
    // so Phase-1 rollback restores Define bodies when has_rollback_data.
    //
    // next_mutation_id_ is NOT reset here (it's monotonically increasing
    // within a session; resetting it could break other holders of the
    // higher ids).
    //
    // Returns the number of records erased.
    std::size_t erase_mutations_since(std::uint64_t since_id) {
        const auto old_size = mutation_log_.size();
        mutation_log_.erase(std::remove_if(mutation_log_.begin(), mutation_log_.end(),
                                           [since_id](const MutationRecord& r) {
                                               return r.mutation_id >= since_id;
                                           }),
                            mutation_log_.end());
        return old_size - mutation_log_.size();
    }
    std::uint64_t next_mutation_id() const { return next_mutation_id_; }

    // Get all mutation records (unfiltered).
    const std::pmr::vector<MutationRecord>& all_mutations() const { return mutation_log_; }
    std::pmr::vector<MutationRecord>& all_mutations() { return mutation_log_; }

    // Issue #1638: compact mutation_log to reclaim excess capacity.
    // Called from Evaluator::compact_mutation_log() at
    // exit_mutation_boundary success path when mutation_log_.size()
    // exceeds the COMPACT_THRESHOLD (heavy-mutation safety net;
    // 200MB+/day reclaim in long-running Agent scenarios per the
    // open mutation-log-growth issue). Returns bytes saved
    // (capacity reduction) so the caller can bump
    // mutation_log_compact_bytes_saved metric. No-op when
    // mutation_log_ is empty (shrink_to_fit on empty pmr vector
    // is free; we still return 0 to keep the metric monotonically
    // meaningful).
    std::size_t compact_mutation_log() noexcept {
        if (mutation_log_.empty())
            return 0;
        const std::size_t before = mutation_log_.capacity();
        mutation_log_.shrink_to_fit();
        const std::size_t after = mutation_log_.capacity();
        return before > after ? before - after : 0;
    }

    // Issue #1638: mutation_log entry count accessor used by the
    // boundary-exit compact threshold check (avoids exposing
    // mutation_log_ directly to Evaluator). Cheap atomic-free
    // read; size() is O(1) on pmr vector.
    std::size_t mutation_log_size() const noexcept { return mutation_log_.size(); }

    // Issue #282: Occurrence Typing provenance accessors.
    // narrowing_log_ is captured at synthesize_flat_if time
    // (see type_checker_impl.cpp). Same lifecycle as
    // mutation_log_: cleared on FlatAST reset, persists
    // across typecheck cycles within a generation.
    std::size_t narrowing_count() const { return narrowing_log_.size(); }
    const std::pmr::vector<NarrowingRecord>& all_narrowings() const { return narrowing_log_; }
    std::pmr::vector<NarrowingRecord>& all_narrowings() { return narrowing_log_; }
    // Issue #282: append a new narrowing record. Called from
    // type_checker_impl.cpp's synthesize_flat_if after a
    // successful Occurrence Typing refinement. Monotonic
    // record_id for ordering.
    void record_narrowing(NarrowingRecord rec) {
        rec.record_id = narrowing_log_.size() + 1;
        narrowing_log_.push_back(std::move(rec));
    }

    // Issue #639 / #1455: mark narrowing records stale when a
    // mutation affects their if/cond subtree or capture_epoch is
    // behind the current type-cache generation. Also stamp
    // occ_stale_ + kOccurrenceDirty on the if-node so
    // resolve_if_predicate_occurrence force-reanalyzes and the
    // safe-fallback path fires (rec.stale alone was incomplete).
    std::size_t invalidate_narrowings_in_subtree(NodeId root, std::uint64_t current_epoch) {
        if (root == NULL_NODE || root >= tag_.size())
            return 0;
        std::unordered_set<NodeId> in_subtree;
        std::vector<NodeId> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            const auto id = stack.back();
            stack.pop_back();
            if (id == NULL_NODE || id >= tag_.size())
                continue;
            if (!in_subtree.insert(id).second)
                continue;
            const auto v = get(id);
            for (auto c : v.children)
                stack.push_back(c);
        }
        const std::uint8_t kOccurrenceBit = static_cast<std::uint8_t>(kOccurrenceDirty);
        std::size_t count = 0;
        for (auto& rec : narrowing_log_) {
            if (rec.stale)
                continue;
            const bool in_tree =
                in_subtree.count(rec.if_node) > 0 || in_subtree.count(rec.cond_node) > 0;
            const bool epoch_behind = rec.capture_epoch < current_epoch;
            if (!in_tree && !epoch_behind)
                continue;
            rec.stale = true;
            ++count;
            // #1455: stamp occ_stale_ + kOccurrenceDirty only for ifs
            // inside the mutated subtree. Epoch-only invalidation marks
            // provenance stale (has_stale_narrowing_for_if) without
            // leaving orphaned occ_stale_ bits on detached if-nodes.
            if (in_tree && rec.if_node != NULL_NODE && rec.if_node < tag_.size()) {
                mark_occurrence_stale(rec.if_node);
                // Direct dirty stamp (avoid re-entrancy into
                // mark_dirty_upward invalidation).
                if (rec.if_node >= dirty_.size())
                    dirty_.resize(rec.if_node + 1, 0);
                dirty_[rec.if_node] =
                    static_cast<std::uint8_t>(dirty_[rec.if_node] | kOccurrenceBit);
            }
        }
        narrow_invalidation_post_mutate_ += count;
        return count;
    }

    // Issue #1455: after a successful re-narrow of `if_id`, clear
    // stale flags on NarrowingRecords for that if so
    // has_stale_narrowing_for_if does not keep firing on
    // historical records (new captures are appended fresh).
    std::size_t clear_stale_narrowings_for_if(NodeId if_id, std::uint64_t fresh_epoch) {
        if (if_id == NULL_NODE)
            return 0;
        std::size_t cleared = 0;
        for (auto& rec : narrowing_log_) {
            if (rec.if_node != if_id || !rec.stale)
                continue;
            rec.stale = false;
            rec.capture_epoch = fresh_epoch;
            ++cleared;
        }
        clear_occurrence_stale(if_id);
        clear_dirty_for(if_id, static_cast<std::uint8_t>(kOccurrenceDirty));
        return cleared;
    }

    // Issue #1455: true only when the *latest* record for this if
    // is stale / epoch-behind. A later non-stale re-capture
    // supersedes older stale provenance (blame chain stays
    // intact on historical rows).
    [[nodiscard]] bool has_stale_narrowing_for_if(NodeId if_id, std::uint64_t current_epoch) const {
        const NarrowingRecord* latest = nullptr;
        for (const auto& rec : narrowing_log_) {
            if (rec.if_node != if_id)
                continue;
            if (!latest || rec.record_id >= latest->record_id)
                latest = &rec;
        }
        if (!latest)
            return false;
        return latest->stale || latest->capture_epoch < current_epoch;
    }

    [[nodiscard]] std::size_t stale_narrowing_record_count() const {
        std::size_t n = 0;
        for (const auto& rec : narrowing_log_)
            if (rec.stale)
                ++n;
        return n;
    }

    [[nodiscard]] std::uint64_t narrow_invalidation_post_mutate_count() const {
        return narrow_invalidation_post_mutate_;
    }

    // Rollback a mutation by ID. Returns true if successful.
    // Current FlatAST generation. Incremented on rollback to invalidate stale NodeIds.
    std::uint16_t generation() const { return generation_; }

    // Issue #276: live slot check without generation equality (cross-layer resolve).
    [[nodiscard]] bool is_live_node(NodeId id) const noexcept {
        return id != NULL_NODE && id < tag_.size() && id < node_gen_.size() && node_gen_[id] != 0;
    }

    // Check if a NodeId is valid (in-bounds and from the current generation).
    bool is_valid(const NodeId id) const
        post(r : r == (id != NULL_NODE && id < tag_.size() && id < node_gen_.size() &&
                       node_gen_[id] == generation_)) {
        // Issue #255: bump the check counter (lifetime total).
        is_valid_check_count_.fetch_add(1, std::memory_order_relaxed);
        if (id == NULL_NODE)
            return false;
        if (id >= tag_.size() || id >= node_gen_.size()) {
            // Issue #457: stale access (out-of-range
            // NodeId) — bump the stale counter. This
            // catches dangling references that escaped
            // a structural mutation without going
            // through a StableNodeRef.
            node_gen_stale_access_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (node_gen_[id] != generation_) {
            // Issue #457: stale access (gen mismatch) —
            // same path as out-of-range. The caller
            // should have used a StableNodeRef which
            // would have caught this; raw NodeId
            // access is dangerous in long-running
            // mutates.
            node_gen_stale_access_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // Validate NodeId — panics on stale/dangling NodeIds.
    // Use in debug paths to catch post-rollback staleness early.
    // Issue #273: contract_assert replaces manual abort branches.
    void validate(NodeId id) const {
        if (id != NULL_NODE) [[likely]] {
            contract_assert(id < tag_.size());
            contract_assert(id < node_gen_.size());
            contract_assert(node_gen_[id] == generation_);
        }
    }

    // Safe get — returns nullopt on stale/invalid NodeId.
    std::optional<NodeView> get_safe(const NodeId id) const
        post(r : !r.has_value() ||
             (id < tag_.size() && id < node_gen_.size() && node_gen_[id] == generation_)) {
        if (!is_valid(id))
            return std::nullopt;
        return get(id);
    }

    // Issue #191: StableNodeRef — a safe handle that bundles a
    // NodeId with the FlatAST generation it was captured from.
    // If the generation has changed (because a structural
    // mutation happened between capture and use), the ref is
    // invalid even if the NodeId is still in-bounds.
    //
    // This is the recommended way to store NodeIds across
    // mutation calls in EDSL / query / mutate primitives. The
    // raw NodeId API is kept for performance-critical paths
    // where the caller knows the ID is fresh.
    struct StableNodeRef {
        NodeId id = NULL_NODE;
        std::uint16_t gen = 0;
        // Issue #291: provenance + workspace awareness. These
        // default to 0 / NULL_NODE which matches pre-#291
        // behavior — refs created via make_ref() will populate
        // them from the current FlatAST state. The on-disk
        // format is a packed binary blob (see
        // serialize_stable_ref / deserialize_stable_ref
        // below) so existing in-memory callers see no change.
        //   - mutation_id_at_capture: next_mutation_id_ at the
        //     time the ref was captured. Lets us answer "which
        //     mutations happened after this ref was taken" by
        //     scanning mutation_log_ for entries with id >
        //     mutation_id_at_capture. Full history lookup is a
        //     follow-up; this field exists so the
        //     serialization format is forward-compatible.
        //   - workspace_id: layer index in the WorkspaceTree
        //     (0 = root). Default 0 keeps single-workspace
        //     callers unchanged. Cross-workspace ref
        //     resolution via WorkspaceTree::resolve_stable_ref
        //     uses this field to know which layer to resolve
        //     against.
        std::uint64_t mutation_id_at_capture = 0;
        std::uint32_t workspace_id = 0;
        // Issue #303: fiber_id for cross-fiber provenance
        // tracking. Default 0 means "not in a fiber context"
        // (single-threaded / synchronous caller). Callers in a
        // fiber / agent loop should set this via
        // make_safe_ref(id, fiber_id) or capture_for_fiber()
        // to make ref-stealing bugs visible: a ref that ends up
        // on a different fiber than where it was captured is a
        // candidate stale usage (the lifetime of `fiber_id`
        // is per-mutation-context, not per-evaluator).
        std::uint32_t fiber_id = 0;
        // Issue #303: last_validated_generation. Updated by
        // validate_with_provenance() to record the generation
        // at which the ref was last checked. Lets us answer
        // "how stale is this ref since last validation" via
        // (query:ref-provenance). Defaults to 0 to match
        // pre-#303 behavior.
        std::uint16_t last_validated_generation = 0;
        // Issue #368: wrap_epoch at capture time. Bumped by
        // FlatAST::bump_generation() every time generation_
        // (uint16_t) wraps back to 1. is_valid() compares this
        // to the current wrap_epoch_ — mismatch means the ref
        // was captured at a different wrap cycle. Without this
        // check, a ref captured before wrap #1 could false-
        // positive after wrap #2 returns generation_ to the
        // same numeric value (~130K mutates in). Captured at
        // make_ref() time. Default 0 = pre-#368 ref.
        std::uint32_t wrap_epoch = 0;
        // Issue #392: subtree gen at capture time. The
        // per-top-level-Define counter (subtree_gen_[T])
        // captured at make_ref() time, for T = top-level Define
        // ancestor of the ref's node. Used by
        // is_valid_subtree() to accept refs from subtrees that
        // were NOT scoped-bumped since capture (the over-
        // invalidation fix for EDA / long-running workspaces).
        // Default 0 means "no captured subtree gen" — pre-#392
        // refs (and refs whose node has no enclosing Define)
        // are still accepted by is_valid_subtree() because
        // subtree_gen_[T] starts at 0.
        std::uint16_t subtree_gen_at_capture = 0;
        // Issue #738: COW epoch at capture for cross-boundary
        // validity. Compared against the workspace layer's
        // cow_epoch on lazy clone to detect parent→child
        // boundary crossings without over-invalidating refs
        // that remain live in the child COW copy.
        std::uint64_t cow_epoch_at_capture = 0;
        // Issue #738: set when pin_for_cow() records this ref
        // in the evaluator's boundary pin registry.
        bool boundary_pinned = false;
        // Issue #1566: multi-tenant isolation stamp. Default 0 =
        // unset / single-tenant (isolation check treats 0 as
        // "no provenance tenant constraint"). Not part of the
        // on-wire serialize_stable_ref blob (additive in-memory).
        std::uint64_t tenant_id = 0;

        // Issue #738: mark this ref as pinned across a COW
        // boundary (no-op on the ref itself; Evaluator
        // registry does the bookkeeping).
        void pin_for_cow() noexcept { boundary_pinned = true; }

        // Issue #379: bodies of these methods moved to
        // src/core/ast_stability.cpp (impl unit of aura.core.ast).
        // The class declarations stay here so the public API is
        // unchanged; only the definitions moved. All three methods
        // access FlatAST only through its public interface
        // (ast.is_valid() and ast.generation()), so no friend
        // access is needed.
        bool is_valid_in(const FlatAST& ast) const noexcept;
        // Issue #715: cross-layer StableNodeRef validity check
        // for WorkspaceTree multi-workspace setups. Combines
        // is_valid_in(ast) + workspace_id match + COW epoch
        // match (unless pin_for_cow was called). Pure read;
        // does not update last_validated_generation or bump
        // any counters. Body in src/core/ast_stability.cpp.
        bool is_valid_in_layer(const FlatAST& ast,
                               std::uint32_t target_workspace_id = 0) const noexcept;
        bool validate_with_provenance(const FlatAST& ast) noexcept;

        // Issue #497: refresh gen/wrap from a still-live node id when
        // invalidation is gen-only (same wrap epoch, slot not recycled).
        bool refresh_if_stale(FlatAST& ast) noexcept;
        std::optional<NodeView> validate_or_refresh(FlatAST& ast) noexcept;

        // Issue #303: get provenance snapshot. Returns a tuple
        // describing where the ref came from. Pure read — does
        // not validate the ref.
        struct Provenance {
            NodeId captured_id;
            std::uint16_t captured_gen;
            std::uint64_t mutation_id_at_capture;
            std::uint32_t workspace_id;
            std::uint32_t fiber_id;
            std::uint16_t last_validated_generation;
        };
        [[nodiscard]] Provenance get_provenance() const noexcept;

        // Issue #489: ergonomics for EDSL primitive validation paths.
        [[nodiscard]] explicit operator bool() const noexcept { return id != NULL_NODE; }
        [[nodiscard]] NodeId value_or(NodeId fallback) const noexcept {
            return id != NULL_NODE ? id : fallback;
        }
    };

    // Issue #291: serialize a StableNodeRef to a compact
    // 16-byte binary blob. Format (little-endian):
    //   [u32 magic=0x2901A17A][u32 id][u16 gen][u16 pad][u64 mutation_id][u32 workspace_id][u32
    //   reserved] = 4+4+2+2+8+4+4 = 24 bytes
    // The packed form is designed to be:
    //   - trivially memcpy-able (no host-endian conversion needed
    //     for use within one process; cross-endian callers
    //     should use the Aura helper (ast:ref-serialize) which
    //     returns a string in canonical order)
    //   - forward-compatible: new fields can be appended without
    //     breaking existing readers (just bump the version byte
    //     and ignore unknown trailing data)
    //   - distinguishable from old (id, gen)-only refs: the new
    //     format starts with a 4-byte magic number
    //     (0x2901A17A) so readers can tell a #291+ serialized
    //     blob from a raw (id, gen) binary.
    // Issue #379: kStableRefSerializedSize + kStableRefMagic stay
    // as class statics (callers reference them as
    // FlatAST::kStableRefSerializedSize). Moving them to a free
    // constexpr would change the public API.
    static constexpr std::size_t kStableRefSerializedSize = 24;
    static constexpr std::uint32_t kStableRefMagic = 0x2901A17A; // #291 + AURA tag

    // Issue #291: pack a StableNodeRef into a 20-byte buffer.
    // Returns the number of bytes written (= kStableRefSerializedSize).
    // Issue #379: body moved to src/core/ast_stability.cpp.
    [[nodiscard]] std::size_t serialize_stable_ref(const StableNodeRef& ref,
                                                   std::uint8_t* out) const noexcept;

    // Issue #291: deserialize a 20-byte buffer back to a
    // StableNodeRef. Returns false if the magic doesn't match
    // or buffer is too small. The caller is responsible for
    // checking is_valid() AFTER deserializing to confirm the
    // ref still points to a live node in the current flat.
    // Issue #379: body moved to src/core/ast_stability.cpp.
    [[nodiscard]] bool deserialize_stable_ref(const std::uint8_t* buf, std::size_t buf_size,
                                              StableNodeRef& out) const noexcept;

    // Issue #191: make a StableNodeRef capturing the current
    // generation. Use this in EDSL / query / mutate primitives
    // to safely hold a reference to a node across calls.
    [[nodiscard]] StableNodeRef make_ref(NodeId id) const noexcept {
        // Issue #291: also capture mutation_id_at_capture so
        // downstream callers can answer "which mutations
        // affected this node since capture" (full lookup is a
        // follow-up; the field is captured now to make the
        // serialization format forward-compatible). workspace_id
        // defaults to 0 (root); callers working across
        // WorkspaceTree layers can set it explicitly via
        // make_ref_in_layer(id, workspace_id).
        // Issue #368: also capture wrap_epoch_ so is_valid()
        // can detect false positives from a second generation_
        // wrap returning to the original numeric value.
        // Issue #392: also capture subtree_gen_at_capture
        // (the subtree_gen_ counter for the top-level Define
        // ancestor of `id`) so is_valid_subtree() can skip
        // invalidating refs in untouched subtrees.
        //
        // Backward-compat (Issue #303 Scenario 1): fiber_id and
        // last_validated_generation both default to 0 here.
        // last_validated_generation == 0 means "no validation
        // history" — make_ref() captures don't imply validation.
        // Callers that want provenance should use make_safe_ref().
        return StableNodeRef{id,
                             generation_,
                             next_mutation_id_,
                             0,
                             0,
                             0,
                             wrap_epoch_.load(std::memory_order_relaxed),
                             subtree_generation(id),
                             workspace_cow_epoch_};
    }

    // Issue #291: make_ref variant that also tags the ref
    // with a specific WorkspaceTree layer index. Used by
    // query/mutate primitives that operate on a child
    // workspace.
    [[nodiscard]] StableNodeRef make_ref_in_layer(NodeId id,
                                                  std::uint32_t workspace_id) const noexcept {
        // Issue #392: capture subtree_gen_at_capture too.
        // Backward-compat: fiber_id and last_validated_generation
        // both default to 0 (no fiber context, no validation
        // history). Same convention as make_ref().
        return StableNodeRef{id,
                             generation_,
                             next_mutation_id_,
                             workspace_id,
                             0,
                             0,
                             wrap_epoch_.load(std::memory_order_relaxed),
                             subtree_generation(id),
                             workspace_cow_epoch_};
    }

    // Issue #303: make_safe_ref records full provenance
    // (workspace_id, fiber_id, mutation_id_at_capture,
    // current generation). Prefer this over make_ref when
    // the ref will be stored across mutation boundaries or
    // used from a fiber / agent loop. Existing callers of
    // make_ref() continue to work unchanged (fiber_id
    // defaults to 0 = "not in a fiber").
    [[nodiscard]] StableNodeRef make_safe_ref(NodeId id, std::uint32_t workspace_id = 0,
                                              std::uint32_t fiber_id = 0) const noexcept {
        // Issue #392: capture subtree_gen_at_capture too.
        return StableNodeRef{id,
                             generation_,
                             next_mutation_id_,
                             workspace_id,
                             fiber_id,
                             generation_,
                             wrap_epoch_.load(std::memory_order_relaxed),
                             subtree_generation(id),
                             workspace_cow_epoch_};
    }

    // Issue #303: capture_for_fiber is a shorthand for
    // make_safe_ref with workspace_id=0 and fiber_id set.
    // The returned ref has last_validated_generation =
    // current generation_ (i.e., freshly captured).
    [[nodiscard]] StableNodeRef capture_for_fiber(NodeId id,
                                                  std::uint32_t fiber_id) const noexcept {
        return make_safe_ref(id, 0, fiber_id);
    }

    // Issue #393: explicit (id, gen) pair construction. Use
    // this when you have a `gen` value from an external
    // source (a serialized StableNodeRef buffer, a
    // cross-workspace handoff, or a manual annotation) and
    // want to wrap it as a StableNodeRef without going
    // through make_ref() / make_safe_ref(). Captures the
    // current `mutation_id_at_capture`, `workspace_id`,
    // `fiber_id`, `last_validated_generation`, and
    // `wrap_epoch` from the FlatAST state — only the `id`
    // and `gen` come from the caller's arguments.
    //
    // This is the C++ analog of `(query:children-stable)` /
    // `(query:parent-stable)` for cases where the caller
    // already knows the generation (e.g. a checkpoint file
    // from a prior session, or a cross-workspace handoff).
    [[nodiscard]] StableNodeRef make_ref_from_gen(NodeId id, std::uint16_t gen) const noexcept {
        return StableNodeRef{id,
                             gen,
                             next_mutation_id_,
                             0,
                             0,
                             gen,
                             wrap_epoch_.load(std::memory_order_relaxed),
                             subtree_generation(id),
                             workspace_cow_epoch_};
    }

    // Issue #393: flat-style validity check for callers that
    // have an (id, gen) pair but don't want to allocate a
    // StableNodeRef wrapper (e.g. in hot query primitives
    // that read thousands of pairs from a side-vector).
    // Returns true iff the slot at `id` is in-bounds AND its
    // stored generation (node_gen_[id]) matches `gen` AND
    // the FlatAST's wrap_epoch matches the captured epoch
    // (passed as `wrap_epoch_at_capture`, default = current
    // wrap_epoch_ which matches fresh captures).
    //
    // Does NOT consult generation_ — that's the whole
    // point. The caller decides what gen counts as "valid"
    // (typically the value they captured at, which may be
    // older than the current global gen if they're using
    // scoped ref tracking or a checkpoint file).
    [[nodiscard]] bool
    is_valid_id_gen(NodeId id, std::uint16_t gen,
                    std::uint32_t wrap_epoch_at_capture = 0 /* 0 = use current */) const noexcept {
        if (id == NULL_NODE || id >= tag_.size() || id >= node_gen_.size())
            return false;
        if (node_gen_[id] != gen)
            return false;
        const auto we = (wrap_epoch_at_capture == 0) ? wrap_epoch_.load(std::memory_order_relaxed)
                                                     : wrap_epoch_at_capture;
        // Issue #368: catch second-wrap false positives. If
        // the caller passed a specific wrap_epoch, check
        // against the current one (mismatch = wrapped).
        // If they passed 0 (default), use the current epoch
        // (skip the check — fresh captures are always safe).
        if (wrap_epoch_at_capture != 0 &&
            wrap_epoch_at_capture != wrap_epoch_.load(std::memory_order_relaxed))
            return false;
        (void)we;
        return true;
    }

    // Issue #191: validation + safe get that take a StableNodeRef.
    // The ref's gen is compared to the FlatAST's current gen; if
    // they differ, the ref is stale (a structural mutation
    // happened in between).
    // Issue #1500: full provenance is_valid — gen + wrap_epoch (as before)
    // plus cow_epoch match unless boundary_pinned. subtree_gen remains
    // the relaxed is_valid_subtree() path.
    //
    // Note: brace-init refs default cow_epoch_at_capture=0, which
    // matches a never-COW'd workspace (epoch 0). After a COW advance
    // they become invalid unless pin_for_cow() was called — prefer
    // make_ref / make_safe_ref for full provenance capture.
    [[nodiscard]] bool is_valid(const StableNodeRef& ref) const noexcept
        post(r : r == (ref.id != NULL_NODE && ref.id < tag_.size() && ref.id < node_gen_.size() &&
                       node_gen_[ref.id] == ref.gen && ref.gen == generation_ &&
                       // Issue #368: generation_ is uint16_t; after
                       // ~130K mutates a second wrap could match
                       // the ref's captured gen by accident. Check
                       // wrap_epoch explicitly (uint32_t, wraps
                       // every ~2.6e14 mutates).
                       ref.wrap_epoch == wrap_epoch_.load(std::memory_order_relaxed) &&
                       // Issue #1500: COW epoch must match unless pinned.
                       (ref.boundary_pinned || ref.cow_epoch_at_capture == workspace_cow_epoch_))) {
        // Issue #255: bump the check counter (lifetime total).
        is_valid_check_count_.fetch_add(1, std::memory_order_relaxed);
        bool ok = ref.id != NULL_NODE && ref.id < tag_.size() && ref.id < node_gen_.size() &&
                  node_gen_[ref.id] == ref.gen &&
                  ref.gen == generation_ && // gen must also match current
                  // Issue #368: catch second-wrap false positives.
                  ref.wrap_epoch == wrap_epoch_.load(std::memory_order_relaxed);
        // Issue #1500: enforce cow_epoch unless pin_for_cow() allows
        // the ref to survive a lazy clone boundary.
        if (ok && !ref.boundary_pinned && ref.cow_epoch_at_capture != workspace_cow_epoch_) {
            ok = false;
        }
        if (!ok) {
            // Stale ref — bump the invalidations counter.
            // The whole point of StableNodeRef is to detect
            // this case; counting it tells us how often the
            // mechanism actually catches a stale handle.
            stable_ref_invalidations_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }

    // Issue #392: subtree-aware StableRef validity check.
    // Returns true when:
    //   - ref.id is in-bounds,
    //   - the slot has not been modified since capture
    //     (node_gen_[ref.id] == ref.gen), AND
    //   - the wrap epoch matches (no second-wrap false
    //     positives), AND
    //   - the top-level Define containing ref.id has not
    //     been scoped-bumped since capture
    //     (subtree_gen_[top_define_of(ref.id)] ==
    //      ref.subtree_gen_at_capture).
    //
    // This RELAXED check is the value-prop of #392: refs in
    // subtrees that were NOT touched by a scoped bump stay
    // valid even when other subtrees get bumped. Use this in
    // hot paths that hold many refs across long mutation
    // sequences (AI agent loops, EDA RTL/SV workspaces).
    //
    // Backward compatibility: the strict is_valid() above is
    // unchanged. Callers that don't capture the new
    // subtree_gen_at_capture field (default 0) still work
    // because subtree_gen_ defaults to 0 — the check accepts
    // them as long as no scoped bump has touched their
    // subtree.
    [[nodiscard]] bool is_valid_subtree(const StableNodeRef& ref) const noexcept {
        if (ref.id == NULL_NODE || ref.id >= tag_.size() || ref.id >= node_gen_.size())
            return false;
        if (node_gen_[ref.id] != ref.gen)
            return false;
        if (ref.wrap_epoch != wrap_epoch_.load(std::memory_order_relaxed))
            return false;
        // Subtree check: find the top-level Define ancestor
        // and compare subtree_gen_[T] against the captured
        // value. For nodes with no enclosing Define, top_define_of
        // returns NULL_NODE → we treat them as "no subtree
        // scoping" and accept (subtree_gen_ stays at 0).
        auto top = top_define_of(ref.id);
        if (top == NULL_NODE)
            return true; // no enclosing Define → can't be scope-invalidated
        if (top >= subtree_gen_.size())
            return true; // subtree_gen_ not populated → no scoped bump yet
        return subtree_gen_[top] == ref.subtree_gen_at_capture;
    }
    [[nodiscard]] std::optional<NodeView> get_safe(const StableNodeRef& ref) const noexcept
        post(r : !r.has_value() || is_valid(ref)) {
        if (!is_valid(ref))
            return std::nullopt;
        return get(ref.id);
    }

    // Issue #368: accessor for the current wrap_epoch_.
    // Returned by (ast:generation-stats) under the wrap-epoch key
    // so AI agents can checkpoint / compact before the next
    // generation_ wrap creates a wave of false-positive refs in
    // their long-running workspaces.
    [[nodiscard]] std::uint32_t wrap_epoch() const noexcept {
        return wrap_epoch_.load(std::memory_order_relaxed);
    }
    // Issue #369: structural-rollback counters.
    [[nodiscard]] std::uint64_t structural_rollback_success() const noexcept {
        return structural_rollback_success_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t structural_rollback_besteffort() const noexcept {
        return structural_rollback_besteffort_.load(std::memory_order_relaxed);
    }
    // Issue #1281: children_ PCV topology restores (snapshot path).
    [[nodiscard]] std::uint64_t children_topology_restore_count() const noexcept {
        return children_topology_restore_count_.load(std::memory_order_relaxed);
    }
    // Issue #1502: parent_ topology restores (snapshot or rebuild).
    [[nodiscard]] std::uint64_t parent_topology_restore_count() const noexcept {
        return parent_topology_restore_count_.load(std::memory_order_relaxed);
    }
    // Issue #1282: auto-restamp after generation wrap.
    [[nodiscard]] std::uint64_t auto_restamp_on_wrap_count() const noexcept {
        return auto_restamp_on_wrap_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool auto_restamp_pending() const noexcept {
        return auto_restamp_pending_.load(std::memory_order_relaxed);
    }
    // Issue #370: lifetime-safe view call counter.
    [[nodiscard]] std::uint64_t children_safe_view_count() const noexcept {
        return children_safe_view_count_.load(std::memory_order_relaxed);
    }
    // Issue #678: parent_safe_view call counter.
    [[nodiscard]] std::uint64_t parent_safe_view_count() const noexcept {
        return parent_safe_view_count_.load(std::memory_order_relaxed);
    }

    // Issue #191: bump the generation counter (with wrap-around
    // to 1, skipping 0 which is reserved). Called automatically
    // by structural mutation methods (set_child / insert_child /
    // remove_child) to invalidate all stale NodeIds in the
    // workspace. Also exposed publicly for the mutation
    // primitives to call after a non-SoA-mutating structural
    // change (e.g., a workspace-level COW swap).
    // Issue #273: refresh node_gen_ after a generation bump that does
    // not invalidate the whole FlatAST (e.g. hygienic macro rewrite).
    void restamp_all_node_generations() {
        // node_gen_==0 marks free-list slots, but live nodes at
        // generation_==0 also carry 0 — do not use is_free_slot here.
        std::vector<std::uint8_t> on_free(size(), 0);
        for (NodeId fid : free_list_) {
            if (fid < on_free.size())
                on_free[fid] = 1;
        }
        for (NodeId id = 0; id < size(); ++id) {
            if (!on_free[id] && id < node_gen_.size())
                node_gen_[id] = generation_;
        }
        // Issue #1282: if restamp was pending due to uint16 wrap,
        // clear the flag and count the recovery (Agent-visible via
        // ast:generation-stats / production-sweep-1281-1285-stats).
        if (auto_restamp_pending_.exchange(false, std::memory_order_relaxed)) {
            auto_restamp_on_wrap_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Issue #1282: if a generation wrap marked auto_restamp_pending_,
    // restamp live node_gen_ now. Safe to call from non-noexcept
    // paths (restore_children, mutation boundary exit).
    void maybe_auto_restamp_on_wrap() {
        if (!auto_restamp_pending_.load(std::memory_order_relaxed))
            return;
        restamp_all_node_generations();
    }

    // Refresh node_gen_ on one subtree (narrower than restamp_all).
    void restamp_subtree_generation(NodeId root) {
        if (root == NULL_NODE || root >= size())
            return;
        std::vector<NodeId> stack;
        stack.push_back(root);
        std::vector<std::uint8_t> seen(size(), 0);
        while (!stack.empty()) {
            auto id = stack.back();
            stack.pop_back();
            if (id == NULL_NODE || id >= size() || seen[id])
                continue;
            seen[id] = 1;
            if (id < node_gen_.size())
                node_gen_[id] = generation_;
            for (auto cid : children(id)) {
                if (cid != NULL_NODE)
                    stack.push_back(cid);
            }
        }
    }

    void bump_generation() noexcept post(generation_ != 0) {
        if (bump_generation_suppressed_) {
            // Issue #250: inside an atomic batch, individual
            // structural mutations (set_child / insert_child /
            // remove_child) skip the per-op generation bump.
            // The batch commits with a single bump at the end,
            // so the per-op bumps would be redundant.
            return;
        }
        ++generation_;
        if (generation_ == 0) {
            generation_ = 1;
            // Issue #457: detected a uint16_t wrap-around.
            // generation_ is uint16_t (1..65535) and we
            // wrap 65535 → 0 → 1. After 65K structural
            // mutations in the same FlatAST, every
            // outstanding StableNodeRef becomes invalid
            // (gen mismatch). Bump the wrap counter so
            // the AI Agent can (query:stable-ref-stats)
            // and decide whether to checkpoint / compact.
            generation_wrap_count_.fetch_add(1, std::memory_order_relaxed);
            // Issue #368: bump wrap_epoch_ so StableNodeRefs
            // captured before this wrap become invalid even
            // after the SECOND wrap returns generation_ to
            // its prior value (~130K mutates in).
            // uint32_t: ~2.6e14 mutates per wrap_epoch wrap.
            wrap_epoch_.fetch_add(1, std::memory_order_relaxed);
            // Issue #1282: schedule automatic restamp of live
            // node_gen_ (restamp itself allocates; cannot run in
            // this noexcept path). Consumed by maybe_auto_restamp_on_wrap
            // / restamp_all_node_generations on the next safe path.
            auto_restamp_pending_.store(true, std::memory_order_relaxed);
        }
        // Issue #255: only count actual bumps (suppressed
        // ones are accounted for via atomic_batch_commits_).
        bump_generation_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #392: scoped generation bumping for a single
    // top-level Define subtree. Walks up from subtree_root to
    // find the containing Define, then bumps that Define's
    // subtree_gen_ counter. ALSO bumps the global generation_
    // for backward compatibility with callers using the
    // existing is_valid() path (which checks global gen).
    //
    // The benefit for callers using the new
    // is_valid_subtree() path: refs to nodes in OTHER
    // subtrees stay valid because their subtree_gen_ was not
    // bumped. is_valid_subtree() compares the captured
    // subtree_gen_at_capture against the current subtree_gen_
    // for the top-level Define containing the ref's node.
    //
    // subtree_root must be a node inside the target subtree
    // (the walk-up handles arbitrary nesting). Pass NULL_NODE
    // to no-op safely.
    void bump_generation_subtree(NodeId subtree_root) noexcept {
        if (subtree_root == NULL_NODE || subtree_root >= size())
            return;
        auto top = top_define_of(subtree_root);
        if (top == NULL_NODE)
            return; // no enclosing Define → cannot scope
        // Lazily grow the per-node subtree_gen_ vector.
        if (subtree_gen_.size() < size())
            subtree_gen_.resize(size(), 0);
        // Bump the global generation_ so existing is_valid()
        // continues to behave as before (backward compat).
        bump_generation();
        // Advance the per-subtree counter for this top-level
        // Define. Same uint16_t wrap semantics as the global
        // generation_ (1..65535, skip 0, bump wrap_count_
        // + wrap_epoch_ on wrap).
        std::uint16_t& sg = subtree_gen_[top];
        ++sg;
        if (sg == 0) {
            sg = 1;
            subtree_bump_count_.fetch_add(1, std::memory_order_relaxed);
        }
        subtree_bump_count_.fetch_add(1, std::memory_order_relaxed);
        // Issue #392 fix: restamp node_gen_ for the entire
        // subtree so that refs captured AFTER the bump can
        // still pass is_valid_subtree() (which checks
        // node_gen_[id] == ref.gen for the slot, not just
        // the subtree counter). Without this, a ref captured
        // immediately after bump_generation_subtree(T) would
        // have ref.gen = new global gen, but node_gen_[N]
        // for N in T's subtree would still be the OLD
        // generation → slot check fails → ref is invalid
        // even though the subtree counter matches. The
        // restamp aligns node_gen_ with the new generation
        // so the slot check passes for fresh captures in
        // the bumped subtree.
        restamp_subtree_generation(subtree_root);
    }

    // Issue #392: read the current per-subtree generation for
    // the top-level Define containing subtree_root. Returns 0
    // if there is no enclosing Define (flat AST root with no
    // top-level Define) or if subtree_root is NULL/out-of-
    // bounds. Pair with bump_generation_subtree() and
    // is_valid_subtree() to build subtree-aware StableRef
    // invalidation logic.
    [[nodiscard]] std::uint16_t subtree_generation(NodeId subtree_root) const noexcept {
        if (subtree_root == NULL_NODE || subtree_root >= size())
            return 0;
        auto top = top_define_of(subtree_root);
        if (top == NULL_NODE || top >= subtree_gen_.size())
            return 0;
        return subtree_gen_[top];
    }

    // Issue #392: walk up the parent chain to find the
    // top-level Define root that contains `node`. Returns
    // NULL_NODE if there is no enclosing Define (e.g., the
    // AST root is a bare expression, not wrapped in Define).
    // The walk is bounded by AST depth — O(depth) per call.
    // Callers that need O(1) lookup should cache the result
    // (e.g., add a side-vector `top_define_of_[id]`).
    [[nodiscard]] NodeId top_define_of(NodeId node) const noexcept {
        if (node == NULL_NODE || node >= size())
            return NULL_NODE;
        NodeId cur = node;
        std::uint32_t safety = 0;
        while (cur != NULL_NODE && cur < size()) {
            if (cur < tag_.size() && tag_[cur] == NodeTag::Define)
                return cur;
            auto parent = (cur < parent_.size()) ? parent_[cur] : NULL_NODE;
            if (parent == NULL_NODE)
                return NULL_NODE;
            cur = parent;
            if (++safety > 1000000) // cycle guard
                return NULL_NODE;
        }
        return NULL_NODE;
    }

    // Issue #392: lifetime total of bump_generation_subtree()
    // calls (excludes the no-op when subtree_root has no
    // enclosing Define). Read via the
    // (compile:subtree-bump-count) Aura primitive or
    // directly from the compiler service metrics.
    [[nodiscard]] std::uint64_t subtree_bump_count() const noexcept {
        return subtree_bump_count_.load(std::memory_order_relaxed);
    }

    // Issue #250: atomic-batch API. When begin_atomic_batch()
    // is active, bump_generation() and mark_dirty_upward are
    // suppressed for individual structural mutates, and
    // accumulated for a single end-of-batch commit. The batch
    // holds the structural_mtx_ exclusive lock for its entire
    // lifetime (via the RAII guard).
    //
    // begin_atomic_batch() / commit_atomic_batch() / rollback_atomic_batch()
    // are intended to be called from mutate:atomic-batch
    // (and any future caller that wants true all-or-nothing
    // semantics for a multi-step mutation).
    //
    // IMPORTANT: begin_atomic_batch() must be called while
    // already holding structural_mtx_ (e.g., from the outer
    // MutationBoundaryGuard). The batch guard does NOT acquire
    // the lock itself — the caller's existing lock is
    // extended to cover the entire batch. This is the
    // standard pattern: caller acquires the lock once, then
    // does many mutations under it.
    void begin_atomic_batch() noexcept {
        bump_generation_suppressed_ = true;
        atomic_batch_bumps_saved_ = 0; // reset counter for this batch
        // Issue #1893: always capture hygiene/dirty metadata at batch
        // open so rollback restores MacroIntroduced + provenance even
        // without :snapshot? #t full-flat deep-copy.
        atomic_batch_meta_snap_ = snapshot_metadata_columns();
        atomic_batch_meta_snap_valid_ = true;
        atomic_batch_metadata_captured_total_.fetch_add(1, std::memory_order_relaxed);
    }

    // Commit the batch. Calls bump_generation() once, marks
    // all batched nodes dirty, releases the suppression.
    void commit_atomic_batch() noexcept {
        bump_generation_suppressed_ = false;
        // Discard metadata snapshot (committed state keeps live columns).
        atomic_batch_meta_snap_valid_ = false;
        atomic_batch_meta_snap_ = MetadataColumnsSnapshot{};
        ++generation_;
        if (generation_ == 0)
            generation_ = 1;
        // Note: we don't have a per-batch dirty list (would
        // require tracking all touched nodes in the lockless
        // mutates). The first post-commit structural mutate
        // will trigger the regular mark_dirty_upward path.
        // The single bump + single future dirty sweep is
        // still much cheaper than N bumps + N dirty sweeps.
        // Issue #255: bump the actual gen + the batch counter.
        // commit_atomic_batch() does its own generation bump
        // (not via bump_generation() — that would respect the
        // suppression flag we just cleared, and we want the
        // bump to happen unconditionally). Count it here.
        bump_generation_count_.fetch_add(1, std::memory_order_relaxed);
        atomic_batch_commits_.fetch_add(1, std::memory_order_relaxed);
        // Issue #1348: long-run soft compaction policy — when the
        // free_list_ grows past the threshold (or live size exceeds
        // the warn cap), recycle dead slots without invalidating
        // StableNodeRefs (compact_nodes_soft does not remap ids).
        if (free_list_.size() >= compaction_free_list_threshold_ ||
            size() >= max_live_nodes_warn_) {
            (void)compact_nodes_soft();
            auto_compact_on_commit_count_.fetch_add(1, std::memory_order_relaxed);
            if (size() >= max_live_nodes_warn_)
                live_nodes_threshold_warn_count_.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #1371: after batch commit, refresh tag_arity
        // index via delta when only appends occurred; otherwise
        // leave dirty for ensure_tag_arity_index on next query.
        if (tag_arity_index_dirty_.load(std::memory_order_acquire)) {
            const std::size_t n = size();
            if (!tag_arity_index_.empty() && n > tag_arity_index_built_size_) {
                rebuild_tag_arity_index_delta(static_cast<NodeId>(tag_arity_index_built_size_),
                                              static_cast<NodeId>(n));
            }
            // else: in-place mutates or empty index — keep dirty
            // so ensure_tag_arity_index() full-rebuilds later.
        }
    }

    // Roll back the batch. No bump (the changes were never
    // visible — gen didn't change). Releases the suppression.
    // Issue #1893: restore marker/provenance/dirty metadata snapshotted
    // at begin_atomic_batch so MacroIntroduced protection is not lost.
    void rollback_atomic_batch() noexcept {
        bump_generation_suppressed_ = false;
        // No generation bump. The lockless helper's own
        // rollback_since() has already reverted the
        // mutation_log_ entries; readers holding the
        // pre-batch generation_ see no structural change.
        if (atomic_batch_meta_snap_valid_) {
            restore_metadata_columns(std::move(atomic_batch_meta_snap_));
            atomic_batch_meta_snap_valid_ = false;
            atomic_batch_metadata_restored_total_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t atomic_batch_metadata_restored_total() const noexcept {
        return atomic_batch_metadata_restored_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t atomic_batch_metadata_captured_total() const noexcept {
        return atomic_batch_metadata_captured_total_.load(std::memory_order_relaxed);
    }

    // Issue #250: how many generation bumps the latest batch
    // saved by suppressing per-op bumps. Updated on each
    // commit. Exposed via observability snapshot.
    std::uint64_t atomic_batch_bumps_saved() const noexcept { return atomic_batch_bumps_saved_; }
    [[nodiscard]] std::uint64_t atomic_batch_commits() const noexcept {
        return atomic_batch_commits_.load(std::memory_order_relaxed);
    }

    // True iff an atomic batch is active. Used by
    // mark_dirty_upward to skip dirty propagation during
    // a batch (the commit path handles it).
    bool atomic_batch_active() const noexcept { return bump_generation_suppressed_; }
    // Issue #255: reference stability observability accessors.
    // Read by CompilerService::snapshot() (service.ixx) to
    // accumulate into CompilerMetrics for the
    // (compile:invalidations-stats) Aura primitive and the
    // --evo-explain output.
    std::uint64_t bump_generation_count() const noexcept {
        return bump_generation_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t is_valid_check_count() const noexcept {
        return is_valid_check_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t stable_ref_invalidations() const noexcept {
        return stable_ref_invalidations_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t workspace_cow_epoch() const noexcept {
        return workspace_cow_epoch_;
    }
    void set_workspace_cow_epoch(std::uint64_t epoch) noexcept { workspace_cow_epoch_ = epoch; }
    [[nodiscard]] std::uint64_t pinned_across_boundaries() const noexcept {
        return pinned_across_boundaries_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t cross_boundary_validations() const noexcept {
        return cross_boundary_validations_.load(std::memory_order_relaxed);
    }
    void bump_pinned_across_boundaries() const noexcept {
        pinned_across_boundaries_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1406: counter for pins dropped by the bounded-retention
    // cap in Evaluator::pin_stable_ref_for_cow_boundary. Observability
    // only — not a correctness signal (dropped pins fall back to
    // is_valid() per the cap-policy docstring).
    void bump_pinned_across_boundaries_dropped() const noexcept {
        pinned_across_boundaries_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_cross_boundary_validations() const noexcept {
        cross_boundary_validations_.fetch_add(1, std::memory_order_relaxed);
    }
    void reset_boundary_observability_counters() noexcept {
        pinned_across_boundaries_.store(0, std::memory_order_relaxed);
        cross_boundary_validations_.store(0, std::memory_order_relaxed);
    }
    // Issue #457: generation_ / node_gen_ lifecycle
    // observability accessors. Public so the
    // (query:stable-ref-stats) primitive can read them
    // from evaluator_primitives_query.cpp.
    [[nodiscard]] std::uint64_t generation_wrap_count() const noexcept {
        return generation_wrap_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t node_gen_stale_access_count() const noexcept {
        return node_gen_stale_access_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint16_t current_generation() const noexcept { return generation_; }
    std::uint64_t atomic_batch_commits_v() const noexcept {
        return atomic_batch_commits_.load(std::memory_order_relaxed);
    }
    // Issue #256: AST operation observability accessors.
    // Read by CompilerService::snapshot() to accumulate into
    // CompilerMetrics for the (compile:ast-ops-stats) Aura
    // primitive.
    std::uint64_t children_call_count() const noexcept {
        return children_call_count_.load(std::memory_order_relaxed);
    }
    // Issue #1319
    std::uint64_t structural_mutate_insert_total() const noexcept {
        return structural_mutate_insert_total_.load(std::memory_order_relaxed);
    }
    std::uint64_t structural_mutate_erase_total() const noexcept {
        return structural_mutate_erase_total_.load(std::memory_order_relaxed);
    }
    std::uint64_t parent_of_call_count() const noexcept {
        return parent_of_call_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t mark_dirty_upward_call_count() const noexcept {
        return mark_dirty_upward_call_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t mark_dirty_total_nodes() const noexcept {
        return mark_dirty_total_nodes_.load(std::memory_order_relaxed);
    }
    // Issue #471: SV-scale dirty propagation observability
    // (returns the # of times the mark_dirty_upward_fast
    // early-exit fixed-point check fired + the max depth
    // observed across all mark_dirty_upward(_fast) calls).
    std::uint64_t mark_dirty_early_exit_count() const noexcept {
        return mark_dirty_early_exit_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t mark_dirty_max_depth_observed() const noexcept {
        return mark_dirty_max_depth_observed_.load(std::memory_order_relaxed);
    }
    // Issue #1251: dirty propagation bound truncations + rollback compaction.
    [[nodiscard]] std::uint64_t mark_dirty_truncated_count() const noexcept {
        return mark_dirty_truncated_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t rollback_compaction_triggered() const noexcept {
        return rollback_compaction_triggered_.load(std::memory_order_relaxed);
    }

    // Issue #275: std::expected rollback entry point.
    [[nodiscard]] std::expected<void, MutationError> try_rollback_record(MutationRecord& rec) {
        if (auto valid = mutation::validate_rollback_record(rec, tag_.size()); !valid)
            return valid;
        auto kind = mutation::classify_rollback(rec);
        if (!kind)
            return std::unexpected(kind.error());

        switch (*kind) {
            case RollbackKind::SubtreeMark:
                rec.status = MutationStatus::RolledBack;
                bump_generation_on_rollback();
                if (rec.parent_id < tag_.size())
                    mark_dirty_upward(rec.parent_id);
                return {};

            case RollbackKind::Structural:
                return try_rollback_structural_child_op(rec);

            // Issue #1441: mutate:rebind — restore Define body from rec.old_value.
            case RollbackKind::Rebind:
                return try_rollback_rebind_op(rec);

            case RollbackKind::ScalarInt:
                if (rec.target_node >= int_val_.size())
                    return std::unexpected(MutationError::OutOfRange);
                int_val_[rec.target_node] = mutation::scalar_int_old_value(rec);
                break;
            case RollbackKind::ScalarTypeId:
                if (rec.target_node >= type_id_.size())
                    return std::unexpected(MutationError::OutOfRange);
                type_id_[rec.target_node] = mutation::scalar_type_old_value(rec);
                break;
            case RollbackKind::ScalarSymId:
                if (rec.target_node >= sym_id_.size())
                    return std::unexpected(MutationError::OutOfRange);
                sym_id_[rec.target_node] = mutation::scalar_sym_old_value(rec);
                break;
            case RollbackKind::ScalarFloat:
                if (rec.target_node >= float_val_.size())
                    return std::unexpected(MutationError::OutOfRange);
                float_val_[rec.target_node] = mutation::scalar_float_old_value(rec);
                break;
        }

        rec.status = MutationStatus::RolledBack;
        bump_generation_on_rollback();
        return {};
    }

    bool rollback(std::uint64_t mutation_id) pre(mutation_id != 0) {
        for (auto& rec : mutation_log_) {
            if (rec.mutation_id == mutation_id)
                return try_rollback_record(rec).has_value();
        }
        return false;
    }

private:
    // When true, bump_generation_on_rollback only advances generation_
    // and defers restamp_all_node_generations to the batch end
    // (rollback_since). Avoids O(records × |flat|) restamp storms that
    // timed out jit_late1/late3 (600s) after #1441.
    bool defer_rollback_restamp_ = false;

    void bump_generation_on_rollback() {
        ++generation_;
        if (generation_ == 0)
            generation_ = 1;
        if (!defer_rollback_restamp_)
            restamp_all_node_generations();
    }

    [[nodiscard]] std::expected<void, MutationError>
    try_rollback_structural_child_op(MutationRecord& rec) {
        auto op = mutation::structural_rollback_op(rec.operator_name);
        if (!op)
            return std::unexpected(op.error());

        NodeId parent = rec.target_node;
        if (parent >= children_.size())
            return std::unexpected(MutationError::InvalidParent);

        const auto idx = rec.field_offset;
        const auto old_child = static_cast<NodeId>(rec.old_value);
        const auto new_child = static_cast<NodeId>(rec.new_value);
        auto& list = children_[parent];

        switch (*op) {
            case StructuralRollbackOp::SetChild:
                if (idx >= list.size())
                    return std::unexpected(MutationError::OutOfRange);
                if (new_child != NULL_NODE && new_child < parent_.size())
                    parent_[new_child] = NULL_NODE;
                children_[parent] = list.with_set(idx, old_child);
                if (old_child != NULL_NODE && old_child < parent_.size())
                    parent_[old_child] = parent;
                break;
            case StructuralRollbackOp::InsertChild:
                if (idx > list.size())
                    return std::unexpected(MutationError::OutOfRange);
                if (new_child != NULL_NODE && new_child < parent_.size())
                    parent_[new_child] = NULL_NODE;
                children_[parent] = list.with_erase(idx);
                break;
            case StructuralRollbackOp::RemoveChild:
                if (idx > list.size())
                    return std::unexpected(MutationError::OutOfRange);
                children_[parent] = list.with_insert(idx, old_child);
                if (old_child != NULL_NODE && old_child < parent_.size())
                    parent_[old_child] = parent;
                break;
        }

        rec.status = MutationStatus::RolledBack;
        bump_generation_on_rollback(); // restamp unless deferred by rollback_since
        mark_dirty_upward(parent);
        // Issue #369: bump the per-category counter. Success
        // means the children_ column was restored via the
        // structural op (parent, child_idx, old_child, new_child
        // all valid).
        structural_rollback_success_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    // Issue #1441: variable-state rollback for mutate:rebind.
    // rec.target_node = Define node; rec.old_value / rec.new_value = body NodeIds;
    // rec.field_offset = body child index (0). Restores children_[define][idx]
    // and parent_ links, marks RolledBack.
    [[nodiscard]] std::expected<void, MutationError> try_rollback_rebind_op(MutationRecord& rec) {
        if (!rec.has_rollback_data)
            return std::unexpected(MutationError::NoRollbackData);
        if (rec.operator_name != "rebind" && rec.operator_name != "batch-rebind")
            return std::unexpected(MutationError::UnknownStructuralOp);

        const NodeId define_node = rec.target_node;
        if (define_node >= children_.size() || define_node >= tag_.size())
            return std::unexpected(MutationError::InvalidTarget);

        const auto idx = rec.field_offset; // body slot (normally 0)
        const auto old_child = static_cast<NodeId>(rec.old_value);
        const auto new_child = static_cast<NodeId>(rec.new_value);
        auto& list = children_[define_node];
        if (idx >= list.size())
            return std::unexpected(MutationError::OutOfRange);

        // Inverse of set_child: detach new body, reattach old body.
        if (new_child != NULL_NODE && new_child < parent_.size())
            parent_[new_child] = NULL_NODE;
        children_[define_node] = list.with_set(idx, old_child);
        if (old_child != NULL_NODE && old_child < parent_.size())
            parent_[old_child] = define_node;

        rec.status = MutationStatus::RolledBack;
        bump_generation_on_rollback(); // restamp unless deferred by rollback_since
        mark_dirty_upward(define_node);
        return {};
    }

public:
    // Rollback all mutations since (and including) the given ID.
    // Defers restamp_all_node_generations to a single pass after the
    // reverse walk (Issue #1441 + jit_late1/late3 timeout fix).
    std::size_t rollback_since(std::uint64_t since_id) {
        std::size_t count = 0;
        const bool prev_defer = defer_rollback_restamp_;
        defer_rollback_restamp_ = true;
        for (auto it = mutation_log_.rbegin(); it != mutation_log_.rend(); ++it) {
            if (it->mutation_id >= since_id && it->status == MutationStatus::Committed) {
                if (rollback(it->mutation_id))
                    ++count;
            }
        }
        defer_rollback_restamp_ = prev_defer;
        if (count > 0)
            restamp_all_node_generations();
        return count;
    }

    // Issue #213 Cycle 1: rollback all mutations appended to
    // the log after `checkpoint_size` (i.e. the log size at
    // boundary entry). Walks the log in reverse from the end
    // down to the checkpoint, calling `rollback` on each
    // committed record. Returns the number of records that
    // were successfully rolled back.
    //
    // Why size-based and not id-based: the log is append-only,
    // so the log size at boundary entry is a stable handle.
    // A mid-mutation `mutation_id` could be re-used in the
    // future (after wrap-around at uint64_t max), but the
    // log size is monotonically non-decreasing within a
    // session.
    std::size_t rollback_to_size(std::size_t checkpoint_size)
        // Issue #213 follow-up: C++26 contract. The function
        // is total — handles any checkpoint_size (including
        // past the log end, in which case it's a no-op). The
        // contract documents the semantic: result count
        // is 0 when checkpoint_size >= log.size().
        pre(true) {
        if (mutation_log_.size() <= checkpoint_size)
            return 0;
        // Issue #1251: before replaying a large mutation log, soft-
        // compact dead node slots to bound memory growth under long
        // AI multi-round edit sessions.
        static constexpr std::size_t kRollbackCompactionLogThreshold = 10'000;
        if (mutation_log_.size() > kRollbackCompactionLogThreshold) {
            if (compact_nodes_soft() > 0)
                rollback_compaction_triggered_.fetch_add(1, std::memory_order_relaxed);
            else
                // Still record the decision point so Agents see the
                // hot path even when free_list_ had nothing to recycle.
                rollback_compaction_triggered_.fetch_add(1, std::memory_order_relaxed);
        }
        std::size_t count = 0;
        // Same restamp deferral as rollback_since (jit_late timeout fix).
        const bool prev_defer = defer_rollback_restamp_;
        defer_rollback_restamp_ = true;
        for (std::size_t i = mutation_log_.size(); i > checkpoint_size; --i) {
            auto& rec = mutation_log_[i - 1];
            if (rec.status == MutationStatus::Committed) {
                if (rollback(rec.mutation_id))
                    ++count;
            }
        }
        defer_rollback_restamp_ = prev_defer;
        if (count > 0)
            restamp_all_node_generations();
        // Issue #1301 (P1) + #213: keep RolledBack records for audit
        // by default (status already set by rollback()). Only truncate
        // the rolled-back suffix when the log is huge so long AI
        // sessions cannot OOM. Small rollbacks must retain entries —
        // test_issue_213 and tooling inspect RolledBack status in-place.
        static constexpr std::size_t kMutationLogTruncateThreshold = 10'000;
        if (mutation_log_.size() > checkpoint_size &&
            mutation_log_.size() > kMutationLogTruncateThreshold) {
            const std::size_t dropped = mutation_log_.size() - checkpoint_size;
            mutation_log_.resize(checkpoint_size);
            mutation_log_compacted_records_.fetch_add(dropped, std::memory_order_relaxed);
            mutation_log_compact_ops_.fetch_add(1, std::memory_order_relaxed);
        }
        return count;
    }

    [[nodiscard]] std::uint64_t mutation_log_compacted_records() const noexcept {
        return mutation_log_compacted_records_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t mutation_log_compact_ops() const noexcept {
        return mutation_log_compact_ops_.load(std::memory_order_relaxed);
    }

    // Issue #1362: drop old Committed records so long-running agents
    // do not accumulate ~200 bytes/mutation forever. keep_recent tail
    // is always retained. When keep_all_rolledback is true, older
    // RolledBack records are also retained (diagnostics for #1299).
    // Returns number of records dropped. Idempotent when log is small.
    // keep_recent bounds retained tail; keep_all_rolledback retains
    // older RolledBack records for diagnostics. No default args —
    // the zero-arg compact_mutation_log() is shrink_to_fit (bytes).
    // Callers that want record drop must pass keep_recent explicitly
    // (typical keep_recent=1000). Avoids overload ambiguity with the
    // no-arg shrink_to_fit overload (#1638 / rebuild with #1747).
    [[nodiscard]] std::size_t compact_mutation_log(std::size_t keep_recent,
                                                   bool keep_all_rolledback) {
        if (mutation_log_.size() <= keep_recent)
            return 0;
        const std::size_t drop_to = mutation_log_.size() - keep_recent;
        std::size_t dropped = 0;
        std::pmr::vector<MutationRecord> kept{mutation_log_.get_allocator()};
        kept.reserve(mutation_log_.size());
        if (keep_all_rolledback) {
            for (std::size_t i = 0; i < drop_to; ++i) {
                if (mutation_log_[i].status == MutationStatus::RolledBack)
                    kept.push_back(std::move(mutation_log_[i]));
                else
                    ++dropped;
            }
            for (std::size_t i = drop_to; i < mutation_log_.size(); ++i)
                kept.push_back(std::move(mutation_log_[i]));
        } else {
            for (std::size_t i = drop_to; i < mutation_log_.size(); ++i)
                kept.push_back(std::move(mutation_log_[i]));
            dropped = drop_to;
        }
        mutation_log_ = std::move(kept);
        // Rebuild node_first_mutation_ indices (old offsets are invalid).
        std::fill(node_first_mutation_.begin(), node_first_mutation_.end(), 0);
        for (std::size_t i = 0; i < mutation_log_.size(); ++i) {
            const NodeId n = mutation_log_[i].target_node;
            if (n < node_first_mutation_.size() && node_first_mutation_[n] == 0)
                node_first_mutation_[n] = static_cast<std::uint32_t>(i + 1);
        }
        if (dropped > 0) {
            mutation_log_compacted_records_.fetch_add(dropped, std::memory_order_relaxed);
            mutation_log_compact_ops_.fetch_add(1, std::memory_order_relaxed);
        }
        return dropped;
    }

    // Issue #1362: auto-trigger threshold (log size) — public for tests/tuning.
    static constexpr std::size_t kMutationLogAutoCompactThreshold = 10'000;
    static constexpr std::size_t kMutationLogAutoCompactKeepRecent = 1000;

    // Issue #1362: auto-compact when log exceeds threshold (called after append).
    void maybe_auto_compact_mutation_log() {
        if (mutation_log_.size() > kMutationLogAutoCompactThreshold) {
            (void)compact_mutation_log(kMutationLogAutoCompactKeepRecent,
                                       /*keep_all_rolledback=*/true);
        }
    }

    // ── Type ID access ─────────────────────────────────────────

    // Issue #1620: FlatAST type column hot accessor — bounds-safe,
    // records invariant hit when column is live (mutation reval path).
    std::uint32_t type_id(NodeId id) const {
        if (id < type_id_.size()) {
            aura::core::cpp26::record_hotpath_invariant_hit();
            contract_assert(id < tag_.size() || tag_.empty());
            return type_id_[id];
        }
        return 0;
    }

    void set_type(NodeId id, std::uint32_t tid) {
        if (id < type_id_.size()) {
            // Issue #1620: type stamp hot path (typed mutation reval).
            aura::core::cpp26::record_hotpath_invariant_hit();
            contract_assert(id < tag_.size());
            type_id_[id] = tid;
        }
        // Issue #412: stamp the cache entry with the current
        // generation. The cache hit path compares this against
        // type_cache_generation() — if they match, the entry
        // is still valid (no structural mutation has
        // invalidated it since it was populated). Without this
        // stamp, the gen check would reject every entry (gen
        // would always be 0 in the cache vs. non-zero current).
        if (id < type_cache_gen_.size())
            type_cache_gen_[id] = type_cache_generation_.load(std::memory_order_relaxed);
    }

    // Issue #412: type cache generation accessors. The gen
    // is bumped on every mark_dirty_upward() call (and via
    // the explicit bump_type_cache_generation() accessor for
    // structural changes that don't go through the dirty
    // path, e.g., a new define in a re-eval). Cache hit
    // path reads type_cache_generation() and compares
    // against the per-node value stored at cache time.
    std::uint32_t type_cache_generation() const {
        return type_cache_generation_.load(std::memory_order_relaxed);
    }
    void bump_type_cache_generation() {
        type_cache_generation_.fetch_add(1, std::memory_order_relaxed);
    }
    // Per-node cache gen accessor (read / write). Mirrors
    // type_id() / set_type(). set_type_with_gen() is the
    // canonical call site for the type-checker — it stores
    // both the type and the current gen atomically.
    std::uint32_t type_cache_gen(NodeId id) const {
        return id < type_cache_gen_.size() ? type_cache_gen_[id] : 0;
    }
    void set_type_with_gen(NodeId id, std::uint32_t tid, std::uint32_t gen) {
        if (id >= type_id_.size() || id >= type_cache_gen_.size())
            return;
        type_id_[id] = tid;
        type_cache_gen_[id] = gen;
    }

    // Issue #412 follow-up #1: per-binding type cache
    // generation accessors. Each binding (identified by
    // SymId) has its own gen counter that bumps only
    // when THAT specific binding's structure changes
    // (e.g. mutate:rebind on a top-level define). The
    // per-binding gen is finer-grained than the global
    // type_cache_generation_ (which bumps on every
    // dirty event): bumping on every dirty event would
    // be over-invalidating. The per-binding gen
    // rescues cache entries that don't depend on the
    // mutated binding from being re-inferred.
    //
    // The map is a per-FlatAST map from SymId to atomic
    // gen. The type-checker reads the gen when
    // populating a cache entry (alongside the global
    // gen) and on cache hit to validate. Bumps happen
    // when a binding's structure actually changes
    // (mutate:* primitives that target a Define / Let /
    // LetRec node).
    //
    // Returns 0 for bindings that haven't been touched
    // yet (the default-constructed gen). The 0 sentinel
    // matches the global gen's pre-#412 behavior (0 =
    // no cache entry yet).
    std::uint32_t binding_gen(SymId sym) const {
        if (!binding_gens_)
            return 0;
        auto it = binding_gens_->gens.find(sym);
        if (it == binding_gens_->gens.end())
            return 0;
        return it->second;
    }
    void bump_binding_gen(SymId sym) {
        if (!binding_gens_)
            binding_gens_ = std::make_shared<BindingGenMap>();
        binding_gens_->gens[sym]++;
        binding_gen_bumps_total_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #412 follow-up #1: per-binding gen bump
    // counter accessor. Returns the lifetime total of
    // per-binding gen bumps (one per call to
    // bump_binding_gen). Plumbed to CompilerMetrics via
    // the snapshot for observability.
    std::uint64_t binding_gen_bumps_total() const {
        return binding_gen_bumps_total_.load(std::memory_order_relaxed);
    }
    // Issue #390: per-node schema cache accessors.
    // schema_cache(id) returns the cached schema for
    // node id (0 = no schema, the type checker
    // will infer normally). set_schema_cache(id, tid)
    // sets the schema. The type-checker's cache hit
    // path consults this column (alongside type_id_
    // and type_cache_gen_) to short-circuit
    // re-inference for macro-introduced nodes.
    std::uint32_t schema_cache(aura::ast::NodeId id) const {
        return id < schema_cache_.size() ? schema_cache_[id] : 0;
    }
    void set_schema_cache(aura::ast::NodeId id, std::uint32_t tid) {
        if (id >= schema_cache_.size())
            schema_cache_.resize(id + 1, 0);
        schema_cache_[id] = tid;
    }
    // Issue #413: invalidation trace accessors. The
    // invalidation_trace_ vector grows by one entry per
    // per-binding gen bump (one per mark_dirty_upward on
    // a binding node with valid sym_id). Cleared on full
    // FlatAST reset, same lifecycle as mutation_log_.
    // size_invalidations() returns the lifetime total.
    // last_invalidation_for(mutation_id) returns the
    // most recent InvalidationRecord for that mutation
    // (a single mutation may invalidate multiple bindings
    // — the trace keeps all of them).
    std::size_t invalidation_trace_size() const { return invalidation_trace_.size(); }
    std::optional<InvalidationRecord> last_invalidation_for(std::uint64_t mutation_id) const {
        for (auto it = invalidation_trace_.rbegin(); it != invalidation_trace_.rend(); ++it) {
            if (it->mutation_id == mutation_id)
                return *it;
        }
        return std::nullopt;
    }
    // Plumbed to CompilerMetrics via the snapshot for
    // observability. Counter incremented on every
    // per-binding gen bump that gets traced.
    mutable std::atomic<std::uint64_t> invalidation_trace_records_total_{0};
    // Counter accessor.
    std::uint64_t invalidation_trace_records_total() const {
        return invalidation_trace_records_total_.load(std::memory_order_relaxed);
    }
    // Per-node cache gen accessor for the BINDING the
    // cache entry is for. 0 = no binding context.
    // Mirrors type_cache_gen() / type_cache_binding_gen_
    // column.
    std::uint32_t type_cache_binding_gen(NodeId id) const {
        return id < type_cache_binding_gen_.size() ? type_cache_binding_gen_[id] : 0;
    }
    // The canonical call site for the type-checker when
    // populating a cache entry for a Variable node (or
    // any node whose type depends on a specific binding).
    // Stores the type, the global gen, and the per-binding
    // gen. On cache hit, the check is: global gen matches
    // AND per-binding gen matches → entry is fresh.
    void set_type_with_binding_gen(NodeId id, std::uint32_t tid, std::uint32_t global_gen,
                                   std::uint32_t binding_gen_val) {
        if (id >= type_id_.size() || id >= type_cache_gen_.size() ||
            id >= type_cache_binding_gen_.size())
            return;
        type_id_[id] = tid;
        type_cache_gen_[id] = global_gen;
        type_cache_binding_gen_[id] = binding_gen_val;
    }

    // ── Error kind access (Issue #79) ────────────────────────
    // 0 = no error, non-zero = ErrorKind enum value. Lets the type-checker
    // and runtime evaluator tag the offending node so AuraQuery
    // `(has-error? N)` can find it without scanning all diagnostics.
    std::uint8_t node_error(NodeId id) const {
        return id < error_kind_.size() ? error_kind_[id] : 0;
    }
    void set_node_error(NodeId id, std::uint8_t kind) {
        if (id < error_kind_.size())
            error_kind_[id] = kind;
    }
    void clear_node_error(NodeId id) {
        if (id < error_kind_.size())
            error_kind_[id] = 0;
    }

    void set_int(NodeId id, std::int64_t val)
        // Issue #144: id must be a valid node. Without this,
        // a stale NodeId from a previous generation would
        // silently no-op (the size check below) and the mutation
        // would vanish without a diagnostic.
        pre(id < int_val_.size()) {
        int_val_[id] = val;
    }
    void set_float(NodeId id, double val) pre(id < float_val_.size()) { float_val_[id] = val; }
    void set_sym(NodeId id, SymId val) pre(id < sym_id_.size()) { sym_id_[id] = val; }

    // Capability require count for DefineModule nodes
    std::uint32_t cap_require_count(NodeId id) const {
        return id < cap_require_count_.size() ? cap_require_count_[id] : 0;
    }

    // Access a param by apparent index across the combined param+cap_require storage
    SymId param_at(NodeId id, std::uint32_t idx) const {
        if (idx < param_count_[id] + cap_require_count_[id] &&
            param_begin_[id] + idx < param_data_.size())
            return param_data_[param_begin_[id] + idx];
        return INVALID_SYM;
    }

    // Set a param by index. Used by mutate:rename-symbol to rename
    // Lambda parameters (whose symbols live in param_data_, not
    // sym_id_). Issue #139.
    void set_param_at(NodeId id, std::uint32_t idx, SymId val) {
        if (idx < param_count_[id] + cap_require_count_[id] &&
            param_begin_[id] + idx < param_data_.size())
            param_data_[param_begin_[id] + idx] = val;
    }

    // Issue #139: convenience wrapper that iterates the params of a
    // Lambda (or any node with params) and replaces any param with
    // sym_id matching `oldsym` to `newsym`. Returns the count of
    // replacements. Used by mutate:rename-symbol so the param list
    // of `(lambda (x) ...)` is updated when the caller renames `x`.
    template <typename Callback>
    std::size_t rename_param(NodeId id, SymId oldsym, SymId newsym, Callback next_idx) {
        if (id >= param_count_.size() || id >= param_begin_.size())
            return 0;
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < param_count_[id] + cap_require_count_[id]; ++i) {
            if (param_data_[param_begin_[id] + i] == oldsym) {
                param_data_[param_begin_[id] + i] = newsym;
                ++n;
                if constexpr (!std::is_null_pointer_v<Callback>)
                    next_idx(i);
            }
        }
        return n;
    }

    // Resolve type names → TypeIds for all TypeAnnotation nodes
    // Requires TypeRegistry for name resolution
    void resolve_type_ids(class aura::core::TypeRegistry& reg, StringPool& pool);

    // Issue #249: stable child / parent accessors. These return
    // StableNodeRef (a NodeId + generation pair) so the caller
    // can safely hold the result across structural mutations.
    // If a structural mutation happens after capturing, the
    // ref's generation no longer matches FlatAST::generation_,
    // and is_valid(ref) returns false. This is the recommended
    // way to use NodeIds in EDSL / query / mutate code that
    // may span multiple mutating calls.
    // Issue #1500: return full provenance via make_ref (wrap_epoch /
    // cow_epoch / mutation_id / subtree_gen), not brace-init {id, gen}
    // which left wrap_epoch/cow_epoch at 0 and skipped #368/#738 checks.
    [[nodiscard]] StableNodeRef parent_stable(NodeId id) const noexcept {
        if (id >= parent_.size())
            return StableNodeRef{};
        auto pid = parent_[id];
        if (pid == NULL_NODE)
            return StableNodeRef{};
        return make_ref(pid);
    }

    // Issue #678: generation-tagged parent accessor for query
    // paths that may span structural mutations. Unlike parent_of()
    // (scalar NodeId), the returned StableNodeRef can be validated
    // via is_valid() after a concurrent mutate.
    [[nodiscard]] StableNodeRef parent_safe_view(NodeId id) const noexcept {
        parent_safe_view_count_.fetch_add(1, std::memory_order_relaxed);
        return parent_stable(id);
    }

    // Issue #1500: full-provenance StableNodeRef per child (make_ref).
    [[nodiscard]] std::vector<StableNodeRef> children_stable(NodeId id) const {
        std::vector<StableNodeRef> out;
        if (id >= children_.size())
            return out;
        const auto& pcv = children_[id];
        out.reserve(pcv.size());
        for (std::size_t i = 0; i < pcv.size(); ++i) {
            auto cid = pcv[i];
            if (cid == NULL_NODE)
                continue;
            out.push_back(make_ref(cid));
        }
        return out;
    }

    // Issue #1912: children_stable with boundary_pinned=true on each ref.
    // FlatAST-only: marks the flag; Evaluator::children_stable_batch also
    // registry-pins for steal/Guard survival. Prefer the Evaluator API in
    // AI multi-round loops that cross COW / fiber boundaries.
    [[nodiscard]] std::vector<StableNodeRef> children_stable_batch(NodeId id) const {
        auto out = children_stable(id);
        for (auto& r : out)
            r.pin_for_cow();
        return out;
    }

    // Issue #1912: batch refresh_if_stale over an arbitrary span of refs.
    // Returns count of successfully validated/refreshed refs. Does not
    // touch Evaluator pin registries — use Evaluator::refresh_stable_refs_batch
    // when auto_pin / metrics / COW-boundary registry are required.
    std::size_t refresh_stable_refs_batch(std::span<StableNodeRef> refs) noexcept {
        std::size_t n = 0;
        for (auto& r : refs) {
            if (r.refresh_if_stale(*this))
                ++n;
        }
        return n;
    }

    // Issue #1651: zero-copy span-return variant of children_stable (Task1 review 建议 #4).
    // Returns std::span<const StableNodeRef> over a thread-local pinned buffer of StableNodeRef
    // instead of std::vector<StableNodeRef>. Bumps children_stable_span_calls_total_ on every
    // call (observability surface for Agent's `copy-avoided` count; pairs with the dirty-side
    // mark_dirty_early_exit_count_ from #1251).
    //
    // The thread_local buffer is sized lazily at first call (PersistentChildVector pattern),
    // cleared, then populated by make_ref over a narrowed index range. The returned span is
    // valid until the next call to this method on the SAME thread.
    //
    // Prefer children_stable_span_view over children_stable in AI hot paths (focused subtree
    // mutate + query loops, EDSL navigation); reserve children_stable for boundary crossings
    // where the vector must outlive the caller's frame. NULL_NODE children are filtered out
    // (same as children_stable). Out-of-range ids return an empty span (no buffer mutation).
    [[nodiscard]] std::span<const StableNodeRef> children_stable_span_view(NodeId id) const {
        children_stable_span_calls_total_.fetch_add(1, std::memory_order_relaxed);
        if (id >= children_.size())
            return {};
        const auto& pcv = children_[id];
        thread_local std::vector<StableNodeRef> buf;
        buf.clear();
        buf.reserve(pcv.size());
        for (std::size_t i = 0; i < pcv.size(); ++i) {
            auto cid = pcv[i];
            if (cid == NULL_NODE)
                continue;
            buf.push_back(make_ref(cid));
        }
        return {buf.data(), buf.size()};
    }

    // Issue #398: zero-allocation iteration over stable
    // children. Equivalent to children_stable() but does NOT
    // allocate a vector — each non-NULL child is delivered to
    // the callback as a `StableNodeRef` (with the current
    // generation_ captured at call time). The callback may
    // return any type (typically void or a count).
    //
    // Use this in hot paths (AI Agent multi-round loops,
    // production EDSL navigation) where the caller only needs
    // to iterate once. For callers that need to store the
    // refs (e.g. across mutation boundaries), use the
    // allocating `children_stable()` instead.
    //
    // The callback signature: `void(StableNodeRef)`. Order
    // matches the underlying children span (left-to-right
    // = first-to-last child). NULL_NODE children are
    // filtered out (same as children_stable()).
    //
    // Out-of-range ids are silently a no-op (no callback
    // invocation, same as the allocating version's empty
    // vector).
    // Issue #1500: deliver full-provenance StableNodeRef to callback.
    template <typename Fn> void for_each_stable_child(NodeId id, Fn&& fn) const {
        if (id >= children_.size())
            return;
        const auto& pcv = children_[id];
        for (std::size_t i = 0; i < pcv.size(); ++i) {
            auto cid = pcv[i];
            if (cid == NULL_NODE)
                continue;
            fn(make_ref(cid));
        }
    }

    // Issue #398: count of non-NULL stable children. O(N)
    // in the children span. Use to size a pre-allocated
    // buffer if the caller wants to use the allocating
    // children_stable() but pre-allocate the right size
    // (currently children_stable() already does this via
    // reserve, but the count is useful for callers that
    // need the size before allocating).
    [[nodiscard]] std::size_t stable_child_count(NodeId id) const noexcept {
        if (id >= children_.size())
            return 0;
        const auto& pcv = children_[id];
        std::size_t n = 0;
        for (std::size_t i = 0; i < pcv.size(); ++i) {
            if (pcv[i] != NULL_NODE)
                ++n;
        }
        return n;
    }

    NodeId root = NULL_NODE;

    // Issue #738: workspace-layer COW epoch mirrored from
    // WorkspaceNode::cow_epoch on switch / lazy clone.
    std::uint64_t workspace_cow_epoch_ = 0;
    mutable std::atomic<std::uint64_t> pinned_across_boundaries_{0};
    // Issue #1406: counter for pins dropped by bounded-retention cap.
    mutable std::atomic<std::uint64_t> pinned_across_boundaries_dropped_{0};
    mutable std::atomic<std::uint64_t> cross_boundary_validations_{0};

    // Issue #1355: nested lightweight field-mutation frames (side log).
    // Not part of durable mutation_log_; committed frames are discarded.
    std::vector<std::vector<MutationRecord>> lightweight_frames_;
    bool render_lightweight_active_ = false;

    // Issue #250: atomic-batch state. Placed at the end of the
    // class (after `root`) to preserve the SoA field ordering
    // invariant — the pmr::vector<...> fields above must be
    // contiguous for the FlatAST's load / clear / mutate paths
    // to work. These two scalars come last so they don't shift
    // the offsets of any SoA columns.
    //
    // When true, individual structural mutations (set_child /
    // insert_child / remove_child) skip the per-op generation
    // bump. The batch commits with a single bump at the end,
    // so per-op bumps would be redundant.
    // Set by begin_atomic_batch(); cleared by commit / rollback.
    bool bump_generation_suppressed_ = false;
    // Issue #250: how many per-op bumps were suppressed during
    // the most recent batch. Updated on commit; exposed via
    // atomic_batch_bumps_saved() and observability snapshot.
    std::uint64_t atomic_batch_bumps_saved_ = 0;
    // Issue #1893: metadata columns captured at begin_atomic_batch.
    MetadataColumnsSnapshot atomic_batch_meta_snap_{};
    bool atomic_batch_meta_snap_valid_ = false;
    mutable std::atomic<std::uint64_t> atomic_batch_metadata_restored_total_{0};
    mutable std::atomic<std::uint64_t> atomic_batch_metadata_captured_total_{0};
};

// ── MutationVisitor concept (Issue #274) ─────────────────────
//
// Mirrors the Pass / AnalysisPass pattern in pass_manager.ixx,
// but for FlatAST mutation records instead of IRModule transforms.
// Visitors observe or react to committed mutations; the pipeline
// folds over the mutation log with short-circuit on has_error().
export template <typename V>
concept MutationVisitor = requires(V& v, FlatAST& flat, const MutationRecord& rec) {
    { v.visit_mutation(flat, rec) } -> std::same_as<void>;
    { v.has_error() } -> std::convertible_to<bool>;
};

// Pure-function mutation callbacks (no persistent visitor state).
export template <typename Fn>
concept PureMutationFn = requires(Fn& fn, FlatAST& flat, const MutationRecord& rec) {
    { fn(flat, rec) } -> std::same_as<void>;
};

export template <PureMutationFn Fn> class MutationFnWrap {
public:
    explicit MutationFnWrap(Fn& fn)
        : fn_(&fn) {}

    void visit_mutation(FlatAST& flat, const MutationRecord& rec) { (*fn_)(flat, rec); }
    bool has_error() const { return false; }

private:
    Fn* fn_;
};

// ── StableNodeRef + MutationRecord helpers ───────────────────
// Issue #378: bodies moved to ast_impl.cpp (non-template post-class
// items). Templates above (MutationFnWrap / run_mutation_*) MUST stay
// in this interface unit because templates with external visibility
// can't be defined in a non-exported module implementation unit.
export [[nodiscard]] FlatAST::StableNodeRef mutation_target_ref(const FlatAST& flat,
                                                                const MutationRecord& rec) noexcept;
export [[nodiscard]] FlatAST::StableNodeRef mutation_parent_ref(const FlatAST& flat,
                                                                const MutationRecord& rec) noexcept;
export [[nodiscard]] bool is_mutation_target_valid(const FlatAST& flat,
                                                   const MutationRecord& rec) noexcept;
export [[nodiscard]] bool is_mutation_parent_valid(const FlatAST& flat,
                                                   const MutationRecord& rec) noexcept;

// ── run_mutation_pipeline — fold over mutation log ───────────
export template <MutationVisitor V>
bool run_mutation_visitor_one(FlatAST& flat, const MutationRecord& rec, V& visitor) {
    visitor.visit_mutation(flat, rec);
    return !visitor.has_error();
}

export template <MutationVisitor... Visitors>
bool run_mutation_one(FlatAST& flat, const MutationRecord& rec, Visitors&... visitors) {
    return (run_mutation_visitor_one(flat, rec, visitors) && ...);
}

export template <MutationVisitor... Visitors>
bool run_mutation_pipeline(FlatAST& flat, Visitors&... visitors) {
    for (const auto& rec : flat.all_mutations()) {
        if (!run_mutation_one(flat, rec, visitors...))
            return false;
    }
    return true;
}

export template <MutationVisitor... Visitors>
bool run_mutation_pipeline(FlatAST& flat, std::span<const MutationRecord> records,
                           Visitors&... visitors) {
    for (const auto& rec : records) {
        if (!run_mutation_one(flat, rec, visitors...))
            return false;
    }
    return true;
}

// ── Example mutation visitors ──────────────────────────────────
// Issue #378: bodies moved to ast_impl.cpp. These classes are non-template
// and the bodies don't need to be in the interface unit for instantiation
// — the impl unit provides the definitions.
export class MutationCountVisitor {
public:
    void visit_mutation(FlatAST&, const MutationRecord& rec);
    bool has_error() const;
    std::size_t total_count() const;
    std::size_t committed_count() const;

private:
    std::size_t total_count_ = 0;
    std::size_t committed_count_ = 0;
};

export class MutationTargetValidityVisitor {
public:
    void visit_mutation(FlatAST& flat, const MutationRecord& rec);
    bool has_error() const;

private:
    bool has_error_ = false;
};

// Issue #276: resolve a captured stable ref across workspace layers.
// Issue #378: body moved to ast_impl.cpp (non-template free function).
export [[nodiscard]] std::optional<FlatAST::StableNodeRef>
resolve_across_layer(const FlatAST& target_flat, const mutation::NodeIdRemapTable& layer_remap,
                     FlatAST::StableNodeRef captured, std::uint32_t captured_layer,
                     std::uint32_t target_layer) noexcept;

// ── Patch application ──────────────────────────────────────────
export bool apply_patches(FlatAST& ast, std::span<const Patch> patches) pre(!patches.empty());

// ── Delta fixup (for deserialization) ──────────────────────────
export void fixup_deltas(FlatAST& ast);

// ── Bridge from pointer tree to FlatAST ────────────────────────


} // namespace aura::ast
