// Issue #275: pure mutation / rollback helpers extracted from FlatAST.
module;

#include <chrono>
#include <cstring>
#include <expected>
#include <unordered_map>

export module aura.core.mutation;
import std;
import aura.core.error;
import aura.core.concepts;

namespace aura::ast {

// ── Shared node aliases (same values as aura.core.ast) ───────
export using NodeId = std::uint32_t;
export constexpr NodeId NULL_NODE = ~0u;
export using SymId = std::uint32_t;

// ── NullIdCheck specialization for NodeId ─────────────────
//
// NodeId's null sentinel is NULL_NODE (~0u), NOT the default
// Id{} (= 0). Specialize so walk_ancestors / count_nodes_with_predicate
// (in aura.compiler.query) can correctly detect the null sentinel
// when terminating a parent-chain walk.
//
// Note: explicit specialization of class templates must be in
// the same namespace as the primary template (aura::core). So
// we close aura::ast here, specialize in aura::core, then reopen
// aura::ast for the rest of the module.
} // namespace aura::ast

namespace aura::core {
template <>
struct NullIdCheck<aura::ast::NodeId> {
    static constexpr bool is_null(aura::ast::NodeId id) noexcept {
        return id == aura::ast::NULL_NODE;
    }
};
} // namespace aura::core

