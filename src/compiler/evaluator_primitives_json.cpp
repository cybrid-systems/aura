// evaluator_primitives_json.cpp — P0 step 4: JSON encode/parse primitives
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

using namespace types;

void register_json_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                              std::pmr::vector<std::string>& string_heap) {
    // json-encode: convert Aura value to JSON string
    // (json-encode value) → string
    // Supports: Int, Float, String, Bool, Void→null, Pair→array, Hash→obj
    // json-encode: convert Aura value to JSON string
    // (json-encode value) → string
    add("json-encode", [&pairs, &string_heap](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty()) {
            auto sid = string_heap.size();
            string_heap.push_back("null");
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
                    if (meta[i - 1] != 0xFF) {
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
        auto sid = string_heap.size();
        string_heap.push_back(result);
        return types::make_string(sid);
    });
    // json-get-string: extract string value of a JSON field
    // (json-get-string json-str field-name) → string
    add("json-get-string", [&pairs, &string_heap](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
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
        auto sid = string_heap.size();
        string_heap.push_back(result);
        return types::make_string(sid);
    });


    // json-parse: parse JSON string into Aura value
    // (json-parse json-str) → value (Int/Float/String/Bool/Void/List/Hash)
    add("json-parse", [&pairs, &string_heap](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto json_str = string_heap[types::as_string_idx(a[0])];

        std::size_t pos = 0;
        auto skip_ws = [&]() {
            while (pos < json_str.size() && (json_str[pos] == ' ' || json_str[pos] == '\t' ||
                                             json_str[pos] == '\n' || json_str[pos] == '\r'))
                pos++;
        };

        // Forward declarations for recursive parsing
        std::function<EvalValue()> parse_value;

        auto parse_string = [&]() -> EvalValue {
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
                        default:
                            result += json_str[pos];
                            break;
                    }
                } else {
                    result += json_str[pos];
                }
                pos++;
            }
            auto sid = string_heap.size();
            string_heap.push_back(result);
            return types::make_string(sid);
        };

        auto parse_number = [&]() -> EvalValue {
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
            if (is_float) {
                return types::make_float(std::stod(num_str));
            } else {
                return types::make_int(std::stoll(num_str));
            }
        };

        auto parse_keyword = [&](const std::string& kw, EvalValue val) -> bool {
            if (pos + kw.size() <= json_str.size() && json_str.substr(pos, kw.size()) == kw) {
                pos += kw.size();
                return true;
            }
            return false;
        };

        auto parse_array = [&]() -> EvalValue {
            pos++; // skip [
            skip_ws();
            std::vector<EvalValue> elems;
            while (pos < json_str.size() && json_str[pos] != ']') {
                skip_ws();
                elems.push_back(parse_value());
                skip_ws();
                if (pos < json_str.size() && json_str[pos] == ',')
                    pos++;
                skip_ws();
            }
            if (pos < json_str.size() && json_str[pos] == ']')
                pos++;
            // Build list in correct order
            EvalValue result = make_void();
            for (std::size_t i = elems.size(); i > 0; --i) {
                auto pid = pairs.size();
                pairs.push_back({elems[i - 1], result});
                result = types::make_pair(pid);
            }
            return result;
        };

        auto parse_object = [&]() -> EvalValue {
            pos++; // skip {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            while (pos < json_str.size() && json_str[pos] != '}') {
                skip_ws();
                auto key_val = parse_string();
                skip_ws();
                if (pos < json_str.size() && json_str[pos] == ':')
                    pos++;
                skip_ws();
                auto val = parse_value();
                // Compute hash for key (string or number keys)
                std::uint64_t kh = 0x9e3779b97f4a7c15ull;
                if (types::is_string(key_val)) {
                    auto ksid = types::as_string_idx(key_val);
                    if (ksid < string_heap.size()) {
                        auto& ks = string_heap[ksid];
                        kh = ::aura::compiler::stats::kFnvOffsetBasis;
                        for (char c : ks)
                            kh = (kh ^ static_cast<std::uint8_t>(c)) *
                                 ::aura::compiler::stats::kFnvPrime;
                    }
                } else if (types::is_int(key_val)) {
                    kh = static_cast<std::uint64_t>(types::as_int(key_val)) * 0x9e3779b97f4a7c15ull;
                }
                auto fp = static_cast<std::uint8_t>((kh >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                // Check if key already exists (need string content comparison)
                bool found = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((kh >> 1) + at) & (cap - 1);
                    if (meta[idx] != 0xFF) {
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
                if (!found) {
                    for (std::size_t at = 0; at < cap; ++at) {
                        auto idx = ((kh >> 1) + at) & (cap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_val.val;
                            vals[idx] = val.val;
                            ht->size++;
                            break;
                        }
                    }
                }
                skip_ws();
                if (pos < json_str.size() && json_str[pos] == ',')
                    pos++;
                skip_ws();
            }
            if (pos < json_str.size() && json_str[pos] == '}')
                pos++;
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return types::make_hash(hidx);
        };

        parse_value = [&]() -> EvalValue {
            skip_ws();
            if (pos >= json_str.size())
                return make_void();
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
            return make_void();
        };

        return parse_value();
    });
}

} // namespace aura::compiler::primitives_detail
