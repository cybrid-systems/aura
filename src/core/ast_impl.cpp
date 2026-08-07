module;
#include <chrono>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "core/cpp26_contract_stats.h"
#include <contracts>

module aura.core.ast;
import std;
import aura.core.type;

namespace aura::ast {

// ── Patch application ──────────────────────────────────────────
bool apply_patches(FlatAST& ast, std::span<const Patch> patches) {
    // The pre (!patches.empty()) is on the declaration in
    // ast.ixx. The p.node < ast.size() check below is repeated
    // in the loop body (a soft runtime check that returns false
    // on stale id, rather than aborting via contract).
    for (auto& p : patches) {
        if (!ast.is_valid(p.node))
            return false;
        if (p.node >= ast.size())
            return false;
        switch (p.field_offset) {
            case 0:
                ast.tag(p.node) = static_cast<NodeTag>(p.new_value);
                break;
            case 1:
                ast.int_val(p.node) = static_cast<std::int64_t>(p.new_value);
                break;
            case 2:
                ast.sym_id(p.node) = static_cast<SymId>(p.new_value);
                break;
            default:
                return false;
        }
    }
    // Validate all patched nodes (debug builds assert, release returns false on violation)
    for (auto& p : patches) {
        auto err = ast.validate_node(p.node, /*fail_on_error=*/true);
        if (!err.empty())
            return false;
    }
    return true;
}

// ── Delta fixup (for deserialization) ──────────────────────────
// Issue #221 + Issue #2392: some wire / staged encodings store children
// as *deltas relative to the parent NodeId* (absolute = delta + parent_id).
// Call this once after load to rebase deltas → absolute NodeIds.
//
// Contract of set_child (ast.ixx): does NOT clamp out-of-range child ids
// (only updates parent_[child] when child < parent_.size()). Writing a
// raw (delta+id) that wraps uint32 or lands past size() would silently
// plant invalid edges. Issue #2392 therefore checks overflow + bounds and
// writes NULL_NODE for any unsafe rebase (safe tombstone; no corrupt
// absolute id left in children_).
void fixup_deltas(FlatAST& ast) {
    const auto n = ast.size();
    for (NodeId id = 0; id < n; ++id) {
        // Snapshot child count first — set_child COWs the PCV and
        // invalidates a live span from children(id).
        const auto count = static_cast<std::uint32_t>(ast.children(id).size());
        for (std::uint32_t j = 0; j < count; ++j) {
            const auto list = ast.children(id);
            if (j >= list.size())
                break;
            const NodeId delta = list[j];
            if (delta == NULL_NODE)
                continue;

            // Detect uint32 wrap: delta + id must not exceed max NodeId.
            // NULL_NODE is ~0u; any wrap would also be wrong as absolute.
            NodeId rebased = NULL_NODE;
            if (delta <= static_cast<NodeId>(~static_cast<NodeId>(0)) - id) {
                const NodeId cand = static_cast<NodeId>(delta + id);
                // In-range absolute only; cand == NULL_NODE is impossible
                // without wrap (NULL_NODE is max, sum of two non-null
                // would need one zero — but delta != NULL_NODE here).
                if (cand < static_cast<NodeId>(n))
                    rebased = cand;
            }
            // Always write: either rebased absolute or NULL_NODE clamp.
            // (Even when rebased == delta, e.g. parent id==0, still a
            // no-op set that keeps parent_ links consistent.)
            ast.set_child(id, j, rebased);
        }
    }
}

void FlatAST::resolve_type_ids(aura::core::TypeRegistry& reg, StringPool& pool) {
    for (std::size_t i = 0; i < tag_.size(); ++i) {
        if (tag_[i] == NodeTag::TypeAnnotation) {
            auto sym = sym_id_[i];
            auto sv = pool.resolve(sym);
            std::string name(sv);
            if (!name.empty()) {
                auto tid = reg.lookup_type(name);
                if (tid.valid() && i < type_id_.size())
                    type_id_[i] = tid.index;
            }
        }
    }
}

namespace {
    std::string make_node_error(std::uint32_t id, const std::string& msg) {
        return "[node " + std::to_string(id) + "] " + msg;
    }
} // namespace

// ── Node Validation ────────────────────────────────────────────
// Checks invariants defined by kNodeMeta for each node.
// Returns a description of the first violation, or empty string if valid.

std::string FlatAST::validate_node(NodeId id, bool fail_on_error) const {
    // Issue #2390: never hard-abort on !is_valid. Recovery / validate_all
    // / post-restore paths must report (or throw when fail_on_error) so
    // partial / stale workspaces return PostRestoreReport-style diagnostics
    // instead of crashing the process. is_valid is not a true AURA_PRE for
    // reporting callers (fail_on_error=false); assertion-style callers still
    // get std::logic_error when fail_on_error=true.
    if (!is_valid(id)) {
        auto msg = make_node_error(id, "node ID is not valid (generation/epoch mismatch or freed)");
        if (fail_on_error)
            throw std::logic_error(msg);
        return msg;
    }
    if (id >= size())
        return make_node_error(id, "node ID out of range");

    auto tag = tag_[id];
    auto raw_idx = static_cast<std::size_t>(tag) - 1;
    if (raw_idx >= kNodeMeta.size()) {
        auto msg =
            make_node_error(id, "invalid tag value " + std::to_string(static_cast<int>(tag)));
        if (fail_on_error)
            throw std::logic_error(msg);
        return msg;
    }

    auto& m = kNodeMeta[raw_idx];

    // Gap sentinel check (Issue #2411: prefer is_gap over name spoof)
    if (m.is_gap) {
        auto msg = make_node_error(id, "node has gap tag (unused tag value)");
        if (fail_on_error)
            throw std::logic_error(msg);
        return msg;
    }

    // Tag/name consistency
    if (m.tag != tag) {
        auto msg =
            make_node_error(id, "tag mismatch: meta says " + std::string(m.name) +
                                    " but node has tag " + std::to_string(static_cast<int>(tag)));
        if (fail_on_error)
            throw std::logic_error(msg);
        return msg;
    }

    // Issue #220: child_count now lives in children_[id].size()
    // (the per-node std::pmr::vector<NodeId>), not in the
    // legacy child_count_ SoA column (which is gone).
    auto child_count = children(id).size();
    auto fixed = m.fixed_children;

    // Minimum children check
    if (child_count < fixed) {
        auto msg = make_node_error(id, std::string(m.name) + " requires " + std::to_string(fixed) +
                                           " fixed children, got " + std::to_string(child_count));
        if (fail_on_error)
            throw std::logic_error(msg);
        return msg;
    }

    // Variable children check: if has_var_children, child_count must be >= fixed
    // If not has_var_children, child_count must exactly equal fixed (or match a known pattern)
    if (!m.has_var_children && child_count != fixed) {
        // Special case: some nodes with fixed_children=0 have flexible children
        // (Begin/DefineModule) We only enforce exact match for nodes with fixed_children > 0
        if (fixed > 0) {
            auto msg = make_node_error(id, std::string(m.name) + " expects exactly " +
                                               std::to_string(fixed) + " children, got " +
                                               std::to_string(child_count));
            if (fail_on_error)
                throw std::logic_error(msg);
            return msg;
        }
    }

    // String field check
    if (m.has_string && sym_id_[id] == INVALID_SYM) {
        auto msg = make_node_error(id, std::string(m.name) +
                                           " requires a symbol (sym_id), got INVALID_SYM");
        if (fail_on_error)
            throw std::logic_error(msg);
        return msg;
    }

    // Param count check (Lambda-like nodes)
    // Lambda has fixed_children=1 for body, params in separate param arrays
    // The presence of params is checked via has_params flag, not children

    return {}; // valid
}

std::size_t FlatAST::validate_all_nodes(bool fail_on_error) const {
    std::size_t violations = 0;
    for (NodeId id = 0; id < size(); ++id) {
        auto err = validate_node(id, fail_on_error);
        if (!err.empty())
            ++violations;
    }
    return violations;
}

std::size_t FlatAST::validate_all_nodes(std::vector<ValidationError>& errors) const {
    std::size_t count = 0;
    for (NodeId id = 0; id < size(); ++id) {
        auto err = validate_node(id, false);
        if (!err.empty()) {
            ++count;
            // Parse the error string into expected/actual
            ValidationError ve;
            ve.node = id;
            ve.message = err;
            errors.push_back(ve);
        }
    }
    return count;
}

PostRestoreReport FlatAST::validate_post_restore(std::vector<ValidationError>* errors) const {
    PostRestoreReport report;
    report.generation = generation_;

    auto record = [&](NodeId id, std::string msg) {
        ++report.violations;
        if (errors) {
            ValidationError ve;
            ve.node = id;
            ve.message = std::move(msg);
            errors->push_back(std::move(ve));
        }
    };

    // Issue #2391: cross-column SoA size check before per-node walk.
    // add_node() keeps these columns in lockstep with tag_.size(); drift
    // (partial restore / botched mutator) previously passed validation
    // then OOB'd later on int_val_/sym_id_/parent_ indexing.
    // Skip free_list_ (not parallel), value_cache_ (lazy grow), subtree_gen_
    // (lazy), incoming_parent_edges_ (rebuildable / dirty-tolerant).
    const std::size_t sz = size();
    auto record_size_mismatch = [&](const char* col_name, std::size_t col_size) {
        if (col_size != sz) {
            record(NULL_NODE, std::string("SoA column '") + col_name + "' size " +
                                  std::to_string(col_size) + " != size() " + std::to_string(sz));
        }
    };
    record_size_mismatch("tag_", tag_.size());
    record_size_mismatch("int_val_", int_val_.size());
    record_size_mismatch("float_val_", float_val_.size());
    record_size_mismatch("sym_id_", sym_id_.size());
    record_size_mismatch("children_", children_.size());
    record_size_mismatch("parent_", parent_.size());
    record_size_mismatch("node_gen_", node_gen_.size());
    record_size_mismatch("type_id_", type_id_.size());
    record_size_mismatch("type_cache_gen_", type_cache_gen_.size());
    record_size_mismatch("type_cache_binding_gen_", type_cache_binding_gen_.size());
    record_size_mismatch("marker_", marker_.size());
    record_size_mismatch("provenance_", provenance_.size());
    record_size_mismatch("dirty_", dirty_.size());
    record_size_mismatch("ppa_dirty_", ppa_dirty_.size());
    record_size_mismatch("verify_dirty_", verify_dirty_.size());
    record_size_mismatch("verification_dirty_", verification_dirty_.size());
    record_size_mismatch("macro_dirty_", macro_dirty_.size());
    record_size_mismatch("line_", line_.size());
    record_size_mismatch("col_", col_.size());
    record_size_mismatch("schema_cache_", schema_cache_.size());
    record_size_mismatch("error_kind_", error_kind_.size());
    record_size_mismatch("occ_stale_", occ_stale_.size());
    record_size_mismatch("param_begin_", param_begin_.size());
    record_size_mismatch("param_count_", param_count_.size());
    record_size_mismatch("cap_require_count_", cap_require_count_.size());
    record_size_mismatch("node_first_mutation_", node_first_mutation_.size());
    record_size_mismatch("last_seen_epoch_", last_seen_epoch_.size());

    if (generation_ == 0)
        record(NULL_NODE, "generation_ is zero (invalid workspace epoch)");

    for (NodeId id = 0; id < size(); ++id) {
        const bool has_gen = id < node_gen_.size();
        const bool live = has_gen && node_gen_[id] == generation_;
        const bool tombstone = has_gen && node_gen_[id] == 0;

        if (has_gen) {
            if (live)
                ++report.live_nodes;
            else if (tombstone)
                ++report.free_slots;
            else
                record(id, "slot generation neither live nor tombstone");
        } else if (id < tag_.size()) {
            record(id, "node_gen_ entry missing for occupied slot");
        }

        if (!live)
            continue;

        for (auto child : children(id)) {
            if (child == NULL_NODE)
                continue;
            if (child >= size()) {
                record(id, "child " + std::to_string(child) + " out of range");
                continue;
            }
            if (child >= node_gen_.size() || node_gen_[child] != generation_) {
                record(id, "child " + std::to_string(child) + " is not live");
                continue;
            }
            if (parent_of(child) != id)
                record(id, "child " + std::to_string(child) + " parent_ mismatch");
        }

        auto parent = parent_of(id);
        if (parent == NULL_NODE)
            continue;
        if (parent >= size() || parent >= node_gen_.size() || node_gen_[parent] != generation_) {
            record(id, "parent " + std::to_string(parent) + " is not live");
            continue;
        }
        bool listed = false;
        for (auto child : children(parent)) {
            if (child == id) {
                listed = true;
                break;
            }
        }
        if (!listed)
            record(id, "parent " + std::to_string(parent) + " does not list node as child");
    }

    return report;
}

// ── Issue #378: post-class free functions + non-template visitors ──────
//
// Non-template post-class items live here. MutationVisitor /
// run_mutation_pipeline templates live in aura.core.ast_mutation_pipeline
// (FlatAST decomp step 2). Visitor class declarations stay exported from
// ast.ixx; bodies below are module aura.core.ast (no `export`).

// ── StableNodeRef + MutationRecord helpers ───────────────────
FlatAST::StableNodeRef mutation_target_ref(const FlatAST& flat,
                                           const MutationRecord& rec) noexcept {
    return flat.make_ref(rec.target_node);
}

FlatAST::StableNodeRef mutation_parent_ref(const FlatAST& flat,
                                           const MutationRecord& rec) noexcept {
    return flat.make_ref(rec.parent_id);
}

bool is_mutation_target_valid(const FlatAST& flat, const MutationRecord& rec) noexcept {
    return flat.is_valid(mutation_target_ref(flat, rec));
}

bool is_mutation_parent_valid(const FlatAST& flat, const MutationRecord& rec) noexcept {
    return rec.parent_id == NULL_NODE || flat.is_valid(mutation_parent_ref(flat, rec));
}

// ── Example mutation visitors ──────────────────────────────────
void MutationCountVisitor::visit_mutation(FlatAST&, const MutationRecord& rec) {
    if (rec.status == MutationStatus::Committed)
        ++committed_count_;
    ++total_count_;
}

bool MutationCountVisitor::has_error() const {
    return false;
}

std::size_t MutationCountVisitor::total_count() const {
    return total_count_;
}

std::size_t MutationCountVisitor::committed_count() const {
    return committed_count_;
}

void MutationTargetValidityVisitor::visit_mutation(FlatAST& flat, const MutationRecord& rec) {
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

bool MutationTargetValidityVisitor::has_error() const {
    return has_error_;
}

// Issue #276: resolve a captured stable ref across workspace layers.
std::optional<FlatAST::StableNodeRef>
resolve_across_layer(const FlatAST& target_flat, const mutation::NodeIdRemapTable& layer_remap,
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

// ── Issue #2456: single-TU find_first_node_with specializations ──
//
// Named functors (not per-call lambdas) so the recursive
// find_first_node_with template has one stable P type per
// predicate and is instantiated only in this implementation
// unit. Callers of subtree_uses_sym / find_define_by_name
// share those specializations (no per-importer lambda types).
namespace {

    struct VariableUsesSymPred {
        const FlatAST* self;
        SymId sym;
        [[nodiscard]] bool operator()(NodeId id) const {
            auto v = self->get(id);
            return v.tag == NodeTag::Variable && v.sym_id == sym;
        }
    };

    struct DefineSymPred {
        const FlatAST* self;
        SymId sym;
        [[nodiscard]] bool operator()(NodeId id) const {
            auto v = self->get(id);
            return v.tag == NodeTag::Define && v.sym_id == sym;
        }
    };

} // namespace

bool FlatAST::subtree_uses_sym(NodeId root, SymId sym) const {
    if (root == NULL_NODE || root >= size())
        return false;
    VariableUsesSymPred pred{this, sym};
    auto found = find_first_node_with<std::uint32_t>(*this, root, pred);
    return found.has_value();
}

std::optional<NodeId> FlatAST::find_define_by_name(const StringPool& pool, std::string_view name,
                                                   std::optional<NodeId> search_root) const {
    const auto sym = pool.find_by_name(name);
    if (!sym)
        return std::nullopt;
    const auto start = search_root.value_or(root);
    if (start == NULL_NODE || start >= size())
        return std::nullopt;
    DefineSymPred pred{this, *sym};
    return find_first_node_with<std::uint32_t>(*this, start, pred);
}


// ── FlatAST dirty/generation methods (decomp step 3) ───────────
// Bodies moved from ast.ixx to shrink the FlatAST interface unit.
// Declarations + contracts remain on FlatAST in ast.ixx.

// --- FlatAST::is_subtree_dirty_node ---
bool FlatAST::is_subtree_dirty_node(NodeId id) const noexcept {
    // Issue #2424 Option B: do not call size() (non-atomic tag_).
    // NULL_NODE is ~0u and fails the dirty_.size() check below.
    if (id == NULL_NODE)
        return false;
    std::shared_lock<std::shared_mutex> rlock(dirty_column_mtx_.mutable_get());
    if (static_cast<std::size_t>(id) >= dirty_.size())
        return false; // not built / not yet grown / OOB
    return dirty_[static_cast<std::size_t>(id)] != 0;
}

// --- FlatAST::dirty_nodes_in_range ---
std::size_t FlatAST::dirty_nodes_in_range(NodeId start, NodeId end) const noexcept {
    if (start >= end)
        return 0;
    // Issue #2424: cap against dirty_.size() under shared lock
    // (same Option B invariant as is_subtree_dirty_node).
    std::shared_lock<std::shared_mutex> rlock(dirty_column_mtx_.mutable_get());
    if (dirty_.empty())
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

// --- FlatAST::mark_dirty_upward_with_index_update ---
void FlatAST::mark_dirty_upward_with_index_update(NodeId id) {
    mark_dirty_upward(id); // already exclusive-patches under #2419
    if (id == NULL_NODE || id >= size())
        return;
    // mark_dirty_upward already live-patched under tag_arity lock.
}

// --- FlatAST::apply_verify_dirty_bits ---
void FlatAST::apply_verify_dirty_bits(NodeId id, std::uint8_t verify_reasons) {
    if (verify_reasons == 0)
        return;
    std::uint8_t newly_set = 0;
    {
        std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
        if (id >= verify_dirty_.size())
            verify_dirty_.resize(id + 1, 0);
        // Issue #2440: atomic fetch_or (prev bits → newly_set).
        const auto prev = std::atomic_ref<std::uint8_t>(verify_dirty_[id])
                              .fetch_or(verify_reasons, std::memory_order_acq_rel);
        newly_set = static_cast<std::uint8_t>(verify_reasons & ~prev);
    }
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

// --- FlatAST::apply_verification_dirty_bits ---
void FlatAST::apply_verification_dirty_bits(NodeId id, std::uint8_t reasons) {
    if (reasons == 0)
        return;
    std::uint8_t newly_set = 0;
    {
        std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
        if (id >= verification_dirty_.size())
            verification_dirty_.resize(id + 1, 0);
        // Issue #2440: atomic fetch_or for newly_set metrics.
        const auto prev = std::atomic_ref<std::uint8_t>(verification_dirty_[id])
                              .fetch_or(reasons, std::memory_order_acq_rel);
        newly_set = static_cast<std::uint8_t>(reasons & ~prev);
    }
    if (newly_set & kCoverageFeedbackDirty)
        verification_coverage_feedback_total_.fetch_add(1, std::memory_order_relaxed);
    if (newly_set & kAssertFailureDirty)
        verification_assert_failure_total_.fetch_add(1, std::memory_order_relaxed);
    mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
}

// --- FlatAST::mark_dirty_verification ---
void FlatAST::mark_dirty_verification(NodeId id) {
    apply_verification_dirty_bits(
        id, static_cast<std::uint8_t>(kCoverageFeedbackDirty | kAssertFailureDirty));
}

// --- FlatAST::mark_dirty_verification_upward ---
void FlatAST::mark_dirty_verification_upward(NodeId id) {
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

// --- FlatAST::clear_verification_dirty ---
void FlatAST::clear_verification_dirty(NodeId id) {
    std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
    if (id < verification_dirty_.size())
        std::atomic_ref<std::uint8_t>(verification_dirty_[id]).store(0, std::memory_order_release);
}

// --- FlatAST::clear_verification_dirty_for ---
void FlatAST::clear_verification_dirty_for(NodeId id, std::uint8_t verify_mask) {
    std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
    if (id < verification_dirty_.size()) {
        auto& cell = verification_dirty_[id];
        auto cur = std::atomic_ref<std::uint8_t>(cell).load(std::memory_order_relaxed);
        std::atomic_ref<std::uint8_t>(cell).store(static_cast<std::uint8_t>(cur & ~verify_mask),
                                                  std::memory_order_release);
    }
}

// --- FlatAST::apply_macro_dirty_bits ---
void FlatAST::apply_macro_dirty_bits(NodeId id, std::uint8_t reasons) {
    if (reasons == 0)
        return;
    std::uint8_t newly_set = 0;
    {
        std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
        if (id >= macro_dirty_.size())
            macro_dirty_.resize(id + 1, 0);
        // Issue #2441: atomic fetch_or (prev bits → newly_set).
        const auto prev = std::atomic_ref<std::uint8_t>(macro_dirty_[id])
                              .fetch_or(reasons, std::memory_order_acq_rel);
        newly_set = static_cast<std::uint8_t>(reasons & ~prev);
    }
    if (newly_set & kMacroExpansion)
        macro_expansion_dirty_total_.fetch_add(1, std::memory_order_relaxed);
    if (newly_set & kMacroSelfModify)
        macro_self_modify_dirty_total_.fetch_add(1, std::memory_order_relaxed);
    mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
}

// --- FlatAST::clear_macro_dirty_all ---
void FlatAST::clear_macro_dirty_all() noexcept {
    std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
    for (auto& b : macro_dirty_)
        std::atomic_ref<std::uint8_t>(b).store(0, std::memory_order_relaxed);
}

// --- FlatAST::apply_ppa_dirty_bits ---
void FlatAST::apply_ppa_dirty_bits(NodeId id, std::uint8_t ppa_reasons) {
    if (ppa_reasons == 0)
        return;
    if (id >= ppa_dirty_.size())
        ppa_dirty_.resize(id + 1, 0);
    ppa_dirty_[id] |= ppa_reasons;
    mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty | kPpaHintDirty));
}

// --- FlatAST::mark_dirty ---
void FlatAST::mark_dirty(NodeId id, std::uint8_t reasons) {
    // Issue #2423: exclusive dirty_column_mtx_ for resize + RMW of
    // dirty_[id]. Readers (is_subtree_dirty_node / dirty_nodes_in_range)
    // hold shared. Side effects outside dirty_ (cache clear, restamp
    // touch, occurrence stale, epoch bump) run after unlock.
    {
        std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
        if (id >= dirty_.size())
            dirty_.resize(id + 1, 0);
        dirty_[id] |= reasons;
        // Issue #1519: post — requested reason bits are set after stamp.
        contract_assert(reasons == 0 || (dirty_[id] & reasons) == reasons);
    }
    // Issue #2122: seed wrap-window touched set for incremental restamp.
    note_restamp_touched(id);
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
    // Issue #2440: atomic fetch_add under exclusive lock
    // (no torn 64-bit RMW; only if column already sized).
    {
        std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get());
        if (id < last_seen_epoch_.size())
            std::atomic_ref<std::uint64_t>(last_seen_epoch_[id])
                .fetch_add(1, std::memory_order_relaxed);
    }
}

// --- FlatAST::mark_dirty_for_reinfer ---
void FlatAST::mark_dirty_for_reinfer(NodeId id, std::uint64_t current_epoch) {
    mark_dirty(id, static_cast<std::uint8_t>(kGeneralDirty));
    stamp_last_seen_epoch(id, current_epoch);
}

// --- FlatAST::mark_subtree_dirty ---
void FlatAST::mark_subtree_dirty(NodeId id, std::uint8_t reasons, std::uint8_t ppa_reasons) {
    mark_dirty(id, reasons);
    apply_ppa_dirty_bits(id, ppa_reasons);
    auto v = get(id);
    for (auto c : v.children) {
        if (c != NULL_NODE)
            mark_subtree_dirty(c, reasons, ppa_reasons);
    }
}

// --- FlatAST::clear_dirty ---
void FlatAST::clear_dirty(NodeId id) {
    if (id < dirty_.size())
        dirty_[id] = 0;
}

// --- FlatAST::clear_dirty_for ---
void FlatAST::clear_dirty_for(NodeId id, std::uint8_t reason_mask) {
    if (id < dirty_.size())
        dirty_[id] &= static_cast<std::uint8_t>(~reason_mask);
}

// --- FlatAST::clear_ppa_dirty ---
void FlatAST::clear_ppa_dirty(NodeId id) {
    if (id < ppa_dirty_.size())
        ppa_dirty_[id] = 0;
}

// --- FlatAST::clear_ppa_dirty_for ---
void FlatAST::clear_ppa_dirty_for(NodeId id, std::uint8_t ppa_mask) {
    if (id < ppa_dirty_.size())
        ppa_dirty_[id] &= static_cast<std::uint8_t>(~ppa_mask);
}

// --- FlatAST::clear_dirty_for_subtree ---
void FlatAST::clear_dirty_for_subtree(NodeId id, std::uint8_t reason_mask) {
    if (id < dirty_.size())
        dirty_[id] &= static_cast<std::uint8_t>(~reason_mask);
    auto v = get(id);
    for (auto c : v.children) {
        if (c != NULL_NODE)
            clear_dirty_for_subtree(c, reason_mask);
    }
}

// --- FlatAST::has_dirty_subtree ---
bool FlatAST::has_dirty_subtree(NodeId id) const {
    if (is_dirty(id))
        return true;
    auto v = get(id);
    for (auto c : v.children) {
        if (c != NULL_NODE && has_dirty_subtree(c))
            return true;
    }
    return false;
}

// --- FlatAST::clear_all_dirty ---
void FlatAST::clear_all_dirty() {
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

// --- FlatAST::restamp_all_node_generations ---
void FlatAST::restamp_all_node_generations() {
    const auto t0 = std::chrono::steady_clock::now();
    const bool wrap_recovery = auto_restamp_pending_.load(std::memory_order_relaxed);
    std::uint64_t restamped = 0;
    std::vector<std::uint8_t> on_free(size(), 0);
    for (NodeId fid : free_list_) {
        if (fid < on_free.size())
            on_free[fid] = 1;
    }
    // Seed touched from mutation log targets (structural + field records).
    if (wrap_recovery) {
        for (const auto& rec : mutation_log_) {
            note_restamp_touched(rec.target_node);
            note_restamp_touched(rec.parent_id);
        }
    }
    std::uint64_t live = 0;
    std::uint64_t touched_live = 0;
    for (NodeId id = 0; id < size(); ++id) {
        if (on_free[id] || id >= node_gen_.size())
            continue;
        ++live;
        if (id < restamp_touched_.size() && restamp_touched_[id])
            ++touched_live;
    }
    const RestampPolicy policy = restamp_policy();
    bool use_incremental = false;
    bool lazy_only = false; // #2402 Incremental + empty cone
    if (wrap_recovery && live > 0) {
        if (policy == RestampPolicy::Full) {
            use_incremental = false;
        } else if (policy == RestampPolicy::Incremental) {
            if (touched_live > 0)
                use_incremental = true;
            else
                lazy_only = true; // no dirty → skip O(N) full walk
        } else {
            // Auto (#2122): density-gated when any dirty; empty → full.
            if (touched_live > 0) {
                const auto dens_bp = static_cast<std::uint64_t>(touched_live * 10000ull / live);
                use_incremental = dens_bp <= restamp_incremental_density_threshold_bp_;
            }
        }
    }
    if (lazy_only) {
        // Issue #2402: wrap with no dirty under Incremental policy —
        // enable lazy gen-align only (zero eager restamp). wrap_epoch
        // still invalidates pre-wrap StableNodeRefs.
        // Issue #2421: release so is_valid/make_ref see the enable.
        restamp_lazy_align_enabled_.store(true, std::memory_order_release);
        restamped = 0;
    } else if (use_incremental) {
        // Issue #2122 / #2402 AC1: restamp only dirty/touched cone.
        for (NodeId id = 0; id < size(); ++id) {
            if (on_free[id] || id >= node_gen_.size())
                continue;
            if (id < restamp_touched_.size() && restamp_touched_[id]) {
                node_gen_[id] = generation_;
                ++restamped;
            }
        }
        restamp_incremental_nodes_total_.fetch_add(restamped, std::memory_order_relaxed);
        restamp_lazy_align_enabled_.store(true, std::memory_order_release);
    } else {
        // Full live restamp (Auto empty/dense fallback / Full / non-wrap).
        if (wrap_recovery)
            restamp_full_fallback_total_.fetch_add(1, std::memory_order_relaxed);
        for (NodeId id = 0; id < size(); ++id) {
            if (!on_free[id] && id < node_gen_.size()) {
                node_gen_[id] = generation_;
                ++restamped;
            }
        }
        restamp_lazy_align_enabled_.store(false, std::memory_order_release);
    }
    // Clear touched window after wrap recovery consume.
    if (wrap_recovery && !restamp_touched_.empty())
        std::fill(restamp_touched_.begin(), restamp_touched_.end(), 0);
    const auto t1 = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const auto us_u = static_cast<std::uint64_t>(us);
    restamp_nodes_total_.fetch_add(restamped, std::memory_order_relaxed);
    restamp_us_total_.fetch_add(us_u, std::memory_order_relaxed);
    // Issue #2402: last-call cost for Agent SLO dashboards.
    restamp_nodes_last_.store(restamped, std::memory_order_relaxed);
    restamp_us_last_.store(us_u, std::memory_order_relaxed);
    // Issue #2528: SLO breach detection + max-approximation p99.
    // Zero cost when no restamp runs (the only call site). Each call
    // past the configured AURA_RESTAMP_SLO_US budget bumps the breach
    // counter; p99 is a CAS-loop max (cheap, no histogram buffer).
    const auto slo_budget_us = static_cast<std::uint64_t>(restamp_slo_us_budget());
    if (us_u > slo_budget_us)
        restamp_slo_breach_total_.fetch_add(1, std::memory_order_relaxed);
    auto p99 = restamp_us_p99_.load(std::memory_order_relaxed);
    while (us_u > p99 && !restamp_us_p99_.compare_exchange_weak(
                             p99, us_u, std::memory_order_relaxed, std::memory_order_relaxed)) {
        // retry until either us_u <= p99 (someone else raised it) or CAS succeeds.
    }
    // Issue #1282: if restamp was pending due to uint16 wrap,
    // clear the flag and count the recovery (Agent-visible via
    // ast:generation-stats / production-sweep-1281-1285-stats).
    if (auto_restamp_pending_.exchange(false, std::memory_order_relaxed)) {
        auto_restamp_on_wrap_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

// --- FlatAST::note_restamp_touched ---
void FlatAST::note_restamp_touched(NodeId id) noexcept {
    if (id == NULL_NODE || id >= size())
        return;
    if (restamp_touched_.size() < size())
        restamp_touched_.resize(size(), 0);
    if (id < restamp_touched_.size())
        restamp_touched_[id] = 1;
}

// --- FlatAST::restamp_macro_introduced_generations ---
std::size_t FlatAST::restamp_macro_introduced_generations() {
    std::vector<std::uint8_t> on_free(size(), 0);
    for (NodeId fid : free_list_) {
        if (fid < on_free.size())
            on_free[fid] = 1;
    }
    constexpr auto kExpansion = static_cast<std::uint8_t>(MacroDirtyReason::kMacroExpansion);
    std::size_t restamped = 0;
    const auto n = size();
    if (parent_.size() < n)
        parent_.resize(n, NULL_NODE);
    for (NodeId id = 0; id < n; ++id) {
        if (on_free[id] || !is_macro_introduced(id))
            continue;
        if (id < node_gen_.size())
            node_gen_[id] = generation_;
        // Parent/child consistency: MacroIntroduced node owns its children.
        if (id < children_.size()) {
            for (NodeId cid : children_[id]) {
                if (cid != NULL_NODE && cid < parent_.size())
                    parent_[cid] = id;
            }
        }
        // macro_dirty: ensure kMacroExpansion without double-counting
        // when already set (Issue #2441: apply uses atomic fetch_or).
        apply_macro_dirty_bits(id, kExpansion);
        ++restamped;
    }
    if (restamped > 0) {
        macro_restamp_after_flat_total_.fetch_add(1, std::memory_order_relaxed);
        if (auto_restamp_pending_.exchange(false, std::memory_order_relaxed))
            auto_restamp_on_wrap_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return restamped;
}

// --- FlatAST::restamp_macro_introduced_subtree ---
std::size_t FlatAST::restamp_macro_introduced_subtree(NodeId root) {
    if (root == NULL_NODE || root >= size())
        return 0;
    constexpr auto kExpansion = static_cast<std::uint8_t>(MacroDirtyReason::kMacroExpansion);
    std::size_t restamped = 0;
    std::vector<NodeId> stack;
    std::vector<std::uint8_t> seen(size(), 0);
    seen[root] = 1;
    stack.push_back(root);
    while (!stack.empty()) {
        auto id = stack.back();
        stack.pop_back();
        if (id == NULL_NODE || id >= size() || seen[id] == 0)
            continue;
        seen[id] = 1;
        if (is_macro_introduced(id)) {
            if (id < node_gen_.size())
                node_gen_[id] = generation_;
            // Parent/child consistency: MacroIntroduced node owns its children.
            if (id < children_.size()) {
                for (NodeId cid : children_[id]) {
                    if (cid != NULL_NODE && cid < parent_.size())
                        parent_[cid] = id;
                }
            }
            // kMacroExpansion dirty bit — idempotent OR (#2441 fetch_or).
            apply_macro_dirty_bits(id, kExpansion);
            ++restamped;
        }
        // Descend into children (MacroIntroduced + User + ...).
        for (NodeId cid : children(id)) {
            if (cid != NULL_NODE && cid < size() && seen[cid] == 0) {
                stack.push_back(cid);
            }
        }
    }
    if (restamped > 0)
        macro_expand_mutate_restamp_total_.fetch_add(1, std::memory_order_relaxed);
    return restamped;
}

// --- FlatAST::maybe_auto_restamp_on_wrap ---
void FlatAST::maybe_auto_restamp_on_wrap() {
    if (!auto_restamp_pending_.load(std::memory_order_relaxed))
        return;
    restamp_all_node_generations();
}

// --- FlatAST::restamp_subtree_generation ---
void FlatAST::restamp_subtree_generation(NodeId root) {
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

// --- FlatAST::bump_generation ---
void FlatAST::bump_generation() noexcept {
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

// --- FlatAST::bump_generation_subtree ---
void FlatAST::bump_generation_subtree(NodeId subtree_root) noexcept {
    if (subtree_root == NULL_NODE || subtree_root >= size())
        return;
    auto top = top_define_of(subtree_root);
    if (top == NULL_NODE)
        return; // no enclosing Define → cannot scope
    // Issue #2422: exclusive for resize + atomic store of gen cell.
    {
        std::unique_lock<std::shared_mutex> wlock(subtree_gen_mtx_.mutable_get());
        if (subtree_gen_.size() < size())
            subtree_gen_.resize(size(), 0);
        // Advance the per-subtree counter for this top-level
        // Define. Same uint16_t wrap semantics as the global
        // generation_ (1..65535, skip 0).
        auto word =
            std::atomic_ref<std::uint32_t>(subtree_gen_[top]).load(std::memory_order_relaxed);
        auto sg = static_cast<std::uint16_t>(word & 0xFFFFu);
        ++sg;
        if (sg == 0) {
            sg = 1;
            subtree_bump_count_.fetch_add(1, std::memory_order_relaxed);
        }
        std::atomic_ref<std::uint32_t>(subtree_gen_[top])
            .store(static_cast<std::uint32_t>(sg), std::memory_order_release);
    }
    // Bump the global generation_ so existing is_valid()
    // continues to behave as before (backward compat).
    bump_generation();
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

// --- FlatAST::bump_generation_on_rollback ---
void FlatAST::bump_generation_on_rollback() {
    ++generation_;
    if (generation_ == 0)
        generation_ = 1;
    if (!defer_rollback_restamp_)
        restamp_all_node_generations();
}


// --- multi-line signature dirty methods ---
// --- FlatAST::mark_dirty_upward ---
void FlatAST::mark_dirty_upward(const NodeId id, std::uint8_t reasons, std::uint8_t ppa_reasons) {
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
                                src_tag == NodeTag::Coverpoint || src_tag == NodeTag::Constraint ||
                                src_tag == NodeTag::Class);
    }
    // Issue #2440: use locked atomic accessor (not raw column).
    if (!propagate_sva_verify)
        propagate_sva_verify = (verify_dirty(id) & kSvaDirty) != 0;
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
    // Issue #1503 / #2419: live-patch under exclusive map lock.
    {
        std::unique_lock<std::shared_mutex> wlock(tag_arity_index_mtx_.mutable_get());
        if (!tag_arity_index_.empty() && id < size())
            patch_tag_arity_index_node(id);
    }
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
        if ((tgv.tag == NodeTag::Define || tgv.tag == NodeTag::Let || tgv.tag == NodeTag::LetRec) &&
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
    (void)invalidate_narrowings_in_subtree(id,
                                           type_cache_generation_.load(std::memory_order_relaxed));
}

// --- FlatAST::mark_dirty_upward_fast ---
void FlatAST::mark_dirty_upward_fast(const NodeId id, std::uint8_t reasons,
                                     std::uint8_t ppa_reasons, int max_depth,
                                     bool stop_at_boundary) {
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
    // Issue #1503 / #2419: live-patch seed under exclusive map lock.
    {
        std::unique_lock<std::shared_mutex> wlock(tag_arity_index_mtx_.mutable_get());
        if (!tag_arity_index_.empty() && id < size())
            patch_tag_arity_index_node(id);
    }
    type_cache_generation_.fetch_add(1, std::memory_order_relaxed);
    // Issue #1455: fast upward path must invalidate
    // NarrowingRecords + occ_stale_ the same way as
    // mark_dirty_upward (predicate-affecting mutates
    // often use _fast).
    (void)invalidate_narrowings_in_subtree(id,
                                           type_cache_generation_.load(std::memory_order_relaxed));
}

// --- FlatAST::mark_dirty_upward_until ---
void FlatAST::mark_dirty_upward_until(NodeId id, std::uint8_t reasons, NodeId stop_at,
                                      std::uint8_t ppa_reasons) {
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


// --- FlatAST::mark_ppa_dirty ---
void FlatAST::mark_ppa_dirty(NodeId id, std::uint8_t ppa_reasons) {
    apply_ppa_dirty_bits(id, ppa_reasons);
}

} // namespace aura::ast