namespace aura::ast {

// ── MutationRecord — typed mutation audit log ─────────────────
export enum class MutationStatus : std::uint8_t {
    Committed,
    RolledBack,
};

export enum class InvariantStatus : std::uint8_t {
    NotChecked = 0,
    Ok = 1,
    Warnings = 2,
    Violations = 3,
};

export struct MutationRecord {
    std::uint64_t mutation_id;
    std::uint64_t timestamp_ms;
    NodeId target_node;
    std::string operator_name;
    std::string old_type_str;
    std::string new_type_str;
    std::string summary;
    MutationStatus status;
    std::uint32_t field_offset;
    std::uint64_t old_value;
    std::uint64_t new_value;
    bool has_rollback_data;
    NodeId parent_id = NULL_NODE;
    std::uint32_t child_idx = 0;
    std::string old_subtree_source;
    bool has_subtree_rollback = false;
    InvariantStatus invariant_status = InvariantStatus::NotChecked;
};

// Issue #282: provenance record for Occurrence Typing
// narrowings. Captured when synthesize_flat_if applies a
// refinement (e.g. (string? x) → x : String in the then-branch).
// Side-channel to MutationRecord: this is about which
// *predicate* caused the narrowing, not which mutation
// changed the AST.
//
// Used by (query:provenance-of var-name) to answer "why does
// this variable have type T in this scope?".
export struct NarrowingRecord {
    // The variable that was narrowed.
    std::string var_name;
    // The predicate that caused the narrowing (e.g. "string?",
    // "pair?", "(and string? (> (length x) 2))").
    std::string predicate_src;
    // The refined type as a string ("Pair", "String", etc.).
    std::string refined_type_str;
    // The IfExpr node where the narrowing was applied. Lets
    // the provenance query distinguish "this narrowing is
    // live in this branch" from "this narrowing came from
    // a previous IfExpr and may be stale".
    NodeId if_node = NULL_NODE;
    // The cond node that the predicate analyzer walked.
    NodeId cond_node = NULL_NODE;
    // True if the predicate was a (not p) wrapper. Lets the
    // consumer reconstruct the narrowing source.
    bool is_negation = false;
    // Narrowing-evidence bitmask (kNarrow* values). Mirrors
    // the Branch instruction's narrow_evidence (#280) so
    // the provenance can be joined with the IR.
    std::uint32_t narrow_evidence = 0;
    // Epoch at capture. A narrowing is considered "stale" if
    // the FlatAST's generation has advanced past this value.
    // The consumer can filter on (record.epoch < flat.generation()).
    std::uint64_t capture_epoch = 0;
    // Sequence number so the consumer can iterate in
    // application order.
    std::uint64_t record_id = 0;
    // Issue #537 / #518 Phase 2: mutation that triggered
    // this record's (re-)capture. 0 = unknown / first-typecheck.
    std::uint64_t source_mutation_id = 0;
};

export enum class MutationSoAField : std::uint32_t {
    IntVal = 0,
    TypeId = 1,
    SymId = 2,
    FloatVal = 3,
};

// ── std::expected error surface ───────────────────────────────
export enum class MutationError : std::uint8_t {
    NotCommitted,
    NoRollbackData,
    InvalidTarget,
    InvalidParent,
    InvalidField,
    UnknownStructuralOp,
    OutOfRange,
};

export [[nodiscard]] constexpr std::string_view mutation_error_string(MutationError err) noexcept {
    switch (err) {
        case MutationError::NotCommitted:
            return "mutation is not committed";
        case MutationError::NoRollbackData:
            return "mutation has no rollback data";
        case MutationError::InvalidTarget:
            return "invalid mutation target node";
        case MutationError::InvalidParent:
            return "invalid mutation parent node";
        case MutationError::InvalidField:
            return "invalid SoA field for rollback";
        case MutationError::UnknownStructuralOp:
            return "unknown structural rollback operator";
        case MutationError::OutOfRange:
            return "rollback index out of range";
    }
    return "unknown mutation error";
}

// Issue #474: AuraErrorKind is defined in aura.core.error
// (the new unified error module). The kind-only conversion
// (MutationError → AuraErrorKind) lives here because
// AuraErrorKind is a complete enum at this scope (forward-
// declared above, outside the export namespace, so no
// circular dep on aura.core.error). The AuraError-struct
// conversion (MutationError → AuraError) lives in
// mutation_impl.cpp where the full AuraError definition
// is visible.

// Issue #474: convert aura::ast::MutationError to the
// unified AuraErrorKind. One-way mapping (some AuraErrorKind
// values don't have a MutationError equivalent; those map
// to InternalInvariantViolation for now). Used by adapters
// at module boundaries during the migration.
export [[nodiscard]] constexpr ::aura::core::AuraErrorKind
mutation_error_to_aura_error_kind(MutationError err) noexcept {
    switch (err) {
        case MutationError::NotCommitted:        return ::aura::core::AuraErrorKind::MutationNotCommitted;
        case MutationError::NoRollbackData:      return ::aura::core::AuraErrorKind::MutationNoRollbackData;
        case MutationError::InvalidTarget:       return ::aura::core::AuraErrorKind::MutationInvalidTarget;
        case MutationError::InvalidParent:       return ::aura::core::AuraErrorKind::MutationInvalidParent;
        case MutationError::InvalidField:        return ::aura::core::AuraErrorKind::MutationInvalidField;
        case MutationError::UnknownStructuralOp: return ::aura::core::AuraErrorKind::MutationUnknownStructuralOp;
        case MutationError::OutOfRange:          return ::aura::core::AuraErrorKind::MutationOutOfRange;
    }
    return ::aura::core::AuraErrorKind::InternalInvariantViolation;
}

// Issue #275: constrain record types that participate in rollback.
export template <typename Rec>
concept RollbackCapable = requires(const Rec& rec) {
    { rec.mutation_id } -> std::convertible_to<std::uint64_t>;
    { rec.status } -> std::convertible_to<MutationStatus>;
    { rec.target_node } -> std::convertible_to<NodeId>;
    { rec.parent_id } -> std::convertible_to<NodeId>;
    { rec.has_rollback_data } -> std::convertible_to<bool>;
    { rec.has_subtree_rollback } -> std::convertible_to<bool>;
    { rec.operator_name } -> std::convertible_to<const std::string&>;
    { rec.field_offset } -> std::convertible_to<std::uint32_t>;
    { rec.old_value } -> std::convertible_to<std::uint64_t>;
    { rec.new_value } -> std::convertible_to<std::uint64_t>;
};

static_assert(RollbackCapable<MutationRecord>);

export enum class RollbackKind : std::uint8_t {
    SubtreeMark,
    Structural,
    ScalarInt,
    ScalarTypeId,
    ScalarSymId,
    ScalarFloat,
};

export enum class StructuralRollbackOp : std::uint8_t {
    SetChild,
    InsertChild,
    RemoveChild,
};

namespace mutation {

export struct MutationRecordParams {
    std::uint64_t mutation_id = 0;
    NodeId target_node = NULL_NODE;
    std::string_view operator_name;
    std::string_view old_type_str;
    std::string_view new_type_str;
    std::string_view summary;
    MutationStatus status = MutationStatus::Committed;
    std::uint32_t field_offset = 0;
    std::uint64_t old_value = 0;
    std::uint64_t new_value = 0;
    bool has_rollback_data = false;
};

export struct SubtreeMutationParams {
    std::uint64_t mutation_id = 0;
    NodeId target_node = NULL_NODE;
    NodeId parent_id = NULL_NODE;
    std::uint32_t child_idx = 0;
    std::string_view old_subtree_source;
    std::string_view operator_name;
    std::string_view summary;
};

export [[nodiscard]] std::uint64_t timestamp_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

export [[nodiscard]] MutationRecord create_mutation_record(const MutationRecordParams& p) {
    return MutationRecord{p.mutation_id,
                          timestamp_ms(),
                          p.target_node,
                          std::string(p.operator_name),
                          std::string(p.old_type_str),
                          std::string(p.new_type_str),
                          std::string(p.summary),
                          p.status,
                          p.field_offset,
                          p.old_value,
                          p.new_value,
                          p.has_rollback_data,
                          NULL_NODE,
                          0,
                          "",
                          false,
                          InvariantStatus::NotChecked};
}

export [[nodiscard]] MutationRecord create_subtree_mutation_record(const SubtreeMutationParams& p) {
    return MutationRecord{p.mutation_id,
                          timestamp_ms(),
                          p.target_node,
                          std::string(p.operator_name),
                          "",
                          "",
                          std::string(p.summary),
                          MutationStatus::Committed,
                          0,
                          0,
                          0,
                          false,
                          p.parent_id,
                          p.child_idx,
                          std::string(p.old_subtree_source),
                          true,
                          InvariantStatus::NotChecked};
}

export [[nodiscard]] std::expected<StructuralRollbackOp, MutationError>
structural_rollback_op(std::string_view op_name) noexcept {
    // Canonical names (used by add_mutation_child_op directly).
    if (op_name == "structural-set-child")
        return StructuralRollbackOp::SetChild;
    if (op_name == "structural-insert-child")
        return StructuralRollbackOp::InsertChild;
    if (op_name == "structural-remove-child")
        return StructuralRollbackOp::RemoveChild;
    // Issue #369: alias map for wrapper-level Aura primitives
    // that mutate the children_ column. The wrappers
    // (mutate:remove-node, mutate:insert-child, etc.) historically
    // called add_mutation() which records the op name but DOES NOT
    // populate field_offset / old_value / new_value, so
    // try_rollback_structural_child_op would return
    // UnknownStructuralOp + leave children_ partially modified.
    // The alias map + the new add_structural_mutation_log_entry
    // helper below close this gap for the 3 most-critical ops:
    // remove-node, insert-child, set-body. The other structural
    // ops (splice, wrap, move-node, replace-pattern, replace-value
    // for children) remain a separate follow-up to wire up
    // add_mutation_child_op at the call sites (each requires
    // knowing parent + child_idx + old_child + new_child, which
    // is site-specific and out of scope for #369).
    if (op_name == "remove-node" || op_name == "remove-child")
        return StructuralRollbackOp::RemoveChild;
    if (op_name == "insert-child")
        return StructuralRollbackOp::InsertChild;
    if (op_name == "set-body" || op_name == "set-child")
        return StructuralRollbackOp::SetChild;
    return std::unexpected(MutationError::UnknownStructuralOp);
}

export template <RollbackCapable Rec>
[[nodiscard]] std::expected<void, MutationError> validate_rollback_record(const Rec& rec,
                                                                        std::size_t tag_size) noexcept {
    if (rec.status != MutationStatus::Committed)
        return std::unexpected(MutationError::NotCommitted);
    if (rec.has_subtree_rollback) {
        if (rec.parent_id == NULL_NODE || rec.parent_id >= tag_size)
            return std::unexpected(MutationError::InvalidParent);
        return {};
    }
    if (!rec.has_rollback_data)
        return std::unexpected(MutationError::NoRollbackData);
    if (rec.operator_name.starts_with("structural-"))
        return {};
    if (rec.target_node >= tag_size)
        return std::unexpected(MutationError::InvalidTarget);
    switch (rec.field_offset) {
        case static_cast<std::uint32_t>(MutationSoAField::IntVal):
        case static_cast<std::uint32_t>(MutationSoAField::TypeId):
        case static_cast<std::uint32_t>(MutationSoAField::SymId):
        case static_cast<std::uint32_t>(MutationSoAField::FloatVal):
            return {};
        default:
            return std::unexpected(MutationError::InvalidField);
    }
}

export template <RollbackCapable Rec>
[[nodiscard]] std::expected<RollbackKind, MutationError> classify_rollback(const Rec& rec) noexcept {
    if (rec.has_subtree_rollback)
        return RollbackKind::SubtreeMark;
    if (!rec.has_rollback_data)
        return std::unexpected(MutationError::NoRollbackData);
    if (rec.operator_name.starts_with("structural-"))
        return RollbackKind::Structural;
    switch (rec.field_offset) {
        case static_cast<std::uint32_t>(MutationSoAField::IntVal):
            return RollbackKind::ScalarInt;
        case static_cast<std::uint32_t>(MutationSoAField::TypeId):
            return RollbackKind::ScalarTypeId;
        case static_cast<std::uint32_t>(MutationSoAField::SymId):
            return RollbackKind::ScalarSymId;
        case static_cast<std::uint32_t>(MutationSoAField::FloatVal):
            return RollbackKind::ScalarFloat;
        default:
            return std::unexpected(MutationError::InvalidField);
    }
}

export [[nodiscard]] std::int64_t scalar_int_old_value(const MutationRecord& rec) noexcept {
    return static_cast<std::int64_t>(rec.old_value);
}

export [[nodiscard]] std::uint32_t scalar_type_old_value(const MutationRecord& rec) noexcept {
    return static_cast<std::uint32_t>(rec.old_value);
}

export [[nodiscard]] SymId scalar_sym_old_value(const MutationRecord& rec) noexcept {
    return static_cast<SymId>(rec.old_value);
}

export [[nodiscard]] double scalar_float_old_value(const MutationRecord& rec) noexcept {
    double old_f = 0.0;
    std::memcpy(&old_f, &rec.old_value, sizeof(old_f));
    return old_f;
}

// ── Wire format (pure serialization) ───────────────────────────
namespace detail {

inline void wire_write_string(std::vector<char>& buf, std::string_view s) {
    auto len = static_cast<std::uint32_t>(s.size());
    buf.insert(buf.end(), reinterpret_cast<const char*>(&len),
               reinterpret_cast<const char*>(&len) + 4);
    buf.insert(buf.end(), s.begin(), s.end());
}

inline std::string wire_read_string(const std::vector<char>& buf, std::size_t& pos) {
    std::uint32_t len;
    std::memcpy(&len, &buf[pos], 4);
    pos += 4;
    std::string s(buf.data() + pos, buf.data() + pos + len);
    pos += len;
    return s;
}

} // namespace detail

export void wire_write_mutation_record(std::vector<char>& buf, const MutationRecord& r) {
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.mutation_id),
               reinterpret_cast<const char*>(&r.mutation_id) + 8);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.timestamp_ms),
               reinterpret_cast<const char*>(&r.timestamp_ms) + 8);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.target_node),
               reinterpret_cast<const char*>(&r.target_node) + 4);
    detail::wire_write_string(buf, r.operator_name);
    detail::wire_write_string(buf, r.old_type_str);
    detail::wire_write_string(buf, r.new_type_str);
    detail::wire_write_string(buf, r.summary);
    auto status = static_cast<std::uint8_t>(r.status);
    buf.push_back(static_cast<char>(status));
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.field_offset),
               reinterpret_cast<const char*>(&r.field_offset) + 4);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.old_value),
               reinterpret_cast<const char*>(&r.old_value) + 8);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.new_value),
               reinterpret_cast<const char*>(&r.new_value) + 8);
    buf.push_back(r.has_rollback_data ? '\1' : '\0');
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.parent_id),
               reinterpret_cast<const char*>(&r.parent_id) + 4);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&r.child_idx),
               reinterpret_cast<const char*>(&r.child_idx) + 4);
    detail::wire_write_string(buf, r.old_subtree_source);
    buf.push_back(r.has_subtree_rollback ? '\1' : '\0');
    auto inv = static_cast<std::uint8_t>(r.invariant_status);
    buf.push_back(static_cast<char>(inv));
}

