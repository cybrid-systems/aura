// flatast_domain_views.hh — FlatAST decomp step 4: non-owning Storage / Index views.
//
// Design constraints (flatast_domains.hh):
//   - Views do NOT own SoA columns, free-list, indexes, or locks.
//   - Never split locks across types without a lock-order rewrite.
//   - Call through FlatAST public API only (duck-typed templates so this
//     header stays freestanding for the aura.core.ast GMF include path).
//
// Purpose: typed blast-radius handles for agents / call sites that only
// need Storage (SoA + free-slot) or Indexes (tag/arity + incoming parents)
// without dragging Mutation / Dirty surfaces into their mental model.

#ifndef AURA_CORE_FLATAST_DOMAIN_VIEWS_HH
#define AURA_CORE_FLATAST_DOMAIN_VIEWS_HH

#include <cstddef>
#include <cstdint>
#include <utility>

namespace aura::ast::domains {

// ── Storage domain view ──────────────────────────────────────────
// SoA size, free-list slot query, single-column reads, parent/children
// topology *reads*. Writers still go through FlatAST mutators under the
// existing concurrent-access contract (#2413 / #2418).
template <typename Ast> class FlatASTStorageView {
public:
    explicit FlatASTStorageView(Ast& ast) noexcept
        : ast_(&ast) {}

    [[nodiscard]] Ast& ast() const noexcept { return *ast_; }

    [[nodiscard]] auto size() const { return ast_->size(); }
    [[nodiscard]] auto empty() const { return ast_->empty(); }

    template <typename Id> [[nodiscard]] bool is_free_slot(Id id) const noexcept {
        return ast_->is_free_slot(id);
    }

    template <typename Id> [[nodiscard]] auto tag(Id id) const { return ast_->tag(id); }

    template <typename Id> [[nodiscard]] auto int_val(Id id) const { return ast_->int_val(id); }

    template <typename Id> [[nodiscard]] auto sym_id(Id id) const { return ast_->sym_id(id); }

    template <typename Id> [[nodiscard]] auto get(Id id) const { return ast_->get(id); }

    template <typename Id> [[nodiscard]] auto parent_of(Id id) const { return ast_->parent_of(id); }

    template <typename Id> [[nodiscard]] auto children(Id id) const { return ast_->children(id); }

    template <typename Id> [[nodiscard]] auto children_columnar(Id id) const {
        return ast_->children_columnar(id);
    }

private:
    Ast* ast_;
};

// ── Indexes domain view ──────────────────────────────────────────
// Tag/arity inverted index + multi-parent edge index. Rebuild/ensure
// still run on the host FlatAST (same locks / dirty flags).
// Non-const mutators (ensure/rebuild tag-arity) are only available when
// Ast is non-const; const Ast gets query/stats + incoming-parent ensure
// (those are const on FlatAST via mutable dirty flags).
template <typename Ast> class FlatASTIndexView {
public:
    explicit FlatASTIndexView(Ast& ast) noexcept
        : ast_(&ast) {}

    [[nodiscard]] Ast& ast() const noexcept { return *ast_; }

    // Tag / arity inverted index (#447 / #1371 / #2419)
    void ensure_tag_arity_index()
        requires requires(Ast& a) { a.ensure_tag_arity_index(); }
    {
        ast_->ensure_tag_arity_index();
    }
    void rebuild_tag_arity_index()
        requires requires(Ast& a) { a.rebuild_tag_arity_index(); }
    {
        ast_->rebuild_tag_arity_index();
    }
    void mark_tag_arity_index_dirty() const { ast_->mark_tag_arity_index_dirty(); }

    [[nodiscard]] bool tag_arity_index_dirty() const noexcept {
        return ast_->tag_arity_index_dirty();
    }
    [[nodiscard]] auto tag_arity_index_size() const noexcept {
        return ast_->tag_arity_index_size();
    }
    [[nodiscard]] auto tag_arity_index_hits() const noexcept {
        return ast_->tag_arity_index_hits();
    }
    [[nodiscard]] auto tag_arity_index_misses() const noexcept {
        return ast_->tag_arity_index_misses();
    }
    [[nodiscard]] auto tag_arity_index_rebuilds() const noexcept {
        return ast_->tag_arity_index_rebuilds();
    }

    [[nodiscard]] auto find_by_tag_arity(std::uint32_t tag, std::uint16_t arity_min,
                                         std::uint16_t arity_max) const {
        return ast_->find_by_tag_arity(tag, arity_min, arity_max);
    }

    // Incoming multi-parent edges (#1689 / #2416 / #2452)
    void ensure_incoming_parent_index() const { ast_->ensure_incoming_parent_index(); }
    void rebuild_incoming_parent_index() const { ast_->rebuild_incoming_parent_index(); }
    void mark_incoming_parent_index_dirty() const noexcept {
        ast_->mark_incoming_parent_index_dirty();
    }

    template <typename Id> [[nodiscard]] auto collect_incoming_parent_edges(Id target) const {
        return ast_->collect_incoming_parent_edges(target);
    }

    [[nodiscard]] auto incoming_parent_index_rebuilds() const noexcept {
        return ast_->incoming_parent_index_rebuilds();
    }
    [[nodiscard]] auto incoming_parent_index_lookups() const noexcept {
        return ast_->incoming_parent_index_lookups();
    }
    [[nodiscard]] auto incoming_parent_index_hits() const noexcept {
        return ast_->incoming_parent_index_hits();
    }

private:
    Ast* ast_;
};

template <typename Ast> [[nodiscard]] FlatASTStorageView<Ast> storage_view(Ast& ast) noexcept {
    return FlatASTStorageView<Ast>{ast};
}

template <typename Ast> [[nodiscard]] FlatASTIndexView<Ast> index_view(Ast& ast) noexcept {
    return FlatASTIndexView<Ast>{ast};
}

} // namespace aura::ast::domains

#endif // AURA_CORE_FLATAST_DOMAIN_VIEWS_HH
