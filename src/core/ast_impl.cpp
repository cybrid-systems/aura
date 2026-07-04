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
void fixup_deltas(FlatAST& ast) {
    // Issue #221: the per-node children_ is a PersistentChildVector
    // (immutable + COW). Iterate the read-only span and apply
    // each delta as a set_child (which COW-creates a new PCV
    // for that node).
    for (NodeId id = 0; id < ast.size(); ++id) {
        const auto& list = ast.children(id);
        for (std::uint32_t j = 0; j < list.size(); ++j) {
            NodeId cid = list[j];
            if (cid != NULL_NODE) {
                ast.set_child(id, j, cid + id);
            }
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
    // AURA_PRE
    if (!is_valid(id))
        std::abort();
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

    // Gap sentinel check
    if (m.name == "<gap>") {
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
// Scope-limited first cut: move the non-template post-class items from
// ast.ixx to this impl unit. Templates (MutationVisitor / PureMutationFn
// concepts, MutationFnWrap<F>, run_mutation_* templates) MUST stay in the
// interface unit — templates with external linkage can't be defined in a
// non-exported module implementation unit.
//
// All declarations stay `export` in ast.ixx (declarations only, no body);
// the bodies below live here in module aura.core.ast (no `export`).

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

} // namespace aura::ast
