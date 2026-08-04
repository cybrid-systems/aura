// evaluator_primitives_vector.cpp — P0 step 5: vector/hash primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "hash_meta.h" // FNV constants (#901)

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
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

namespace {

    // Issue #2654: language hash key hash (FNV for strings, mix for ints).
    // Keep in sync with hash / hash-set! / grow rehash placement.
    [[nodiscard]] std::uint64_t
    eval_hash_key_hash(const EvalValue& k, const std::pmr::vector<std::string>& sh) noexcept {
        if (is_int(k))
            return static_cast<std::uint64_t>(as_int(k)) * 0x9e3779b97f4a7c15ull;
        if (is_string(k)) {
            auto i = as_string_idx(k);
            if (i < sh.size()) {
                auto& s = sh[i];
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : s)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                return h;
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
    [[nodiscard]] bool flat_hash_grow_eval(FlatHashTable*& htp,
                                           const std::pmr::vector<std::string>& sh,
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
                                             const std::pmr::vector<std::string>& sh,
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
            for (std::uint64_t at = 0; at < cap; ++at) {
                auto idx = ((kh >> 1) + at) & (cap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key.val;
                    vals[idx] = val.val;
                    htp->size++;
                    return true;
                }
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

    [[nodiscard]] bool eval_hash_keys_eq(const EvalValue& a, const EvalValue& b,
                                         const std::pmr::vector<std::string>& sh) noexcept {
        if (is_int(a) && is_int(b))
            return as_int(a) == as_int(b);
        if (is_string(a) && is_string(b)) {
            auto ai = as_string_idx(a), bi = as_string_idx(b);
            return (ai < sh.size() && bi < sh.size()) && sh[ai] == sh[bi];
        }
        return a.val == b.val;
    }

} // namespace

void register_vector_and_hash_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         std::vector<EvalValue>& error_values,
                                         std::vector<std::vector<EvalValue>>& vector_heap,
                                         std::atomic<std::uint64_t>* primitive_error_counter,
                                         Evaluator& ev) {
    add("vector", [&vector_heap](std::span<const EvalValue> a) {
        std::vector<EvalValue> elems(a.begin(), a.end());
        auto idx = vector_heap.size();
        vector_heap.push_back(std::move(elems));
        return make_vector(idx);
    });
    add("vector-ref", [&vector_heap, &string_heap, &error_values,
                       primitive_error_counter](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_vector(a[0])) {
            return make_primitive_error(string_heap, error_values, "vector-ref: not a vector",
                                        primitive_error_counter);
        }
        auto idx = as_vector_idx(a[0]);
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (idx >= vector_heap.size() || pos >= vector_heap[idx].size()) {
            return make_primitive_error(string_heap, error_values,
                                        "vector-ref: index out of bounds", primitive_error_counter);
        }
        return vector_heap[idx][pos];
    });
    add("vector-set!", [&vector_heap, &string_heap, &error_values,
                        primitive_error_counter](std::span<const EvalValue> a) {
        if (a.size() < 3 || !is_vector(a[0])) {
            return make_primitive_error(string_heap, error_values, "vector-set!: not a vector",
                                        primitive_error_counter);
        }
        auto idx = as_vector_idx(a[0]);
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (idx >= vector_heap.size() || pos >= vector_heap[idx].size()) {
            return make_primitive_error(string_heap, error_values,
                                        "vector-set!: index out of bounds",
                                        primitive_error_counter);
        }
        vector_heap[idx][pos] = a[2];
        return make_void();
    });
    add("vector-length", [&vector_heap](std::span<const EvalValue> a) {
        if (a.empty() || !is_vector(a[0]))
            return make_int(0);
        auto idx = as_vector_idx(a[0]);
        if (idx >= vector_heap.size())
            return make_int(0);
        return make_int(static_cast<std::int64_t>(vector_heap[idx].size()));
    });
    add("vector?", [](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_vector(a[0]));
    });
    add("make-vector", [&vector_heap](std::span<const EvalValue> a) {
        auto n = a.empty() ? 0 : static_cast<std::size_t>(as_int(a[0]));
        EvalValue init = a.size() > 1 ? a[1] : make_void();
        std::vector<EvalValue> elems(n, init);
        auto idx = vector_heap.size();
        vector_heap.push_back(std::move(elems));
        return make_vector(idx);
    });
    add("list->vector", [&pairs, &vector_heap](std::span<const EvalValue> a) {
        std::vector<EvalValue> elems;
        if (!a.empty()) {
            auto v = a[0];
            while (is_pair(v)) {
                auto idx = as_pair_idx(v);
                if (idx >= pairs.size())
                    break;
                elems.push_back(pairs[idx].car);
                v = pairs[idx].cdr;
            }
        }
        auto idx = vector_heap.size();
        vector_heap.push_back(std::move(elems));
        return make_vector(idx);
    });
    add("vector->list", [&pairs, &vector_heap](std::span<const EvalValue> a) {
        if (a.empty() || !is_vector(a[0]))
            return make_void();
        auto idx = as_vector_idx(a[0]);
        if (idx >= vector_heap.size())
            return make_void();
        EvalValue result = make_void();
        for (auto it = vector_heap[idx].rbegin(); it != vector_heap[idx].rend(); ++it) {
            auto pid = pairs.size();
            pairs.push_back({*it, result});
            result = make_pair(pid);
        }
        return result;
    });

    add("hash", [&ev, &string_heap](std::span<const EvalValue> a) {
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
            // Skip empty string keys (corruption / make_string(0) sentinel).
            if (is_string(a[i])) {
                auto ki = as_string_idx(a[i]);
                if (ki >= string_heap.size() || string_heap[ki].empty())
                    continue;
            }
            // Unpublished table: safe to free old blocks on grow.
            if (!flat_hash_insert_eval(ht, a[i], a[i + 1], string_heap,
                                       /*destroy_old_on_grow=*/true)) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
    add("hash-ref", [&ev, &string_heap](std::span<const EvalValue> a) {
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
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto sh = &string_heap;
        for (std::size_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF)
                continue;
            auto k = EvalValue{keys[i]};
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = keys[i] == a[1].val;
            if (eq)
                return EvalValue{vals[i]};
        }
        return a.size() >= 3 ? a[2] : make_void();
    });
    add("hash-has-key?", [&ev, &string_heap](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_hash(a[0]))
            return make_bool(false);
        std::lock_guard hlock(ev.hash_tables_mutex());
        std::lock_guard slock(ev.alloc_storage_lock_);
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
            return make_bool(false);
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto sh = &string_heap;
        for (std::size_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF)
                continue;
            auto k = EvalValue{keys[i]};
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = keys[i] == a[1].val;
            if (eq)
                return make_bool(true);
        }
        return make_bool(false);
    });
    add("hash-set!", [&ev, &string_heap](std::span<const EvalValue> a) {
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
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        // Issue #2652: refuse empty string keys (corruption / sentinel make_string(0)).
        if (is_string(a[1])) {
            auto bi = as_string_idx(a[1]);
            if (bi >= string_heap.size() || string_heap[bi].empty())
                return make_void();
        }
        for (std::size_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF)
                continue;
            auto k = EvalValue{keys[i]};
            if (eval_hash_keys_eq(k, a[1], string_heap)) {
                vals[i] = a[2].val;
                return make_void();
            }
        }
        // New key: insert with grow. Published table — do NOT free old
        // blocks (JIT / concurrent readers may still hold the prior
        // pointer under workspace lock, not hash_tables_mutex_). Always
        // publish the possibly-new pointer.
        (void)flat_hash_insert_eval(ht, a[1], a[2], string_heap,
                                    /*destroy_old_on_grow=*/false);
        g_hash_tables[hidx] = ht;
        return make_void();
    });
    add("hash-length", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_hash(a[0]))
            return make_int(0);
        std::lock_guard hlock(ev.hash_tables_mutex());
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
            return make_int(0);
        return make_int(static_cast<std::int64_t>(g_hash_tables[hidx]->size));
    });
    add("hash-keys", [&ev, &pairs](std::span<const EvalValue> a) {
        if (a.empty() || !is_hash(a[0]))
            return make_void();
        // Issue #2652: lock hash store + pairs_ growth.
        std::lock_guard hlock(ev.hash_tables_mutex());
        std::lock_guard slock(ev.alloc_storage_lock_);
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
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
            if (meta[i - 1] != 0xFF) {
                auto pid = pairs.size();
                pairs.push_back({EvalValue{keys[i - 1]}, result});
                result = make_pair(pid);
            }
        }
        return result;
    });
    add("hash-values", [&ev, &pairs](std::span<const EvalValue> a) {
        if (a.empty() || !is_hash(a[0]))
            return make_void();
        std::lock_guard hlock(ev.hash_tables_mutex());
        std::lock_guard slock(ev.alloc_storage_lock_);
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
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
            if (meta[i - 1] != 0xFF) {
                auto pid = pairs.size();
                pairs.push_back({EvalValue{vals[i - 1]}, result});
                result = make_pair(pid);
            }
        }
        return result;
    });
    add("hash?", [](std::span<const EvalValue> a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_hash(a[0]));
    });

    // Issue #278 follow-up #3: (hash->alist hash) — return the
    // hash as a list of (key . value) pairs in arbitrary order.
    // Pairs Aura's existing hash API (which has hash-ref /
    // hash-set / hash-keys / hash-values but no enumeration
    // primitive) with a way to iterate. Common use case: AI
    // agent wants to render a mutation-log:summary hash as a
    // string by iterating the alist.
    add("hash->alist", [&pairs](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
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
            if (meta[i - 1] != 0xFF) {
                // Build (key . value) cons cell.
                auto kv_pid = pairs.size();
                pairs.push_back({EvalValue{keys[i - 1]}, EvalValue{vals[i - 1]}});
                // Prepend to the result list.
                auto list_pid = pairs.size();
                pairs.push_back({make_pair(kv_pid), result});
                result = make_pair(list_pid);
            }
        }
        return result;
    });
    add("hash-remove!", [&string_heap](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
            return make_void();
        auto* ht = g_hash_tables[hidx];
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto sh = &string_heap;
        for (std::size_t i = 0; i < ht->capacity; ++i) {
            if (meta[i] == 0xFF)
                continue;
            auto k = EvalValue{keys[i]};
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = keys[i] == a[1].val;
            if (eq) {
                meta[i] = 0xFF;
                ht->size--;
                return make_bool(true);
            }
        }
        return make_bool(false);
    });
}

} // namespace aura::compiler::primitives_detail