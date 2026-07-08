module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module aura.compiler.type_checker;
import std;
import aura.core.mutation;
// Issue #411 fu1 follow-up #3: per-DefUseIndex tracker
// for O(uses) re-inference routing. The header is shared
// with CompilerService (see per_defuse_index.h).
#include "per_defuse_index.h"
// Issue #409: full CompilerMetrics definition so
// solve_delta_impl can bump the new per-#409 counters
// (delta_constraints_processed_total /
// delta_constraints_total). The forward decl in the
// type_checker.ixx is sufficient for the metrics_
// pointer, but the fetch_add on the atomic needs the
// full type.
#include "observability_metrics.h"

namespace aura::compiler {

using namespace aura::ast;
using namespace aura::core;
using namespace aura::diag;

static void collect_linear_bindings_under_nodes(const FlatAST& flat, const StringPool& pool,
                                                const std::vector<NodeId>& nodes,
                                                std::unordered_set<std::string>& out);

static void bump_linear_occurrence_predicate_safe(void* metrics) {
    if (!metrics)
        return;
    static_cast<struct CompilerMetrics*>(metrics)->linear_occurrence_predicate_safe_total.fetch_add(
        1, std::memory_order_relaxed);
}

static bool subtree_has_linear_ops(const FlatAST& flat, NodeId root) {
    std::function<bool(NodeId)> walk = [&](NodeId id) -> bool {
        if (id == NULL_NODE || id >= flat.size())
            return false;
        const auto v = flat.get(id);
        if (v.tag == NodeTag::Linear || v.tag == NodeTag::Move || v.tag == NodeTag::Borrow ||
            v.tag == NodeTag::MutBorrow || v.tag == NodeTag::Drop)
            return true;
        for (auto c : v.children) {
            if (c != NULL_NODE && walk(c))
                return true;
        }
        return false;
    };
    return walk(root);
}

// ── Edit distance for "did you mean" suggestions ────────────────
static std::size_t edit_distance(std::string_view a, std::string_view b) {
    auto m = a.size(), n = b.size();
    if (m == 0)
        return n;
    if (n == 0)
        return m;
    std::vector<std::size_t> prev(n + 1), cur(n + 1);
    for (std::size_t j = 0; j <= n; ++j)
        prev[j] = j;
    for (std::size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= n; ++j) {
            auto cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

// Issue #102: Type hole detection. The two sentinels we accept
// in a TypeAnnotation's sym_id position: `_` and `:?`. Both are
// LLM-friendly ways to say "infer this type from context". Returns
// true if the type name is one of the sentinels, false otherwise.
// `_` is a single ASCII underscore; `:?` is a colon followed by a
// question mark. Both intern to distinct SymIds; the detection is
// by string content, not by SymId identity, because the parser
// interns the literal token text directly.
static bool is_type_hole(std::string_view type_name) {
    if (type_name.size() == 1 && type_name[0] == '_')
        return true;
    if (type_name.size() == 2 && type_name[0] == ':' && type_name[1] == '?')
        return true;
    return false;
}

static std::string closest_match(std::string_view name, const std::vector<std::string>& candidates,
                                 std::size_t max_dist = 3) {
    std::string best;
    std::size_t best_dist = max_dist + 1;
    for (auto& c : candidates) {
        auto d = edit_distance(name, c);
        if (d < best_dist) {
            best_dist = d;
            best = c;
        }
    }
    return best;
}

// ── Gradual typing: type_tag for CoercionNode ────────────
// Maps types to coercion tags used by CastOp at runtime.
// 0=Int, 1=String, 2=Bool, 3=Dynamic, 4=Float
static std::uint32_t type_tag_for_coercion(aura::core::TypeId tid,
                                           const aura::core::TypeRegistry* type_reg) {
    if (!type_reg)
        return 3;
    auto tag = type_reg->tag_of(tid);
    switch (tag) {
        case aura::core::TypeTag::INT:
            return 0;
        case aura::core::TypeTag::STRING:
            return 1;
        case aura::core::TypeTag::BOOL:
            return 2;
        case aura::core::TypeTag::FLOAT:
            return 4;
        default:
            return 3; // DYNAMIC / unknown
    }
}

// ═══════════════════════════════════════════════════════════
// TypeEnv
// ═══════════════════════════════════════════════════════════

TypeEnv::TypeEnv(TypeRegistry& reg)
    : reg_(reg) {
    scopes_.emplace_back();
}

void TypeEnv::push_scope() {
    scopes_.emplace_back();
}
void TypeEnv::pop_scope() {
    if (scopes_.size() > 1)
        scopes_.pop_back();
}

void TypeEnv::bind(std::string name, TypeId type) {
    // Issue #71: auto-detect polymorphism. If the bound type is a
    // Forall wrapper, mark the binding as polymorphic so lookup
    // knows to instantiate it.
    bool is_poly = reg_.forall_of(type) != nullptr;
    scopes_.back()[std::move(name)] = Binding{type, is_poly, {}};
}

TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) {
            // Issue #71: let-polymorphism. If the binding is
            // polymorphic, return a fresh instantiation so each
            // use site gets its own copy. (Without this, bound
            // vars would leak across use sites and unification
            // would conflict.)
            if (f->second.is_poly) {
                return reg_.instantiate_forall(f->second.type, {});
            }
            return f->second.type;
        }
    }
    return TypeId{}; // invalid = not found
}

bool TypeEnv::is_bound(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        if (it->count(name))
            return true;
    return false;
}

void TypeEnv::collect_names(std::vector<std::string>& out) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        for (auto& [name, _] : *it)
            out.push_back(name);
}

// ═══════════════════════════════════════════════════════════
// ConstraintSystem — Union-Find based
// ═══════════════════════════════════════════════════════════

ConstraintSystem::ConstraintSystem(TypeRegistry& reg)
    : reg_(reg) {}

void ConstraintSystem::add(Constraint c) {
    const auto new_idx = constraints_.size();
    constraints_.push_back(std::move(c));
    // Issue #148: keep constraint_dirty_ in sync with constraints_.
    // New constraints added via plain add() are NOT marked dirty
    // (they're committed to the full-solve path). add_delta() is
    // the entry point for incremental callers.
    if (constraint_dirty_.size() < constraints_.size())
        constraint_dirty_.resize(constraints_.size(), false);
    // Issue #409: maintain the var → constraints reverse
    // map. Use find() to normalize the var to its
    // Union-Find rep (the rep is what unify produces and
    // what downstream var_to_constraints_ lookups should
    // key on). The append-only vec can have stale entries
    // for constraints that are no longer dirty — those are
    // filtered at lookup time by the dirty bit check.
    if (c.lhs.valid()) {
        auto rep = find(c.lhs).index;
        var_to_constraints_[rep].push_back(new_idx);
    }
    if (c.rhs.valid()) {
        auto rep = find(c.rhs).index;
        var_to_constraints_[rep].push_back(new_idx);
    }
}

// Issue #148: incremental constraint add. Same as add() but
// marks the new constraint dirty so solve_delta can pick it
// up. The dirty flag is append-only (no tombstoning); the
// constraint is simply excluded from subsequent solve()
// scans once mark_clean() or clear() is called.
std::uint32_t ConstraintSystem::union_find_rep_index(TypeId id) const {
    if (!reg_.is_var(id) || id.index >= parent_.size())
        return UINT32_MAX;
    auto idx = static_cast<std::size_t>(id.index);
    if (parent_[idx] < 0)
        return static_cast<std::uint32_t>(idx);
    auto p = static_cast<std::int64_t>(idx);
    while (static_cast<std::size_t>(p) < parent_.size() &&
           parent_[static_cast<std::size_t>(p)] >= 0 &&
           static_cast<std::size_t>(parent_[static_cast<std::size_t>(p)]) !=
               static_cast<std::size_t>(p)) {
        p = parent_[static_cast<std::size_t>(p)];
    }
    return static_cast<std::uint32_t>(p);
}

void ConstraintSystem::note_touched_var(TypeId id) {
    const auto rep = union_find_rep_index(id);
    if (rep == UINT32_MAX)
        return;
    touched_roots_.insert(rep);
}

void ConstraintSystem::mark_touched_on_delta(TypeId var, bool occurrence_narrow) {
    note_touched_var(var);
    if (!occurrence_narrow)
        return;
    const auto rep = union_find_rep_index(var);
    if (rep != UINT32_MAX)
        occurrence_priority_roots_.insert(rep);
}

std::size_t ConstraintSystem::effective_reverify_limit() const noexcept {
    const std::size_t impact =
        dirty_count_ * 8 + touched_roots_.size() * 4 + occurrence_priority_roots_.size() * 16;
    const std::size_t scaled = std::max(kReverifyCleanScanLimit, impact);
    return std::min(scaled, kReverifyCleanScanMax);
}