export MutationRecord wire_read_mutation_record(const std::vector<char>& buf, std::size_t& pos) {
    MutationRecord r;
    std::memcpy(&r.mutation_id, &buf[pos], 8);
    pos += 8;
    std::memcpy(&r.timestamp_ms, &buf[pos], 8);
    pos += 8;
    std::memcpy(&r.target_node, &buf[pos], 4);
    pos += 4;
    r.operator_name = detail::wire_read_string(buf, pos);
    r.old_type_str = detail::wire_read_string(buf, pos);
    r.new_type_str = detail::wire_read_string(buf, pos);
    r.summary = detail::wire_read_string(buf, pos);
    r.status = static_cast<MutationStatus>(static_cast<std::uint8_t>(buf[pos++]));
    std::memcpy(&r.field_offset, &buf[pos], 4);
    pos += 4;
    std::memcpy(&r.old_value, &buf[pos], 8);
    pos += 8;
    std::memcpy(&r.new_value, &buf[pos], 8);
    pos += 8;
    r.has_rollback_data = buf[pos++] != 0;
    std::memcpy(&r.parent_id, &buf[pos], 4);
    pos += 4;
    std::memcpy(&r.child_idx, &buf[pos], 4);
    pos += 4;
    r.old_subtree_source = detail::wire_read_string(buf, pos);
    r.has_subtree_rollback = buf[pos++] != 0;
    r.invariant_status = static_cast<InvariantStatus>(static_cast<std::uint8_t>(buf[pos++]));
    return r;
}

