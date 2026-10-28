// evaluator_primitives_vector.cpp — P0 step 5: vector/hash primitives
// Issue #2996: migrate onto register_prim + PrimSpec (core TU).
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;
#include "compiler/locked_string_heap.hh" // Issue #4543

#include "runtime_shared.h"
#include "prim_heap_quota.hh" // Issue #2916
#include "hash_meta.h"        // FNV constants (#901)

#include "prim_registrar_scaffold.hh" // Issue #2996
#include "security_side_effect.hh"    // Issue #4298: kEffectMutate on the in-range consult

#include <cstring> // Issue #4614: hash-copy slot-for-slot table copy

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_imm_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_imm_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_imm_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

namespace {

    // Issue #2654: language hash key hash (FNV for strings, mix for ints).
    // Keep in sync with hash / hash-set! / grow rehash placement.
    // Issue #4612: "no slot" sentinel for the probe result.
    constexpr std::size_t kNoHashSlot = static_cast<std::size_t>(-1);

    [[nodiscard]] std::uint64_t eval_hash_key_hash(const EvalValue& k,
                                                   ::aura::compiler::StringPoolView sh) noexcept {
        if (is_int(k))
            return static_cast<std::uint64_t>(as_int(k)) * 0x9e3779b97f4a7c15ull;
        if (is_string(k)) {
            auto i = as_string_idx(k);
            if (i < sh.size()) {
                // Issue #4612: the SAME hash the native string-hash
                // primitive returns (#4614) -- hash_meta.h fnv1a_bytes.
                // One string hash in the tree, not two.
                return ::aura::compiler::hash::fnv1a_bytes(std::string_view(sh[i]));
            }
        }
        return 0x9e3779b97f4a7c15ull;
    }

    [[nodiscard]] std::uint8_t eval_hash_fingerprint(std::uint64_t h) noexcept {
        auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
        if (fp == 0xFF)
            fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
        return fp;
    }