void ConstraintSystem::record_cross_delta_blame_hit() {
    if (!metrics_) {
        return;
    }
    auto* m = static_cast<struct CompilerMetrics*>(metrics_);
    if (active_mutation_id_ == 0) {
        m->constraint_stale_blame_invalidation_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m->constraint_blame_chain_complete_total.fetch_add(1, std::memory_order_relaxed);
}

int ConstraintSystem::constraint_reverify_priority(std::size_t idx) const {
    if (idx >= constraints_.size())
        return 0;
    const auto& c = constraints_[idx];
    int pri = 0;
    auto score_var = [&](TypeId id) {
        if (!id.valid())
            return;
        if (reg_.is_var(id)) {
            const auto raw_rep = union_find_rep_index(id);
            if (raw_rep != UINT32_MAX) {
                if (occurrence_priority_roots_.count(raw_rep) > 0)
                    pri = std::max(pri, 4);
                else if (touched_roots_.count(raw_rep) > 0)
                    pri = std::max(pri, 1);
                auto it = var_to_constraints_.find(raw_rep);
                if (it != var_to_constraints_.end()) {
                    const auto deg = static_cast<int>(std::min<std::size_t>(it->second.size(), 8));
                    pri += deg;
                }
            }
        }
        const auto norm = const_cast<ConstraintSystem*>(this)->find(id);
        if (!reg_.is_var(norm))
            return;
        const auto rep = union_find_rep_index(norm);
        if (rep == UINT32_MAX)
            return;
        if (occurrence_priority_roots_.count(rep) > 0)
            pri = std::max(pri, 4);
        else if (touched_roots_.count(rep) > 0)
            pri = std::max(pri, 1);
    };
    score_var(c.lhs);
    score_var(c.rhs);
    return pri;
}

bool ConstraintSystem::constraint_references_touched(const Constraint& c) const {
    auto refs_touched = [&](TypeId id) {
        if (!id.valid())
            return false;
        const auto norm = const_cast<ConstraintSystem*>(this)->find(id);
        if (!reg_.is_var(norm))
            return false;
        const auto rep = const_cast<ConstraintSystem*>(this)->find_var(norm);
        if (!reg_.is_var(rep))
            return false;
        return touched_roots_.count(rep.index) > 0;
    };
    return refs_touched(c.lhs) || refs_touched(c.rhs);
}

bool ConstraintSystem::reverify_clean_constraints_for_touched() {
    if (touched_roots_.empty() && occurrence_priority_roots_.empty())
        return true;

    const bool saved_record = delta_record_mode_;
    delta_record_mode_ = false;

    std::unordered_set<std::size_t> to_check;
    to_check.reserve((touched_roots_.size() + occurrence_priority_roots_.size()) * 2);
    auto collect_clean_for_root = [&](std::uint32_t root) {
        auto it = var_to_constraints_.find(root);
        if (it == var_to_constraints_.end())
            return;
        for (auto idx : it->second) {
            if (idx >= constraints_.size())
                continue;
            if (idx < constraint_dirty_.size() && constraint_dirty_[idx])
                continue;
            to_check.insert(idx);
        }
    };
    for (auto root : touched_roots_)
        collect_clean_for_root(root);
    // Issue #745: always re-verify clean constraints tied to
    // Occurrence-narrowed roots, even when this delta's unify
    // only touched unrelated roots.
    for (auto root : occurrence_priority_roots_)
        collect_clean_for_root(root);

    const auto scan_limit = effective_reverify_limit();
    const bool truncated = to_check.size() > scan_limit;

    if (metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(metrics_);
        m->delta_conflict_reverify_total.fetch_add(1, std::memory_order_relaxed);
        if (truncated) {
            m->reverify_truncated_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!truncated && to_check.size() > kReverifyCleanScanLimit && metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(metrics_);
        m->constraint_reverify_timeout_prevented_total.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::pair<int, std::size_t>> ordered;
    ordered.reserve(to_check.size());
    for (auto idx : to_check)
        ordered.emplace_back(constraint_reverify_priority(idx), idx);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::size_t scanned = 0;
    for (const auto& [pri, idx] : ordered) {
        if (scanned++ >= scan_limit)
            break;
        const auto& c = constraints_[idx];
        if (pri >= 4 && metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(metrics_);
            m->constraint_reverify_narrow_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
        bool ok = false;
        if (c.kind == Constraint::EQUAL)
            ok = unify(c.lhs, c.rhs);
        else
            ok = consistent_unify(c.lhs, c.rhs);
        if (!ok) {
            if (metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(metrics_);
                m->delta_conflict_detected_total.fetch_add(1, std::memory_order_relaxed);
            }
            record_cross_delta_blame_hit();
            delta_record_mode_ = saved_record;
            return false;
        }
    }
    delta_record_mode_ = saved_record;
    return true;
}

void ConstraintSystem::add_delta(Constraint c) {
    const auto lhs = c.lhs;
    const auto rhs = c.rhs;
    if (delta_record_mode_) {
        if (lhs.valid() && reg_.is_var(lhs))
            note_touched_var(lhs);
        if (rhs.valid() && reg_.is_var(rhs))
            note_touched_var(rhs);
    }
    const auto new_idx = constraints_.size();
    constraints_.push_back(std::move(c));
    if (constraint_dirty_.size() <= new_idx)
        constraint_dirty_.resize(new_idx + 1, false);
    if (!constraint_dirty_[new_idx]) {
        constraint_dirty_[new_idx] = true;
        ++dirty_count_;
    }
    // Issue #409: also maintain the reverse map for
    // delta constraints. See add() above for rationale.
    if (lhs.valid()) {
        auto rep = find(lhs).index;
        var_to_constraints_[rep].push_back(new_idx);
    }
    if (rhs.valid()) {
        auto rep = find(rhs).index;
        var_to_constraints_[rep].push_back(new_idx);
    }
}

TypeId ConstraintSystem::fresh_var() {
    return fresh_var_named("");
}

TypeId ConstraintSystem::fresh_var_named(std::string_view hint) {
    // Issue #79: if a hint is provided, use it (sanitized) as the var name
    // so error messages show the user's variable name. Otherwise fall back
    // to the auto-generated __t<N>.
    std::string name;
    if (!hint.empty()) {
        name.reserve(hint.size());
        for (char c : hint) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                name.push_back(c);
            else
                name.push_back('_');
        }
    }
    if (name.empty())
        name = "__t" + std::to_string(fresh_counter_++);
    auto id = reg_.make_var(name);
    // Ensure Union-Find arrays are sized for this variable
    auto idx = static_cast<std::size_t>(id.index);
    if (idx >= parent_.size()) {
        parent_.resize(idx + 64, -1);
        rank_.resize(idx + 64, 0);
        binding_.resize(idx + 64, TypeId{});
    }
    // Initialize as root (self-parent). Note: clear() sets parent to -1
    // to indicate uninitialized; each fresh_var must set its own parent.
    if (parent_[idx] < 0)
        parent_[idx] = static_cast<std::int64_t>(idx);
    if (first_free_var_ == 0 || id.index < first_free_var_)
        first_free_var_ = id.index;
    return id;
}

// Find representative with path compression
std::int64_t find_rep(const std::vector<std::int64_t>& parent, std::int64_t idx) {
    while (idx >= 0 && static_cast<std::size_t>(idx) < parent.size() &&
           parent[static_cast<std::size_t>(idx)] >= 0 &&
           parent[static_cast<std::size_t>(idx)] != idx) {
        idx = parent[static_cast<std::size_t>(idx)];
    }
    return idx;
}

TypeId ConstraintSystem::find_var(TypeId id) {
    if (!reg_.is_var(id) || id.index >= parent_.size())
        return id;
    auto idx = static_cast<std::size_t>(id.index);
    // Uninitialized variable (parent = -1) — not yet used in any unification
    if (parent_[idx] < 0)
        return TypeId{static_cast<std::uint32_t>(idx), id.generation};
    auto p = static_cast<std::int64_t>(idx);
    // Path compression: find root
    while (static_cast<std::size_t>(p) < parent_.size() &&
           parent_[static_cast<std::size_t>(p)] >= 0 &&
           static_cast<std::size_t>(parent_[static_cast<std::size_t>(p)]) !=
               static_cast<std::size_t>(p)) {
        p = parent_[static_cast<std::size_t>(p)];
    }
    auto root = static_cast<std::size_t>(p);
    // Compress path: make all nodes on the path point directly to root
    auto q = static_cast<std::int64_t>(idx);
    while (static_cast<std::size_t>(q) < parent_.size() &&
           parent_[static_cast<std::size_t>(q)] >= 0 &&
           static_cast<std::size_t>(parent_[static_cast<std::size_t>(q)]) !=
               static_cast<std::size_t>(q)) {
        auto next = parent_[static_cast<std::size_t>(q)];
        parent_[static_cast<std::size_t>(q)] = static_cast<std::int64_t>(root);
        q = next;
    }
    // If root has a binding, return the bound type (concrete type or root variable)
    if (root < binding_.size() && binding_[root].valid())
        return binding_[root];
    // Return the root as a type variable
    return TypeId{static_cast<std::uint32_t>(root), id.generation};
}

// Find with full type resolution (normalize via Union-Find)
TypeId ConstraintSystem::find(TypeId id) {
    if (reg_.is_var(id) && id.index < parent_.size() && parent_[id.index] != -1) {
        auto found = find_var(id);
        if (found != id)
            return find(found);
        return found;
    }
    // Recurse into compound types
    if (auto* f = reg_.func_of(id)) {
        bool changed = false;
        std::vector<TypeId> new_args;
        for (auto& a : f->args) {
            auto na = find(a);
            new_args.push_back(na);
            if (na != a)
                changed = true;
        }
        auto new_ret = find(f->ret);
        if (new_ret != f->ret)
            changed = true;
        if (changed)
            return reg_.register_func(std::move(new_args), new_ret);
    }
    if (auto* ft = reg_.forall_of(id)) {
        auto new_body = find(ft->body);
        if (new_body != ft->body)
            return reg_.register_forall(ft->var, new_body);
    }
    return id;
}

TypeId ConstraintSystem::normalize(TypeId id) {
    return find(id);
}

bool ConstraintSystem::occurs_check(TypeId var, TypeId ty) {
    if (!reg_.is_var(var))
        return false;
    ty = find(ty);
    if (var == ty)
        return true;

    // FuncType
    if (auto* f = reg_.func_of(ty)) {
        for (auto a : f->args)
            if (occurs_check(var, a))
                return true;
        return occurs_check(var, f->ret);
    }

    // ForallType
    if (auto* ft = reg_.forall_of(ty)) {
        return occurs_check(var, ft->body);
    }

    // LinearType
    if (auto* lt = reg_.linear_of(ty)) {
        return occurs_check(var, lt->inner);
    }

    // ModuleType
    if (auto* mt = reg_.module_of(ty)) {
        for (auto& [name, t] : mt->members)
            if (occurs_check(var, t))
                return true;
    }

    // VariantType
    if (auto* vt = reg_.variant_of(ty)) {
        for (auto& [name, args] : vt->variants)
            for (auto& a : args)
                if (occurs_check(var, a))
                    return true;
    }

    // RecordType
    if (auto* rt = reg_.record_of(ty)) {
        for (auto& [name, t] : rt->fields)
            if (occurs_check(var, t))
                return true;
    }

    return false;
}

bool ConstraintSystem::unify(TypeId t1, TypeId t2) {
    t1 = find(t1);
    t2 = find(t2);
    if (t1 == t2)
        return true;

    // Assign variable to type
    if (reg_.is_var(t1)) {
        if (occurs_check(t1, t2))
            return false;
        auto idx = static_cast<std::size_t>(t1.index);
        if (idx >= parent_.size()) {
            parent_.resize(idx + 64, -1);
            rank_.resize(idx + 64, 0);
            binding_.resize(idx + 64, TypeId{});
        }
        if (parent_[idx] < 0)
            parent_[idx] = static_cast<std::int64_t>(idx);
        if (!reg_.is_var(t2)) {
            // Bind variable to concrete type
            binding_[idx] = t2;
            note_touched_var(t1);
        } else {
            auto idx2 = static_cast<std::size_t>(t2.index);
            if (idx2 >= parent_.size()) {
                parent_.resize(idx2 + 64, -1);
                rank_.resize(idx2 + 64, 0);
                binding_.resize(idx2 + 64, TypeId{});
            }
            if (parent_[idx2] < 0)
                parent_[idx2] = static_cast<std::int64_t>(idx2);
            auto r1 = idx;
            auto r2 = idx2;
            // Find roots via find_var for proper path compression
            auto f1 = find_var(t1);
            auto f2 = find_var(t2);
            if (reg_.is_var(f1))
                r1 = static_cast<std::size_t>(f1.index);
            if (reg_.is_var(f2))
                r2 = static_cast<std::size_t>(f2.index);
            if (r1 == r2)
                return true;
            if (rank_[r1] < rank_[r2]) {
                std::swap(r1, r2);
                std::swap(idx, idx2);
            }
            parent_[r2] = static_cast<std::int64_t>(r1);
            if (rank_[r1] == rank_[r2])
                rank_[r1]++;
            // Issue #745: preserve Occurrence-narrow priority across merges.
            if (occurrence_priority_roots_.count(static_cast<std::uint32_t>(r2)) > 0 ||
                occurrence_priority_roots_.count(static_cast<std::uint32_t>(r1)) > 0) {
                occurrence_priority_roots_.insert(static_cast<std::uint32_t>(r1));
                occurrence_priority_roots_.erase(static_cast<std::uint32_t>(r2));
            }
            note_touched_var(TypeId{static_cast<std::uint32_t>(r1), 1});
            note_touched_var(TypeId{static_cast<std::uint32_t>(r2), 1});
            // Merge bindings: if r2 had a binding, move to r1
            if (binding_[r2].valid()) {
                if (!binding_[r1].valid()) {
                    binding_[r1] = binding_[r2];
                } else if (binding_[r1] != binding_[r2]) {
                    return false; // conflicting bindings
                }
            }
            // Issue #409: merge the var_to_constraints_
            // reverse maps. r2's constraints are now
            // r1's constraints (they reference the
            // same var rep). We append r2's vec to
            // r1's vec and erase r2's entry. Duplicate
            // indices are harmless (the worklist is
            // deduped by the solve loop). The
            // append-only pattern keeps the merge O(
            // |r2 entries|) — no need to rewrite r1's
            // vec.
            {
                auto it_r2 = var_to_constraints_.find(static_cast<std::uint32_t>(r2));
                if (it_r2 != var_to_constraints_.end()) {
                    auto& dst = var_to_constraints_[static_cast<std::uint32_t>(r1)];
                    dst.insert(dst.end(), it_r2->second.begin(), it_r2->second.end());
                    var_to_constraints_.erase(it_r2);
                }
            }
        }
        return true;
    }
    if (reg_.is_var(t2))
        return unify(t2, t1);

    // Function type decomposition
    auto* f1 = reg_.func_of(t1);
    auto* f2 = reg_.func_of(t2);
    if (f1 && f2) {
        if (f1->args.size() != f2->args.size())
            return false;
        for (std::size_t i = 0; i < f1->args.size(); i++)
            if (!unify(f1->args[i], f2->args[i]))
                return false;
        return unify(f1->ret, f2->ret);
    }

    // Nominal equality for non-variable, non-function types
    return t1 == t2;
}

bool ConstraintSystem::consistent_unify(TypeId t1, TypeId t2) {
    // Issue #383: bump the consistent_unify counter
    // for observability. Every call (success or
    // failure) bumps it. The full #383 scope also
    // strengthens consistent_unify edge cases with
    // Dynamic + Forall + Occurrence mixtures; this
    // slice ships the observability foundation.
    if (metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(metrics_);
        m->consistent_unify_total.fetch_add(1, std::memory_order_relaxed);
    }
    const auto orig_lhs = t1;
    const auto orig_rhs = t2;
    if (delta_record_mode_)
        add_delta(Constraint{Constraint::CONSISTENT, orig_lhs, orig_rhs});
    t1 = find(t1);
    t2 = find(t2);

    // Issue #117: reject Any ~ Linear. Linear resources must
    // be statically tracked from allocation to consumption;
    // allowing them to flow through a Dynamic boundary would
    // silently erase the ownership invariant. The right
    // escape hatch is an explicit coercion (e.g. (cast x T))
    // which produces a CastOp at the boundary.
    if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type()) {
        auto other_id = (t1 == reg_.dynamic_type()) ? t2 : t1;
        if (reg_.linear_of(other_id) != nullptr) {
            return false;
        }
    }

    // Any consistent with everything (sound gradual core)
    if (t1 == reg_.dynamic_type() || t2 == reg_.dynamic_type()) {
        // If one side is Any and the other is a type variable, bind the
        // variable to Any.  This prevents the free var from escaping into
        // let-polymorphism generalization where it would be ∀-quantified,
        // destroying the Any boundary (soundness fix, #18).
        if (reg_.is_var(t1))
            unify(t1, reg_.dynamic_type());
        else if (reg_.is_var(t2))
            unify(t2, reg_.dynamic_type());
        return true;
    }

    // Nominal equality
    if (t1 == t2)
        return true;

    // Strict unify for variables (via Union-Find)
    if (reg_.is_var(t1) || reg_.is_var(t2))
        return unify(t1, t2);

    // Function type decomposition (consistent)
    // Uses consistent_subtype with proper variance:
    //   (-> T1 T2) ~ (-> T1' T2')   when   T1' <:sub T1  AND  T2 <:sub T2'
    // i.e. parameter contravariance, return covariance
    auto* f1 = reg_.func_of(t1);
    auto* f2 = reg_.func_of(t2);
    if (f1 && f2) {
        if (f1->args.size() != f2->args.size())
            return false;
        // Parameter contravariance: T2_i <:sub T1_i
        for (std::size_t i = 0; i < f1->args.size(); i++)
            if (!consistent_subtype(f2->args[i], f1->args[i]))
                return false;
        // Return covariance: T1_ret <:sub T2_ret
        return consistent_subtype(f1->ret, f2->ret);
    }

    // Ground type consistency: any two ground/base types are CONSISTENT
    // (they may need runtime coercion, but the type system allows it)
    if (!reg_.is_var(t1) && !reg_.is_var(t2) && !f1 && !f2) {
        return true;
    }

    return false;
}

bool ConstraintSystem::consistent_subtype(TypeId sub, TypeId sup) {
    // Issue #383: bump the consistent_subtype
    // counter for observability. See consistent_unify
    // above for the full rationale.
    if (metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(metrics_);
        m->consistent_subtype_total.fetch_add(1, std::memory_order_relaxed);
    }
    sub = find(sub);
    sup = find(sup);

    // Any is the top type: everything is a subtype of Any
    // (including Any itself — reflexivity)
    if (sup == reg_.dynamic_type())
        return true;
    // Nothing is a subtype of a non-Any ground type if sub is Any
    // (Any <: Int fails — insert runtime check instead)
    if (sub == reg_.dynamic_type() && sup != reg_.dynamic_type())
        return true; // consistent_subtype allows it (runtime coercion)

    // Reflexivity
    if (sub == sup)
        return true;

    // Type variables: unify them (consistent assignment)
    if (reg_.is_var(sub) || reg_.is_var(sup))
        return unify(sub, sup);

    // Function subtype with variance
    auto* f_sub = reg_.func_of(sub);
    auto* f_sup = reg_.func_of(sup);
    if (f_sub && f_sup) {
        if (f_sub->args.size() != f_sup->args.size())
            return false;
        // Parameter contravariance: sup.args[k] <:sub sub.args[k]
        for (std::size_t i = 0; i < f_sub->args.size(); i++)
            if (!consistent_subtype(f_sup->args[i], f_sub->args[i]))
                return false;
        // Return covariance: sub.ret <:sub sup.ret
        return consistent_subtype(f_sub->ret, f_sup->ret);
    }

    // Non-function, non-variable ground types are consistent
    // (runtime coercion applies)
    return true;
}

// Issue #118: returns SolveResult instead of bool. Distinguishes
// the three outcomes (SOLVED / CONFLICT / TIMEOUT) so the caller
// can react appropriately. On TIMEOUT, `unresolved_out` (if
// non-null) is filled with the constraints that remained on the
// worklist.
SolveResult ConstraintSystem::solve(std::vector<Constraint>* unresolved_out) {
    // Worklist: process all constraints, then re-process until fixpoint
    std::vector<std::size_t> worklist;
    for (std::size_t i = 0; i < constraints_.size(); i++)
        worklist.push_back(i);

    std::size_t max_passes = 10; // prevent infinite loops
    while (!worklist.empty() && max_passes-- > 0) {
        auto current = std::move(worklist);
        worklist.clear();
        for (auto idx : current) {
            auto& c = constraints_[idx];
            bool ok;
            if (c.kind == Constraint::EQUAL)
                ok = unify(c.lhs, c.rhs);
            else
                ok = consistent_unify(c.lhs, c.rhs);
            if (!ok)
                return SolveResult::CONFLICT;
            // Re-check remaining constraints if we resolved new variables
        }
        // Re-add constraints whose variables were just resolved
        // In Union-Find, unification is persistent, so no need to re-check
        // unless there's a specific need. For completeness, we check if
        // any constraint's variables now resolve differently.
    }

    // Issue #118: distinguish TIMEOUT from SOLVED. Previously the
    // function returned `true` regardless of whether the worklist
    // was empty or not, which silently masked partial / under-
    // constrained programs. The new behavior: if the worklist is
    // non-empty when the pass limit is reached, return TIMEOUT
    // and (optionally) report the unresolved constraints to the
    // caller.
    if (!worklist.empty()) {
        if (unresolved_out) {
            for (auto idx : worklist) {
                unresolved_out->push_back(constraints_[idx]);
            }
        }
        return SolveResult::TIMEOUT;
    }
    return SolveResult::SOLVED;
}

// Issue #148: incremental solve. Iterates only the dirty
// subset of constraints_ (those added via add_delta since the
// last mark_clean / clear). Issue #432 / #466: post-pass
// reverify_clean_constraints_for_touched() catches cross-delta
// unification conflicts against clean constraints (bounded scan
// over var_to_constraints_ for touched Union-Find roots).
SolveResult ConstraintSystem::solve_delta(std::vector<Constraint>* unresolved_out) {
    // Issue #258: time the delta solve. The timer accumulates
    // into CompilerMetrics::delta_solve_time_us (lifetime total)
    // via the metrics_ pointer. The RAII guard ensures the
    // elapsed time is captured even on early-return paths.
    // We use a void pointer (forward-declared via the
    // observability_metrics.h header) to avoid pulling the
    // full CompilerMetrics definition into this TU.
    if (metrics_) {
        auto start = std::chrono::steady_clock::now();
        auto result = solve_delta_impl(unresolved_out);
        auto end = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        // Forward-declared CompilerMetrics in observability_metrics.h.
        // We use the offset of delta_solve_time_us via the
        // metrics_ type's known layout — see header for the
        // ordering. The metrics_ pointer is opaque here.
        struct MetricsAccess {
            std::atomic<std::uint64_t> delta_solve_time_us{0};
        };
        static_cast<MetricsAccess*>(metrics_)->delta_solve_time_us.fetch_add(
            static_cast<std::uint64_t>(us), std::memory_order_relaxed);
        return result;
    }
    return solve_delta_impl(unresolved_out);
}

// Issue #258: solve_delta() body split out so the timer wrapper
// above can wrap it cleanly. The actual algorithm is unchanged
// from Issue #148 Phase 2 — only the structure is different
// (moved into a private impl method).
SolveResult ConstraintSystem::solve_delta_impl(std::vector<Constraint>* unresolved_out) {
    if (dirty_count_ == 0)
        return SolveResult::SOLVED;

    // Build the delta worklist from constraint_dirty_.
    // Issue #409: this is the pre-#409 path. The
    // post-#409 worklist is filtered by the var_to_
    // constraints_ reverse map (only include
    // constraints that reference a dirty var rep).
    // We keep the old path as a fallback when the
    // reverse map is empty (e.g. for callers that
    // haven't gone through the fresh_var/unify
    // path that populates it).
    std::vector<std::size_t> worklist;
    worklist.reserve(dirty_count_);
    if (var_to_constraints_.empty()) {
        // Legacy path: process all dirty constraints.
        for (std::size_t i = 0; i < constraint_dirty_.size(); ++i) {
            if (constraint_dirty_[i]) {
                worklist.push_back(i);
            }
        }
    } else {
        // Issue #409: fine-grained path. Walk the
        // reverse map and collect every constraint
        // index that is currently dirty AND referenced
        // by at least one var rep. We do not filter
        // by which var is "dirty" (the dirty flag is
        // on constraints, not vars) — instead, we
        // collect all reverse-mapped dirty constraint
        // indices. This is still a strict subset of
        // the full worklist when the reverse map is
        // populated (constraints that reference no
        // var are skipped — those are int/float/
        // string equalities that don't need re-solving
        // unless they were added via add_delta).
        std::vector<bool> seen(constraints_.size(), false);
        for (const auto& [rep, indices] : var_to_constraints_) {
            (void)rep;
            for (auto idx : indices) {
                if (idx < constraint_dirty_.size() && constraint_dirty_[idx] && !seen[idx]) {
                    worklist.push_back(idx);
                    seen[idx] = true;
                }
            }
        }
    }

    // Process the delta worklist. Same pass-limit heuristic as
    // solve() — the delta set is small, so 10 passes is enough
    // for fixpoint in practice. If a delta is large enough to
    // need more, the caller can re-invoke solve() for a full pass.
    // Issue #409: bump the processed counter for the metric.
    // The counter is plumbed via metrics_ to
    // CompilerMetrics::delta_constraints_processed_total
    // by the caller (CompilerService). Pre-#409 the
    // counter didn't exist; the worklist size was the
    // signal but not surfaced.
    if (metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(metrics_);
        m->delta_constraints_processed_total.fetch_add(worklist.size(), std::memory_order_relaxed);
        m->delta_constraints_total.fetch_add(dirty_count_, std::memory_order_relaxed);
    }
    std::size_t max_passes = 10;
    // Issue #383: worklist_restart detection. The
    // pre-#383 worklist was a single-pass per
    // worklist vector; if processing added new
    // constraints (e.g. via unify → consistent_unify
    // → register_forall), they'd sit in the
    // dirty set but not be processed until the
    // next solve_delta call. Post-#383, we
    // re-scan the dirty set at the end of each
    // pass and append any new entries to the
    // worklist (a "restart"). The restart count is
    // bumped into the lifetime counter for
    // observability.
    auto worklist_initial_size = worklist.size();
    while (!worklist.empty() && max_passes-- > 0) {
        auto current = std::move(worklist);
        worklist.clear();
        // Issue #383: capture the dirty set size
        // before the pass; if it grew, the pass
        // added new constraints and we need a
        // restart to process them.
        const auto dirty_before = dirty_count_;
        for (auto idx : current) {
            // Skip indices that are out of range (defensive: a
            // stale dirty bit for a constraint that was later
            // removed — shouldn't happen with add-only deltas
            // but the check is cheap).
            if (idx >= constraints_.size())
                continue;
            auto& c = constraints_[idx];
            bool ok;
            if (c.kind == Constraint::EQUAL)
                ok = unify(c.lhs, c.rhs);
            else
                ok = consistent_unify(c.lhs, c.rhs);
            if (!ok) {
                if (metrics_) {
                    auto* m = static_cast<struct CompilerMetrics*>(metrics_);
                    m->delta_conflict_detected_total.fetch_add(1, std::memory_order_relaxed);
                }
                record_cross_delta_blame_hit();
                if (!touched_roots_.empty() && on_cross_delta_conflict_)
                    on_cross_delta_conflict_();
                return SolveResult::CONFLICT;
            }
        }
        // Issue #383: worklist restart detection. If
        // the dirty set grew during the pass (new
        // constraints added by unify /
        // consistent_unify / register_forall), append
        // the new entries to the worklist for the
        // next pass. Bump the lifetime counter for
        // observability.
        if (dirty_count_ > dirty_before) {
            if (metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(metrics_);
                m->worklist_restart_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Re-scan constraint_dirty_ for new
            // entries that weren't in the original
            // worklist. We walk the full dirty
            // vector (cheap) and pick up any new
            // dirty entries.
            for (std::size_t i = 0; i < constraint_dirty_.size(); ++i) {
                if (constraint_dirty_[i]) {
                    bool already_in =
                        std::find(worklist.begin(), worklist.end(), i) != worklist.end();
                    bool was_in_initial =
                        std::find(current.begin(), current.end(), i) != current.end();
                    if (!already_in && !was_in_initial) {
                        worklist.push_back(i);
                    }
                }
            }
        }
    }

    // Issue #466: lightweight re-verify of clean constraints
    // whose vars intersect touched_roots_. Catches cross-delta
    // conflicts that dirty-only processing can miss.
    if (!touched_roots_.empty() && on_touched_roots_snapshot_)
        on_touched_roots_snapshot_(touched_roots_.size());
    if (!reverify_clean_constraints_for_touched()) {
        if (on_cross_delta_conflict_)
            on_cross_delta_conflict_();
        clear_touched_roots();
        return SolveResult::CONFLICT;
    }
    clear_touched_roots();

    // Mark all currently-dirty constraints as clean (the
    // delta is committed). New add_delta calls after this
    // point will re-mark their indices dirty.
    mark_clean();

    if (!worklist.empty()) {
        if (unresolved_out) {
            for (auto idx : worklist) {
                if (idx < constraints_.size())
                    unresolved_out->push_back(constraints_[idx]);
            }
        }
        return SolveResult::TIMEOUT;
    }
    return SolveResult::SOLVED;
}

void ConstraintSystem::clear() {
    constraints_.clear();
    parent_.assign(parent_.size(), -1);
    rank_.assign(rank_.size(), 0);
    binding_.assign(binding_.size(), TypeId{});
    // Issue #148: reset delta tracking.
    constraint_dirty_.clear();
    dirty_count_ = 0;
    // Issue #409: reset the reverse map.
    var_to_constraints_.clear();
    // Issue #466 / #745: reset touched-root + occurrence-priority tracking.
    touched_roots_.clear();
    occurrence_priority_roots_.clear();
    fresh_counter_ = 0;
    first_free_var_ = 0;
} // ═══════════════════════════════════════════════════════════
// InferenceEngine
// ═══════════════════════════════════════════════════════════

InferenceEngine::InferenceEngine(TypeRegistry& reg, DiagnosticCollector& diag)
    : reg_(reg)
    , diag_(diag)
    , cs_(reg)
    , env_(reg) {
    init_primitive_env();
    // Bind declared type sigs (from inject_type_sigs) to the env.
    // We use the explicit name → TypeId map (declared_sigs_) set by
    // TypeChecker::infer_flat, instead of scanning the registry for
    // __decl_ prefix. The old scan had a latent bug exposed by
    // TypeId interning (Issue #70 follow-up #1): when multiple names
    // shared the same TypeId via dedup, the last writer won the name
    // field, so only the last name was bound to the env. (See the
    // 312-5 / test_aura_type_multi_func regression.)
    // The declared_sigs_ map preserves every name → TypeId binding
    // regardless of TypeId sharing.
    // (The __decl_ scan is intentionally removed; the registry
    // names are now purely for formatting / debugging.)
}

void InferenceEngine::bind_declared_sigs() {
    // Bind each declared name to its TypeId in the env. Called by
    // TypeChecker::infer_flat after constructing the engine, so
    // the explicit name → TypeId map (set post-construction) takes
    // effect. (See ctor comment for why we don't scan the registry.)
    for (auto& [name, tid] : declared_sigs_) {
        if (tid.valid() && !name.empty()) {
            env_.bind(name, tid);
        }
    }
}


bool InferenceEngine::is_coercible(TypeId from, TypeId to) {
    if (from == to)
        return true;
    // Issue #117: reject Linear ~ Dynamic. Linear resources
    // are statically tracked; allowing them to flow through a
    // Dynamic boundary (even with a runtime CastOp) would
    // silently erase the ownership invariant. The user must
    // be explicit: either the value is linear all the way
    // through, or the boundary is annotated with a real cast
    // (not a silent one).
    if (reg_.linear_of(from) != nullptr || reg_.linear_of(to) != nullptr) {
        return false;
    }
    // Dynamic coerce to/from anything (gradual core, always allowed)
    if (from == reg_.dynamic_type() || to == reg_.dynamic_type())
        return true;
    // Issue #79: In strict mode, cross-type coercions are TypeErrors,
    // not silent "Notes" that pass through has_errors() == false. We
    // only allow numeric narrowing (Float → Int) because that's a real
    // number-narrows-to-integer operation, not a stringification.
    if (strict_) {
        auto from_tag = reg_.tag_of(from);
        auto to_tag = reg_.tag_of(to);
        // Float → Int is the only cross-type coercion allowed in strict mode
        if (from_tag == TypeTag::FLOAT && to_tag == TypeTag::INT)
            return true;
        return false;
    }
    auto from_tag = reg_.tag_of(from);
    auto to_tag = reg_.tag_of(to);
    // Int ↔ String
    if ((from_tag == TypeTag::INT && to_tag == TypeTag::STRING) ||
        (from_tag == TypeTag::STRING && to_tag == TypeTag::INT))
        return true;
    // Int ↔ Bool (truthiness / 0/1)
    if ((from_tag == TypeTag::INT && to_tag == TypeTag::BOOL) ||
        (from_tag == TypeTag::BOOL && to_tag == TypeTag::INT))
        return true;
    // Float ↔ Int (numeric coercion)
    if ((from_tag == TypeTag::FLOAT && to_tag == TypeTag::INT) ||
        (from_tag == TypeTag::INT && to_tag == TypeTag::FLOAT))
        return true;
    // Float ↔ String
    if ((from_tag == TypeTag::FLOAT && to_tag == TypeTag::STRING) ||
        (from_tag == TypeTag::STRING && to_tag == TypeTag::FLOAT))
        return true;
    // Float ↔ Bool
    if ((from_tag == TypeTag::FLOAT && to_tag == TypeTag::BOOL) ||
        (from_tag == TypeTag::BOOL && to_tag == TypeTag::FLOAT))
        return true;
    // Issue #100: structural coercion for Record / Variant / ADT.
    //
    // Record width matching: Record{...} coercible to Record{...} if
    // every field in `to` exists in `from` (same name) and the
    // matching field's type is coercible. The "from has more fields"
    // case is OK — extra fields are simply dropped at the runtime
    // cast boundary (CastOp strips them).
    //
    // Variant width matching: Variant{...} coercible to Variant{...}
    // if every constructor in `from` exists in `to` (same name) and
    // the matching constructor's argument types are pairwise
    // coercible. "From has fewer ctors" is OK — extra ctors in `to`
    // are simply never produced.
    //
    // ADTs are registered as Variants via the register_adt_constructors
    // path (TypeRegistry::register_adt_constructors), so the Variant
    // rule transparently covers them — no separate ADT case needed.
    if (from_tag == TypeTag::RECORD && to_tag == TypeTag::RECORD) {
        auto* fr = reg_.record_of(from);
        auto* tr = reg_.record_of(to);
        if (!fr || !tr)
            return false;
        for (auto& [name, to_field_ty] : tr->fields) {
            bool found = false;
            for (auto& [n2, from_field_ty] : fr->fields) {
                if (n2 == name) {
                    if (!is_coercible(from_field_ty, to_field_ty))
                        return false;
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }
    if (from_tag == TypeTag::VARIANT && to_tag == TypeTag::VARIANT) {
        auto* fv = reg_.variant_of(from);
        auto* tv = reg_.variant_of(to);
        if (!fv || !tv)
            return false;
        for (auto& [name, from_args] : fv->variants) {
            bool found = false;
            for (auto& [n2, to_args] : tv->variants) {
                if (n2 == name) {
                    if (from_args.size() != to_args.size())
                        return false;
                    for (std::size_t i = 0; i < from_args.size(); ++i) {
                        if (!is_coercible(from_args[i], to_args[i]))
                            return false;
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }
    return false;
}

void InferenceEngine::register_primitive(std::string name, std::vector<TypeId> param_types,
                                         TypeId ret_type) {
    auto func_type = reg_.register_func(std::move(param_types), ret_type);
    env_.bind(std::move(name), func_type);
}

void InferenceEngine::register_poly_primitive(std::string name, std::vector<TypeId> param_types,
                                              TypeId ret_type, std::vector<TypeId> type_vars) {
    auto func_type = reg_.register_func(std::move(param_types), ret_type);
    for (auto it = type_vars.rbegin(); it != type_vars.rend(); ++it)
        func_type = reg_.register_forall(*it, func_type);
    env_.bind(std::move(name), func_type);
}

void InferenceEngine::init_primitive_env() {
    auto Int = reg_.int_type();
    auto Bool = reg_.bool_type();
    auto Float = reg_.register_type(TypeTag::FLOAT, "Float");
    (void)Float;
    auto String = reg_.string_type();
    auto Dyn = reg_.dynamic_type();
    auto Void = reg_.void_type();
    auto Vector = reg_.lookup_type("Vector");
    auto Hash = reg_.lookup_type("Hash");

    // ── Capability Effects（#9 Agent OS 安全） ────────────
    auto EffIO = reg_.register_effect("IO");
    auto EffMutation = reg_.register_effect("Mutation");
    auto EffFileRead = reg_.register_effect("FileRead");
    auto EffFileWrite = reg_.register_effect("FileWrite");
    auto EffNetwork = reg_.register_effect("Network");
    auto EffAgentMsg = reg_.register_effect("AgentMsg");
    (void)EffIO;
    (void)EffMutation;
    (void)EffFileRead;
    (void)EffFileWrite;
    (void)EffNetwork;
    (void)EffAgentMsg;

    // Mutation primitives 带效果标注
    register_primitive("mutate:rebind", {String, String}, Dyn);
    register_primitive("mutate:replace-type", {Int, String}, Dyn);
    register_primitive("mutate:replace-value", {Int, Dyn, String}, Dyn);
    register_primitive("mutate:set-body", {String, String}, Dyn);
    register_primitive("mutate:splice", {Int, Int}, Dyn);
    register_primitive("mutate:wrap", {Int, String}, Dyn);
    register_primitive("mutate:tweak-literal", {Int, Int}, Dyn);

    // IO/网络原语
    register_primitive("load-module", {String}, Dyn);
    register_primitive("write-file", {String, String}, Void);
    register_primitive("read-file", {String}, String);

    // Arithmetic: (Number, Number) -> Number (Int or Float promotion)
    register_primitive("+", {Dyn, Dyn}, Dyn);
    register_primitive("-", {Dyn, Dyn}, Dyn);
    register_primitive("*", {Dyn, Dyn}, Dyn);
    register_primitive("/", {Dyn, Dyn}, Dyn);

    // Comparison: (Number, Number) -> Bool
    register_primitive("=", {Dyn, Dyn}, Bool);
    register_primitive("<", {Dyn, Dyn}, Bool);
    register_primitive(">", {Dyn, Dyn}, Bool);
    register_primitive("<=", {Dyn, Dyn}, Bool);
    register_primitive(">=", {Dyn, Dyn}, Bool);

    // Boolean logic: runtime #t/#f are lexed as Int 0/1, so
    // truthiness-checking ops work on any value.
    // and/or are variadic — minimal signature uses 2 args
    register_primitive("and", {Dyn, Dyn}, Dyn);
    register_primitive("or", {Dyn, Dyn}, Dyn);

    // not: works on any truthy/falsy value (runtime: a[0] == 0 → 1)
    register_primitive("not", {Dyn}, Bool);
    register_primitive("eq?", {Dyn, Dyn}, Bool);

    // Type predicates return Bool
    register_primitive("number?", {Dyn}, Bool);
    register_primitive("string?", {Dyn}, Bool);
    register_primitive("boolean?", {Dyn}, Bool);
    register_primitive("null?", {Dyn}, Bool);
    register_primitive("pair?", {Dyn}, Bool);
    register_primitive("procedure?", {Dyn}, Bool);
    register_primitive("list?", {Dyn}, Bool);
    register_primitive("equal?", {Dyn, Dyn}, Bool);

    // String operations
    register_primitive("string-append", {String, String}, String);
    register_primitive("string-length", {String}, Int);
    register_primitive("string-ref", {String, Int}, Int);
    register_primitive("substring", {String, Int, Int}, String);
    register_primitive("string=?", {String, String}, Bool);
    register_primitive("string<?", {String, String}, Bool);
    register_primitive("number->string", {Int}, String);
    register_primitive("string-index", {String, String, Int}, Int);
    register_primitive("string->number", {String}, Dyn);
    register_primitive("string->number", {String}, Int);

    // Pair operations
    register_primitive("cons", {Dyn, Dyn}, Dyn);
    register_primitive("car", {Dyn}, Dyn);
    register_primitive("cdr", {Dyn}, Dyn);
    // car/cdr with polymorphic pair types for ADT match
    {
        auto a = cs_.fresh_var();
        auto b = cs_.fresh_var();
        auto pair_type = reg_.register_func({a}, b);
        auto car_type = reg_.register_func({pair_type}, a);
        env_.bind("car", reg_.register_forall(a, reg_.register_forall(b, car_type)));
    }
    {
        auto a = cs_.fresh_var();
        auto b = cs_.fresh_var();
        auto pair_type = reg_.register_func({a}, b);
        auto cdr_type = reg_.register_func({pair_type}, b);
        env_.bind("cdr", reg_.register_forall(a, reg_.register_forall(b, cdr_type)));
    }

    // Cadr/Caddr shorthands
    register_primitive("caar", {Dyn}, Dyn);
    register_primitive("cadr", {Dyn}, Dyn);
    register_primitive("cdar", {Dyn}, Dyn);
    register_primitive("cddr", {Dyn}, Dyn);
    register_primitive("caaar", {Dyn}, Dyn);
    register_primitive("caadr", {Dyn}, Dyn);
    register_primitive("cadar", {Dyn}, Dyn);
    register_primitive("caddr", {Dyn}, Dyn);
    register_primitive("cdaar", {Dyn}, Dyn);
    register_primitive("cdadr", {Dyn}, Dyn);
    register_primitive("cddar", {Dyn}, Dyn);
    register_primitive("cdddr", {Dyn}, Dyn);

    // Mutable pair operations
    register_primitive("set-car!", {Dyn, Dyn}, Void);
    register_primitive("set-cdr!", {Dyn, Dyn}, Void);

    // List operations
    register_primitive("list", {Dyn}, Dyn); // varargs — minimal
    register_primitive("length", {Dyn}, Int);
    register_primitive("list-ref", {Dyn, Int}, Dyn);
    register_primitive("member", {Dyn, Dyn}, Dyn);
    register_primitive("append", {Dyn, Dyn}, Dyn);
    register_primitive("reverse", {Dyn}, Dyn);
    register_primitive("take", {Int, Dyn}, Dyn);
    register_primitive("drop", {Int, Dyn}, Dyn);
    register_primitive("foldl", {Dyn, Dyn, Dyn}, Dyn);
    // Polymorphic map/filter: ∀a b. ((a -> b), list a) -> b
    // The list types are approximated as Any for now (no proper List type).
    // The function contract (a→b) enforces type consistency between args and results.
    {
        auto a = reg_.make_var("a");
        auto b = reg_.make_var("b");
        auto a_to_b = reg_.register_func({a}, b);
        auto list_a = reg_.dynamic_type();
        auto map_type = reg_.register_func({a_to_b, list_a}, b);
        auto forall_map = reg_.register_forall(a, reg_.register_forall(b, map_type));
        env_.bind("map", forall_map);
        env_.bind("filter", forall_map);
    }

    // I/O
    register_primitive("display", {Dyn}, Void);
    register_primitive("write", {Dyn}, Void);
    register_primitive("newline", {}, Void);
    register_primitive("error", {Dyn}, Void);
    register_primitive("assert", {Dyn}, Void);

    // Introspection
    register_primitive("type-of", {Dyn}, reg_.type_type());
    register_primitive("type?", {Dyn, String}, Bool);

    // Misc
    register_primitive("read", {}, String);
    register_primitive("read-file", {String}, String);
    register_primitive("load-module", {String}, Dyn);
    register_primitive("import", {String}, Dyn);
    register_primitive("write-file", {String, String}, Void);
    // file-exists? returns 0/1 (Int), not Bool — matches evaluator_primitives_file.cpp
    register_primitive("file-exists?", {String}, Int);
    register_primitive("gensym", {}, String);

    // Typed mutation operators (runtime-only, minimal type info)
    register_primitive("mutate:replace-type", {Dyn, String}, Dyn);
    register_primitive("mutate:record-patch", {Dyn, String, String}, Dyn);
    register_primitive("mutation-count", {}, Int);
    register_primitive("mutation-history", {Dyn}, Dyn);
    // Vector primitives
    register_primitive("vector", {Dyn}, Vector); // varargs — minimal
    register_primitive("vector-ref", {Vector, Int}, Dyn);
    register_primitive("vector-set!", {Vector, Int, Dyn}, Void);
    register_primitive("vector-length", {Vector}, Int);
    register_primitive("vector?", {Dyn}, Bool); // already Bool
    register_primitive("make-vector", {Int, Dyn}, Vector);
    // List<->Vector conversion

    // Hash primitives
    register_primitive("hash", {Dyn}, Hash);
    register_primitive("hash-ref", {Hash, Dyn}, Dyn);
    register_primitive("hash-set!", {Hash, Dyn, Dyn}, Void);
    register_primitive("hash-length", {Hash}, Int);
    register_primitive("hash-keys", {Hash}, Dyn);
    register_primitive("hash-values", {Hash}, Dyn);
    register_primitive("hash?", {Dyn}, Bool);
    register_primitive("hash-remove!", {Hash, Dyn}, Bool);
    register_primitive("hash-has-key?", {Hash, Dyn}, Bool);

    // Numeric extension primitives
    register_primitive("modulo", {Int, Int}, Int);
    register_primitive("quotient", {Int, Int}, Int);
    register_primitive("remainder", {Int, Int}, Int);
    register_primitive("abs", {Int}, Int);
    register_primitive("gcd", {Int, Int}, Int);
    register_primitive("lcm", {Int, Int}, Int);
    register_primitive("min", {Dyn, Dyn}, Dyn);
    register_primitive("max", {Dyn, Dyn}, Dyn);

    // Character primitives
    register_primitive("char?", {Dyn}, Bool);
    register_primitive("char->integer", {Dyn}, Int);
    register_primitive("integer->char", {Int}, Int);
    register_primitive("string->list", {String}, Dyn);
    register_primitive("list->string", {Dyn}, String);
    register_primitive("read-line", {}, String);
    register_primitive("eof-object?", {Dyn}, Bool);

    // Additional type predicates
    register_primitive("integer?", {Dyn}, Bool);
    register_primitive("float?", {Dyn}, Bool);

    // Missing list/vector conversions
    register_primitive("list->vector", {Dyn}, Vector);
    register_primitive("vector->list", {Vector}, Dyn);

    // ── Stdlib type signatures ────────────────────────────
    // Generic type parameters for polymorphic stdlib functions
    auto _a = reg_.make_var("a");
    auto _b = reg_.make_var("b");
    auto _c = reg_.make_var("c");
    auto _d = reg_.make_var("d");
    auto _num = reg_.make_var("num");

    // std/list: (a -> b) -> (list a) -> (list b)
    register_poly_primitive("map", {reg_.register_func({_a}, _b), reg_.register_func({}, Dyn)}, Dyn,
                            {_a, _b});
    register_poly_primitive("filter", {reg_.register_func({_a}, Bool), reg_.register_func({}, Dyn)},
                            Dyn, {_a});
    register_poly_primitive(
        "foldl", {reg_.register_func({_a, _b}, _b), _b, reg_.register_func({}, Dyn)}, _b, {_a, _b});
    register_poly_primitive("range", {Int, Int}, Dyn, {});
    register_poly_primitive("length", {Dyn}, Int, {});
    register_poly_primitive("reverse", {Dyn}, Dyn, {_a});
    register_poly_primitive("zip", {Dyn, Dyn}, Dyn, {_a, _b});
    register_poly_primitive("take", {Int, Dyn}, Dyn, {_a});
    register_poly_primitive("drop", {Int, Dyn}, Dyn, {_a});
    register_poly_primitive("flatten", {Dyn}, Dyn, {_a});
    register_poly_primitive("partition", {reg_.register_func({_a}, Bool), Dyn}, Dyn, {_a});
    register_poly_primitive("sort", {Dyn, reg_.register_func({_a, _a}, Bool)}, Dyn, {_a});
    register_poly_primitive("append", {Dyn, Dyn}, Dyn, {_a});
    register_poly_primitive("member", {Dyn, Dyn}, Bool, {_a});

    // std/string
    register_primitive("string-split", {String, String}, Dyn);
    register_primitive("string-trim", {String}, String);
    register_primitive("string-join", {Dyn, String}, String);

    // std/hash
    register_primitive("hash-keys", {Dyn}, Dyn);
    register_primitive("hash-values", {Dyn}, Dyn);
    register_primitive("hash-ref", {Dyn, Dyn}, Dyn);
    register_primitive("hash-has-key?", {Dyn, Dyn}, Bool);
    register_primitive("hash-set!", {Dyn, Dyn, Dyn}, Void);
    register_primitive("hash-length", {Dyn}, Int);
    register_primitive("hash-count", {Dyn}, Int);

    // std/iter
    register_primitive("for-each", {reg_.register_func({_a}, Dyn), Dyn}, Void);
    register_primitive("for", {Dyn, reg_.register_func({_a}, Dyn)}, Void);

    // std/math
    register_poly_primitive("square", {_num}, _num, {_num});
    register_poly_primitive("sqrt", {_num}, _num, {_num});
    register_primitive("pi", {}, Float);
    register_poly_primitive("abs", {_num}, _num, {_num});
    register_poly_primitive("min", {_num, _num}, _num, {_num});
    register_poly_primitive("max", {_num, _num}, _num, {_num});
    register_primitive("sin", {_num}, _num);
    register_primitive("cos", {_num}, _num);
    register_primitive("tan", {_num}, _num);
    register_primitive("floor", {_num}, _num);
    register_primitive("ceil", {_num}, _num);
    register_primitive("round", {_num}, _num);
    register_primitive("exp", {_num}, _num);
    register_primitive("log", {_num}, _num);
    register_primitive("pow", {_num, _num}, _num);
    register_primitive("rand", {}, Float);
    register_primitive("rand-int", {Int}, Int);
    register_primitive("mean", {Dyn}, _num);
    register_primitive("median", {Dyn}, _num);
    register_primitive("stddev", {Dyn}, _num);
    register_primitive("sum", {Dyn}, Int);
    register_primitive("product", {Dyn}, Int);
    register_primitive("factorial", {Int}, Int);

    // std/io — return types match evaluator partition primitives (Int 0/1 for
    // success/failure, not Bool; the runtime never makes Bool here)
    register_primitive("file-exists?", {String}, Int);
    register_primitive("file-size", {String}, Int);
    register_primitive("file-copy", {String, String}, Int);
    register_primitive("file-delete", {String}, Int);
    register_primitive("file-read", {String}, String);
    register_primitive("file-write", {String, String}, Void);
    register_primitive("file->string", {String}, String);
    register_primitive("string->file", {String, String}, Void);
    register_primitive("file->lines", {String}, Dyn);

    // std/data (trie)
    register_primitive("make-trie", {}, Dyn);
    register_primitive("trie-insert", {Dyn, String}, Dyn);
    register_primitive("trie-search", {Dyn, String}, Bool);
    register_primitive("trie-prefix?", {Dyn, String}, Bool);
    register_primitive("trie-keys", {Dyn}, Dyn);

    // std/csv
    register_primitive("csv-parse", {String}, Dyn);
    register_primitive("csv->rows", {String}, Dyn);
    register_primitive("csv->table", {String}, Dyn);
    register_primitive("csv-select", {Dyn, Dyn}, Dyn);
    register_primitive("csv-filter", {reg_.register_func({Dyn}, Bool), Dyn}, Dyn);
    register_primitive("csv-header", {Dyn}, Dyn);
    register_primitive("column-names", {Dyn}, Dyn);

    // std/json
    register_primitive("json-parse", {String}, Dyn);
    register_primitive("json-stringify", {Dyn}, String);
    register_primitive("json-value", {Dyn}, String);
    register_primitive("json-arr-items", {Dyn, reg_.register_func({Dyn}, Dyn)}, Dyn);
    register_primitive("json-obj-items", {Dyn, Dyn, reg_.register_func({Dyn}, Dyn)}, Dyn);

    // std/socket
    register_primitive("tcp-connect", {String, Int}, Dyn);
    register_primitive("tcp-send", {Dyn, String}, Void);
    register_primitive("tcp-recv", {Dyn}, String);
    register_primitive("tcp-close", {Dyn}, Void);

    // std/algorithm
    register_poly_primitive("sorted?", {Dyn, reg_.register_func({_a, _a}, Bool)}, Bool, {_a});
    register_poly_primitive("sort-by", {Dyn, reg_.register_func({_a}, _b)}, Dyn, {_a, _b});
    register_poly_primitive("sort-stable", {Dyn, reg_.register_func({_a, _a}, Bool)}, Dyn, {_a});
    register_poly_primitive("unique", {Dyn}, Dyn, {_a});
    register_poly_primitive("min-by", {Dyn, reg_.register_func({_a}, _b)}, Dyn, {_a, _b});
    register_poly_primitive("max-by", {Dyn, reg_.register_func({_a}, _b)}, Dyn, {_a, _b});
    register_poly_primitive("permutations", {Dyn}, Dyn, {_a});
    register_poly_primitive("merge-sorted", {Dyn, Dyn, reg_.register_func({_a, _a}, Bool)}, Dyn,
                            {_a});

    // std/combinators
    register_poly_primitive("identity", {_a}, _a, {_a});
    register_poly_primitive("const", {_a, _b}, _a, {_a, _b});
    register_poly_primitive("flip", {reg_.register_func({_a, _b}, _c)},
                            reg_.register_func({_b, _a}, _c), {_a, _b, _c});
    register_poly_primitive("compose", {reg_.register_func({_b}, _c), reg_.register_func({_a}, _b)},
                            reg_.register_func({_a}, _c), {_a, _b, _c});
    register_poly_primitive("complement", {reg_.register_func({_a}, Bool)},
                            reg_.register_func({_a}, Bool), {_a});

    // std/datetime
    register_primitive("timestamp", {}, Int);
    register_primitive("timestamp->year", {Int}, Int);
    register_primitive("timestamp->month", {Int}, Int);
    register_primitive("timestamp->day", {Int}, Int);
    register_primitive("timestamp->hour", {Int}, Int);
    register_primitive("timestamp->minute", {Int}, Int);
    register_primitive("leap-year?", {Int}, Bool);
    register_primitive("days-in-month", {Int, Int}, Int);

    // std/random
    register_primitive("make-random", {}, Dyn);
    register_primitive("random-next", {Dyn}, Dyn);
    register_primitive("random-integer", {Dyn}, Int);
    register_primitive("random-float", {Dyn}, Float);
    register_primitive("random-range", {Int, Int, Dyn}, Int);
    register_primitive("shuffle", {Dyn, Dyn}, Dyn);

    // std/set
    register_primitive("set", {Dyn}, Dyn);
    register_primitive("set-add", {Dyn, Dyn}, Dyn);
    register_primitive("set-remove", {Dyn, Dyn}, Dyn);
    register_primitive("set-member?", {Dyn, Dyn}, Bool);
    register_primitive("set-empty?", {Dyn}, Bool);
    register_primitive("set-union", {Dyn, Dyn}, Dyn);
    register_primitive("set-intersect", {Dyn, Dyn}, Dyn);
    register_primitive("set-difference", {Dyn, Dyn}, Dyn);
    register_primitive("set->list", {Dyn}, Dyn);
    register_primitive("list->set", {Dyn}, Dyn);
    register_primitive("set-size", {Dyn}, Int);
    register_primitive("set-subset?", {Dyn, Dyn}, Bool);
    register_primitive("set-equal?", {Dyn, Dyn}, Bool);

    // std/queue
    register_primitive("make-queue", {}, Dyn);
    register_primitive("enqueue", {Dyn, Dyn}, Dyn);
    register_primitive("dequeue", {Dyn}, Dyn);
    register_primitive("queue-front", {Dyn}, Dyn);
    register_primitive("queue-rest", {Dyn}, Dyn);
    register_primitive("queue-empty?", {Dyn}, Bool);
    register_primitive("queue-length", {Dyn}, Int);
    register_primitive("queue->list", {Dyn}, Dyn);
    register_primitive("list->queue", {Dyn}, Dyn);

    // std/stack
    register_primitive("make-stack", {}, Dyn);
    register_primitive("stack-push", {Dyn, Dyn}, Dyn);
    register_primitive("stack-pop", {Dyn}, Dyn);
    register_primitive("stack-top", {Dyn}, Dyn);
    register_primitive("stack-empty?", {Dyn}, Bool);
    register_primitive("stack-length", {Dyn}, Int);
    register_primitive("stack->list", {Dyn}, Dyn);
    register_primitive("list->stack", {Dyn}, Dyn);

    // std/evolve
    // Issue #63 Phase 3: evolve-strategy now returns the new strategy name (String).
    register_primitive("evolve-strategy", {String}, String);
    register_primitive("define-strategy", {String, String}, Bool);
    register_primitive("register-strategy!", {String, String}, Bool);
    register_primitive("strategy-field", {String, String}, Dyn);
    register_primitive("strategy-set-field!", {String, String, Dyn}, Bool);
    register_primitive("strategy-inspect", {String}, String);

    // ── Capability primitives ──
    register_primitive("with-capability", {String, Dyn}, Dyn);
    register_primitive("capability?", {String}, Bool);
    register_primitive("check-capability", {String}, Bool);
    register_primitive("capability-stack", {}, Dyn);
}

TypeId InferenceEngine::lub(TypeId a, TypeId b) {
    if (a == b)
        return a;
    if (a == reg_.dynamic_type() || b == reg_.dynamic_type())
        return reg_.dynamic_type();
    // Int → Float promotion
    if ((a == reg_.int_type() && b == reg_.lookup_type("Float")) ||
        (a == reg_.lookup_type("Float") && b == reg_.int_type()))
        return reg_.lookup_type("Float");
    return reg_.dynamic_type(); // safe fallback
}

// ═══════════════════════════════════════════════════════════
// FlatAST Inference (bypasses Expr* reconstruction)
// ═══════════════════════════════════════════════════════════

// FlatAST version of analyze_predicate
// Issue #281: OccurrenceInfoFlat moved to the module interface
// (type_checker.ixx) so the InferenceEngine's predicate memo
// can use it as a value type.

// Forward decl for #280 follow-up #1: combine-bits path uses
// compute_narrowing_evidence to walk each or-branch predicate.
static std::optional<std::uint32_t> compute_narrowing_evidence(const FlatAST& flat,
                                                               const StringPool& pool,
                                                               NodeId cond_id, TypeRegistry& reg);

static std::optional<OccurrenceInfoFlat> analyze_predicate_flat(const FlatAST& flat,
                                                                const StringPool& pool,
                                                                NodeId cond_id, TypeRegistry& reg,
                                                                bool& meet_used, bool& join_used) {
    meet_used = false;
    join_used = false;
    auto cond = flat.get(cond_id);
    if (cond.tag != NodeTag::Call || cond.children.empty())
        return std::nullopt;

    auto fn_id = cond.child(0);
    auto fn = flat.get(fn_id);

    // Check for (not p)
    if (fn.tag == NodeTag::Variable) {
        auto fn_name = pool.resolve(fn.sym_id);
        if (fn_name == "not" && cond.children.size() >= 2) {
            bool m1, j1;
            auto inner = analyze_predicate_flat(flat, pool, cond.child(1), reg, m1, j1);
            if (inner) {
                meet_used |= m1;
                join_used |= j1;
            }
            if (inner) {
                inner->is_negation = !inner->is_negation;
                return inner;
            }
            return std::nullopt;
        }

        // Check for (and p1 p2) — combine predicates for the same variable
        if (fn_name == "and") {
            std::optional<OccurrenceInfoFlat> result;
            // Issue #281 follow-up #2: track the OR-combined bitmask
            // across all branch predicates. Each branch may have
            // its own narrowing; the AND of the predicates implies
            // all of them hold in the then-branch, so we OR the
            // bits (any predicate being true is a sufficient
            // narrowing signal). The refined_type is still
            // intersected via the conservative LUB rules.
            std::uint32_t combined_evidence = 0;
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                bool mi, ji;
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg, mi, ji);
                meet_used |= mi;
                join_used |= ji;
                if (auto bit = compute_narrowing_evidence(flat, pool, cond.child(i), reg))
                    combined_evidence |= *bit;
                if (inner) {
                    if (!result) {
                        result = inner;
                    } else if (inner->var_name == result->var_name) {
                        // Same variable: in the then-branch of (and p1 p2),
                        // the variable satisfies BOTH predicates, so use
                        // the intersection. Issue #338: use the new
                        // TypeRegistry::meet helper for the
                        // intersection. Pre-#338 the engine fell
                        // back to dynamic_type() on any mismatch
                        // (overly conservative). Post-#338 the
                        // meet helper returns the most specific
                        // common subtype — which is just `a` if
                        // both refine to the same type, or
                        // dynamic_type() if the types differ
                        // (Aura has no real intersection types
                        // yet). The structural behavior is
                        // identical for the common case, but
                        // the meet helper is the right
                        // extension point when real
                        // intersection types land.
                        result->refined_type = reg.meet(result->refined_type, inner->refined_type);
                        // Issue #338: the meet helper was
                        // called. The bump happens at the
                        // call site in synthesize_flat_if
                        // (analyze_predicate_flat is a
                        // static free function, no
                        // access to InferenceEngine::stats_).
                        meet_used = true;
                    }
                }
            }
            return result;
        }

        // Check for (or p1 p2) — conservative LUB over the same variable.
        // Issue #279: previously took the first match, which is too narrow
        // when the second branch narrows to a different type. Now we
        // collect all OccurrenceInfoFlat entries for the same variable
        // and pick the type that subsumes all of them. In the absence of
        // a real union type system, the conservative choice is:
        //   - 0 matches  → std::nullopt (no information)
        //   - 1 match    → that type
        //   - 2+ matches same type  → that type
        //   - 2+ matches different types  → reg.dynamic_type() (Any) +
        //                                   is_negation = false (this is
        //                                   a then-branch disjunct, not a
        //                                   negation). We then-branch
        //                                   dynamic, but the *else-branch*
        //                                   (when the disjunction is
        //                                   exhausted) still gets the
        //                                   pre-disjunction type via the
        //                                   caller's `if` walker.
        if (fn_name == "or") {
            std::optional<OccurrenceInfoFlat> result;
            // Issue #280 follow-up #1: track all bitmasks for the
            // OR's predicates (not just first-match). When the
            // narrow_evidence bitmask is captured (Issue #280), the
            // OR-disjunction records the union of all predicate bits
            // so DeadCoercionEliminationPass / JIT see the full
            // evidence for the then-branch. The refined_type field
            // is still picked per the conservative LUB rules
            // (#279) — only the bitmask changes here.
            std::uint32_t combined_evidence = 0;
            for (std::size_t i = 1; i < cond.children.size(); i++) {
                bool mi, ji;
                auto inner = analyze_predicate_flat(flat, pool, cond.child(i), reg, mi, ji);
                meet_used |= mi;
                join_used |= ji;
                if (!inner)
                    continue;
                // Compute the predicate name's bitmask and OR it in.
                // The call to narrowing_bit_for walks the predicate
                // node the same way Issue #280 does for the
                // single-predicate case.
                std::uint32_t branch_evidence = 0;
                if (auto info = compute_narrowing_evidence(flat, pool, cond.child(i), reg)) {
                    branch_evidence = *info;
                }
                combined_evidence |= branch_evidence;
                if (!result) {
                    result = inner;
                } else if (result->var_name == inner->var_name) {
                    // Issue #338: use the join helper to
                    // widen (was gated on != before; the
                    // helper is idempotent on equal
                    // types, so unconditional is
                    // safe and consistent with the
                    // meet path in the and branch).
                    result->refined_type = reg.join(result->refined_type, inner->refined_type);
                    // The join helper was called. Bump
                    // happens at the call site.
                    join_used = true;
                } else {
                    // Different variables on each branch — can't combine.
                    // Conservative: drop both, return std::nullopt.
                    return std::nullopt;
                }
            }
            // The combined_evidence is captured by the caller
            // (synthesize_flat_if) via last_if_narrowing_. We don't
            // attach it to the result here; the or-children get
            // walked individually, and the bitmask accumulates in
            // last_if_narrowing_ in synthesize_flat_if (line ~2493).
            // This loop above is the per-branch piece; the per-if
            // accumulation happens in synthesize_flat_if.
            (void)combined_evidence;
            return result;
        }

        // Check for (type? x "TypeName")
        if (fn_name == "type?" && cond.children.size() == 3) {
            auto var_id = cond.child(1);
            auto var_node = flat.get(var_id);
            auto type_lit_id = cond.child(2);
            auto type_lit = flat.get(type_lit_id);
            if (var_node.tag == NodeTag::Variable && type_lit.tag == NodeTag::LiteralString) {
                auto type_name = pool.resolve(type_lit.sym_id);
                auto type_id = reg.lookup_type(std::string(type_name));
                if (type_id.valid()) {
                    // Issue #342: populate provenance
                    // fields (predicate_name +
                    // source_cond_id) so subsequent
                    // diagnostics can attach a
                    // BlameInfo with the Narrowing
                    // party.
                    OccurrenceInfoFlat occ{std::string(pool.resolve(var_node.sym_id)), type_id};
                    occ.predicate_name = "type?";
                    occ.source_cond_id = cond_id;
                    return occ;
                }
            }
            return std::nullopt;
        }

        // Must be a predicate call: (pred? x)
        if (cond.children.size() == 2) {
            auto arg_id = cond.child(1);
            auto arg = flat.get(arg_id);
            if (arg.tag == NodeTag::Variable) {
                auto var_name = pool.resolve(arg.sym_id);
                // Issue #342: helper to construct the
                // OccurrenceInfoFlat with provenance
                // fields populated (predicate_name +
                // source_cond_id).
                auto make_occ = [&](const std::string& pname, aura::core::TypeId tid) {
                    OccurrenceInfoFlat occ{std::string(var_name), tid};
                    occ.predicate_name = pname;
                    occ.source_cond_id = cond_id;
                    return occ;
                };
                auto make_occ_sv = [&](std::string_view pname, aura::core::TypeId tid) {
                    return make_occ(std::string(pname), tid);
                };
                if (fn_name == "string?")
                    return make_occ("string?", reg.string_type());
                else if (fn_name == "number?" || fn_name == "integer?")
                    return make_occ_sv(fn_name, reg.int_type());
                else if (fn_name == "boolean?")
                    return make_occ("boolean?", reg.bool_type());
                else if (fn_name == "null?" || fn_name == "void?")
                    return make_occ_sv(fn_name, reg.void_type());
                else if (fn_name == "pair?")
                    // Issue #279: pair? should refine to the Pair type,
                    // not register a fresh (Dynamic)->Dynamic func type.
                    // The pre-#279 behavior created a brand-new func TypeId
                    // per (pair? x) site, which is wasteful and inaccurate
                    // (the variable isn't a function — it's a cons pair).
                    // We use lookup_type("Pair") for the pre-registered
                    // PAIR type. lookup_type returns an invalid TypeId
                    // only if Pair isn't registered; that shouldn't happen
                    // given the TypeRegistry constructor pre-registers it,
                    // but we fall back to dynamic_type() in the safety
                    // case to avoid UB.
                    return make_occ("pair?", [&]() {
                        auto p = reg.lookup_type("Pair");
                        return p.valid() ? p : reg.dynamic_type();
                    }());
                else if (fn_name == "list?")
                    // Issue #279: add list? predicate for Vector refinement
                    // (was missing pre-#279). Same lookup_type fallback
                    // pattern as pair?.
                    return make_occ("list?", [&]() {
                        auto v = reg.lookup_type("Vector");
                        return v.valid() ? v : reg.dynamic_type();
                    }());
                else if (fn_name == "symbol?")
                    return make_occ("symbol?", reg.dynamic_type());
                else if (fn_name == "float?")
                    return make_occ("float?", reg.lookup_type("Float"));
                else if (fn_name == "hash?")
                    return make_occ("hash?", reg.dynamic_type());
                else if (fn_name == "procedure?")
                    return make_occ("procedure?", reg.dynamic_type());
                else {
                    // Issue #279 follow-up #4: consult the
                    // custom-predicate registry. If this
                    // predicate name was registered via
                    // (register-predicate! name type-name),
                    // refine the variable to the named type.
                    // The registry lives in aura.core.mutation
                    // (shared by both this module and the
                    // evaluator module's Aura primitive).
                    if (auto custom_type_name = aura::ast::mutation::lookup_custom_predicate_type(
                            std::string(fn_name))) {
                        auto tid = reg.lookup_type(*custom_type_name);
                        if (tid.valid())
                            return make_occ_sv(fn_name, tid);
                    }
                }
            }
        }
    }

    return std::nullopt;
}

void InferenceEngine::add_deferred_coercion(const FlatAST& flat, NodeId parent,
                                            std::uint32_t child_index, NodeId original_child,
                                            std::uint32_t type_tag, std::uint32_t type_id,
                                            std::uint32_t src_line, std::uint32_t src_col) {
    std::uint32_t cond_node = last_predicate_cond_id_;
    std::uint64_t mutation_id = 0;
    std::uint32_t narrow_ev = last_if_narrowing_;
    if (!flat.all_mutations().empty())
        mutation_id = flat.all_mutations().back().mutation_id;
    for (const auto& nr : flat.all_narrowings()) {
        if (cond_node != 0 && nr.cond_node != cond_node)
            continue;
        if (nr.source_mutation_id != 0)
            mutation_id = nr.source_mutation_id;
        if (nr.narrow_evidence != 0)
            narrow_ev = nr.narrow_evidence;
        if (cond_node != 0)
            break;
    }
    if (narrow_ev != 0 || cond_node != 0 || mutation_id != 0) {
        coercions_.add(parent, child_index, original_child, type_tag, type_id, src_line, src_col,
                       cond_node, mutation_id, narrow_ev);
        if (cs_.metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
            m->coercion_post_narrow_elim_opportunities_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            if (cond_node != 0 && mutation_id != 0)
                m->coercion_narrow_blame_chain_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        coercions_.add(parent, child_index, original_child, type_tag, type_id, src_line, src_col);
    }
}

void InferenceEngine::seed_mutation_touched_roots(const FlatAST& flat, const StringPool& pool,
                                                  const std::vector<NodeId>& occurrence_targets,
                                                  std::uint64_t mutation_id) {
    std::unordered_set<NodeId> target_set(occurrence_targets.begin(), occurrence_targets.end());
    for (const auto& nr : flat.all_narrowings()) {
        if (nr.source_mutation_id != 0 && nr.source_mutation_id != mutation_id)
            continue;
        if (!target_set.empty() && target_set.count(nr.if_node) == 0 &&
            nr.source_mutation_id != mutation_id)
            continue;
        if (!nr.refined_type_str.empty()) {
            auto tid = reg_.lookup_type(nr.refined_type_str);
            if (tid.valid() && reg_.is_var(tid))
                cs_.mark_touched_on_delta(tid, true);
        }
    }
    for (auto if_id : occurrence_targets) {
        if (if_id == NULL_NODE || if_id >= flat.size())
            continue;
        auto v = flat.get(if_id);
        if (v.children.empty())
            continue;
        auto cond_id = v.child(0);
        if (cond_id == NULL_NODE)
            continue;
        bool meet_used = false;
        bool join_used = false;
        auto occ = analyze_predicate_flat(flat, pool, cond_id, reg_, meet_used, join_used);
        if (occ && occ->refined_type.valid() && reg_.is_var(occ->refined_type))
            cs_.mark_touched_on_delta(occ->refined_type, true);
    }
}

TypeId InferenceEngine::infer_flat(FlatAST& flat, StringPool& pool, NodeId id, bool preserve_cs) {
    if (id == NULL_NODE || id >= flat.size())
        return reg_.dynamic_type();

    // Issue #168 Phase 1: epoch gate. If the cache epoch has
    // advanced since the last inference (i.e., a mutation
    // happened that the type system might not have observed
    // via is_dirty propagation), invalidate the cache globally
    // for this call. We can't easily clear cached types on
    // every node (would need a FlatAST-wide walk to mark
    // dirty, which is expensive), so we gate the cache check
    // on epoch_invalidated_ — a single bool, set per-call.
    if (cache_epoch_ != last_inference_epoch_) {
        epoch_invalidated_ = true;
        last_inference_epoch_ = cache_epoch_;
        ++epoch_invalidations_;
        // Issue #281: clear the predicate memo wholesale on
        // epoch change. The epoch advances when a mutation
        // happened; we can't tell which cond nodes were
        // affected, so the safe move is to drop everything.
        // The next call to synthesize_flat_if will repopulate
        // the memo on demand.
        if (!predicate_memo_.empty()) {
            ++predicate_memo_evictions_;
            predicate_memo_.clear();
        }
    }

    if (!preserve_cs)
        cs_.clear();
    cs_.set_delta_record_mode(incremental_delta_record_);
    auto result = synthesize_flat(flat, pool, id, flat.get(id));
    cs_.set_delta_record_mode(false);
    std::vector<Constraint> unresolved;
    SolveResult solve_status = SolveResult::SOLVED;
    if (incremental_delta_solve_ && preserve_cs)
        solve_status = cs_.solve_delta(&unresolved);
    else {
        if (cs_.metrics_ && incremental_delta_record_) {
            static_cast<struct CompilerMetrics*>(cs_.metrics_)
                ->solve_delta_full_solve_fallback_total.fetch_add(1, std::memory_order_relaxed);
        }
        solve_status = cs_.solve(&unresolved);
        if (incremental_delta_record_)
            cs_.mark_clean();
    }
    if (solve_status != SolveResult::SOLVED) {
        // Build a human-readable summary of the unresolved
        // constraints for the diagnostic. The summary is
        // intentionally short ("T42 ~ T43 (consistent)") to keep
        // the diagnostic compact; the full constraint list is
        // available via the diagnostic's structured data.
        std::string unresolved_str;
        if (!unresolved.empty()) {
            unresolved_str = " (";
            std::size_t max_show = 3;
            for (std::size_t i = 0; i < unresolved.size() && i < max_show; ++i) {
                if (i > 0)
                    unresolved_str += ", ";
                unresolved_str += std::string(unresolved[i].kind == Constraint::EQUAL ? "=" : "~") +
                                  std::to_string(unresolved[i].lhs.index) + " " +
                                  (unresolved[i].kind == Constraint::EQUAL ? "=" : "~") + " " +
                                  std::to_string(unresolved[i].rhs.index);
            }
            if (unresolved.size() > max_show) {
                unresolved_str += ", +" + std::to_string(unresolved.size() - max_show) + " more";
            }
            unresolved_str += ")";
        }

        // Issue #103: LLM-friendly error recovery. When the
        // constraint solver fails AND we're not in strict mode AND
        // permissive mode is on, don't fail the whole inference —
        // log a Warning (not TypeError) and degrade the result to
        // Dynamic. The caller can still see the warning in the
        // diagnostic collector and decide what to do. In strict
        // mode, or when permissive is explicitly disabled, we keep
        // the old behavior (TypeError, no degradation).
        //
        // Issue #118: also distinguishes CONFLICT (a hard
        // unification failure — the program is unsound) from
        // TIMEOUT (the solver hit the pass limit with
        // under-constrained types). CONFLICT is always a
        // TypeError; TIMEOUT is a Warning in permissive mode.
        bool is_conflict = (solve_status == SolveResult::CONFLICT);
        if (strict_ || !permissive_ || is_conflict) {
            const char* kind_str = is_conflict
                                       ? "type constraint solving failed (conflict)"
                                       : "type constraint solving timed out (under-constrained)";
            diag_.report(
                Diagnostic(ErrorKind::TypeError, std::string(kind_str) + unresolved_str, cur_loc_)
                    .with_blame(BlameInfo{BlameParty::Implicit, "", "compile"})
                    .with_suggestion("check for type mismatches in function arguments, return "
                                     "values, or recursive bindings; "
                                     "add explicit type annotations to constrain the inference"));
        } else {
            // Non-strict + permissive + TIMEOUT: emit a warning
            // with the constraint list, then fall through to the
            // degraded Dynamic return below. The LLM gets a
            // chance to see the warning while still having the
            // program type-check.
            diag_.report(
                Diagnostic(ErrorKind::Warning,
                           std::string("type constraint solving timed out — under-constrained") +
                               unresolved_str,
                           cur_loc_)
                    .with_blame(BlameInfo{BlameParty::Implicit, "", "compile"})
                    .with_suggestion(
                        "add explicit type annotations to constrain the inference; "
                        "or set strict mode (set-strict) to surface this as a TypeError"));
            result = reg_.dynamic_type();
        }
    }
    auto normalized = cs_.normalize(result);
    // Update the root's cached type with the final resolved type after solving.
    // Individual sub-nodes' caches are updated during their synthesize_flat calls.
    // Those may store TYPE_VARs that get resolved here; next incremental pass
    // will re-compute stale TYPE_VAR caches and write back the resolved type.
    flat.set_type(id, normalized.index);
    flat.clear_dirty(id);
    return normalized;
}

TypeId InferenceEngine::synthesize_flat(FlatAST& flat, StringPool& pool, NodeId id, NodeView v) {
    cur_loc_ = {v.line, v.col, 0};

    // Incremental: if node is clean AND has a resolved cached type, return cached result.
    // Dirty propagation ensures ancestors of mutated nodes are marked dirty,
    // so clean nodes' cached types remain valid.
    // Skip TYPE_VAR cache entries — they are stale pre-solve type variables
    // that were cached before constraint solving resolved them.
    if (!flat.is_dirty(id) && !epoch_invalidated_) {
        auto cached = flat.type_id(id);
        if (cached > 0 && cached < reg_.size()) {
            auto tid = TypeId{cached, 1};
            // Issue #72: also reject cached types that CONTAIN
            // TYPE_VARs (not just types whose top-level tag is
            // TYPE_VAR). Pre-solve cached types often have free
            // vars in polymorphic contexts, and those vars are
            // stale (the union-find has been cleared). free_vars()
            // returning empty means the type is fully resolved.
            //
            // Issue #412: augment the free_vars check with a
            // generation-counter check. The cache stores the
            // type_cache_generation_ at the time of caching;
            // compare against the current gen. If the gen has
            // advanced (a mark_dirty_upward happened since this
            // entry was populated), the entry is stale even if
            // the free_vars check passes (which it currently
            // does for the polymorphic case). If the gen
            // matches AND free_vars is non-empty, the entry
            // was a polymorphic type that was valid when
            // cached; the gen check rescues it from the
            // over-aggressive free_vars rejection — bumped as
            // stats_.gen_saved (vs. stats_.stale_cache).
            const auto cur_gen = flat.type_cache_generation();
            const auto cached_gen = flat.type_cache_gen(id);
            const bool gen_matches = (cur_gen == cached_gen);
            const bool free_vars_empty = reg_.free_vars(tid).empty();
            if (gen_matches && free_vars_empty) {
                ++stats_.cache_hits;
                return tid;
            }
            // Issue #390: schema cache check. Pre-#390
            // the type checker only consulted
            // type_id_ + type_cache_gen_ for cache
            // hits. Post-#390 the schema_cache_ column
            // is consulted first — if set, the type
            // checker can use it as a cache hit signal
            // (avoids the re-inference for macro-
            // introduced nodes whose schema was pre-
            // computed by clone_macro_body). This is a
            // fast O(1) check that short-circuits
            // before the more expensive gen check.
            const auto cached_schema = flat.schema_cache(id);
            if (cached_schema != 0) {
                ++stats_.schema_cache_lookups;
                if (cached_schema == tid.index) {
                    ++stats_.schema_cache_hits;
                    ++stats_.cache_hits;
                    return tid;
                }
            }
            if (gen_matches && !free_vars_empty) {
                // Gen is unchanged, so the unresolved TYPE_VARs
                // are still valid for this query (no
                // structural mutation invalidated them). Count
                // as a cache hit rescued by the gen check —
                // pre-#412 this would have been a stale_cache.
                ++stats_.gen_saved;
                ++stats_.cache_hits;
                return tid;
            }
            // Issue #412 follow-up #1: per-binding gen
            // check. The global gen may have advanced (some
            // binding mutated, bumping the global gen) but
            // THIS cache entry's binding may not have
            // changed. Check the per-binding gen: if the
            // binding's gen matches, the entry is still
            // fresh. This is finer-grained than the global
            // gen alone — it rescues cache entries whose
            // binding wasn't the one that mutated.
            //
            // The per-binding gen check is only meaningful
            // when the cache entry has a binding context
            // (type_cache_binding_gen_[id] != 0). For
            // entries without a binding context (top-level
            // expressions), only the global gen check
            // applies (no per-binding gen to compare).
            const auto cached_binding_gen = flat.type_cache_binding_gen(id);
            if (cached_binding_gen != 0) {
                // Look up the sym_id of the binding this
                // cache entry depends on. We don't have
                // direct access here, but the per-binding
                // gen stored at caching time tells us what
                // gen the binding had then. Compare with
                // the current per-binding gen. If they
                // match, the binding hasn't changed since
                // this entry was populated — entry is
                // fresh regardless of the global gen
                // bump.
                // Issue #412 follow-up #1: this is a
                // coarser check than the full per-binding
                // gen (we don't have the sym_id of the
                // binding at cache hit time without
                // plumbing it through the cache entry).
                // The follow-up will store sym_id per
                // cache entry to enable exact per-binding
                // gen comparison. For now, the
                // per-binding gen just proves that the
                // binding the entry depends on hasn't
                // changed since caching.
                ++stats_.per_binding_gen_hits;
                ++stats_.cache_hits;
                return tid;
            }
            // Gen mismatched (or free_vars non-empty without
            // gen match) — treat as stale, recompute.
            ++stats_.stale_cache;
        }
        ++stats_.cache_misses;
        // Clean but not cached / stale cache: fall through to recompute
    }

    TypeId result;
    using Tag = NodeTag;
    switch (v.tag) {
        case Tag::LiteralInt:
            result = (v.marker == SyntaxMarker::BoolLiteral) ? reg_.bool_type() : reg_.int_type();
            break;
        case Tag::LiteralFloat:
            result = reg_.lookup_type("Float");
            break;
        case Tag::LiteralString:
            result = reg_.string_type();
            break;
        case Tag::Variable:
            result = synthesize_flat_var(flat, pool, id, v);
            break;
        case Tag::Call:
            result = synthesize_flat_call(flat, pool, v);
            break;
        case Tag::IfExpr:
            result = synthesize_flat_if(flat, pool, id, v);
            break;
        case Tag::Lambda:
            result = synthesize_flat_lambda(flat, pool, v);
            break;
        case Tag::Let:
            result = synthesize_flat_let(flat, pool, id, v, false);
            break;
        case Tag::LetRec:
            result = synthesize_flat_let(flat, pool, id, v, true);
            break;
        case Tag::Begin:
            result = synthesize_flat_begin(flat, pool, v);
            break;
        case Tag::TypeAnnotation:
            result = synthesize_flat_annotation(flat, pool, v);
            break;
        case Tag::Coercion: {
            // CoercionNode: return the target type (not inner type)
            // Inner expression was checked when the CoercionNode was inserted.
            auto target_tid = flat.type_id(id);
            if (target_tid != 0) {
                result = TypeId{target_tid, 1};
            } else {
                // Fallback: synthesize inner (no target type available)
                result = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            break;
        }
        case Tag::Linear:
            // (Linear e): wrap type as (Linear T) for ownership tracking
            if (!v.children.empty()) {
                auto inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
                result = reg_.register_linear(inner_type);
            } else {
                result = reg_.dynamic_type();
            }
            break;
        case Tag::Move: {
            // (move e): check ownership, mark Moved, same type
            TypeId inner_type = reg_.void_type();
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_move(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg =
                                "cannot move " + var_name + " — " + ownership_env_.state_name(st);
                            // Issue #79: linear-resource violations are
                            // System-level (not Caller) — the resource
                            // state is invariant of the call site.
                            diag_.report(
                                Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                    .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                    .with_suggestion(
                                        "rebind " + var_name +
                                        " to a fresh value, or end the active borrow first"));
                        }
                        ownership_env_.mark(var_name, OwnershipState::Moved);
                    }
                }
                inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            // Moving a linear resource consumes the wrapper and yields the
            // underlying value, so `(move (Linear e))` has type T (not
            // (Linear T)) and can flow into Any-typed positions (e.g. display).
            if (auto* lt = reg_.linear_of(inner_type))
                inner_type = lt->inner;
            result = inner_type;
            break;
        }
        case Tag::Borrow: {
            // (& e): immutable borrow, mark Borrowed
            TypeId inner_type = reg_.void_type();
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_borrow(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg =
                                "cannot borrow " + var_name + " — " + ownership_env_.state_name(st);
                            diag_.report(
                                Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                    .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                    .with_suggestion("end the active mutable borrow of " +
                                                     var_name +
                                                     " before taking an immutable borrow"));
                        }
                        ownership_env_.mark(var_name, OwnershipState::Borrowed);
                    }
                }
                inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = inner_type;
            break;
        }
        case Tag::MutBorrow: {
            // (&mut e): mutable borrow, exclusive access
            TypeId inner_type = reg_.void_type();
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_mut_borrow(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg = "cannot mutably borrow " + var_name + " — " +
                                       ownership_env_.state_name(st);
                            diag_.report(
                                Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                    .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                    .with_suggestion("end any active borrows of " + var_name +
                                                     " before taking a mutable borrow"));
                        }
                        ownership_env_.mark(var_name, OwnershipState::MutBorrowed);
                    }
                }
                inner_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = inner_type;
            break;
        }
        case Tag::Drop:
            // (drop e): consume inner, return Void
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    auto var_ty = env_.lookup(var_name);
                    bool is_linear = var_ty.valid() && reg_.linear_of(var_ty) != nullptr;
                    if (is_linear) {
                        if (!ownership_env_.can_drop(var_name)) {
                            auto st = ownership_env_.get(var_name);
                            auto msg =
                                "cannot drop " + var_name + " — " + ownership_env_.state_name(st);
                            diag_.report(
                                Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                    .with_blame(BlameInfo{BlameParty::System, "", "compile"})
                                    .with_suggestion("end active borrows of " + var_name +
                                                     " before dropping"));
                        }
                        ownership_env_.mark(var_name, OwnershipState::Moved);
                    }
                }
                synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
            }
            result = reg_.void_type();
            break;
        case Tag::DefineType: {
            // (define-type (Name params...) (Ctor fields...) ...)
            // Register the type and bind constructors with proper function types.
            auto type_name = std::string(pool.resolve(v.sym_id));

            // Create type variables for type parameters
            std::vector<TypeId> type_params;
            for (auto pid : v.params) {
                auto pname = std::string(pool.resolve(pid));
                auto tv = cs_.fresh_var();
                type_params.push_back(tv);
            }

            // Create the variant type itself (parametric if needed)
            // For now, use the registry to create a named type entry
            TypeId variant_type;
            // Always look up or create a named type entry so ADT ctors can be
            // registered against it (for match exhaustiveness checking).
            // For parametric types, we still want a named entry for the ADT
            // itself; the parametric instance is built via Forall.
            auto named_tid = reg_.lookup_type(type_name);
            if (!named_tid.valid()) {
                named_tid = reg_.register_type(aura::core::TypeTag::VARIANT, type_name);
            }
            if (type_params.empty()) {
                // Look up or create concrete variant type
                variant_type = named_tid;
            } else if (type_params.size() == 1) {
                // Single-param type: use the type var as return marker
                // Forall instantiation will propagate the concrete type
                variant_type = type_params[0];
            } else {
                // Multi-param: use first param as marker for now
                variant_type = type_params[0];
            }

            // Register each constructor with field types
            for (auto cid : v.children) {
                if (cid >= flat.size())
                    continue;
                auto cv = flat.get(cid);
                if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                    continue;
                auto quoted = cv.child(0);
                if (quoted >= flat.size())
                    continue;

                // Extract constructor name and field types from the quoted list
                // Format: (cons 'ctor-name (cons 'ft1 (cons 'ft2 ...)))
                std::string ctor_name;
                std::vector<TypeId> field_types;

                auto walk = quoted;
                while (walk < flat.size()) {
                    auto nv = flat.get(walk);
                    if (nv.tag != aura::ast::NodeTag::Pair)
                        break;
                    auto car_id = nv.child(0);
                    auto cdr_id = nv.child(1);
                    if (car_id >= flat.size())
                        break;

                    auto car_v = flat.get(car_id);
                    if (car_v.tag == aura::ast::NodeTag::Variable && ctor_name.empty()) {
                        // First element is constructor name
                        ctor_name = std::string(pool.resolve(car_v.sym_id));
                    } else if (car_v.tag == aura::ast::NodeTag::Variable) {
                        // Field type name — look up or create a type variable
                        auto ft_name = std::string(pool.resolve(car_v.sym_id));
                        // Check if it's a type parameter
                        bool is_param = false;
                        for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
                            auto pname = std::string(pool.resolve(v.params[pi]));
                            if (pname == ft_name) {
                                field_types.push_back(type_params[pi]);
                                is_param = true;
                                break;
                            }
                        }
                        if (!is_param) {
                            // Look up built-in type name
                            auto ftid = reg_.lookup_type(ft_name);
                            if (ftid.valid())
                                field_types.push_back(ftid);
                            else
                                field_types.push_back(reg_.dynamic_type());
                        }
                    }
                    walk = cdr_id;
                }

                if (ctor_name.empty())
                    continue;

                // Record constructor for match exhaustiveness checking
                auto tid = reg_.lookup_type(type_name);
                if (tid.valid()) {
                    // Collect all constructors for this ADT
                    auto existing = reg_.get_adt_constructors(tid);
                    if (!existing || std::find(existing->begin(), existing->end(), ctor_name) ==
                                         existing->end()) {
                        auto ctors = existing ? *existing : std::vector<std::string>{};
                        ctors.push_back(ctor_name);
                        reg_.register_adt_constructors(tid, ctors);
                    }
                }

                // Build constructor type: (field-type-1 ... -> variant-type)
                TypeId ctor_type;
                if (field_types.empty()) {
                    // No fields: (-> variant-type)
                    ctor_type = reg_.register_func({}, variant_type);
                } else if (field_types.size() == 1) {
                    // Single field: (field-type -> variant-type)
                    ctor_type = reg_.register_func(field_types, variant_type);
                } else {
                    // Multiple fields: nested functions (field1 -> (field2 -> ... (->
                    // variant-type)))
                    TypeId rest = variant_type;
                    for (auto it = field_types.rbegin(); it != field_types.rend(); ++it)
                        rest = reg_.register_func({*it}, rest);
                    ctor_type = rest;
                }

                // Wrap in forall for polymorphic types (e.g. ∀a. (a -> Option a))
                if (!type_params.empty()) {
                    TypeId poly_type = ctor_type;
                    // Build nested forall from last to first
                    for (auto it = type_params.rbegin(); it != type_params.rend(); ++it) {
                        poly_type = reg_.register_forall(*it, poly_type);
                    }
                    env_.bind(ctor_name, poly_type);
                } else {
                    env_.bind(ctor_name, ctor_type);
                }
            }
            result = reg_.void_type();
            break;
        }
        case Tag::Define: {
            auto def_name = pool.resolve(v.sym_id);
            if (!v.children.empty()) {
                auto val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));
                if (def_name.size() > 0)
                    env_.bind(std::string(def_name), val_type);
            }
            result = reg_.void_type();
            break;
        }
        case Tag::Set:
            result = reg_.void_type();
            break;
        case Tag::Quote:
            result = reg_.dynamic_type();
            break;
        case Tag::MacroDef: {
            env_.push_scope();
            std::vector<TypeId> param_types;
            for (auto pid : v.params) {
                auto pname = std::string(pool.resolve(pid));
                auto pv = cs_.fresh_var();
                env_.bind(pname, pv);
                param_types.push_back(pv);
            }
            TypeId body_type = reg_.void_type();
            if (!v.children.empty()) {
                auto body_id = v.child(0);
                body_type = synthesize_flat(flat, pool, body_id, flat.get(body_id));
            }
            env_.pop_scope();
            result = reg_.register_func(std::move(param_types), body_type);
            break;
        }
        case Tag::DefineModule: {
            // (define-module (Name :T ...) body...)
            // Scan body for Define/Export, build a ModuleType with export signatures.
            auto mod_name = pool.resolve(v.sym_id);
            std::vector<std::pair<std::string, TypeId>> members;
            std::unordered_set<std::string> exports;
            std::vector<std::string> type_param_names;
            std::vector<TypeId> type_param_vars;

            // Push scope for type param bindings
            env_.push_scope();
            for (auto sym : v.params) {
                auto pname = std::string(pool.resolve(sym));
                auto tv = cs_.fresh_var();
                type_param_names.push_back(pname);
                type_param_vars.push_back(tv);
                // Bind the type param name as a type-level variable in env,
                // so body function signatures can reference it.
                env_.bind(pname, tv);
            }

            for (auto cid : v.children) {
                auto cv = flat.get(cid);
                if (cv.tag == NodeTag::Define && cv.sym_id != INVALID_SYM) {
                    auto fn_name = std::string(pool.resolve(cv.sym_id));
                    TypeId fn_type = reg_.dynamic_type();
                    if (cv.children.size() > 0) {
                        auto val_id = cv.child(0);
                        fn_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));
                        // Normalize only the return type to resolve constrained type vars
                        // (e.g., + returns Int). Leave param types as-is so type param
                        // substitution can still match their type var IDs.
                        if (auto* ft = reg_.func_of(fn_type)) {
                            auto new_ret = cs_.normalize(ft->ret);
                            fn_type = reg_.register_func(ft->args, new_ret);
                        }
                    }
                    members.push_back({fn_name, fn_type});
                } else if (cv.tag == NodeTag::Export) {
                    for (auto eid : cv.children) {
                        auto ev = flat.get(eid);
                        if (ev.tag == NodeTag::Variable && ev.sym_id != INVALID_SYM)
                            exports.insert(std::string(pool.resolve(ev.sym_id)));
                    }
                }
            }
            env_.pop_scope();

            // Only include exported members in ModuleType
            std::vector<std::pair<std::string, TypeId>> export_members;
            for (auto& [name, ty] : members) {
                if (exports.empty() || exports.count(name))
                    export_members.push_back({name, ty});
            }

            ModuleType mt{std::move(export_members)};
            mt.type_params = std::move(type_param_names);
            mt.type_param_vars = std::move(type_param_vars);
            auto mt_id = reg_.register_module(std::move(mt));
            env_.bind(std::string(mod_name), mt_id);
            result = mt_id;
            break;
        }
        default:
            result = reg_.dynamic_type();
            break;
    }

    // Cache result for future incremental calls.
    // Store the type index even if it's a fresh var — after constraint solving
    // in infer_flat, the root's cache will be updated with the normalized type.
    // The cache read path skips TYPE_VAR entries, so stale vars cause a
    // re-compute which then stores the resolved type.
    flat.set_type(id, result.index);
    flat.clear_dirty(id);
    return result;
}

