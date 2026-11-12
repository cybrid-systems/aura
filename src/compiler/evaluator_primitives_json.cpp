// evaluator_primitives_json.cpp — P0 step 4: JSON encode/parse primitives
// Issue #2996: migrate evaluator_primitives_json.cpp onto register_prim + PrimSpec.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;
#include "compiler/locked_string_heap.hh" // Issue #4543

#include "runtime_shared.h"
#include "prim_heap_quota.hh" // Issue #2916 / #2997
#include "hash_meta.h"        // FNV constants (#901)

#include <stdexcept> // Issue #2480: out_of_range / invalid_argument from stod/stoll

#include "prim_registrar_scaffold.hh" // Issue #2996
#include "compiler/evo_log_4910.hh"   // Issue #4910: evo:record!

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function_ref<void(std::string, PrimFn)>;

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

// Issue #5032: json-parse state and its recursive helpers live outside the
// registration lambda so every helper stays under 300 lines. Member names match
// the old captures, so the parser body did not change in the extraction.
// Issue #5152: the source parser refuses past 512 frames. json-parse used to
// recurse once per bracket until the native stack died with SIGSEGV. 512 is
// the same cap; the evolution-log reader stays stricter (64).
inline constexpr std::size_t kJsonParseMaxNesting = 512;

struct JsonParser {
    std::string json_str;
    std::size_t pos = 0;
    std::pmr::vector<Pair>& pairs;
    ::aura::compiler::LockedStringHeap& string_heap;
    std::vector<EvalValue>& error_values;
    std::atomic<std::uint64_t>* primitive_error_counter;
    Evaluator& ev;
    std::size_t depth = 0;
    bool nesting_refused = false;

    void skip_ws();
    EvalValue parse_string();
    EvalValue parse_number();
    bool parse_keyword(std::string_view kw, EvalValue val);
    EvalValue nesting_too_deep();
    EvalValue parse_array();
    std::uint64_t json_object_key_hash(const EvalValue& key_val);
    EvalValue parse_object();
    EvalValue parse_value();
};

