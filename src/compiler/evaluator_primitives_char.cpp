// evaluator_primitives_char.cpp — P0 step 30: char? / string->list / read-line primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

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

void register_char_primitives(PrimRegistrar add, Evaluator& ev) {

    add("char?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_int(a[0]));
    });

    add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });

    add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });

    add("string->list", [&ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < ev.string_heap_.size())
                s = ev.string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });

    add("list->string", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || (!is_pair(a[0]) && !is_void(a[0])))
            return make_int(0);
        std::string result;
        auto v = a[0];
        while (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= ev.pairs_.size())
                break;
            auto car = ev.pairs_[idx].car;
            if (is_int(car))
                result.push_back(static_cast<char>(as_int(car)));
            v = ev.pairs_[idx].cdr;
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    add("read-line", [&ev](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // Issue #4055: native encoding prims. The EDSL folds in
    // lib/std/encoding.aura appended each chunk to an immutable accumulator
    // string — O(n²) copy where every intermediate became a string_heap_
    // entry; a 64 KB http-post response under Soft --serve-async denseness
    // retained ~42.8 GB RSS and wedged/OOM-killed Soft mid-batch (third
    // concurrent MiniMax batch → serve_session_timeout). These allocate ONE
    // heap entry per call; lib/std/encoding.aura delegates to them.
    add("encoding:base64-encode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        static const char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const std::string& data = ev.string_heap_[idx];
        std::string out;
        out.reserve((data.size() + 2) / 3 * 4);
        std::size_t i = 0;
        while (i + 2 < data.size()) {
            const auto b0 = static_cast<unsigned char>(data[i]);
            const auto b1 = static_cast<unsigned char>(data[i + 1]);
            const auto b2 = static_cast<unsigned char>(data[i + 2]);
            out.push_back(kAlphabet[b0 >> 2]);
            out.push_back(kAlphabet[((b0 & 0x3) << 4) | (b1 >> 4)]);
            out.push_back(kAlphabet[((b1 & 0xF) << 2) | (b2 >> 6)]);
            out.push_back(kAlphabet[b2 & 0x3F]);
            i += 3;
        }
        const std::size_t rem = data.size() - i;
        if (rem == 1) {
            const auto b0 = static_cast<unsigned char>(data[i]);
            out.push_back(kAlphabet[b0 >> 2]);
            out.push_back(kAlphabet[(b0 & 0x3) << 4]);
            out.push_back('=');
            out.push_back('=');
        } else if (rem == 2) {
            const auto b0 = static_cast<unsigned char>(data[i]);
            const auto b1 = static_cast<unsigned char>(data[i + 1]);
            out.push_back(kAlphabet[b0 >> 2]);
            out.push_back(kAlphabet[((b0 & 0x3) << 4) | (b1 >> 4)]);
            out.push_back(kAlphabet[(b1 & 0xF) << 2]);
            out.push_back('=');
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("encoding:base64-decode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const std::string& in = ev.string_heap_[idx];
        auto b64val = [](unsigned char c) -> int {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };
        std::string out;
        out.reserve(in.size() / 4 * 3);
        unsigned int acc = 0;
        int bits = 0;
        for (unsigned char c : in) {
            if (c == '=')
                break;
            const int v = b64val(c);
            if (v < 0)
                break;
            acc = (acc << 6) | static_cast<unsigned int>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((acc >> bits) & 0xFF));
            }
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("encoding:hex-encode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        static const char kHex[] = "0123456789abcdef";
        const std::string& data = ev.string_heap_[idx];
        std::string out;
        out.reserve(data.size() * 2);
        for (unsigned char c : data) {
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("encoding:hex-decode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const std::string& in = ev.string_heap_[idx];
        auto hexval = [](unsigned char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(in.size() / 2);
        int hi = -1;
        for (unsigned char c : in) {
            const int v = hexval(c);
            if (v < 0)
                break;
            if (hi < 0) {
                hi = v;
            } else {
                out.push_back(static_cast<char>((hi << 4) | v));
                hi = -1;
            }
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("eof-object?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        // EOF is represented as void (the same as when read-line returns empty)
        return make_bool(is_void(a[0]));
    });
}

} // namespace aura::compiler::primitives_detail