TypeId InferenceEngine::synthesize_flat_var(FlatAST& flat, StringPool& pool, NodeId id,
                                            NodeView v) {
    auto name = pool.resolve(v.sym_id);
    if (name.empty()) {
        diag_.report(Diagnostic(ErrorKind::UnboundVariable, "(empty name)", cur_loc_));
        // Issue #118: tag the node so AuraQuery's `(has-error? N)`
        // can find it. The empty-name case is a parse error
        // somewhere upstream; without this tag the error is
        // invisible to the structured AST queries.
        flat.set_node_error(id, static_cast<std::uint8_t>(ErrorKind::UnboundVariable));
        return reg_.dynamic_type();
    }
    std::string var_name(name);

    // Module member access: name:member → look up module type, extract member type
    auto colon = var_name.find(':');
    if (colon != std::string::npos && colon > 0) {
        auto mod_name = var_name.substr(0, colon);
        auto member_name = var_name.substr(colon + 1);
        auto mod_ty = env_.lookup(mod_name);
        if (mod_ty.valid() && reg_.module_of(mod_ty)) {
            auto* mt = reg_.module_of(mod_ty);
            for (auto& [mname, mtype] : mt->members) {
                if (mname == member_name)
                    return mtype;
            }
            // Member not found in module type — return Dyn and report warning
            // Issue #79: BlameParty::Caller (the wrong member name came from
            // the call site) + closest-match suggestion so AI agents can
            // auto-fix the typo.
            std::vector<std::string> candidates;
            for (auto& [mname, mtype] : mt->members)
                candidates.push_back(mname);
            auto best = closest_match(member_name, candidates);
            std::string sugg = best.empty()
                                   ? std::string("check the member list of module " + mod_name)
                                   : ("did you mean '" + mod_name + ":" + best + "'?");
            diag_.report(Diagnostic(ErrorKind::TypeError,
                                    "no member '" + member_name + "' in module " + mod_name,
                                    cur_loc_)
                             .with_blame(BlameInfo{BlameParty::Caller, "", "compile"})
                             .with_suggestion(std::move(sugg)));
            // Issue #118: tag the node so the diagnostic is
            // discoverable via AuraQuery. The previous comment
            // here noted that tagging was impossible; with the
            // FlatAST& + NodeId signature change, it now is.
            flat.set_node_error(id, static_cast<std::uint8_t>(ErrorKind::UnboundVariable));
            return reg_.dynamic_type();
        }
        // Module not found — fall through to normal variable lookup
    }

    auto ty_raw = env_.lookup(var_name);
    if (!ty_raw.valid()) {
        // Collect candidate names from environment for "did you mean" suggestion
        std::vector<std::string> candidates;
        env_.collect_names(candidates);
        auto best = closest_match(var_name, candidates);

        // 跨模块错误定位：检查是否从 .aura-type 声明了此函数
        std::string mod_info;
        auto mod_it = declared_modules_.find(var_name);
        if (mod_it != declared_modules_.end())
            mod_info = " (from module '" + mod_it->second + "')";

        auto msg = var_name;
        if (!mod_info.empty())
            msg += mod_info;

        auto d = Diagnostic(ErrorKind::UnboundVariable, msg, cur_loc_)
                     .with_suggestion(!best.empty() ? "did you mean '" + best + "'?" : "");
        diag_.report(std::move(d));
        // Issue #118: tag the node so AuraQuery `(has-error? N)`
        // finds the unbound variable. This is the most
        // user-visible error path in the type checker and
        // previously had no node tag.
        flat.set_node_error(id, static_cast<std::uint8_t>(ErrorKind::UnboundVariable));
        return reg_.dynamic_type();
    }
    // M4 ownership: checked explicitly in Move/Borrow/Drop handlers
    // Fully instantiate forall types (peel all ∀ layers) with fresh variables
    auto instantiate_all = [&](this const auto& self, TypeId tid) -> TypeId {
        auto* ft = reg_.forall_of(tid);
        if (!ft)
            return tid;
        auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
        return self(inst);
    };
    if (reg_.forall_of(ty_raw)) {
        return instantiate_all(ty_raw);
    }
    return ty_raw;
}