void JsonParser::skip_ws() {
    while (pos < json_str.size() && (json_str[pos] == ' ' || json_str[pos] == '\t' ||
                                     json_str[pos] == '\n' || json_str[pos] == '\r'))
        pos++;
}
EvalValue JsonParser::parse_string() {
    // Expected at ": advance past it
    if (pos >= json_str.size() || json_str[pos] != '"')
        return make_void();
    pos++; // skip opening quote
    std::string result;
    while (pos < json_str.size()) {
        if (json_str[pos] == '"') {
            pos++;
            break;
        }
        if (json_str[pos] == '\\' && pos + 1 < json_str.size()) {
            pos++;
            switch (json_str[pos]) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 'u': {
                    // Issue #1715: decode \uXXXX to UTF-8 (BMP).
                    if (pos + 4 < json_str.size()) {
                        unsigned code = 0;
                        bool ok = true;
                        for (int i = 1; i <= 4; ++i) {
                            char h = json_str[pos + i];
                            code <<= 4;
                            if (h >= '0' && h <= '9')
                                code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                code |= static_cast<unsigned>(h - 'A' + 10);
                            else {
                                ok = false;
                                break;
                            }
                        }
                        if (ok) {
                            pos += 4; // last hex; loop pos++ advances past it
                            if (code < 0x80) {
                                result += static_cast<char>(code);
                            } else if (code < 0x800) {
                                result += static_cast<char>(0xC0 | (code >> 6));
                                result += static_cast<char>(0x80 | (code & 0x3F));
                            } else {
                                result += static_cast<char>(0xE0 | (code >> 12));
                                result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                result += static_cast<char>(0x80 | (code & 0x3F));
                            }
                            break;
                        }
                    }
                    result += 'u';
                    break;
                }
                default:
                    result += json_str[pos];
                    break;
            }
        } else {
            result += json_str[pos];
        }
        pos++;
    }
    if (!ev.prim_heap_quota_allow(PrimHeapDim::Strings, string_heap.size() + 1)) {
        return make_primitive_error(string_heap, error_values,
                                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Strings)),
                                    primitive_error_counter);
    }
    auto sid = string_heap.mint(result);
    return types::make_string(sid);
}
EvalValue JsonParser::parse_number() {
    std::size_t start = pos;
    bool is_float = false;
    if (pos < json_str.size() && json_str[pos] == '-')
        pos++;
    while (pos < json_str.size() && json_str[pos] >= '0' && json_str[pos] <= '9')
        pos++;
    if (pos < json_str.size() && json_str[pos] == '.') {
        is_float = true;
        pos++;
        while (pos < json_str.size() && json_str[pos] >= '0' && json_str[pos] <= '9')
            pos++;
    }
    if (pos < json_str.size() && (json_str[pos] == 'e' || json_str[pos] == 'E')) {
        is_float = true;
        pos++;
        if (pos < json_str.size() && (json_str[pos] == '+' || json_str[pos] == '-'))
            pos++;
        while (pos < json_str.size() && json_str[pos] >= '0' && json_str[pos] <= '9')
            pos++;
    }
    auto num_str = json_str.substr(start, pos - start);
    // Issue #2480: stod/stoll throw invalid_argument / out_of_range —
    // must not escape the primitive (fiber crash). Return PRIM_ERROR.
    try {
        if (is_float)
            return types::make_float(std::stod(num_str));
        return types::make_int_or_promote(std::stoll(num_str));
    } catch (const std::out_of_range&) {
        // [SILENCE-PRIM-#615] structured error path (#2480).
        return make_primitive_error(string_heap, error_values, "json-parse: number out of range",
                                    primitive_error_counter);
    } catch (const std::invalid_argument&) {
        // [SILENCE-PRIM-#615] structured error path (#2480).
        return make_primitive_error(string_heap, error_values, "json-parse: invalid number format",
                                    primitive_error_counter);
    }
}
bool JsonParser::parse_keyword(std::string_view kw, EvalValue val) {
    if (pos + kw.size() <= json_str.size() && json_str.substr(pos, kw.size()) == kw) {
        pos += kw.size();
        return true;
    }
    return false;
}
EvalValue JsonParser::nesting_too_deep() {
    nesting_refused = true;
    return make_primitive_error(string_heap, error_values, "json-parse: nesting too deep",
                                primitive_error_counter);
}
EvalValue JsonParser::parse_array() {
    // Issue #5152: refuse before the recursive parse_value frame.
    if (nesting_refused)
        return make_void();
    if (depth >= kJsonParseMaxNesting)
        return nesting_too_deep();
    ++depth;
    pos++; // skip [
    skip_ws();
    std::vector<EvalValue> elems;
    while (pos < json_str.size() && json_str[pos] != ']') {
        skip_ws();
        const auto before = pos;
        auto elem = parse_value();
        if (nesting_refused)
            return elem;
        elems.push_back(std::move(elem));
        // Advance on stuck parse to avoid infinite loops (#1715).
        if (pos == before) {
            if (pos < json_str.size())
                ++pos;
            else
                break;
        }
        skip_ws();
        if (pos < json_str.size() && json_str[pos] == ',')
            pos++;
        skip_ws();
    }
    if (pos < json_str.size() && json_str[pos] == ']')
        pos++;
    --depth;
    // Issue #2916 / #2997: lock only materialization; one allow + reserve.
    // Issue #4453: geometric pairs reserve (monotonic arena; not size+n).
    Evaluator::ListCtorLockHold hold(ev);
    const auto n = elems.size();
    if ((ev.prim_heap_quota_limited(PrimHeapDim::Pairs) || n > kPrimHeapUnlimitedSmall) &&
        !ev.prim_heap_quota_allow(PrimHeapDim::Pairs, pairs.size() + n)) {
        return make_primitive_error(string_heap, error_values,
                                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Pairs)),
                                    primitive_error_counter);
    }
    pairs.reserve(monotonic_pairs_reserve_cap(pairs.size(), pairs.capacity(), n));
    EvalValue result = make_void();
    for (std::size_t i = n; i > 0; --i) {
        auto pid = pairs.size();
        pairs.push_back({elems[i - 1], result});
        result = types::make_pair(pid);
    }
    return result;
}
std::uint64_t JsonParser::json_object_key_hash(const EvalValue& key_val) {
    std::uint64_t kh = 0x9e3779b97f4a7c15ull;
    if (types::is_string(key_val)) {
        auto ksid = types::as_string_idx(key_val);
        if (ksid < string_heap.size()) {
            auto& ks = string_heap[ksid];
            kh = ::aura::compiler::stats::kFnvOffsetBasis;
            for (char c : ks)
                kh = (kh ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
        }
    } else if (types::is_int(key_val)) {
        kh = static_cast<std::uint64_t>(types::as_int(key_val)) * 0x9e3779b97f4a7c15ull;
    }
    return kh;
}
EvalValue JsonParser::parse_object() {
    // Issue #5152: same nesting cap as parse_array.
    if (nesting_refused)
        return make_void();
    if (depth >= kJsonParseMaxNesting)
        return nesting_too_deep();
    ++depth;
    pos++; // skip {
    // Issue #2997: snapshot k/v then reserve capacity (no parse-time grow).
    std::vector<std::pair<EvalValue, EvalValue>> entries;
    while (pos < json_str.size() && json_str[pos] != '}') {
        skip_ws();
        const auto before = pos;
        auto key_val = parse_string();
        skip_ws();
        if (pos < json_str.size() && json_str[pos] == ':')
            pos++;
        skip_ws();
        auto val = parse_value();
        if (nesting_refused)
            return val;
        // Advance on stuck parse to avoid infinite loops (#1715).
        if (pos == before) {
            if (pos < json_str.size())
                ++pos;
            else
                break;
        }
        entries.emplace_back(key_val, val);
        skip_ws();
        if (pos < json_str.size() && json_str[pos] == ',')
            pos++;
        skip_ws();
    }
    --depth;
    if (pos < json_str.size() && json_str[pos] == '}')
        pos++;

    // Issue #2481: start create(8), grow to 0.7 load so keys are not dropped.
    auto* ht = FlatHashTable::create(8);
    if (!ht)
        return make_void();

    auto grow_object_table = [&]() -> bool {
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
            // Issue #4612: a deleted slot (tombstone) must not be
            // resurrected by the rehash.
            if (old_meta[i] == ::aura::compiler::hash::kTombstoneSlot)
                continue;
            EvalValue kv{old_keys[i]};
            auto kh = json_object_key_hash(kv);
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
        FlatHashTable::destroy(ht);
        ht = new_ht;
        return true;
    };

    while (entries.size() * 10 > ht->capacity * 7) {
        if (!grow_object_table()) {
            FlatHashTable::destroy(ht);
            return make_primitive_error(string_heap, error_values,
                                        "json-parse: hash table grow failed",
                                        primitive_error_counter);
        }
    }

    for (auto& [key_val, val] : entries) {
        auto kh = json_object_key_hash(key_val);
        const auto fp = ::aura::compiler::hash::fingerprint(kh);

        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto cap = ht->capacity;

        bool found = false;
        for (std::size_t at = 0; at < cap; ++at) {
            auto idx = ((kh >> 1) + at) & (cap - 1);
            if (::aura::compiler::hash::hash_slot_is_live(meta[idx])) {
                bool eq = false;
                auto existing_key = EvalValue{keys[idx]};
                if (types::is_string(existing_key) && types::is_string(key_val)) {
                    auto ai = types::as_string_idx(existing_key);
                    auto bi = types::as_string_idx(key_val);
                    eq = (ai < string_heap.size() && bi < string_heap.size()) &&
                         string_heap[ai] == string_heap[bi];
                } else {
                    eq = keys[idx] == key_val.val;
                }
                if (eq) {
                    vals[idx] = val.val;
                    found = true;
                    break;
                }
            }
        }
        if (found)
            continue;
        if (ht->size * 10 > ht->capacity * 7) {
            if (!grow_object_table()) {
                FlatHashTable::destroy(ht);
                return make_primitive_error(string_heap, error_values,
                                            "json-parse: hash table grow failed",
                                            primitive_error_counter);
            }
            meta = ht->metadata();
            keys = ht->keys();
            vals = ht->values();
            cap = ht->capacity;
        }
        bool inserted = false;
        for (std::size_t at = 0; at < cap; ++at) {
            auto idx = ((kh >> 1) + at) & (cap - 1);
            if (meta[idx] == ::aura::compiler::hash::kEmptySlot) {
                meta[idx] = fp;
                keys[idx] = key_val.val;
                vals[idx] = val.val;
                ht->size++;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            if (!grow_object_table()) {
                FlatHashTable::destroy(ht);
                return make_primitive_error(string_heap, error_values,
                                            "json-parse: hash table grow failed",
                                            primitive_error_counter);
            }
            meta = ht->metadata();
            keys = ht->keys();
            vals = ht->values();
            cap = ht->capacity;
            for (std::size_t at = 0; at < cap; ++at) {
                auto idx = ((kh >> 1) + at) & (cap - 1);
                if (meta[idx] == ::aura::compiler::hash::kEmptySlot) {
                    meta[idx] = fp;
                    keys[idx] = key_val.val;
                    vals[idx] = val.val;
                    ht->size++;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                FlatHashTable::destroy(ht);
                return make_primitive_error(string_heap, error_values,
                                            "json-parse: hash table insert failed",
                                            primitive_error_counter);
            }
        }
    }
    // Issue #4788: publish under the same locks every other
    // g_hash_tables writer takes ((hash), hash-set! grow, the
    // reclaim). json-parse runs on fiber worker threads, so the
    // unlocked size()+push_back raced a concurrent publish: two
    // threads reallocating the vector freed the same block
    // (glibc "double free or corruption") and could also hand
    // out the same index twice. Index read, push and owner
    // stamp are one critical section now.
    std::size_t hidx = 0;
    {
        std::scoped_lock hlock(ev.hash_tables_mutex(), ev.alloc_storage_lock_);
        hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        // Issue #4110: stamp the owner parallel to the table (SSOT helper;
        // runtime_ssot contract).
        aura_hash_stamp_new_table_owner();
    }
    return types::make_hash(hidx);
}
EvalValue JsonParser::parse_value() {
    skip_ws();
    if (pos >= json_str.size()) {
        return make_primitive_error(string_heap, error_values,
                                    "json-parse: unexpected end of input", primitive_error_counter);
    }
    char c = json_str[pos];
    if (c == '"')
        return parse_string();
    if (c == '-' || (c >= '0' && c <= '9'))
        return parse_number();
    if (c == '[')
        return parse_array();
    if (c == '{')
        return parse_object();
    if (parse_keyword("true", make_bool(true)))
        return make_bool(true);
    if (parse_keyword("false", make_bool(false)))
        return make_bool(false);
    if (parse_keyword("null", make_void()))
        return make_void();
    // Issue #2998: invalid token is a true parse error, not silent void.
    return make_primitive_error(string_heap, error_values, "json-parse: invalid input",
                                primitive_error_counter);
}

void RegSplitParts::register_json_evo_primitives(
    PrimRegistrar add, std::pmr::vector<Pair>& pairs,
    ::aura::compiler::LockedStringHeap& string_heap, std::vector<EvalValue>& error_values,
    std::atomic<std::uint64_t>* primitive_error_counter, Evaluator& ev) {
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    // Issue #4910: (evo:record! candidate decision reason evidence [new-src [dur-ms [parent]]])
    // appends one worldline record to AURA_EVO_LOG (see evo_log_4910.hh) so user
    // gates land in the same log as mutate:* / ast:restore. Evidence is any
    // json-encode-able value (hash / list / number / string). Returns the
    // record id, or #f when the log is off (AURA_EVO_LOG unset).
    //
    // Issue #4939: an optional 8th argument is a hash of v=2 members
    // (multi-objective / constrained / multi-parent / behavior records).
    // They are emitted in the FIXED order below — never in hash iteration
    // order — so two identical runs write byte-identical lines. An unknown
    // key is an error, not a silent drop: a stage that logs "behavior" must
    // not lose it.
    static constexpr std::string_view kEvoV2Keys[] = {"algo",     "gen",        "slot",
                                                      "search",   "candidate",  "parents",
                                                      "behavior", "objectives", "constraints"};
    register_prim(
        add, ev, "evo:record!",
        [&ev, &string_heap, &pairs, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) -> EvalValue {
            if (!aura::evo::log_enabled())
                return make_bool(false);
            auto text = [&](std::size_t i) -> std::string {
                if (i >= a.size())
                    return {};
                if (is_string(a[i])) {
                    const auto idx = as_string_idx(a[i]);
                    return idx < string_heap.size() ? std::string(ev.string_at(idx))
                                                    : std::string();
                }
                if (is_int(a[i]))
                    return std::to_string(as_int(a[i]));
                if (is_bool(a[i]))
                    return as_bool(a[i]) ? "true" : "false";
                return {};
            };
            auto json = [&](std::size_t i) -> std::string {
                if (i >= a.size())
                    return {};
                auto fn = ev.primitives().lookup("json-encode");
                if (!fn)
                    return {};
                const EvalValue r = (*fn)({a[i]});
                if (!is_string(r))
                    return {};
                const auto idx = as_string_idx(r);
                return idx < string_heap.size() ? std::string(ev.string_at(idx)) : std::string();
            };
            std::string decision = text(1);
            std::string up = decision;
            for (auto& c : up)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (up == "KEEP" || up == "ROLLBACK" || up == "REJECT")
                decision = up == "REJECT" ? "ROLLBACK" : up;
            double dur = 0.0;
            if (a.size() > 5) {
                if (is_int(a[5]))
                    dur = static_cast<double>(as_int(a[5]));
                else if (is_float(a[5]))
                    dur = as_float(a[5]);
            }
            const std::string target = text(0);
            const std::string reason = text(2);
            const std::string evidence = json(3);
            const std::string new_src = text(4);
            const std::string parent = text(6);
            // Issue #4939: v=2 members.
            int version = 1;
            std::string extras;
            if (a.size() > 7 && !is_void(a[7])) {
                if (!is_hash(a[7]))
                    return make_primitive_error(string_heap, error_values,
                                                "evo:record!: v2 members must be a hash",
                                                primitive_error_counter);
                auto hkeys = ev.primitives().lookup("hash-keys");
                auto href = ev.primitives().lookup("hash-ref");
                if (!hkeys || !href)
                    return make_primitive_error(string_heap, error_values,
                                                "evo:record!: hash-keys/hash-ref unavailable",
                                                primitive_error_counter);
                const EvalValue ks = (*hkeys)({a[7]});
                std::vector<std::string> names;
                for (EvalValue cur = ks; is_pair(cur); cur = pairs[as_pair_idx(cur)].cdr) {
                    const EvalValue k = pairs[as_pair_idx(cur)].car;
                    if (!is_string(k))
                        continue;
                    const auto ki = as_string_idx(k);
                    const std::string name =
                        ki < string_heap.size() ? std::string(ev.string_at(ki)) : std::string();
                    bool known = false;
                    for (const auto known_key : kEvoV2Keys)
                        known = known || name == known_key;
                    if (!known)
                        return make_primitive_error(
                            string_heap, error_values,
                            "evo:record!: unknown v2 key '" + name +
                                "' (allowed: algo gen slot search candidate parents behavior "
                                "objectives constraints)",
                            primitive_error_counter);
                    names.push_back(name);
                }
                version = 2;
                bool first = true;
                for (const auto known_key : kEvoV2Keys) {
                    // Presence comes from the key scan above, not from the
                    // value: () is a legitimate member (an empty parents
                    // list) and is indistinguishable from "absent" as a
                    // hash-ref default.
                    if (std::find(names.begin(), names.end(), known_key) == names.end())
                        continue;
                    const auto key = types::make_string(string_heap.mint(std::string(known_key)));
                    const EvalValue v = (*href)({a[7], key});
                    auto fn = ev.primitives().lookup("json-encode");
                    if (!fn)
                        return make_primitive_error(string_heap, error_values,
                                                    "evo:record!: json-encode unavailable",
                                                    primitive_error_counter);
                    const EvalValue enc = (*fn)({v});
                    if (!is_string(enc))
                        return make_primitive_error(string_heap, error_values,
                                                    "evo:record!: v2 member '" +
                                                        std::string(known_key) +
                                                        "' is not json-encode-able",
                                                    primitive_error_counter);
                    const auto ei = as_string_idx(enc);
                    if (ei >= string_heap.size())
                        continue;
                    if (!first)
                        extras += ",";
                    first = false;
                    aura::evo::json_escape_into(extras, known_key);
                    extras += ":";
                    extras += ev.string_at(ei);
                }
            }
            const std::string id = aura::evo::log_append({.kind = "record",
                                                          .target = target,
                                                          .decision = decision,
                                                          .reason = reason,
                                                          .evidence_json = evidence,
                                                          .old_src = {},
                                                          .new_src = new_src,
                                                          .parent = parent,
                                                          .dur_ms = dur,
                                                          .extras_json = extras,
                                                          .version = version});
            if (id.empty())
                return make_bool(false);
            return types::make_string(string_heap.mint(id));
        },
        aura::compiler::impure_general(
            255,
            "(evo:record! candidate decision reason evidence [new-src dur-ms parent extras]) -> "
            "id|#f",
            "Append a worldline record to the evolution log (AURA_EVO_LOG; #4910/#4939: "
            "an extras hash writes schema v=2)."));

    // Issue #4910: (evo:log-path) -> the active evolution log path, or #f.
    register_prim(
        add, ev, "evo:log-path",
        [&string_heap](std::span<const EvalValue>) -> EvalValue {
            const std::string p = aura::evo::log_path();
            if (p.empty())
                return make_bool(false);
            return types::make_string(string_heap.mint(p));
        },
        aura::compiler::impure_general(0, "(evo:log-path) -> string|#f",
                                       "Active evolution log path (AURA_EVO_LOG), or #f (#4910)."));
}

void RegSplitParts::register_json_codec_primitives(
    PrimRegistrar add, std::pmr::vector<Pair>& pairs,
    ::aura::compiler::LockedStringHeap& string_heap, std::vector<EvalValue>& error_values,
    std::atomic<std::uint64_t>* primitive_error_counter, Evaluator& ev) {
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    // json-encode: convert Aura value to JSON string
    // (json-encode value) → string
    // Supports: Int, Float, String, Bool, Void→null, Pair→array, Hash→obj
    // json-encode: convert Aura value to JSON string
    // (json-encode value) → string
    // Issue #2916: soft string quota on result interning under multi-fiber loops.
    register_prim(
        add, ev, "json-encode",
        [&pairs, &string_heap, &error_values, primitive_error_counter,
         &ev](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty()) {
                if (!ev.prim_heap_quota_allow(PrimHeapDim::Strings, string_heap.size() + 1)) {
                    return make_primitive_error(
                        string_heap, error_values,
                        std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Strings)),
                        primitive_error_counter);
                }
                auto sid = string_heap.mint("null");
                return types::make_string(sid);
            }

            // Use explicit std::function for recursion
            std::function<std::string(const types::EvalValue&)> to_json;
            to_json = [&](const types::EvalValue& v) -> std::string {
                if (types::is_int(v))
                    return std::to_string(types::as_int(v));
                if (types::is_float(v)) {
                    auto s = std::to_string(types::as_float(v));
                    if (s.find('.') != std::string::npos) {
                        s.erase(s.find_last_not_of('0') + 1, std::string::npos);
                        if (s.back() == '.')
                            s.pop_back();
                    }
                    return s;
                }
                if (types::is_string(v)) {
                    auto idx = types::as_string_idx(v);
                    std::string str = (idx < string_heap.size()) ? string_heap[idx] : "";
                    std::string r = "\"";
                    for (auto c : str) {
                        switch (c) {
                            case '"':
                                r += "\\\"";
                                break;
                            case '\\':
                                r += "\\\\";
                                break;
                            case '\n':
                                r += "\\n";
                                break;
                            case '\r':
                                r += "\\r";
                                break;
                            case '\t':
                                r += "\\t";
                                break;
                            default:
                                r += c;
                        }
                    }
                    r += '\"';
                    return r;
                }
                if (types::is_bool(v))
                    return types::as_bool(v) ? "true" : "false";
                if (types::is_void(v))
                    return "null";
                if (types::is_pair(v)) {
                    std::string r = "[";
                    bool first = true;
                    auto cur = v;
                    while (types::is_pair(cur)) {
                        auto pidx = types::as_pair_idx(cur);
                        if (pidx >= pairs.size())
                            break;
                        if (!first)
                            r += ",";
                        first = false;
                        r += to_json(pairs[pidx].car);
                        cur = pairs[pidx].cdr;
                    }
                    if (!types::is_void(cur)) {
                        r += ",";
                        r += to_json(cur);
                    }
                    r += "]";
                    return r;
                }

                if (types::is_hash(v)) {
                    auto hidx = types::as_hash_idx(v);
                    if (hidx >= g_hash_tables.size() || !g_hash_tables[hidx])
                        return "null";
                    auto* ht = g_hash_tables[hidx];
                    auto meta = ht->metadata();
                    auto keys = ht->keys();
                    auto vals = ht->values();
                    std::string r = "{";
                    bool first = true;
                    for (std::size_t i = ht->capacity; i > 0; --i) {
                        // Issue #4612: a deleted slot (tombstone) is not a live entry.
                        if (::aura::compiler::hash::hash_slot_is_live(meta[i - 1])) {
                            if (!first)
                                r += ",";
                            first = false;
                            r += to_json(EvalValue{keys[i - 1]});
                            r += ":";
                            r += to_json(EvalValue{vals[i - 1]});
                        }
                    }
                    r += "}";
                    return r;
                }
                return "null";
            };

            auto result = to_json(a[0]);
            if (!ev.prim_heap_quota_allow(PrimHeapDim::Strings, string_heap.size() + 1)) {
                return make_primitive_error(
                    string_heap, error_values,
                    std::string(prim_heap_quota_exceeded_msg(PrimHeapDim::Strings)),
                    primitive_error_counter);
            }
            auto sid = string_heap.mint(result);
            return types::make_string(sid);
        },
        pure_general(1, "(any) -> string", "Encode an Aura value as a JSON string."));
    // json-get-string: extract string value of a JSON field
    // (json-get-string json-str field-name) → string
    register_prim(
        add, ev, "json-get-string",
        [&pairs, &string_heap, &error_values,
         primitive_error_counter](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1])) {
                return make_primitive_error(string_heap, error_values,
                                            "json-get-string: expected two strings",
                                            primitive_error_counter);
            }
            auto json = string_heap[types::as_string_idx(a[0])];
            auto field = string_heap[types::as_string_idx(a[1])];

            // Search for "fieldName":" in the JSON string
            std::string search = "\"" + field + "\":\"";
            std::size_t pos = json.find(search);
            if (pos == std::string::npos)
                return make_void();
            std::size_t start = pos + search.size();
            // Read until closing quote (handle escaped quotes)
            std::string result;
            for (std::size_t i = start; i < json.size(); ++i) {
                if (json[i] == '"')
                    break;
                if (json[i] == '\\' && i + 1 < json.size()) {
                    ++i;
                    if (json[i] == '"')
                        result += '"';
                    else if (json[i] == 'n')
                        result += '\n';
                    else if (json[i] == 't')
                        result += '\t';
                    else if (json[i] == 'r')
                        result += '\r';
                    else {
                        result += '\\';
                        result += json[i];
                    }
                } else {
                    result += json[i];
                }
            }
            auto sid = string_heap.mint(result);
            return types::make_string(sid);
        },
        pure_general(2, "(any string) -> string", "Lookup a string field from JSON/hash."));
}

