// evaluator_primitives_runtime.cpp — P0 step 26: equal? / gensym / apply / display / format / error
// / check-* primitives aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.evaluator_pure;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

using aura::compiler::pure::is_truthy;

namespace {
    // Check if value is the end of a list (void is the proper sentinel)
    // Note: int 0 is ALSO used as the empty list sentinel in some contexts,
    // but we only treat it as end-of-list in cdr chain position.
    static bool is_end_of_list(const EvalValue& v) {
        return is_void(v) || (is_int(v) && as_int(v) == 0);
    }
    // Format a value to string (same formatting as io_print_val but returns string)
    static std::string fmt_val_to_string(const EvalValue& v, std::span<const std::string> heap,
                                         std::span<const Pair> pairs, bool quote, int depth = 0) {
        std::string out;
        auto app = [&](const auto&... args) { (out += ... += args); };
        if (depth > 64)
            return "...";
        if (is_void(v))
            return "()";
        if (is_bool(v))
            return as_bool(v) ? "#t" : "#f";
        if (is_float(v))
            return std::to_string(as_float(v));
        if (is_int(v))
            return std::to_string(as_int(v));
        if (is_string(v)) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                if (quote)
                    return "\"" + heap[idx] + "\"";
                return heap[idx];
            }
            return "";
        }
        if (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return "<pair>";
            out = "(";
            out += fmt_val_to_string(pairs[idx].car, heap, pairs, quote, depth + 1);
            auto cdr = pairs[idx].cdr;
            while (is_pair(cdr)) {
                out += " ";
                auto nidx = as_pair_idx(cdr);
                if (nidx >= pairs.size()) {
                    out += "<pair>";
                    break;
                }
                out += fmt_val_to_string(pairs[nidx].car, heap, pairs, quote, depth + 1);
                cdr = pairs[nidx].cdr;
            }
            if (!is_end_of_list(cdr)) {
                out += " . ";
                out += fmt_val_to_string(cdr, heap, pairs, quote, depth + 1);
            }
            out += ")";
            return out;
        }
        if (is_vector(v))
            return std::format("<vector[{}]>", as_vector_idx(v));
        if (is_hash(v))
            return std::format("<hash[{}]>", as_hash_idx(v));
        if (is_closure(v))
            return "#<procedure>";
        return "<unknown>";
    }

    static void io_print_val(const EvalValue& v, std::span<const std::string> heap,
                             std::span<const Pair> pairs, bool quote, int depth = 0,
                             std::span<const std::string> keywords = {}) {
        if (depth > 64) {
            std::fprintf(stdout, "...");
            return;
        }
        if (is_void(v)) {
            std::fprintf(stdout, "()");
            return;
        }
        if (is_bool(v)) {
            std::fprintf(stdout, "%s", as_bool(v) ? "#t" : "#f");
            return;
        }
        // IMPORTANT: Check is_string BEFORE is_keyword (Issue #96 bug fix;
        // pre-#181 encoding rationale). The v2 encoding (Issue #181)
        // uses (v & 3) == 2 as the dedicated string tag, so this is
        // no longer a correctness concern — but the ordering is kept
        // for semantic clarity (a string at idx N is never a keyword
        // at the same value).
        if (is_string(v) && !heap.empty()) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                if (quote)
                    std::fprintf(stdout, "\"%s\"", heap[idx].c_str());
                else
                    std::fprintf(stdout, "%s", heap[idx].c_str());
                return;
            }
        }
        if (is_keyword(v)) {
            auto kidx = as_keyword_idx(v);
            if (!keywords.empty() && kidx < keywords.size()) {
                auto kname = keywords[kidx];
                std::fprintf(stdout, "%s", kname.c_str());
            } else {
                std::fprintf(stdout, ":%zu", (size_t)kidx);
            }
            return;
        }
        if (is_float(v)) {
            std::fprintf(stdout, "%g", as_float(v));
            return;
        }
        if (is_int(v)) {
            std::fprintf(stdout, "%ld", (long)as_int(v));
            return;
        }
        if (is_pair(v) && !pairs.empty()) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size()) {
                std::fprintf(stdout, "<pair[%zu]>", (size_t)idx);
                return;
            }
            // Check if it's a proper list (cdr chain ends in void or int 0 sentinel)
            auto cdr = pairs[idx].cdr;
            if (is_end_of_list(cdr) && !quote) {
                // Single-element list: (x)
                std::fprintf(stdout, "(");
                io_print_val(pairs[idx].car, heap, pairs, quote, depth + 1, keywords);
                std::fprintf(stdout, ")");
                return;
            }
            // Walk the chain to see if it's a proper list
            std::vector<EvalValue> elements;
            elements.push_back(pairs[idx].car);
            auto next = cdr;
            bool proper = true;
            while (!is_end_of_list(next)) {
                if (!is_pair(next)) {
                    proper = false;
                    break;
                }
                auto nidx = as_pair_idx(next);
                if (nidx >= pairs.size()) {
                    proper = false;
                    break;
                }
                elements.push_back(pairs[nidx].car);
                next = pairs[nidx].cdr;
            }
            std::fprintf(stdout, "(");
            for (std::size_t i = 0; i < elements.size(); ++i) {
                if (i > 0)
                    std::fprintf(stdout, " ");
                io_print_val(elements[i], heap, pairs, quote, depth + 1, keywords);
            }
            if (!is_end_of_list(next)) {
                std::fprintf(stdout, " . ");
                io_print_val(next, heap, pairs, quote, depth + 1, keywords);
            }
            std::fprintf(stdout, ")");
            return;
        }
        if (is_vector(v)) {
            std::fprintf(stdout, "<vector[%zu]>", (size_t)as_vector_idx(v));
            return;
        }
        if (is_hash(v)) {
            std::fprintf(stdout, "<hash[%zu]>", (size_t)as_hash_idx(v));
            return;
        }
        if (is_closure(v)) {
            std::fprintf(stdout, "#<procedure>");
            std::fprintf(stderr, "⚠ program returned an uncalled function\n");
            return;
        }
        if (is_cell(v)) {
            std::fprintf(stdout, "<cell[%zu]>", (size_t)as_cell_id(v));
            return;
        }
        std::fprintf(stdout, "<unknown>");
    }
} // namespace