    // Issue #2654 / #2481 sibling: grow FlatHashTable at 0.7 load or when full.
    // Interpreter path stores EvalValue bits + fingerprint meta — cannot use
    // FlatHashTable::rebuild (JIT HASH_OCCUPIED + splitmix64). Replaces *htp.
    //
    // destroy_old: free the previous block. Only safe when the table is not
    // yet reachable via g_hash_tables / JIT bridges (those use workspace
    // locks, not hash_tables_mutex_). Published tables: leave old block
    // orphaned so concurrent readers cannot UAF after pointer swap.
    [[nodiscard]] bool flat_hash_grow_eval(FlatHashTable*& htp, ::aura::compiler::StringPoolView sh,
                                           bool destroy_old) {
        auto* ht = htp;
        if (!ht)
            return false;
        auto old_cap = ht->capacity;
        auto new_cap = old_cap * 2;
        if (new_cap < 8)
            new_cap = 8;
        auto* new_ht = FlatHashTable::create(new_cap);
        if (!new_ht)
            return false;
        auto old_meta = ht->metadata();
        auto old_keys = ht->keys();
        auto old_vals = ht->values();
        auto new_meta = new_ht->metadata();
        auto new_keys = new_ht->keys();
        auto new_vals = new_ht->values();
        for (std::uint64_t i = 0; i < old_cap; ++i) {
            if (old_meta[i] == 0xFF)
                continue;
            // Issue #4612: a deleted slot (tombstone) must NOT be
            // resurrected by the rehash -- the key is gone.
            if (old_meta[i] == ::aura::compiler::hash::kTombstoneSlot)
                continue;
            EvalValue kv{old_keys[i]};
            auto kh = eval_hash_key_hash(kv, sh);
            auto fp = old_meta[i];
            bool placed = false;
            for (std::uint64_t at = 0; at < new_cap; ++at) {
                auto idx = ((kh >> 1) + at) & (new_cap - 1);
                if (new_meta[idx] == 0xFF) {
                    new_meta[idx] = fp;
                    new_keys[idx] = old_keys[i];
                    new_vals[idx] = old_vals[i];
                    new_ht->size++;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                FlatHashTable::destroy(new_ht);
                return false;
            }
        }
        if (destroy_old)
            FlatHashTable::destroy(ht);
        htp = new_ht;
        return true;
    }

    // Insert key/val into ht (must not already contain key). Grows as needed.
    // destroy_old_on_grow: see flat_hash_grow_eval (false for published tables).
    [[nodiscard]] bool flat_hash_insert_eval(FlatHashTable*& htp, const EvalValue& key,
                                             const EvalValue& val,
                                             ::aura::compiler::StringPoolView sh,
                                             bool destroy_old_on_grow) {
        if (!htp)
            return false;
        auto kh = eval_hash_key_hash(key, sh);
        auto fp = eval_hash_fingerprint(kh);

        auto try_insert = [&]() -> bool {
            auto meta = htp->metadata();
            auto keys = htp->keys();
            auto vals = htp->values();
            auto cap = htp->capacity;
            // Issue #4612: reuse the first deleted slot of the chain
            // (the JIT runtime does the same) so a remove/insert cycle
            // does not grow the probe chain without bound.
            std::size_t first_tombstone = cap;
            for (std::uint64_t at = 0; at < cap; ++at) {
                auto idx = ((kh >> 1) + at) & (cap - 1);
                if (meta[idx] == ::aura::compiler::hash::kEmptySlot) {
                    auto use =
                        (first_tombstone < cap) ? first_tombstone : static_cast<std::size_t>(idx);
                    meta[use] = fp;
                    keys[use] = key.val;
                    vals[use] = val.val;
                    htp->size++;
                    return true;
                }
                if (meta[idx] == ::aura::compiler::hash::kTombstoneSlot && first_tombstone >= cap)
                    first_tombstone = static_cast<std::size_t>(idx);
            }
            return false;
        };

        // Grow at 0.7 load factor (same threshold as aura_hash_set / #2481).
        if (htp->size * 10 > htp->capacity * 7) {
            if (!flat_hash_grow_eval(htp, sh, destroy_old_on_grow))
                return false;
        }
        if (try_insert())
            return true;
        // Full table (collisions): force grow + retry once.
        if (!flat_hash_grow_eval(htp, sh, destroy_old_on_grow))
            return false;
        return try_insert();
    }

    // Issue #4489: key equality for every hash probe (ref / has-key? / set!
    // / remove!). Ints and strings as before; floats by eqv? (same double
    // bits, so 1.5 finds 1.5 while 0.0 and -0.0 stay distinct); pairs and
    // vectors structurally (Racket make-hash is equal?-based). Anything
    // else keeps identity. Probes are linear scans, so the bucket hash
    // only affects placement and can stay coarse for these key kinds.
    // Issue #4505: the equality walk is a byte/element oracle. When
    // tenant_ev is wired (every interpreter hash prim passes the executing
    // evaluator + its own prim name), the in-range stamp compare runs
    // before a pair car/cdr, vector element, or string byte is read; a
    // foreign mismatch reads as inequality. Soft/Off the helpers return
    // true before any stamp load. IsolationDeny rides the helper (op =
    // the probing prim).
    [[nodiscard]] bool eval_hash_keys_eq(const EvalValue& a, const EvalValue& b,
                                         ::aura::compiler::StringPoolView sh,
                                         const std::pmr::vector<Pair>* pairs = nullptr,
                                         const std::vector<std::vector<EvalValue>>* vh = nullptr,
                                         int depth = 0, Evaluator* tenant_ev = nullptr,
                                         std::string_view tenant_op = {}) noexcept {
        if (a.val == b.val)
            return true;
        if (is_int(a) && is_int(b))
            return as_int(a) == as_int(b);
        if (is_string(a) && is_string(b)) {
            auto ai = as_string_idx(a), bi = as_string_idx(b);
            if (tenant_ev &&
                (!tenant_ev->inrange_string_tenant_allows(ai, /*required_effects=*/0, tenant_op) ||
                 !tenant_ev->inrange_string_tenant_allows(bi, /*required_effects=*/0, tenant_op)))
                return false;
            return (ai < sh.size() && bi < sh.size()) && sh[ai] == sh[bi];
        }
        if (is_float(a) && is_float(b))
            return std::bit_cast<std::uint64_t>(as_float(a)) ==
                   std::bit_cast<std::uint64_t>(as_float(b));
        if (depth > 64)
            return false;
        if (pairs && is_pair(a) && is_pair(b)) {
            auto ai = as_pair_idx(a), bi = as_pair_idx(b);
            if (ai >= pairs->size() || bi >= pairs->size())
                return false;
            if (tenant_ev &&
                (!tenant_ev->inrange_slot_tenant_allows(ai, /*vector_heap=*/false,
                                                        /*required_effects=*/0, tenant_op) ||
                 !tenant_ev->inrange_slot_tenant_allows(bi, /*vector_heap=*/false,
                                                        /*required_effects=*/0, tenant_op)))
                return false;
            return eval_hash_keys_eq((*pairs)[ai].car, (*pairs)[bi].car, sh, pairs, vh, depth + 1,
                                     tenant_ev, tenant_op) &&
                   eval_hash_keys_eq((*pairs)[ai].cdr, (*pairs)[bi].cdr, sh, pairs, vh, depth + 1,
                                     tenant_ev, tenant_op);
        }
        if (vh && is_vector(a) && is_vector(b)) {
            auto ai = as_vector_idx(a), bi = as_vector_idx(b);
            if (ai >= vh->size() || bi >= vh->size())
                return false;
            if (tenant_ev &&
                (!tenant_ev->inrange_slot_tenant_allows(ai, /*vector_heap=*/true,
                                                        /*required_effects=*/0, tenant_op) ||
                 !tenant_ev->inrange_slot_tenant_allows(bi, /*vector_heap=*/true,
                                                        /*required_effects=*/0, tenant_op)))
                return false;
            const auto& va = (*vh)[ai];
            const auto& vb = (*vh)[bi];
            if (va.size() != vb.size())
                return false;
            for (std::size_t i = 0; i < va.size(); ++i)
                if (!eval_hash_keys_eq(va[i], vb[i], sh, pairs, vh, depth + 1, tenant_ev,
                                       tenant_op))
                    return false;
            return true;
        }
        return false;
    }

    // Issue #4612: the open-addressed probe. A key whose hash is kh
    // lives at ((kh >> 1) + at) & (cap - 1) -- the first free slot at
    // or after its home slot (linear probing) -- so a lookup walks THAT
    // chain instead of every slot of the table. The miss used to cost
    // O(capacity): hash-ref / hash-has-key? / hash-set! / hash-remove!
    // each scanned the whole table and compared every key.
    //
    // The chain ends at the first empty slot. A deleted slot
    // (kTombstoneSlot) is skipped, NOT treated as the end of the chain:
    // a removal used to punch a hole (0xFF) into the middle of a chain
    // and the key that had been displaced past the hole became
    // unreachable. hash-remove! now writes a tombstone instead.
    //
    // The key comparison is still eval_hash_keys_eq, so the equal?
    // semantics for list / vector keys (#4489) and the in-range tenant
    // stamp compare (#4505) are unchanged.
    //
    // Returns the slot index of the match, or kNoHashSlot for a miss.
    [[nodiscard]] std::size_t
    eval_hash_probe(const FlatHashTable* ht, std::uint64_t kh, const EvalValue& key,
                    ::aura::compiler::StringPoolView sh, const std::pmr::vector<Pair>* pairs,
                    const std::vector<std::vector<EvalValue>>* vh, Evaluator* tenant_ev,
                    std::string_view tenant_op) noexcept {
        const auto* meta = ht->metadata();
        const auto* keys = ht->keys();
        const auto cap = ht->capacity;
        for (std::uint64_t at = 0; at < cap; ++at) {
            const auto idx = ((kh >> 1) + at) & (cap - 1);
            const auto m = meta[idx];
            if (m == ::aura::compiler::hash::kEmptySlot)
                return kNoHashSlot; // empty slot: the chain ends here
            if (m == ::aura::compiler::hash::kTombstoneSlot)
                continue; // deleted slot: keep probing
            if (eval_hash_keys_eq(EvalValue{keys[idx]}, key, sh, pairs, vh, 0, tenant_ev,
                                  tenant_op))
                return static_cast<std::size_t>(idx);
        }
        return kNoHashSlot;
    }

    // ── Issue #4638: native persistent map (32-way bitmap HAMT) ─────
    // Nodes live in an append-only arena and are NEVER mutated: a write
    // builds a new path and shares every untouched subtree, so a version
    // is just a root index and forking a checkpoint is O(1).
    struct ImmNode {
        enum Kind : std::uint8_t { Leaf = 0, Branch = 1, Collision = 2 };
        std::uint8_t kind = Leaf;
        std::uint32_t bitmap = 0;
        std::uint32_t count = 0;
        std::uint64_t hash = 0;
        EvalValue key{};
        EvalValue value{};
        std::vector<std::uint32_t> children;
    };

    struct ImmMapHead {
        std::uint32_t root = 0;
        std::uint32_t count = 0;
        std::uint64_t stamp = 0;
    };

    struct ImmHashArena {
        std::mutex mtx;
        std::vector<ImmNode> nodes;
        std::vector<ImmMapHead> maps;

        ImmHashArena() {
            nodes.push_back(ImmNode{}); // 0 = null
            maps.push_back(ImmMapHead{});
        }
        [[nodiscard]] std::uint32_t new_map(std::uint64_t stamp) {
            maps.push_back(ImmMapHead{0, 0, stamp});
            return static_cast<std::uint32_t>(maps.size() - 1);
        }
        [[nodiscard]] std::uint32_t add_node(const ImmNode& n) {
            nodes.push_back(n);
            return static_cast<std::uint32_t>(nodes.size() - 1);
        }
        [[nodiscard]] std::uint32_t leaf(std::uint64_t h, const EvalValue& k, const EvalValue& v) {
            ImmNode n;
            n.kind = ImmNode::Leaf;
            n.hash = h;
            n.count = 1;
            n.key = k;
            n.value = v;
            return add_node(n);
        }
        [[nodiscard]] std::uint32_t branch(std::uint32_t bitmap, std::vector<std::uint32_t> kids,
                                           std::uint32_t count) {
            ImmNode n;
            n.kind = ImmNode::Branch;
            n.bitmap = bitmap;
            n.count = count;
            n.children = std::move(kids);
            return add_node(n);
        }
        [[nodiscard]] std::uint32_t collision(std::uint64_t h, std::vector<std::uint32_t> kids,
                                              std::uint32_t count) {
            ImmNode n;
            n.kind = ImmNode::Collision;
            n.hash = h;
            n.count = count;
            n.children = std::move(kids);
            return add_node(n);
        }
    };

    inline ImmHashArena& imm_arena() {
        static ImmHashArena arena;
        return arena;
    }

    [[nodiscard]] inline bool imm_key_eq(const EvalValue& a, const EvalValue& b,
                                         ::aura::compiler::StringPoolView sh) {
        if (a.val == b.val)
            return true;
        if (is_string(a) && is_string(b)) {
            const auto ia = as_string_idx(a);
            const auto ib = as_string_idx(b);
            if (ia < sh.size() && ib < sh.size())
                return sh[ia] == sh[ib];
        }
        return false;
    }

    constexpr int kImmBits = 5;
    constexpr std::uint64_t kImmMask = 31;

    [[nodiscard]] std::uint32_t imm_merge(ImmHashArena& ar, std::uint64_t ha, std::uint32_t a,
                                          std::uint64_t hb, std::uint32_t b, int shift) {
        const auto ia = static_cast<unsigned>((ha >> shift) & kImmMask);
        const auto ib = static_cast<unsigned>((hb >> shift) & kImmMask);
        if (ia == ib) {
            if (shift + kImmBits >= 64)
                return ar.collision(ha, {a, b}, 2);
            const auto child = imm_merge(ar, ha, a, hb, b, shift + kImmBits);
            return ar.branch(static_cast<std::uint32_t>(1u << ia), {child}, 2);
        }
        const auto bit = static_cast<std::uint32_t>((1u << ia) | (1u << ib));
        std::vector<std::uint32_t> kids;
        kids.reserve(2);
        if (ia < ib) {
            kids.push_back(a);
            kids.push_back(b);
        } else {
            kids.push_back(b);
            kids.push_back(a);
        }
        return ar.branch(bit, std::move(kids), 2);
    }

    [[nodiscard]] std::uint32_t imm_insert(ImmHashArena& ar, std::uint32_t node, std::uint64_t h,
                                           int shift, const EvalValue& k, const EvalValue& v,
                                           bool& added, ::aura::compiler::StringPoolView sh) {
        if (node == 0) {
            added = true;
            return ar.leaf(h, k, v);
        }
        const ImmNode n = ar.nodes[node]; // by value: the arena may reallocate
        if (n.kind == ImmNode::Leaf) {
            if (n.hash == h && imm_key_eq(n.key, k, sh)) {
                added = false;
                return ar.leaf(h, k, v);
            }
            added = true;
            const auto fresh = ar.leaf(h, k, v);
            if (n.hash == h)
                return ar.collision(h, {node, fresh}, 2);
            return imm_merge(ar, n.hash, node, h, fresh, shift);
        }
        if (n.kind == ImmNode::Collision) {
            std::vector<std::uint32_t> kids = n.children;
            for (std::size_t i = 0; i < kids.size(); ++i) {
                const ImmNode c = ar.nodes[kids[i]];
                if (imm_key_eq(c.key, k, sh)) {
                    added = false;
                    kids[i] = ar.leaf(h, k, v);
                    return ar.collision(h, std::move(kids), n.count);
                }
            }
            added = true;
            kids.push_back(ar.leaf(h, k, v));
            return ar.collision(h, std::move(kids), n.count + 1);
        }
        const auto idx = static_cast<unsigned>((h >> shift) & kImmMask);
        const auto bit = static_cast<std::uint32_t>(1u << idx);
        const auto slot =
            static_cast<std::size_t>(std::popcount(static_cast<unsigned>(n.bitmap & (bit - 1))));
        std::vector<std::uint32_t> kids = n.children;
        if ((n.bitmap & bit) == 0) {
            kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(slot), ar.leaf(h, k, v));
            added = true;
            return ar.branch(static_cast<std::uint32_t>(n.bitmap | bit), std::move(kids),
                             n.count + 1);
        }
        bool sub_added = false;
        kids[slot] = imm_insert(ar, kids[slot], h, shift + kImmBits, k, v, sub_added, sh);
        added = sub_added;
        return ar.branch(n.bitmap, std::move(kids), sub_added ? n.count + 1 : n.count);
    }

