// ── Issue #379: AST stability operations impl unit ────────────
//
// Scope-limited first cut of #379. The full AC proposes
// "centralize AST stability, generation and dirty tracking into
// dedicated component" — too big for one cycle. This file
// establishes the pattern (declaration in ast.ixx, body here) by
// moving the 5 smallest, lowest-coupling stability functions:
//
//   - FlatAST::serialize_stable_ref          (member fn, public only)
//   - FlatAST::deserialize_stable_ref        (member fn, public only)
//   - FlatAST::StableNodeRef::is_valid_in    (calls public ast.is_valid)
//   - FlatAST::StableNodeRef::validate_with_provenance
//                                            (uses public ast.generation()
//                                             instead of private ast.generation_
//                                             — same uint16_t value, no
//                                             behavior change)
//   - FlatAST::StableNodeRef::get_provenance (pure field read, no FlatAST)
//
// Same module + impl-unit pattern as ast_impl.cpp: this file
// is `module aura.core.ast;` (no `export`), so all declarations
// stay `export` in ast.ixx. The impl unit sees the interface
// (including private members via the class scope) and provides
// the function bodies.
//
// What stays in ast.ixx (deferred to separate issues / follow-ups):
//
//   - StableNodeRef struct itself: nested inside FlatAST, can't
//     move to a separate file without breaking the public API
//     (`FlatAST::StableNodeRef` is referenced from many call sites).
//     Promoting to a top-level type is a separate refactor with its
//     own deprecation path.
//
//   - make_ref / make_ref_in_layer / make_safe_ref /
//     capture_for_fiber: construct a StableNodeRef from FlatAST's
//     current state (generation_, next_mutation_id_, wrap_epoch_).
//     These are tightly coupled to FlatAST's internal counters and
//     belong with the class body. Could move with friend access.
//
//   - bump_generation / mark_dirty_upward and variants
//     (mark_dirty_upward_fast, mark_dirty_upward_until,
//     mark_dirty_defuse_entries): need to read and write private
//     SoA columns (dirty_, node_gen_, parent_, type_cache_generation_,
//     binding_gen, etc.). To move them, FlatAST would need friend
//     declarations for the free functions in this module, OR a
//     public accessor layer over those columns. Either is a larger
//     change — separate follow-up.
//
//   - bump_generation_on_rollback: couples to the rollback
//     state machine in FlatAST. Could move with friend access.

module;

#include <cstring>
#include "core/provenance_tracker.hh"

module aura.core.ast;
import std;
import aura.core.type;