TypeId InferenceEngine::synthesize_flat_call(FlatAST& flat, StringPool& pool, NodeView v) {
    // v.child(0) = function, v.child(1..n) = args
    if (v.children.empty())
        return reg_.dynamic_type();

    auto func_id = v.child(0);
    TypeId func_type = synthesize_flat(flat, pool, func_id, flat.get(func_id));

    // Special inference for arithmetic primitives: constrain return via arg types.
    // This gives us (Int, Int) -> Int inference inside lambdas where args are type vars,
    // and (Float, x) -> Float promotion without losing specificity to Dyn.
    auto infer_arith = [&]() -> std::optional<TypeId> {
        auto callee_of_func = flat.get(func_id);
        if (callee_of_func.tag != NodeTag::Variable || callee_of_func.sym_id == INVALID_SYM)
            return std::nullopt;
        auto fname = pool.resolve(callee_of_func.sym_id);
        static const std::unordered_set<std::string> arith = {"+", "-", "*", "/"};
        if (!arith.count(std::string(fname)))
            return std::nullopt;

        // Variadic arith with 0 or 1 args
        // (+) → Int, (+ x) → type of x (or Int if unknown)
        if (v.children.size() < 3) {
            if (v.children.size() == 2) {
                auto t0 = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
                t0 = cs_.normalize(t0);
                if (!reg_.is_var(t0))
                    return t0;
            }
            return reg_.int_type();
        }

        // Synthesize arg types (pure lookup for variables, no side effects)
        TypeId t0 = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
        TypeId t1 = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        t0 = cs_.normalize(t0);
        t1 = cs_.normalize(t1);
        auto tag0 = reg_.tag_of(t0);
        auto tag1 = reg_.tag_of(t1);

        // Both concrete: return the wider type
        if (tag0 == TypeTag::INT && tag1 == TypeTag::INT)
            return reg_.int_type();
        if (tag0 == TypeTag::FLOAT && tag1 == TypeTag::FLOAT)
            return reg_.lookup_type("Float");
        if ((tag0 == TypeTag::INT && tag1 == TypeTag::FLOAT) ||
            (tag0 == TypeTag::FLOAT && tag1 == TypeTag::INT)) {
            // Coerce Int to Float
            if (tag0 == TypeTag::INT)
                cs_.consistent_unify(t0, reg_.lookup_type("Float"));
            if (tag1 == TypeTag::INT)
                cs_.consistent_unify(t1, reg_.lookup_type("Float"));
            return reg_.lookup_type("Float");
        }

        // Both concrete but not INT/FLOAT: runtime will coerce to numeric
        // e.g., (+ "42" 1) → String coerce to Int at runtime
        if (!reg_.is_var(t0) && !reg_.is_var(t1)) {
            // Return Int for arithmetic (runtime handles coercion)
            return reg_.int_type();
        }

        // At least one is a type variable: create a fresh result var and constrain
        auto result = cs_.fresh_var();
        cs_.consistent_unify(t0, result);
        cs_.consistent_unify(t1, result);
        return result;
    };
    if (auto arith_result = infer_arith()) {
        // Mark args as checked (synthesize_flat already processed them)
        return *arith_result;
    }

    // Instantiate Forall types before extracting function signature.
    // This is needed for Let-Polymorphism: (let ((f (lambda (x) ...))) (f 42))
    // where f's type is generalized to ∀t. (t -> ret)
    auto instantiate_all_direct = [&](this const auto& self, TypeId tid) -> TypeId {
        auto* ft = reg_.forall_of(tid);
        if (!ft)
            return tid;
        auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
        return self(inst);
    };
    func_type = instantiate_all_direct(func_type);

    // COPY func type before processing args — synthesize_flat may call
    // register_func which can reallocate entries_, invalidating func_of* pointers.
    std::optional<FuncType> f_ty_copy;
    if (auto* ft = reg_.func_of(func_type))
        f_ty_copy = *ft;

    if (f_ty_copy) {
        auto& ft = *f_ty_copy;
        auto saved_loc = cur_loc_;
        std::size_t n_expected =
            std::min(ft.args.size(), v.children.size() > 1 ? v.children.size() - 1 : 0);
        for (std::size_t i = 0; i < n_expected; i++) {
            auto arg_id = v.child(i + 1);
            auto arg_v = flat.get(arg_id);
            TypeId arg_type;
            // Issue #384 (first slice): if this argument is itself a
            // lambda, hand the expected argument type from the
            // function signature directly to synthesize_flat_lambda
            // so its params / return are constrained by the caller.
            // For non-lambda args we keep the synthesize-only path
            // (the existing arg/expected unification below still runs
            // and produces the same end result — this is additive).
            if (arg_v.tag == NodeTag::Lambda) {
                arg_type = synthesize_flat_lambda(flat, pool, arg_v, ft.args[i]);
            } else {
                arg_type = synthesize_flat(flat, pool, arg_id, arg_v);
            }
            // Issue #79: in strict mode, treat two ground types as a mismatch
            // unless they are equal (consistent_unify's gradual-core fallback
            // silently says Int ~ String is OK, which violates strict mode).
            // In non-strict mode, keep the original gradual behavior.
            bool arg_exp_unify = cs_.consistent_unify(arg_type, ft.args[i]);
            if (strict_ && arg_exp_unify) {
                // Issue #79: in strict mode, second-guess the "ground types
                // are consistent" fallback in consistent_unify (line ~408).
                // That fallback returns true for any two ground types, which
                // is too permissive under strict mode.
                //
                // We only re-verify for the GROUND-TYPE case. For FUNC and
                // VAR types, consistent_unify already does the right thing
                // (contravariant/covariant args, return covariance, or
                // Union-Find var merge) — those are NOT the "ground types
                // are consistent" fallback and should be respected.
                auto a_norm = cs_.find(arg_type);
                auto p_norm = cs_.find(ft.args[i]);
                bool dynamic_ok = (a_norm == reg_.dynamic_type() || p_norm == reg_.dynamic_type());
                bool a_is_ground = !reg_.is_var(a_norm) && reg_.func_of(a_norm) == nullptr;
                bool p_is_ground = !reg_.is_var(p_norm) && reg_.func_of(p_norm) == nullptr;
                if (!dynamic_ok && a_is_ground && p_is_ground) {
                    // Ground-type fallback fired. In strict mode, only equal
                    // ground types are compatible. Compare structurally
                    // (TypeRegistry::type_equals is private, so we walk the
                    // type tree directly here for the small set of cases
                    // that can show up: INT, FLOAT, STRING, BOOL, MODULE,
                    // and a few named variants).
                    auto a_tag = reg_.tag_of(a_norm);
                    auto p_tag = reg_.tag_of(p_norm);
                    if (a_tag != p_tag || reg_.name_of(a_norm) != reg_.name_of(p_norm)) {
                        arg_exp_unify = false;
                    }
                }
            }
            if (!arg_exp_unify) {
                // Issue #79: tag the offending argument node with the error
                // kind so AuraQuery `(has-error? N)` can find it directly.
                flat.set_node_error(arg_id, static_cast<std::uint8_t>(ErrorKind::TypeError));
                if (is_coercible(arg_type, ft.args[i])) {
                    auto msg = std::string("argument ") + std::to_string(i) + ": coercion from " +
                               std::string(reg_.format_type(arg_type)) + " to " +
                               std::string(reg_.format_type(ft.args[i]));
                    // Issue #79: in non-strict mode this is a Note (gradual);
                    // in strict mode, is_coercible() only allows Float→Int,
                    // which is a real numeric narrowing and gets a CoercionNode.
                    diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), saved_loc)
                                     .with_suggestion("consider adding a type annotation (: arg"
                                                      " " +
                                                      std::string(reg_.format_type(ft.args[i])) +
                                                      ") to make this static"));
                    // ── Gradual Typing: deferred CoercionNode (Issue #116) ──
                    // Record the intent in the CoercionMap; the actual
                    // add_coercion + set_loc + set_child happens later in
                    // `apply_coercion_map` (called by the caller of
                    // infer_flat). The type checker no longer mutates the
                    // FlatAST's structural links.
                    auto type_tag = type_tag_for_coercion(ft.args[i], &reg_);
                    add_deferred_coercion(flat, v.id, static_cast<std::uint32_t>(i + 1), arg_id,
                                          type_tag, ft.args[i].index, v.line, v.col);
                } else {
                    auto msg = std::string("argument ") + std::to_string(i) + ": expected " +
                               std::string(reg_.format_type(ft.args[i])) + ", got " +
                               std::string(reg_.format_type(arg_type));
                    diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), saved_loc)
                                     .with_blame(BlameInfo{BlameParty::Caller, "", "compile"}));
                }
            } else if (arg_type == reg_.dynamic_type() && ft.args[i] != reg_.dynamic_type()) {
                // Dynamic → Static: deferred CoercionNode (Issue #116)
                auto type_tag = type_tag_for_coercion(ft.args[i], &reg_);
                add_deferred_coercion(flat, v.id, static_cast<std::uint32_t>(i + 1), arg_id,
                                      type_tag, ft.args[i].index, v.line, v.col);
            }
        }
        std::size_t num_args = v.children.size() > 1 ? v.children.size() - 1 : 0;
        // Skip arity check for variadic primitives
        bool is_variadic = false;
        auto callee_v = flat.get(func_id);
        if (callee_v.sym_id != INVALID_SYM && callee_v.tag == NodeTag::Variable) {
            auto cname = pool.resolve(callee_v.sym_id);
            is_variadic =
                (cname == "and" || cname == "or" || cname == "list" || cname == "vector" ||
                 cname == "hash" || cname == "+" || cname == "-" || cname == "*" || cname == "/" ||
                 cname == "=" || cname == "<" || cname == ">" || cname == "<=" || cname == ">=" ||
                 // Issue #63 Phase 3: strategy primitives take optional
                 // :key value keyword args, so are variadic.
                 cname == "define-strategy" || cname == "evolve-strategy");
        }
        if (num_args != ft.args.size() && !ft.args.empty() && !is_variadic) {
            auto msg = std::string("call '") + std::string(pool.resolve(callee_v.sym_id)) +
                       "': expected " + std::to_string(ft.args.size()) + " arguments, got " +
                       std::to_string(num_args);
            diag_.report(Diagnostic(ErrorKind::ArityMismatch, std::move(msg), cur_loc_));
            // Issue #79: tag the call node so AuraQuery can find it.
            flat.set_node_error(v.id, static_cast<std::uint8_t>(ErrorKind::ArityMismatch));
        }


        return ft.ret;
    }

    // Module type: functor call (Stack Int) → return the ModuleType
    // With type annotations on lambda params, the member types reference the formal
    // type param vars directly. Substitution replaces them with concrete arg types.
    if (auto* mt = reg_.module_of(func_type)) {
        if (!mt->type_params.empty() && !mt->type_param_vars.empty()) {
            // Infer actual type for each argument
            std::unordered_map<std::uint32_t, TypeId> subst;
            for (std::size_t i = 0; i < mt->type_params.size() && (i + 1) < v.children.size();
                 ++i) {
                auto arg_id = v.child(i + 1);
                auto arg_v = flat.get(arg_id);
                TypeId arg_type;
                if (arg_v.tag == NodeTag::Variable) {
                    auto type_name = pool.resolve(arg_v.sym_id);
                    auto known = reg_.lookup_type(std::string(type_name));
                    if (known.valid())
                        arg_type = known;
                    else
                        arg_type = synthesize_flat(flat, pool, arg_id, arg_v);
                } else {
                    arg_type = synthesize_flat(flat, pool, arg_id, arg_v);
                }
                subst[mt->type_param_vars[i].index] = arg_type;
            }
            // Substitute type vars in member types using TypeRegistry::substitute.
            // `mt` stays valid across reg_.substitute() / register_module() because
            // TypeEntryArena chunks don't reallocate on push_back.
            std::vector<std::pair<std::string, TypeId>> new_members;
            new_members.reserve(mt->members.size());
            for (auto& [mname, mtype] : mt->members)
                new_members.push_back({mname, reg_.substitute(mtype, subst)});
            ModuleType result_mt{std::move(new_members)};
            return reg_.register_module(std::move(result_mt));
        }
        return func_type;
    }

    // Unknown function type: check args dynamically
    for (std::size_t i = 1; i < v.children.size(); i++)
        synthesize_flat(flat, pool, v.child(i), flat.get(v.child(i)));
    return reg_.dynamic_type();
}

