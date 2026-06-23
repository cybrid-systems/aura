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
#include <utility>
#include <vector>
#include "core/persistent_child_vector.hh"
#include <shared_mutex>

export module aura.core.ast;
import std;
import aura.core.type;
import aura.core.mutation;
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

    // Intern a string — returns a stable SymId
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
export constexpr std::array<NodeMeta, 26> kNodeMeta = {{
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
// deleted. This wrapper exposes a shared_mutex by pointer and
// defines the copy/move semantics we want for FlatAST:
//
//   - Copy ctor: allocates a FRESH shared_mutex. Each copy
//     gets its own mutex (independent mutation isolation).
//   - Copy assign: no-op (the destination keeps its own mutex;
//     only the data members are overwritten).
//   - Move ctor / move assign: default (the unique_ptr is moved,
//     transferring ownership of the mutex).
//
// Used as the type of `FlatAST::structural_mtx_`.
class OwnedSharedMutex {
public:
    OwnedSharedMutex()
        : ptr_(std::make_unique<std::shared_mutex>()) {}
    // Copy: allocate a fresh mutex.
    OwnedSharedMutex(const OwnedSharedMutex&) noexcept
        : ptr_(std::make_unique<std::shared_mutex>()) {}
    // Move: transfer ownership.
    OwnedSharedMutex(OwnedSharedMutex&&) noexcept = default;
    // Copy-assign: keep our own mutex (the data being copied
    // doesn't include the mutex state).
    OwnedSharedMutex& operator=(const OwnedSharedMutex&) noexcept { return *this; }
    // Move-assign: transfer ownership.
    OwnedSharedMutex& operator=(OwnedSharedMutex&&) noexcept = default;
    std::shared_mutex& get() noexcept { return *ptr_; }
    const std::shared_mutex& get() const noexcept { return *ptr_; }
    // Like get() but returns a non-const reference even through
    // a const OwnedSharedMutex. Needed because shared_lock /
    // unique_lock require a non-const mutex reference to acquire
    // (the lock state is part of the mutex). The const_cast is
    // safe here because acquiring a lock is a "logical const"
    // operation: it doesn't modify the protected data.
    std::shared_mutex& mutable_get() const noexcept { return *ptr_; }

private:
    std::unique_ptr<std::shared_mutex> ptr_;
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
export class FlatAST {
private:
    // Issue #261: node_gen_[id] == 0 marks a recycled (free-list) slot.
    // Live slots always carry the generation_ active when the slot was
    // last allocated or reset.
    [[nodiscard]] bool is_free_slot(NodeId id) const noexcept {
        return id >= node_gen_.size() || node_gen_[id] == 0;
    }

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
        error_kind_[id] = 0;
        if (id < value_cache_.size())
            value_cache_[id] = kNotCached;
        node_first_mutation_[id] = 0;
        parent_[id] = NULL_NODE;
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

    [[nodiscard]] NodeId add_node(NodeTag tag, SyntaxMarker m = SyntaxMarker::User) {
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
        type_id_.push_back(0);
        dirty_.push_back(0);
        ppa_dirty_.push_back(0);
        // Issue #437: verify_dirty_ column. Mirrors ppa_dirty_'s
        // push_back(0) pattern; populated by apply_verify_dirty_bits.
        verify_dirty_.push_back(0);
        // Issue #79: per-node error kind (0 = no error, non-zero = ErrorKind).
        // Populated by the type-checker and runtime evaluator; queryable via
        // the AuraQuery `(has-error? N)` clause.
        error_kind_.push_back(0);
        // value cache initialized lazily (not in arena — module-level vector)
        if (id >= static_cast<NodeId>(value_cache_.size()))
            value_cache_.resize(id + 1, kNotCached);
        else
            value_cache_[id] = kNotCached;
        node_first_mutation_.push_back(0);
        parent_.push_back(NULL_NODE);
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
    //
    // The children_ field uses the DEFAULT polymorphic_allocator
    // (i.e. the global memory resource). Arena propagation to
    // nested pmr vectors is brittle in C++26; a future commit
    // can wire a custom allocator wrapper if arena-backed
    // per-node lists are needed.
    std::pmr::vector<PersistentChildVector<NodeId>> children_;
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
    std::pmr::vector<NodeId> parent_;
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
    std::pmr::vector<std::uint32_t> type_id_;
    // Issue #79: per-node error kind, 0 = no error, non-zero = ErrorKind
    // enum value. Populated by the type-checker and runtime evaluator;
    // queryable via the AuraQuery `(has-error? N)` clause.
    std::pmr::vector<std::uint8_t> error_kind_;
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
    // Issue #282: Occurrence Typing provenance log. Each entry
    // is captured when synthesize_flat_if applies a narrowing
    // (predicate → refined type in a branch). Exposed via
    // (query:provenance-of var-name) and all_narrowings() /
    // narrowing_count() accessors. The log is cleared on
    // full FlatAST reset, same lifecycle as mutation_log_.
    std::pmr::vector<NarrowingRecord> narrowing_log_;
    std::pmr::vector<std::uint32_t> node_first_mutation_;
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
    // Issue #457: generation_ / node_gen_ lifecycle
    // observability counters. All stats-only (relaxed
    // ordering). Bumped in bump_generation() (wrap
    // detection), is_valid() (stale access), and
    // StableNodeRef validation (invalidation).
    // Exposed via the (query:stable-ref-stats) primitive.
    mutable std::atomic<std::uint64_t> generation_wrap_count_{0};
    mutable std::atomic<std::uint64_t> node_gen_stale_access_count_{0};
    mutable std::atomic<std::uint64_t> atomic_batch_commits_{0};
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
    // Issue #261: NodeId lifecycle observability counters.
    mutable std::atomic<std::uint64_t> node_recycle_total_{0};
    mutable std::atomic<std::uint64_t> node_slot_reuse_count_{0};
    mutable std::atomic<std::uint64_t> node_compact_total_{0};
    // Issue #437: verification-dirty observability counters.
    // Bumped by apply_verify_dirty_bits. Stats-only
    // (relaxed-ordering). Exposed via the
    // (query:verify-dirty-stats) primitive.
    mutable std::atomic<std::uint64_t> verify_assertion_dirty_total_{0};
    mutable std::atomic<std::uint64_t> verify_coverage_dirty_total_{0};
    mutable std::atomic<std::uint64_t> verify_sva_dirty_total_{0};
    mutable std::atomic<std::uint64_t> verify_formal_cex_dirty_total_{0};

public:
    // Issue #437: per-reason verify-dirty stat accessors.
    // Public so the (query:verify-dirty-stats) primitive
    // can read them from evaluator_primitives_compile.cpp.
    [[nodiscard]] std::uint64_t verify_assertion_dirty_total() const noexcept {
        return verify_assertion_dirty_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t verify_coverage_dirty_total() const noexcept {
        return verify_coverage_dirty_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t verify_sva_dirty_total() const noexcept {
        return verify_sva_dirty_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t verify_formal_cex_dirty_total() const noexcept {
        return verify_formal_cex_dirty_total_.load(std::memory_order_relaxed);
    }

public:
    // Issue #437: verify_dirty_ accessor (public, used by
    // the (compile:verify-dirty?) primitive).
    [[nodiscard]] std::uint8_t verify_dirty(NodeId id) const noexcept {
        if (id >= verify_dirty_.size()) return 0;
        return verify_dirty_[id];
    }
    // Issue #456: public accessor for the main dirty_ byte
    // (used by (query:dirty-subtree) to check each node's
    // dirty bitmask against a reason filter).
    [[nodiscard]] std::uint8_t dirty(NodeId id) const noexcept {
        if (id >= dirty_.size()) return 0;
        return dirty_[id];
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
        , param_begin_(std::move(other.param_begin_))
        , param_count_(std::move(other.param_count_))
        , cap_require_count_(std::move(other.cap_require_count_))
        , param_data_(std::move(other.param_data_))
        , param_annot_data_(std::move(other.param_annot_data_))
        , line_(std::move(other.line_))
        , col_(std::move(other.col_))
        , type_id_(std::move(other.type_id_))
        , error_kind_(std::move(other.error_kind_))
        , value_cache_(std::move(other.value_cache_))
        , mutation_log_(std::move(other.mutation_log_))
        , node_first_mutation_(std::move(other.node_first_mutation_))
        , next_mutation_id_(other.next_mutation_id_)
        , generation_(other.generation_)
        , node_gen_(std::move(other.node_gen_))
        , free_list_(std::move(other.free_list_))
        , bump_generation_count_(other.bump_generation_count_.load())
        , is_valid_check_count_(other.is_valid_check_count_.load())
        , stable_ref_invalidations_(other.stable_ref_invalidations_.load())
        , atomic_batch_commits_(other.atomic_batch_commits_.load())
        , generation_wrap_count_(other.generation_wrap_count_.load())
        , node_gen_stale_access_count_(other.node_gen_stale_access_count_.load())
        , children_call_count_(other.children_call_count_.load())
        , parent_of_call_count_(other.parent_of_call_count_.load())
        , mark_dirty_upward_call_count_(other.mark_dirty_upward_call_count_.load())
        , mark_dirty_total_nodes_(other.mark_dirty_total_nodes_.load())
        , node_recycle_total_(other.node_recycle_total_.load())
        , node_slot_reuse_count_(other.node_slot_reuse_count_.load())
        , node_compact_total_(other.node_compact_total_.load())
        , match_info_(std::move(other.match_info_))
        , region_by_sym_(std::move(other.region_by_sym_))
        , region_by_lambda_id_(std::move(other.region_by_lambda_id_))
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
            param_begin_ = std::move(other.param_begin_);
            param_count_ = std::move(other.param_count_);
            cap_require_count_ = std::move(other.cap_require_count_);
            param_data_ = std::move(other.param_data_);
            param_annot_data_ = std::move(other.param_annot_data_);
            line_ = std::move(other.line_);
            col_ = std::move(other.col_);
            type_id_ = std::move(other.type_id_);
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
            generation_wrap_count_.store(other.generation_wrap_count_.load());
            node_gen_stale_access_count_.store(other.node_gen_stale_access_count_.load());
            children_call_count_.store(other.children_call_count_.load());
            parent_of_call_count_.store(other.parent_of_call_count_.load());
            mark_dirty_upward_call_count_.store(other.mark_dirty_upward_call_count_.load());
            mark_dirty_total_nodes_.store(other.mark_dirty_total_nodes_.load());
            node_recycle_total_.store(other.node_recycle_total_.load());
            node_slot_reuse_count_.store(other.node_slot_reuse_count_.load());
            node_compact_total_.store(other.node_compact_total_.load());
            match_info_ = std::move(other.match_info_);
            region_by_sym_ = std::move(other.region_by_sym_);
            region_by_lambda_id_ = std::move(other.region_by_lambda_id_);
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
        , param_begin_(other.param_begin_)
        , param_count_(other.param_count_)
        , cap_require_count_(other.cap_require_count_)
        , param_data_(other.param_data_)
        , param_annot_data_(other.param_annot_data_)
        , line_(other.line_)
        , col_(other.col_)
        , type_id_(other.type_id_)
        , error_kind_(other.error_kind_)
        , value_cache_(other.value_cache_)
        , mutation_log_(other.mutation_log_)
        , node_first_mutation_(other.node_first_mutation_)
        , next_mutation_id_(other.next_mutation_id_)
        , generation_(other.generation_)
        , node_gen_(other.node_gen_)
        , free_list_(other.free_list_)
        , bump_generation_count_(other.bump_generation_count_.load())
        , is_valid_check_count_(other.is_valid_check_count_.load())
        , stable_ref_invalidations_(other.stable_ref_invalidations_.load())
        , atomic_batch_commits_(other.atomic_batch_commits_.load())
        , children_call_count_(other.children_call_count_.load())
        , parent_of_call_count_(other.parent_of_call_count_.load())
        , mark_dirty_upward_call_count_(other.mark_dirty_upward_call_count_.load())
        , mark_dirty_total_nodes_(other.mark_dirty_total_nodes_.load())
        , node_recycle_total_(other.node_recycle_total_.load())
        , node_slot_reuse_count_(other.node_slot_reuse_count_.load())
        , node_compact_total_(other.node_compact_total_.load())
        , match_info_(other.match_info_)
        , region_by_sym_(other.region_by_sym_)
        , region_by_lambda_id_(other.region_by_lambda_id_)
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
            param_begin_ = other.param_begin_;
            param_count_ = other.param_count_;
            cap_require_count_ = other.cap_require_count_;
            param_data_ = other.param_data_;
            param_annot_data_ = other.param_annot_data_;
            line_ = other.line_;
            col_ = other.col_;
            type_id_ = other.type_id_;
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
            children_call_count_.store(other.children_call_count_.load());
            parent_of_call_count_.store(other.parent_of_call_count_.load());
            mark_dirty_upward_call_count_.store(other.mark_dirty_upward_call_count_.load());
            mark_dirty_total_nodes_.store(other.mark_dirty_total_nodes_.load());
            node_recycle_total_.store(other.node_recycle_total_.load());
            node_slot_reuse_count_.store(other.node_slot_reuse_count_.load());
            node_compact_total_.store(other.node_compact_total_.load());
            match_info_ = other.match_info_;
            region_by_sym_ = other.region_by_sym_;
            region_by_lambda_id_ = other.region_by_lambda_id_;
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
        , children_(alloc)

        , parent_(alloc)
        , param_begin_(alloc)
        , param_count_(alloc)
        , cap_require_count_(alloc)
        , param_data_(alloc)
        , param_annot_data_(alloc)
        , line_(alloc)
        , col_(alloc)
        , type_id_(alloc)
        , error_kind_(alloc)
        , value_cache_(alloc)
        , mutation_log_(alloc)
        , node_first_mutation_(alloc)
        , node_gen_(alloc)
        , free_list_(alloc) {}

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
        link_children(id);
        return id;
    }

    [[nodiscard]] NodeId add_variable(SymId name) {
        auto id = add_node(NodeTag::Variable);
        sym_id_[id] = name;
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
    void set_function_region(SymId name, std::uint8_t region) { region_by_sym_[name] = region; }
    // Overload tag: pass a 0 literal to disambiguate. We
    // use a sentinel parameter (an unused int) so the two
    // overloads don't collide on the same uint32_t underlying
    // type. Callers use set_function_region_sym and
    // set_function_region_lambda explicitly.
    void set_function_region_lambda(NodeId lambda_id, std::uint8_t region) {
        region_by_lambda_id_[lambda_id] = region;
    }
    [[nodiscard]] std::optional<std::uint8_t> get_function_region_for_sym(SymId name) const {
        auto it = region_by_sym_.find(name);
        if (it == region_by_sym_.end())
            return std::nullopt;
        return it->second;
    }
    [[nodiscard]] std::optional<std::uint8_t>
    get_function_region_for_lambda(NodeId lambda_id) const {
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
    bool subtree_uses_sym(aura::ast::NodeId root, SymId sym) const {
        if (root == aura::ast::NULL_NODE || root >= size())
            return false;
        auto v = get(root);
        if (v.tag == aura::ast::NodeTag::Variable && v.sym_id == sym)
            return true;
        for (auto c : v.children) {
            if (subtree_uses_sym(c, sym))
                return true;
        }
        return false;
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

    // ── Parent access ──────────────────────────────────────────
    NodeId parent_of(NodeId id) const {
        // Issue #256: bump the call counter (lifetime total).
        parent_of_call_count_.fetch_add(1, std::memory_order_relaxed);
        return id < parent_.size() ? parent_[id] : NULL_NODE;
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
        return std::span<const NodeId>(children_[id].data(), children_[id].size());
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
    void set_child_locked(NodeId id, std::uint32_t idx, NodeId child) {
        const auto& list = children_[id];
        if (idx >= list.size())
            return;
        auto old_cid = list[idx];
        if (old_cid != NULL_NODE && old_cid < parent_.size())
            parent_[old_cid] = NULL_NODE;
        children_[id] = list.with_set(idx, child);
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
        add_mutation_child_op(id, idx, old_cid, child, "structural-set-child");
    }
    void insert_child_locked(NodeId id, std::uint32_t idx, NodeId child) {
        const auto& list = children_[id];
        auto pos = std::min(static_cast<std::uint32_t>(list.size()), idx);
        children_[id] = list.with_insert(pos, child);
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
        add_mutation_child_op(id, pos, NULL_NODE, child, "structural-insert-child");
    }
    void remove_child_locked(NodeId id, std::uint32_t idx) {
        const auto& list = children_[id];
        if (idx < list.size()) {
            auto cid = list[idx];
            if (cid != NULL_NODE && cid < parent_.size())
                parent_[cid] = NULL_NODE;
            children_[id] = list.with_erase(idx);
            add_mutation_child_op(id, idx, cid, NULL_NODE, "structural-remove-child");
        }
    }

    void set_child(NodeId id, std::uint32_t idx, NodeId child) {
        // Issue #222: acquire the structural mutation guard. The
        // guard's dtor bumps generation_ + releases the lock.
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
    std::pmr::vector<PersistentChildVector<NodeId>> snapshot_children() const {
        return children_; // vector copy ctor; each PCV is shared_ptr copy
    }

    // Issue #221: restore children_ from a pre-captured snapshot.
    // The passed-in vector is moved (its shared_ptrs are now bound
    // to children_; back-references to the old PCVs in the
    // snapshot are released as the snapshot goes out of scope).
    void restore_children(std::pmr::vector<PersistentChildVector<NodeId>>&& snapshot) {
        children_ = std::move(snapshot);
        bump_generation();
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
        std::pmr::vector<PersistentChildVector<NodeId>> new_children(alloc);
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
        std::pmr::vector<std::uint32_t> new_type_id(alloc);
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
            new_type_id.push_back(type_id_[id]);
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
        type_id_ = std::move(new_type_id);
        error_kind_ = std::move(new_error_kind);
        node_first_mutation_ = std::move(new_node_first_mutation);
        node_gen_ = std::move(new_node_gen);
        value_cache_ = std::move(new_value_cache);

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
        dirty_.clear();
        ppa_dirty_.clear();
        // Issue #437: clear verify_dirty_ alongside ppa_dirty_
        verify_dirty_.clear();
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
        buf.insert(buf.end(), reinterpret_cast<char*>(&len),
                   reinterpret_cast<char*>(&len) + 4);
        buf.insert(buf.end(), s.begin(), s.end());
    }

    static void wire_write_vec_u32(std::vector<char>& buf,
                                   const std::vector<SymId>& v) {
        std::uint32_t sz = static_cast<std::uint32_t>(v.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&sz),
                   reinterpret_cast<char*>(&sz) + 4);
        if (!v.empty()) {
            buf.insert(buf.end(), reinterpret_cast<const char*>(v.data()),
                       reinterpret_cast<const char*>(v.data()) + v.size() * sizeof(SymId));
        }
    }

    static void wire_write_match_clause_info(std::vector<char>& buf,
                                             const MatchClauseInfo& m) {
        wire_write_vec_u32(buf, m.used_constructors);
        wire_write_vec_u32(buf, m.candidate_constructors);
        buf.push_back(m.has_wildcard ? '\1' : '\0');
    }

    static void wire_write_map_u32_u8(
        std::vector<char>& buf,
        const std::pmr::unordered_map<std::uint32_t, std::uint8_t>& m) {
        std::uint32_t count = static_cast<std::uint32_t>(m.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
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

    static std::vector<SymId> wire_read_vec_u32(const std::vector<char>& buf,
                                                  std::size_t& pos) {
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

    static void wire_read_map_u32_u8(
        const std::vector<char>& buf, std::size_t& pos,
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
        // Issue #437: read verify_dirty_ alongside dirty_. The
        // read is conditional on the remaining buffer size —
        // older binaries won't have this column, so we treat
        // an empty read as "all zeros" (default-initialized).
        if (pos < buf.size()) {
            read_column(ast.verify_dirty_);
        }
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
        kStructDirty = 0x20,     // structural shape changed (children/parent)
        kDefUseDirty = 0x40,     // def-use / caller graph may be stale
        kPpaHintDirty = 0x80,    // PPA-hint metadata needs refresh
    };

    // Issue #277: PPA-specific dirty bits stored in the orthogonal
    // ppa_dirty_ column (DirtyReason uint8_t is full). Setting any
    // PPA bit also ORs kPpaHintDirty on dirty_ for backward-compat
    // with dirty:counts "ppa-hint" aggregation.
    enum PpaDirtyReason : std::uint8_t {
        kTimingDirty = 0x01,  // timing closure stale
        kPowerDirty = 0x02,   // power estimate stale
        kAreaDirty = 0x04,    // area estimate stale
        kBackendHint = 0x08,  // Verilog/HW backend should re-emit
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
        kAssertionDirty = 0x01,        // assertion failed
        kCoverageDirty = 0x02,         // coverage hole detected
        kSvaDirty = 0x04,              // SVA property/sequence affected
        kFormalCounterexampleDirty = 0x08, // formal proof counterexample
    };

    // Issue #437: OR verify bits into verify_dirty_ and mirror
    // kGeneralDirty on dirty_ so legacy is_dirty() callers still
    // see "this node needs work". The public definition is
    // in the public section below.
    // (No forward declaration needed; the public definition
    //  at line ~775 is reachable from the dirty_observability
    //  path via class-internal lookup.)

    // Issue #188: mark a node dirty for one or more specific reasons.
    // The `kGeneralDirty` bit is set automatically so existing
    // is_dirty() callers still see "this node needs work".
    void mark_dirty(NodeId id, std::uint8_t reasons = kGeneralDirty) {
        if (id >= dirty_.size())
            dirty_.resize(id + 1, 0);
        dirty_[id] |= reasons;
        clear_cached_value(id); // invalidate result cache
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
    void mark_dirty_upward(const NodeId id, std::uint8_t reasons = kGeneralDirty,
                           std::uint8_t ppa_reasons = 0)
        // Issue #273: node must be in-bounds; NULL_NODE would resize dirty_ to ~4G.
        pre(id < tag_.size()) {
        // Issue #256: bump the call counter + track total nodes
        // touched. The ratio (total_nodes / call_count) gives
        // the average dirty-propagation depth per mutation —
        // the key metric for whether the std::meta refactor is
        // worth it.
        mark_dirty_upward_call_count_.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t touched = 0;
        std::deque<NodeId> queue;
        queue.push_back(id);
        while (!queue.empty()) {
            auto nid = queue.front();
            queue.pop_front();
            mark_dirty(nid, reasons);
            apply_ppa_dirty_bits(nid, ppa_reasons);
            ++touched;
            auto p = parent_[nid];
            if (p != NULL_NODE)
                queue.push_back(p);
        }
        mark_dirty_total_nodes_.fetch_add(touched, std::memory_order_relaxed);
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

    // Record a mutation on a node. Returns the mutation_id.
    std::uint64_t add_mutation(NodeId node, std::string_view op_name, std::string_view old_type,
                               std::string_view new_type, std::string_view summary,
                               MutationStatus status = MutationStatus::Committed) {
        return add_mutation_with_rollback(node, op_name, old_type, new_type, summary, status, 0, 0,
                                          0, false);
    }

    // Record a mutation with rollback data (field_offset + old/new_value)
    std::uint64_t add_mutation_with_rollback(const NodeId node, std::string_view op_name,
                                             std::string_view old_type, std::string_view new_type,
                                             std::string_view summary, MutationStatus status,
                                             std::uint32_t field_offset, std::uint64_t old_value,
                                             std::uint64_t new_value, bool has_rollback)
        pre(node < tag_.size()) post(r: r >= 1) {
        const std::uint64_t mid = next_mutation_id_++;
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
        }));
        mark_dirty_upward(node);
        if (node < node_first_mutation_.size() && node_first_mutation_[node] == 0)
            node_first_mutation_[node] = static_cast<std::uint32_t>(mutation_log_.size());
        return mid;
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
        }));
        if (target_node != NULL_NODE)
            mark_dirty_upward(target_node);
        if (parent_id != NULL_NODE)
            mark_dirty_upward(parent_id);
        if (target_node < node_first_mutation_.size() && node_first_mutation_[target_node] == 0)
            node_first_mutation_[target_node] = static_cast<std::uint32_t>(mutation_log_.size());
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
    std::uint64_t next_mutation_id() const { return next_mutation_id_; }

    // Get all mutation records (unfiltered).
    const std::pmr::vector<MutationRecord>& all_mutations() const { return mutation_log_; }
    std::pmr::vector<MutationRecord>& all_mutations() { return mutation_log_; }

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

    // Rollback a mutation by ID. Returns true if successful.
    // Current FlatAST generation. Incremented on rollback to invalidate stale NodeIds.
    std::uint16_t generation() const { return generation_; }

    // Issue #276: live slot check without generation equality (cross-layer resolve).
    [[nodiscard]] bool is_live_node(NodeId id) const noexcept {
        return id != NULL_NODE && id < tag_.size() && id < node_gen_.size() && node_gen_[id] != 0;
    }

    // Check if a NodeId is valid (in-bounds and from the current generation).
    bool is_valid(const NodeId id) const
        post(r: r == (id != NULL_NODE && id < tag_.size() && id < node_gen_.size() &&
                      node_gen_[id] == generation_)) {
        // Issue #255: bump the check counter (lifetime total).
        is_valid_check_count_.fetch_add(1, std::memory_order_relaxed);
        if (id == NULL_NODE) return false;
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
        post(r: !r.has_value() || (id < tag_.size() && id < node_gen_.size() &&
                                   node_gen_[id] == generation_)) {
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

        // Default-constructed refs are always invalid (id=NULL).
        bool is_valid_in(const FlatAST& ast) const noexcept { return ast.is_valid(*this); }
    };

    // Issue #191: make a StableNodeRef capturing the current
    // generation. Use this in EDSL / query / mutate primitives
    // to safely hold a reference to a node across calls.
    [[nodiscard]] StableNodeRef make_ref(NodeId id) const noexcept {
        return StableNodeRef{id, generation_};
    }

    // Issue #191: validation + safe get that take a StableNodeRef.
    // The ref's gen is compared to the FlatAST's current gen; if
    // they differ, the ref is stale (a structural mutation
    // happened in between).
    [[nodiscard]] bool is_valid(const StableNodeRef& ref) const noexcept
        post(r: r == (ref.id != NULL_NODE && ref.id < tag_.size() &&
                      ref.id < node_gen_.size() && node_gen_[ref.id] == ref.gen &&
                      ref.gen == generation_)) {
        // Issue #255: bump the check counter (lifetime total).
        is_valid_check_count_.fetch_add(1, std::memory_order_relaxed);
        bool ok = ref.id != NULL_NODE && ref.id < tag_.size() && ref.id < node_gen_.size() &&
                  node_gen_[ref.id] == ref.gen &&
                  ref.gen == generation_; // gen must also match current
        if (!ok) {
            // Stale ref — bump the invalidations counter.
            // The whole point of StableNodeRef is to detect
            // this case; counting it tells us how often the
            // mechanism actually catches a stale handle.
            stable_ref_invalidations_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }
    [[nodiscard]] std::optional<NodeView> get_safe(const StableNodeRef& ref) const noexcept
        post(r: !r.has_value() || is_valid(ref)) {
        if (!is_valid(ref))
            return std::nullopt;
        return get(ref.id);
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
        }
        // Issue #255: only count actual bumps (suppressed
        // ones are accounted for via atomic_batch_commits_).
        bump_generation_count_.fetch_add(1, std::memory_order_relaxed);
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
    }

    // Commit the batch. Calls bump_generation() once, marks
    // all batched nodes dirty, releases the suppression.
    void commit_atomic_batch() noexcept {
        bump_generation_suppressed_ = false;
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
    }

    // Roll back the batch. No bump (the changes were never
    // visible — gen didn't change). Releases the suppression.
    void rollback_atomic_batch() noexcept {
        bump_generation_suppressed_ = false;
        // No bump. No dirty sweep. The lockless helper's
        // own rollback_since() has already reverted the
        // mutation_log_ entries; readers holding the
        // pre-batch generation_ see no change.
    }

    // Issue #250: how many generation bumps the latest batch
    // saved by suppressing per-op bumps. Updated on each
    // commit. Exposed via observability snapshot.
    std::uint64_t atomic_batch_bumps_saved() const noexcept { return atomic_batch_bumps_saved_; }

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
    [[nodiscard]] std::uint16_t current_generation() const noexcept {
        return generation_;
    }
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
    std::uint64_t parent_of_call_count() const noexcept {
        return parent_of_call_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t mark_dirty_upward_call_count() const noexcept {
        return mark_dirty_upward_call_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t mark_dirty_total_nodes() const noexcept {
        return mark_dirty_total_nodes_.load(std::memory_order_relaxed);
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
    void bump_generation_on_rollback() {
        ++generation_;
        if (generation_ == 0)
            generation_ = 1;
    }

    [[nodiscard]] std::expected<void, MutationError> try_rollback_structural_child_op(
        MutationRecord& rec) {
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
        bump_generation_on_rollback();
        mark_dirty_upward(parent);
        return {};
    }

public:

    // Rollback all mutations since (and including) the given ID.
    std::size_t rollback_since(std::uint64_t since_id) {
        std::size_t count = 0;
        for (auto it = mutation_log_.rbegin(); it != mutation_log_.rend(); ++it) {
            if (it->mutation_id >= since_id && it->status == MutationStatus::Committed) {
                if (rollback(it->mutation_id))
                    ++count;
            }
        }
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
        std::size_t count = 0;
        for (std::size_t i = mutation_log_.size(); i > checkpoint_size; --i) {
            auto& rec = mutation_log_[i - 1];
            if (rec.status == MutationStatus::Committed) {
                if (rollback(rec.mutation_id))
                    ++count;
            }
        }
        return count;
    }

    // ── Type ID access ─────────────────────────────────────────

    std::uint32_t type_id(NodeId id) const { return id < type_id_.size() ? type_id_[id] : 0; }

    void set_type(NodeId id, std::uint32_t tid) {
        if (id < type_id_.size())
            type_id_[id] = tid;
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
    [[nodiscard]] StableNodeRef parent_stable(NodeId id) const noexcept {
        if (id >= parent_.size())
            return StableNodeRef{};
        auto pid = parent_[id];
        if (pid == NULL_NODE)
            return StableNodeRef{};
        return StableNodeRef{pid, generation_};
    }

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
            out.push_back(StableNodeRef{cid, generation_});
        }
        return out;
    }

    NodeId root = NULL_NODE;

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

export template <PureMutationFn Fn>
class MutationFnWrap {
public:
    explicit MutationFnWrap(Fn& fn) : fn_(&fn) {}

    void visit_mutation(FlatAST& flat, const MutationRecord& rec) { (*fn_)(flat, rec); }
    bool has_error() const { return false; }

private:
    Fn* fn_;
};

// ── StableNodeRef + MutationRecord helpers ───────────────────
export [[nodiscard]] FlatAST::StableNodeRef mutation_target_ref(const FlatAST& flat,
                                                                const MutationRecord& rec) noexcept {
    return flat.make_ref(rec.target_node);
}

export [[nodiscard]] FlatAST::StableNodeRef mutation_parent_ref(const FlatAST& flat,
                                                                const MutationRecord& rec) noexcept {
    return flat.make_ref(rec.parent_id);
}

export [[nodiscard]] bool is_mutation_target_valid(const FlatAST& flat,
                                                   const MutationRecord& rec) noexcept {
    return flat.is_valid(mutation_target_ref(flat, rec));
}

export [[nodiscard]] bool is_mutation_parent_valid(const FlatAST& flat,
                                                  const MutationRecord& rec) noexcept {
    return rec.parent_id == NULL_NODE || flat.is_valid(mutation_parent_ref(flat, rec));
}

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
export class MutationCountVisitor {
public:
    void visit_mutation(FlatAST&, const MutationRecord& rec) {
        if (rec.status == MutationStatus::Committed)
            ++committed_count_;
        ++total_count_;
    }

    bool has_error() const { return false; }
    std::size_t total_count() const { return total_count_; }
    std::size_t committed_count() const { return committed_count_; }

private:
    std::size_t total_count_ = 0;
    std::size_t committed_count_ = 0;
};

export class MutationTargetValidityVisitor {
public:
    void visit_mutation(FlatAST& flat, const MutationRecord& rec) {
        if (rec.status != MutationStatus::Committed)
            return;
        const bool has_target = rec.target_node != NULL_NODE;
        const bool has_parent = rec.parent_id != NULL_NODE;
        if (!has_target && !has_parent)
            return;
        if (has_target && !is_mutation_target_valid(flat, rec))
            has_error_ = true;
        if (has_parent && !is_mutation_parent_valid(flat, rec))
            has_error_ = true;
    }

    bool has_error() const { return has_error_; }

private:
    bool has_error_ = false;
};

// Issue #276: resolve a captured stable ref across workspace layers.
export [[nodiscard]] std::optional<FlatAST::StableNodeRef> resolve_across_layer(
    const FlatAST& target_flat, const mutation::NodeIdRemapTable& layer_remap,
    FlatAST::StableNodeRef captured, std::uint32_t captured_layer,
    std::uint32_t target_layer) noexcept {
    if (captured_layer == target_layer)
        return target_flat.is_valid(captured) ? std::optional{captured} : std::nullopt;
    NodeId mapped = captured.id;
    if (captured_layer < target_layer)
        mapped = layer_remap.resolve_from_parent(mapped);
    else
        mapped = layer_remap.resolve_to_parent(mapped);
    if (!target_flat.is_live_node(mapped))
        return std::nullopt;
    return FlatAST::StableNodeRef{mapped, target_flat.generation()};
}

// ── Patch application ──────────────────────────────────────────
export bool apply_patches(FlatAST& ast, std::span<const Patch> patches) pre(!patches.empty());

// ── Delta fixup (for deserialization) ──────────────────────────
export void fixup_deltas(FlatAST& ast);

// ── Bridge from pointer tree to FlatAST ────────────────────────


} // namespace aura::ast