namespace aura::ast {

// ── StableNodeRef methods (moved from ast.ixx) ──────────────

// Default-constructed refs are always invalid (id=NULL).
bool FlatAST::StableNodeRef::is_valid_in(const FlatAST& ast) const noexcept {
    return ast.is_valid(*this);
}

// Issue #715: cross-layer StableNodeRef validity check. A ref
// captured in workspace layer A is only valid in workspace
// layer B if BOTH:
//   - is_valid_in(ast) passes (gen + wrap_epoch match), AND
//   - ref.workspace_id_ == target_workspace_id (the ref was
//     actually captured in the target layer), AND
//   - ref.cow_epoch_at_capture == ast.workspace_cow_epoch()
//     (the target layer hasn't done a COW clone since the
//     ref was captured; if it has, the ref points to the
//     stale pre-COW parent copy unless pin_for_cow() was
//     called, in which case the ref is intended to survive
//     the boundary).
//
// Pure read — does NOT update last_validated_generation (use
// validate_with_provenance for that). Does NOT bump counters
// (callers in hot paths can call bump_stable_ref_cross_layer
// _validation / _mismatch via Evaluator if they want
// observability; the helper itself stays allocation-free
// for use in tight loops).
//
// The default target_workspace_id is 0 (root) so callers
// migrating from is_valid_in() can drop in this helper
// without changing call sites. The cross-layer check
// becomes meaningful when a WorkspaceTree merges or
// resolves refs across layers — see
// query:stable-ref-layer-stats for the per-workspace
// observability surface (Issue #715).
bool FlatAST::StableNodeRef::is_valid_in_layer(const FlatAST& ast,
                                               std::uint32_t target_workspace_id) const noexcept {
    if (!is_valid_in(ast))
        return false;
    if (workspace_id != target_workspace_id)
        return false;
    // COW boundary check: if the target layer's cow_epoch
    // has advanced past the ref's capture epoch, the ref
    // points to a stale parent copy UNLESS it was
    // explicitly pinned via pin_for_cow(). Pinned refs
    // intentionally survive COW so the agent's checkpoint
    // state remains usable across lazy clones.
    if (!boundary_pinned && cow_epoch_at_capture != ast.workspace_cow_epoch())
        return false;
    return true;
}

// Issue #303: validate with provenance update. Refreshes
// last_validated_generation to the current FlatAST
// generation_ and returns the validation result. The
// side effect of updating the field is the audit trail:
// subsequent code can compare ref.last_validated_generation
// against ast.generation_() to detect "ref hasn't been
// re-checked in a while" (proxy for staleness without
// requiring a full re-validation).
bool FlatAST::StableNodeRef::validate_with_provenance(const FlatAST& ast) noexcept {
    bool ok = ast.is_valid(*this);
    if (ok) {
        // Issue #379: switched from ast.generation_ (private member
        // access from the original inline body) to ast.generation()
        // (public accessor at L4527 in ast.ixx). Same uint16_t
        // value, no behavior change. The switch is required because
        // this method is no longer defined inside FlatAST's class
        // body, so it loses implicit access to FlatAST's private
        // members.
        last_validated_generation = ast.generation();
    }
    return ok;
}

// Issue #1500/#1564: full-provenance auto-refresh + process-wide enforcement counters.
//
// Prior behavior required is_valid_id_gen(id, gen, wrap_epoch) — i.e.
// the slot still carried the *captured* gen. That fails after any
// restamp_all_node_generations() (MutationBoundaryGuard exit always
// restamps), even when the node is still live. Production multi-agent
// loops hit that path constantly.
//
// New policy for a still-live node (same wrap cycle, not free):
//   1. restamp the subtree node_gen_ to current generation_
//   2. remake full provenance (gen/wrap/cow/mutation_id/subtree_gen)
//   3. preserve fiber_id / workspace_id / boundary_pinned from the
//      caller's long-held handle (Agent / fiber context)
//
// Hard failures (return false): NULL/OOR id, free slot, wrap_epoch
// mismatch with a non-zero captured epoch (second wrap cycle).
//
// Contract (#1564): every refresh that restamps gen/cow is counted on
// both FlatAST::stale_ref_auto_refresh and process-wide
// provenance::stable_ref_auto_refresh_total. Epoch-fence style wrap
// mismatches bump stable_ref_epoch_fence_hit_total (no restamp).
bool FlatAST::StableNodeRef::refresh_if_stale(FlatAST& ast) noexcept {
    if (is_valid_in(ast)) {
        validate_with_provenance(ast);
        return true;
    }
    if (id == NULL_NODE || id >= ast.size())
        return false;
    // wrap_epoch == 0 means pre-#368 / brace-init legacy: allow
    // refresh into the current wrap cycle. Non-zero mismatch is fatal
    // (epoch fence hit — second wrap cycle).
    if (wrap_epoch != 0 && wrap_epoch != ast.wrap_epoch()) {
        aura::core::provenance::record_epoch_fence_hit();
        return false;
    }
    if (ast.is_free_slot(id) || !ast.is_live_node(id))
        return false;

    // Cross-layer COW without pin: count mismatch before restamp.
    if (!boundary_pinned && cow_epoch_at_capture != 0 &&
        cow_epoch_at_capture != ast.workspace_cow_epoch()) {
        aura::core::provenance::record_cross_layer_mismatch();
    }

    // Preserve cross-fiber / cross-layer / pin / tenant provenance across
    // refresh. Issue #2056: tenant_id must survive remake so cross-tenant
    // isolation still denies after gen restamp (FailOnStale refuses silent
    // tenant restamp; AutoRefresh only remakes gen/cow/wrap).
    const auto preserved_fiber = fiber_id;
    const auto preserved_ws = workspace_id;
    const auto preserved_pin = boundary_pinned;
    const auto preserved_tenant = tenant_id;

    // Align slot gen with current FlatAST generation before remake.
    ast.restamp_subtree_generation(id);
    const auto fresh = ast.make_safe_ref(id, preserved_ws, preserved_fiber);
    id = fresh.id;
    gen = fresh.gen;
    mutation_id_at_capture = fresh.mutation_id_at_capture;
    workspace_id = preserved_ws;
    fiber_id = preserved_fiber;
    last_validated_generation = ast.generation();
    wrap_epoch = fresh.wrap_epoch;
    subtree_gen_at_capture = fresh.subtree_gen_at_capture;
    // Always restamp cow_epoch to the live layer; pin flag is preserved
    // so a subsequent COW advance without re-refresh still allows the
    // boundary_pinned exception in is_valid / is_valid_in_layer.
    cow_epoch_at_capture = fresh.cow_epoch_at_capture;
    boundary_pinned = preserved_pin;
    tenant_id = preserved_tenant;
    if (preserved_tenant != 0)
        aura::core::provenance::record_stable_ref_tenant_preserved_on_refresh();

    ast.record_stale_ref_auto_refresh();
    aura::core::provenance::record_auto_refresh();
    return is_valid_in(ast);
}

std::optional<NodeView> FlatAST::StableNodeRef::validate_or_refresh(FlatAST& ast) noexcept {
    // Issue #1346/#1564: lock-free hot path — pure atomic reads of generation /
    // free-slot / provenance fields; no workspace_mtx_ acquisition.
    // Contended mutation paths still take MutationBoundaryGuard elsewhere.
    // Contract: all EDSL/query/mutate paths that hold StableNodeRef must
    // prefer this entry (or Evaluator::ensure_valid_or_refresh).
    if (!refresh_if_stale(ast))
        return std::nullopt;
    // Stale-refresh already bumped stale_ref_auto_refresh when remap needed;
    // always count a successful lock-free validate for observability.
    ast.record_lockfree_stable_ref_validate();
    return ast.get_safe(*this);
}

// Issue #303: get provenance snapshot. Returns a tuple
// describing where the ref came from. Pure read — does
// not validate the ref.
FlatAST::StableNodeRef::Provenance FlatAST::StableNodeRef::get_provenance() const noexcept {
    return Provenance{id,           gen,      mutation_id_at_capture,
                      workspace_id, fiber_id, last_validated_generation};
}

// ── StableNodeRef serialization (moved from ast.ixx) ────────
//
// Issue #291 / #392 / #2198: pack a StableNodeRef into the
// current wire format (v2, 56 bytes). Returns bytes written.
//
// v1 (24 bytes) is still accepted on deserialize with safe
// defaults for tenant/fiber/pin/cow/wrap (AC2). v2 round-trips
// the full provenance surface required for cross-session Agent
// memory and multi-tenant restore (AC1 / #2056).
//
// Layout documented on FlatAST::kStableRefSerializedSize* in
// ast.ixx — keep that comment in sync when changing the blob.
std::size_t FlatAST::serialize_stable_ref(const StableNodeRef& ref,
                                          std::uint8_t* out) const noexcept {
    // Zero the full v2 record so reserved/trailing bytes are
    // deterministic (and v1 readers that only look at 24 bytes
    // still see a valid header if they ignore trailing data).
    std::memset(out, 0, kStableRefSerializedSizeV2);

    // [0..3] magic
    out[0] = static_cast<std::uint8_t>(kStableRefMagic & 0xFF);
    out[1] = static_cast<std::uint8_t>((kStableRefMagic >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((kStableRefMagic >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((kStableRefMagic >> 24) & 0xFF);
    // [4..7] id
    std::memcpy(out + 4, &ref.id, sizeof(ref.id));
    // [8..9] gen
    std::memcpy(out + 8, &ref.gen, sizeof(ref.gen));
    // [10] version = 2  [11] flags
    out[10] = kStableRefWireVersionV2;
    out[11] = ref.boundary_pinned ? kStableRefFlagBoundaryPinned : static_cast<std::uint8_t>(0);
    // [12..15] mutation_id low 32 (v1-compatible slot)
    const std::uint32_t mid_lo =
        static_cast<std::uint32_t>(ref.mutation_id_at_capture & 0xFFFFFFFFu);
    std::memcpy(out + 12, &mid_lo, sizeof(mid_lo));
    // [16..17] subtree_gen_at_capture
    std::memcpy(out + 16, &ref.subtree_gen_at_capture, sizeof(ref.subtree_gen_at_capture));
    // [18..19] last_validated_generation (v2 header; v1 had reserved zeros)
    std::memcpy(out + 18, &ref.last_validated_generation, sizeof(ref.last_validated_generation));
    // [20..23] workspace_id
    std::memcpy(out + 20, &ref.workspace_id, sizeof(ref.workspace_id));

    // ── v2 extension ───────────────────────────────────────
    // [24..27] mutation_id high 32 — full mid without silent truncation
    const std::uint32_t mid_hi =
        static_cast<std::uint32_t>((ref.mutation_id_at_capture >> 32) & 0xFFFFFFFFu);
    std::memcpy(out + 24, &mid_hi, sizeof(mid_hi));
    // [28..31] fiber_id
    std::memcpy(out + 28, &ref.fiber_id, sizeof(ref.fiber_id));
    // [32..39] tenant_id — never silently dropped (AC1 / #2056)
    std::memcpy(out + 32, &ref.tenant_id, sizeof(ref.tenant_id));
    // [40..43] wrap_epoch
    std::memcpy(out + 40, &ref.wrap_epoch, sizeof(ref.wrap_epoch));
    // [44..51] cow_epoch_at_capture
    std::memcpy(out + 44, &ref.cow_epoch_at_capture, sizeof(ref.cow_epoch_at_capture));
    // [52..55] reserved (already zeroed)

    return kStableRefSerializedSizeV2;
}

// Issue #291 / #2198: deserialize v1 (24) or v2 (56+) StableNodeRef.
//
// AC2: old 24-byte buffers still accepted; missing fields default
// safely (tenant=0, pin=false, fiber=0, wrap=0, cow=0, mid high=0).
// AC1: v2 restores tenant_id / fiber_id / boundary_pinned /
// cow_epoch_at_capture / wrap_epoch / full mutation_id.
bool FlatAST::deserialize_stable_ref(std::span<const std::uint8_t> buf,
                                     StableNodeRef& out) const noexcept {
    // Minimum is always the v1 header (24 bytes). Larger buffers
    // may be v2; smaller than v1 is hard reject.
    if (buf.size() < kStableRefSerializedSizeV1)
        return false;
    std::uint32_t magic = 0;
    std::memcpy(&magic, buf.data(), 4);
    if (magic != kStableRefMagic)
        return false;

    StableNodeRef r{};
    std::memcpy(&r.id, buf.data() + 4, sizeof(r.id));
    std::memcpy(&r.gen, buf.data() + 8, sizeof(r.gen));

    const std::uint8_t version = buf[10];
    const std::uint8_t flags = buf[11];

    std::uint32_t mid_lo = 0;
    std::memcpy(&mid_lo, buf.data() + 12, sizeof(mid_lo));
    r.mutation_id_at_capture = static_cast<std::uint64_t>(mid_lo);

    std::memcpy(&r.subtree_gen_at_capture, buf.data() + 16, sizeof(r.subtree_gen_at_capture));
    // v1: bytes 18..19 were reserved (0). v2: last_validated_generation.
    // Reading them on v1 just yields 0 — safe default.
    std::memcpy(&r.last_validated_generation, buf.data() + 18, sizeof(r.last_validated_generation));
    std::memcpy(&r.workspace_id, buf.data() + 20, sizeof(r.workspace_id));

    const bool is_v2 =
        (version == kStableRefWireVersionV2) && (buf.size() >= kStableRefSerializedSizeV2);
    if (is_v2) {
        // Flags: boundary_pinned
        r.boundary_pinned = (flags & kStableRefFlagBoundaryPinned) != 0;
        std::uint32_t mid_hi = 0;
        std::memcpy(&mid_hi, buf.data() + 24, sizeof(mid_hi));
        r.mutation_id_at_capture |= (static_cast<std::uint64_t>(mid_hi) << 32);
        std::memcpy(&r.fiber_id, buf.data() + 28, sizeof(r.fiber_id));
        std::memcpy(&r.tenant_id, buf.data() + 32, sizeof(r.tenant_id));
        std::memcpy(&r.wrap_epoch, buf.data() + 40, sizeof(r.wrap_epoch));
        std::memcpy(&r.cow_epoch_at_capture, buf.data() + 44, sizeof(r.cow_epoch_at_capture));
    } else {
        // v1 path: safe defaults for fields not on the wire.
        // tenant_id / fiber_id / boundary_pinned / wrap_epoch /
        // cow_epoch_at_capture remain 0 / false (AC2).
        r.boundary_pinned = false;
        r.fiber_id = 0;
        r.tenant_id = 0;
        r.wrap_epoch = 0;
        r.cow_epoch_at_capture = 0;
        // last_validated may be non-zero if reserved was non-zero on
        // corrupt input; leave as read (usually 0 for true v1).
    }

    out = r;
    return true;
}

} // namespace aura::ast