    [[nodiscard]] std::uint32_t imm_erase(ImmHashArena& ar, std::uint32_t node, std::uint64_t h,
                                          int shift, const EvalValue& k, bool& removed,
                                          ::aura::compiler::StringPoolView sh) {
        if (node == 0)
            return 0;
        const ImmNode n = ar.nodes[node];
        if (n.kind == ImmNode::Leaf) {
            if (n.hash == h && imm_key_eq(n.key, k, sh)) {
                removed = true;
                return 0;
            }
            return node;
        }
        if (n.kind == ImmNode::Collision) {
            std::vector<std::uint32_t> kids;
            for (auto cid : n.children) {
                const ImmNode c = ar.nodes[cid];
                if (!removed && c.hash == h && imm_key_eq(c.key, k, sh)) {
                    removed = true;
                    continue;
                }
                kids.push_back(cid);
            }
            if (!removed)
                return node;
            if (kids.empty())
                return 0;
            if (kids.size() == 1)
                return kids[0];
            return ar.collision(h, std::move(kids), static_cast<std::uint32_t>(kids.size()));
        }
        const auto idx = static_cast<unsigned>((h >> shift) & kImmMask);
        const auto bit = static_cast<std::uint32_t>(1u << idx);
        if ((n.bitmap & bit) == 0)
            return node;
        const auto slot =
            static_cast<std::size_t>(std::popcount(static_cast<unsigned>(n.bitmap & (bit - 1))));
        std::vector<std::uint32_t> kids = n.children;
        bool sub_removed = false;
        const auto child = imm_erase(ar, kids[slot], h, shift + kImmBits, k, sub_removed, sh);
        if (!sub_removed)
            return node;
        removed = true;
        if (child == 0) {
            kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(slot));
            if (kids.empty())
                return 0;
            if (kids.size() == 1)
                return kids[0];
            return ar.branch(static_cast<std::uint32_t>(n.bitmap & ~bit), std::move(kids),
                             n.count - 1);
        }
        kids[slot] = child;
        return ar.branch(n.bitmap, std::move(kids), n.count - 1);
    }

    [[nodiscard]] bool imm_lookup(ImmHashArena& ar, std::uint32_t node, std::uint64_t h,
                                  const EvalValue& k, ::aura::compiler::StringPoolView sh,
                                  EvalValue& out) {
        int shift = 0;
        while (node != 0) {
            const ImmNode n = ar.nodes[node];
            if (n.kind == ImmNode::Leaf) {
                if (n.hash == h && imm_key_eq(n.key, k, sh)) {
                    out = n.value;
                    return true;
                }
                return false;
            }
            if (n.kind == ImmNode::Collision) {
                for (auto cid : n.children) {
                    const ImmNode c = ar.nodes[cid];
                    if (c.hash == h && imm_key_eq(c.key, k, sh)) {
                        out = c.value;
                        return true;
                    }
                }
                return false;
            }
            const auto idx = static_cast<unsigned>((h >> shift) & kImmMask);
            const auto bit = static_cast<std::uint32_t>(1u << idx);
            if ((n.bitmap & bit) == 0)
                return false;
            const auto slot = static_cast<std::size_t>(
                std::popcount(static_cast<unsigned>(n.bitmap & (bit - 1))));
            node = n.children[slot];
            shift += kImmBits;
        }
        return false;
    }

    void imm_collect(ImmHashArena& ar, std::uint32_t node, std::vector<EvalValue>& out) {
        if (node == 0)
            return;
        const ImmNode n = ar.nodes[node];
        if (n.kind == ImmNode::Leaf) {
            out.push_back(n.key);
            return;
        }
        for (auto cid : n.children)
            imm_collect(ar, cid, out);
    }

} // namespace

