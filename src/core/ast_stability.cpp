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

bool FlatAST::StableNodeRef::refresh_if_stale(FlatAST& ast) noexcept {
    if (is_valid_in(ast)) {
        validate_with_provenance(ast);
        return true;
    }
    if (id == NULL_NODE || id >= ast.size())
        return false;
    if (wrap_epoch != ast.wrap_epoch())
        return false;
    if (ast.is_free_slot(id))
        return false;
    if (!ast.is_valid_id_gen(id, gen, wrap_epoch))
        return false;
    ast.restamp_subtree_generation(id);
    const auto fresh = ast.make_ref(id);
    gen = fresh.gen;
    wrap_epoch = fresh.wrap_epoch;
    subtree_gen_at_capture = fresh.subtree_gen_at_capture;
    last_validated_generation = ast.generation();
    ast.record_stale_ref_auto_refresh();
    return is_valid_in(ast);
}

std::optional<NodeView> FlatAST::StableNodeRef::validate_or_refresh(FlatAST& ast) noexcept {
    if (!refresh_if_stale(ast))
        return std::nullopt;
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
// Issue #291: pack a StableNodeRef into a 24-byte buffer.
// Returns the number of bytes written (= kStableRefSerializedSize).
//
// Format (little-endian, 24 bytes total):
//   [u32 magic=0x2901A17A][u32 id][u16 gen][u16 pad]
//   [u32 mutation_id_low][u16 subtree_gen_at_capture][u16 reserved]
//   [u32 workspace_id]
//   = 4+4+2+2+4+2+2+4 = 24 bytes
//
// Issue #392 repurposes the previously-"reserved" bytes 16..19.
// The pre-#392 writer zeroed bytes 16..19 AFTER memcpy'ing
// mutation_id_at_capture (8 bytes) into bytes 12..19, which
// silently clobbered mutation_id's upper 4 bytes (no test
// exercised them). #392 stores subtree_gen_at_capture in
// bytes 16..17 and keeps bytes 18..19 reserved for future
// fields. mutation_id_at_capture round-trips only its lower
// 32 bits — same pre-existing limitation, just no longer
// hiding under a "reserved" comment.
std::size_t FlatAST::serialize_stable_ref(const StableNodeRef& ref,
                                          std::uint8_t* out) const noexcept {
    // First 4 bytes: magic (high 16 bits = 0x2901, low 16
    // bits = 0xA17A). Reader checks this to distinguish
    // #291+ serialized refs from raw (id, gen) binary.
    out[0] = static_cast<std::uint8_t>(kStableRefMagic & 0xFF);
    out[1] = static_cast<std::uint8_t>((kStableRefMagic >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((kStableRefMagic >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((kStableRefMagic >> 24) & 0xFF);
    // Next 4 bytes: id (NodeId)
    std::memcpy(out + 4, &ref.id, sizeof(ref.id));
    // Next 2 bytes: gen
    std::memcpy(out + 8, &ref.gen, sizeof(ref.gen));
    // 2 bytes padding
    out[10] = 0;
    out[11] = 0;
    // Issue #392: mutation_id_at_capture only round-trips its
    // lower 32 bits (same trade-off as the pre-existing #291
    // serializer — upper bits were already lost in practice
    // because of the byte-range overlap with the "reserved"
    // slot). The writer is explicit about it now.
    std::uint32_t mid_lo = static_cast<std::uint32_t>(ref.mutation_id_at_capture & 0xFFFFFFFFu);
    std::memcpy(out + 12, &mid_lo, sizeof(mid_lo));
    // Issue #392: subtree_gen_at_capture round-trips through
    // bytes 16..17 (the previously-reserved slot). Pre-#392
    // buffers contain zeros in this range, which matches the
    // field's default-0 contract — old refs are still
    // accepted by is_valid_subtree() because subtree_gen_
    // starts at 0 in fresh FlatASTs.
    std::memcpy(out + 16, &ref.subtree_gen_at_capture, sizeof(ref.subtree_gen_at_capture));
    // Bytes 18..19 still reserved for future fields (fiber_id,
    // last_validated_generation, etc.). #291/#303 deliberately
    // left those fields out of the wire format — they're
    // in-memory observability only.
    out[18] = 0;
    out[19] = 0;
    // 4 bytes: workspace_id
    std::memcpy(out + 20, &ref.workspace_id, sizeof(ref.workspace_id));
    return kStableRefSerializedSize;
}

// Issue #291: deserialize a 24-byte buffer back to a
// StableNodeRef. Returns false if the magic doesn't match
// or buffer is too small. The caller is responsible for
// checking is_valid() AFTER deserializing to confirm the
// ref still points to a live node in the current flat.
//
// Issue #392: subtree_gen_at_capture round-trips through the
// repurposed byte range (16..17). mutation_id_at_capture is
// reconstructed from its lower 32 bits only (upper 32 zero-
// filled on deserialize).
bool FlatAST::deserialize_stable_ref(const std::uint8_t* buf, std::size_t buf_size,
                                     StableNodeRef& out) const noexcept {
    if (buf_size < kStableRefSerializedSize)
        return false;
    std::uint32_t magic = 0;
    std::memcpy(&magic, buf, 4);
    if (magic != kStableRefMagic)
        return false;
    StableNodeRef r{};
    std::memcpy(&r.id, buf + 4, sizeof(r.id));
    std::memcpy(&r.gen, buf + 8, sizeof(r.gen));
    // Issue #392: mutation_id_at_capture only round-trips its
    // lower 32 bits (same trade-off as the serializer).
    std::uint32_t mid_lo = 0;
    std::memcpy(&mid_lo, buf + 12, sizeof(mid_lo));
    r.mutation_id_at_capture = static_cast<std::uint64_t>(mid_lo);
    // Subtree-gen-at-capture round-trips through bytes 16..17.
    std::memcpy(&r.subtree_gen_at_capture, buf + 16, sizeof(r.subtree_gen_at_capture));
    std::memcpy(&r.workspace_id, buf + 20, sizeof(r.workspace_id));
    out = r;
    return true;
}

} // namespace aura::ast