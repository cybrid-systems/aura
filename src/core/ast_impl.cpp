module;

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

} // namespace aura::ast