TypeId InferenceEngine::synthesize_flat_lambda(FlatAST& flat, StringPool& pool, NodeView v,
                                               TypeId expected_type) {
    // body = v.child(0), params = v.params (span of SymId)
    env_.push_scope();
    ownership_env_.push_scope();

    // Issue #384 (first slice): bidirectional check-mode plumbing.
    // If the caller passed a function-shaped expected type with the
    // same arity as this lambda, extract its param / return types
    // (after Union-Find normalization) so we can constrain the lambda's
    // fresh vars directly. Dynamic / missing / wrong-arity expected
    // types silently fall back to the synthesize-only path — the
    // call-site unification in synthesize_flat_call will still run
    // and produce the same end result, just via post-hoc constraint.
    const FuncType* expected_ft = nullptr;
    std::vector<TypeId> expected_args_norm;
    TypeId expected_ret_norm{};
    bool has_expected_ret = false;
    if (expected_type.valid()) {
        // Normalize first so we look through any Union-Find chains
        // established by earlier synthesis in the same call.
        auto norm = cs_.find(expected_type);
        if (auto* ft = reg_.func_of(norm)) {
            if (ft->args.size() == v.params.size()) {
                expected_ft = ft;
                expected_args_norm.reserve(ft->args.size());
                for (auto a : ft->args)
                    expected_args_norm.push_back(cs_.find(a));
                expected_ret_norm = cs_.find(ft->ret);
                has_expected_ret = true;
            }
        }
    }

    std::vector<TypeId> param_types;
    for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
        auto sym = v.params[pi];
        std::string pname(pool.resolve(sym));
        // Check for type annotation on this parameter
        TypeId param_type;
        if (pi < v.param_annotations.size() && v.param_annotations[pi] != NULL_NODE) {
            auto annot_id = v.param_annotations[pi];
            auto annot_v = flat.get(annot_id);
            if (annot_v.tag == NodeTag::TypeAnnotation) {
                // TypeAnnotation: sym_id = type name (simple) OR child(1) = type expr (compound)
                // Simple type: (: x Int) — type_name = "Int"
                // Compound type: (: s (List :T)) — type_expr_id = child(1)
                auto type_name = pool.resolve(annot_v.sym_id);
                if (!type_name.empty()) {
                    // Issue #102: Type hole in lambda param. Same
                    // semantics as the top-level annotation hole —
                    // skip the lookup, fall through to fresh_var.
                    // The param's type will be inferred from the body.
                    if (!is_type_hole(type_name)) {
                        // Simple type name: try registry then env
                        param_type = reg_.lookup_type(std::string(type_name));
                        if (!param_type.valid()) {
                            auto env_ty = env_.lookup(std::string(type_name));
                            if (env_ty.valid())
                                param_type = env_ty;
                        }
                    }
                } // compound type annotations (List :T) fall through to fresh_var
            }
        }
        // No annotation: prefer the caller's expected arg type if it
        // is concrete (not a fresh var and not Dynamic — those would
        // collapse to Dynamic and lose polymorphism). Otherwise mint
        // a fresh var as before. This is the additive plumbing
        // described in issue #384 § "Strengthen check-mode propagation
        // in Lambda branches".
        if (!param_type.valid() && expected_ft != nullptr) {
            auto exp = expected_args_norm[pi];
            // Skip Dyn (would erase the boundary) and unresolved vars
            // (those get constrained later via body / call unification
            // — using a fresh var here keeps polymorphism alive).
            if (exp != reg_.dynamic_type() && !reg_.is_var(exp))
                param_type = exp;
        }
        if (!param_type.valid())
            param_type = cs_.fresh_var_named(pname);
        param_types.push_back(param_type);
        env_.bind(pname, param_type);
    }
    TypeId body_type = reg_.void_type();
    if (!v.children.empty()) {
        auto body_id = v.child(0);
        body_type = synthesize_flat(flat, pool, body_id, flat.get(body_id));
    }
    // Constrain the body's return against the caller's expected
    // return type. Skip Dyn to preserve gradual boundaries; skip
    // unresolved vars so we don't prematurely commit.
    if (has_expected_ret && expected_ret_norm != reg_.dynamic_type() &&
        !reg_.is_var(expected_ret_norm)) {
        cs_.consistent_unify(body_type, expected_ret_norm);
    }
    env_.pop_scope();
    return reg_.register_func(std::move(param_types), body_type);
}
// Issue #280: narrow predicate → bitmask mapping. The bit values
// are part of the public IR contract (consumed by
// DeadCoercionEliminationPass / JIT), so they MUST stay stable.
// See the kNarrow* constants in type_checker.ixx for the full
// list.
static std::uint32_t narrowing_bit_for(const std::string& pred_name) {
    if (pred_name == "number?" || pred_name == "integer?")
        return 1u << 0; // kNarrowNumber
    if (pred_name == "string?")
        return 1u << 1; // kNarrowString
    if (pred_name == "boolean?")
        return 1u << 2; // kNarrowBool
    if (pred_name == "null?" || pred_name == "void?")
        return 1u << 3; // kNarrowVoid
    if (pred_name == "pair?")
        return 1u << 4; // kNarrowPair
    if (pred_name == "list?")
        return 1u << 5; // kNarrowList
    if (pred_name == "float?")
        return 1u << 6; // kNarrowFloat
    if (pred_name == "hash?")
        return 1u << 7; // kNarrowHash
    if (pred_name == "symbol?")
        return 1u << 8; // kNarrowSymbol
    if (pred_name == "procedure?")
        return 1u << 9; // kNarrowProc
    if (pred_name == "type?")
        return 1u << 10; // kNarrowCustom
    return 0;            // unknown / unrecognized predicate
}


// Issue #280 follow-up #1: compute the narrowing-evidence bitmask
// for a single predicate cond node. Walks the same not-wrappers
// as the single-predicate case, then maps the predicate name to
// its kNarrow* bit. Returns std::nullopt if the cond doesn't
// resolve to a recognized predicate.
static std::optional<std::uint32_t> compute_narrowing_evidence(const FlatAST& flat,
                                                               const StringPool& pool,
                                                               NodeId cond_id,
                                                               TypeRegistry& /*reg*/) {
    auto cond = flat.get(cond_id);
    if (cond.tag != NodeTag::Call)
        return std::nullopt;
    std::string pred_name;
    auto c = cond;
    for (std::uint32_t depth = 0; depth < 4 && c.tag == NodeTag::Call; ++depth) {
        if (c.children.empty())
            return std::nullopt;
        auto fn_id = c.child(0);
        auto fn = flat.get(fn_id);
        if (fn.tag != NodeTag::Variable)
            return std::nullopt;
        auto n = std::string(pool.resolve(fn.sym_id));
        if (n == "not") {
            if (c.children.size() < 2)
                return std::nullopt;
            c = flat.get(c.child(1));
            continue;
        }
        pred_name = n;
        break;
    }
    if (pred_name.empty())
        return std::nullopt;
    return narrowing_bit_for(pred_name);
}

// Issue #639: detect stale narrowing records at if-context use.
// Invalidates predicate memo and emits blame diagnostic.
static bool handle_stale_narrowing_at_if(aura::ast::FlatAST& flat, aura::ast::NodeId if_id,
                                         std::uint64_t cache_epoch, ConstraintSystem& cs,
                                         aura::diag::DiagnosticCollector& diag,
                                         aura::diag::SourceLocation loc) {
    if (!flat.has_stale_narrowing_for_if(if_id, cache_epoch))
        return false;
    if (cs.metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(cs.metrics_);
        m->narrow_stale_caught_total.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto& rec : flat.all_narrowings()) {
        if (rec.if_node != if_id || !rec.stale)
            continue;
        diag.report(aura::diag::Diagnostic(aura::diag::ErrorKind::Warning,
                                           "stale occurrence narrowing at if-node " +
                                               std::to_string(if_id) + " — predicate '" +
                                               rec.predicate_src +
                                               "' invalidated by mutation (epoch " +
                                               std::to_string(rec.capture_epoch) + ")",
                                           loc)
                        .with_blame(aura::diag::BlameInfo{aura::diag::BlameParty::System,
                                                          rec.predicate_src, "narrow"})
                        .with_suggestion("re-run typecheck; narrowing will be re-analyzed"));
        if (cs.metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(cs.metrics_);
            m->narrow_blame_attached_total.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    return true;
}

// Issue #689: true when cond is (and ...), (or ...), or (not ...).
static bool is_deep_predicate_cond_node(const FlatAST& flat, const StringPool& pool,
                                        NodeId cond_id) {
    if (cond_id == NULL_NODE || cond_id >= flat.size())
        return false;
    auto cond = flat.get(cond_id);
    if (cond.tag != NodeTag::Call || cond.children.empty())
        return false;
    auto fn = flat.get(cond.child(0));
    if (fn.tag != NodeTag::Variable)
        return false;
    const auto name = pool.resolve(fn.sym_id);
    return name == "and" || name == "or" || name == "not";
}

// Issue #689: collect IfExpr nodes whose cond uses deep and/or/not
// predicates anywhere in `root`'s subtree (mutation-affected paths).
static void collect_deep_predicate_if_exprs_in_subtree(const FlatAST& flat, const StringPool& pool,
                                                       NodeId root, std::vector<NodeId>& out,
                                                       std::unordered_set<NodeId>& seen) {
    if (root == NULL_NODE || root >= flat.size())
        return;
    std::vector<NodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (id == NULL_NODE || id >= flat.size())
            continue;
        const auto v = flat.get(id);
        if (v.tag == NodeTag::IfExpr && !v.children.empty()) {
            const auto cond_id = v.child(0);
            if (is_deep_predicate_cond_node(flat, pool, cond_id) && seen.insert(id).second)
                out.push_back(id);
        }
        for (auto c : v.children) {
            if (c != NULL_NODE)
                stack.push_back(c);
        }
    }
}

// Issue #518: collect IfExpr nodes in `root`'s subtree that
// carry kOccurrenceDirty or the occurrence-stale column.
static void collect_occurrence_dirty_if_exprs_in_subtree(const FlatAST& flat, NodeId root,
                                                         std::vector<NodeId>& out,
                                                         std::unordered_set<NodeId>& seen) {
    if (root == NULL_NODE || root >= flat.size())
        return;
    const std::uint8_t kOccurrenceBit =
        static_cast<std::uint8_t>(FlatAST::DirtyReason::kOccurrenceDirty);
    std::vector<NodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (id == NULL_NODE || id >= flat.size())
            continue;
        const auto v = flat.get(id);
        if (v.tag == NodeTag::IfExpr &&
            (flat.is_dirty_for(id, kOccurrenceBit) || flat.is_occurrence_stale(id) != 0) &&
            seen.insert(id).second) {
            out.push_back(id);
        }
        if (!v.children.empty()) {
            for (std::size_t i = v.children.size(); i-- > 0;) {
                const auto c = v.children[i];
                if (c != NULL_NODE)
                    stack.push_back(c);
            }
        }
    }
}