void register_runtime_primitives(PrimRegistrar add, Evaluator& ev) {

    add("equal?", [&ev](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(true);

        struct EqCheck {
            Evaluator& e;
            bool operator()(const EvalValue& x, const EvalValue& y, int depth) const {
                if (depth > 64)
                    return true;
                if (x == y)
                    return true;
                if (is_int(x) && is_int(y))
                    return as_int(x) == as_int(y);
                if (is_float(x) && is_float(y))
                    return as_float(x) == as_float(y);
                if (is_bool(x) && is_bool(y))
                    return as_bool(x) == as_bool(y);
                if (is_string(x) && is_string(y)) {
                    auto xi = as_string_idx(x), yi = as_string_idx(y);
                    if (xi < e.string_heap_.size() && yi < e.string_heap_.size())
                        return e.string_heap_[xi] == e.string_heap_[yi];
                    return false;
                }
                if (is_pair(x) && is_pair(y)) {
                    auto xi = as_pair_idx(x), yi = as_pair_idx(y);
                    if (xi < e.pairs_.size() && yi < e.pairs_.size())
                        return (*this)(e.pairs_[xi].car, e.pairs_[yi].car, depth + 1) &&
                               (*this)(e.pairs_[xi].cdr, e.pairs_[yi].cdr, depth + 1);
                    return false;
                }
                if (is_vector(x) && is_vector(y)) {
                    auto xi = as_vector_idx(x), yi = as_vector_idx(y);
                    if (xi < e.vector_heap_.size() && yi < e.vector_heap_.size()) {
                        auto& vx = e.vector_heap_[xi];
                        auto& vy = e.vector_heap_[yi];
                        if (vx.size() != vy.size())
                            return false;
                        for (std::size_t i = 0; i < vx.size(); ++i)
                            if (!(*this)(vx[i], vy[i], depth + 1))
                                return false;
                        return true;
                    }
                    return false;
                }
                // Empty list sentinel: void or int 0
                if ((is_void(x) || (is_int(x) && as_int(x) == 0)) &&
                    (is_void(y) || (is_int(y) && as_int(y) == 0)))
                    return true;
                return false;
            }
        };

        return make_bool(EqCheck{ev}(a[0], a[1], 0));
    });

    add("gensym", [&ev](std::span<const EvalValue> a) -> EvalValue {
        static std::atomic<std::int64_t> gs_counter_{0};
        auto id = gs_counter_.fetch_add(1, std::memory_order_relaxed);
        std::string prefix = "G__";
        if (a.size() >= 1 && is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < ev.string_heap_.size()) {
                prefix = ev.string_heap_[idx] + "__";
            }
        }
        std::string name = prefix + std::to_string(id);
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(name);
        return make_string(sid);
    });

    add("symbol-append", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < ev.string_heap_.size())
                    result += ev.string_heap_[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return make_string(sid);
    });

    add("apply", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2)
            return make_void();
        auto& fn = a[0];
        auto& arg_list = a[1];
        std::vector<EvalValue> args;
        auto current = arg_list;
        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= ev.pairs_.size())
                break;
            args.push_back(ev.pairs_[idx].car);
            current = ev.pairs_[idx].cdr;
        }
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            auto pfn = ev.primitives_.lookup(ev.primitives_.name_for_slot(slot));
            if (pfn)
                return (*pfn)(args);
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = ev.apply_closure(cid, args);
            if (result)
                return *result;
        }
        return make_void();
    });

    add("display", [&ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        io_print_val(a[0], ev.string_heap_, ev.pairs_, false, 0, ev.keyword_table_);
        std::fflush(stdout);
        return make_void();
    });

    add("write", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        io_print_val(a[0], ev.string_heap_, ev.pairs_, true, 0, ev.keyword_table_);
        std::fflush(stdout);
        return make_void();
    });

    add("newline", [](const auto&) -> EvalValue {
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
        return make_void();
    });

    add("format", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto tidx = as_string_idx(a[0]);
        if (tidx >= ev.string_heap_.size())
            return make_bool(false);
        auto& tmpl = ev.string_heap_[tidx];
        std::string result;
        std::size_t arg_idx = 1; // first arg in a[1..]
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '~' && i + 1 < tmpl.size()) {
                switch (tmpl[i + 1]) {
                    case 'a': // display arg
                        if (arg_idx < a.size()) {
                            auto val = a[arg_idx++];
                            result += fmt_val_to_string(val, ev.string_heap_, ev.pairs_, false);
                        }
                        ++i;
                        break;
                    case 's': // write arg (quoted)
                        if (arg_idx < a.size()) {
                            auto val = a[arg_idx++];
                            result += fmt_val_to_string(val, ev.string_heap_, ev.pairs_, true);
                        }
                        ++i;
                        break;
                    case '%': // newline
                        result += '\n';
                        ++i;
                        break;
                    case '~': // literal ~
                        result += '~';
                        ++i;
                        break;
                    default:
                        result += tmpl[i];
                        break;
                }
            } else {
                result += tmpl[i];
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return make_string(sidx);
    });

    add("error", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Ensure ev.error_values_[0] always exists for default errors
        if (ev.error_values_.empty())
            ev.error_values_.push_back(make_void());
        types::EvalValue cause = make_string(0); // default
        if (!a.empty())
            cause = a[0];
        auto eidx = ev.error_values_.size();
        ev.error_values_.push_back(cause);
        return make_error(eidx);
    });

    add("assert", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (!a.empty() && is_truthy(a[0]))
            return make_int(1);
        // Assertion failed — return error
        types::EvalValue cause = make_string(0);
        if (a.size() > 1)
            cause = a[1];
        auto eidx = ev.error_values_.size();
        ev.error_values_.push_back(cause);
        return make_error(eidx);
    });

    add("raise", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto cause = a.empty() ? make_void() : a[0];
        auto eidx = ev.error_values_.size();
        ev.error_values_.push_back(cause);
        return make_error(eidx);
    });

    add("error?", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_bool(false);
        // Guard against encoding collision: strings can accidentally pass is_error()
        return make_bool(is_error(a[0]) && !is_string(a[0]));
    });

    add("check", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_error(0);
        if (is_truthy(a[0]))
            return make_int(1);
        // Store failing value as error cause
        auto eidx = ev.error_values_.size();
        ev.error_values_.push_back(a[0]);
        return make_error(eidx);
    });

    add("check=", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2)
            return make_bool(false);
        if (types::is_void(a[0]) && types::is_void(a[1]))
            return make_int(1);
        if (types::is_int(a[0]) && types::is_int(a[1])) {
            if (types::as_int(a[0]) == types::as_int(a[1]))
                return make_int(1);
            auto eidx = ev.error_values_.size();
            ev.error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_float(a[0]) && types::is_float(a[1])) {
            if (types::as_float(a[0]) == types::as_float(a[1]))
                return make_int(1);
            auto eidx = ev.error_values_.size();
            ev.error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_string(a[0]) && types::is_string(a[1])) {
            auto i0 = types::as_string_idx(a[0]);
            auto i1 = types::as_string_idx(a[1]);
            if (i0 < ev.string_heap_.size() && i1 < ev.string_heap_.size() &&
                ev.string_heap_[i0] == ev.string_heap_[i1])
                return make_int(1);
            auto eidx = ev.error_values_.size();
            ev.error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        return make_bool(false);
    });

    add("check-success", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_pair(a[1]))
            return make_bool(false);
        // Get normalized output (strip quotes)
        auto out_idx = as_string_idx(a[0]);
        if (out_idx >= ev.string_heap_.size())
            return make_bool(false);
        auto norm_out = ev.string_heap_[out_idx];
        // Strip surrounding quotes
        if (!norm_out.empty() && (norm_out[0] == '"' || norm_out[0] == '\''))
            norm_out = norm_out.substr(1);
        if (!norm_out.empty() && (norm_out.back() == '"' || norm_out.back() == '\''))
            norm_out.pop_back();
        // Detect if output looks like an error message
        auto lower = norm_out;
        for (auto& c : lower)
            c = std::tolower(static_cast<unsigned char>(c));
        bool is_error_like = lower.find("error:") != std::string::npos ||
                             lower.find("parse error") != std::string::npos ||
                             lower.find("unbound variable") != std::string::npos ||
                             lower.find("type error") != std::string::npos ||
                             lower.find("syntax error") != std::string::npos ||
                             lower.find("invalid syntax") != std::string::npos ||
                             lower.find("expected expression") != std::string::npos ||
                             (norm_out.size() > 1 && norm_out[0] == '(' && norm_out[1] == '"');
        // Iterate expected list
        auto pair_idx = as_pair_idx(a[1]);
        while (pair_idx < ev.pairs_.size()) {
            auto& p = ev.pairs_[pair_idx];
            if (is_string(p.car)) {
                auto kw_idx = as_string_idx(p.car);
                if (kw_idx < ev.string_heap_.size()) {
                    auto& kw = ev.string_heap_[kw_idx];
                    if (!kw.empty() && norm_out.find(kw) != std::string::npos) {
                        // Guard: for error-like output with short keywords
                        if (is_error_like && kw.size() <= 5) {
                            // Generic error words that should never match in error output
                            static const std::unordered_set<std::string> generic_words = {
                                "error", "type", "parse", "syntax", "kind"};
                            if (generic_words.count(kw)) {
                                // Skip — too generic, likely part of error message
                                if (is_pair(p.cdr)) {
                                    pair_idx = as_pair_idx(p.cdr);
                                    continue;
                                }
                                break;
                            }
                            // Word boundary check for other short keywords
                            auto pos = norm_out.find(kw);
                            bool word_boundary = true;
                            if (pos > 0) {
                                char prev = norm_out[pos - 1];
                                if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')
                                    word_boundary = false;
                            }
                            if (pos + kw.size() < norm_out.size()) {
                                char next = norm_out[pos + kw.size()];
                                if (std::isalnum(static_cast<unsigned char>(next)) || next == '_')
                                    word_boundary = false;
                            }
                            if (!word_boundary) {
                                // Move to next keyword
                                if (is_pair(p.cdr)) {
                                    pair_idx = as_pair_idx(p.cdr);
                                    continue;
                                }
                                break;
                            }
                        }
                        return make_bool(true);
                    }
                }
            }
            if (is_pair(p.cdr))
                pair_idx = as_pair_idx(p.cdr);
            else
                break;
        }
        return make_bool(false);
    });
}

} // namespace aura::compiler::primitives_detail
