// query_result_decode.hh — Issue #3424
// Decode schema-2 QueryResult hash for resolve_mutate_node_arg /
// resolve_query_node_arg. Production query:* auto-upgrades to this hash
// (#3395 / #3286); the write/query helpers must accept it without
// dropping provenance through a raw NodeId restamp.
//
// Include AFTER `module aura.compiler.evaluator;` (+ value / ast imports)
// so Pair / EvalValue / FlatAST are in scope.

#ifndef AURA_COMPILER_QUERY_RESULT_DECODE_HH
#define AURA_COMPILER_QUERY_RESULT_DECODE_HH

#include <cstdint>
#include <string>
#include <string_view>

namespace aura::compiler::query_result_decode {

inline constexpr int kQueryResultHashResolveIssue = 3424;

// Same validator as evaluator_primitives_query_workspace.cpp (moved here
// so mutate.cpp can share it). Soft / empty matches → Fresh after epoch.
[[nodiscard]] inline aura::core::QueryResultFreshness
query_result_is_fresh_with_refs(const aura::core::QueryResult& qr, const aura::ast::FlatAST& flat,
                                std::uint64_t current_tenant_id,
                                std::uint64_t current_fiber_id) noexcept {
    if (!qr.is_fresh_live(flat.generation()))
        return aura::core::QueryResultFreshness::StaleByEpoch;
    if (flat.nested_authority_gap())
        return aura::core::QueryResultFreshness::StaleByEpoch;
    if (qr.match_count == 0)
        return aura::core::QueryResultFreshness::Fresh;
    const bool hard = aura::compiler::typed_audit::production_defaults_active();
    if (!qr.matches[0].has_full_provenance()) {
        if (hard)
            aura::core::note_query_result_full_provenance_stale();
        return aura::core::QueryResultFreshness::SoftOnlyNoProvenance;
    }
    if (hard && qr.matches[0].reserved != aura::core::kQueryResultMatchSchema2Prod) {
        aura::core::note_query_result_full_provenance_stale();
        return aura::core::QueryResultFreshness::SoftOnlyNoProvenance;
    }
    const auto live_mutation = aura::core::current_mutation_epoch();
    const auto live_cow = flat.workspace_cow_epoch();
    for (std::size_t i = 0; i < qr.match_count; ++i) {
        const auto& m = qr.matches[i];
        if (hard) {
            if (current_tenant_id != 0 && m.tenant_id != current_tenant_id) {
                aura::core::note_query_result_full_provenance_tenant_mismatch();
                return aura::core::QueryResultFreshness::InvalidTenant;
            }
            if (current_fiber_id != 0 && m.fiber_id != current_fiber_id) {
                aura::core::note_query_result_full_provenance_fiber_mismatch();
                return aura::core::QueryResultFreshness::InvalidFiber;
            }
            if (live_cow != 0 && m.cow_epoch_at_capture != 0 &&
                m.cow_epoch_at_capture != live_cow) {
                aura::core::note_query_result_full_provenance_cow_mismatch();
                return aura::core::QueryResultFreshness::InvalidCowLayer;
            }
            if (live_mutation != 0 && m.mutation_id_at_capture != 0 &&
                static_cast<std::uint64_t>(m.mutation_id_at_capture) != live_mutation)
                return aura::core::QueryResultFreshness::InvalidMutation;
            continue;
        }
        if (current_tenant_id != 0 && m.tenant_id != 0 && m.tenant_id != current_tenant_id) {
            aura::core::note_query_result_full_provenance_tenant_mismatch();
            return aura::core::QueryResultFreshness::InvalidTenant;
        }
        if (current_fiber_id != 0 && m.fiber_id != 0 && m.fiber_id != current_fiber_id) {
            aura::core::note_query_result_full_provenance_fiber_mismatch();
            return aura::core::QueryResultFreshness::InvalidFiber;
        }
        if (m.cow_epoch_at_capture != 0 && live_cow != 0 && m.cow_epoch_at_capture != live_cow) {
            aura::core::note_query_result_full_provenance_cow_mismatch();
            return aura::core::QueryResultFreshness::InvalidCowLayer;
        }
        if (m.mutation_id_at_capture != 0 && live_mutation != 0 &&
            static_cast<std::uint64_t>(m.mutation_id_at_capture) != live_mutation)
            return aura::core::QueryResultFreshness::InvalidMutation;
    }
    return aura::core::QueryResultFreshness::Fresh;
}

enum class HashNodeKind : std::uint8_t { NotHash = 0, Ok = 1, Stale = 2, BadArg = 3 };

struct HashNodeResolve {
    HashNodeKind kind = HashNodeKind::NotHash;
    aura::ast::NodeId node = 0;
    const char* err_kind = "bad-arg";
    std::string err_msg;
};

template <typename StringHeap, typename PairVec>
[[nodiscard]] inline bool decode_query_result_hash(types::EvalValue arg, const StringHeap& heap,
                                                   const PairVec& pairs,
                                                   aura::core::QueryResult& out) noexcept {
    using types::as_hash_idx;
    using types::as_int;
    using types::as_pair_idx;
    using types::as_string_idx;
    using types::is_hash;
    using types::is_int;
    using types::is_pair;
    using types::is_string;
    using types::make_void;
    if (!is_hash(arg))
        return false;
    const auto hidx = as_hash_idx(arg);
    if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
        return false;
    auto* ht = g_hash_tables[hidx];
    std::int64_t mut = -1, gen = -1, wrap = 0, cow = 0, tenant = 0, fiber = 0, mid = 0, tag = 0,
                 schema3137 = 0;
    types::EvalValue matches = make_void();
    bool have_matches = false;
    auto meta = ht->metadata();
    auto keys = ht->keys();
    auto vals = ht->values();
    for (std::size_t i = 0; i < ht->capacity; ++i) {
        if (meta[i] == 0xFF)
            continue;
        types::EvalValue k{keys[i]};
        if (!is_string(k))
            continue;
        const auto sidx = as_string_idx(k);
        if (sidx >= heap.size())
            continue;
        const auto& s = heap[sidx];
        types::EvalValue v{vals[i]};
        if (s == "matches") {
            matches = v;
            have_matches = true;
            continue;
        }
        if (!is_int(v))
            continue;
        const auto n = as_int(v);
        if (s == "mutation-epoch")
            mut = n;
        else if (s == "generation")
            gen = n;
        else if (s == "query-result-tag" || s == "schema-2933")
            tag = n;
        else if (s == "schema-3137")
            schema3137 = n;
        else if (s == "wrap-epoch")
            wrap = n;
        else if (s == "cow-epoch-at-capture")
            cow = n;
        else if (s == "tenant-id")
            tenant = n;
        else if (s == "fiber-id")
            fiber = n;
        else if (s == "mutation-id-at-capture")
            mid = n;
    }
    if (tag == 0 && schema3137 == 0 && mut < 0)
        return false;
    out = {};
    out.epoch.mutation_epoch = static_cast<std::uint64_t>(mut < 0 ? 0 : mut);
    out.epoch.generation = static_cast<std::uint64_t>(gen < 0 ? 0 : gen);
    const auto reserved = schema3137 != 0 ? aura::core::kQueryResultMatchSchema2Prod
                                          : (tag != 0 ? aura::core::kQueryResultMatchSchema2
                                                      : static_cast<std::uint8_t>(0));
    if (have_matches) {
        types::EvalValue cur = matches;
        while (is_pair(cur) && cur.val != make_void().val) {
            const auto outer = as_pair_idx(cur);
            if (static_cast<std::size_t>(outer) >= pairs.size())
                break;
            const auto car = pairs[outer].car;
            std::uint32_t node_id = 0;
            std::uint16_t node_gen = 0;
            bool got = false;
            if (is_int(car)) {
                node_id = static_cast<std::uint32_t>(as_int(car));
                got = true;
            } else if (is_pair(car)) {
                const auto inner = as_pair_idx(car);
                if (static_cast<std::size_t>(inner) < pairs.size()) {
                    const auto id_ev = pairs[inner].car;
                    if (is_int(id_ev)) {
                        node_id = static_cast<std::uint32_t>(as_int(id_ev));
                        got = true;
                    }
                    const auto cdr_ev = pairs[inner].cdr;
                    if (is_int(cdr_ev))
                        node_gen = static_cast<std::uint16_t>(as_int(cdr_ev));
                }
            }
            if (got) {
                if (!out.push_match_full(
                        node_id, node_gen, static_cast<std::uint16_t>(wrap),
                        static_cast<std::uint16_t>(cow), static_cast<std::uint32_t>(tenant),
                        static_cast<std::uint32_t>(fiber), static_cast<std::uint32_t>(mid), 0))
                    break;
                out.matches[out.match_count - 1].reserved = reserved;
            }
            cur = pairs[outer].cdr;
        }
    }
    if (out.match_count == 0 && reserved != 0) {
        // Tag-only hash with empty matches is still a QueryResult.
    }
    return true;
}

// Shared node-operand resolve. NotHash → caller continues pair/int.
// Ok → out.node is the match identity (no occupancy restamp).
// Stale / BadArg → caller returns mev(err_kind, err_msg).
template <typename StringHeap, typename PairVec>
[[nodiscard]] inline HashNodeResolve
resolve_query_result_match(types::EvalValue arg, const StringHeap& heap, const PairVec& pairs,
                           const aura::ast::FlatAST& flat, std::uint64_t tenant,
                           std::uint64_t fiber, const char* op) {
    HashNodeResolve r;
    using types::is_hash;
    if (!is_hash(arg))
        return r;
    aura::core::QueryResult qr;
    if (!decode_query_result_hash(arg, heap, pairs, qr)) {
        r.kind = HashNodeKind::BadArg;
        r.err_kind = "bad-arg";
        r.err_msg = std::string(op) + ": not a QueryResult hash";
        return r;
    }
    const auto fresh = query_result_is_fresh_with_refs(qr, flat, tenant, fiber);
    if (fresh != aura::core::QueryResultFreshness::Fresh) {
        r.kind = HashNodeKind::Stale;
        r.err_kind = "stale-ref";
        r.err_msg = std::string(op) + ": QueryResult not fresh";
        return r;
    }
    if (qr.match_count != 1) {
        r.kind = HashNodeKind::BadArg;
        r.err_kind = "bad-arg";
        r.err_msg = std::string(op) + ": need single match or explicit index";
        return r;
    }
    r.kind = HashNodeKind::Ok;
    r.node = static_cast<aura::ast::NodeId>(qr.matches[0].node_id);
    return r;
}

} // namespace aura::compiler::query_result_decode

#endif // AURA_COMPILER_QUERY_RESULT_DECODE_HH