void RegSplitParts::register_json_parse_primitive(
    PrimRegistrar add, std::pmr::vector<Pair>& pairs,
    ::aura::compiler::LockedStringHeap& string_heap, std::vector<EvalValue>& error_values,
    std::atomic<std::uint64_t>* primitive_error_counter, Evaluator& ev) {
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    // json-parse: parse JSON string into Aura value
    // (json-parse json-str) -> value (Int/Float/String/Bool/Void/List/Hash)
    register_prim(
        add, ev, "json-parse",
        [&pairs, &string_heap, &error_values, primitive_error_counter,
         &ev](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !types::is_string(a[0])) {
                return make_primitive_error(string_heap, error_values,
                                            "json-parse: expected a string",
                                            primitive_error_counter);
            }
            JsonParser parser{string_heap[types::as_string_idx(a[0])],
                              0,
                              pairs,
                              string_heap,
                              error_values,
                              primitive_error_counter,
                              ev};
            return parser.parse_value();
        },
        pure_general(1, "(string) -> any",
                     "Parse a JSON string into an Aura value. Nesting deeper than 512 "
                     "returns json-parse: nesting too deep."));
}

void register_json_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                              ::aura::compiler::LockedStringHeap& string_heap,
                              std::vector<EvalValue>& error_values,
                              std::atomic<std::uint64_t>* primitive_error_counter, Evaluator& ev) {
    RegSplitParts::register_json_evo_primitives(add, pairs, string_heap, error_values,
                                                primitive_error_counter, ev);
    RegSplitParts::register_json_codec_primitives(add, pairs, string_heap, error_values,
                                                  primitive_error_counter, ev);
    RegSplitParts::register_json_parse_primitive(add, pairs, string_heap, error_values,
                                                 primitive_error_counter, ev);
}

} // namespace aura::compiler::primitives_detail