// Issue #276: per-layer NodeId remapping for WorkspaceTree COW layers.
export class NodeIdRemapTable {
public:
    void reset_identity(std::uint32_t parent_layer, std::uint64_t cow_epoch, std::size_t node_count) {
        parent_layer_ = parent_layer;
        cow_epoch_ = cow_epoch;
        node_count_ = node_count;
        parent_to_local_.clear();
        local_to_parent_.clear();
        remap_hits_ = 0;
        remap_misses_ = 0;
    }

    [[nodiscard]] std::uint32_t parent_layer() const noexcept { return parent_layer_; }
    [[nodiscard]] std::uint64_t cow_epoch() const noexcept { return cow_epoch_; }
    [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }
    [[nodiscard]] std::size_t explicit_entry_count() const noexcept {
        return parent_to_local_.size();
    }
    [[nodiscard]] std::uint64_t remap_hits() const noexcept { return remap_hits_; }
    [[nodiscard]] std::uint64_t remap_misses() const noexcept { return remap_misses_; }

    void record_parent_local(NodeId parent_id, NodeId local_id) {
        if (parent_id == local_id) {
            parent_to_local_.erase(parent_id);
            local_to_parent_.erase(local_id);
            return;
        }
        parent_to_local_[parent_id] = local_id;
        local_to_parent_[local_id] = parent_id;
    }