void register_vector_and_hash_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                         ::aura::compiler::LockedStringHeap& string_heap,
                                         std::vector<EvalValue>& error_values,
                                         std::vector<std::vector<EvalValue>>& vector_heap,
                                         std::atomic<std::uint64_t>* primitive_error_counter,
                                         Evaluator& ev) {
    using aura::compiler::mutate_general;
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    register_prim(
        add, ev, "vector",
        [&string_heap, &error_values, primitive_error_counter, &ev](std::span<const EvalValue> a) {
            // Issue #2916: quota is inside mint_vector, and only when the
            // slot freelist is empty (#4459). Reuse does not grow the heap.
            std::vector<EvalValue> elems(a.begin(), a.end());
            const auto idx = ev.mint_vector(std::move(elems), /*check_quota=*/true);
            if (idx == Evaluator::kFileHeapMintDenied) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Vectors)),
                    primitive_error_counter);
            }
            return make_vector(idx);
        },
        pure_general(255, "(...vals) -> vector", "Construct a vector from arguments."));
    register_prim(
        add, ev, "vector-ref",
        [&ev, &vector_heap, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_vector(a[0])) {
                return make_primitive_error(string_heap, error_values, "vector-ref: not a vector",
                                            primitive_error_counter);
            }
            // Issue #4456: a float index must error, not abort in as_int.
            // 1.0 stays an error. make-vector's exact-float length (#2965)
            // does not apply to an index.
            if (!is_int(a[1])) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-ref: index must be an integer",
                                            primitive_error_counter);
            }
            auto idx = as_vector_idx(a[0]);
            auto pos = static_cast<std::size_t>(as_int(a[1]));
            // Issue #4298 / #4505: the owner compare runs BEFORE the #4497
            // out-of-range branch formats vector_heap[idx].size() into the
            // error — a foreign length is payload too. The in-range element
            // return below shares this single gate.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/true, /*required_effects=*/0,
                                               "vector-ref")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "vector-ref: cross-tenant vector slot read denied (#4298)",
                    primitive_error_counter);
            }
            if (idx >= vector_heap.size() || pos >= vector_heap[idx].size()) {
                // Issue #4497: name the index and length.
                const std::size_t len = idx < vector_heap.size() ? vector_heap[idx].size() : 0;
                return make_primitive_error(
                    string_heap, error_values,
                    std::format("vector-ref: index {} out of range for vector of length {}",
                                as_int(a[1]), len),
                    primitive_error_counter);
            }
            return vector_heap[idx][pos];
        },
        pure_general(2, "(vector int) -> any", "Element at index in a vector."));
    register_prim(
        add, ev, "vector-set!",
        [&ev, &vector_heap, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 3 || !is_vector(a[0])) {
                return make_primitive_error(string_heap, error_values, "vector-set!: not a vector",
                                            primitive_error_counter);
            }
            if (!is_int(a[1])) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-set!: index must be an integer",
                                            primitive_error_counter);
            }
            // Issue #3235: lock symmetry with set-car! / hash-set! (#1397/#1399).
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_vector_idx(a[0]);
            auto pos = static_cast<std::size_t>(as_int(a[1]));
            if (idx >= vector_heap.size() || pos >= vector_heap[idx].size()) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-set!: index out of bounds",
                                            primitive_error_counter);
            }
            // Issue #4298: in-range vector_heap had no owner compare. Prod face
            // audits a foreign mint and leaves the element unchanged.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/true,
                                               aura::compiler::security::kEffectMutate,
                                               "vector-set!")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "vector-set!: cross-tenant vector slot write denied (#4298)",
                    primitive_error_counter);
            }
            vector_heap[idx][pos] = a[2];
            return make_void();
        },
        mutate_general(3, "(vector int any) -> void", "Set vector element at index."));
    register_prim(
        add, ev, "vector-length",
        [&ev, &vector_heap, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty() || !is_vector(a[0])) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-length: not a vector", primitive_error_counter);
            }
            auto idx = as_vector_idx(a[0]);
            if (idx >= vector_heap.size()) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-length: corrupted vector",
                                            primitive_error_counter);
            }
            // Issue #4505: the size is foreign structure; same reader gate
            // as vector-ref before it is returned.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/true, /*required_effects=*/0,
                                               "vector-length")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "vector-length: cross-tenant vector slot read denied (#4505)",
                    primitive_error_counter);
            }
            return make_int(static_cast<std::int64_t>(vector_heap[idx].size()));
        },
        pure_general(1, "(vector) -> int", "Number of elements in a vector."));
    register_prim(
        add, ev, "vector?",
        [](std::span<const EvalValue> a) {
            if (a.empty())
                return make_bool(false);
            return make_bool(is_vector(a[0]));
        },
        pure_general(1, "(any) -> bool", "True if value is a vector."));
    // Issue #2965: accept exact integer-valued floats as length (floor/round
    // results); never reinterpret float bits via as_int → max_size() noise.
    register_prim(
        add, ev, "make-vector",
        [&string_heap, &error_values, primitive_error_counter, &ev](std::span<const EvalValue> a) {
            std::size_t n = 0;
            if (!a.empty()) {
                std::int64_t len = 0;
                if (is_int(a[0])) {
                    len = as_int(a[0]);
                } else if (is_float(a[0])) {
                    const double d = as_float(a[0]);
                    constexpr double kMaxI64 =
                        static_cast<double>(std::numeric_limits<std::int64_t>::max());
                    // Exact integer-valued, finite, non-negative, in int64 range.
                    if (!(d == d) || d < 0.0 || d >= kMaxI64 || d != std::trunc(d)) {
                        return make_primitive_error(
                            string_heap, error_values,
                            std::string(
                                "make-vector: length must be a non-negative integer, got ") +
                                std::to_string(d) + " (float)",
                            primitive_error_counter);
                    }
                    len = static_cast<std::int64_t>(d);
                } else {
                    return make_primitive_error(
                        string_heap, error_values,
                        "make-vector: length must be a non-negative integer",
                        primitive_error_counter);
                }
                if (len < 0) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string("make-vector: length must be a non-negative integer, got ") +
                            std::to_string(len),
                        primitive_error_counter);
                }
                n = static_cast<std::size_t>(len);
                // Pre-check max_size so callers never see the raw std::vector message.
                if (n > std::vector<EvalValue>{}.max_size()) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string("make-vector: length exceeds maximum vector size, got ") +
                            std::to_string(len),
                        primitive_error_counter);
                }
            }
            // Issue #2916 / #4459: quota before the buffer exists, and only
            // when no dead slot can take the fill. mint_vector_fill assigns
            // into a cleared slot so capacity stays put.
            EvalValue init = a.size() > 1 ? a[1] : make_void();
            const auto idx = ev.mint_vector_fill(n, init, /*check_quota=*/true);
            if (idx == Evaluator::kFileHeapMintDenied) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Vectors)),
                    primitive_error_counter);
            }
            return make_vector(idx);
        },
        pure_general(2, "(int any) -> vector", "Allocate a vector filled with a value."));
    register_prim(
        add, ev, "list->vector",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            // Issue #4613: hold the per-heap lock across the walk. The
            // pairs are read from pairs_ and the result is minted into
            // vector_heap_; a reclaim that ran in between could poison
            // the pair slots the walk is reading (the same shape the
            // list constructors use).
            Evaluator::ListCtorLockHold hold(ev);
            std::vector<EvalValue> elems;
            if (!a.empty()) {
                auto v = a[0];
                while (is_pair(v)) {
                    auto idx = as_pair_idx(v);
                    if (idx >= pairs.size())
                        break;
                    // Issue #4505: deny before the car is copied into the
                    // new vector's element storage.
                    if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                       /*required_effects=*/0, "list->vector")) {
                        return make_primitive_error(
                            string_heap, error_values,
                            "list->vector: cross-tenant pair slot read denied (#4505)",
                            primitive_error_counter);
                    }
                    elems.push_back(pairs[idx].car);
                    v = pairs[idx].cdr;
                }
            }
            // Issue #4613: mint the result vector through the
            // evaluator's vector heap. A raw vector_heap.push_back grew
            // the heap by one slot - and one element buffer - per call
            // and never consulted the file-mode reclaim's vector free
            // list, so a loop that converted a list to a vector leaked
            // the whole buffer every iteration.
            const auto idx = ev.mint_vector(std::move(elems), /*check_quota=*/true);
            if (idx == Evaluator::kFileHeapMintDenied) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Vectors)),
                    primitive_error_counter);
            }
            return make_vector(idx);
        },
        pure_general(1, "(list) -> vector", "Convert a list to a vector."));
    register_prim(
        add, ev, "vector->list",
        [&ev, &vector_heap, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty() || !is_vector(a[0])) {
                return make_primitive_error(string_heap, error_values, "vector->list: not a vector",
                                            primitive_error_counter);
            }
            auto idx = as_vector_idx(a[0]);
            if (idx >= vector_heap.size()) {
                return make_primitive_error(string_heap, error_values,
                                            "vector->list: corrupted vector",
                                            primitive_error_counter);
            }
            // Issue #4505: no foreign element is copied into the new pairs.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/true, /*required_effects=*/0,
                                               "vector->list")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "vector->list: cross-tenant vector slot read denied (#4505)",
                    primitive_error_counter);
            }
            // Issue #4613: mint the pairs through the evaluator's pair
            // heap. A raw pairs.push_back grew the heap by one slot per
            // element and never consulted the file-mode reclaim's pair
            // free list, so a loop that converted a vector to a list
            // leaked every pair it minted (and the mint never counted
            // toward the reclaim window, so the reclaim never ran).
            // The ListCtorLockHold keeps the reclaim out while the list
            // is half-built: the pairs are not reachable from any root
            // until the list is returned.
            Evaluator::ListCtorLockHold hold(ev);
            std::vector<EvalValue> elems(vector_heap[idx].begin(), vector_heap[idx].end());
            EvalValue result = make_void();
            for (auto it = elems.rbegin(); it != elems.rend(); ++it) {
                const auto pid = ev.mint_pair(*it, result, /*check_quota=*/true);
                if (pid == Evaluator::kFileHeapMintDenied) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                        primitive_error_counter);
                }
                result = make_pair(pid);
            }
            ev.bump_pair_alloc_count_n(static_cast<std::uint64_t>(elems.size()));
            return result;
        },
        pure_general(1, "(vector) -> list", "Convert a vector to a list."));

    // Issue #4614: native vector-copy. The stdlib loop it replaces
    // (make-vector + one vector-set! per slot) cost 564 us for a 64-slot
    // vector — 80x slower than a native element copy. This is a single
    // O(n) element copy into a freshly minted vector (5.75 us on the
    // release build of this tree, vs 425 us for the interpreted loop --
    // a 74x win; the AC is <= 10 us for 64 slots).
    register_prim(
        add, ev, "vector-copy",
        [&ev, &vector_heap, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty() || !is_vector(a[0])) {
                return make_primitive_error(string_heap, error_values, "vector-copy: not a vector",
                                            primitive_error_counter);
            }
            auto idx = as_vector_idx(a[0]);
            if (idx >= vector_heap.size()) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-copy: corrupted vector",
                                            primitive_error_counter);
            }
            // Issue #4298 / #4505: the source slots are payload — the owner
            // compare runs before any element is copied out.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/true, /*required_effects=*/0,
                                               "vector-copy")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "vector-copy: cross-tenant vector slot read denied (#4298)",
                    primitive_error_counter);
            }
            const auto len = vector_heap[idx].size();
            // start / end are optional; out-of-range clamps like the stdlib
            // loop did (a reversed range yields an empty vector).
            std::size_t start = 0;
            std::size_t end = len;
            if (a.size() > 1) {
                if (!is_int(a[1])) {
                    return make_primitive_error(string_heap, error_values,
                                                "vector-copy: start must be an integer",
                                                primitive_error_counter);
                }
                const auto s = as_int(a[1]);
                start = s < 0 ? 0 : static_cast<std::size_t>(s);
            }
            if (a.size() > 2) {
                if (!is_int(a[2])) {
                    return make_primitive_error(string_heap, error_values,
                                                "vector-copy: end must be an integer",
                                                primitive_error_counter);
                }
                const auto e = as_int(a[2]);
                end = e < 0 ? 0 : static_cast<std::size_t>(e);
            }
            if (end > len)
                end = len;
            if (start > end)
                start = end;
            // Copy the slice out BEFORE the mint: mint_vector may reuse a
            // dead slot and reallocate the heap's element storage.
            std::vector<EvalValue> elems;
            elems.reserve(end - start);
            for (std::size_t i = start; i < end; ++i)
                elems.push_back(vector_heap[idx][i]);
            const auto out = ev.mint_vector(std::move(elems), /*check_quota=*/true);
            if (out == Evaluator::kFileHeapMintDenied) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Vectors)),
                    primitive_error_counter);
            }
            return make_vector(out);
        },
        pure_general(255, "(vector [int] [int]) -> vector",
                     "Copy a vector (optionally the start..end slice) into a new vector."));
    // Issue #4614: native vector-fill!. Same contract as the stdlib loop it
    // replaces: every slot takes the value and the vector itself is returned.
    register_prim(
        add, ev, "vector-fill!",
        [&ev, &vector_heap, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_vector(a[0])) {
                return make_primitive_error(string_heap, error_values, "vector-fill!: not a vector",
                                            primitive_error_counter);
            }
            // Issue #3235: lock symmetry with set-car! / hash-set!.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_vector_idx(a[0]);
            if (idx >= vector_heap.size()) {
                return make_primitive_error(string_heap, error_values,
                                            "vector-fill!: corrupted vector",
                                            primitive_error_counter);
            }
            // Issue #4298: the in-range write face compares the owner stamp
            // before the store (same gate as vector-set!).
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/true,
                                               aura::compiler::security::kEffectMutate,
                                               "vector-fill!")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "vector-fill!: cross-tenant vector slot write denied (#4298)",
                    primitive_error_counter);
            }
            for (auto& slot : vector_heap[idx])
                slot = a[1];
            return a[0];
        },
        mutate_general(2, "(vector any) -> vector", "Fill every slot of a vector with a value."));

    register_prim(
        add, ev, "hash",
        [&ev, &string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
            // Issue #2652: lock string_heap reads + g_hash_tables push.
            // Issue #2654: grow past fixed capacity 8 so 16+ k/v pairs are not
            // silently dropped (same class as #2481 json-parse). Pre-size so
            // common N≤16 literals need no grow.
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            const std::size_t npairs = a.size() / 2;
            // Min capacity 32 so sequential hash-set! of 16 keys (AC2 / denseness
            // stats) never needs an immediate grow under multi-fiber pressure.
            std::uint64_t init_cap = 32;
            while (npairs * 10 > init_cap * 7)
                init_cap *= 2;
            auto* ht = FlatHashTable::create(init_cap);
            if (!ht)
                return make_void();
            for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                // Issue #4631: the empty string is an ORDINARY key. The #2652
                // guard treated "" as a corruption sentinel (make_string(0))
                // and SKIPPED the pair, so (hash "" v) lost its entry with no
                // error at all. Only a genuinely corrupt string INDEX is
                // invalid now, and it RAISES instead of silently dropping
                // the pair.
                if (is_string(a[i])) {
                    auto ki = as_string_idx(a[i]);
                    if (ki >= string_heap.size()) {
                        FlatHashTable::destroy(ht);
                        return make_primitive_error(string_heap, error_values,
                                                    "hash: corrupt string key (index out of range)",
                                                    primitive_error_counter);
                    }
                }
                // Unpublished table: safe to free old blocks on grow.
                if (!flat_hash_insert_eval(ht, a[i], a[i + 1], string_heap,
                                           /*destroy_old_on_grow=*/true)) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            // Issue #4613: publish the table through the evaluator's
            // hash free list. A table the file-mode reclaim proved
            // unreachable was freed and its slot released, so the next
            // (hash) mint reuses that slot instead of leaking another
            // table block (~560 bytes) per iteration. With no released
            // slot the table is published into a fresh slot, exactly as
            // before. The mint counts toward the reclaim window too:
            // without that a hash-only loop never reached the trigger.
            const auto hidx = ev.mint_hash_table(ht);
            return make_hash(hidx);
        },
        pure_general(255, "(...kvs) -> hash", "Construct a hash from key/value pairs."));
    // Issue #4614: native hash-copy — the copy-on-write primitive behind the
    // stdlib hash-set / hash-remove wrappers, which now return a FRESH
    // table instead of mutating the argument in place (the aliasing trap
    // that broke checkpoint / replay / forked-worldline code).
    register_prim(
        add, ev, "hash-copy",
        [&ev, &string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty() || !is_hash(a[0])) {
                return make_primitive_error(string_heap, error_values, "hash-copy: not a hash",
                                            primitive_error_counter);
            }
            // Issue #2652: table reads are serialized against the
            // process-wide g_hash_tables vector.
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx]) {
                return make_primitive_error(string_heap, error_values, "hash-copy: not a hash",
                                            primitive_error_counter);
            }
            // Issue #4110 / #4296: armed + foreign stamp -> deny BEFORE the
            // table is read (same gate the other table readers use; the
            // caller/face are this executing evaluator).
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-copy"))
                return make_primitive_error(string_heap, error_values,
                                            "hash-copy: cross-tenant hash read denied (#4110)",
                                            primitive_error_counter);
            auto* ht = g_hash_tables[hidx];
            auto* copy = FlatHashTable::create(ht->capacity);
            if (!copy)
                return make_primitive_error(string_heap, error_values, "hash-copy: out of memory",
                                            primitive_error_counter);
            // Slot-for-slot copy: the new table owns its own metadata /
            // keys / values arrays, so the two tables are independent.
            std::memcpy(copy->metadata(), ht->metadata(), ht->capacity);
            std::memcpy(copy->keys(), ht->keys(), ht->capacity * sizeof(std::int64_t));
            std::memcpy(copy->values(), ht->values(), ht->capacity * sizeof(std::int64_t));
            copy->size = ht->size;
            // Issue #4093 / #4296: stamp the new table with the executing
            // evaluator's principal (the inline stamp shape the hash prim
            // uses) and publish it.
            auto newidx = g_hash_tables.size();
            g_hash_tables.push_back(copy);
            if (g_hash_tenants.size() < g_hash_tables.size())
                g_hash_tenants.resize(g_hash_tables.size(), 0);
            g_hash_tenants[newidx] = ev.capability_tenant_id();
            return make_hash(newidx);
        },
        pure_general(1, "(hash) -> hash", "Copy a hash into a new, independent table."));
    register_prim(
        add, ev, "hash-ref",
        [&ev, &string_heap, &pairs, &vector_heap](std::span<const EvalValue> a) {
            // Issue #2569: honor optional 3rd-arg default when key is missing
            // (Racket-compatible). 2-arg miss still returns void.
            if (a.size() < 2 || !is_hash(a[0]))
                return a.size() >= 3 ? a[2] : make_void();
            // Issue #2652: serialize hash probe + string key compare under multi-fiber
            // stats-bump (empty key / wrong counters when g_hash_tables races).
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return a.size() >= 3 ? a[2] : make_void();
            // Issue #4110 / #4296: same armed-face compare the JIT checked
            // seams run, but the caller and face are this evaluator's
            // (not the JIT owner). Armed + foreign stamp reads as missing
            // (3rd-arg default / void, table untouched); the deny fires
            // through check_workspace_isolation. Unarmed (face == 0):
            // today's probe, no tenant load.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-ref"))
                return a.size() >= 3 ? a[2] : make_void();
            auto* ht = g_hash_tables[hidx];
            // Issue #4612: probe by hash (open addressing) instead of
            // scanning every slot -- a miss used to cost O(capacity).
            const auto kh = eval_hash_key_hash(a[1], string_heap);
            const auto slot =
                eval_hash_probe(ht, kh, a[1], string_heap, &pairs, &vector_heap, &ev, "hash-ref");
            if (slot != kNoHashSlot)
                return EvalValue{ht->values()[slot]};
            return a.size() >= 3 ? a[2] : make_void();
        },
        pure_general(2, "(hash any) -> any", "Lookup a key in a hash."));
    register_prim(
        add, ev, "hash-has-key?",
        [&ev, &string_heap, &pairs, &vector_heap](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_hash(a[0]))
                return make_bool(false);
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_bool(false);
            // Issue #4110 / #4296: armed + foreign stamp → #f without the
            // probe. Caller/face are the executing evaluator.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-has-key?"))
                return make_bool(false);
            auto* ht = g_hash_tables[hidx];
            // Issue #4612: probe by hash (open addressing) -- the miss
            // used to scan every slot of the table.
            const auto kh = eval_hash_key_hash(a[1], string_heap);
            const auto slot = eval_hash_probe(ht, kh, a[1], string_heap, &pairs, &vector_heap, &ev,
                                              "hash-has-key?");
            return make_bool(slot != kNoHashSlot);
        },
        pure_general(2, "(hash any) -> bool", "True if hash contains the key."));
    register_prim(
        add, ev, "hash-set!",
        [&ev, &string_heap, &pairs, &vector_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 3 || !is_hash(a[0]))
                return make_void();
            // Issue #2652: aether:stats-bump! path — concurrent hash-set! + string
            // key compare must not race g_hash_tables or string_heap_ (empty ""
            // keys / zero counters under overnight fanout).
            // Issue #2654: grow when full / load > 0.7 — never silent-drop keys.
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_void();
            // Issue #4631: the empty string is an ORDINARY key. The #2652
            // guard REFUSED "" as if it were a corruption sentinel
            // (make_string(0)), so the write was dropped with no error at all
            // -- a silent data loss (a missing field, the root namespace, a
            // default tenant). Only a genuinely corrupt string INDEX is
            // invalid now, and it RAISES instead of no-op'ing. Checked before
            // the tenant gate: a corrupt key is a caller-side fault, not a
            // tenancy decision.
            if (is_string(a[1])) {
                auto bi = as_string_idx(a[1]);
                if (bi >= string_heap.size()) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "hash-set!: corrupt string key (index out of range)",
                        primitive_error_counter);
                }
            }
            // Issue #4110 / #4296: armed + foreign stamp → void WITHOUT
            // storing. Caller/face are the executing evaluator; the deny
            // fires through check_workspace_isolation. Unarmed: no load.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-set!"))
                return make_void();
            auto* ht = g_hash_tables[hidx];
            // Issue #4612: probe by hash (open addressing) -- finding the
            // existing key used to scan every slot of the table.
            const auto kh = eval_hash_key_hash(a[1], string_heap);
            const auto slot =
                eval_hash_probe(ht, kh, a[1], string_heap, &pairs, &vector_heap, &ev, "hash-set!");
            if (slot != kNoHashSlot) {
                ht->values()[slot] = a[2].val;
                return make_void();
            }
            // New key: insert with grow. Published table — do NOT free old
            // blocks (JIT / concurrent readers may still hold the prior
            // pointer under workspace lock, not hash_tables_mutex_). Always
            // publish the possibly-new pointer.
            (void)flat_hash_insert_eval(ht, a[1], a[2], string_heap,
                                        /*destroy_old_on_grow=*/false);
            g_hash_tables[hidx] = ht;
            return make_void();
        },
        mutate_general(3, "(hash any any) -> void", "Insert or replace a hash entry."));
    register_prim(
        add, ev, "hash-length",
        [&ev](std::span<const EvalValue> a) {
            if (a.empty() || !is_hash(a[0]))
                return make_int(0);
            std::lock_guard hlock(ev.hash_tables_mutex());
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_int(0);
            // Issue #4429: metadata is still a table read. Armed foreign
            // or unstamped returns 0; Soft/Off does not load the stamp.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-length"))
                return make_int(0);
            return make_int(static_cast<std::int64_t>(g_hash_tables[hidx]->size));
        },
        pure_general(1, "(hash) -> int", "Number of keys in a hash."));
    register_prim(
        add, ev, "hash-keys",
        [&ev, &pairs](std::span<const EvalValue> a) {
            if (a.empty() || !is_hash(a[0]))
                return make_void();
            // Issue #2652: lock hash store + pairs_ growth.
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_void();
            // Issue #4429: same armed-face compare as hash-ref, before any
            // key is copied into pairs_.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-keys"))
                return make_void();
            auto* ht = g_hash_tables[hidx];
            auto meta = ht->metadata();
            auto keys = ht->keys();
            // Issue #1398 contract: hash-keys returns BY VALUE — each key
            // is copied into a freshly-constructed EvalValue and cons'd into
            // a brand-new pair list. The returned list is independent of the
            // underlying hash table's internal storage. Caller can safely
            // invoke (hash-set!) / (hash-remove!) / (hash-clear!) etc. on
            // the same hash after this call without invalidating the returned
            // list. Verified by tests/test_hash_iter_invalidation.cpp.
            EvalValue result = make_void();
            for (std::size_t i = ht->capacity; i > 0; --i) {
                // Issue #4612: a deleted slot (tombstone) is not a key.
                if (::aura::compiler::hash::hash_slot_is_live(meta[i - 1])) {
                    auto pid = pairs.size();
                    pairs.push_back({EvalValue{keys[i - 1]}, result});
                    ev.note_inrange_pair_mint(pid); // Issue #4431
                    result = make_pair(pid);
                }
            }
            return result;
        },
        pure_general(1, "(hash) -> list", "List of keys in a hash."));
    register_prim(
        add, ev, "hash-values",
        [&ev, &pairs](std::span<const EvalValue> a) {
            if (a.empty() || !is_hash(a[0]))
                return make_void();
            std::lock_guard hlock(ev.hash_tables_mutex());
            std::lock_guard slock(ev.alloc_storage_lock_);
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_void();
            // Issue #4429: deny before any value is copied into pairs_.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-values"))
                return make_void();
            auto* ht = g_hash_tables[hidx];
            auto meta = ht->metadata();
            auto vals = ht->values();
            // Issue #1398 contract: hash-values returns BY VALUE — each
            // value is copied into a freshly-constructed EvalValue and
            // cons'd into a brand-new pair list. Independent of the hash
            // table's internal storage; safe to mutate the hash afterward.
            // Verified by tests/test_hash_iter_invalidation.cpp.
            EvalValue result = make_void();
            for (std::size_t i = ht->capacity; i > 0; --i) {
                // Issue #4612: a deleted slot (tombstone) is not a value.
                if (::aura::compiler::hash::hash_slot_is_live(meta[i - 1])) {
                    auto pid = pairs.size();
                    pairs.push_back({EvalValue{vals[i - 1]}, result});
                    ev.note_inrange_pair_mint(pid); // Issue #4431
                    result = make_pair(pid);
                }
            }
            return result;
        },
        pure_general(1, "(hash) -> list", "List of values in a hash."));
    register_prim(
        add, ev, "hash?",
        [](std::span<const EvalValue> a) {
            if (a.empty())
                return make_bool(false);
            return make_bool(is_hash(a[0]));
        },
        pure_general(1, "(any) -> bool", "True if value is a hash."));

    register_prim(
        add, ev, "immutable-hash",
        [](std::span<const EvalValue>) {
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            return make_imm_hash(ar.new_map(0));
        },
        pure_general(0, "() -> immutable-hash", "Empty persistent map."));
    register_prim(
        add, ev, "immutable-hash?",
        [](std::span<const EvalValue> a) { return make_bool(!a.empty() && is_imm_hash(a[0])); },
        pure_general(1, "(any) -> bool", "True if the value is a persistent map."));
    register_prim(
        add, ev, "ihash-set",
        [&string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 3 || !is_imm_hash(a[0]))
                return make_primitive_error(string_heap, error_values,
                                            "ihash-set: not an immutable hash",
                                            primitive_error_counter);
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            const auto mi = as_imm_hash_idx(a[0]);
            if (mi == 0 || mi >= ar.maps.size())
                return make_primitive_error(string_heap, error_values,
                                            "ihash-set: stale map handle", primitive_error_counter);
            const auto head = ar.maps[mi];
            bool added = false;
            const auto h = eval_hash_key_hash(a[1], string_heap);
            const auto root = imm_insert(ar, head.root, h, 0, a[1], a[2], added, string_heap);
            ar.maps.push_back(ImmMapHead{root, head.count + (added ? 1u : 0u), head.stamp});
            return make_imm_hash(static_cast<std::uint64_t>(ar.maps.size() - 1));
        },
        pure_general(3, "(immutable-hash any any) -> immutable-hash",
                     "Persistent insert: returns a new map, shares untouched nodes."));
    register_prim(
        add, ev, "ihash-remove",
        [&string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_imm_hash(a[0]))
                return make_primitive_error(string_heap, error_values,
                                            "ihash-remove: not an immutable hash",
                                            primitive_error_counter);
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            const auto mi = as_imm_hash_idx(a[0]);
            if (mi == 0 || mi >= ar.maps.size())
                return make_primitive_error(string_heap, error_values,
                                            "ihash-remove: stale map handle",
                                            primitive_error_counter);
            const auto head = ar.maps[mi];
            bool removed = false;
            const auto h = eval_hash_key_hash(a[1], string_heap);
            const auto root = imm_erase(ar, head.root, h, 0, a[1], removed, string_heap);
            ar.maps.push_back(ImmMapHead{
                root, removed && head.count > 0 ? head.count - 1 : head.count, head.stamp});
            return make_imm_hash(static_cast<std::uint64_t>(ar.maps.size() - 1));
        },
        pure_general(2, "(immutable-hash any) -> immutable-hash",
                     "Persistent remove: returns a new map, shares untouched nodes."));
    register_prim(
        add, ev, "ihash-ref",
        [&string_heap](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_imm_hash(a[0]))
                return a.size() >= 3 ? a[2] : make_void();
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            const auto mi = as_imm_hash_idx(a[0]);
            if (mi == 0 || mi >= ar.maps.size())
                return a.size() >= 3 ? a[2] : make_void();
            const auto head = ar.maps[mi];
            EvalValue out{};
            const auto h = eval_hash_key_hash(a[1], string_heap);
            if (imm_lookup(ar, head.root, h, a[1], string_heap, out))
                return out;
            return a.size() >= 3 ? a[2] : make_void();
        },
        pure_general(2, "(immutable-hash any [any]) -> any", "Persistent lookup."));
    register_prim(
        add, ev, "ihash-has-key?",
        [&string_heap](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_imm_hash(a[0]))
                return make_bool(false);
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            const auto mi = as_imm_hash_idx(a[0]);
            if (mi == 0 || mi >= ar.maps.size())
                return make_bool(false);
            EvalValue out{};
            const auto h = eval_hash_key_hash(a[1], string_heap);
            return make_bool(imm_lookup(ar, ar.maps[mi].root, h, a[1], string_heap, out));
        },
        pure_general(2, "(immutable-hash any) -> bool", "True if the key is present."));
    register_prim(
        add, ev, "ihash-count",
        [](std::span<const EvalValue> a) {
            if (a.empty() || !is_imm_hash(a[0]))
                return make_int(0);
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            const auto mi = as_imm_hash_idx(a[0]);
            if (mi == 0 || mi >= ar.maps.size())
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ar.maps[mi].count));
        },
        pure_general(1, "(immutable-hash) -> int", "Number of entries (O(1))."));
    register_prim(
        add, ev, "ihash-keys",
        [&pairs](std::span<const EvalValue> a) {
            if (a.empty() || !is_imm_hash(a[0]))
                return make_void();
            auto& ar = imm_arena();
            std::lock_guard lk(ar.mtx);
            const auto mi = as_imm_hash_idx(a[0]);
            if (mi == 0 || mi >= ar.maps.size())
                return make_void();
            std::vector<EvalValue> keys;
            imm_collect(ar, ar.maps[mi].root, keys);
            EvalValue result = make_void();
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
                auto pid = pairs.size();
                pairs.push_back({*it, result});
                result = make_pair(pid);
            }
            return result;
        },
        pure_general(1, "(immutable-hash) -> list", "Keys of a persistent map."));

    // Issue #278 follow-up #3: (hash->alist hash) — return the
    // hash as a list of (key . value) pairs in arbitrary order.
    // Pairs Aura's existing hash API (which has hash-ref /
    // hash-set / hash-keys / hash-values but no enumeration
    // primitive) with a way to iterate. Common use case: AI
    // agent wants to render a mutation-log:summary hash as a
    // string by iterating the alist.
    register_prim(
        add, ev, "hash->alist",
        [&ev, &pairs](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_hash(a[0]))
                return make_void();
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_void();
            // Issue #4429: deny before any (key . value) cons is built.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash->alist"))
                return make_void();
            auto* ht = g_hash_tables[hidx];
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            EvalValue result = make_void();
            // Walk in reverse so the resulting alist is in
            // insertion / hash order.
            // Issue #1398 contract: hash->alist returns BY VALUE — each
            // (key . value) cons cell is built from freshly-constructed
            // EvalValue copies, so the returned alist is independent of the
            // hash table's internal storage. Safe to mutate the hash
            // afterward. Verified by tests/test_hash_iter_invalidation.cpp.
            for (std::size_t i = ht->capacity; i > 0; --i) {
                // Issue #4612: a deleted slot (tombstone) is not a key.
                if (::aura::compiler::hash::hash_slot_is_live(meta[i - 1])) {
                    // Build (key . value) cons cell.
                    auto kv_pid = pairs.size();
                    pairs.push_back({EvalValue{keys[i - 1]}, EvalValue{vals[i - 1]}});
                    ev.note_inrange_pair_mint(kv_pid); // Issue #4431
                    // Prepend to the result list.
                    auto list_pid = pairs.size();
                    pairs.push_back({make_pair(kv_pid), result});
                    ev.note_inrange_pair_mint(list_pid); // Issue #4431
                    result = make_pair(list_pid);
                }
            }
            return result;
        },
        pure_general(1, "(hash) -> list", "Convert a hash to an association list."));
    register_prim(
        add, ev, "hash-remove!",
        [&ev, &string_heap, &pairs, &vector_heap](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_hash(a[0]))
                return make_void();
            auto hidx = as_hash_idx(a[0]);
            if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                return make_void();
            // Issue #4110 / #4296: armed + foreign stamp → #f WITHOUT the
            // tombstone write. Caller/face are the executing evaluator.
            if (aura_hash_gate_checked(hidx, ev.capability_tenant_id(), ev.effect_sandbox_mode(),
                                       "hash-remove!"))
                return make_bool(false);
            auto* ht = g_hash_tables[hidx];
            // Issue #4612: probe by hash (open addressing) -- finding the
            // key used to scan every slot of the table.
            const auto kh = eval_hash_key_hash(a[1], string_heap);
            const auto slot = eval_hash_probe(ht, kh, a[1], string_heap, &pairs, &vector_heap, &ev,
                                              "hash-remove!");
            if (slot == kNoHashSlot)
                return make_bool(false);
            // Issue #4612: tombstone, NOT an empty slot. An empty slot
            // ends the probe chain and would hide the keys that were
            // displaced past the hole.
            ht->metadata()[slot] = ::aura::compiler::hash::kTombstoneSlot;
            ht->size--;
            return make_bool(true);
        },
        mutate_general(2, "(hash any) -> bool", "Remove a key from a hash."));
}

} // namespace aura::compiler::primitives_detail