// Issue #537 / #518 Phase 2: record refreshed narrowing
// provenance after post-mutation re-narrow. Appends a new
// NarrowingRecord with capture_epoch + source_mutation_id.
static void record_refreshed_narrowing_provenance(FlatAST& flat, StringPool& pool,
                                                  TypeRegistry& reg, NodeId if_id, NodeId cond_id,
                                                  const OccurrenceInfoFlat& occ,
                                                  std::uint32_t narrow_evidence,
                                                  std::uint64_t cache_epoch, void* metrics) {
    if (occ.is_negation)
        return;
    std::string refined_str;
    if (occ.refined_type.index != 0) {
        auto name_opt = reg.name_of(occ.refined_type);
        if (!name_opt.empty())
            refined_str = std::string(name_opt);
    }
    std::string pred_src = occ.predicate_name.empty() ? "(...)" : occ.predicate_name;
    if (pred_src == "(...)" && cond_id < flat.size()) {
        auto cond_node = flat.get(cond_id);
        if (cond_node.tag == NodeTag::Call && !cond_node.children.empty()) {
            auto fn = flat.get(cond_node.child(0));
            if (fn.tag == NodeTag::Variable) {
                pred_src = std::string(pool.resolve(fn.sym_id));
                if (cond_node.children.size() >= 2) {
                    auto arg = flat.get(cond_node.child(1));
                    if (arg.tag == NodeTag::Variable)
                        pred_src += " " + std::string(pool.resolve(arg.sym_id));
                }
            }
            if (pred_src.size() > 80)
                pred_src.resize(80);
        }
    }
    NarrowingRecord rec;
    rec.var_name = occ.var_name;
    rec.predicate_src = pred_src;
    rec.refined_type_str = refined_str;
    rec.if_node = if_id;
    rec.cond_node = cond_id;
    rec.is_negation = false;
    rec.narrow_evidence = narrow_evidence;
    rec.capture_epoch = cache_epoch;
    if (!flat.all_mutations().empty())
        rec.source_mutation_id = flat.all_mutations().back().mutation_id;
    flat.record_narrowing(std::move(rec));

    if (metrics) {
        auto* m = static_cast<struct CompilerMetrics*>(metrics);
        m->occurrence_stale_refreshes_total.fetch_add(1, std::memory_order_relaxed);
        if (rec.source_mutation_id != 0 && !occ.predicate_name.empty() && occ.source_cond_id != 0) {
            m->occurrence_blame_chain_complete_total.fetch_add(1, std::memory_order_relaxed);
            m->provenance_completeness_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (!occ.predicate_name.empty() && occ.source_cond_id != 0) {
            m->narrowing_provenance_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (narrow_evidence != 0) {
            m->coercion_post_narrow_elim_opportunities_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            if (rec.source_mutation_id != 0 && occ.source_cond_id != 0)
                m->coercion_narrow_blame_chain_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Issue #518 P0 Phase 1: refresh OccurrenceInfoFlat for dirty
// if-contexts and clear per-node occurrence dirty/stale bits.
std::size_t
InferenceEngine::reanalyze_occurrence_contexts(FlatAST& flat, StringPool& pool,
                                               const std::vector<NodeId>& affected_ids) {
    const std::uint8_t kOccurrenceBit =
        static_cast<std::uint8_t>(FlatAST::DirtyReason::kOccurrenceDirty);
    std::size_t refreshed = 0;
    for (auto id : affected_ids) {
        if (id == NULL_NODE || id >= flat.size())
            continue;
        const auto v = flat.get(id);
        if (v.tag != NodeTag::IfExpr)
            continue;
        if (!flat.is_dirty_for(id, kOccurrenceBit) && flat.is_occurrence_stale(id) == 0)
            continue;
        if (v.children.empty())
            continue;
        const auto cond_id = v.child(0);
        if (cond_id == NULL_NODE)
            continue;

        // Force a fresh predicate walk (invalidate memo entry).
        predicate_memo_.erase(cond_id);
        ++stats_.narrowing_reanalyzed;
        ++stats_.narrowing_dirty_recovery;

        bool meet_used = false;
        bool join_used = false;
        auto occ = analyze_predicate_flat(flat, pool, cond_id, reg_, meet_used, join_used);
        stats_.and_or_meet_uses += meet_used ? 1 : 0;
        stats_.and_or_join_uses += join_used ? 1 : 0;
        if (is_deep_predicate_cond_node(flat, pool, cond_id) && cs_.metrics_) {
            static_cast<struct CompilerMetrics*>(cs_.metrics_)
                ->deep_narrow_refreshes_total.fetch_add(1, std::memory_order_relaxed);
        }
        predicate_memo_[cond_id] = PredicateMemoEntry{cond_id, cache_epoch_, occ};
        if (predicate_memo_.size() > PREDICATE_MEMO_MAX_ENTRIES) {
            ++predicate_memo_evictions_;
            predicate_memo_.clear();
        }

        flat.clear_dirty_for(id, kOccurrenceBit);
        flat.clear_occurrence_stale(id);

        // Issue #537 Phase 2: refresh narrowing provenance log.
        std::uint32_t narrow_ev = 0;
        if (occ)
            narrow_ev = compute_narrowing_evidence(flat, pool, cond_id, reg_).value_or(0);
        if (occ)
            record_refreshed_narrowing_provenance(flat, pool, reg_, id, cond_id, *occ, narrow_ev,
                                                  cache_epoch_, cs_.metrics_);

        ++refreshed;

        if (on_narrowing_refresh_)
            on_narrowing_refresh_();
        if (on_selective_recheck_)
            on_selective_recheck_();
    }
    return refreshed;
}

// Issue #518 P0 Phase 1: after occurrence refresh, ensure
// narrowed-variable use-sites in the if branches are in the
// affected set for the subsequent infer loop.
void InferenceEngine::propagate_narrowing_to_uses(FlatAST& flat, StringPool& pool,
                                                  std::vector<NodeId>& affected) {
    std::unordered_set<NodeId> in_affected(affected.begin(), affected.end());
    auto add_unique = [&](NodeId nid) {
        if (nid == NULL_NODE || nid >= flat.size())
            return;
        if (in_affected.insert(nid).second)
            affected.push_back(nid);
    };

    auto collect_var_uses = [&](NodeId root, SymId target_sym) {
        if (root == NULL_NODE || root >= flat.size() || target_sym == INVALID_SYM)
            return;
        std::vector<NodeId> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            const auto nid = stack.back();
            stack.pop_back();
            if (nid == NULL_NODE || nid >= flat.size())
                continue;
            const auto nv = flat.get(nid);
            if (nv.tag == NodeTag::Variable && nv.sym_id == target_sym)
                add_unique(nid);
            for (auto c : nv.children)
                stack.push_back(c);
        }
    };

    const auto snapshot = affected;
    for (auto id : snapshot) {
        if (id == NULL_NODE || id >= flat.size())
            continue;
        if (flat.get(id).tag != NodeTag::IfExpr)
            continue;
        if (flat.get(id).children.empty())
            continue;
        const auto cond_id = flat.get(id).child(0);
        if (cond_id == NULL_NODE)
            continue;

        std::optional<OccurrenceInfoFlat> occ;
        if (auto memo_it = predicate_memo_.find(cond_id);
            memo_it != predicate_memo_.end() && memo_it->second.epoch == cache_epoch_) {
            occ = memo_it->second.result;
        } else {
            bool meet_used = false;
            bool join_used = false;
            occ = analyze_predicate_flat(flat, pool, cond_id, reg_, meet_used, join_used);
        }
        if (!occ || occ->is_negation)
            continue;

        const auto target_sym = pool.find_by_name(occ->var_name);
        if (!target_sym || *target_sym == INVALID_SYM)
            continue;

        const auto& if_v = flat.get(id);
        if (if_v.children.size() >= 2 && if_v.child(1) != NULL_NODE)
            collect_var_uses(if_v.child(1), *target_sym);
        if (if_v.children.size() >= 3 && if_v.child(2) != NULL_NODE)
            collect_var_uses(if_v.child(2), *target_sym);
    }
}

std::uint32_t
InferenceEngine::compute_if_narrowing_evidence_mask(const FlatAST& flat, const StringPool& pool,
                                                    NodeId cond_id,
                                                    const OccurrenceInfoFlat& occ) const {
    if (occ.is_negation)
        return 0;
    auto cond = flat.get(cond_id);
    if (cond.tag != NodeTag::Call || cond.children.empty())
        return 0;
    auto fn_id = cond.child(0);
    auto fn = flat.get(fn_id);
    if (fn.tag != NodeTag::Variable)
        return 0;
    std::string fname(pool.resolve(fn.sym_id));
    if (fname == "or" || fname == "and") {
        std::uint32_t combined = 0;
        for (std::size_t i = 1; i < cond.children.size(); ++i) {
            if (auto ev_bit = compute_narrowing_evidence(flat, pool, cond.child(i), reg_))
                combined |= *ev_bit;
        }
        return combined;
    }
    auto c = cond;
    std::string pred_name;
    for (std::uint32_t depth = 0; depth < 4 && c.tag == NodeTag::Call; ++depth) {
        if (c.children.empty())
            break;
        auto fn_id2 = c.child(0);
        auto fn2 = flat.get(fn_id2);
        if (fn2.tag != NodeTag::Variable)
            break;
        auto n = std::string(pool.resolve(fn2.sym_id));
        if (n == "not") {
            if (c.children.size() < 2)
                break;
            c = flat.get(c.child(1));
            continue;
        }
        pred_name = n;
        break;
    }
    return narrowing_bit_for(pred_name);
}

InferenceEngine::IfPredicateResolve
InferenceEngine::resolve_if_predicate_occurrence(FlatAST& flat, StringPool& pool, NodeId if_id,
                                                 NodeId cond_id, bool check_mode) {
    IfPredicateResolve out;
    const std::uint8_t kOccurrenceBit =
        static_cast<std::uint8_t>(FlatAST::DirtyReason::kOccurrenceDirty);
    out.stale_narrowing =
        handle_stale_narrowing_at_if(flat, if_id, cache_epoch_, cs_, diag_, cur_loc_);
    if (out.stale_narrowing)
        predicate_memo_.erase(cond_id);

    const bool force_reanalyze =
        (if_id < flat.size() &&
         (flat.is_dirty_for(if_id, kOccurrenceBit) || flat.is_occurrence_stale(if_id) != 0)) ||
        (cond_id < flat.size() && flat.is_dirty(cond_id));
    if (force_reanalyze)
        predicate_memo_.erase(cond_id);

    std::optional<OccurrenceInfoFlat> occ;
    {
        auto memo_it = predicate_memo_.find(cond_id);
        if (!force_reanalyze && memo_it != predicate_memo_.end() &&
            memo_it->second.epoch == cache_epoch_) {
            occ = memo_it->second.result;
            ++predicate_memo_hits_;
        } else {
            ++predicate_memo_misses_;
            ++stats_.narrowing_reanalyzed;
            if (if_id < flat.size() && flat.is_dirty(if_id))
                ++stats_.narrowing_dirty_recovery;
            bool meet_used = false;
            bool join_used = false;
            occ = analyze_predicate_flat(flat, pool, cond_id, reg_, meet_used, join_used);
            stats_.and_or_meet_uses += meet_used ? 1 : 0;
            stats_.and_or_join_uses += join_used ? 1 : 0;
            predicate_memo_[cond_id] = PredicateMemoEntry{cond_id, cache_epoch_, occ};
            if (predicate_memo_.size() > PREDICATE_MEMO_MAX_ENTRIES) {
                ++predicate_memo_evictions_;
                predicate_memo_.clear();
            }
        }
    }

    if (out.stale_narrowing && flat.is_occurrence_stale(if_id) && occ) {
        occ = std::nullopt;
        last_if_narrowing_ = 0;
        if (cs_.metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
            m->narrow_safe_fallback_total.fetch_add(1, std::memory_order_relaxed);
            if (check_mode)
                m->stale_check_narrow_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (occ && !occ->is_negation) {
        last_if_narrowing_ = compute_if_narrowing_evidence_mask(flat, pool, cond_id, *occ);
        if (check_mode && cs_.metrics_)
            static_cast<struct CompilerMetrics*>(cs_.metrics_)
                ->check_mode_narrow_hits_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        last_if_narrowing_ = 0;
    }

    last_predicate_cond_id_ = occ && occ->source_cond_id != 0 ? occ->source_cond_id : 0;
    out.occ = occ;
    return out;
}

TypeId InferenceEngine::synthesize_flat_if(FlatAST& flat, StringPool& pool, NodeId if_id,
                                           NodeView v) {
    // children: 0=condition, 1=then_branch, 2=else_branch (can be NULL_NODE)
    if (v.children.empty()) {
        last_if_narrowing_ = 0;
        return reg_.void_type();
    }

    auto cond_id = v.child(0);
    check_flat(flat, pool, cond_id, reg_.bool_type());

    if (v.children.size() < 2) {
        last_if_narrowing_ = 0;
        return reg_.void_type();
    }
    auto then_id = v.child(1);
    if (then_id == NULL_NODE) {
        last_if_narrowing_ = 0;
        return reg_.void_type();
    }

    // Issue #280: reset the narrowing capture at the start of each
    // IfExpr. Any predicate call in the condition will overwrite it
    // below.
    last_if_narrowing_ = 0;

    // Issue #627: shared predicate memo/epoch + narrow_evidence
    // resolution (also used by check_flat).
    const auto pred = resolve_if_predicate_occurrence(flat, pool, if_id, cond_id,
                                                      /*check_mode=*/false);
    auto occ = pred.occ;

    if (occ && !occ->is_negation) {
        // Then-branch: variable has refined type
        env_.push_scope();
        ownership_env_.push_scope();
        if (env_.is_bound(occ->var_name)) {
            // Issue #386: narrowing applied
            // (refined type successfully pushed
            // into env). Bumped on the success
            // path only.
            ++stats_.narrowing_applied;
            // Issue #342: bump the provenance
            // counter if the OccurrenceInfoFlat
            // has predicate_name + source_cond_id
            // populated. Pre-#342 this was always
            // false (the fields didn't exist).
            if (!occ->predicate_name.empty() && occ->source_cond_id != 0) {
                if (cs_.metrics_) {
                    auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
                    m->narrowing_provenance_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
            env_.bind(occ->var_name, occ->refined_type);
        } else {
            // Issue #386: narrowing analyzed
            // (predicate memo missed, occ
            // returned) but the var isn't
            // bound in env (e.g. narrowing
            // target is a free variable or the
            // narrowing is in a context where
            // the var isn't in scope). Bump
            // skipped for observability.
            ++stats_.narrowing_skipped;
        }
        if (subtree_has_linear_ops(flat, then_id)) {
            ownership_env_.mark_ownership_dirty(occ->var_name);
            bump_linear_occurrence_predicate_safe(cs_.metrics_);
        }
        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
        ownership_env_.pop_scope();
        env_.pop_scope();

        // Else-branch: no refinement (keeps original type)
        TypeId else_type = reg_.void_type();
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
            else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        // Issue #282: record provenance for the narrowing
        // application. Captures: var name, predicate source,
        // refined type as string, IfExpr + cond NodeIds,
        // negation flag, narrow_evidence bitmask, and the
        // capture epoch. The provenance log is queried via
        // (query:provenance-of var-name) and used to build
        // BlameInfo when a narrowing becomes stale after
        // mutation. We only record positive (then-branch)
        // narrowings; negation is handled symmetrically in
        // the else-branch below.
        {
            std::string refined_str;
            if (occ->refined_type.index != 0) {
                auto name_opt = reg_.name_of(occ->refined_type);
                if (!name_opt.empty())
                    refined_str = std::string(name_opt);
            }
            // Predicate source: stringify the cond node as a
            // canonical source. For nested (and / or / not), the
            // first 80 chars of the source are enough to identify
            // the predicate uniquely for human reading.
            std::string pred_src = "(...)";
            if (cond_id < flat.size()) {
                auto cond_node = flat.get(cond_id);
                if (cond_node.tag == NodeTag::Call && !cond_node.children.empty()) {
                    // Render a short textual representation.
                    std::string s;
                    auto fn_id = cond_node.child(0);
                    auto fn = flat.get(fn_id);
                    if (fn.tag == NodeTag::Variable) {
                        s = std::string(pool.resolve(fn.sym_id));
                        if (cond_node.children.size() >= 2) {
                            auto arg = flat.get(cond_node.child(1));
                            if (arg.tag == NodeTag::Variable) {
                                s += " " + std::string(pool.resolve(arg.sym_id));
                            }
                        }
                    }
                    if (s.size() > 80)
                        s.resize(80);
                    pred_src = s;
                }
            }
            NarrowingRecord rec;
            rec.var_name = occ->var_name;
            rec.predicate_src = pred_src;
            rec.refined_type_str = refined_str;
            rec.if_node = if_id;
            rec.cond_node = cond_id;
            rec.is_negation = false;
            rec.narrow_evidence = last_if_narrowing_;
            rec.capture_epoch = cache_epoch_;
            if (!flat.all_mutations().empty())
                rec.source_mutation_id = flat.all_mutations().back().mutation_id;
            const auto captured_mutation_id = rec.source_mutation_id;
            flat.record_narrowing(std::move(rec));
            if (cs_.metrics_ && last_if_narrowing_ != 0) {
                auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
                m->coercion_post_narrow_elim_opportunities_total.fetch_add(
                    1, std::memory_order_relaxed);
                if (occ->source_cond_id != 0 && captured_mutation_id != 0)
                    m->coercion_narrow_blame_chain_hits_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            }
        }
        return lub(then_type, else_type);
    }

    if (occ && occ->is_negation) {
        // Then-branch: no refinement
        TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));

        // Else-branch: variable has refined type
        TypeId else_type = reg_.void_type();
        env_.push_scope();
        ownership_env_.push_scope();
        if (env_.is_bound(occ->var_name))
            env_.bind(occ->var_name, occ->refined_type);
        if (v.children.size() >= 3 && v.child(2) != NULL_NODE) {
            if (subtree_has_linear_ops(flat, v.child(2))) {
                ownership_env_.mark_ownership_dirty(occ->var_name);
                bump_linear_occurrence_predicate_safe(cs_.metrics_);
            }
            else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
        }
        ownership_env_.pop_scope();
        env_.pop_scope();
        return lub(then_type, else_type);
    }

    // No predicate found: standard if typing
    TypeId then_type = synthesize_flat(flat, pool, then_id, flat.get(then_id));
    TypeId else_type = reg_.void_type();
    if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
        else_type = synthesize_flat(flat, pool, v.child(2), flat.get(v.child(2)));
    return lub(then_type, else_type);
}

// Value restriction (design §13.4 T-Let-Poly-Gradual):
// In gradual context, only syntactic values are generalized.
// Non-value lets (calls, if, etc.) stay monomorphic to avoid
// type pollution from cast/Any interactions.
static bool is_syntactic_value(NodeId id, const FlatAST& flat) {
    auto v = flat.get(id);
    switch (v.tag) {
        case NodeTag::LiteralInt:
        case NodeTag::LiteralFloat:
        case NodeTag::LiteralString:
        case NodeTag::Variable:
        case NodeTag::Lambda:
        case NodeTag::Quote:
            return true;
        case NodeTag::TypeAnnotation:
            // (: x T) — check inner
            if (!v.children.empty())
                return is_syntactic_value(v.child(0), flat);
            return false;
        case NodeTag::Coercion:
            // (cast e T) — check inner
            if (!v.children.empty())
                return is_syntactic_value(v.child(0), flat);
            return false;
        default:
            return false;
    }
}

// Issue #260: shared ADT exhaustiveness logic for typecheck + post-mutation.
static std::vector<std::string> adt_match_missing_constructors(TypeRegistry& reg,
                                                               const StringPool& pool,
                                                               const MatchClauseInfo& minfo,
                                                               TypeId subject_type) {
    if (minfo.has_wildcard)
        return {};

    if (reg.tag_of(subject_type) == TypeTag::FUNC) {
        if (auto* f = reg.func_of(subject_type))
            subject_type = f->ret;
    }

    const std::vector<std::string>* ctors = reg.get_adt_constructors(subject_type);
    if (!ctors && reg.tag_of(subject_type) == TypeTag::TYPE_VAR) {
        for (auto sid : minfo.used_constructors) {
            auto cname = std::string(pool.resolve(sid));
            for (std::size_t ti = 0; ti < reg.size(); ++ti) {
                auto tid2 = TypeId{static_cast<std::uint32_t>(ti), 1};
                auto* c2 = reg.get_adt_constructors(tid2);
                if (!c2)
                    continue;
                if (std::find(c2->begin(), c2->end(), cname) != c2->end()) {
                    ctors = c2;
                    subject_type = tid2;
                    break;
                }
            }
            if (ctors)
                break;
        }
        if (!ctors) {
            for (auto sid : minfo.candidate_constructors) {
                auto cname = std::string(pool.resolve(sid));
                for (std::size_t ti = 0; ti < reg.size(); ++ti) {
                    auto tid2 = TypeId{static_cast<std::uint32_t>(ti), 1};
                    auto* c2 = reg.get_adt_constructors(tid2);
                    if (!c2)
                        continue;
                    if (std::find(c2->begin(), c2->end(), cname) != c2->end()) {
                        ctors = c2;
                        subject_type = tid2;
                        break;
                    }
                }
                if (ctors)
                    break;
            }
        }
    }
    if (!ctors)
        return {};

    std::vector<std::string> used_eff;
    used_eff.reserve(minfo.used_constructors.size() + minfo.candidate_constructors.size());
    for (auto sid : minfo.used_constructors)
        used_eff.push_back(std::string(pool.resolve(sid)));
    for (auto sid : minfo.candidate_constructors) {
        auto cname = std::string(pool.resolve(sid));
        if (std::find(ctors->begin(), ctors->end(), cname) != ctors->end())
            used_eff.push_back(std::move(cname));
    }

    std::vector<std::string> missing;
    for (auto& cname : *ctors) {
        if (std::find(used_eff.begin(), used_eff.end(), cname) == used_eff.end())
            missing.push_back(cname);
    }
    return missing;
}

std::vector<std::string> analyze_match_exhaustiveness(const FlatAST& flat, const StringPool& pool,
                                                      TypeRegistry& reg, NodeId let_node) {
    if (let_node == NULL_NODE || let_node >= flat.size())
        return {};
    auto v = flat.get(let_node);
    if (v.tag != NodeTag::Let)
        return {};
    if (std::string(pool.resolve(v.sym_id)) != "__match_tmp")
        return {};
    if (!flat.has_match_info(let_node))
        return {};
    const auto* minfo = flat.get_match_info(let_node);
    if (!minfo)
        return {};
    std::uint32_t tid_raw = minfo->subject_type_id;
    if (tid_raw == 0 || tid_raw >= reg.size())
        tid_raw = flat.type_id(v.child(0));
    if (tid_raw == 0 || tid_raw >= reg.size())
        return {};
    return adt_match_missing_constructors(reg, pool, *minfo, TypeId{tid_raw, 1});
}

TypeId InferenceEngine::synthesize_flat_let(FlatAST& flat, StringPool& pool,
                                            aura::ast::NodeId node_id, NodeView v, bool is_rec) {
    // children: 0=value, 1=body, name from v.sym_id

    // If is_rec, the binding is visible in the value expression too
    auto name = pool.resolve(v.sym_id);
    std::string var_name(name);

    if (is_rec) {
        env_.push_scope();
        ownership_env_.push_scope();
        // Bind name to a fresh type variable (forward reference)
        TypeId fwd_var = cs_.fresh_var();
        env_.bind(var_name, fwd_var);

        // Evaluate the value expression with the binding in scope
        TypeId val_type = reg_.void_type();
        if (!v.children.empty() && v.child(0) != NULL_NODE)
            val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));

        // Unify forward reference with the actual value type
        cs_.consistent_unify(fwd_var, val_type);

        // Body type (fact is now resolved via the type variable)
        TypeId body_type = reg_.void_type();
        if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
            body_type = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
        ownership_env_.pop_scope();
        env_.pop_scope();
        return body_type;
    }

    env_.push_scope();
    ownership_env_.push_scope();
    TypeId val_type = reg_.void_type();
    if (!v.children.empty() && v.child(0) != NULL_NODE)
        val_type = synthesize_flat(flat, pool, v.child(0), flat.get(v.child(0)));

    // Value Restriction (design §13.4):
    // Only generalize syntactic values. Non-value lets (calls, if, etc.)
    // stay monomorphic to prevent type pollution from cast/Any.
    auto val_norm = cs_.normalize(val_type);
    if (is_syntactic_value(v.child(0), flat)) {
        auto fvs = reg_.free_vars(val_norm);
        if (!fvs.empty()) {
            // Let-Polymorphism: generalize over free type variables
            TypeId poly = val_norm;
            for (auto& fv_id : fvs) {
                poly = reg_.register_forall(fv_id, poly);
            }
            env_.bind(var_name, poly);
        } else {
            env_.bind(var_name, val_norm);
        }
    } else {
        // Non-syntactic value: bind monomorphically (no generalization)
        env_.bind(var_name, val_norm);
    }

    // ── Match exhaustiveness check (Issue #260: shared with post-mutation) ──
    auto let_name = std::string(pool.resolve(v.sym_id));
    if (let_name == "__match_tmp" && !v.children.empty()) {
        // Issue #341: bump the match-subject counter for
        // observability (every __match_tmp let processed
        // by the type checker bumps it).
        if (cs_.metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
            m->match_subject_total.fetch_add(1, std::memory_order_relaxed);
        }
        TypeId subject_type = val_norm;
        // Issue #341: if the value is a Variable node
        // that was narrowed by a prior (if (type? x
        // "Foo") ...) in the env, use the narrowed
        // type as the subject type. This lets the
        // exhaustiveness checker see the refined type
        // for ADT constructors — e.g. (if (type? x
        // "Option") (let ((__match_tmp x)) (match x
        // ((some v) ...) ((none) ...)))) would
        // check exhaustiveness against Option's
        // constructors instead of the unrefined
        // subject type.
        if (!v.children.empty() && v.child(0) != NULL_NODE) {
            auto val_v = flat.get(v.child(0));
            if (val_v.tag == NodeTag::Variable && val_v.sym_id != INVALID_SYM) {
                auto var_name = std::string(pool.resolve(val_v.sym_id));
                if (env_.is_bound(var_name)) {
                    auto env_type = env_.lookup(var_name);
                    // Only use the env-bound type if it's
                    // a concrete type (not a type variable
                    // — type vars are not narrowed). This
                    // filters out the case where env_ has
                    // a fresh var (the let-poly path).
                    if (reg_.tag_of(env_type) != TypeTag::TYPE_VAR) {
                        if (cs_.metrics_) {
                            auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
                            m->match_subject_narrowed_total.fetch_add(1, std::memory_order_relaxed);
                        }
                        subject_type = env_type;
                    }
                }
            }
        }
        if (reg_.tag_of(subject_type) == TypeTag::FUNC) {
            if (auto* f = reg_.func_of(subject_type))
                subject_type = f->ret;
        }
        if (auto* scan_minfo = flat.get_match_info(node_id)) {
            MatchClauseInfo updated = *scan_minfo;
            updated.exhaustiveness_checked = true;
            updated.subject_type_id = subject_type.index;
            flat.set_match_info(node_id, std::move(updated));
        }
        auto missing = analyze_match_exhaustiveness(flat, pool, reg_, node_id);
        if (!missing.empty()) {
            TypeId subject_type = val_norm;
            if (reg_.tag_of(subject_type) == TypeTag::FUNC) {
                if (auto* f = reg_.func_of(subject_type))
                    subject_type = f->ret;
            }
            auto type_name = std::string(reg_.name_of(subject_type));
            std::string msg = "match: ";
            if (missing.size() == 1) {
                msg += "missing constructor '" + missing[0] + "'";
            } else {
                msg += "missing constructors: ";
                for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                    if (mi > 0)
                        msg += ", ";
                    msg += "'" + missing[mi] + "'";
                }
            }
            msg += " in " + type_name;
            if (missing.size() == 1) {
                diag_.report(Diagnostic(ErrorKind::TypeError, msg, cur_loc_)
                                 .with_suggestion("add clause for '" + missing[0] + "' pattern"));
            } else {
                std::string suggest = "add clauses for ";
                for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                    if (mi > 0)
                        suggest += ", ";
                    suggest += "'" + missing[mi] + "'";
                }
                diag_.report(
                    Diagnostic(ErrorKind::TypeError, msg, cur_loc_).with_suggestion(suggest));
            }
        }
    }

    TypeId body_type = reg_.void_type();
    if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
        body_type = synthesize_flat(flat, pool, v.child(1), flat.get(v.child(1)));
    ownership_env_.pop_scope();
    env_.pop_scope();
    return body_type;
}

TypeId InferenceEngine::synthesize_flat_begin(FlatAST& flat, StringPool& pool, NodeView v) {
    if (v.children.empty())
        return reg_.void_type();
    TypeId last = reg_.void_type();
    for (auto child_id : v.children)
        last = synthesize_flat(flat, pool, child_id, flat.get(child_id));
    return last;
}

TypeId InferenceEngine::synthesize_flat_annotation(FlatAST& flat, StringPool& pool, NodeView v) {
    // child(0) = inner_expr, sym_id = type name string
    if (v.children.empty())
        return reg_.dynamic_type();
    auto inner_id = v.child(0);
    TypeId inner_type = synthesize_flat(flat, pool, inner_id, flat.get(inner_id));

    auto type_name = pool.resolve(v.sym_id);
    if (!type_name.empty()) {
        // Issue #102: Type hole. The LLM-friendly way to say "infer
        // this" — `(check x :?)` or `(check x _)`. The type checker
        // does NOT look the type up, does NOT report "unknown type",
        // and does NOT constrain the inner expression. Whatever
        // type the inner expression synthesizes to is the type.
        // This is the gradual-typing analogue of a hole in Idris /
        // Agda — the goal is to make the AI's first try pass without
        // it having to commit to a type name.
        if (is_type_hole(type_name)) {
            return inner_type;
        }
        auto expected = reg_.lookup_type(std::string(type_name));
        if (!expected.valid()) {
            diag_.report(
                Diagnostic(ErrorKind::TypeError, "unknown type: " + std::string(type_name),
                           cur_loc_)
                    .with_blame(BlameInfo{BlameParty::Annotation,
                                          "(: ... " + std::string(type_name) + ")", "compile"}));
        } else {
            check_flat(flat, pool, inner_id, expected);
        }
    }
    return inner_type;
}

void InferenceEngine::check_flat(FlatAST& flat, StringPool& pool, NodeId id, TypeId expected) {
    if (id == NULL_NODE || id >= flat.size())
        return;
    if (cs_.metrics_) {
        static_cast<struct CompilerMetrics*>(cs_.metrics_)
            ->synthesize_check_switch_count_total.fetch_add(1, std::memory_order_relaxed);
    }
    auto v = flat.get(id);
    cur_loc_ = {v.line, v.col, 0};

    if (v.tag == NodeTag::Call)
        check_flat_call(flat, pool, v, expected);
    else if (v.tag == NodeTag::Lambda)
        check_flat_lambda(flat, pool, v, expected);
    else if (v.tag == NodeTag::IfExpr) {
        // If in check mode: check condition is Bool, check both branches
        // against expected, and unify them.
        // Issue #283: also run analyze_predicate_flat on the
        // condition so that the then-branch can see the
        // narrowed type. This makes check-mode produce the
        // same diagnostic precision as synthesize-mode for
        // Occurrence Typing predicates.
        if (v.children.size() < 2)
            return;
        auto cond_id = v.child(0);
        auto then_id = v.child(1);
        // Condition must be Bool
        auto cond_type = synthesize_flat(flat, pool, cond_id, flat.get(cond_id));
        cs_.consistent_unify(cond_type, reg_.bool_type());
        // Issue #283: extract Occurrence Typing narrowing on the
        // condition. Mirrors the synthesize_flat_if path
        // (line ~2493) but in check-mode we only APPLY the
        // narrowing; we don't recompute via synthesize. The
        // expected-type check below is what enforces the
        // contract; the narrowing just gives the checker more
        // information about the then-branch.
        //
        // Issue #283 follow-up #5: opt-out flag. When
        // bidirectional_mode_ is false, skip the narrowing
        // application and fall through to the uniform check
        // (preserves legacy behavior). Default true.
        if (!bidirectional_mode_) {
            // Opt-out: no narrowing application.
            check_flat(flat, pool, then_id, expected);
            if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
                check_flat(flat, pool, v.child(2), expected);
        } else {
            // Issue #627: shared predicate memo/epoch + narrow_evidence
            // (force re-analyze on dirty predicate nodes).
            const auto pred = resolve_if_predicate_occurrence(flat, pool, id, cond_id,
                                                              /*check_mode=*/true);
            auto occ = pred.occ;
            if (occ && !occ->is_negation) {
                // Then-branch: variable has refined type
                env_.push_scope();
                ownership_env_.push_scope();
                if (env_.is_bound(occ->var_name))
                    env_.bind(occ->var_name, occ->refined_type);
                // Linear ownership: narrowed bindings are Owned
                // (the predicate guarantees presence; use is
                // permitted). This mirrors the original-type
                // default of `Owned` for unknown vars.
                ownership_env_.mark(occ->var_name, OwnershipState::Owned);
                // Issue #283 follow-up #3: also capture provenance
                // in check-mode. This mirrors the synthesize_flat_if
                // capture so (query:provenance-of var) works
                // whether the workspace was last typechecked via
                // synthesize or check. The capture_epoch is the
                // current inference engine's epoch.
                {
                    std::string refined_str;
                    if (occ->refined_type.index != 0) {
                        auto n = reg_.name_of(occ->refined_type);
                        if (!n.empty())
                            refined_str = std::string(n);
                    }
                    std::string pred_src = "(...)";
                    if (cond_id < flat.size()) {
                        auto cn = flat.get(cond_id);
                        if (cn.tag == NodeTag::Call && !cn.children.empty()) {
                            auto fn = flat.get(cn.child(0));
                            if (fn.tag == NodeTag::Variable) {
                                pred_src = std::string(pool.resolve(fn.sym_id));
                                if (cn.children.size() >= 2) {
                                    auto arg = flat.get(cn.child(1));
                                    if (arg.tag == NodeTag::Variable)
                                        pred_src += " " + std::string(pool.resolve(arg.sym_id));
                                }
                            }
                        }
                    }
                    NarrowingRecord rec;
                    rec.var_name = occ->var_name;
                    rec.predicate_src = pred_src;
                    rec.refined_type_str = refined_str;
                    rec.if_node = id; // check_flat's id param
                    rec.cond_node = cond_id;
                    rec.is_negation = false;
                    rec.narrow_evidence = last_if_narrowing_;
                    rec.capture_epoch = cache_epoch_;
                    flat.record_narrowing(std::move(rec));
                }
                if (subtree_has_linear_ops(flat, then_id)) {
                    ownership_env_.mark_ownership_dirty(occ->var_name);
                    bump_linear_occurrence_predicate_safe(cs_.metrics_);
                }
                check_flat(flat, pool, then_id, expected);
                ownership_env_.pop_scope();
                env_.pop_scope();
            } else if (occ && occ->is_negation) {
                // Negation: else-branch gets the refinement.
                env_.push_scope();
                ownership_env_.push_scope();
                if (env_.is_bound(occ->var_name))
                    env_.bind(occ->var_name, occ->refined_type);
                ownership_env_.mark(occ->var_name, OwnershipState::Owned);
                // Issue #283 follow-up #3: capture provenance for
                // the negation case (else-branch gets refinement).
                {
                    std::string refined_str;
                    if (occ->refined_type.index != 0) {
                        auto n = reg_.name_of(occ->refined_type);
                        if (!n.empty())
                            refined_str = std::string(n);
                    }
                    NarrowingRecord rec;
                    rec.var_name = occ->var_name;
                    rec.predicate_src = "(not (...))";
                    rec.refined_type_str = refined_str;
                    rec.if_node = id;
                    rec.cond_node = cond_id;
                    rec.is_negation = true;
                    rec.narrow_evidence = 0;
                    rec.capture_epoch = cache_epoch_;
                    flat.record_narrowing(std::move(rec));
                }
                if (v.children.size() >= 3 && v.child(2) != NULL_NODE) {
                    if (subtree_has_linear_ops(flat, v.child(2))) {
                        ownership_env_.mark_ownership_dirty(occ->var_name);
                        bump_linear_occurrence_predicate_safe(cs_.metrics_);
                    }
                    check_flat(flat, pool, v.child(2), expected);
                }
                ownership_env_.pop_scope();
                env_.pop_scope();
                // Then-branch: no refinement
                check_flat(flat, pool, then_id, expected);
            } else {
                // No narrowing predicate — fall back to original
                // uniform check.
                check_flat(flat, pool, then_id, expected);
                if (v.children.size() >= 3 && v.child(2) != NULL_NODE)
                    check_flat(flat, pool, v.child(2), expected);
            }
        } // end bidirectional_mode_ opt-out
    } else if (v.tag == NodeTag::Let || v.tag == NodeTag::LetRec) {
        // Let in check mode: check value, then check body against expected
        bool is_rec = (v.tag == NodeTag::LetRec);
        auto name = pool.resolve(v.sym_id);
        std::string var_name(name);
        env_.push_scope();
        ownership_env_.push_scope();
        if (!v.children.empty() && v.child(0) != NULL_NODE) {
            auto val_id = v.child(0);
            // Check the value expression against expected if annotated
            // For now: synthesize val, bind it
            TypeId val_type =
                is_rec ? cs_.fresh_var() : synthesize_flat(flat, pool, val_id, flat.get(val_id));
            env_.bind(var_name, val_type);
        }
        if (v.children.size() >= 2 && v.child(1) != NULL_NODE)
            check_flat(flat, pool, v.child(1), expected);
        ownership_env_.pop_scope();
        env_.pop_scope();
    } else if (v.tag == NodeTag::Begin) {
        // Begin in check mode: check last expression against expected,
        // synthesize all others for side effects
        for (std::size_t i = 0; i < v.children.size(); ++i) {
            auto child_id = v.child(i);
            if (child_id == NULL_NODE)
                continue;
            if (i + 1 == v.children.size()) {
                check_flat(flat, pool, child_id, expected);
            } else {
                synthesize_flat(flat, pool, child_id, flat.get(child_id));
            }
        }
    } else if (v.tag == NodeTag::TypeAnnotation) {
        // Annotation in check mode: check inner against expected,
        // then check inner against annotation type
        if (v.children.empty())
            return;
        auto inner_id = v.child(0);
        auto type_name = pool.resolve(v.sym_id);
        if (!type_name.empty()) {
            auto ann_type = reg_.lookup_type(std::string(type_name));
            if (ann_type.valid()) {
                // Check inner against both annotation type and expected
                if (reg_.is_subtype(ann_type, expected) ||
                    cs_.consistent_unify(ann_type, expected)) {
                    // Annotation type is compatible with expected: check inner against ann_type
                    check_flat(flat, pool, inner_id, ann_type);
                } else {
                    // Annotation conflicts with expected context
                    auto msg = "annotation type " + std::string(reg_.format_type(ann_type)) +
                               " conflicts with context expecting " +
                               std::string(reg_.format_type(expected));
                    diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                                     .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"}));
                }
            } else {
                synthesize_flat(flat, pool, inner_id, flat.get(inner_id));
            }
        } else {
            synthesize_flat(flat, pool, inner_id, flat.get(inner_id));
        }
    } else if (v.tag == NodeTag::Set) {
        // (set! var value): synthesize the value, unify with var's type
        if (v.children.size() >= 1 && v.child(0) != NULL_NODE) {
            auto val_id = v.child(0);
            auto val_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));
            // Look up variable type from env
            auto var_name = std::string(pool.resolve(v.sym_id));
            auto var_type = env_.lookup(var_name);
            if (var_type.valid()) {
                cs_.consistent_unify(val_type, var_type);
            }
            // Also unify with expected context
            cs_.consistent_unify(val_type, expected);
        }
    } else if (v.tag == NodeTag::Define) {
        // (define name value): check value against expected if matched
        if (v.children.size() >= 1 && v.child(0) != NULL_NODE) {
            auto val_id = v.child(0);
            auto val_type = synthesize_flat(flat, pool, val_id, flat.get(val_id));
            // For define, check that value type is consistent with expected context
            // (define is a declaration, not an expression, so the expected context
            //  is about the defined value, not the define node itself)
        }
        // Define returns Void — no check against expected needed
    } else {
        TypeId inferred = synthesize_flat(flat, pool, id, v);
        if (!cs_.consistent_unify(inferred, expected)) {
            if (is_coercible(inferred, expected)) {
                auto msg = "coercion from " + std::string(reg_.format_type(inferred)) + " to " +
                           std::string(reg_.format_type(expected));
                diag_.report(Diagnostic(ErrorKind::Note, std::move(msg), cur_loc_));
                // ── Gradual Typing: deferred CoercionNode (Issue #116) ──
                auto type_tag = type_tag_for_coercion(expected, &reg_);
                auto src_v = flat.get(id);
                auto parent_id = flat.parent_of(id);
                if (parent_id != aura::ast::NULL_NODE) {
                    auto parent_v = flat.get(parent_id);
                    for (std::size_t ci = 0; ci < parent_v.children.size(); ++ci) {
                        if (parent_v.child(static_cast<std::uint32_t>(ci)) == id) {
                            add_deferred_coercion(flat, parent_id, static_cast<std::uint32_t>(ci),
                                                  id, type_tag, expected.index, src_v.line,
                                                  src_v.col);
                            break;
                        }
                    }
                } else {
                    // No parent (top-level expression). Record with
                    // parent_id = NULL_NODE; the apply pass will
                    // create the CoercionNode but won't rewrite
                    // any parent slot.
                    add_deferred_coercion(flat, aura::ast::NULL_NODE, 0, id, type_tag,
                                          expected.index, src_v.line, src_v.col);
                }
            } else {
                auto msg = "type mismatch: expected " + std::string(reg_.format_type(expected)) +
                           ", got " + std::string(reg_.format_type(inferred));
                diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                                 .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"}));
            }
        } else if (inferred == reg_.dynamic_type() && expected != reg_.dynamic_type()) {
            // Issue #691: skip coercion when env narrowing already matches expected.
            bool narrowed_satisfies = false;
            if (last_if_narrowing_ != 0 && v.tag == NodeTag::Variable) {
                auto var_ty = env_.lookup(std::string(pool.resolve(v.sym_id)));
                if (var_ty.valid()) {
                    auto norm = cs_.normalize(var_ty);
                    if (norm.index == expected.index || cs_.consistent_unify(norm, expected)) {
                        narrowed_satisfies = true;
                        if (cs_.metrics_) {
                            auto* m = static_cast<struct CompilerMetrics*>(cs_.metrics_);
                            m->coercion_post_narrow_elim_opportunities_total.fetch_add(
                                1, std::memory_order_relaxed);
                            m->coercion_cast_elim_from_narrow_total.fetch_add(
                                1, std::memory_order_relaxed);
                            m->dead_coercion_elision_evidence_hits_total.fetch_add(
                                1, std::memory_order_relaxed);
                            m->dead_coercion_elision_narrowing_stable_paths_total.fetch_add(
                                1, std::memory_order_relaxed);
                            if (last_predicate_cond_id_ != 0 && !flat.all_mutations().empty()) {
                                m->coercion_narrow_blame_chain_hits_total.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
            if (!narrowed_satisfies) {
                // Dynamic → Static boundary: consistent_unify succeeded because
                // DYNAMIC is consistent with everything, but we need a runtime
                // check at the boundary. Deferred CoercionNode (Issue #116).
                auto type_tag = type_tag_for_coercion(expected, &reg_);
                auto src_v = flat.get(id);
                auto parent_id = flat.parent_of(id);
                if (parent_id != aura::ast::NULL_NODE) {
                    auto parent_v = flat.get(parent_id);
                    for (std::size_t ci = 0; ci < parent_v.children.size(); ++ci) {
                        if (parent_v.child(static_cast<std::uint32_t>(ci)) == id) {
                            add_deferred_coercion(flat, parent_id, static_cast<std::uint32_t>(ci),
                                                  id, type_tag, expected.index, src_v.line,
                                                  src_v.col);
                            break;
                        }
                    }
                } else {
                    add_deferred_coercion(flat, aura::ast::NULL_NODE, 0, id, type_tag,
                                          expected.index, src_v.line, src_v.col);
                }
            }
        }
    }
}

void InferenceEngine::check_flat_call(FlatAST& flat, StringPool& pool, NodeView v,
                                      TypeId expected) {
    // Synthesize the call's type normally, then check against expected
    TypeId inferred = synthesize_flat_call(flat, pool, v);
    if (!cs_.consistent_unify(inferred, expected)) {
        if (is_coercible(inferred, expected)) {
            auto msg = "call return type: coercion from " +
                       std::string(reg_.format_type(inferred)) + " to " +
                       std::string(reg_.format_type(expected));
            // ── Gradual Typing: deferred CoercionNode (Issue #116) ──
            auto type_tag = type_tag_for_coercion(expected, &reg_);
            auto parent_id = flat.parent_of(v.id);
            if (parent_id != aura::ast::NULL_NODE) {
                auto parent_v = flat.get(parent_id);
                for (std::size_t ci = 0; ci < parent_v.children.size(); ++ci) {
                    if (parent_v.child(static_cast<std::uint32_t>(ci)) == v.id) {
                        add_deferred_coercion(flat, parent_id, static_cast<std::uint32_t>(ci), v.id,
                                              type_tag, expected.index, v.line, v.col);
                        break;
                    }
                }
            } else {
                add_deferred_coercion(flat, aura::ast::NULL_NODE, 0, v.id, type_tag, expected.index,
                                      v.line, v.col);
            }
        } else {
            auto msg = "call return type mismatch: expected " +
                       std::string(reg_.format_type(expected)) + ", got " +
                       std::string(reg_.format_type(inferred));
            diag_.report(Diagnostic(ErrorKind::TypeError, std::move(msg), cur_loc_)
                             .with_blame(BlameInfo{BlameParty::Caller, "", "compile"}));
        }
    }
}

void InferenceEngine::check_flat_lambda(FlatAST& flat, StringPool& pool, NodeView v,
                                        TypeId expected) {
    auto* f_ty = reg_.func_of(expected);
    if (!f_ty) {
        diag_.report(Diagnostic(ErrorKind::TypeError,
                                "expected a function type but got " +
                                    std::string(reg_.format_type(expected)),
                                cur_loc_)
                         .with_blame(BlameInfo{BlameParty::Annotation, "", "compile"})
                         .with_suggestion("the context annotation does not match a function; check "
                                          "the type annotation or the binding site"));
        return;
    }
    if (f_ty->args.size() != v.params.size()) {
        diag_.report(Diagnostic(ErrorKind::ArityMismatch,
                                "lambda expects " + std::to_string(v.params.size()) +
                                    " parameters but context provides " +
                                    std::to_string(f_ty->args.size()),
                                cur_loc_));
        return;
    }
    env_.push_scope();
    for (std::size_t i = 0; i < v.params.size(); ++i) {
        std::string pname(pool.resolve(v.params[i]));
        env_.bind(pname, f_ty->args[i]);
    }
    if (!v.children.empty() && v.child(0) != NULL_NODE)
        check_flat(flat, pool, v.child(0), f_ty->ret);
    env_.pop_scope();
}

// ═══════════════════════════════════════════════════════════
// TypeChecker — Public API
// ═══════════════════════════════════════════════════════════

void TypeChecker::inject_type_sigs(const std::unordered_map<std::string, std::string>& sigs,
                                   const std::unordered_map<std::string, std::string>& module_src) {
    auto lookup = [&](const std::string& name) -> TypeId {
        if (name == "Int")
            return types.int_type();
        if (name == "Bool")
            return types.bool_type();
        if (name == "String")
            return types.string_type();
        if (name == "Float")
            return types.lookup_type("Float");
        if (name == "Void")
            return types.void_type();
        if (name == "Any" || name == "Dyn")
            return types.dynamic_type();
        return types.dynamic_type();
    };
    for (auto& [name, sig] : sigs) {
        auto pipe = sig.find('|');
        if (pipe == std::string::npos)
            continue;
        std::vector<TypeId> param_types;
        std::istringstream iss(sig.substr(0, pipe));
        std::string tok;
        while (iss >> tok)
            param_types.push_back(lookup(tok));
        auto tid = types.register_func_named(std::move(param_types), lookup(sig.substr(pipe + 1)),
                                             "__decl_" + name);
        // Record the name → TypeId mapping so InferenceEngine can
        // bind each declared name to the env, even if multiple names
        // share the same TypeId post-interning. (See #70 follow-up
        // interning + #77 regression: 312-5 / test_aura_type_multi_func.)
        type_sigs_[name] = tid;
        auto mod_it = module_src.find(name);
        if (mod_it != module_src.end() && !mod_it->second.empty()) {
            type_module_src_[name] = mod_it->second;
        }
    }
}

std::string TypeChecker::declared_type_module(const std::string& name) const {
    auto it = type_module_src_.find(name);
    if (it != type_module_src_.end())
        return it->second;
    return "";
}


TypeId TypeChecker::infer_flat(FlatAST& flat, StringPool& pool, NodeId node,
                               DiagnosticCollector& diag) {
    // Issue #212 Phase 1d: route through the pure function.
    // The Wrap holds per-instance state (sigs, module_src,
    // strict, cache_epoch, stats accumulator) and passes them
    // to the pure function. The result struct bundles the
    // inferred type, deferred coercions, and per-call stats so
    // we don't need to call back into the engine.
    auto r = type_check_flat_pure(flat, pool, node, types, diag, type_sigs_, type_module_src_,
                                  strict_, cache_epoch_, metrics_, bidirectional_mode_);
    stats_.cache_hits += r.cache_hits;
    stats_.cache_misses += r.cache_misses;
    stats_.stale_cache += r.stale_cache;
    stats_.gen_saved += r.gen_saved;
    // Issue #386: aggregate narrowing counters.
    stats_.narrowing_applied += r.narrowing_applied;
    stats_.narrowing_skipped += r.narrowing_skipped;
    stats_.narrowing_reanalyzed += r.narrowing_reanalyzed;
    // Issue #338: aggregate and/or precision.
    stats_.and_or_meet_uses += r.and_or_meet_uses;
    stats_.and_or_join_uses += r.and_or_join_uses;
    // Issue #434: aggregate dirty recovery.
    stats_.narrowing_dirty_recovery += r.narrowing_dirty_recovery;
    // Issue #390: aggregate schema cache.
    stats_.schema_cache_lookups += r.schema_cache_lookups;
    stats_.schema_cache_hits += r.schema_cache_hits;
    last_coercions_ = std::move(r.coercions);
    return r.inferred_type;
}

// Issue #212 Phase 1d: pure-function entry point for type
// checking. Mirrors the pattern of constant_fold_function /
// compute_kind / check_arity — takes all dependencies as
// parameters, returns a result struct, no member state.
TypeCheckResult type_check_flat_pure(FlatAST& flat, StringPool& pool, NodeId root,
                                     TypeRegistry& types, DiagnosticCollector& diag,
                                     const std::unordered_map<std::string, TypeId>& sigs,
                                     const std::unordered_map<std::string, std::string>& module_src,
                                     bool strict, std::uint64_t cache_epoch,
                                     void* metrics, // Issue #258: optional metrics pointer
                                     bool bidirectional_mode) // Issue #283 f/u #5
{
    TypeCheckResult result;
    InferenceEngine engine(types, diag);
    engine.declared_modules_ = module_src;
    engine.declared_sigs_ = sigs;
    engine.set_strict(strict);           // Issue #79: plumb strict mode
    engine.set_cache_epoch(cache_epoch); // Issue #168
    // Issue #283 follow-up #5: plumb bidirectional_mode flag
    // from caller (default true to match post-#283 behavior).
    engine.set_bidirectional_mode(bidirectional_mode);
    // Issue #258: forward metrics so solve_delta timing
    // accumulates into CompilerMetrics::delta_solve_time_us.
    if (metrics)
        engine.set_metrics(metrics);
    engine.bind_declared_sigs();
    result.inferred_type = engine.infer_flat(flat, pool, root);
    // Issue #280: capture the most recent IfExpr's narrowing
    // evidence bitmask. The lowering pass reads this to set
    // `narrow_evidence` on the corresponding Branch instruction.
    result.narrow_evidence = engine.last_narrowing_evidence();
    // Capture per-call stats (Issue #72) for the result.
    auto es = engine.stats();
    result.cache_hits = es.cache_hits;
    result.cache_misses = es.cache_misses;
    result.stale_cache = es.stale_cache;
    // Issue #412: plumb the gen_saved counter to the result
    // so the caller (TypeChecker::infer_flat) can accumulate
    // it into the lifetime total.
    result.gen_saved = es.gen_saved;
    // Issue #116: capture the engine's deferred coercions so
    // the caller can apply them after type checking returns.
    // The engine is short-lived (per call) so we move-out here
    // to avoid an extra copy.
    result.coercions = engine.take_coercions();
    // Issue #281: predicate memo stats from this call.
    result.predicate_memo_hits = engine.predicate_memo_hits();
    result.predicate_memo_misses = engine.predicate_memo_misses();
    result.predicate_memo_evictions = engine.predicate_memo_evictions();
    // Issue #386: narrowing observability
    // (per-call engine stats). The full #386 scope
    // wires narrowing into the let/if paths, this
    // slice ships the observability foundation
    // (counters for the application paths the
    // engine took).
    result.narrowing_applied = es.narrowing_applied;
    result.narrowing_skipped = es.narrowing_skipped;
    result.narrowing_reanalyzed = es.narrowing_reanalyzed;
    // Issue #338: and/or precision.
    result.and_or_meet_uses = es.and_or_meet_uses;
    result.and_or_join_uses = es.and_or_join_uses;
    // Issue #434: dirty recovery.
    result.narrowing_dirty_recovery = es.narrowing_dirty_recovery;
    // Issue #390: schema cache.
    result.schema_cache_lookups = es.schema_cache_lookups;
    result.schema_cache_hits = es.schema_cache_hits;
    return result;
}

// Issue #148 Phase 4b: actual per-node re-inference. We
// spin up a per-call InferenceEngine (the existing pattern
// from TypeChecker::infer_flat), iterate the affected set,
// and call the existing per-node InferenceEngine::infer_flat
// for each. The per-node path ALREADY has the cache check
// at the top of synthesize_flat ("if (!flat.is_dirty(id))
// return cached") — so clean-but-re-inferred nodes hit the
// cache, and dirty nodes re-synthesize + re-solve.
//
// The speedup vs full infer_flat comes from:
//   - NOT re-inferring nodes outside the affected set
//     (full infer_flat re-synthesizes the whole tree; we
//     scope the re-synthesis to the affected set).
//   - The engine is per-call here (not persistent across
//     calls) so the CS doesn't carry forward state — that
//     would require a re-usable engine + add_delta/solve_delta
//     plumbing, which is a future optimization.
//
// Returns the number of nodes that were re-inferred
// (i.e. NOT just cache hits). The IncrementalStats
// Issue #688: collect linear binding names under affected nodes for
// post-mutate OwnershipEnv revalidate in infer_flat_partial.
static void collect_linear_bindings_under_nodes(const aura::ast::FlatAST& flat,
                                                const aura::ast::StringPool& pool,
                                                const std::vector<aura::ast::NodeId>& nodes,
                                                std::unordered_set<std::string>& out) {
    std::function<void(aura::ast::NodeId)> walk = [&](aura::ast::NodeId id) {
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            return;
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Let && v.sym_id != aura::ast::INVALID_SYM &&
            !v.children.empty() && v.child(0) != aura::ast::NULL_NODE &&
            flat.get(v.child(0)).tag == aura::ast::NodeTag::Linear) {
            auto name = std::string(pool.resolve(v.sym_id));
            if (!name.empty())
                out.insert(name);
        }
        for (auto c : v.children) {
            if (c != aura::ast::NULL_NODE)
                walk(c);
        }
    };
    for (auto id : nodes)
        walk(id);
}

// (cache_hits/cache_misses) is updated as a side effect
// on `stats_`.
std::size_t TypeChecker::infer_flat_partial(aura::ast::FlatAST& flat,
                                            const aura::ast::StringPool& pool,
                                            const aura::ast::MutationRecord& rec,
                                            aura::diag::DiagnosticCollector& diag) {
    return infer_flat_partial(flat, pool, rec, diag, /*per_defuse_index_tracker=*/nullptr);
}

std::size_t TypeChecker::infer_flat_partial(aura::ast::FlatAST& flat,
                                            const aura::ast::StringPool& pool,
                                            const aura::ast::MutationRecord& rec,
                                            aura::diag::DiagnosticCollector& diag,
                                            void* per_defuse_index_tracker) {
    // Issue #411 follow-up #1: per-symbol re-inference
    // wiring. The baseline (ancestor-walk) used
    // `affected_subtree_from_mutation` which marks every
    // ancestor of the mutated node as affected — for a
    // mutation deep in the AST this re-infers O(depth)
    // nodes that didn't actually need re-inference. The
    // per-symbol path (`affected_subtree_for_symbol`)
    // returns only the Variable use-sites of the binding
    // the mutation changed, which is O(uses) and is
    // almost always a strict subset of the ancestor set.
    //
    // Issue #411 fu1 follow-up #3: per-DefUseIndex path
    // (the actual O(uses) optimization). When the caller
    // passes a non-null `per_defuse_index_tracker`
    // pointer AND the tracker has the sym registered,
    // the affected set comes straight from the tracker
    // (O(uses) hash lookup) instead of the O(n) walk
    // in `affected_subtree_for_symbol`. Falls back to
    // the O(n) walk when the tracker is null or the sym
    // isn't tracked (e.g. tracker not yet populated for
    // this binding, or the sym changed since the last
    // tracker build).
    //
    // The three paths compose: per-DefUseIndex (fastest,
    // O(uses)) → per-symbol walk (O(n)) → ancestor walk
    // (O(depth)). The metrics plumbed back
    // (per_defuse_index_used_total,
    // per_defuse_index_visited_total,
    // per_symbol_used_total, per_symbol_visited_total,
    // ancestor_used_total, ancestor_visited_total) let
    // the user measure the share of work that went
    // through each path via the
    // (compile:per-symbol-reinfer-stats) Aura primitive.
    std::vector<aura::ast::NodeId> affected;
    bool per_defuse_index_used = false;
    bool per_symbol_used = false;
    aura::ast::SymId sym_for_lookup = aura::ast::INVALID_SYM;
    // Extract the binding sym_id (if applicable) once,
    // up-front. Same logic as the pre-#411-follow-up-1
    // path.
    if (rec.target_node != aura::ast::NULL_NODE && rec.target_node < flat.size()) {
        auto tgv = flat.get(rec.target_node);
        if (tgv.sym_id != aura::ast::INVALID_SYM &&
            (tgv.tag == aura::ast::NodeTag::Define || tgv.tag == aura::ast::NodeTag::Let ||
             tgv.tag == aura::ast::NodeTag::LetRec)) {
            sym_for_lookup = tgv.sym_id;
        }
    }
    // 1) per-DefUseIndex path (fastest). Only fires when
    // the caller passed a tracker AND the sym is
    // registered. The O(uses) lookup returns the
    // use-sites directly without scanning the flat.
    if (per_defuse_index_tracker && sym_for_lookup != aura::ast::INVALID_SYM) {
        // The PerDefUseIndexTracker stores callers as
        // Caller structs (location strings), not as
        // NodeIds — the tracker is a registration of
        // "this caller depends on this binding", not a
        // pre-resolved NodeId set. For the affected-set
        // metrics we just need the count, which is the
        // size_for_index (O(1) hash lookup). The actual
        // re-inference iterates over a separate
        // node-id affected set (computed below from
        // the O(n) walk if the per-DefUseIndex path
        // didn't yield one), so the per-DefUseIndex
        // path's contribution is purely the
        // visited-count + speedup signal.
        //
        // The O(n) walk in affected_subtree_for_symbol
        // is the canonical "use-sites" computation
        // (variable nodes whose sym_id matches). The
        // per-DefUseIndex tracker tracks the same
        // population, so its size_for_index should
        // match the O(n) walk's count. We use the
        // tracker count for the metric (cheap) and
        // still run the O(n) walk to get the actual
        // NodeIds (for the inference loop).
        const auto* tracker =
            static_cast<const per_defuse_index::PerDefUseIndexTracker*>(per_defuse_index_tracker);
        // Look up the tracker entry by sym's lexical
        // name. Note: the tracker keys on string
        // names, not SymId. The caller is expected to
        // have registered entries by the same name
        // string that the binding uses. If the lookup
        // misses, fall through to the O(n) walk.
        // We use the sym's name from the flat's
        // string pool.
        std::string sym_name;
        if (sym_for_lookup != aura::ast::INVALID_SYM) {
            // The FlatAST doesn't expose the string
            // pool directly; service.ixx populates
            // per_defuse_index_tracker_ with names
            // (see compile:per-defuse-index-add). The
            // tracker uses std::string for its key.
            // We don't have a direct sym → name API
            // here, so we use a heuristic: the
            // tracker may have the entry under any
            // name. For scope-limited #411 fu1
            // follow-up #3, the per-DefUseIndex path
            // just BUMPS the metric when the tracker
            // is non-null; the actual NodeId set
            // comes from the O(n) walk below. This
            // keeps the path metrics honest (the
            // O(uses) optimization is the next
            // follow-up commit which will store
            // NodeIds directly in the tracker).
            //
            // For now, we conservatively bump
            // per_defuse_index_used_total only if
            // tracker.index_count() > 0 (signals that
            // SOME syms are tracked). The O(uses)
            // wins in scope-limited #411 fu1 fu1
            // are: (a) avoiding the Variable sym_id
            // comparison per node when tracker
            // says "no use-sites for this sym" (we
            // still walk but count the O(n) cost in
            // per_symbol_visited), (b) the metric
            // signals to the user that the path
            // fired.
            if (tracker->index_count() > 0) {
                per_defuse_index_used = true;
            }
        }
    }
    if (per_defuse_index_used) {
        ++stats_.per_defuse_index_used_total;
        // Issue #411 fu1 fu4: the actual O(uses) win.
        // The tracker now stores NodeIds directly, so we
        // can iterate the use-sites without paying the
        // O(n) `affected_subtree_for_symbol` walk cost.
        // Get the sym's name from the StringPool, look
        // up the tracker entry, and use the stored
        // NodeIds as the affected set. If the sym isn't
        // in the tracker (the `index_count() > 0` check
        // was a coarse gate — the tracker might be
        // populated for OTHER syms), fall through to the
        // O(n) walk (bump walk_fallback).
        if (sym_for_lookup != aura::ast::INVALID_SYM) {
            const auto* tracker = static_cast<const per_defuse_index::PerDefUseIndexTracker*>(
                per_defuse_index_tracker);
            const std::string sym_name(pool.resolve(sym_for_lookup));
            const auto& tracker_callers =
                tracker->get_callers(per_defuse_index::DefUseIndex{sym_name});
            if (!tracker_callers.empty()) {
                // O(uses) — directly iterate the tracker
                // entries. Bump visited_total with the
                // actual O(uses) count.
                affected.reserve(tracker_callers.size());
                for (const auto& c : tracker_callers)
                    affected.push_back(c.node_id);
                stats_.per_defuse_index_visited_total += affected.size();
            } else {
                // Tracker doesn't have this sym — fall
                // through to O(n) walk (bump walk_fallback).
                affected = affected_subtree_for_symbol(flat, sym_for_lookup);
                ++stats_.per_defuse_index_walk_fallback_total;
                stats_.per_symbol_visited_total += affected.size();
            }
        }
    } else {
        // 2) per-symbol path (existing O(n) walk, from
        // #411 fu1 follow-up #1).
        if (sym_for_lookup != aura::ast::INVALID_SYM) {
            affected = affected_subtree_for_symbol(flat, sym_for_lookup);
            ++stats_.per_symbol_used_total;
            stats_.per_symbol_visited_total += affected.size();
            per_symbol_used = true;
        }
    }
    if (affected.empty()) {
        // 3) ancestor walk fallback (pre-#411-follow-up-1
        // path). Used when the mutation record doesn't
        // carry a binding sym_id (sub-expression mutation
        // like replace-type) or when the per-symbol path
        // returns empty (no use-sites of the changed
        // binding in the workspace).
        affected = affected_subtree_from_mutation(flat, rec);
        // Issue #487: bump the affected_subtree counter
        // for observability (the dirty propagation
        // path fired). Distinct from should_relower
        // (the IR re-lower decision — happens
        // downstream of the affected set).
        if (metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(metrics_);
            m->affected_subtree_total.fetch_add(1, std::memory_order_relaxed);
        }
        ++stats_.ancestor_used_total;
        stats_.ancestor_visited_total += affected.size();
        // Bump the per-DefUseIndex walk-fallback metric
        // (signals the index didn't have the sym — the
        // user can use this to track index coverage).
        if (per_defuse_index_tracker)
            ++stats_.per_defuse_index_walk_fallback_total;
    }
    if (affected.empty()) {
        // No affected nodes — the mutation was a no-op (or
        // target_node + parent_id both null). Count as a
        // full cache hit (no re-inference needed).
        return 0;
    }

    // Issue #518 P0 Phase 1: collect dirty if-contexts from the
    // affected set plus the mutation target subtree (rebind
    // auto-wires kOccurrenceDirty on if-nodes in the new body
    // even when the per-symbol affected set is only use-sites).
    std::vector<NodeId> occurrence_targets;
    std::unordered_set<NodeId> occurrence_seen;
    occurrence_targets.reserve(affected.size());
    for (auto id : affected)
        collect_occurrence_dirty_if_exprs_in_subtree(flat, id, occurrence_targets, occurrence_seen);
    if (rec.target_node != NULL_NODE && rec.target_node < flat.size())
        collect_occurrence_dirty_if_exprs_in_subtree(flat, rec.target_node, occurrence_targets,
                                                     occurrence_seen);
    if (rec.parent_id != NULL_NODE && rec.parent_id < flat.size())
        collect_occurrence_dirty_if_exprs_in_subtree(flat, rec.parent_id, occurrence_targets,
                                                     occurrence_seen);
    // Issue #689: deep and/or/not predicates in mutation-affected subtrees.
    for (auto id : affected)
        collect_deep_predicate_if_exprs_in_subtree(flat, pool, id, occurrence_targets,
                                                   occurrence_seen);
    if (rec.target_node != NULL_NODE && rec.target_node < flat.size())
        collect_deep_predicate_if_exprs_in_subtree(flat, pool, rec.target_node, occurrence_targets,
                                                   occurrence_seen);
    {
        const std::uint8_t kOccurrenceBit =
            static_cast<std::uint8_t>(FlatAST::DirtyReason::kOccurrenceDirty);
        for (auto id : occurrence_targets) {
            flat.mark_dirty(id, kOccurrenceBit);
            flat.mark_occurrence_stale(id);
        }
    }

    // Issue #747: linear bindings in occurrence-dirty if-contexts → ownership_dirty.
    OwnershipEnv occurrence_linear_dirty;
    for (auto if_id : occurrence_targets) {
        if (!subtree_has_linear_ops(flat, if_id))
            continue;
        std::unordered_set<std::string> if_linear;
        collect_linear_bindings_under_nodes(flat, pool, {if_id}, if_linear);
        for (const auto& name : if_linear)
            occurrence_linear_dirty.mark_ownership_dirty(name);
        if (metrics_)
            bump_linear_occurrence_predicate_safe(metrics_);
    }

    // Spin up a per-call engine. Same pattern as the existing
    // TypeChecker::infer_flat — short-lived engine per
    // re-inference pass.
    InferenceEngine engine(types, diag);
    engine.declared_modules_ = type_module_src_;
    engine.declared_sigs_ = type_sigs_;
    engine.set_strict(strict_);                         // Issue #79: plumb strict mode
    engine.set_cache_epoch(cache_epoch_);               // Issue #168
    engine.set_metrics(metrics_);                       // Issue #537: provenance refresh metrics
    engine.set_bidirectional_mode(bidirectional_mode_); // Issue #627
    engine.bind_declared_sigs();
    engine.set_narrowing_observability_hooks(on_narrowing_refresh_, on_selective_recheck_);
    engine.set_solve_delta_observability_hooks(on_touched_roots_snapshot_,
                                               on_cross_delta_conflict_);
    engine.set_active_mutation_id(rec.mutation_id);
    if (on_touched_roots_snapshot_)
        on_touched_roots_snapshot_(engine.constraint_touched_roots_size());

    // Issue #518: re-narrow dirty if-contexts before the infer
    // loop, then propagate narrowed-variable use-sites into the
    // affected set.
    last_occurrence_refresh_count_ = engine.reanalyze_occurrence_contexts(
        flat, const_cast<StringPool&>(pool), occurrence_targets);
    engine.propagate_narrowing_to_uses(flat, const_cast<StringPool&>(pool), affected);
    engine.seed_mutation_touched_roots(flat, pool, occurrence_targets, rec.mutation_id);
    if (on_touched_roots_snapshot_)
        on_touched_roots_snapshot_(engine.constraint_touched_roots_size());

    std::size_t re_inferred = 0;
    std::uint32_t max_narrow_evidence = 0;
    for (std::size_t ai = 0; ai < affected.size(); ++ai) {
        const auto id = affected[ai];
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            continue;
        // Issue #656: dedupe. If `id` has an ancestor in the affected
        // set, the ancestor's recursive walk will re-check `id` with
        // the correct env_/scope context. Without this dedupe, every
        // descendant node is processed as a SEPARATE engine.infer_flat
        // call, and the env_ (which is popped back to empty after each
        // Lambda scope exits) doesn't have the new Lambda's params
        // bound — so Variables in the new body get reported as
        // unbound. Symptom: test_issue_166 Test 3 fails on
        // `(mutate:set-body "g" "(lambda (x) (+ x 100))")` because
        // the selective recheck visits the new Lambda's body Variable
        // `x` without x being in env_. Fix: skip `id` if any ancestor
        // is also in the affected set.
        bool ancestor_in_affected = false;
        for (std::size_t bi = 0; bi < affected.size(); ++bi) {
            if (bi == ai)
                continue;
            auto other = affected[bi];
            // Walk up from `id` toward root and check if any step
            // is `other` (also in the affected set). Bounded by
            // flat.size() for the (impossible-but-safe) cycle case.
            NodeId cur = flat.parent_of(id);
            std::size_t safety = 0;
            while (cur != aura::ast::NULL_NODE && cur < flat.size() && safety++ < flat.size()) {
                if (cur == other) {
                    ancestor_in_affected = true;
                    break;
                }
                cur = flat.parent_of(cur);
            }
            if (ancestor_in_affected)
                break;
        }
        if (ancestor_in_affected) {
            continue; // ancestor's recursive walk will cover us
        }
        // Issue #466: preserve ConstraintSystem across affected
        // nodes; first node full-solves + mark_clean, subsequent
        // nodes add_delta + solve_delta with touched-root reverify.
        engine.set_incremental_delta_mode(true, ai > 0);
        auto type = engine.infer_flat(flat, const_cast<aura::ast::StringPool&>(pool), id,
                                      /*preserve_cs=*/ai > 0);
        max_narrow_evidence = std::max(max_narrow_evidence, engine.last_narrowing_evidence());
        if (type != types.dynamic_type()) {
            ++re_inferred;
        }
    }
    if (on_touched_roots_snapshot_)
        on_touched_roots_snapshot_(engine.constraint_touched_roots_size());

    // Accumulate per-call engine stats into TypeChecker stats.
    auto es = engine.stats();
    stats_.cache_hits += es.cache_hits;
    stats_.cache_misses += es.cache_misses;
    stats_.stale_cache += es.stale_cache;
    stats_.gen_saved += es.gen_saved;
    // Issue #386: aggregate narrowing counters.
    stats_.narrowing_applied += es.narrowing_applied;
    stats_.narrowing_skipped += es.narrowing_skipped;
    stats_.narrowing_reanalyzed += es.narrowing_reanalyzed;
    // Issue #338: aggregate and/or precision.
    stats_.and_or_meet_uses += es.and_or_meet_uses;
    stats_.and_or_join_uses += es.and_or_join_uses;
    // Issue #434: aggregate dirty recovery.
    stats_.narrowing_dirty_recovery += es.narrowing_dirty_recovery;
    // Issue #390: aggregate schema cache.
    stats_.schema_cache_lookups += es.schema_cache_lookups;
    stats_.schema_cache_hits += es.schema_cache_hits;
    // Issue #411 follow-up #1: per_symbol / ancestor path
    // tracking already bumped on this infer_flat_partial
    // call (at the top of the function). The per-call
    // engine doesn't carry the per_symbol / ancestor
    // counts; we accumulate them on stats_ directly here.

    // Issue #627: capture narrowing evidence for post-mutate
    // lowering freshness + consistency observability.
    last_partial_narrowing_evidence_ = max_narrow_evidence;
    if (last_occurrence_refresh_count_ > 0 && last_partial_narrowing_evidence_ != 0 && metrics_) {
        static_cast<struct CompilerMetrics*>(metrics_)
            ->post_mutate_narrow_consistency_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #688: automatic OwnershipEnv revalidate after partial infer
    // when the affected subtree contains linear bindings.
    if (flat.root != aura::ast::NULL_NODE && flat.root < flat.size()) {
        std::unordered_set<std::string> linear_bindings;
        collect_linear_bindings_under_nodes(flat, pool, affected, linear_bindings);
        if (rec.target_node != aura::ast::NULL_NODE && rec.target_node < flat.size()) {
            collect_linear_bindings_under_nodes(flat, pool, {rec.target_node}, linear_bindings);
        }
        if (rec.parent_id != aura::ast::NULL_NODE && rec.parent_id < flat.size()) {
            collect_linear_bindings_under_nodes(flat, pool, {rec.parent_id}, linear_bindings);
        }
        if (sym_for_lookup != aura::ast::INVALID_SYM) {
            const auto sym_name = std::string(pool.resolve(sym_for_lookup));
            if (!sym_name.empty())
                linear_bindings.insert(sym_name);
        }
        for (const auto& name : occurrence_linear_dirty.ownership_dirty_bindings())
            linear_bindings.insert(name);
        if (!linear_bindings.empty()) {
            std::vector<OwnershipNote> ownership_notes;
            const bool ownership_pass = OwnershipEnv::validate_ownership(
                flat, pool, flat.root, linear_bindings, ownership_notes);
            record_linear_ownership_mutation_metrics(metrics_, true, ownership_notes,
                                                     ownership_pass);
            if (metrics_) {
                static_cast<struct CompilerMetrics*>(metrics_)
                    ->linear_dirty_revalidate_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Issue #692: ADT DefineType + match exhaustiveness incremental
    // re-validation + pattern NarrowingRecord provenance refresh.
    {
        std::vector<NodeId> adt_roots;
        adt_roots.reserve(affected.size() + 2);
        for (auto id : affected)
            adt_roots.push_back(id);
        if (rec.target_node != NULL_NODE && rec.target_node < flat.size())
            adt_roots.push_back(rec.target_node);
        if (rec.parent_id != NULL_NODE && rec.parent_id < flat.size())
            adt_roots.push_back(rec.parent_id);
        revalidate_adt_typed_mutation_scope(flat, pool, types, adt_roots, rec, cache_epoch_,
                                            metrics_);
    }

    return re_inferred;
}

// ── Ownership Validation ────────────────────────────────────────
//
// Walks the AST post-mutation, re-simulates ownership flow for bindings
// in the dirty set, and reports violations. This detects:
//   - Use-after-move: a variable used after its value has been moved
//   - Double-borrow: mutable borrow after existing borrow
//   - Leaked linear resource: linear-typed binding not moved/dropped
//   - Invalid ownership state: any other inconsistency
//
// The validation uses a mimimal OwnershipEnv that only tracks bindings
// in the supplied set. Two callers:
//   - validate_ownership: post-mutation path, the set is the
//     dirty_bindings (bindings that just changed). Clean
//     bindings are assumed correct (validated at the last
//     full type-check pass).
//   - validate_ownership_full: full re-simulation path, the
//     set is all linear-typed bindings discovered by walking
//     the AST. Slower but catches cross-function / closure /
//     global-scope cases.
//
bool OwnershipEnv::validate_ownership(const FlatAST& flat, const StringPool& pool, NodeId root,
                                      const std::unordered_set<std::string>& dirty_bindings,
                                      std::vector<OwnershipNote>& notes_out) {
    if (dirty_bindings.empty())
        return true;
    return validate_ownership_impl(flat, pool, root, dirty_bindings, notes_out);
}

// Issue #117: full re-simulation mode. Walks the AST to
// discover ALL linear-typed bindings (not just dirty ones)
// and validates them as a single pass. Catches cross-function
// / closure / global-scope ownership flows that the dirty
// path misses.
//
// Linear bindings are discovered by walking the AST: a
// let-introduced binding is linear if the let value is a
// `(Linear ...)` node (syntactic check). For type-driven
// discovery (caller knows the registry), pass the
// pre-computed set via the dirty-only `validate_ownership`
// instead — that's the more precise path.
bool OwnershipEnv::validate_ownership_full(const FlatAST& flat, const StringPool& pool, NodeId root,
                                           std::vector<OwnershipNote>& notes_out) {
    std::unordered_set<std::string> linear_bindings;
    std::function<void(NodeId)> discover = [&](NodeId id) {
        if (id == NULL_NODE || id >= flat.size())
            return;
        auto v = flat.get(id);
        // Pattern 1: (let ((x (Linear e))) ...) — value is a
        // Linear wrapper node. x is a linear binding.
        if (v.tag == NodeTag::Let && v.sym_id != INVALID_SYM) {
            if (!v.children.empty() && v.child(0) != NULL_NODE &&
                flat.get(v.child(0)).tag == NodeTag::Linear) {
                auto name = std::string(pool.resolve(v.sym_id));
                if (!name.empty())
                    linear_bindings.insert(name);
            }
        }
        // Recurse.
        for (auto c : v.children)
            discover(c);
    };
    discover(root);

    if (linear_bindings.empty())
        return true;
    return validate_ownership_impl(flat, pool, root, linear_bindings, notes_out);
}

// Shared implementation of the post-hoc ownership walk.
// The original `validate_ownership` and the new
// `validate_ownership_full` both call this; the difference is
// just which set of bindings to track (dirty-only vs. all
// linear-typed).
//
// `dirty_bindings` here is a misnomer inherited from the
// historical API; in the full-re-simulation path it's really
// "all linear-typed bindings", and in the post-mutation path
// it's the dirty set. The walk is identical.
bool OwnershipEnv::validate_ownership_impl(const FlatAST& flat, const StringPool& pool, NodeId root,
                                           const std::unordered_set<std::string>& dirty_bindings,
                                           std::vector<OwnershipNote>& notes_out) {
    // Build a temporary ownership environment from scratch, seeded with
    // Owned for all dirty bindings. This gives us a clean starting point
    // to detect violations.
    OwnershipEnv tmp_env;
    for (auto& name : dirty_bindings) {
        tmp_env.mark(name, OwnershipState::Owned);
    }

    // Issue #74: scope-aware tracking. The previous implementation
    // walked ops in a flat list, ignoring scope nesting. As a result,
    // a linear resource declared in a let body that was never moved
    // before the let ended was silently passed (the final-Owned
    // check was a no-op comment). We now maintain a scope stack so
    // that on scope exit, we can detect linear bindings that ended
    // in Owned state (i.e., never moved or dropped) and report them
    // as leaked-linear.
    //
    // Scope structure:
    //   - Each Let (body) introduces a new scope
    //   - Each Lambda body is a new scope
    //   - Each If then/else branch is a new scope
    //   - The whole Begin is one scope (per-expression scopes would
    //     be too granular for typical Lisp code)
    struct ScopeInfo {
        NodeId exit_node = NULL_NODE;               // node that ends this scope
        std::unordered_set<std::string> introduced; // bindings declared here
    };
    std::vector<ScopeInfo> scope_stack;
    scope_stack.push_back({root, {}}); // root scope ends at root

    bool all_pass = true;

    // Helper: process an op and update state.
    auto apply_op = [&](NodeId op_node, NodeTag op_type, const std::string& target_var) -> void {
        switch (op_type) {
            case NodeTag::Move:
                if (!tmp_env.can_move(target_var)) {
                    auto st = tmp_env.get(target_var);
                    notes_out.push_back(
                        {op_node, "use-after-move: " + target_var + " is " + tmp_env.state_name(st),
                         "use-after-move"});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::Moved);
                break;
            case NodeTag::Borrow:
                if (!tmp_env.can_borrow(target_var)) {
                    auto st = tmp_env.get(target_var);
                    std::string kind;
                    if (st == OwnershipState::MutBorrowed)
                        kind = "double-borrow";
                    else
                        kind = "invalid-state";
                    notes_out.push_back({op_node,
                                         "immutable borrow of " + target_var +
                                             " denied — current state: " + tmp_env.state_name(st),
                                         kind});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::Borrowed);
                break;
            case NodeTag::MutBorrow:
                if (!tmp_env.can_mut_borrow(target_var)) {
                    auto st = tmp_env.get(target_var);
                    std::string kind;
                    if (st == OwnershipState::Borrowed || st == OwnershipState::MutBorrowed)
                        kind = "double-borrow";
                    else
                        kind = "invalid-state";
                    notes_out.push_back({op_node,
                                         "mutable borrow of " + target_var +
                                             " denied — current state: " + tmp_env.state_name(st),
                                         kind});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::MutBorrowed);
                break;
            case NodeTag::Drop:
                if (!tmp_env.can_drop(target_var)) {
                    auto st = tmp_env.get(target_var);
                    notes_out.push_back(
                        {op_node, "cannot drop " + target_var + " — " + tmp_env.state_name(st),
                         "leaked-linear"});
                    all_pass = false;
                }
                tmp_env.mark(target_var, OwnershipState::Moved);
                break;
            default:
                break;
        }
    };

    // Helper: pop a scope and check for leaks.
    auto pop_scope = [&]() -> void {
        if (scope_stack.size() <= 1)
            return; // never pop root
        auto info = scope_stack.back();
        scope_stack.pop_back();
        for (auto& name : info.introduced) {
            // Only check linear bindings that are in the dirty set
            // (other bindings are not under ownership simulation).
            if (dirty_bindings.count(name) == 0)
                continue;
            auto st = tmp_env.get(name);
            if (st == OwnershipState::Owned) {
                // Linear resource declared in this scope was never
                // moved or dropped. That's a leak.
                notes_out.push_back({info.exit_node,
                                     "leaked linear resource: " + name +
                                         " (never moved or dropped at end of scope)",
                                     "leaked-linear"});
                all_pass = false;
            }
        }
    };

    // Recursive walker.
    auto walk = [&](this const auto& self, NodeId id) -> void {
        if (id >= flat.size())
            return;
        auto v = flat.get(id);

        // Process op nodes (Move/Borrow/MutBorrow/Drop).
        if (v.tag == NodeTag::Move || v.tag == NodeTag::Borrow || v.tag == NodeTag::MutBorrow ||
            v.tag == NodeTag::Drop) {
            if (!v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    if (dirty_bindings.count(var_name)) {
                        apply_op(id, v.tag, var_name);
                    }
                }
            }
        }

        // Handle scope-introducing nodes.
        if (v.tag == NodeTag::Let) {
            // add_let layout: children = [val, body]; name in sym_id_
            // Standard ML/Scheme semantics: the bound name is introduced
            // in the let BODY's scope (not the outer). The value is
            // evaluated in the outer scope. The body scope pops when the
            // let ends, and any linear bindings still Owned at that point
            // are reported as leaked.
            std::string name;
            if (v.sym_id != INVALID_SYM) {
                name = std::string(pool.resolve(v.sym_id));
            }
            // Process value (child 0) in current scope.
            if (v.children.size() >= 1 && v.child(0) != NULL_NODE)
                self(v.child(0));
            // Push new scope for the body, with x as introduced.
            if (v.children.size() >= 2 && v.child(1) != NULL_NODE) {
                ScopeInfo body_scope;
                body_scope.exit_node = id;
                if (!name.empty()) {
                    body_scope.introduced.insert(name);
                }
                scope_stack.push_back(std::move(body_scope));
                self(v.child(1));
                pop_scope();
            }
            return;
        }
        if (v.tag == NodeTag::Lambda) {
            // (lambda (params...) body) — children: [0..n-1]=params, [n]=body
            // Params and body are in a new scope.
            scope_stack.push_back({id, {}});
            // Add all param names to the new scope's introduced set.
            for (std::size_t i = 0; i + 1 < v.children.size(); ++i) {
                auto param = flat.get(v.child(i));
                if (param.tag == NodeTag::Variable && param.sym_id != INVALID_SYM) {
                    auto name = std::string(pool.resolve(param.sym_id));
                    scope_stack.back().introduced.insert(name);
                }
            }
            // Process body.
            if (!v.children.empty()) {
                auto last = v.children.back();
                if (last != NULL_NODE)
                    self(last);
            }
            pop_scope();
            return;
        }
        if (v.tag == NodeTag::IfExpr) {
            // (if cond then else) — children: [0]=cond, [1]=then, [2]=else (optional)
            // cond is in current scope; then/else are in new scopes.
            if (!v.children.empty() && v.child(0) != NULL_NODE)
                self(v.child(0));
            if (v.children.size() >= 2 && v.child(1) != NULL_NODE) {
                scope_stack.push_back({id, {}});
                self(v.child(1));
                pop_scope();
            }
            if (v.children.size() >= 3 && v.child(2) != NULL_NODE) {
                scope_stack.push_back({id, {}});
                self(v.child(2));
                pop_scope();
            }
            return;
        }

        // Default: recurse into children in current scope.
        for (auto c : v.children) {
            if (c != NULL_NODE)
                self(c);
        }
    };
    walk(root);

    return all_pass;
}

// ═══════════════════════════════════════════════════════════
// Issue #147: post-mutation invariant check.
//
// A typed mutation (mutate:replace-type, mutate:wrap, ...) changes a
// subtree in the workspace FlatAST. Downstream, two soundness risks
// need re-validation:
//
//   1. Linear ownership: a binding x introduced as (let ((x (Linear e))) ...)
//      is in Owned state at introduction. If the mutation reorders
//      or removes the use of x, the linear state may now be wrong
//      (leaked-linear or use-after-move). validate_ownership is the
//      single source of truth for that walk.
//
//   2. Occurrence narrowing: an (if (number? x) ...else...) condition
//      produces a refined type for x in the then-branch. The refinement
//      was derived from the pre-mutation type. If x was just mutated
//      to a different type, the refinement is now stale — code in the
//      then-branch is typechecked under a claim that no longer holds.
//
// Phase 2 implements the dirty-subtree walk and emits OwnershipNotes
// for both classes. Phase 3 (typed_mutate integration) decides whether
// to block execution based on InvariantCheckMode.
//

namespace {

    // Collect all descendant NodeIds rooted at `id` (including `id` itself).
    // Walks via FlatAST::get(id).children. O(size of subtree).
    static void collect_descendants(const FlatAST& flat, NodeId id, std::vector<NodeId>& out) {
        if (id == NULL_NODE || id >= flat.size())
            return;
        out.push_back(id);
        auto v = flat.get(id);
        for (auto c : v.children) {
            if (c != NULL_NODE)
                collect_descendants(flat, c, out);
        }
    }

    // Walk `nodes` and collect names of any (let ((x (Linear e))) ...)
    // binding discovered. These are the bindings whose linear state may
    // have been altered by the mutation, so validate_ownership must
    // re-check them.
    static void discover_linear_bindings(const FlatAST& flat, const StringPool& pool,
                                         const std::vector<NodeId>& nodes,
                                         std::unordered_set<std::string>& out) {
        for (auto id : nodes) {
            if (id == NULL_NODE || id >= flat.size())
                continue;
            auto v = flat.get(id);
            if (v.tag != NodeTag::Let || v.sym_id == INVALID_SYM)
                continue;
            if (v.children.empty() || v.child(0) == NULL_NODE)
                continue;
            if (flat.get(v.child(0)).tag != NodeTag::Linear)
                continue;
            auto name = std::string(pool.resolve(v.sym_id));
            if (!name.empty())
                out.insert(name);
        }
    }

    // Walk `nodes` looking for IfExpr expressions whose predicate yields
    // a non-empty occurrence narrowing. For each such occurrence context,
    // emit an OwnershipNote tagged "invalidated-occurrence-narrowing" so
    // the caller can warn or block under Strict mode. We do NOT attempt
    // to re-evaluate the predicate's truth value — that would require
    // running the program. Instead we flag the *context* as suspect,
    // which is the sound conservative signal.
    //
    // Note: FlatAST expresses match via let + constructor patterns +
    // MatchInfo metadata (no NodeTag::Match). Occurrence narrowing on
    // pattern bindings is exercised at typecheck time, so a post-
    // mutation re-typecheck would catch it. For Phase 2 we only flag
    // IfExpr predicates; a full re-typecheck integration can be added
    // later if the dirty pattern-bindings case shows up empirically.
    static void find_occurrence_contexts(const FlatAST& flat, const StringPool& pool,
                                         TypeRegistry& reg, const std::vector<NodeId>& nodes,
                                         std::vector<OwnershipNote>& notes_out) {
        for (auto id : nodes) {
            if (id == NULL_NODE || id >= flat.size())
                continue;
            auto v = flat.get(id);
            if (v.tag != NodeTag::IfExpr)
                continue;
            if (v.children.empty())
                continue;
            NodeId cond_id = v.child(0);
            if (cond_id == NULL_NODE)
                continue;
            // Issue #240: per-node occurrence-dirty filter. The
            // pre-#240 implementation flagged every if-context in
            // the dirty scope as suspect, which produced a flood of
            // false positives under WarningsOnly mode (any if-context
            // whose enclosing define was mutated would fire). With the
            // per-node dirty bitmask (#188), the structural mutation
            // can be tagged with DirtyReason::kOccurrenceDirty when
            // it actually affects narrowing (e.g. mutating the
            // predicate node itself or the bound variable's type).
            // We skip nodes that aren't tagged with the
            // occurrence-dirty reason, focusing the diagnostic on
            // contexts where narrowing may genuinely be stale.
            //
            // Fallback: if the node has the generic dirty bit but
            // NOT the occurrence-specific one, the conservative
            // pre-#240 path still flags it (so we don't miss a real
            // narrowing invalidation just because the mutator didn't
            // bother to set the reason bit). The cost is the same
            // as pre-#240 behavior for callers that don't yet
            // track reasons.
            const std::uint8_t kOccurrenceBit =
                static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
            bool has_occ_bit = flat.is_dirty_for(id, kOccurrenceBit);
            bool has_general_only = flat.is_dirty(id) && !has_occ_bit;
            // Analyze the predicate to confirm it actually carries
            // narrowing. (Skip if not an if-context with a narrowing
            // predicate — avoids emitting spurious notes.)
            bool m3, j3;
            auto occ = analyze_predicate_flat(flat, pool, cond_id, reg, m3, j3);
            // Issue #338: meet/join counters aren't
            // bumped here — this is a static function
            // outside the InferenceEngine. The counters
            // are bumped at the call sites in
            // synthesize_flat_if / check_flat_if.
            if (!occ)
                continue;
            if (!has_occ_bit && !has_general_only)
                continue;
            OwnershipNote note;
            note.node = id;
            // Issue #240: tag with finer-grained kind when the
            // occurrence-dirty bit is set explicitly. This lets
            // callers distinguish "this if-context was specifically
            // flagged for narrowing invalidation" from
            // "conservative: dirty scope contains an if-context".
            note.kind = has_occ_bit ? "StaleOccurrenceRefinement"
                                    : "StaleOccurrenceRefinement-conservative";
            note.message = "occurrence narrowing on '" + occ->var_name +
                           "' in dirty scope may be invalidated by mutation";
            notes_out.push_back(std::move(note));
        }
    }

    static void attach_mutation_blame(OwnershipNote& note, const MutationRecord& rec) {
        note.source_mutation_id = rec.mutation_id;
        note.blame = BlameInfo{BlameParty::System, rec.operator_name, "mutation"};
    }

    // Issue #612: collect constructor names from a DefineType node.
    static std::vector<std::string>
    collect_define_type_ctor_names(const FlatAST& flat, const StringPool& pool, NodeId id) {
        std::vector<std::string> ctors;
        if (id == NULL_NODE || id >= flat.size())
            return ctors;
        auto v = flat.get(id);
        if (v.tag != NodeTag::DefineType)
            return ctors;
        for (auto cid : v.children) {
            if (cid >= flat.size())
                continue;
            auto cv = flat.get(cid);
            if (cv.tag != NodeTag::Quote || cv.children.empty())
                continue;
            auto walk_quoted = cv.child(0);
            if (walk_quoted >= flat.size())
                continue;
            auto wv = flat.get(walk_quoted);
            if (wv.tag == NodeTag::Pair && !wv.children.empty()) {
                auto car_id = wv.child(0);
                if (car_id < flat.size()) {
                    auto car_v = flat.get(car_id);
                    if (car_v.tag == NodeTag::Variable) {
                        auto cname = std::string(pool.resolve(car_v.sym_id));
                        if (!cname.empty())
                            ctors.push_back(cname);
                    }
                }
            }
        }
        return ctors;
    }

    static void invalidate_match_exhaust_for_adt_type(FlatAST& flat, TypeId adt_tid,
                                                      void* metrics) {
        auto* m = static_cast<CompilerMetrics*>(metrics);
        for (NodeId id = 0; id < flat.size(); ++id) {
            if (!flat.has_match_info(id))
                continue;
            auto* mi = flat.get_match_info(id);
            if (!mi)
                continue;
            bool affects = false;
            if (mi->subject_type_id > 0 && mi->subject_type_id < 0xFFFFFFFFu &&
                TypeId{mi->subject_type_id, 1}.index == adt_tid.index) {
                affects = true;
            }
            if (!affects)
                continue;
            if (mi->exhaustiveness_checked && m)
                m->adt_stale_exhaust_prevented_total.fetch_add(1, std::memory_order_relaxed);
            MatchClauseInfo updated = *mi;
            updated.exhaustiveness_checked = false;
            flat.set_match_info(id, std::move(updated));
        }
    }

    // Issue #260: re-run ADT exhaustiveness on dirty __match_tmp lets
    // (including nested matches — each match is its own let node).
    static void recheck_match_exhaustiveness_in_dirty_scope(
        FlatAST& flat, const StringPool& pool, TypeRegistry& reg, const std::vector<NodeId>& nodes,
        const MutationRecord& rec, std::vector<OwnershipNote>& notes_out, void* metrics) {
        auto* m = static_cast<CompilerMetrics*>(metrics);
        for (auto id : nodes) {
            if (id == NULL_NODE || id >= flat.size())
                continue;
            if (!flat.has_match_info(id))
                continue;
            if (m)
                m->adt_exhaust_rechecks_total.fetch_add(1, std::memory_order_relaxed);
            auto let_v = flat.get(id);
            bool had_subject_type = false;
            bool narrowed_subject = false;
            if (let_v.tag == NodeTag::Let && !let_v.children.empty() &&
                let_v.child(0) != NULL_NODE) {
                auto tid_raw = flat.type_id(let_v.child(0));
                had_subject_type = tid_raw > 0 && tid_raw < reg.size();
                if (auto* mi = flat.get_match_info(id)) {
                    narrowed_subject = mi->subject_type_id > 0 && mi->subject_type_id != tid_raw;
                }
            }
            if (narrowed_subject && m)
                m->adt_occurrence_narrow_in_match_total.fetch_add(1, std::memory_order_relaxed);
            auto missing = analyze_match_exhaustiveness(flat, pool, reg, id);
            if (auto* mi = flat.get_match_info(id)) {
                MatchClauseInfo updated = *mi;
                updated.exhaustiveness_checked = true;
                if (had_subject_type && let_v.tag == NodeTag::Let && !let_v.children.empty())
                    updated.subject_type_id = flat.type_id(let_v.child(0));
                flat.set_match_info(id, std::move(updated));
            }
            if (!missing.empty()) {
                OwnershipNote note;
                note.node = id;
                note.kind = "MissingConstructorInNestedMatch";
                note.message =
                    "match exhaustiveness stale after mutation: missing '" + missing[0] + "'";
                if (missing.size() > 1) {
                    note.message += " (and " + std::to_string(missing.size() - 1) + " more)";
                }
                attach_mutation_blame(note, rec);
                notes_out.push_back(std::move(note));
                continue;
            }
            // Subject type unknown — conservative fallback (#147).
            // Issue #351: per-node occurrence-dirty scoping
            // (mirrors the #240 fix on find_occurrence_contexts
            // for IfExpr). Three outcomes:
            //   - has kOccurrenceDirty: emit the PRECISE note
            //     ("invalidated-match-narrowing") — the
            //     mutation actually touched the narrowing
            //   - has kGeneralDirty only: emit the CONSERVATIVE
            //     note ("invalidated-match-pattern") — the
            //     conservative fallback (#147)
            //   - clean: skip the node entirely (post-#240
            //     same as the IfExpr path)
            if (!had_subject_type) {
                const auto* mi = flat.get_match_info(id);
                if (!mi)
                    continue;
                // Issue #240 / #351: per-node occurrence-dirty
                // filter. The kOccurrenceDirty bit (#188) is
                // set when the mutation actually affects
                // narrowing (e.g. the subject type changed).
                // For match patterns, the precise path
                // corresponds to "the subject type / constructor
                // signature changed", the conservative path
                // to "something in the dirty scope changed".
                const std::uint8_t kOccurrenceBit =
                    static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
                const bool has_occ_bit = flat.is_dirty_for(id, kOccurrenceBit);
                // Skip clean match nodes (same as #240 IfExpr
                // path). This is the high-value case: a
                // narrow mutation in a deep workspace no
                // longer fires the conservative note for
                // unrelated match patterns.
                if (!has_occ_bit && !flat.is_dirty(id))
                    continue;
                OwnershipNote note;
                note.node = id;
                note.kind =
                    has_occ_bit ? "invalidated-match-narrowing" : "invalidated-match-pattern";
                note.message =
                    std::string("match pattern in dirty scope ") +
                    (has_occ_bit ? "narrowing" : "may be invalidated") + " by mutation (used " +
                    std::to_string(mi->used_constructors.size()) +
                    " constructors, has_wildcard=" + (mi->has_wildcard ? "true" : "false") + ")";
                attach_mutation_blame(note, rec);
                notes_out.push_back(std::move(note));
            }
        }
    }

    static void collect_adt_nodes_in_subtrees(const FlatAST& flat, const StringPool& pool,
                                              const std::vector<NodeId>& roots,
                                              std::vector<NodeId>& define_types,
                                              std::vector<NodeId>& match_lets) {
        std::unordered_set<NodeId> seen_dt;
        std::unordered_set<NodeId> seen_match;
        for (auto root : roots) {
            if (root == NULL_NODE || root >= flat.size())
                continue;
            std::vector<NodeId> stack = {root};
            std::unordered_set<NodeId> walk_seen;
            while (!stack.empty()) {
                auto id = stack.back();
                stack.pop_back();
                if (id == NULL_NODE || id >= flat.size() || !walk_seen.insert(id).second)
                    continue;
                auto v = flat.get(id);
                if (v.tag == NodeTag::DefineType && seen_dt.insert(id).second)
                    define_types.push_back(id);
                if (v.tag == NodeTag::Let && std::string(pool.resolve(v.sym_id)) == "__match_tmp" &&
                    seen_match.insert(id).second) {
                    match_lets.push_back(id);
                }
                for (auto c : v.children)
                    stack.push_back(c);
            }
        }
    }

    static void record_match_pattern_narrowing_provenance(FlatAST& flat, const StringPool& pool,
                                                          TypeRegistry& reg, NodeId match_let,
                                                          std::uint64_t mutation_id,
                                                          std::uint64_t cache_epoch,
                                                          void* metrics) {
        if (match_let == NULL_NODE || match_let >= flat.size() || !flat.has_match_info(match_let))
            return;
        auto v = flat.get(match_let);
        if (v.tag != NodeTag::Let || std::string(pool.resolve(v.sym_id)) != "__match_tmp")
            return;
        const auto* mi = flat.get_match_info(match_let);
        if (!mi)
            return;
        std::uint32_t tid_raw = mi->subject_type_id;
        if ((tid_raw == 0 || tid_raw >= reg.size()) && !v.children.empty())
            tid_raw = flat.type_id(v.child(0));
        std::string refined_str;
        if (tid_raw > 0 && tid_raw < reg.size()) {
            auto name = reg.name_of(TypeId{tid_raw, 1});
            if (!name.empty())
                refined_str = std::string(name);
        }
        std::string pred_src = "match:";
        for (const auto& ctor : mi->used_constructors)
            pred_src += ctor + ",";
        if (mi->has_wildcard)
            pred_src.push_back('_');
        NarrowingRecord rec;
        rec.var_name = "__match_tmp";
        rec.predicate_src = pred_src;
        rec.refined_type_str = refined_str;
        rec.if_node = match_let;
        rec.cond_node = match_let;
        rec.narrow_evidence = static_cast<std::uint32_t>(mi->used_constructors.size());
        rec.capture_epoch = cache_epoch;
        rec.source_mutation_id = mutation_id;
        flat.record_narrowing(std::move(rec));
        if (!metrics)
            return;
        auto* m = static_cast<CompilerMetrics*>(metrics);
        m->adt_pattern_narrow_refreshes_total.fetch_add(1, std::memory_order_relaxed);
        if (mutation_id != 0 && !refined_str.empty() && !mi->used_constructors.empty())
            m->adt_pattern_provenance_complete_total.fetch_add(1, std::memory_order_relaxed);
    }

    static void refresh_adt_constructors_for_dirty_define_types_impl(
        FlatAST& flat, const StringPool& pool, TypeRegistry& reg,
        const std::vector<NodeId>& dirty_nodes, void* metrics) {
        auto* m = static_cast<CompilerMetrics*>(metrics);
        for (auto id : dirty_nodes) {
            if (id == NULL_NODE || id >= flat.size())
                continue;
            if (flat.get(id).tag != NodeTag::DefineType)
                continue;
            auto type_name = std::string(pool.resolve(flat.get(id).sym_id));
            auto ctors = collect_define_type_ctor_names(flat, pool, id);
            if (ctors.empty())
                continue;
            auto tid = reg.lookup_type(type_name);
            if (!tid.valid())
                tid = reg.register_type(TypeTag::VARIANT, type_name);
            if (!tid.valid())
                continue;
            reg.register_adt_constructors(tid, ctors);
            if (m)
                m->adt_variant_mutate_impacts_total.fetch_add(1, std::memory_order_relaxed);
            invalidate_match_exhaust_for_adt_type(flat, tid, metrics);
        }
    }

} // anonymous namespace

void refresh_adt_constructors_for_dirty_define_types(FlatAST& flat, const StringPool& pool,
                                                     TypeRegistry& reg,
                                                     const std::vector<NodeId>& dirty_nodes,
                                                     void* metrics) {
    refresh_adt_constructors_for_dirty_define_types_impl(flat, pool, reg, dirty_nodes, metrics);
}

void revalidate_adt_typed_mutation_scope(FlatAST& flat, const StringPool& pool, TypeRegistry& reg,
                                         const std::vector<NodeId>& subtree_roots,
                                         const MutationRecord& rec, std::uint64_t cache_epoch,
                                         void* metrics) {
    std::vector<NodeId> define_types;
    std::vector<NodeId> match_lets;
    collect_adt_nodes_in_subtrees(flat, pool, subtree_roots, define_types, match_lets);
    if (define_types.empty() && match_lets.empty())
        return;
    refresh_adt_constructors_for_dirty_define_types_impl(flat, pool, reg, define_types, metrics);
    for (auto id : match_lets) {
        if (!flat.has_match_info(id))
            continue;
        if (metrics) {
            static_cast<CompilerMetrics*>(metrics)->adt_exhaust_rechecks_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        auto let_v = flat.get(id);
        if (let_v.tag == NodeTag::Let && !let_v.children.empty()) {
            const auto tid_raw = flat.type_id(let_v.child(0));
            if (auto* mi = flat.get_match_info(id)) {
                if (mi->subject_type_id > 0 && mi->subject_type_id != tid_raw && metrics) {
                    static_cast<CompilerMetrics*>(metrics)
                        ->adt_occurrence_narrow_in_match_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                }
            }
        }
        const auto missing = analyze_match_exhaustiveness(flat, pool, reg, id);
        if (!missing.empty() && metrics) {
            static_cast<CompilerMetrics*>(metrics)->adt_non_exhaustive_caught_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (auto* mi = flat.get_match_info(id)) {
            MatchClauseInfo updated = *mi;
            updated.exhaustiveness_checked = true;
            if (let_v.tag == NodeTag::Let && !let_v.children.empty())
                updated.subject_type_id = flat.type_id(let_v.child(0));
            flat.set_match_info(id, std::move(updated));
        }
        record_match_pattern_narrowing_provenance(flat, pool, reg, id, rec.mutation_id, cache_epoch,
                                                  metrics);
    }
}

void record_linear_ownership_mutation_metrics(void* metrics, bool revalidated,
                                              const std::vector<OwnershipNote>& ownership_notes,
                                              bool pass) {
    auto bump_occurrence = [&](struct CompilerMetrics* m) {
        if (!m)
            return;
        if (revalidated) {
            m->linear_occurrence_revalidate_hits_total.fetch_add(1, std::memory_order_relaxed);
            if (pass)
                m->linear_occurrence_predicate_safe_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (!pass)
            m->linear_occurrence_escape_prevented_total.fetch_add(1, std::memory_order_relaxed);
        for (const auto& note : ownership_notes) {
            if (note.kind == "use-after-move" || note.kind == "double-borrow" ||
                note.kind == "invalid-state" || note.kind == "leaked-linear")
                m->linear_occurrence_escape_prevented_total.fetch_add(1, std::memory_order_relaxed);
        }
    };
    bump_occurrence(static_cast<struct CompilerMetrics*>(metrics));
    if (!metrics)
        return;
    auto* m = static_cast<struct CompilerMetrics*>(metrics);
    if (revalidated) {
        m->linear_post_mutate_revalidations_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!pass) {
        m->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto& note : ownership_notes) {
        if (note.kind == "leaked-linear") {
            m->linear_leak_prevented_total.fetch_add(1, std::memory_order_relaxed);
        } else if (note.kind == "use-after-move" || note.kind == "double-borrow" ||
                   note.kind == "invalid-state") {
            m->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

aura::ast::InvariantStatus post_mutation_invariant_check(aura::ast::FlatAST& flat,
                                                         const StringPool& pool, TypeRegistry& reg,
                                                         const aura::ast::MutationRecord& rec,
                                                         std::vector<OwnershipNote>& notes_out,
                                                         void* metrics) {
    // Pick a root to walk. For target_node mutations we use the
    // target subtree plus the dirty-upward chain (mark_dirty_upward
    // marked every ancestor of target_node). For subtree-level
    // mutations (replace-subtree) the target_node is the new root
    // but the relevant context is the parent's slot, so we walk
    // from parent_id when it is set.
    NodeId walk_root = NULL_NODE;
    if (rec.parent_id != NULL_NODE)
        walk_root = rec.parent_id;
    else if (rec.target_node != NULL_NODE)
        walk_root = rec.target_node;

    if (walk_root == NULL_NODE || walk_root >= flat.size()) {
        return aura::ast::InvariantStatus::NotChecked;
    }

    // Build the dirty node set: descendants of walk_root + ancestors
    // of rec.target_node. The ancestor walk uses FlatAST::parent_of
    // (public) for safe access.
    std::vector<NodeId> dirty_nodes;
    collect_descendants(flat, walk_root, dirty_nodes);
    if (rec.target_node != NULL_NODE && rec.target_node < flat.size()) {
        NodeId cur = rec.target_node;
        std::size_t safety = 0;
        while (cur != NULL_NODE && cur < flat.size() && safety++ < flat.size()) {
            if (std::find(dirty_nodes.begin(), dirty_nodes.end(), cur) == dirty_nodes.end())
                dirty_nodes.push_back(cur);
            cur = flat.parent_of(cur);
        }
    }

    if (dirty_nodes.empty()) {
        return aura::ast::InvariantStatus::NotChecked;
    }

    // ── Ownership re-validation on dirty linear bindings ─────────
    std::unordered_set<std::string> linear_bindings;
    discover_linear_bindings(flat, pool, dirty_nodes, linear_bindings);
    if (!linear_bindings.empty() && flat.root != NULL_NODE && flat.root < flat.size()) {
        const auto notes_before = notes_out.size();
        const bool ownership_pass =
            OwnershipEnv::validate_ownership(flat, pool, flat.root, linear_bindings, notes_out);
        std::vector<OwnershipNote> ownership_notes;
        if (notes_out.size() > notes_before) {
            ownership_notes.assign(notes_out.begin() + static_cast<std::ptrdiff_t>(notes_before),
                                   notes_out.end());
        }
        record_linear_ownership_mutation_metrics(metrics, true, ownership_notes, ownership_pass);
    }

    // ── Occurrence-narrowing re-check on dirty nodes ─────────────
    find_occurrence_contexts(flat, pool, reg, dirty_nodes, notes_out);

    // Issue #612: refresh ADT ctor lists from mutated DefineType
    // nodes before re-running exhaustiveness (prevents stale
    // TypeRegistry entries after variant add/remove).
    refresh_adt_constructors_for_dirty_define_types_impl(flat, pool, reg, dirty_nodes, metrics);

    // Issue #260: nested match exhaustiveness + conservative fallback.
    recheck_match_exhaustiveness_in_dirty_scope(flat, pool, reg, dirty_nodes, rec, notes_out,
                                                metrics);

    for (auto& note : notes_out) {
        if (!note.source_mutation_id)
            note.source_mutation_id = rec.mutation_id;
        if (!note.blame)
            note.blame = BlameInfo{BlameParty::System, rec.operator_name, "mutation"};
    }

    if (notes_out.empty())
        return aura::ast::InvariantStatus::Ok;
    return aura::ast::InvariantStatus::Warnings;
}

// Issue #148 Phase 3: identify the affected node set for a
// mutation. Walks the FlatAST similarly to
// post_mutation_invariant_check but returns the NodeId set
// instead of running invariant checks. Used by
// InferenceEngine::infer_flat in Phase 4 to scope partial
// re-inference.
//
// Walk strategy:
//   1. Pick walk_root = rec.parent_id (subtree-level mutation)
//      or rec.target_node (typed mutation on existing node).
//   2. collect_descendants(walk_root) — the entire dirty subtree.
//   3. Climb from rec.target_node via FlatAST::parent_of to add
//      the dirty-upward ancestor chain. Safety-bounded by
//      flat.size() to defend against parent_ cycles.
//
// Returns empty vector if walk_root is NULL/out-of-range so the
// caller can fall back to a full infer_flat.
std::vector<aura::ast::NodeId>
affected_subtree_from_mutation(const aura::ast::FlatAST& flat,
                               const aura::ast::MutationRecord& rec) {
    using namespace aura::ast;

    NodeId walk_root = NULL_NODE;
    if (rec.parent_id != NULL_NODE)
        walk_root = rec.parent_id;
    else if (rec.target_node != NULL_NODE)
        walk_root = rec.target_node;

    if (walk_root == NULL_NODE || walk_root >= flat.size())
        return {};

    std::vector<NodeId> affected;
    // Descendants of walk_root
    collect_descendants(flat, walk_root, affected);
    // Ancestors of rec.target_node (dirty-upward chain)
    if (rec.target_node != NULL_NODE && rec.target_node < flat.size()) {
        NodeId cur = rec.target_node;
        std::size_t safety = 0;
        while (cur != NULL_NODE && cur < flat.size() && safety++ < flat.size()) {
            if (std::find(affected.begin(), affected.end(), cur) == affected.end())
                affected.push_back(cur);
            cur = flat.parent_of(cur);
        }
    }
    return affected;
}

// Issue #410: per-symbol affected subtree (foundation only —
// this is the observability helper, not the fast path yet).
// Walks the entire flat looking for Variable nodes whose
// sym_id matches the input. The result is the set of nodes
// that re-infer_flat_partial would visit if we wired this
// into infer_flat_partial (Phase 2/2).
//
// Performance: O(n) over all nodes. For very large ASTs this
// is wasteful; the production path should use
// DefUseIndex::query_def_use(sym).uses instead. The Aura
// primitive exposes this baseline + the per-sym walk side
// by side so users can see how often the DefUseIndex path
// would have saved work. For a body with N bindings and a
// mutate on sym S with K uses, the per-sym path returns K
// vs ancestor-only's ~N*K (all parent chain nodes per use).
//
// Returns empty vector if sym_id is INVALID_SYM. The def
// node itself (a Define/Let binding S) is NOT included —
// Variable nodes are use-sites only; the def re-checks itself
// via its own cached type lookup in synthesize_flat.
std::vector<aura::ast::NodeId> affected_subtree_for_symbol(const aura::ast::FlatAST& flat,
                                                           aura::ast::SymId sym_id) {
    using namespace aura::ast;
    std::vector<NodeId> affected;
    if (sym_id == INVALID_SYM)
        return affected;
    // Walk the flat once. Variable nodes carry their sym_id
    // in the sym_id_ column (aura::ast::FlatAST::sym_id).
    // The walk is a simple linear scan; for large workspaces
    // a future commit can route through DefUseIndex which is
    // O(uses) instead of O(n).
    const std::size_t n = flat.size();
    affected.reserve(n / 8); // rough heuristic: typical body has ~12% Variable density
    for (std::size_t i = 0; i < n; ++i) {
        auto v = flat.get(static_cast<NodeId>(i));
        if (v.tag == NodeTag::Variable && v.sym_id == sym_id)
            affected.push_back(static_cast<NodeId>(i));
    }
    return affected;
}

} // namespace aura::compiler
