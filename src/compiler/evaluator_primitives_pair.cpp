// evaluator_primitives_pair.cpp — P0 step 2: pair/list/string heap primitives
// Issue #2996: migrate evaluator_primitives_pair.cpp onto register_prim + PrimSpec.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// aura.compiler.evaluator module partition.
// Registration uses PrimRegistrar + heap refs (FFI/adt_runtime pattern).

module;


#include "runtime_shared.h"
#include "float_repr.hh"      // Issue #4468
#include "utf8_scalar.hh"     // Issue #4472
#include "prim_heap_quota.hh" // Issue #2916

#include "prim_registrar_scaffold.hh" // Issue #2996
#include "security_side_effect.hh"    // Issue #4057: kEffectMutate on the isolation consult
#include "core/sandbox.hh"            // Issue #4057: is_strict() production-face arm
#include "core/provenance_tracker.hh" // Issue #4057: multi_tenant_env_active() face arm

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
using types::make_int_or_promote;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

namespace {

    // Issue #4472: lowercase digits, radix 2..36. Radix 10 stays std::to_string.
    std::string format_int_radix(std::int64_t n, int radix) {
        if (radix == 10)
            return std::to_string(n);
        const bool neg = n < 0;
        std::uint64_t mag = 0;
        if (neg)
            mag = static_cast<std::uint64_t>(-(n + 1)) + 1u;
        else
            mag = static_cast<std::uint64_t>(n);
        if (mag == 0)
            return "0";
        char buf[72];
        int i = 0;
        while (mag > 0 && i < static_cast<int>(sizeof(buf))) {
            const auto d = static_cast<unsigned>(mag % static_cast<unsigned>(radix));
            buf[i++] = static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
            mag /= static_cast<unsigned>(radix);
        }
        std::string s;
        s.reserve(static_cast<std::size_t>(i) + (neg ? 1 : 0));
        if (neg)
            s.push_back('-');
        while (i > 0)
            s.push_back(buf[--i]);
        return s;
    }

    EvalValue radix_out_of_range(Evaluator& ev, std::vector<EvalValue>& error_values,
                                 const char* who) {
        auto idx = static_cast<std::uint64_t>(
            ev.push_string_heap(std::string(who) + ": radix out of range"));
        auto e = error_values.size();
        error_values.push_back(make_string(idx));
        return make_error(e);
    }

    // Issue #4472: live string->number. Optional radix 2..36, default 10.
    // Integer digits go through from_chars before any double conversion so
    // values past 2^53 stay exact and [2^62, 2^63) does not abort (#4462).
    // Parse failure stays #f ([SILENCE-PRIM-#615]). A radix outside 2..36
    // is an error, not a silent decimal parse.
    EvalValue parse_string_number(Evaluator& ev, std::pmr::vector<std::string>& string_heap,
                                  std::vector<EvalValue>& error_values,
                                  std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= string_heap.size())
            return make_bool(false);
        // Issue #4530: a foreign numeric string must not answer with its
        // parsed value; same reader gate as string->list (#4505).
        if (!ev.inrange_string_tenant_allows(i, /*required_effects=*/0, "string->number"))
            return make_primitive_error(
                string_heap, error_values,
                "string->number: cross-tenant string slot read denied (#4505)",
                ev.primitive_error_counter_ptr());
        const auto& str = string_heap[i];
        if (str.empty())
            return make_bool(false);
        int base = 10;
        if (a.size() > 1) {
            if (!is_int(a[1]))
                return radix_out_of_range(ev, error_values, "string->number");
            const auto r = as_int(a[1]);
            if (r < 2 || r > 36)
                return radix_out_of_range(ev, error_values, "string->number");
            base = static_cast<int>(r);
        }
        const auto first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return make_bool(false);
        const auto last = str.find_last_not_of(" \t\r\n");
        const std::string_view trimmed(str.data() + first, last - first + 1);
        const bool want_float = base == 10 && (trimmed.find('.') != std::string_view::npos ||
                                               trimmed.find('e') != std::string_view::npos ||
                                               trimmed.find('E') != std::string_view::npos);
        if (!want_float) {
            std::string_view body = trimmed;
            if (!body.empty() && body.front() == '+')
                body.remove_prefix(1);
            if (body.empty())
                return make_bool(false);
            std::int64_t iv = 0;
            const char* b0 = body.data();
            const char* b1 = b0 + body.size();
            const auto [pi, eci] = std::from_chars(b0, b1, iv, base);
            if (eci == std::errc{} && pi == b1)
                return make_int_or_promote(iv);
            if (base == 10 && eci == std::errc::result_out_of_range) {
                double wide = 0;
                const char* t0 = trimmed.data();
                const char* t1 = t0 + trimmed.size();
                const auto [pf, ecf] = std::from_chars(t0, t1, wide);
                if (ecf == std::errc{} && pf == t1)
                    return make_float(wide);
            }
            return make_bool(false);
        }
        const char* start = trimmed.data();
        const char* end = start + trimmed.size();
        double fval = 0;
        const auto [pfloat, ec_float] = std::from_chars(start, end, fval);
        if (ec_float == std::errc{} && pfloat == end)
            return make_float(fval);
        return make_bool(false);
    }

} // namespace