    void record_local_remap(NodeId old_local, NodeId new_local) {
        if (old_local == new_local)
            return;
        const NodeId parent_id = resolve_to_parent(old_local);
        parent_to_local_[parent_id] = new_local;
        local_to_parent_.erase(old_local);
        local_to_parent_[new_local] = parent_id;
    }

    [[nodiscard]] NodeId resolve_from_parent(NodeId parent_id) const noexcept {
        if (parent_id == NULL_NODE)
            return NULL_NODE;
        if (auto it = parent_to_local_.find(parent_id); it != parent_to_local_.end()) {
            ++remap_hits_;
            return it->second;
        }
        ++remap_misses_;
        return parent_id;
    }

    [[nodiscard]] NodeId resolve_to_parent(NodeId local_id) const noexcept {
        if (local_id == NULL_NODE)
            return NULL_NODE;
        if (auto it = local_to_parent_.find(local_id); it != local_to_parent_.end()) {
            ++remap_hits_;
            return it->second;
        }
        ++remap_misses_;
        return local_id;
    }

private:
    std::uint32_t parent_layer_ = 0;
    std::uint64_t cow_epoch_ = 0;
    std::size_t node_count_ = 0;
    std::unordered_map<NodeId, NodeId> parent_to_local_;
    std::unordered_map<NodeId, NodeId> local_to_parent_;
    mutable std::uint64_t remap_hits_ = 0;
    mutable std::uint64_t remap_misses_ = 0;
};

export struct LayerStableRef {
    NodeId id = NULL_NODE;
    std::uint16_t gen = 0;
    std::uint32_t layer_id = 0;
};

// Issue #279 follow-up #4: custom-predicate registry. The
// (register-predicate!) Aura primitive in the evaluator
// module writes here, and the analyzer in the type_checker
// module reads here. Putting it in aura.core.mutation (a
// shared dependency of both) avoids the cross-module symbol
// resolution problem.
export void register_custom_predicate(const std::string& pred_name,
                                      const std::string& type_name);
export std::optional<std::string>
lookup_custom_predicate_type(const std::string& pred_name);

} // namespace mutation
} // namespace aura::ast