void register_pair_and_string_primitives(PrimRegistrar add, Evaluator& ev,
                                         std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         std::vector<EvalValue>& error_values,
                                         std::atomic<std::uint64_t>* primitive_error_counter) {
    using aura::compiler::mutate_general;
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    register_prim(
        add, ev, "string-copy",
        [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.empty() || !is_string(a[0]))
                return make_bool(false);
            // Strings are immutable-like; just return the same reference
            return a[0];
        },
        pure_general(1, "(string) -> string", "Copy a string."));
    register_prim(
        add, ev, "string-fill!",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
                return make_void();
            // Issue #2651: serialize content mutate vs concurrent reallocate.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_string_idx(a[0]);
            if (idx >= string_heap.size())
                return make_void();
            // Issue #4428: dispatch Mutate is not an owner check. Prod face
            // compares the intern stamp and leaves the bytes unchanged on
            // mismatch. Soft/Off does not read the stamp.
            if (!ev.inrange_string_tenant_allows(idx, aura::compiler::security::kEffectMutate,
                                                 "string-fill!")) {
                return make_primitive_error(string_heap, error_values,
                                            "string-fill!: cross-tenant string slot write "
                                            "denied (#4428)");
            }
            auto fill_char = static_cast<char>(as_int(a[1]));
            std::fill(string_heap[idx].begin(), string_heap[idx].end(), fill_char);
            return make_void();
        },
        mutate_general(2, "(string char) -> void", "Fill a string with a character."));
    register_prim(
        add, ev, "string->list",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty() || !is_string(a[0]))
                return make_bool(false);
            // Issue #2651: pairs_ + string_heap_ under alloc_storage_lock_.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_string_idx(a[0]);
            if (idx >= string_heap.size())
                return make_bool(false);
            // Issue #4505: reader gate before the code points are read.
            if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string->list")) {
                return make_primitive_error(
                    string_heap, error_values,
                    "string->list: cross-tenant string slot read denied (#4505)",
                    primitive_error_counter);
            }
            const auto& s = string_heap[idx];
            // Issue #4472: one code point per element. Byte length is an
            // upper bound, so the #2916 quota check stays conservative.
            if (!ev.prim_heap_quota_allow(PrimHeapDim::Pairs, pairs.size() + s.size())) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                    primitive_error_counter);
            }
            std::vector<std::int64_t> cps;
            cps.reserve(s.size());
            std::size_t bi = 0;
            Utf8Unit unit;
            while (utf8_unit_at(s, bi, unit)) {
                cps.push_back(static_cast<std::int64_t>(unit.cp));
                bi += unit.nbytes;
            }
            EvalValue result = make_void();
            for (auto it = cps.rbegin(); it != cps.rend(); ++it) {
                auto pid = pairs.size();
                pairs.push_back({make_int(*it), result});
                ev.note_inrange_pair_mint(pid); // Issue #4298
                result = make_pair(pid);
            }
            return result;
        },
        pure_general(1, "(string) -> list", "Convert a string to a list of code points."));
    register_prim(
        add, ev, "list->string",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty())
                return make_bool(false);
            // Issue #2651: snapshot under lock then intern.
            std::string result;
            {
                std::lock_guard lock(ev.alloc_storage_lock_);
                auto v = a[0];
                while (is_pair(v)) {
                    auto p = as_pair_idx(v);
                    if (p >= pairs.size())
                        break;
                    auto car = pairs[p].car;
                    if (is_int(car)) {
                        // Issue #4472: encode a code point. ASCII stays one byte.
                        utf8_append(result, as_int(car));
                    } else if (is_string(car)) {
                        auto sidx = as_string_idx(car);
                        if (sidx < string_heap.size())
                            result += string_heap[sidx];
                    }
                    v = pairs[p].cdr;
                }
                if (!ev.prim_heap_quota_allow(PrimHeapDim::Strings, string_heap.size() + 1)) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Strings)),
                        primitive_error_counter);
                }
                auto sidx = string_heap.size();
                string_heap.push_back(std::move(result));
                ev.note_inrange_string_mint(sidx); // Issue #4428
                return make_string(sidx);
            }
        },
        pure_general(1, "(list) -> string", "Convert a list of chars to a string."));
    register_prim(
        add, ev, "string-join",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.size() < 2 || !is_string(a[1]))
                return make_bool(false);
            // Issue #2651: multi-fiber long joins (overnight pads) must lock.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto delim_idx = as_string_idx(a[1]);
            if (delim_idx >= string_heap.size())
                return make_bool(false);
            // Issue #4530: reader gates before any foreign byte is joined —
            // the delimiter, every visited spine slot, and every element
            // string. Helpers are lock-free; we hold alloc_storage_lock_.
            if (!ev.inrange_string_tenant_allows(delim_idx, /*required_effects=*/0, "string-join"))
                return make_primitive_error(
                    string_heap, error_values,
                    "string-join: cross-tenant string slot read denied (#4505)",
                    primitive_error_counter);
            auto& delim = string_heap[delim_idx];
            std::string result;
            bool first = true;
            auto v = a[0];
            while (is_pair(v)) {
                auto p = as_pair_idx(v);
                if (p >= pairs.size())
                    break;
                if (!ev.inrange_slot_tenant_allows(p, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "string-join"))
                    return make_primitive_error(
                        string_heap, error_values,
                        "string-join: cross-tenant pair slot read denied (#4505)",
                        primitive_error_counter);
                auto car = pairs[p].car;
                if (is_string(car)) {
                    auto sidx = as_string_idx(car);
                    if (sidx < string_heap.size()) {
                        if (!ev.inrange_string_tenant_allows(sidx, /*required_effects=*/0,
                                                             "string-join"))
                            return make_primitive_error(
                                string_heap, error_values,
                                "string-join: cross-tenant string slot read denied (#4505)",
                                primitive_error_counter);
                        if (!first)
                            result += delim;
                        result += string_heap[sidx];
                        first = false;
                    }
                }
                v = pairs[p].cdr;
            }
            if (!ev.prim_heap_quota_allow(PrimHeapDim::Strings, string_heap.size() + 1)) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Strings)),
                    primitive_error_counter);
            }
            auto sidx = string_heap.size();
            string_heap.push_back(std::move(result));
            ev.note_inrange_string_mint(sidx); // Issue #4428
            return make_string(sidx);
        },
        pure_general(2, "(list string) -> string", "Join string list with a separator."));

    // ── Pair / List / String primitives ─────────────────────────
    register_prim(
        add, ev, "cons",
        [&ev, &string_heap, &error_values, primitive_error_counter](std::span<const EvalValue> a) {
            // Issue #2651 / #1397: mint_pair locks pairs_ (recursive).
            // Issue #2916: quota is the grow path only. A freelist hit (#4459)
            // does not change pairs.size(), so it must not fail the quota.
            const auto id = ev.mint_pair(a[0], a[1], /*check_quota=*/true);
            if (id == Evaluator::kFileHeapMintDenied) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                    primitive_error_counter);
            }
            return make_pair(id);
        },
        pure_general(2, "(any any) -> pair", "Construct a pair."));
    register_prim(
        add, ev, "car",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            // Issue #4322: production (query :find) / (query :children) is a
            // schema-2 hash (#3449 / #3827). The historical locator is
            // (car (query :children (car found))), which is the first match
            // NodeId on the Soft bare list. Same car on the hash returns
            // that NodeId. A hash that is not a QueryResult stays "not a pair".
            if (!a.empty() && is_hash(a[0])) {
                std::lock_guard hlock(ev.hash_tables_mutex());
                std::lock_guard slock(ev.alloc_storage_lock_);
                const auto hidx = as_hash_idx(a[0]);
                if (hidx < g_hash_tables.size() && g_hash_tables[hidx] &&
                    !aura_hash_gate_checked(hidx, ev.capability_tenant_id(),
                                            ev.effect_sandbox_mode(), "car")) {
                    auto* ht = g_hash_tables[hidx];
                    auto meta = ht->metadata();
                    auto keys = ht->keys();
                    auto vals = ht->values();
                    auto lookup = [&](std::string_view want) -> EvalValue {
                        for (std::size_t i = 0; i < ht->capacity; ++i) {
                            if (meta[i] == 0xFF)
                                continue;
                            auto k = EvalValue{keys[i]};
                            if (!is_string(k))
                                continue;
                            auto ki = as_string_idx(k);
                            if (ki < string_heap.size() && string_heap[ki] == want)
                                return EvalValue{vals[i]};
                        }
                        return make_void();
                    };
                    auto tag = lookup("query-result-tag");
                    auto matches = lookup("matches");
                    if (is_int(tag) && as_int(tag) == 1 && is_pair(matches)) {
                        auto mid = as_pair_idx(matches);
                        if (mid < pairs.size() && is_pair(pairs[mid].car)) {
                            auto inner = as_pair_idx(pairs[mid].car);
                            if (inner < pairs.size() && is_int(pairs[inner].car))
                                return pairs[inner].car;
                        }
                    }
                }
            }
            if (a.empty() || !is_pair(a[0])) {
                return make_primitive_error(string_heap, error_values, "car: not a pair",
                                            primitive_error_counter);
            }
            auto id = as_pair_idx(a[0]);
            if (id < pairs.size()) {
                // Issue #4298: mailbox of an in-range pair index. Prod face
                // compares the mint stamp and does not return the payload.
                if (!ev.inrange_slot_tenant_allows(id, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "car")) {
                    return make_primitive_error(string_heap, error_values,
                                                "car: cross-tenant pair slot read denied (#4298)",
                                                primitive_error_counter);
                }
                return pairs[id].car;
            }
            // Fallback to shared pair storage (JIT/arena pairs)
            if (id < g_pair_slots.size() && g_pair_slots[id]) {
                // Issue #4111: g_pair_slots index space is process-shared - the
                // same #4057 rationale as set-car!/set-cdr!: a foreign
                // tenant's JIT pair can land at idx >= the local pair
                // vector (mailbox handoff), so the index alone is not
                // ownership. Under the production face (sandbox != 0 and
                // Strict or Restricted+MT) compare the slot's owner stamp
                // against the caller principal BEFORE the read; a foreign
                // or unstamped (0) slot is refused with the slot unread.
                // IsolationDeny rides the existing check_workspace_isolation /
                // record_audit path (fiber id + Mutation epoch) with
                // required_effects = 0 - readers consume no effects and
                // dispatch stays the single Mutate choke for writers
                // (#4036). Soft/Off: the face probe short-circuits and
                // the tenant array is never read (zero-cost contract).
                const bool prod_pair_face =
                    ev.effect_sandbox_mode() != 0 &&
                    (ev.effect_sandbox_mode() == 2 || ::aura::core::sandbox::is_strict() ||
                     ::aura::core::provenance::multi_tenant_env_active());
                if (prod_pair_face) {
                    const std::uint64_t slot_tenant =
                        (id < g_pair_slot_tenants.size()) ? g_pair_slot_tenants[id] : 0;
                    if (slot_tenant != ev.capability_tenant_id()) {
                        (void)ev.check_workspace_isolation(
                            /*target=*/ev.capability_tenant_id(), /*ref_tenant=*/slot_tenant,
                            /*required_effects=*/0, "car");
                        return make_primitive_error(
                            string_heap, error_values,
                            "car: cross-tenant pair slot read denied (#4111)",
                            primitive_error_counter);
                    }
                }
                return types::EvalValue{g_pair_slots[id]->car};
            }
            // Issue #2998: corrupted pair index is a true error, not silent 0.
            return make_primitive_error(string_heap, error_values, "car: corrupted pair",
                                        primitive_error_counter);
        },
        pure_general(1, "(pair) -> any", "First element of a pair."));
    register_prim(
        add, ev, "cdr",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            if (a.empty() || !is_pair(a[0])) {
                return make_primitive_error(string_heap, error_values, "cdr: not a pair",
                                            primitive_error_counter);
            }
            auto id = as_pair_idx(a[0]);
            if (id < pairs.size()) {
                // Issue #4298: same in-range read gate as car.
                if (!ev.inrange_slot_tenant_allows(id, /*vector_heap=*/false,
                                                   /*required_effects=*/0, "cdr")) {
                    return make_primitive_error(string_heap, error_values,
                                                "cdr: cross-tenant pair slot read denied (#4298)",
                                                primitive_error_counter);
                }
                return pairs[id].cdr;
            }
            // Fallback to shared pair storage (JIT/arena pairs)
            if (id < g_pair_slots.size() && g_pair_slots[id]) {
                // Issue #4111: g_pair_slots index space is process-shared - the
                // same #4057 rationale as set-car!/set-cdr!: a foreign
                // tenant's JIT pair can land at idx >= the local pair
                // vector (mailbox handoff), so the index alone is not
                // ownership. Under the production face (sandbox != 0 and
                // Strict or Restricted+MT) compare the slot's owner stamp
                // against the caller principal BEFORE the read; a foreign
                // or unstamped (0) slot is refused with the slot unread.
                // IsolationDeny rides the existing check_workspace_isolation /
                // record_audit path (fiber id + Mutation epoch) with
                // required_effects = 0 - readers consume no effects and
                // dispatch stays the single Mutate choke for writers
                // (#4036). Soft/Off: the face probe short-circuits and
                // the tenant array is never read (zero-cost contract).
                const bool prod_pair_face =
                    ev.effect_sandbox_mode() != 0 &&
                    (ev.effect_sandbox_mode() == 2 || ::aura::core::sandbox::is_strict() ||
                     ::aura::core::provenance::multi_tenant_env_active());
                if (prod_pair_face) {
                    const std::uint64_t slot_tenant =
                        (id < g_pair_slot_tenants.size()) ? g_pair_slot_tenants[id] : 0;
                    if (slot_tenant != ev.capability_tenant_id()) {
                        (void)ev.check_workspace_isolation(
                            /*target=*/ev.capability_tenant_id(), /*ref_tenant=*/slot_tenant,
                            /*required_effects=*/0, "cdr");
                        return make_primitive_error(
                            string_heap, error_values,
                            "cdr: cross-tenant pair slot read denied (#4111)",
                            primitive_error_counter);
                    }
                }
                return types::EvalValue{g_pair_slots[id]->cdr};
            }
            // Issue #2998: corrupted pair index is a true error, not silent 0.
            return make_primitive_error(string_heap, error_values, "cdr: corrupted pair",
                                        primitive_error_counter);
        },
        pure_general(1, "(pair) -> any", "Rest of a pair."));

    // Issue #4111: the caar/cadr/... shorthands below read only the
    // evaluator-local pair vector (idx >= its size yields void) - they
    // never fall through to g_pair_slot_tenants, so the shorthand family has no
    // ungated shared-slot read face and nested composition cannot
    // bypass the #4111 car/cdr gate (documented no-fallthrough per
    // the issue AC).
    register_prim(
        add, ev, "pair?",
        [](const auto& a) {
            if (a.empty())
                return make_bool(false);
            return make_bool(is_pair(a[0]));
        },
        pure_general(1, "(any) -> bool", "True if value is a pair."));

    // ── Cadr / Caddr shorthands ────────────────────────────────────
    register_prim(
        add, ev, "caar",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: the shorthand family reads the same in-range
            // pairs_ slots as car/cdr (#4298) — stamp compare before each
            // field read, deny shape identical, no effect consumed.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caar")) {
                return make_primitive_error(string_heap, error_values,
                                            "caar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].car;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caar")) {
                return make_primitive_error(string_heap, error_values,
                                            "caar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[ci].car;
        },
        pure_general(1, "(pair) -> any", "car of car."));
    register_prim(
        add, ev, "cadr",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].cdr;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[ci].car;
        },
        pure_general(1, "(pair) -> any", "car of cdr."));
    register_prim(
        add, ev, "cdar",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].car;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[ci].cdr;
        },
        pure_general(1, "(pair) -> any", "cdr of car."));
    register_prim(
        add, ev, "cddr",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].cdr;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[ci].cdr;
        },
        pure_general(1, "(pair) -> any", "cdr of cdr."));
    register_prim(
        add, ev, "caaar",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caaar")) {
                return make_primitive_error(string_heap, error_values,
                                            "caaar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].car;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caaar")) {
                return make_primitive_error(string_heap, error_values,
                                            "caaar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].car;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caaar")) {
                return make_primitive_error(string_heap, error_values,
                                            "caaar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].car;
        },
        pure_general(1, "(pair) -> any", "car of caar."));
    register_prim(
        add, ev, "caadr",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "caadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].cdr;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "caadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].car;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "caadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].car;
        },
        pure_general(1, "(pair) -> any", "car of cadr."));
    register_prim(
        add, ev, "cadar",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cadar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cadar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].car;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cadar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cadar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].cdr;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cadar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cadar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].car;
        },
        pure_general(1, "(pair) -> any", "car of cdar."));
    register_prim(
        add, ev, "caddr",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "caddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].cdr;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "caddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].cdr;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "caddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "caddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].car;
        },
        pure_general(1, "(pair) -> any", "car of cddr."));
    register_prim(
        add, ev, "cdaar",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdaar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdaar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].car;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdaar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdaar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].car;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdaar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdaar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].cdr;
        },
        pure_general(1, "(pair) -> any", "cdr of caar."));
    register_prim(
        add, ev, "cdadr",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].cdr;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].car;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdadr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdadr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].cdr;
        },
        pure_general(1, "(pair) -> any", "cdr of cadr."));
    register_prim(
        add, ev, "cddar",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cddar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cddar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].car;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cddar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cddar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].cdr;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cddar")) {
                return make_primitive_error(string_heap, error_values,
                                            "cddar: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].cdr;
        },
        pure_general(1, "(pair) -> any", "cdr of cdar."));
    register_prim(
        add, ev, "cdddr",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_pair(a[0]))
                return make_void();
            auto idx = as_pair_idx(a[0]);
            if (idx >= pairs.size())
                return make_void();
            // Issue #4505: same reader gate as caar, one compare per slot.
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto c = pairs[idx].cdr;
            if (!is_pair(c))
                return make_void();
            auto ci = as_pair_idx(c);
            if (!ev.inrange_slot_tenant_allows(ci, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            auto d = pairs[ci].cdr;
            if (!is_pair(d))
                return make_void();
            auto di = as_pair_idx(d);
            if (!ev.inrange_slot_tenant_allows(di, /*vector_heap=*/false,
                                               /*required_effects=*/0, "cdddr")) {
                return make_primitive_error(string_heap, error_values,
                                            "cdddr: cross-tenant pair slot read denied (#4505)",
                                            ev.primitive_error_counter_ptr());
            }
            return pairs[di].cdr;
        },
        pure_general(1, "(pair) -> any", "cdr of cddr."));

    // ── Mutable pair operations ───────────────────────────────────
    // Issue #1397 + #1399: set-car!/set-cdr! mutate an existing
    // pair slot. The pair-index resolution + field write must be
    // atomic relative to concurrent `pairs_.push_back` /
    // `pairs_.resize` that may reallocate the underlying vector
    // (invalidating the resolved index).
    //
    // #1397 wrapped the field write under `alloc_storage_lock_`,
    // but idx was resolved BEFORE the lock — leaving a window
    // where another fiber's push_back could realloc pairs_ between
    // idx capture and the locked write, producing a write to the
    // wrong slot (or to freed memory).
    //
    // #1399 fix: resolve idx INSIDE the lock_guard so the
    // (idx capture → bounds check → field write) sequence is one
    // atomic critical section. Combined with #1397, this closes
    // the cross-fiber pair-mutation race.
    register_prim(
        add, ev, "set-car!",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !is_pair(a[0]))
                return make_void();
            // Issue #1397 + #1399: resolve idx + bounds check + field
            // write all under alloc_storage_lock_ so concurrent
            // push_back / resize cannot realloc pairs_ between
            // idx capture and write.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_pair_idx(a[0]);
            if (idx < pairs.size()) {
                // Issue #4298: in-range pairs_ skipped the #4057 owner compare.
                // Prod face audits a foreign mint and leaves the slot unchanged.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   aura::compiler::security::kEffectMutate,
                                                   "set-car!")) {
                    return make_primitive_error(string_heap, error_values,
                                                "set-car!: cross-tenant pair slot write "
                                                "denied (#4298)");
                }
                pairs[idx].car = a[1];
            } else if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
                // Issue #4057: g_pair_slots index space is process-shared — a
                // foreign tenant's JIT pair can land at idx >= pairs_.size()
                // (mailbox handoff), so the index alone is not ownership.
                // Production face (sandbox != 0 and Strict or Restricted+MT):
                // compare the slot's owner stamp (#4036-style parallel array)
                // against the caller principal BEFORE the write; a foreign or
                // unstamped (0) slot is refused with the slot unchanged.
                // IsolationDeny goes through the existing
                // check_workspace_isolation / record_audit path (fiber id +
                // Mutation epoch). Dispatch is the single Mutate choke
                // (#4036 body-choke contract) — no second effect check in the
                // body, a single-use grant must not double-consume.
                // Soft/Off: the tenant array is never read, write proceeds.
                const std::uint64_t slot_tenant =
                    (idx < g_pair_slot_tenants.size()) ? g_pair_slot_tenants[idx] : 0;
                const bool prod_pair_face =
                    ev.effect_sandbox_mode() != 0 &&
                    (ev.effect_sandbox_mode() == 2 || ::aura::core::sandbox::is_strict() ||
                     ::aura::core::provenance::multi_tenant_env_active());
                if (prod_pair_face && slot_tenant != ev.capability_tenant_id()) {
                    (void)ev.check_workspace_isolation(
                        /*target=*/ev.capability_tenant_id(), /*ref_tenant=*/slot_tenant,
                        aura::compiler::security::kEffectMutate, "set-car!");
                    return make_primitive_error(string_heap, error_values,
                                                "set-car!: cross-tenant pair slot write "
                                                "denied (#4057)");
                }
                g_pair_slots[idx]->car = a[1].val;
            }
            return make_void();
        },
        mutate_general(2, "(pair any) -> void", "Set the car of a pair."));
    register_prim(
        add, ev, "set-cdr!",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !is_pair(a[0]))
                return make_void();
            // Issue #1397 + #1399: same atomic guard as set-car! —
            // idx capture inside lock_guard, not before.
            std::lock_guard lock(ev.alloc_storage_lock_);
            auto idx = as_pair_idx(a[0]);
            if (idx < pairs.size()) {
                // Issue #4298: same in-range write gate as set-car!.
                if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                                   aura::compiler::security::kEffectMutate,
                                                   "set-cdr!")) {
                    return make_primitive_error(string_heap, error_values,
                                                "set-cdr!: cross-tenant pair slot write "
                                                "denied (#4298)");
                }
                pairs[idx].cdr = a[1];
            } else if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
                // Issue #4057: same cross-tenant arm as set-car! — the
                // process-shared slot's owner stamp must equal the caller
                // principal under the production face (foreign or unstamped
                // (0) refused, slot unchanged, IsolationDeny via
                // check_workspace_isolation); Soft/Off writes unchanged.
                const std::uint64_t slot_tenant =
                    (idx < g_pair_slot_tenants.size()) ? g_pair_slot_tenants[idx] : 0;
                const bool prod_pair_face =
                    ev.effect_sandbox_mode() != 0 &&
                    (ev.effect_sandbox_mode() == 2 || ::aura::core::sandbox::is_strict() ||
                     ::aura::core::provenance::multi_tenant_env_active());
                if (prod_pair_face && slot_tenant != ev.capability_tenant_id()) {
                    (void)ev.check_workspace_isolation(
                        /*target=*/ev.capability_tenant_id(), /*ref_tenant=*/slot_tenant,
                        aura::compiler::security::kEffectMutate, "set-cdr!");
                    return make_primitive_error(string_heap, error_values,
                                                "set-cdr!: cross-tenant pair slot write "
                                                "denied (#4057)");
                }
                g_pair_slots[idx]->cdr = a[1].val;
            }
            return make_void();
        },
        mutate_general(2, "(pair any) -> void", "Set the cdr of a pair."));

    register_prim(
        add, ev, "string?",
        [&pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.empty())
                return make_bool(false);
            return make_bool(is_string(a[0]));
        },
        pure_general(1, "(any) -> bool", "True if value is a string."));
    register_prim(
        add, ev, "string-append",
        [&ev, &pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) {
            // Issue #2651: overnight multi-agent long-context pads call
            // string-append heavily under fiber fanout. Unlocked push_back
            // races pmr::vector / monotonic_buffer_resource → SIGSEGV in
            // allocate/deallocate (H9). Hold alloc_storage_lock_ for the
            // entire read-intern-push critical section.
            std::lock_guard lock(ev.alloc_storage_lock_);
            std::string result;
            for (auto& v : a) {
                if (is_string(v)) {
                    auto idx = as_string_idx(v);
                    if (idx < string_heap.size()) {
                        // Issue #4505: no foreign bytes in the concatenated
                        // copy; same reader gate as string=?.
                        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0,
                                                             "string-append")) {
                            return make_primitive_error(
                                string_heap, error_values,
                                "string-append: cross-tenant string slot read denied (#4505)",
                                primitive_error_counter);
                        }
                        result += string_heap[idx];
                    }
                } else if (is_int(v)) {
                    result += std::to_string(as_int(v));
                }
            }
            // Issue #2577: content intern — repeated append of the same pieces
            // (hot PrimCall loops) reuses one heap slot instead of O(N) growth.
            for (std::size_t i = 0; i < string_heap.size(); ++i) {
                if (string_heap[i] == result)
                    return make_string(i);
            }
            // Issue #2916: soft strings quota only when a new slot is required.
            if (!ev.prim_heap_quota_allow(PrimHeapDim::Strings, string_heap.size() + 1)) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Strings)),
                    primitive_error_counter);
            }
            auto id = string_heap.size();
            string_heap.push_back(std::move(result));
            ev.note_inrange_string_mint(id); // Issue #4428
            return make_string(id);
        },
        pure_general(255, "(...strings) -> string", "Concatenate strings."));
    register_prim(
        add, ev, "string-length",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.empty())
                return make_int(0);
            std::size_t len = 0;
            // Issue #4472: code points, not UTF-8 bytes. A non-string is 0
            // (an integer is not measured as its decimal spelling).
            if (is_string(a[0])) {
                auto idx = as_string_idx(a[0]);
                if (idx < string_heap.size()) {
                    // Issue #4505: the count is foreign structure; same
                    // reader gate as the other string readers.
                    if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0,
                                                         "string-length")) {
                        return make_primitive_error(
                            string_heap, error_values,
                            "string-length: cross-tenant string slot read denied (#4505)",
                            ev.primitive_error_counter_ptr());
                    }
                    len = utf8_unit_count(string_heap[idx]);
                }
            }
            return make_int(static_cast<std::int64_t>(len));
        },
        pure_general(1, "(string) -> int",
                     "Number of Unicode code points. An ill-formed byte counts as one."));
    register_prim(
        add, ev, "string-ref",
        [&ev, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2) {
                auto __i =
                    static_cast<std::uint64_t>(ev.push_string_heap("string-ref: too few args"));
                auto __e = error_values.size();
                error_values.push_back(make_string(__i));
                return make_error(__e);
            }
            auto pos = static_cast<std::size_t>(as_int(a[1]));
            auto oob = [&]() {
                auto __i = static_cast<std::uint64_t>(
                    ev.push_string_heap("string-ref: index out of bounds"));
                auto __e = error_values.size();
                error_values.push_back(make_string(__i));
                return make_error(__e);
            };
            // Issue #4320 / #4472: one locked scan, not a copy of the heap
            // string. The result is the code point (a character fixnum).
            if (is_string(a[0])) {
                auto idx = as_string_idx(a[0]);
                // Issue #4505: the code point is payload; compare the intern
                // stamp before the locked scan. Same error shape as oob().
                if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string-ref")) {
                    auto __i = static_cast<std::uint64_t>(ev.push_string_heap(
                        "string-ref: cross-tenant string slot read denied (#4505)"));
                    auto __e = error_values.size();
                    error_values.push_back(make_string(__i));
                    return make_error(__e);
                }
                int cp = ev.string_heap_code_point_at(idx, pos);
                if (cp < 0)
                    return oob();
                return make_int(cp);
            }
            return oob();
        },
        pure_general(2, "(string int) -> int", "Code point at a character index."));
    register_prim(
        add, ev, "substring",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 3)
                return make_int(0);
            // Issue #2652: copy source under lock then push via locked helper.
            std::string s;
            if (is_string(a[0])) {
                // Issue #4505: the bytes are payload; compare the intern
                // stamp before the copy.
                if (!ev.inrange_string_tenant_allows(as_string_idx(a[0]), /*required_effects=*/0,
                                                     "substring")) {
                    return make_primitive_error(
                        string_heap, error_values,
                        "substring: cross-tenant string slot read denied (#4505)",
                        ev.primitive_error_counter_ptr());
                }
                s = ev.copy_string_heap_at(as_string_idx(a[0]));
            } else
                return make_int(0);
            // Issue #4472: indexes are code points, so a slice never splits
            // a well-formed UTF-8 sequence. Past-the-end clamps.
            auto start = static_cast<std::size_t>(as_int(a[1]));
            auto end = static_cast<std::size_t>(as_int(a[2]));
            auto start_b = utf8_byte_offset(s, start);
            auto end_b = utf8_byte_offset(s, end);
            if (start_b >= end_b)
                return make_string(static_cast<std::uint64_t>(ev.push_string_heap("")));
            return make_string(static_cast<std::uint64_t>(
                ev.push_string_heap(s.substr(start_b, end_b - start_b))));
        },
        pure_general(3, "(string int int) -> string",
                     "Substring from start to end, in code points."));
    register_prim(
        add, ev, "string=?",
        [&ev, &string_heap, &error_values](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2)
                return make_bool(false);
            // Issue #4505: a foreign operand must not answer byte equality —
            // the intern stamp compare runs before to_str reads the heap.
            auto tenant_allows = [&](const EvalValue& v) {
                if (!is_string(v))
                    return true;
                auto idx = as_string_idx(v);
                return idx >= string_heap.size() ||
                       ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string=?");
            };
            if (!tenant_allows(a[0]) || !tenant_allows(a[1])) {
                return make_primitive_error(
                    string_heap, error_values,
                    "string=?: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            }
            auto to_str = [&string_heap](const EvalValue& v) -> std::string {
                if (is_string(v)) {
                    auto idx = as_string_idx(v);
                    return (idx < string_heap.size()) ? string_heap[idx] : "";
                }
                if (is_int(v))
                    return std::to_string(as_int(v));
                return "";
            };
            return make_bool(to_str(a[0]) == to_str(a[1]));
        },
        pure_general(2, "(string string) -> bool", "String equality."));
    register_prim(
        add, ev, "string<?",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2)
                return make_bool(false);
            // Issue #4530: lexicographic compares answer byte facts — same
            // reader gate as string=? (#4505), one IsolationDeny per call.
            auto tenant_allows = [&](const EvalValue& v) {
                if (!is_string(v))
                    return true;
                auto idx = as_string_idx(v);
                return idx >= string_heap.size() ||
                       ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string<?");
            };
            if (!tenant_allows(a[0]) || !tenant_allows(a[1]))
                return make_primitive_error(
                    string_heap, error_values,
                    "string<?: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            auto to_str = [&string_heap](const EvalValue& v) -> std::string {
                if (is_string(v)) {
                    auto idx = as_string_idx(v);
                    return (idx < string_heap.size()) ? string_heap[idx] : "";
                }
                if (is_int(v))
                    return std::to_string(as_int(v));
                return "";
            };
            return make_bool(to_str(a[0]) < to_str(a[1]));
        },
        pure_general(2, "(string string) -> bool", "Lexicographic string less-than."));
    // Issue #4175: complete the R7RS string comparison suite — string>?,
    // string>=?, string<=? mirror the string<? registration shape (same
    // string domain, lexicographic compare, 2-arg minimum). Left unbound,
    // aura-build kernel modules using string>? tie-breaks failed with
    // "unbound variable: string>?" (aura/orch.aura select-best).
    register_prim(
        add, ev, "string>?",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2)
                return make_bool(false);
            // Issue #4530: same reader gate as string<? (#4505).
            auto tenant_allows = [&](const EvalValue& v) {
                if (!is_string(v))
                    return true;
                auto idx = as_string_idx(v);
                return idx >= string_heap.size() ||
                       ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string>?");
            };
            if (!tenant_allows(a[0]) || !tenant_allows(a[1]))
                return make_primitive_error(
                    string_heap, error_values,
                    "string>?: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            auto to_str = [&string_heap](const EvalValue& v) -> std::string {
                if (is_string(v)) {
                    auto idx = as_string_idx(v);
                    return (idx < string_heap.size()) ? string_heap[idx] : "";
                }
                if (is_int(v))
                    return std::to_string(as_int(v));
                return "";
            };
            return make_bool(to_str(a[0]) > to_str(a[1]));
        },
        pure_general(2, "(string string) -> bool", "Lexicographic string greater-than."));
    register_prim(
        add, ev, "string>=?",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2)
                return make_bool(false);
            // Issue #4530: same reader gate as string<? (#4505).
            auto tenant_allows = [&](const EvalValue& v) {
                if (!is_string(v))
                    return true;
                auto idx = as_string_idx(v);
                return idx >= string_heap.size() ||
                       ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string>=?");
            };
            if (!tenant_allows(a[0]) || !tenant_allows(a[1]))
                return make_primitive_error(
                    string_heap, error_values,
                    "string>=?: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            auto to_str = [&string_heap](const EvalValue& v) -> std::string {
                if (is_string(v)) {
                    auto idx = as_string_idx(v);
                    return (idx < string_heap.size()) ? string_heap[idx] : "";
                }
                if (is_int(v))
                    return std::to_string(as_int(v));
                return "";
            };
            return make_bool(to_str(a[0]) >= to_str(a[1]));
        },
        pure_general(2, "(string string) -> bool", "Lexicographic string greater-or-equal."));
    register_prim(
        add, ev, "string<=?",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2)
                return make_bool(false);
            // Issue #4530: same reader gate as string<? (#4505).
            auto tenant_allows = [&](const EvalValue& v) {
                if (!is_string(v))
                    return true;
                auto idx = as_string_idx(v);
                return idx >= string_heap.size() ||
                       ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string<=?");
            };
            if (!tenant_allows(a[0]) || !tenant_allows(a[1]))
                return make_primitive_error(
                    string_heap, error_values,
                    "string<=?: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            auto to_str = [&string_heap](const EvalValue& v) -> std::string {
                if (is_string(v)) {
                    auto idx = as_string_idx(v);
                    return (idx < string_heap.size()) ? string_heap[idx] : "";
                }
                if (is_int(v))
                    return std::to_string(as_int(v));
                return "";
            };
            return make_bool(to_str(a[0]) <= to_str(a[1]));
        },
        pure_general(2, "(string string) -> bool", "Lexicographic string less-or-equal."));
    // Both registrations share parse_string_number. The later one wins
    // name lookup; keeping the same body means the earlier slot agrees.
    register_prim(
        add, ev, "string->number",
        [&ev, &string_heap, &error_values](std::span<const EvalValue> a) {
            return parse_string_number(ev, string_heap, error_values, a);
        },
        pure_general(2, "(string int) -> any",
                     "Parse a string as a number. Optional radix 2..36."));

    register_prim(
        add, ev, "string-index",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.size() < 2)
                return make_int(-1);
            // Issue #4530: gate before haystack/needle bytes are copied.
            auto tenant_allows = [&](const EvalValue& v) {
                if (!is_string(v))
                    return true;
                auto idx = as_string_idx(v);
                return idx >= string_heap.size() ||
                       ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string-index");
            };
            if (!tenant_allows(a[0]) || !tenant_allows(a[1]))
                return make_primitive_error(
                    string_heap, error_values,
                    "string-index: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            auto haystack = (is_string(a[0]) && as_string_idx(a[0]) < string_heap.size())
                                ? string_heap[as_string_idx(a[0])]
                                : "";
            auto needle = (is_string(a[1]) && as_string_idx(a[1]) < string_heap.size())
                              ? string_heap[as_string_idx(a[1])]
                              : "";
            // Issue #4472: start and the result are code-point indexes,
            // the same unit substring uses.
            auto start_cp =
                (a.size() > 2 && is_int(a[2])) ? static_cast<std::size_t>(as_int(a[2])) : 0;
            if (needle.empty())
                return make_int(0);
            auto start_b = utf8_byte_offset(haystack, start_cp);
            auto pos = haystack.find(needle, start_b);
            if (pos == std::string::npos)
                return make_int(-1);
            return make_int(static_cast<std::int64_t>(utf8_units_before(haystack, pos)));
        },
        pure_general(3, "(string string int) -> int", "Code-point index of substring or -1."));

    register_prim(
        add, ev, "number->string",
        [&ev, &pairs, &string_heap, &error_values](std::span<const EvalValue> a) {
            if (a.empty())
                return make_int(0);
            int radix = 10;
            if (a.size() > 1) {
                if (!is_int(a[1]))
                    return radix_out_of_range(ev, error_values, "number->string");
                const auto r = as_int(a[1]);
                if (r < 2 || r > 36)
                    return radix_out_of_range(ev, error_values, "number->string");
                radix = static_cast<int>(r);
            }
            std::string s;
            // Issue #4468: display, write, and number->string share one spelling.
            // std::to_string keeps 6 digits after the point, so 1e-9 became "0".
            // Radix 10 keeps that spelling. Another radix formats integers only.
            if (is_float(a[0])) {
                if (radix != 10)
                    return radix_out_of_range(ev, error_values, "number->string");
                s = aura::compiler::format_float_shortest(as_float(a[0]));
            } else if (is_int(a[0]))
                s = format_int_radix(as_int(a[0]), radix);
            else
                s = "0";
            // Issue #2652: locked push.
            return make_string(static_cast<std::uint64_t>(ev.push_string_heap(std::move(s))));
        },
        pure_general(2, "(number int) -> string",
                     "Render a number as a string. Optional radix 2..36."));
    register_prim(
        add, ev, "string->number",
        [&ev, &string_heap, &error_values](std::span<const EvalValue> a) {
            return parse_string_number(ev, string_heap, error_values, a);
        },
        pure_general(2, "(string int) -> any",
                     "Parse a string as a number. Optional radix 2..36."));
}

} // namespace aura::compiler::primitives_detail
