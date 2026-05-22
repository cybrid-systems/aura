module;
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <regex>
#include <cmath>
module aura.compiler.evaluator;
import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.diag;
import aura.parser.parser;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

// ── Edit distance for error suggestions ────────────────────────
[[maybe_unused]] static std::size_t edit_distance(std::string_view a, std::string_view b) {
    auto m = a.size(), n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;
    // Use two-row DP for efficiency
    std::vector<std::size_t> prev(n + 1), cur(n + 1);
    for (std::size_t j = 0; j <= n; ++j) prev[j] = j;
    for (std::size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= n; ++j) {
            auto cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

static std::string closest_match(std::string_view name, const std::vector<std::string>& candidates, std::size_t max_dist = 3) {
    std::string best; std::size_t best_dist = max_dist + 1;
    for (auto& c : candidates) {
        auto d = edit_distance(name, c);
        if (d < best_dist) { best_dist = d; best = c; }
    }
    return best;
}


using namespace aura::diag;

// Forward decl: macro body cloner (defined at end of file)
static aura::ast::NodeId clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
    aura::ast::FlatAST& source, aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst = nullptr,
    std::unordered_map<std::string, std::string>* name_map = nullptr);

// ── Env::lookup: returns EvalValue variant ─────────────────────
std::optional<EvalValue> Env::lookup(const std::string& n) const {
    // 1. Check local bindings
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            auto& v = it->second;
            // If the binding is a cell reference, dereference it
            if (is_cell(v) && cells_) {
                auto idx = as_cell_id(v);
                if (idx < cells_->size()) return (*cells_)[idx];
            }
            return v;
        }
    // 2. Check parent
    if (parent_) return parent_->lookup(n);
    // 3. Fallback: check primitives (allows passing names like `+` as values)
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    return std::nullopt;
}

// ── Env::lookup_binding: returns raw binding (cell sentinel as-is) ─
std::optional<EvalValue> Env::lookup_binding(const std::string& n) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) return it->second;
    return parent_ ? parent_->lookup_binding(n) : std::nullopt;
}

// ── Helper: coerce EvalValue to int (string → int parsing) ────
namespace {
    static std::int64_t coerce_to_int(const EvalValue& v, const std::vector<std::string>* heap) {
        if (is_int(v)) return as_int(v);
        if (is_float(v)) return static_cast<std::int64_t>(as_float(v)); // truncate
        if (is_string(v) && heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size()) {
                try { return static_cast<std::int64_t>(std::stoll((*heap)[idx])); }
                catch (...) { return 0; }
            }
        }
        if (is_bool(v)) return as_bool(v) ? 1 : 0;
        return 0;
    }

    [[maybe_unused]] static double coerce_to_double(const EvalValue& v, const std::vector<std::string>* heap) {
        if (is_float(v)) return as_float(v);
        return static_cast<double>(coerce_to_int(v, heap));
    }
}

// ── Primitives: EvalValue operations ──────────────────────────

Primitives::Primitives() {
    // ── Variadic arithmetic ────────────────────────────────────────
    // (+) → 0, (+ x) → x, (+ x y ...) → sum; float if any arg is float
    table_["+"] = [this](auto& a) {
        if (a.empty()) return make_int(0);
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        if (any_f) {
            double r = 0.0;
            for (auto& v : a) r += is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
            return make_float(r);
        }
        std::int64_t r = 0;
        for (auto& v : a) r += coerce_to_int(v, string_heap_);
        return make_int(r);
    };
    // (-) → 0, (- x) → -x, (- x y ...) → x - y - z - ...
    table_["-"] = [this](auto& a) {
        if (a.empty()) return make_int(0);
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        auto to_f = [this](const EvalValue& v) { return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_)); };
        if (any_f) {
            if (a.size() == 1) return make_float(-to_f(a[0]));
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i) r -= to_f(a[i]);
            return make_float(r);
        }
        if (a.size() == 1) return make_int(-coerce_to_int(a[0], string_heap_));
        std::int64_t r = coerce_to_int(a[0], string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i) r -= coerce_to_int(a[i], string_heap_);
        return make_int(r);
    };
    // (*) → 1, (* x) → x, (* x y ...) → product; float if any arg is float
    table_["*"] = [this](auto& a) {
        if (a.empty()) return make_int(1);
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        if (any_f) {
            double r = 1.0;
            for (auto& v : a) r *= is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
            return make_float(r);
        }
        std::int64_t r = 1;
        for (auto& v : a) r *= coerce_to_int(v, string_heap_);
        return make_int(r);
    };
    // (/) → 1, (/ x) → 1.0/x (float reciprocal), (/ x y ...) → x / y / z / ...
    table_["/"] = [this](auto& a) {
        if (a.empty()) return make_int(1);
        auto to_f = [this](const EvalValue& v) { return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_)); };
        if (a.size() == 1) {
            double x = to_f(a[0]);
            return (x == 0.0) ? make_int(0) : make_float(1.0 / x);
        }
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        if (any_f) {
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i) {
                double d = to_f(a[i]);
                r = (d == 0.0) ? 0.0 : (r / d);
            }
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto d = coerce_to_int(a[i], string_heap_);
            r = (d == 0) ? 0 : (r / d);
        }
        return make_int(r);
    };
    auto chain_cmp = [this](const auto& a, auto fn_int, auto fn_float) -> EvalValue {
        if (a.size() < 2) return make_bool(true);
        auto to_f = [this](const EvalValue& v) -> double {
            return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
        };
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        if (any_f) {
            for (std::size_t i = 1; i < a.size(); ++i)
                if (!fn_float(to_f(a[i-1]), to_f(a[i]))) return make_bool(false);
            return make_bool(true);
        }
        for (std::size_t i = 1; i < a.size(); ++i)
            if (!fn_int(coerce_to_int(a[i-1], string_heap_), coerce_to_int(a[i], string_heap_))) return make_bool(false);
        return make_bool(true);
    };
    table_["="]  = [chain_cmp](auto& a) { return chain_cmp(a, [](auto x, auto y){ return x == y; }, [](auto x, auto y){ return x == y; }); };
    table_["<"]  = [chain_cmp](auto& a) { return chain_cmp(a, [](auto x, auto y){ return x < y; }, [](auto x, auto y){ return x < y; }); };
    table_[">"]  = [chain_cmp](auto& a) { return chain_cmp(a, [](auto x, auto y){ return x > y; }, [](auto x, auto y){ return x > y; }); };
    table_["<="] = [chain_cmp](auto& a) { return chain_cmp(a, [](auto x, auto y){ return x <= y; }, [](auto x, auto y){ return x <= y; }); };
    table_[">="] = [chain_cmp](auto& a) { return chain_cmp(a, [](auto x, auto y){ return x >= y; }, [](auto x, auto y){ return x >= y; }); };
    // Ghuloum Step 9: booleans
    table_["not"]  = [](auto& a) { return make_bool(a.empty() || !is_truthy(a[0])); };
    table_["and"]  = [](auto& a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (!is_truthy(a[i])) return a[i];
        return a.empty() ? make_int(1) : a.back();
    };
    table_["or"]   = [](auto& a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (is_truthy(a[i])) return a[i];
        return a.empty() ? make_int(0) : a.back();
    };
    table_["eq?"]  = [](auto& a) { return make_bool(a.size() >= 2 && a[0] == a[1]); };
    table_["current-time"]  = [](auto& a) { (void)a; return make_int(static_cast<std::int64_t>(::time(nullptr))); };
    // Populate ordered_names_ with all primitives registered directly via table_[]
    for (auto& [name, _] : table_) {
        if (std::find(ordered_names_.begin(), ordered_names_.end(), name) == ordered_names_.end()) {
            ordered_names_.push_back(name);
        }
    }
}

// ── I/O helper for EvalValue ──────────────────────────────────
struct Pair;
namespace {
    // Check if value is the end of a list (void is the proper sentinel)
    // Note: int 0 is ALSO used as the empty list sentinel in some contexts,
    // but we only treat it as end-of-list in cdr chain position.
    static bool is_end_of_list(const EvalValue& v) {
        return is_void(v) || (is_int(v) && as_int(v) == 0);
    }
    // Format a value to string (same formatting as io_print_val but returns string)
    static std::string fmt_val_to_string(const EvalValue& v, const std::vector<std::string>& heap,
                                          const std::vector<Pair>& pairs, bool quote, int depth = 0) {
        std::string out;
        auto app = [&](const auto&... args) { (out += ... += args); };
        if (depth > 64) return "...";
        if (is_void(v))         return "()";
        if (is_bool(v))         return as_bool(v) ? "#t" : "#f";
        if (is_float(v))        return std::to_string(as_float(v));
        if (is_int(v))          return std::to_string(as_int(v));
        if (is_string(v)) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                if (quote) return "\"" + heap[idx] + "\"";
                return heap[idx];
            }
            return "";
        }
        if (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size()) return "<pair>";
            out = "(";
            out += fmt_val_to_string(pairs[idx].car, heap, pairs, quote, depth + 1);
            auto cdr = pairs[idx].cdr;
            while (is_pair(cdr)) {
                out += " ";
                auto nidx = as_pair_idx(cdr);
                if (nidx >= pairs.size()) { out += "<pair>"; break; }
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
        if (is_vector(v))       return std::format("<vector[{}]>", as_vector_idx(v));
        if (is_hash(v))         return std::format("<hash[{}]>", as_hash_idx(v));
        if (is_closure(v))      return "#<procedure>";
        return "<unknown>";
    }

    static void io_print_val(const EvalValue& v, const std::vector<std::string>* heap,
                             const std::vector<Pair>* pairs, bool quote, int depth = 0) {
        if (depth > 64) { std::fprintf(stdout, "..."); return; }
        if (is_void(v))         { std::fprintf(stdout, "()"); return; }
        if (is_bool(v))         { std::fprintf(stdout, "%s", as_bool(v) ? "#t" : "#f"); return; }
        if (is_float(v))        { std::fprintf(stdout, "%g", as_float(v)); return; }
        if (is_int(v))          { std::fprintf(stdout, "%ld", (long)as_int(v)); return; }
        if (is_string(v) && heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size()) {
                if (quote) std::fprintf(stdout, "\"%s\"", (*heap)[idx].c_str());
                else       std::fprintf(stdout, "%s",       (*heap)[idx].c_str());
                return;
            }
        }
        if (is_pair(v) && pairs) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs->size()) { std::fprintf(stdout, "<pair[%zu]>", (size_t)idx); return; }
            // Check if it's a proper list (cdr chain ends in void or int 0 sentinel)
            auto cdr = (*pairs)[idx].cdr;
            if (is_end_of_list(cdr) && !quote) {
                // Single-element list: (x)
                std::fprintf(stdout, "(");
                io_print_val((*pairs)[idx].car, heap, pairs, quote, depth + 1);
                std::fprintf(stdout, ")");
                return;
            }
            // Walk the chain to see if it's a proper list
            std::vector<EvalValue> elements;
            elements.push_back((*pairs)[idx].car);
            auto next = cdr;
            bool proper = true;
            while (!is_end_of_list(next)) {
                if (!is_pair(next)) { proper = false; break; }
                auto nidx = as_pair_idx(next);
                if (nidx >= pairs->size()) { proper = false; break; }
                elements.push_back((*pairs)[nidx].car);
                next = (*pairs)[nidx].cdr;
            }
            std::fprintf(stdout, "(");
            for (std::size_t i = 0; i < elements.size(); ++i) {
                if (i > 0) std::fprintf(stdout, " ");
                io_print_val(elements[i], heap, pairs, quote, depth + 1);
            }
            if (!is_end_of_list(next)) {
                std::fprintf(stdout, " . ");
                io_print_val(next, heap, pairs, quote, depth + 1);
            }
            std::fprintf(stdout, ")");
            return;
        }
        if (is_vector(v))       { std::fprintf(stdout, "<vector[%zu]>", (size_t)as_vector_idx(v)); return; }
        if (is_hash(v))         { std::fprintf(stdout, "<hash[%zu]>", (size_t)as_hash_idx(v)); return; }
        if (is_closure(v))      { std::fprintf(stdout, "#<procedure>"); std::fprintf(stderr, "⚠ program returned an uncalled function\n"); return; }
        if (is_cell(v))         { std::fprintf(stdout, "<cell[%zu]>", (size_t)as_cell_id(v)); return; }
        std::fprintf(stdout, "<unknown>");
    }
}

void Evaluator::init_pair_primitives() {
    // ── Type predicates ──────────────────────────────────────────
    primitives_.add("integer?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_int(a[0]));
    });
    primitives_.add("float?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_float(a[0]));
    });
    primitives_.add("boolean?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_bool(a[0]));
    });
    primitives_.add("number?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_int(a[0]) || is_float(a[0]));
    });
    primitives_.add("symbol?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        // Symbols are interned during parsing and not represented as
        // first-class EvalValue values; always return false.
        return make_bool(false);
    });
    primitives_.add("procedure?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_closure(a[0]) || is_primitive(a[0]));
    });
    primitives_.add("void?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_void(a[0]));
    });
    // ── Character operations (chars are integers in Aura) ──────────
    primitives_.add("char=?", [](const auto& a) {
        if (a.size() < 2) return make_bool(false);
        return make_bool(is_int(a[0]) && is_int(a[1]) && as_int(a[0]) == as_int(a[1]));
    });
    primitives_.add("char<?", [](const auto& a) {
        if (a.size() < 2) return make_bool(false);
        return make_bool(is_int(a[0]) && is_int(a[1]) && as_int(a[0]) < as_int(a[1]));
    });
    primitives_.add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        return a[0];
    });
    primitives_.add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        return a[0];
    });
    // ── Character predicates ──────────────────────────────────────
    primitives_.add("char-alphabetic?", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        auto c = as_int(a[0]);
        return make_bool((c >= 65 && c <= 90) || (c >= 97 && c <= 122));
    });
    primitives_.add("char-numeric?", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        return make_bool(as_int(a[0]) >= 48 && as_int(a[0]) <= 57);
    });
    primitives_.add("char-whitespace?", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        auto c = as_int(a[0]);
        return make_bool(c == 32 || (c >= 9 && c <= 13));
    });
    primitives_.add("char-upcase", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        auto c = as_int(a[0]);
        if (c >= 97 && c <= 122) return make_int(c - 32);
        return make_int(c);
    });
    primitives_.add("char-downcase", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_bool(false);
        auto c = as_int(a[0]);
        if (c >= 65 && c <= 90) return make_int(c + 32);
        return make_int(c);
    });
    // ── String operations ─────────────────────────────────────────
    primitives_.add("string-copy", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_bool(false);
        // Strings are immutable-like; just return the same reference
        return a[0];
    });
    primitives_.add("string-fill!", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto fill_char = static_cast<char>(as_int(a[1]));
        std::fill(string_heap_[idx].begin(), string_heap_[idx].end(), fill_char);
        return make_void();
    });
    primitives_.add("string->list", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_bool(false);
        auto& s = string_heap_[idx];
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    primitives_.add("list->string", [this](const auto& a) {
        if (a.empty()) return make_bool(false);
        auto v = a[0];
        std::string result;
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= pairs_.size()) break;
            auto car = pairs_[p].car;
            if (is_int(car)) {
                result += static_cast<char>(as_int(car));
            } else if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < string_heap_.size()) result += string_heap_[sidx];
            }
            v = pairs_[p].cdr;
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });
    primitives_.add("string-join", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[1])) return make_bool(false);
        auto delim_idx = as_string_idx(a[1]);
        if (delim_idx >= string_heap_.size()) return make_bool(false);
        auto& delim = string_heap_[delim_idx];
        std::string result;
        bool first = true;
        auto v = a[0];
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= pairs_.size()) break;
            auto car = pairs_[p].car;
            if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < string_heap_.size()) {
                    if (!first) result += delim;
                    result += string_heap_[sidx];
                    first = false;
                }
            }
            v = pairs_[p].cdr;
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });

    // ── Pair / List / String primitives ─────────────────────────
    primitives_.add("cons", [this](const auto& a) {
        auto id = pairs_.size();
        pairs_.push_back({a[0], a[1]});
        return make_pair(id);
    });
    primitives_.add("car", [this](const auto& a) {
        if (!is_pair(a[0])) { do {
    auto __e_sidx = string_heap_.size();
    string_heap_.push_back("car: not a pair");
    auto __e_eidx = error_values_.size();
    error_values_.push_back(make_string(__e_sidx));
    return make_error(__e_eidx);
} while(0); }
        auto id = as_pair_idx(a[0]);
        return id < pairs_.size() ? pairs_[id].car : make_int(0);
    });
    primitives_.add("cdr", [this](const auto& a) {
        if (!is_pair(a[0])) { do {
    auto __e_sidx = string_heap_.size();
    string_heap_.push_back("cdr: not a pair");
    auto __e_eidx = error_values_.size();
    error_values_.push_back(make_string(__e_sidx));
    return make_error(__e_eidx);
} while(0); }
        auto id = as_pair_idx(a[0]);
        return id < pairs_.size() ? pairs_[id].cdr : make_int(0);
    });
    primitives_.add("pair?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_pair(a[0]));
    });

    // ── Cadr / Caddr shorthands ────────────────────────────────────
    primitives_.add("caar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c)) return make_void();
        return pairs_[as_pair_idx(c)].car;
    });
    primitives_.add("cadr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        return pairs_[as_pair_idx(c)].car;
    });
    primitives_.add("cdar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c)) return make_void();
        return pairs_[as_pair_idx(c)].cdr;
    });
    primitives_.add("cddr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        return pairs_[as_pair_idx(c)].cdr;
    });
    primitives_.add("caaar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].car;
        return is_pair(c) ? pairs_[as_pair_idx(c)].car : make_void();
    });
    primitives_.add("caadr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        return pairs_[as_pair_idx(c)].car;
    });
    primitives_.add("cadar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c)) return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d)) return make_void();
        return pairs_[as_pair_idx(d)].car;
    });
    primitives_.add("caddr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d)) return make_void();
        return pairs_[as_pair_idx(d)].car;
    });
    primitives_.add("cdaar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c)) return make_void();
        return pairs_[as_pair_idx(c)].cdr;
    });
    primitives_.add("cdadr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        auto d = pairs_[as_pair_idx(c)].car;
        if (!is_pair(d)) return make_void();
        return pairs_[as_pair_idx(d)].cdr;
    });
    primitives_.add("cddar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d)) return make_void();
        return pairs_[as_pair_idx(d)].cdr;
    });
    primitives_.add("cdddr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c)) return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d)) return make_void();
        return pairs_[as_pair_idx(d)].cdr;
    });

    // ── Mutable pair operations ───────────────────────────────────
    primitives_.add("set-car!", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        pairs_[idx].car = a[1];
        return make_void();
    });
    primitives_.add("set-cdr!", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_pair(a[0])) return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size()) return make_void();
        pairs_[idx].cdr = a[1];
        return make_void();
    });

    primitives_.add("string?", [this](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_string(a[0]));
    });
    // json-encode: convert Aura value to JSON string
    // (json-encode value) → string
    // Supports: Int, Float, String, Bool, Void→null, Pair→array, Hash→obj
    // json-encode: convert Aura value to JSON string
    // (json-encode value) → string
    primitives_.add("json-encode", [this](const auto& a) -> EvalValue {
        if (a.empty()) {
            auto sid = string_heap_.size();
            string_heap_.push_back("null");
            return types::make_string(sid);
        }

        // Use explicit std::function for recursion
        std::function<std::string(const types::EvalValue&)> to_json;
        to_json = [&](const types::EvalValue& v) -> std::string {
            if (types::is_int(v)) return std::to_string(types::as_int(v));
            if (types::is_float(v)) {
                auto s = std::to_string(types::as_float(v));
                if (s.find('.') != std::string::npos) {
                    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
                    if (s.back() == '.') s.pop_back();
                }
                return s;
            }
            if (types::is_string(v)) {
                auto idx = types::as_string_idx(v);
                std::string str = (idx < string_heap_.size()) ? string_heap_[idx] : "";
                std::string r = "\"";
                for (auto c : str) {
                    switch (c) {
                        case '"': r += "\\\""; break;
                        case '\\': r += "\\\\"; break;
                        case '\n': r += "\\n"; break;
                        case '\r': r += "\\r"; break;
                        case '\t': r += "\\t"; break;
                        default: r += c;
                    }
                }
                r += '\"';
                return r;
            }
            if (types::is_bool(v)) return types::as_bool(v) ? "true" : "false";
            if (types::is_void(v)) return "null";
            if (types::is_pair(v)) {
                std::string r = "[";
                bool first = true;
                auto cur = v;
                while (types::is_pair(cur)) {
                    auto pidx = types::as_pair_idx(cur);
                    if (pidx >= pairs_.size()) break;
                    if (!first) r += ",";
                    first = false;
                    r += to_json(pairs_[pidx].car);
                    cur = pairs_[pidx].cdr;
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
                if (hidx >= hash_heap_.size()) return "null";
                auto& ht = hash_heap_[hidx];
                std::string r = "{";
                bool first = true;
                for (std::size_t i = ht.capacity; i > 0; --i) {
                    if (ht.metadata[i - 1] != 0xFF) {
                        if (!first) r += ",";
                        first = false;
                        r += to_json(ht.keys[i - 1]);
                        r += ":";
                        r += to_json(ht.values[i - 1]);
                    }
                }
                r += "}";
                return r;
            }
            return "null";
        };

        auto result = to_json(a[0]);
        auto sid = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sid);
    });
    // json-get-string: extract string value of a JSON field
    // (json-get-string json-str field-name) → string
    primitives_.add("json-get-string", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        auto json = string_heap_[types::as_string_idx(a[0])];
        auto field = string_heap_[types::as_string_idx(a[1])];

        // Search for "fieldName":" in the JSON string
        std::string search = "\"" + field + "\":\"";
        std::size_t pos = json.find(search);
        if (pos == std::string::npos) return make_void();
        std::size_t start = pos + search.size();
        // Read until closing quote (handle escaped quotes)
        std::string result;
        for (std::size_t i = start; i < json.size(); ++i) {
            if (json[i] == '"') break;
            if (json[i] == '\\' && i + 1 < json.size()) {
                ++i;
                if (json[i] == '"') result += '"';
                else if (json[i] == 'n') result += '\n';
                else if (json[i] == 't') result += '\t';
                else if (json[i] == 'r') result += '\r';
                else { result += '\\'; result += json[i]; }
            } else {
                result += json[i];
            }
        }
        auto sid = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sid);
    });



    
    // json-parse: parse JSON string into Aura value
    // (json-parse json-str) → value (Int/Float/String/Bool/Void/List/Hash)
    primitives_.add("json-parse", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) return make_void();
        auto json_str = string_heap_[types::as_string_idx(a[0])];

        std::size_t pos = 0;
        auto skip_ws = [&]() {
            while (pos < json_str.size() && (json_str[pos] == ' ' || json_str[pos] == '\t' || json_str[pos] == '\n' || json_str[pos] == '\r'))
                pos++;
        };

        // Forward declarations for recursive parsing
        std::function<EvalValue()> parse_value;

        auto parse_string = [&]() -> EvalValue {
            // Expected at ": advance past it
            if (pos >= json_str.size() || json_str[pos] != '"') return make_void();
            pos++; // skip opening quote
            std::string result;
            while (pos < json_str.size()) {
                if (json_str[pos] == '"') { pos++; break; }
                if (json_str[pos] == '\\' && pos + 1 < json_str.size()) {
                    pos++;
                    switch (json_str[pos]) {
                        case '"': result += '"'; break;
                        case '\\': result += '\\'; break;
                        case '/': result += '/'; break;
                        case 'b': result += '\b'; break;
                        case 'f': result += '\f'; break;
                        case 'n': result += '\n'; break;
                        case 't': result += '\t'; break;
                        case 'r': result += '\r'; break;
                        default: result += json_str[pos]; break;
                    }
                } else {
                    result += json_str[pos];
                }
                pos++;
            }
            auto sid = string_heap_.size();
            string_heap_.push_back(result);
            return types::make_string(sid);
        };

        auto parse_number = [&]() -> EvalValue {
            std::size_t start = pos;
            bool is_float = false;
            if (pos < json_str.size() && json_str[pos] == '-') pos++;
            while (pos < json_str.size() && json_str[pos] >= '0' && json_str[pos] <= '9') pos++;
            if (pos < json_str.size() && json_str[pos] == '.') {
                is_float = true;
                pos++;
                while (pos < json_str.size() && json_str[pos] >= '0' && json_str[pos] <= '9') pos++;
            }
            if (pos < json_str.size() && (json_str[pos] == 'e' || json_str[pos] == 'E')) {
                is_float = true;
                pos++;
                if (pos < json_str.size() && (json_str[pos] == '+' || json_str[pos] == '-')) pos++;
                while (pos < json_str.size() && json_str[pos] >= '0' && json_str[pos] <= '9') pos++;
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
                if (pos < json_str.size() && json_str[pos] == ',') pos++;
                skip_ws();
            }
            if (pos < json_str.size() && json_str[pos] == ']') pos++;
            // Build list in correct order
            EvalValue result = make_void();
            for (std::size_t i = elems.size(); i > 0; --i) {
                auto pid = pairs_.size();
                pairs_.push_back({elems[i - 1], result});
                result = types::make_pair(pid);
            }
            return result;
        };

        auto parse_object = [&]() -> EvalValue {
            pos++; // skip {
            HashTable ht;
            ht.capacity = 8;
            ht.metadata.resize(ht.capacity, 0xFF);
            ht.keys.resize(ht.capacity);
            ht.values.resize(ht.capacity);
            while (pos < json_str.size() && json_str[pos] != '}') {
                skip_ws();
                auto key_val = parse_string();
                skip_ws();
                if (pos < json_str.size() && json_str[pos] == ':') pos++;
                skip_ws();
                auto val = parse_value();
                // Compute hash for key (string or number keys)
                std::uint64_t kh = 0x9e3779b97f4a7c15ull;
                if (types::is_string(key_val)) {
                    auto ksid = types::as_string_idx(key_val);
                    if (ksid < string_heap_.size()) {
                        auto& ks = string_heap_[ksid];
                        kh = 0xcbf29ce484222325ull;
                        for (char c : ks) kh = (kh ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    }
                } else if (types::is_int(key_val)) {
                    kh = static_cast<std::uint64_t>(types::as_int(key_val)) * 0x9e3779b97f4a7c15ull;
                }
                auto fp = static_cast<std::uint8_t>(kh >> 57) | 0x80;
                // Check if key already exists (need string content comparison)
                bool found = false;
                for (std::size_t at = 0; at < ht.capacity; ++at) {
                    auto idx = ((kh >> 1) + at) & (ht.capacity - 1);
                    if (ht.metadata[idx] != 0xFF) {
                        bool eq = false;
                        auto& existing_key = ht.keys[idx];
                        if (types::is_string(existing_key) && types::is_string(key_val)) {
                            auto ai = types::as_string_idx(existing_key);
                            auto bi = types::as_string_idx(key_val);
                            eq = (ai < string_heap_.size() && bi < string_heap_.size()) && string_heap_[ai] == string_heap_[bi];
                        } else {
                            eq = existing_key == key_val;
                        }
                        if (eq) { ht.values[idx] = val; found = true; break; }
                    }
                }
                if (!found) {
                    for (std::size_t at = 0; at < ht.capacity; ++at) {
                        auto idx = ((kh >> 1) + at) & (ht.capacity - 1);
                        if (ht.metadata[idx] == 0xFF) {
                            ht.metadata[idx] = fp; ht.keys[idx] = key_val; ht.values[idx] = val;
                            ht.size++;
                            break;
                        }
                    }
                }
                skip_ws();
                if (pos < json_str.size() && json_str[pos] == ',') pos++;
                skip_ws();
            }
            if (pos < json_str.size() && json_str[pos] == '}') pos++;
            auto hidx = hash_heap_.size();
            hash_heap_.push_back(std::move(ht));
            return types::make_hash(hidx);
        };

        parse_value = [&]() -> EvalValue {
            skip_ws();
            if (pos >= json_str.size()) return make_void();
            char c = json_str[pos];
            if (c == '"') return parse_string();
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
            if (c == '[') return parse_array();
            if (c == '{') return parse_object();
            if (parse_keyword("true", make_bool(true))) return make_bool(true);
            if (parse_keyword("false", make_bool(false))) return make_bool(false);
            if (parse_keyword("null", make_void())) return make_void();
            return make_void();
        };

        return parse_value();
    });

primitives_.add("string-append", [this](const auto& a) {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < string_heap_.size()) result += string_heap_[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return make_string(id);
    });
    primitives_.add("string-length", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        std::size_t len = 0;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            len = (idx < string_heap_.size()) ? string_heap_[idx].size() : 0;
        } else if (is_int(a[0])) {
            len = std::to_string(as_int(a[0])).size();
        }
        return make_int(static_cast<std::int64_t>(len));
    });
    primitives_.add("string-ref", [this](const auto& a) {
        if (a.size() < 2) { auto __i = string_heap_.size(); string_heap_.push_back("string-ref: too few args"); auto __e = error_values_.size(); error_values_.push_back(make_string(__i)); return make_error(__e); }
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size()) s = string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (pos >= s.size()) { auto __i = string_heap_.size(); string_heap_.push_back("string-ref: index out of bounds"); auto __e = error_values_.size(); error_values_.push_back(make_string(__i)); return make_error(__e); }
        if (pos < s.size())
            return make_int(static_cast<std::int64_t>(static_cast<unsigned char>(s[pos])));
        return make_int(0);
    });
    primitives_.add("substring", [this](const auto& a) {
        if (a.size() < 3) return make_int(0);
        std::string s_buf;
        const std::string* sp = nullptr;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size()) sp = &string_heap_[idx];
        } else if (is_int(a[0])) {
            s_buf = std::to_string(as_int(a[0]));
            sp = &s_buf;
        }
        if (!sp) return make_int(0);
        const auto& s = *sp;
        auto start = static_cast<std::size_t>(as_int(a[1]));
        auto end   = static_cast<std::size_t>(as_int(a[2]));
        if (start > s.size()) start = s.size();
        if (end > s.size()) end = s.size();
        if (start >= end) {
            auto id = string_heap_.size();
            string_heap_.push_back("");
            return make_string(id);
        }
        auto sub = s.substr(start, end - start);
        auto nid = string_heap_.size();
        string_heap_.push_back(std::move(sub));
        return make_string(nid);
    });
    primitives_.add("string=?", [this](const auto& a) {
        if (a.size() < 2) return make_bool(false);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (is_int(v)) return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) == to_str(a[1]));
    });
    primitives_.add("string<?", [this](const auto& a) {
        if (a.size() < 2) return make_bool(false);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (is_int(v)) return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) < to_str(a[1]));
    });
    primitives_.add("number->string", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        std::string s;
        if (is_float(a[0])) s = std::to_string(as_float(a[0]));
        else if (is_int(a[0])) s = std::to_string(as_int(a[0]));
        else s = "0";
        // Trim trailing zeros from float representation
        if (is_float(a[0])) {
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last > dot) s = s.substr(0, last + 1);
                else s = s.substr(0, dot);
            }
        }
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(s));
        return make_string(id);
    });
    primitives_.add("string->number", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= string_heap_.size()) return make_bool(false);
        auto& str = string_heap_[i];
        if (str.empty()) return make_bool(false);
        // Trim leading/trailing whitespace (CSV fields, user input)
        auto first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return make_bool(false);
        auto last = str.find_last_not_of(" \t\r\n");
        std::string_view trimmed(str.data() + first, last - first + 1);
        // Use from_chars — entire trimmed string must be consumed
        const char* start = trimmed.data();
        const char* end = start + trimmed.size();
        // Try float first (includes ints like "42" → 42.0)
        double fval;
        auto [pfloat, ec_float] = std::from_chars(start, end, fval);
        if (ec_float == std::errc{} && pfloat == end) {
            // Check if it has a decimal point or exponent → return float
            if (trimmed.find('.') != std::string_view::npos || trimmed.find('e') != std::string_view::npos || trimmed.find('E') != std::string_view::npos)
                return make_float(fval);
            return make_int(static_cast<std::int64_t>(fval));
        }
        return make_bool(false);
    });
    primitives_.add("list", [this](const auto& a) {
        // Build proper list (pair chain ending with void)
        EvalValue result = make_void();
        for (auto it = a.rbegin(); it != a.rend(); ++it) {
            auto id = pairs_.size();
            pairs_.push_back({*it, result});
            result = make_pair(id);
        }
        return result;
    });
    primitives_.add("list?", [this](const auto& a) {
        if (a.empty()) return make_bool(true);
        auto v = a[0];
        while (!is_end_of_list(v)) {
            if (!is_pair(v)) return make_bool(false);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return make_bool(false);
            v = pairs_[idx].cdr;  // follow cdr chain
        }
        return make_int(1);
    });
    primitives_.add("null?", [](const auto& a) {
        return make_bool(!a.empty() && (is_void(a[0]) || (is_int(a[0]) && as_int(a[0]) == 0)));
    });
    primitives_.add("length", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        auto v = a[0]; std::int64_t n = 0;
        while (!is_end_of_list(v)) {
            if (!is_pair(v)) return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return make_int(0);
            v = pairs_[idx].cdr; n++;
        }
        return make_int(n);
    });
    primitives_.add("list-ref", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto v = a[0]; auto pos = static_cast<std::size_t>(as_int(a[1]));
        for (std::size_t i = 0; i < pos; ++i) {
            if (!is_pair(v)) return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return make_int(0);
            v = pairs_[idx].cdr;
        }
        if (is_pair(v)) {
            auto idx = as_pair_idx(v);
            return idx < pairs_.size() ? pairs_[idx].car : make_int(0);
        }
        return v;
    });
    primitives_.add("member", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto& val = a[0]; auto v = a[1];
        while (!is_end_of_list(v)) {
            if (!is_pair(v)) return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return make_int(0);
            if (pairs_[idx].car == val) return v;
            v = pairs_[idx].cdr;
        }
        return make_int(0);
    });
    // (append list ...) — Variadic: concatenate all provided lists
    primitives_.add("append", [this](const auto& a) {
        if (a.empty()) return make_void();
        if (a.size() < 2) return a[0];
        // Iteratively append all arguments
        auto result = a[0];
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto list2 = a[i];
            if (is_end_of_list(result)) { result = list2; continue; }
            EvalValue new_result = make_void(); EvalValue tail = make_void();
            auto v = result;
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) { result = a[0]; break; }
                auto idx = as_pair_idx(v);
                if (idx >= pairs_.size()) { result = a[0]; break; }
                auto new_id = pairs_.size();
                pairs_.push_back({pairs_[idx].car, make_void()});
                auto new_pair = make_pair(new_id);
                if (is_void(new_result)) new_result = new_pair;
                else {
                    auto tidx = as_pair_idx(tail);
                    pairs_[tidx].cdr = new_pair;
                }
                tail = new_pair;
                v = pairs_[idx].cdr;
            }
            if (!is_void(tail)) {
                auto tidx = as_pair_idx(tail);
                pairs_[tidx].cdr = list2;
            }
            if (!is_void(new_result)) result = new_result;
        }
        return result;
    });
    primitives_.add("reverse", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        auto v = a[0]; EvalValue result = make_void();
        while (!is_end_of_list(v)) {
            if (!is_pair(v)) return a[0];
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return a[0];
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, result});
            result = make_pair(new_id);
            v = pairs_[idx].cdr;
        }
        return result;
    });
    primitives_.add("map", [this](const auto& a) {
        // (map func list) — apply func to each element, collect results
        if (a.size() < 2 || is_void(a[1])) return make_void();

        // Helper to apply a function (closure or primitive) to a single argument
        auto apply_fn = [&](const EvalValue& fn, const EvalValue& arg) -> EvalValue {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count()) return make_void();
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim) return make_void();
                return (*prim)({arg});
            }
            if (is_closure(fn)) {
                auto cid = as_closure_id(fn);
                auto result = apply_closure(cid, {arg});
                return result ? *result : make_void();
            }
            return make_void();
        };

        // Walk the list, apply func to each element, build result in order
        EvalValue result = make_void();
        EvalValue tail = make_void();
        bool first = true;
        EvalValue current = a[1];

        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size()) break;

            auto mapped = apply_fn(a[0], pairs_[idx].car);

            auto new_id = pairs_.size();
            pairs_.push_back({mapped, make_void()});
            auto new_pair = make_pair(new_id);

            if (first) {
                result = new_pair;
                tail = new_pair;
                first = false;
            } else {
                auto tail_idx = as_pair_idx(tail);
                if (tail_idx < pairs_.size())
                    pairs_[tail_idx].cdr = new_pair;
                tail = new_pair;
            }

            current = pairs_[idx].cdr;
        }

        return result;
    });
    primitives_.add("filter", [this](const auto& a) {
        // (filter pred list) — keep elements where pred returns truthy
        if (a.size() < 2 || is_void(a[1])) return make_void();

        // Helper to apply a predicate (closure or primitive) to a single argument
        auto apply_pred = [&](const EvalValue& fn, const EvalValue& arg) -> bool {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count()) return false;
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim) return false;
                return types::is_truthy((*prim)({arg}));
            }
            if (is_closure(fn)) {
                auto cid = as_closure_id(fn);
                auto result = apply_closure(cid, {arg});
                return result ? types::is_truthy(*result) : false;
            }
            return false;
        };

        EvalValue result = make_void();
        EvalValue tail = make_void();
        bool first = true;
        EvalValue current = a[1];

        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size()) break;

            bool keep = apply_pred(a[0], pairs_[idx].car);
            if (keep) {
                auto new_id = pairs_.size();
                pairs_.push_back({pairs_[idx].car, make_void()});
                auto new_pair = make_pair(new_id);

                if (first) {
                    result = new_pair;
                    tail = new_pair;
                    first = false;
                } else {
                    auto tail_idx = as_pair_idx(tail);
                    if (tail_idx < pairs_.size())
                        pairs_[tail_idx].cdr = new_pair;
                    tail = new_pair;
                }
            }

            current = pairs_[idx].cdr;
        }

        return result;
    });

    // ── Vector primitives ─────────────────────────────────────────
    primitives_.add("vector", [this](const auto& a) {
        std::vector<EvalValue> elems(a.begin(), a.end());
        auto idx = vector_heap_.size();
        vector_heap_.push_back(std::move(elems));
        return make_vector(idx);
    });
    primitives_.add("vector-ref", [this](const auto& a) {
        if (a.size() < 2 || !is_vector(a[0])) return make_void();
        auto idx = as_vector_idx(a[0]);
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (idx >= vector_heap_.size() || pos >= vector_heap_[idx].size()) return make_void();
        return vector_heap_[idx][pos];
    });
    primitives_.add("vector-set!", [this](const auto& a) {
        if (a.size() < 3 || !is_vector(a[0])) return make_void();
        auto idx = as_vector_idx(a[0]);
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (idx >= vector_heap_.size() || pos >= vector_heap_[idx].size()) return make_void();
        vector_heap_[idx][pos] = a[2];
        return make_void();
    });
    primitives_.add("vector-length", [this](const auto& a) {
        if (a.empty() || !is_vector(a[0])) return make_int(0);
        auto idx = as_vector_idx(a[0]);
        if (idx >= vector_heap_.size()) return make_int(0);
        return make_int(static_cast<std::int64_t>(vector_heap_[idx].size()));
    });
    primitives_.add("vector?", [this](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_vector(a[0]));
    });
    primitives_.add("make-vector", [this](const auto& a) {
        auto n = a.empty() ? 0 : static_cast<std::size_t>(as_int(a[0]));
        EvalValue init = a.size() > 1 ? a[1] : make_void();
        std::vector<EvalValue> elems(n, init);
        auto idx = vector_heap_.size();
        vector_heap_.push_back(std::move(elems));
        return make_vector(idx);
    });
    primitives_.add("list->vector", [this](const auto& a) {
        std::vector<EvalValue> elems;
        if (!a.empty()) {
            auto v = a[0];
            while (is_pair(v)) {
                auto idx = as_pair_idx(v);
                if (idx >= pairs_.size()) break;
                elems.push_back(pairs_[idx].car);
                v = pairs_[idx].cdr;
            }
        }
        auto idx = vector_heap_.size();
        vector_heap_.push_back(std::move(elems));
        return make_vector(idx);
    });
    primitives_.add("vector->list", [this](const auto& a) {
        if (a.empty() || !is_vector(a[0])) return make_void();
        auto idx = as_vector_idx(a[0]);
        if (idx >= vector_heap_.size()) return make_void();
        EvalValue result = make_void();
        for (auto it = vector_heap_[idx].rbegin(); it != vector_heap_[idx].rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back({*it, result});
            result = make_pair(pid);
        }
        return result;
    });

    // ── Hash table (Swiss table) primitives ──────────────────────
    primitives_.add("hash", [this](const auto& a) {
        auto sh = &string_heap_;
        HashTable ht;
        ht.capacity = 8;
        ht.metadata.resize(ht.capacity, 0xFF);
        ht.keys.resize(ht.capacity);
        ht.values.resize(ht.capacity);
        auto khash = [sh](const EvalValue& k) -> std::uint64_t {
            if (is_int(k)) return static_cast<std::uint64_t>(as_int(k)) * 0x9e3779b97f4a7c15ull;
            if (is_string(k)) { auto i = as_string_idx(k);
                if (i < sh->size()) { auto& s = (*sh)[i]; std::uint64_t h = 0xcbf29ce484222325ull;
                    for (char c : s) h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull; return h; } }
            return 0x9e3779b97f4a7c15ull;
        };
        for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
            auto h = khash(a[i]); auto fp = static_cast<std::uint8_t>(h >> 57) | 0x80;
            for (std::size_t at = 0; at < ht.capacity; ++at) {
                auto idx = ((h >> 1) + at) & (ht.capacity - 1);
                if (ht.metadata[idx] == 0xFF) { ht.metadata[idx] = fp; ht.keys[idx] = a[i]; ht.values[idx] = a[i + 1]; ht.size++; break; }
            }
        }
        auto hidx = hash_heap_.size(); hash_heap_.push_back(std::move(ht)); return make_hash(hidx);
    });
    primitives_.add("hash-ref", [this](const auto& a) {
        if (a.size() < 2 || !is_hash(a[0])) return make_void();
        auto hidx = as_hash_idx(a[0]); if (hidx >= hash_heap_.size()) return make_void();
        auto& ht = hash_heap_[hidx]; auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF) continue;
            auto& k = ht.keys[i]; bool eq = false;
            if (is_int(k) && is_int(a[1])) eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) { auto ai = as_string_idx(k), bi = as_string_idx(a[1]); eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi]; }
            else eq = k == a[1];
            if (eq) return ht.values[i];
        }
        return make_void();
    });

    // (hash-has-key? hash key) — Check if key exists in hash
    primitives_.add("hash-has-key?", [this](const auto& a) {
        if (a.size() < 2 || !is_hash(a[0])) return make_bool(false);
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size()) return make_bool(false);
        auto& ht = hash_heap_[hidx];
        auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF) continue;
            auto& k = ht.keys[i];
            bool eq = false;
            if (is_int(k) && is_int(a[1])) eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            }
            else eq = k == a[1];
            if (eq) return make_bool(true);
        }
        return make_bool(false);
    });

    primitives_.add("hash-set!", [this](const auto& a) {
        if (a.size() < 3 || !is_hash(a[0])) return make_void();
        auto hidx = as_hash_idx(a[0]); if (hidx >= hash_heap_.size()) return make_void();
        auto& ht = hash_heap_[hidx]; auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF) continue;
            auto& k = ht.keys[i]; bool eq = false;
            if (is_int(k) && is_int(a[1])) eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) { auto ai = as_string_idx(k), bi = as_string_idx(a[1]); eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi]; }
            else eq = k == a[1];
            if (eq) { ht.values[i] = a[2]; return make_void(); }
        }
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF) {
                std::uint64_t h = 0x9e3779b97f4a7c15ull;
                if (is_int(a[1])) h = static_cast<std::uint64_t>(as_int(a[1])) * h;
                else if (is_string(a[1])) { auto idx = as_string_idx(a[1]); if (idx < sh->size()) { h = 0xcbf29ce484222325ull; for (char c : (*sh)[idx]) h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull; } }
                ht.metadata[i] = static_cast<std::uint8_t>(h >> 57) | 0x80;
                ht.keys[i] = a[1]; ht.values[i] = a[2]; ht.size++; return make_void();
            }
        }
        return make_void();
    });
    primitives_.add("hash-length", [this](const auto& a) {
        if (a.empty() || !is_hash(a[0])) return make_int(0);
        auto hidx = as_hash_idx(a[0]); if (hidx >= hash_heap_.size()) return make_int(0);
        return make_int(static_cast<std::int64_t>(hash_heap_[hidx].size));
    });
    primitives_.add("hash-keys", [this](const auto& a) {
        if (a.empty() || !is_hash(a[0])) return make_void();
        auto hidx = as_hash_idx(a[0]); if (hidx >= hash_heap_.size()) return make_void();
        auto& ht = hash_heap_[hidx]; EvalValue result = make_void();
        for (std::size_t i = ht.capacity; i > 0; --i) {
            if (ht.metadata[i - 1] != 0xFF) {
                auto pid = pairs_.size(); pairs_.push_back({ht.keys[i - 1], result}); result = make_pair(pid);
            }
        }
        return result;
    });
    primitives_.add("hash-values", [this](const auto& a) {
        if (a.empty() || !is_hash(a[0])) return make_void();
        auto hidx = as_hash_idx(a[0]); if (hidx >= hash_heap_.size()) return make_void();
        auto& ht = hash_heap_[hidx]; EvalValue result = make_void();
        for (std::size_t i = ht.capacity; i > 0; --i) {
            if (ht.metadata[i - 1] != 0xFF) {
                auto pid = pairs_.size(); pairs_.push_back({ht.values[i - 1], result}); result = make_pair(pid);
            }
        }
        return result;
    });
    primitives_.add("hash?", [this](const auto& a) {
        if (a.empty()) return make_bool(false); return make_bool(is_hash(a[0]));
    });
    primitives_.add("hash-remove!", [this](const auto& a) {
        if (a.size() < 2 || !is_hash(a[0])) return make_void();
        auto hidx = as_hash_idx(a[0]); if (hidx >= hash_heap_.size()) return make_void();
        auto& ht = hash_heap_[hidx]; auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF) continue;
            auto& k = ht.keys[i]; bool eq = false;
            if (is_int(k) && is_int(a[1])) eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) { auto ai = as_string_idx(k), bi = as_string_idx(a[1]); eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi]; }
            else eq = k == a[1];
            if (eq) { ht.metadata[i] = 0xFF; ht.size--; return make_bool(true); }
        }
        return make_bool(false);
    });
    auto infer_type_name = [](const EvalValue& v) -> const char* {
        if (is_float(v))  return "Float";
        if (is_hash(v))   return "Hash";
        if (is_vector(v)) return "Vector";
        if (is_string(v)) return "String";
        if (is_pair(v))   return "Pair";
        if (is_cell(v))   return "Cell";
        if (is_closure(v)) return "Closure";
        if (is_bool(v))   return "Bool";
        // Backward compat: int 0/1 was historically treated as Bool
        if (is_int(v) && (as_int(v) == 0 || as_int(v) == 1)) return "Bool";
        if (is_int(v))    return "Int";
        if (is_void(v))   return "Void";
        return "Unknown";
    };

    primitives_.add("type-of", [this, infer_type_name](const auto& a) -> EvalValue {
        if (a.empty()) return make_int(0);
        auto type_name = infer_type_name(a[0]);
        auto id = string_heap_.size();
        string_heap_.push_back(type_name);
        return make_string(id);
    });

    primitives_.add("type?", [this, infer_type_name](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[1])) return make_int(0);
        auto val_type = infer_type_name(a[0]);
        auto expected_idx = as_string_idx(a[1]);
        if (expected_idx >= string_heap_.size()) return make_int(0);
        auto& expected = string_heap_[expected_idx];
        return make_int(val_type == expected ? 1 : 0);
    });

        primitives_.add("equal?", [this](const auto& a) {
        if (a.size() < 2) return make_bool(true);

        struct EqCheck {
            Evaluator& e;
            bool operator()(const EvalValue& x, const EvalValue& y, int depth) const {
                if (depth > 64) return true;
                if (x == y) return true;
                if (is_int(x) && is_int(y)) return as_int(x) == as_int(y);
                if (is_float(x) && is_float(y)) return as_float(x) == as_float(y);
                if (is_bool(x) && is_bool(y)) return as_bool(x) == as_bool(y);
                if (is_string(x) && is_string(y)) {
                    auto xi = as_string_idx(x), yi = as_string_idx(y);
                    if (xi < e.string_heap_.size() && yi < e.string_heap_.size())
                        return e.string_heap_[xi] == e.string_heap_[yi];
                    return false;
                }
                if (is_pair(x) && is_pair(y)) {
                    auto xi = as_pair_idx(x), yi = as_pair_idx(y);
                    if (xi < e.pairs_.size() && yi < e.pairs_.size())
                        return (*this)(e.pairs_[xi].car, e.pairs_[yi].car, depth + 1)
                            && (*this)(e.pairs_[xi].cdr, e.pairs_[yi].cdr, depth + 1);
                    return false;
                }
                if (is_vector(x) && is_vector(y)) {
                    auto xi = as_vector_idx(x), yi = as_vector_idx(y);
                    if (xi < e.vector_heap_.size() && yi < e.vector_heap_.size()) {
                        auto& vx = e.vector_heap_[xi];
                        auto& vy = e.vector_heap_[yi];
                        if (vx.size() != vy.size()) return false;
                        for (std::size_t i = 0; i < vx.size(); ++i)
                            if (!(*this)(vx[i], vy[i], depth + 1)) return false;
                        return true;
                    }
                    return false;
                }
                // Empty list sentinel: void or int 0
                if ((is_void(x) || (is_int(x) && as_int(x) == 0)) &&
                    (is_void(y) || (is_int(y) && as_int(y) == 0))) return true;
                return false;
            }
        };

        return make_bool(EqCheck{*this}(a[0], a[1], 0));
    });

    primitives_.add("gensym", [this](const auto&) -> EvalValue {
        static std::atomic<std::int64_t> gs_counter_{0};
        auto id = gs_counter_.fetch_add(1, std::memory_order_relaxed);
        std::string name = "G__" + std::to_string(id);
        auto sid = string_heap_.size();
        string_heap_.push_back(name);
        return make_string(sid);
    });
    primitives_.add("symbol-append", [this](const auto& a) -> EvalValue {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < string_heap_.size()) result += string_heap_[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto sid = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sid);
    });

    // (apply fn list) — call fn with list elements as individual args
    primitives_.add("apply", [this](const auto& a) -> EvalValue {
        if (a.size() < 2) return make_void();
        auto& fn = a[0];
        auto& arg_list = a[1];
        std::vector<EvalValue> args;
        auto current = arg_list;
        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size()) break;
            args.push_back(pairs_[idx].car);
            current = pairs_[idx].cdr;
        }
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            auto pfn = primitives_.lookup(primitives_.name_for_slot(slot));
            if (pfn) return (*pfn)(args);
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = apply_closure(cid, args);
            if (result) return *result;
        }
        return make_void();
    });

    primitives_.add("display", [this](const auto& a) {
        if (a.empty()) return make_void();
        io_print_val(a[0], &string_heap_, &pairs_, false);
        std::fflush(stdout);
        return make_void();
    });
    primitives_.add("write", [this](const auto& a) -> EvalValue {
        if (a.empty()) return make_void();
        io_print_val(a[0], &string_heap_, &pairs_, true);
        std::fflush(stdout);
        return make_void();
    });
    primitives_.add("newline", [](const auto&) -> EvalValue {
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
        return make_void();
    });
    // (format template args...) — Simple string formatting (SRFI-28 subset)
    // ~a  display arg    ~s  write arg    ~%  newline    ~~  literal ~
    primitives_.add("format", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0])) return make_bool(false);
        auto tidx = as_string_idx(a[0]);
        if (tidx >= string_heap_.size()) return make_bool(false);
        auto& tmpl = string_heap_[tidx];
        std::string result;
        std::size_t arg_idx = 1;  // first arg in a[1..]
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '~' && i + 1 < tmpl.size()) {
                switch (tmpl[i + 1]) {
                case 'a':  // display arg
                    if (arg_idx < a.size()) {
                        auto val = a[arg_idx++];
                        result += fmt_val_to_string(val, string_heap_, pairs_, false);
                    }
                    ++i;
                    break;
                case 's':  // write arg (quoted)
                    if (arg_idx < a.size()) {
                        auto val = a[arg_idx++];
                        result += fmt_val_to_string(val, string_heap_, pairs_, true);
                    }
                    ++i;
                    break;
                case '%':  // newline
                    result += '\n';
                    ++i;
                    break;
                case '~':  // literal ~
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
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });
    // (error msg) — Create an error value (no longer throws C++ exception)
    primitives_.add("error", [this](const auto& a) -> EvalValue {
        // Ensure error_values_[0] always exists for default errors
        if (error_values_.empty()) error_values_.push_back(make_void());
        types::EvalValue cause = make_string(0); // default
        if (!a.empty()) cause = a[0];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (assert expr msg) — Assertion, returns error on failure
    primitives_.add("assert", [this](const auto& a) -> EvalValue {
        if (!a.empty() && is_truthy(a[0])) return make_int(1);
        // Assertion failed — return error
        types::EvalValue cause = make_string(0);
        if (a.size() > 1) cause = a[1];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (raise val) — Create an error with arbitrary cause value
    primitives_.add("raise", [this](const auto& a) -> EvalValue {
        auto cause = a.empty() ? make_void() : a[0];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (error? val) — Type predicate for error values
    primitives_.add("error?", [this](const auto& a) -> EvalValue {
        if (a.empty()) return make_bool(false);
        return make_bool(is_error(a[0]));
    });


    // (check expr) — Test assertion, returns #t or error on failure
    primitives_.add("check", [this](const auto& a) -> EvalValue {
        if (a.empty()) return make_error(0);
        if (is_truthy(a[0])) return make_int(1);
        // Store failing value as error cause
        auto eidx = error_values_.size();
        error_values_.push_back(a[0]);
        return make_error(eidx);
    });

    // (check= expected actual) — Test equality, returns #t or error
    primitives_.add("check=", [this](const auto& a) -> EvalValue {
        if (a.size() < 2) return make_bool(false);
        if (types::is_void(a[0]) && types::is_void(a[1])) return make_int(1);
        if (types::is_int(a[0]) && types::is_int(a[1])) {
            if (types::as_int(a[0]) == types::as_int(a[1])) return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_float(a[0]) && types::is_float(a[1])) {
            if (types::as_float(a[0]) == types::as_float(a[1])) return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_string(a[0]) && types::is_string(a[1])) {
            auto i0 = types::as_string_idx(a[0]);
            auto i1 = types::as_string_idx(a[1]);
            if (i0 < string_heap_.size() && i1 < string_heap_.size()
                && string_heap_[i0] == string_heap_[i1]) return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        return make_bool(false);
    });

    // (run-tests) — Find all test:* bindings in top_ env, report summary
    primitives_.add("run-tests", [this](const auto&) -> EvalValue {
        auto& bindings = top_.bindings();
        int total_suites = 0, total_passed = 0, total_failed = 0;

        for (auto& [name, val] : bindings) {
            if (name.size() <= 5 || name.substr(0, 5) != "test:")
                continue;

            // Dereference cell if needed (define stores via cell)
            auto actual = val;
            if (is_cell(val) && as_cell_id(val) < cells_.size())
                actual = cells_[as_cell_id(val)];
            if (!is_pair(actual))
                continue;

            total_suites++;
            auto idx = as_pair_idx(actual);
            if (idx >= pairs_.size()) { total_failed++; continue; }

            // Suite name
            std::string suite_name;
            if (is_string(pairs_[idx].car)) {
                auto sid = as_string_idx(pairs_[idx].car);
                if (sid < string_heap_.size())
                    suite_name = string_heap_[sid];
            }

            // Walk and evaluate each stored check form
            auto forms = pairs_[idx].cdr;
            int sp = 0, sf = 0;

            while (is_pair(forms)) {
                auto fi = as_pair_idx(forms);
                if (fi >= pairs_.size()) break;
                auto check_form = pairs_[fi].car;
                forms = pairs_[fi].cdr;

                // Convert check_form data back to AST and evaluate
                if (!arena_) { sf++; continue; }
                auto alloc = arena_->allocator();
                auto* cf_pool = arena_->create<aura::ast::StringPool>(alloc);
                auto* cf_flat = arena_->create<aura::ast::FlatAST>(alloc);
                auto ast_root = data_to_flat(check_form, *cf_flat, *cf_pool, 0);
                if (ast_root == aura::ast::NULL_NODE) { sf++; continue; }
                cf_flat->root = ast_root;

                auto result = eval_flat(*cf_flat, *cf_pool, ast_root, top_);
                if (!result) { sf++; continue; }

                bool ok = is_int(*result) && as_int(*result) == 1;
                if (ok) { sp++; total_passed++; }
                else { sf++; total_failed++; }
            }

            std::fprintf(stderr, "  Suite '%s': %d/%d passed\n",
                         suite_name.c_str(), sp, sp + sf);
        }

        std::string summary = std::to_string(total_suites) + " suites: "
            + std::to_string(total_passed) + " passed, "
            + std::to_string(total_failed) + " failed";
        auto sidx = string_heap_.size();
        string_heap_.push_back(summary);
        return make_string(sidx);
    });

    primitives_.add("read", [this](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty()) return make_void();
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // ── File I/O (P0) ───────────────────────────────────────────
    primitives_.add("read-file", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto& path = string_heap_[idx];
        std::ifstream f(path);
        if (!f) return make_void();
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(content));
        return make_string(id);
    });

    primitives_.add("write-file", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto& path = string_heap_[idx];
        std::string content;
        if (is_string(a[1])) {
            auto cidx = as_string_idx(a[1]);
            if (cidx < string_heap_.size())
                content = string_heap_[cidx];
        } else if (is_int(a[1])) {
            content = std::to_string(as_int(a[1]));
        } else {
            return make_void();
        }
        std::ofstream f(path);
        if (!f) return make_void();
        f << content;
        return make_int(1);
    });

    primitives_.add("file-exists?", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_int(0);
        auto& path = string_heap_[idx];
        std::ifstream f(path);
        return make_int(f.good() ? 1 : 0);
    });

    // ── File I/O: copy, delete, size, directory list ─────────────
    primitives_.add("file-copy", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1])) return make_void();
        auto sidx = as_string_idx(a[0]), didx = as_string_idx(a[1]);
        if (sidx >= string_heap_.size() || didx >= string_heap_.size()) return make_void();
        std::ifstream src(string_heap_[sidx], std::ios::binary);
        if (!src) return make_void();
        std::ofstream dst(string_heap_[didx], std::ios::binary);
        if (!dst) return make_void();
        dst << src.rdbuf();
        return make_int(1);
    });

    primitives_.add("file-delete", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_int(0);
        return make_int(std::remove(string_heap_[idx].c_str()) == 0 ? 1 : 0);
    });

    primitives_.add("file-size", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_int(0);
        std::ifstream f(string_heap_[idx], std::ios::ate | std::ios::binary);
        if (!f) return make_int(0);
        return make_int(static_cast<std::int64_t>(f.tellg()));
    });

    primitives_.add("directory-list", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto& dir_path = string_heap_[idx];
        EvalValue result = make_void();
        auto dir = opendir(dir_path.c_str());
        if (!dir) return make_void();
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            auto sid = string_heap_.size();
            string_heap_.push_back(name);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sid), result});
            result = make_pair(pid);
        }
        closedir(dir);
        return result;
    });

    // ── Regex ──────────────────────────────────────────────────
    primitives_.add("regex-match?", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1])) return make_int(0);
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap_.size() || si >= string_heap_.size()) return make_int(0);
        try {
            std::regex re(string_heap_[pi]);
            return make_int(std::regex_search(string_heap_[si], re) ? 1 : 0);
        } catch (...) { return make_int(0); }
    });

    primitives_.add("regex-find", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1])) return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap_.size() || si >= string_heap_.size()) return make_void();
        try {
            std::regex re(string_heap_[pi]);
            std::smatch m;
            if (std::regex_search(string_heap_[si], m, re)) {
                auto id = string_heap_.size();
                string_heap_.push_back(m.str());
                return make_string(id);
            }
        } catch (...) {}
        return make_void();
    });

    primitives_.add("regex-replace", [this](const auto& a) {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]) || !is_string(a[2])) return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]), ri = as_string_idx(a[2]);
        if (pi >= string_heap_.size() || si >= string_heap_.size() || ri >= string_heap_.size()) return make_void();
        try {
            std::regex re(string_heap_[pi]);
            auto result = std::regex_replace(string_heap_[si], re, string_heap_[ri]);
            auto id = string_heap_.size();
            string_heap_.push_back(std::move(result));
            return make_string(id);
        } catch (...) { return make_void(); }
    });

    primitives_.add("regex-split", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1])) return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap_.size() || si >= string_heap_.size()) return make_void();
        try {
            std::regex re(string_heap_[pi]);
            std::sregex_token_iterator it(string_heap_[si].begin(), string_heap_[si].end(), re, -1);
            std::sregex_token_iterator end;
            EvalValue result = make_void();
            std::vector<std::string> parts;
            for (; it != end; ++it)
                parts.push_back(it->str());
            for (auto it2 = parts.rbegin(); it2 != parts.rend(); ++it2) {
                auto sid = string_heap_.size();
                string_heap_.push_back(*it2);
                auto pid = pairs_.size();
                pairs_.push_back({make_string(sid), result});
                result = make_pair(pid);
            }
            return result;
        } catch (...) { return make_void(); }
    });

    // ── Math ────────────────────────────────────────────────────
    auto to_double = [this](const EvalValue& v) -> double {
        if (is_float(v)) return as_float(v);
        if (is_int(v)) return static_cast<double>(as_int(v));
        return 0.0;
    };

    primitives_.add("sin", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::sin(to_double(a[0])));
    });
    primitives_.add("cos", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::cos(to_double(a[0])));
    });
    primitives_.add("tan", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::tan(to_double(a[0])));
    });
    primitives_.add("asin", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::asin(to_double(a[0])));
    });
    primitives_.add("acos", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::acos(to_double(a[0])));
    });
    primitives_.add("atan", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::atan(to_double(a[0])));
    });
    primitives_.add("log", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::log(to_double(a[0])));
    });
    primitives_.add("log10", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::log10(to_double(a[0])));
    });
    primitives_.add("exp", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::exp(to_double(a[0])));
    });
    primitives_.add("pow", [to_double](const auto& a) {
        if (a.size() < 2) return make_float(0.0);
        return make_float(std::pow(to_double(a[0]), to_double(a[1])));
    });
    primitives_.add("sqrt", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::sqrt(to_double(a[0])));
    });
    primitives_.add("floor", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::floor(to_double(a[0])));
    });
    primitives_.add("ceil", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::ceil(to_double(a[0])));
    });
    primitives_.add("round", [to_double](const auto& a) {
        if (a.empty()) return make_float(0.0);
        return make_float(std::round(to_double(a[0])));
    });

    primitives_.add("inexact->exact", [](const auto& a) -> EvalValue {
        if (a.empty()) return make_int(0);
        if (types::is_float(a[0])) return types::make_int(static_cast<std::int64_t>(types::as_float(a[0])));
        return a[0];
    });

    // ═══════════════════════════════════════════════════════════════
    // Module primitives (Phase 1: module objects)
    // ═══════════════════════════════════════════════════════════════

    // (module? v) — Check if value is a module object
    primitives_.add("module?", [](const auto& a) {
        return make_bool(!a.empty() && is_module(a[0]));
    });

    // (module-get mod name) — Get a binding from a module by symbol name
    primitives_.add("module-get", [this](const auto& a) {
        if (a.size() < 2 || !is_module(a[0]) || !is_string(a[1]))
            return make_void();
        auto mod_idx = as_module_idx(a[0]);
        auto name_idx = as_string_idx(a[1]);
        if (mod_idx >= modules_.size() || name_idx >= string_heap_.size())
            return make_void();
        auto result = modules_[mod_idx]->lookup(string_heap_[name_idx]);
        return result ? *result : make_void();
    });

    // (module-keys mod) — List all exported binding names from a module
    primitives_.add("module-keys", [this](const auto& a) {
        if (a.empty() || !is_module(a[0])) return make_void();
        auto mod_idx = as_module_idx(a[0]);
        if (mod_idx >= modules_.size()) return make_void();
        EvalValue result = make_void();
        auto& bindings = modules_[mod_idx]->bindings();
        for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(it->first);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (use path) — Load module, return module object (no env injection)
    primitives_.add("use", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        return load_module_file(string_heap_[idx]);
    });

    primitives_.add("load-module", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        return load_module_file(string_heap_[idx]);
    });

    primitives_.add("import", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto& path = string_heap_[idx];

        // Optional prefix: (import "path" "prefix:")
        std::string prefix;
        if (a.size() > 1 && is_string(a[1])) {
            auto pidx = as_string_idx(a[1]);
            if (pidx < string_heap_.size())
                prefix = string_heap_[pidx];
        }

        // Load module (cached, isolated env)
        auto mod_val = load_module_file(path);
        if (!is_module(mod_val)) return make_void();
        auto mod_idx = as_module_idx(mod_val);
        if (mod_idx >= modules_.size()) return make_void();

        // Inject all bindings into top_ env
        auto* mod_env = modules_[mod_idx];
        if (prefix.empty()) {
            // No prefix: inject as-is (backward compat)
            for (auto& [name, val] : mod_env->bindings()) {
                top_.bind(name, val);
            }
        } else {
            // Prefix injection: bind both prefix:name and bare name for each export
            for (auto& [name, val] : mod_env->bindings()) {
                auto prefixed = prefix + name;
                // Inter the prefixed name into the workspace pool
                auto psid = string_heap_.size();
                string_heap_.push_back(prefixed);
                // Bind in top env with prefix
                top_.bind(prefixed, val);
                // Also bind bare name (no prefix) so tree-walker can find it
                top_.bind(name, val);
            }
        }
        return make_bool(true);
    });
    primitives_.add("modulo", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) { do {
    auto __e_sidx = string_heap_.size();
    string_heap_.push_back("modulo: division by zero");
    auto __e_eidx = error_values_.size();
    error_values_.push_back(make_string(__e_sidx));
    return make_error(__e_eidx);
} while(0); }
        auto n = coerce_to_int(a[0], &string_heap_);
        auto r = n % divisor;
        if (r < 0) r += (divisor > 0 ? divisor : -divisor);
        return make_int(r);
    });
    primitives_.add("mod", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) { do {
    auto __e_sidx = string_heap_.size();
    string_heap_.push_back("mod: division by zero");
    auto __e_eidx = error_values_.size();
    error_values_.push_back(make_string(__e_sidx));
    return make_error(__e_eidx);
} while(0); }
        auto n = coerce_to_int(a[0], &string_heap_);
        auto r = n % divisor;
        if (r < 0) r += (divisor > 0 ? divisor : -divisor);
        return make_int(r);
    });
    // quotient: (quotient n m) → integer division truncating toward zero
    primitives_.add("quotient", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) { do {
    auto __e_sidx = string_heap_.size();
    string_heap_.push_back("quotient: division by zero");
    auto __e_eidx = error_values_.size();
    error_values_.push_back(make_string(__e_sidx));
    return make_error(__e_eidx);
} while(0); }
        return make_int(coerce_to_int(a[0], &string_heap_) / divisor);
    });
    // remainder: (remainder n m) → remainder with sign of dividend
    primitives_.add("remainder", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) { do {
    auto __e_sidx = string_heap_.size();
    string_heap_.push_back("remainder: division by zero");
    auto __e_eidx = error_values_.size();
    error_values_.push_back(make_string(__e_sidx));
    return make_error(__e_eidx);
} while(0); }
        return make_int(coerce_to_int(a[0], &string_heap_) % divisor);
    });
    // abs: (abs n) → absolute value
    primitives_.add("abs", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        if (is_float(a[0])) return make_float(std::abs(as_float(a[0])));
        auto n = coerce_to_int(a[0], &string_heap_);
        return make_int(n < 0 ? -n : n);
    });
    // gcd: (gcd a b ...) → greatest common divisor (variadic)
    primitives_.add("gcd", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        auto to_int = [this](const EvalValue& v) { return coerce_to_int(v, &string_heap_); };
        auto r = to_int(a[0]);
        auto abs_gcd = [](std::int64_t x, std::int64_t y) -> std::int64_t {
            x = x < 0 ? -x : x;
            y = y < 0 ? -y : y;
            while (y != 0) { auto t = y; y = x % y; x = t; }
            return x;
        };
        for (std::size_t i = 1; i < a.size(); ++i)
            r = abs_gcd(r, to_int(a[i]));
        return make_int(r);
    });
    // lcm: (lcm a b ...) → least common multiple (variadic)
    primitives_.add("lcm", [this](const auto& a) {
        if (a.empty()) return make_int(1);
        auto to_int = [this](const EvalValue& v) { return coerce_to_int(v, &string_heap_); };
        auto r = to_int(a[0]);
        auto gcd = [](std::int64_t x, std::int64_t y) -> std::int64_t {
            x = x < 0 ? -x : x;
            y = y < 0 ? -y : y;
            if (x == 0 || y == 0) return 0;
            while (y != 0) { auto t = y; y = x % y; x = t; }
            return x;
        };
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto n = to_int(a[i]);
            auto g = gcd(r, n);
            r = (g == 0) ? 0 : (r / g) * n;
        }
        if (r < 0) r = -r;
        return make_int(r);
    });
    // min: (min a b ...) → minimum (variadic)
    primitives_.add("min", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        if (any_f) {
            auto to_f = [this](const EvalValue& v) { return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, &string_heap_)); };
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i) r = std::min(r, to_f(a[i]));
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], &string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i) r = std::min(r, coerce_to_int(a[i], &string_heap_));
        return make_int(r);
    });
    // max: (max a b ...) → maximum (variadic)
    primitives_.add("max", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        bool any_f = false;
        for (auto& v : a) if (is_float(v)) { any_f = true; break; }
        if (any_f) {
            auto to_f = [this](const EvalValue& v) { return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, &string_heap_)); };
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i) r = std::max(r, to_f(a[i]));
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], &string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i) r = std::max(r, coerce_to_int(a[i], &string_heap_));
        return make_int(r);
    });

    // ── Character + I/O extensions ────────────────────────────────

    // char?: (char? v) → true if is_int(v) (chars represented as ints)
    primitives_.add("char?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        return make_bool(is_int(a[0]));
    });
    // char->integer: (char->integer c) → integer value
    primitives_.add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_int(0);
        return a[0];
    });
    // integer->char: (integer->char n) → identity
    primitives_.add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0])) return make_int(0);
        return a[0];
    });
    // string->list: (string->list s) → list of char codes
    primitives_.add("string->list", [this](const auto& a) {
        if (a.empty()) return make_void();
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size()) s = string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    // list->string: (list->string lst) → string from char codes
    primitives_.add("list->string", [this](const auto& a) {
        if (a.empty() || !is_pair(a[0]) && !is_void(a[0])) return make_int(0);
        std::string result;
        auto v = a[0];
        while (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) break;
            auto car = pairs_[idx].car;
            if (is_int(car))
                result.push_back(static_cast<char>(as_int(car)));
            v = pairs_[idx].cdr;
        }
        auto sid = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return make_string(sid);
    });
    // read-line: (read-line) → read a line from stdin as string
    primitives_.add("read-line", [this](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty()) return make_void();
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return make_string(id);
    });
    // eof-object?: (eof-object? v) → check if value represents EOF
    primitives_.add("eof-object?", [](const auto& a) {
        if (a.empty()) return make_bool(false);
        // EOF is represented as void (the same as when read-line returns empty)
        return make_bool(is_void(a[0]));
    });

    // ── List utility primitives ────────────────────────────────────
    primitives_.add("take", [this](const auto& a) {
        if (a.size() < 2) return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        EvalValue result = make_void();
        // Build result in reverse then reverse it
        for (std::size_t i = 0; i < n; ++i) {
            if (!is_pair(v)) return result;
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return result;
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, result});
            result = make_pair(new_id);
            v = pairs_[idx].cdr;
        }
        // Reverse to get correct order
        EvalValue final = make_void();
        while (!is_end_of_list(result)) {
            if (!is_pair(result)) break;
            auto idx = as_pair_idx(result);
            if (idx >= pairs_.size()) break;
            auto nid = pairs_.size();
            pairs_.push_back({pairs_[idx].car, final});
            final = make_pair(nid);
            result = pairs_[idx].cdr;
        }
        return final;
    });
    primitives_.add("drop", [this](const auto& a) {
        if (a.size() < 2) return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        for (std::size_t i = 0; i < n; ++i) {
            if (is_end_of_list(v)) return v;
            if (!is_pair(v)) return v;
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return v;
            v = pairs_[idx].cdr;
        }
        return v;
    });
    primitives_.add("foldl", [this](const auto& a) {
        if (a.size() < 3) return make_void();
        auto f = a[0];
        auto acc = a[1];
        auto lst = a[2];

        // Handle primitive function (e.g., (foldl + 0 (list 1 2 3)))
        if (is_primitive(f)) {
            auto slot = as_primitive_slot(f);
            if (slot >= primitives_.slot_count()) return make_void();
            auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
            if (!prim) return make_void();

            while (!is_end_of_list(lst)) {
                if (!is_pair(lst)) break;
                auto idx = as_pair_idx(lst);
                if (idx >= pairs_.size()) break;

                // Call primitive with (acc, car)
                acc = (*prim)({acc, pairs_[idx].car});
                lst = pairs_[idx].cdr;
            }
            return acc;
        }

        // Handle closure function
        if (!is_closure(f)) return make_void();
        auto cid = as_closure_id(f);

        while (!is_end_of_list(lst)) {
            if (!is_pair(lst)) break;
            auto idx = as_pair_idx(lst);
            if (idx >= pairs_.size()) break;

            // Build args: (acc element) for binary foldl, (acc) for unary
            std::vector<EvalValue> fargs = {acc, pairs_[idx].car};
            auto result = apply_closure(cid, fargs);
            if (!result) break;
            acc = *result;
            lst = pairs_[idx].cdr;
        }
        return acc;
    });

    // ── Typed mutation operators ──────────────────────────────────

    // (mutate:replace-type node-id new-type-str)
    primitives_.add("mutate:replace-type", [this](const auto& a) {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1])) return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto type_idx = as_string_idx(a[1]);
        if (type_idx >= string_heap_.size()) return make_int(0);
        if (!workspace_flat_) return make_int(0);
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) return make_int(0);

        auto old_tid = flat.type_id(node);
        std::string old_type_str = (old_tid > 0)
            ? "#" + std::to_string(old_tid) : "Any";
        auto old_val = static_cast<std::uint64_t>(old_tid);
        auto new_val = static_cast<std::uint64_t>(string_heap_.size()); // placeholder

        // Simple type ID mapping based on well-known type names
        auto type_str = string_heap_[type_idx];
        std::uint32_t new_tid = 0;
        if (type_str == "Int") new_tid = 1;
        else if (type_str == "Float") new_tid = 2;
        else if (type_str == "String") new_tid = 3;
        else if (type_str == "Bool") new_tid = 4;
        else if (type_str == "Dyn" || type_str == "Any") new_tid = 0;
        else new_tid = 0; // unknown → Dyn

        auto mid = flat.add_mutation_with_rollback(node, "replace-type",
            old_type_str, string_heap_[type_idx], "replace type annotation",
            aura::ast::MutationStatus::Committed, 1, old_val, new_tid, true);
        // Actually apply the type change
        flat.set_type(node, new_tid);
        return make_int(static_cast<std::int64_t>(mid));
    });

    // (mutate:replace-value node-id new-value summary)
    // Replaces the value of a node. The type of new-value must match the
    // target node: int → LiteralInt, float → LiteralFloat, string → Variable/LiteralString.
    primitives_.add("mutate:replace-value", [this](const auto& a) {
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[2]))
            return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto sum_idx = as_string_idx(a[2]);
        if (sum_idx >= string_heap_.size()) return make_int(0);
        if (!workspace_flat_) return make_int(0);
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) return make_int(0);

        auto nv = flat.get(node);
        std::uint64_t old_val = 0;

        switch (nv.tag) {
        case aura::ast::NodeTag::LiteralInt: {
            if (!is_int(a[1])) return make_int(0);
            auto new_val = static_cast<std::int64_t>(as_int(a[1]));
            old_val = static_cast<std::uint64_t>(nv.int_value);
            auto mid = flat.add_mutation_with_rollback(node, "replace-value",
                "Int", "Int", string_heap_[sum_idx],
                aura::ast::MutationStatus::Committed, 0, old_val,
                static_cast<std::uint64_t>(new_val), true);
            flat.set_int(node, new_val);
            return make_int(static_cast<std::int64_t>(mid));
        }
        case aura::ast::NodeTag::LiteralFloat: {
            if (!is_float(a[1])) return make_int(0);
            // Pack double as uint64 for mutation log
            double new_val = as_float(a[1]);
            std::uint64_t new_bits;
            std::memcpy(&new_bits, &new_val, sizeof(new_bits));
            std::uint64_t old_bits;
            std::memcpy(&old_bits, &nv.float_value, sizeof(old_bits));
            auto mid = flat.add_mutation_with_rollback(node, "replace-value",
                "Float", "Float", string_heap_[sum_idx],
                aura::ast::MutationStatus::Committed, 1, old_bits, new_bits, true);
            flat.set_float(node, new_val);
            return make_int(static_cast<std::int64_t>(mid));
        }
        case aura::ast::NodeTag::Variable:
        case aura::ast::NodeTag::LiteralString: {
            if (!is_string(a[1])) return make_int(0);
            auto new_sym_idx = as_string_idx(a[1]);
            if (new_sym_idx >= string_heap_.size()) return make_int(0);
            auto new_name = string_heap_[new_sym_idx];
            old_val = nv.sym_id;
            auto new_sym = workspace_pool_->intern(new_name);
            auto mid = flat.add_mutation_with_rollback(node, "replace-value",
                "Sym", "Sym", string_heap_[sum_idx],
                aura::ast::MutationStatus::Committed, 2, old_val, new_sym, true);
            flat.set_sym(node, new_sym);
            return make_int(static_cast<std::int64_t>(mid));
        }
        default:
            return make_int(0);  // no replaceable value on this node type
        }
    });

    // (mutate:record-patch node-id op-name summary)
    primitives_.add("mutate:record-patch", [this](const auto& a) {
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto op_idx = as_string_idx(a[1]);
        auto sum_idx = as_string_idx(a[2]);
        if (op_idx >= string_heap_.size() || sum_idx >= string_heap_.size())
            return make_int(0);
        if (!workspace_flat_) return make_int(0);
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) return make_int(0);

        auto mid = flat.add_mutation(node, string_heap_[op_idx],
            "<runtime>", "<runtime>", string_heap_[sum_idx]);
        return make_int(static_cast<std::int64_t>(mid));
    });

    // (mutation-count)
    primitives_.add("mutation-count", [this](const auto&) {
        if (!workspace_flat_) return make_int(0);
        return make_int(static_cast<std::int64_t>(workspace_flat_->mutation_count()));
    });

    // (mutation-history node-id) → list of summary strings
    primitives_.add("mutation-history", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto hist = workspace_flat_->mutation_history(node);
        EvalValue result = make_void();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            auto& rec = *it;
            auto sid = string_heap_.size();
            string_heap_.push_back(std::format("[{}] {}: {}{}",
                rec.mutation_id, rec.operator_name, rec.summary,
                rec.status == aura::ast::MutationStatus::RolledBack ? " [rolled-back]" : ""));
            auto pair_id = pairs_.size();
            pairs_.push_back({make_string(sid), result});
            result = make_pair(pair_id);
        }
        return result;
    });

    // (rollback mutation-id) → true if successful
    primitives_.add("rollback", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_bool(false);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_bool(workspace_flat_->rollback(mid));
    });

    // (rollback-since mutation-id) → count of rolled-back mutations
    primitives_.add("rollback-since", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_int(0);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(workspace_flat_->rollback_since(mid)));
    });

    // (check-preconditions node-id (field-offset|new-type-str)) → true if valid
    // With int second arg: check field existence (0=int_val_, 1=type_id_)
    // With string second arg: check type compatibility (new type string)
    primitives_.add("check-preconditions", [this](const auto& a) {
        if (a.size() < 2 || !is_int(a[0]) || !workspace_flat_)
            return make_bool(false);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) return make_bool(false);
        auto nv = flat.get(node);

        // String arg: type compatibility check
        if (is_string(a[1])) {
            auto type_idx = as_string_idx(a[1]);
            if (type_idx >= string_heap_.size()) return make_bool(false);
            auto new_type = string_heap_[type_idx];

            // Check compatibility based on node tag
            switch (nv.tag) {
            case aura::ast::NodeTag::LiteralInt:
                // Int literal: compatible with Int, Float, Bool (≠0), Dyn
                return make_bool(new_type == "Int" || new_type == "Float"
                              || new_type == "Bool" || new_type == "Dyn"
                              || new_type == "Any");
            case aura::ast::NodeTag::LiteralFloat:
                // Float literal: compatible with Float, Dyn
                return make_bool(new_type == "Float" || new_type == "Dyn"
                              || new_type == "Any");
            case aura::ast::NodeTag::LiteralString:
                return make_bool(new_type == "String" || new_type == "Dyn"
                              || new_type == "Any");
            case aura::ast::NodeTag::Call:
            case aura::ast::NodeTag::Lambda:
                // Structural nodes: always OK (any type)
                return make_bool(true);
            case aura::ast::NodeTag::Variable:
                // Variable: always OK (outer scope determines type)
                return make_bool(true);
            default:
                // Other nodes: permissive
                return make_bool(true);
            }
        }

        // Int arg: field existence check
        if (!is_int(a[1])) return make_bool(false);
        auto field = static_cast<std::uint32_t>(as_int(a[1]));
        switch (field) {
        case 0: return make_bool(nv.has_int());   // int_val_
        case 1: return make_bool(true);            // type_id_ (always valid)
        default: return make_bool(false);
        }
    });

    // ═══════════════════════════════════════════════════════════════
    // P6: Query/Transform EDSL 原语
    // ═══════════════════════════════════════════════════════════════

    // (set-code code-string) — Parse code and set as current workspace AST
    // Nodes in workspace AST have stable IDs across query/mutate operations
    // Multi-expression code is automatically wrapped in (begin ...) by the parser.
    primitives_.add("set-code", [this](const auto& a) {
        coverage_counters_[0]++; coverage_counters_[5]++;
        if (a.empty() || !is_string(a[0])) return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_bool(false);
        if (!arena_) return make_bool(false);
        auto alloc = arena_->allocator();
        auto* pool_ptr = arena_->create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_->create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(string_heap_[idx], *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_bool(false);
        flat_ptr->root = pr.root;
        workspace_flat_ = flat_ptr;
        workspace_pool_ = pool_ptr;
        return make_bool(true);
    });

    // (current-source) — Return the current workspace AST as source code string
    // Implemented inline to avoid circular dependency with lowering module.
    // (eval code) — Parse and evaluate a string of Aura code
    primitives_.add("eval", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) return make_void();
        auto code = string_heap_[types::as_string_idx(a[0])];
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto pr = aura::parser::parse_to_flat(code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_void();
        flat.root = pr.root;
        auto result = eval_flat(flat, pool, pr.root, top_);
        if (result) return *result;
        return make_void();
    });

    primitives_.add("current-source", [this](const auto&) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_) return make_string(0);
        
        // Inline unparse for the workspace root
        auto unparse = [&](this const auto& self, aura::ast::NodeId id, int indent) -> std::string {
            if (id == aura::ast::NULL_NODE || id >= workspace_flat_->size()) return "()";
            auto v = workspace_flat_->get(id);
            switch (v.tag) {
            case aura::ast::NodeTag::LiteralInt: {
                if (workspace_flat_->marker(id) == aura::ast::SyntaxMarker::BoolLiteral)
                    return v.int_value ? "#t" : "#f";
                return std::to_string(v.int_value);
            }
            case aura::ast::NodeTag::LiteralFloat: {
                auto s = std::to_string(v.float_value);
                if (s.find('.') == std::string::npos) s += ".0";
                return s;
            }
            case aura::ast::NodeTag::LiteralString: {
                auto raw = workspace_pool_->resolve(v.sym_id);
                std::string esc = "\"";
                for (auto c : std::string_view(raw)) {
                    if (c == '\\' || c == '"') esc += '\\';
                    esc += c;
                }
                esc += '"';
                return esc;
            }
            case aura::ast::NodeTag::Variable:
                return std::string(workspace_pool_->resolve(v.sym_id));
            case aura::ast::NodeTag::Call: {
                std::string s = "(";
                for (std::size_t i = 0; i < v.children.size(); ++i) {
                    if (i > 0) s += " ";
                    s += self(v.child(i), indent + 1);
                }
                return s + ")";
            }
            case aura::ast::NodeTag::Lambda: {
                std::string s = "(lambda (";
                for (std::size_t i = 0; i < v.params.size(); ++i) {
                    if (i > 0) s += " ";
                    s += std::string(workspace_pool_->resolve(v.params[i]));
                }
                s += ")";
                if (!v.children.empty())
                    s += " " + self(v.child(0), indent + 1);
                return s + ")";
            }
            case aura::ast::NodeTag::Let:
            case aura::ast::NodeTag::LetRec: {
                auto kw = (v.tag == aura::ast::NodeTag::LetRec) ? std::string("letrec") : std::string("let");
                std::string s = "(" + kw + " ((" + std::string(workspace_pool_->resolve(v.sym_id)) + " ";
                if (!v.children.empty())
                    s += self(v.child(0), indent + 1);
                s += "))";
                if (v.children.size() > 1)
                    s += " " + self(v.child(1), indent + 1);
                return s + ")";
            }
            case aura::ast::NodeTag::Define: {
                return "(define " + std::string(workspace_pool_->resolve(v.sym_id)) + " "
                    + (v.children.empty() ? "()" : self(v.child(0), indent + 1)) + ")";
            }
            case aura::ast::NodeTag::IfExpr: {
                std::string s = "(if";
                for (std::size_t i = 0; i < v.children.size(); ++i)
                    s += " " + self(v.child(i), indent + 1);
                return s + ")";
            }
            case aura::ast::NodeTag::Begin: {
                std::string s = "(begin";
                for (std::size_t i = 0; i < v.children.size(); ++i)
                    s += " " + self(v.child(i), indent + 1);
                return s + ")";
            }
            case aura::ast::NodeTag::Set: {
                return "(set! " + std::string(workspace_pool_->resolve(v.sym_id)) + " "
                    + (v.children.empty() ? "()" : self(v.child(0), indent + 1)) + ")";
            }
            case aura::ast::NodeTag::Quote: {
                return "(quote " + (v.children.empty() ? "()" : self(v.child(0), indent + 1)) + ")";
            }
            case aura::ast::NodeTag::Pair: {
                return "(" + (v.children.empty() ? "()" : self(v.child(0), indent + 1) + " . " + self(v.child(1), indent + 1)) + ")";
            }
            case aura::ast::NodeTag::Export: {
                std::string s = "(export";
                for (auto pid : v.params)
                    s += " " + std::string(workspace_pool_->resolve(pid));
                return s + ")";
            }
            case aura::ast::NodeTag::MacroDef: {
                std::string s = "(defmacro (" + std::string(workspace_pool_->resolve(v.sym_id));
                for (auto pid : v.params)
                    s += " " + std::string(workspace_pool_->resolve(pid));
                s += ")";
                if (!v.children.empty())
                    s += " " + self(v.child(0), indent + 1);
                return s + ")";
            }
            default: {
                // Fallback: generic node dump for unknown types
                return std::format("<{}>", static_cast<int>(v.tag));
            }
            }
        };
        
        auto src = unparse(workspace_flat_->root, 0);
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(src));
        return make_string(id);
    });

    // (api-reference) — Return all registered primitives as a string for LLM reference
    primitives_.add("api-reference", [this](const auto&) -> EvalValue {
        std::string out = "Available Aura primitives:\n\n";
        for (std::size_t i = 0; i < primitives_.slot_count(); ++i) {
            auto name = primitives_.name_for_slot(i);
            if (!name.empty()) {
                out += "  " + std::string(name) + "\n";
            }
        }
        out += "\nStandard library: (require std/name) loads with prefix (std/name:func-name)\n";
        out += "Or (require std/name all:) for bare names\n";
        out += "Try it: (require std/list all:) then (sort (list 3 1 2))\n";
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(out));
        return make_string(id);
    });

    // (eval-current) — Evaluate the current workspace AST
    primitives_.add("eval-current", [this](const auto&) {
        coverage_counters_[2]++;
        if (!workspace_flat_ || !workspace_pool_) return make_void();
        auto expanded = aura::compiler::macro_expand_all(
            *workspace_flat_, *workspace_pool_, workspace_flat_->root);
        coverage_counters_[4]++;
        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
        // Clear dirty flags after successful eval
        workspace_flat_->clear_all_dirty();
        if (!result) return make_void();
        return *result;
    });

    // (eval-current-output) — Evaluate workspace, return display output + value
    // Captures all display output during eval to a string.
    primitives_.add("eval-current-output", [this](const auto&) {
        if (!workspace_flat_ || !workspace_pool_) return make_void();
        auto expanded = aura::compiler::macro_expand_all(
            *workspace_flat_, *workspace_pool_, workspace_flat_->root);
        // Redirect stdout to capture display output
        // Use a temp file or pipe approach
        // For now, return the value (display output still goes to stdout)
        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
        workspace_flat_->clear_all_dirty();
        if (!result) return make_void();
        return *result;
    });

    // (query:find name) — Find all node IDs with matching symbol name
    primitives_.add("query:find", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size() || !workspace_flat_ || !workspace_pool_) return make_void();
        auto& flat = *workspace_flat_;
        auto name = string_heap_[idx];
        auto sym = workspace_pool_->intern(name);
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.sym_id == sym) {
                auto pid = pairs_.size();
                pairs_.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:children node-id) — Get children node IDs
    primitives_.add("query:children", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_void();
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) return make_void();
        auto v = flat.get(node);
        EvalValue result = make_void();
        for (std::size_t i = v.children.size(); i > 0; --i) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(v.child(i - 1))), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (query:node node-id) — Get node details as list (tag value type sym-id)
    primitives_.add("query:node", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_ || !workspace_pool_) return make_void();
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) return make_void();
        auto v = flat.get(node);

        // Build result: list of (tag-id value sym-name children-count)
        EvalValue result = make_void();

        // children-count
        auto pid = pairs_.size();
        pairs_.push_back({make_int(static_cast<std::int64_t>(v.children.size())), result});
        result = make_pair(pid);

        // value (int or string content for string literals)
        if (v.has_int() && v.tag != aura::ast::NodeTag::LiteralString) {
            pid = pairs_.size();
            pairs_.push_back({make_int(v.int_value), result});
            result = make_pair(pid);
        } else if (v.sym_id != aura::ast::INVALID_SYM) {
            auto sym_name = std::string(workspace_pool_->resolve(v.sym_id));
            auto sid = string_heap_.size();
            string_heap_.push_back(sym_name);
            pid = pairs_.size();
            pairs_.push_back({make_string(sid), result});
            result = make_pair(pid);
        }

        // tag-id (integer tag == NodeTag enum value)
        pid = pairs_.size();
        pairs_.push_back({make_int(static_cast<std::int64_t>(v.tag)), result});
        result = make_pair(pid);

        return result;
    });

    // (query:calls name) — Find all call sites of a named function
    primitives_.add("query:calls", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]) || !workspace_flat_ || !workspace_pool_) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto& flat = *workspace_flat_;
        auto name = string_heap_[idx];
        auto sym = workspace_pool_->intern(name);
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == aura::ast::NodeTag::Variable && callee.sym_id == sym) {
                    auto pid = pairs_.size();
                    pairs_.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                }
            }
        }
        return result;
    });

    // (mutate:rebind name new-code-string "summary") — Replace function definition by name
    // Unlike mutate:replace-value, this works by function name (no node ID needed).
    // Parses new code INTO the existing workspace FlatAST, then redirects the old
    // Define's value reference to the newly parsed nodes. All pre-existing mutations
    // on other nodes are preserved.
    primitives_.add("mutate:rebind", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ || !workspace_pool_)
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto code_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
            return make_bool(false);
        auto& flat = *workspace_flat_;
        auto name = string_heap_[name_idx];
        auto sym = workspace_pool_->intern(name);

        // Find old Define node by name
        aura::ast::NodeId old_define = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                old_define = id;
                break;
            }
        }
        if (old_define == aura::ast::NULL_NODE) return make_bool(false);

        // Parse new code INTO workspace flat (append mode). All new node IDs
        // are valid in the same FlatAST and can be cross-referenced.
        auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_bool(false);

        // The parsed root may be a Define (if code includes "(define (name ...) ...)")
        // or a bare expression (just the value/lambda). Extract the value.
        aura::ast::NodeId new_value = pr.root;
        auto root_v = flat.get(pr.root);
        if (root_v.tag == aura::ast::NodeTag::Define) {
            // New code is a full define — extract its value child
            if (root_v.children.empty()) return make_bool(false);
            new_value = root_v.child(0);
        }

        // Record mutation on the old define node
        std::string summary = (a.size() > 2 && is_string(a[2]))
            ? string_heap_[as_string_idx(a[2])] : "rebind " + name;
        flat.add_mutation(old_define, "rebind", name, summary, summary);

        // Redirect old Define's value child to the new nodes
        // This is a valid NodeId in workspace_flat_ since we parsed into it
        flat.set_child(old_define, 0, new_value);
        return make_bool(true);
    });

    // ═══════════════════════════════════════════════════════════════
    // P1: Query/Transform EDSL 扩展
    // ═══════════════════════════════════════════════════════════════

    // (query:parent node-id) — Find parent node IDs (nodes whose children include this ID)
    primitives_.add("query:parent", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_void();
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size()) return make_void();
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    auto pid = pairs_.size();
                    pairs_.push_back({make_int(static_cast<std::int64_t>(id)), result});
                    result = make_pair(pid);
                    break;
                }
            }
        }
        return result;
    });

    // (query:siblings node-id) — Find sibling node IDs (other children of same parent)
    primitives_.add("query:siblings", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_void();
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size()) return make_void();
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            bool parent_of_target = false;
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) { parent_of_target = true; break; }
            }
            if (parent_of_target) {
                for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                    auto child = v.child(ci);
                    if (child != target) {
                        auto pid = pairs_.size();
                        pairs_.push_back({make_int(static_cast<std::int64_t>(child)), result});
                        result = make_pair(pid);
                    }
                }
            }
        }
        return result;
    });

    // (query:node-type tag-name) — Find all nodes with a given NodeTag name
    // Tag names: LiteralInt, Variable, Call, IfExpr, Lambda, Let, LetRec,
    //            Define, Begin, Set, Quote, LiteralString, TypeAnnotation,
    //            Coercion, LiteralFloat, MacroDef
    primitives_.add("query:node-type", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]) || !workspace_flat_)
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto target_name = string_heap_[idx];
        auto& flat = *workspace_flat_;

        // Convert tag name to NodeTag enum
        aura::ast::NodeTag target_tag = static_cast<aura::ast::NodeTag>(-1);
        bool found_tag = false;
        for (auto& m : aura::ast::kNodeMeta) {
            if (m.name == target_name && m.name != "<gap>") {
                target_tag = m.tag;
                found_tag = true;
                break;
            }
        }
        if (!found_tag) return make_void();

        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (flat.get(id).tag == target_tag) {
                auto pid = pairs_.size();
                pairs_.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P8: Query/Transform EDSL 扩展 — pattern matching
    // ═══════════════════════════════════════════════════════════════

    // (query:pattern "expr") — Find all nodes matching a structural pattern
    //
    // Pattern syntax:
    //   (+ 1 2)     — exact match: Call("+", 1, 2)
    //   (+ 1 ...)   — wildcard: "..." matches any single subtree
    //   fib         — matches a Variable named "fib"
    //
    // The pattern is parsed as an S-expression. A Variable named "..." acts as
    // wildcard and matches any single node or subtree.
    primitives_.add("query:pattern", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]) || !workspace_flat_ || !workspace_pool_)
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();

        // Parse pattern string into its own FlatAST (separate from workspace)
        auto alloc = arena_->allocator();
        auto* pat_pool = arena_->create<aura::ast::StringPool>(alloc);
        auto* pat_flat = arena_->create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(string_heap_[idx], *pat_flat, *pat_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_void();

        // Intern "..." in the pattern pool for wildcard matching
        auto wildcard_sym = pat_pool->intern("...");

        // Recursive subtree matcher
        std::function<bool(aura::ast::NodeId, aura::ast::NodeId)> match_subtree;
        match_subtree = [&, wildcard_sym](aura::ast::NodeId ws_id, aura::ast::NodeId pat_id) -> bool {
            if (pat_id >= pat_flat->size()) return ws_id >= workspace_flat_->size();
            if (ws_id >= workspace_flat_->size() || pat_id == aura::ast::NULL_NODE)
                return (pat_id == aura::ast::NULL_NODE) ? (ws_id == aura::ast::NULL_NODE) : false;

            auto ws_node = workspace_flat_->get(ws_id);
            auto pat_node = pat_flat->get(pat_id);

            // Wildcard "..." matches any single subtree
            if (pat_node.tag == aura::ast::NodeTag::Variable && pat_node.sym_id == wildcard_sym)
                return true;

            // Same tag required
            if (ws_node.tag != pat_node.tag) return false;

            switch (pat_node.tag) {
            case aura::ast::NodeTag::LiteralInt:
                return ws_node.int_value == pat_node.int_value;
            case aura::ast::NodeTag::LiteralFloat:
                return ws_node.float_value == pat_node.float_value;
            case aura::ast::NodeTag::Variable:
            case aura::ast::NodeTag::LiteralString:
                return workspace_pool_->resolve(ws_node.sym_id) == pat_pool->resolve(pat_node.sym_id);
            case aura::ast::NodeTag::MacroDef:
                return true;
            default:
                if (ws_node.children.size() != pat_node.children.size()) return false;
                for (std::size_t ci = 0; ci < ws_node.children.size(); ++ci) {
                    if (!match_subtree(ws_node.child(ci), pat_node.child(ci)))
                        return false;
                }
                return true;
            }
        };

        auto& flat = *workspace_flat_;
        EvalValue result = make_void();

        // Walk every node in workspace and try matching at each position
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (match_subtree(id, pr.root)) {
                auto pid = pairs_.size();
                pairs_.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }

        return result;
    });

    // (mutate:set-body name-str new-body-code-str) — Replace function body by name
    // Finds (define (name params) ...) and replaces the Lambda body.
    // Parses new body INTO the workspace FlatAST so all node IDs are valid.
    primitives_.add("mutate:set-body", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ || !workspace_pool_)
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto code_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
            return make_bool(false);
        auto& flat = *workspace_flat_;
        auto name = string_heap_[name_idx];
        auto sym = workspace_pool_->intern(name);

        // Find Define node with matching symbol name
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                // The Define should have one child: a Lambda
                if (v.children.size() != 1) return make_bool(false);
                auto lambda_id = v.child(0);
                auto lv = flat.get(lambda_id);
                if (lv.tag != aura::ast::NodeTag::Lambda) return make_bool(false);

                // Parse new body INTO workspace flat (all IDs stay valid)
                auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
                if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_bool(false);

                // Record mutation
                flat.add_mutation(id, "set-body", name, name,
                    "set-body " + name);

                // Replace the Lambda's body — pr.root is a valid node in workspace_flat_
                flat.set_child(lambda_id, 0, pr.root);
                return make_bool(true);
            }
        }
        return make_bool(false);
    });

    // (mutate:remove-node node-id) — Remove a node by setting parent's reference to NULL_NODE
    // The node entry remains in the FlatAST but is disconnected from the tree.
    // The tree walker in eval_flat skips NULL_NODE children.
    primitives_.add("mutate:remove-node", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_) return make_bool(false);
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size()) return make_bool(false);

        // Find parent and remove target from its children
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.children.empty()) continue;
            auto children = flat.children(id);
            for (std::size_t ci = 0; ci < children.size(); ++ci) {
                if (children[ci] == target) {
                    children[ci] = aura::ast::NULL_NODE;
                    flat.add_mutation(id, "remove-node",
                        std::to_string(target), "",
                        "remove node " + std::to_string(target));
                    return make_bool(true);
                }
            }
        }
        return make_bool(false);
    });

    // (mutate:insert-child parent-id position code-string "summary")
    // Insert a child node into a parent's children list at the given position.
    // Position 0 = first child, child_count = append at end.
    // Parses code-string INTO workspace, preserving all existing nodes/IDs.
    primitives_.add("mutate:insert-child", [this](const auto& a) {
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_string(a[2])
            || !workspace_flat_ || !workspace_pool_)
            return make_bool(false);
        auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto pos = static_cast<std::uint32_t>(as_int(a[1]));
        auto code_idx = as_string_idx(a[2]);
        if (code_idx >= string_heap_.size()) return make_bool(false);
        auto& flat = *workspace_flat_;
        if (parent >= flat.size()) return make_bool(false);

        // Parse child code INTO workspace (append mode — all IDs stay valid)
        auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_bool(false);

        // Insert the parsed node at position pos in parent's children
        flat.insert_child(parent, pos, pr.root);

        std::string summary = (a.size() > 3 && is_string(a[3]))
            ? string_heap_[as_string_idx(a[3])] : "insert child at " + std::to_string(pos);
        flat.add_mutation(parent, "insert-child", std::to_string(pos), summary, summary);
        return make_int(static_cast<std::int64_t>(pr.root));
    });

    // (typecheck-current) — Type check the workspace AST
    // Uses a persistent TypeRegistry across calls so type IDs are stable.
    // Full traversal for now; incremental skip (dirty-aware) requires
    // TypeChecker to accept a dirty filter — future work.
    primitives_.add("typecheck-current", [this](const auto&) {
        coverage_counters_[1]++;
        if (!workspace_flat_ || !workspace_pool_) {
            auto eidx = string_heap_.size();
            string_heap_.push_back("no workspace");
            return make_string(eidx);
        }

        // Lazily create persistent type registry (stable TypeIds across calls)
        if (!type_registry_) {
            type_registry_ = new aura::core::TypeRegistry();
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;

        auto result = tc.infer_flat(*workspace_flat_, *workspace_pool_,
                                     workspace_flat_->root, diag);

        // Cache inferred TypeIds back to type_id_ for future incremental use
        auto& flat = *workspace_flat_;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            // Only overwrite if dirty (clean nodes already have valid cached type)
            if (flat.is_dirty(id)) {
                // We'd need a per-node type query, which the TypeChecker doesn't expose
                // For now: mark the full traversal as a TODO for incremental
            }
        }

        std::string out = "type: " + treg.format_type(result) + "\n";
        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            out += "diagnostics:\n";
            for (auto& d : all_diags) {
                out += "  [" + std::to_string(static_cast<int>(d.kind))
                     + "] " + d.format() + "\n";
            }
        }

        flat.clear_all_dirty();

        auto sidx = string_heap_.size();
        string_heap_.push_back(out);
        return make_string(sidx);
    });
}

// ── C FFI: c-load / c-func ─────────────────────────────────
// Global FFI state (shared across all evaluator instances)
// In production, this should be per-evaluator.
static std::vector<void*> g_ffi_libs;
struct FFIFunc {
    void* fn_ptr;
    std::string name;
    int ret_type;          // 0=void, 1=Int, 2=Float, 3=String, 4=Opaque
    std::vector<int> arg_types;  // per-arg type tags
};
static std::vector<FFIFunc> g_ffi_funcs;

// ── Env::lookup_cell_ptr: returns EvalValue* ──────────────────
EvalValue* Env::lookup_cell_ptr(const std::string& n, std::vector<EvalValue>* cells) const {
    if (!cells) return nullptr;
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second)) {
                auto ci = as_cell_id(b.second);
                if (ci < cells->size()) return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < cells->size()) return &(*cells)[ci];
                }
                return nullptr;
            }
        }
    }
    return nullptr;
}

std::optional<PrimFn> Primitives::lookup(const std::string& n) const {
    auto i = table_.find(n);
    return i != table_.end() ? std::optional(i->second) : std::nullopt;
}

Evaluator::Evaluator() {
    top_.set_primitives(&primitives_);
    top_.set_cells(&cells_);
    primitives_.set_string_heap(&string_heap_);
    init_pair_primitives();

    // ── C FFI primitives ────────────────────────────────
    primitives_.add("c-load", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) return make_int(0);
        auto path = string_heap_[types::as_string_idx(a[0])];
        void* lib = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            auto err = ::dlerror();
            auto msg = err ? std::string(err) : "dlopen failed";
            fprintf(stderr, "c-load: %s\n", msg.c_str());
            return make_int(0);
        }
        auto idx = g_ffi_libs.size();
        g_ffi_libs.push_back(lib);
        return make_int(static_cast<std::int64_t>(idx));
    });

    // Parse type signature string like "(Int Float) -> Float" or "(String) -> Int"
    auto parse_ffi_sig = [](const std::string& sig, int& ret_type, std::vector<int>& arg_types) -> bool {
        auto arrow = sig.find("->");
        if (arrow == std::string::npos) return false;
        auto arg_part = sig.substr(1, arrow - 1);
        auto ret_part = sig.substr(arrow + 2);
        auto type_to_int = [](const std::string& tn) -> int {
            auto t = tn;
            while (!t.empty() && t.front() == ' ') t = t.substr(1);
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (t == "Int")    return 1;
            if (t == "Float")  return 2;
            if (t == "String") return 3;
            if (t == "Opaque") return 4;
            if (t == "Void")   return 0;
            return -1;
        };
        std::string cur;
        for (auto c : arg_part) {
            if (c == ' ' || c == '(' || c == ')') {
                if (!cur.empty()) {
                    int at = type_to_int(cur);
                    if (at < 0) return false;
                    arg_types.push_back(at);
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) {
            int at = type_to_int(cur);
            if (at < 0) return false;
            arg_types.push_back(at);
        }
        ret_type = type_to_int(ret_part);
        if (ret_type < 0) return false;
        return true;
    };

    primitives_.add("c-func", [this, &parse_ffi_sig](const auto& a) -> EvalValue {
        coverage_counters_[8]++;
        // (c-func lib-id "name" sig-string)  e.g. (c-func 0 "sqrt" "(Float) -> Float")
        // lib-id -1 uses RTLD_DEFAULT (no c-load needed) — architecture independent.
        // Or legacy: (c-func lib-id "name" ret-int arg-int...)
        if (a.size() < 3 || !types::is_int(a[0]) || !types::is_string(a[1])) {
            fprintf(stderr, "c-func: expected (c-func lib-id \"name\" signature\n");
            return make_int(0);
        }
        auto raw_lib_id = types::as_int(a[0]);
        void* lib = RTLD_DEFAULT;
        if (raw_lib_id >= 0) {
            auto lib_idx = static_cast<std::size_t>(raw_lib_id);
            if (lib_idx >= g_ffi_libs.size()) {
                fprintf(stderr, "c-func: invalid library handle %zu\n", lib_idx);
                return make_int(0);
            }
            lib = g_ffi_libs[lib_idx];
        }
        auto name = string_heap_[types::as_string_idx(a[1])];
        int ret_type = 1;
        std::vector<int> arg_types;
        if (types::is_string(a[2])) {
            auto sig = string_heap_[types::as_string_idx(a[2])];
            if (!parse_ffi_sig(sig, ret_type, arg_types)) {
                fprintf(stderr, "c-func: invalid signature '%s' -- expected '(Type) -> Type'\n", sig.c_str());
                return make_int(0);
            }
        } else if (types::is_int(a[2])) {
            ret_type = static_cast<int>(types::as_int(a[2]));
            for (std::size_t i = 3; i < a.size(); ++i)
                if (types::is_int(a[i])) arg_types.push_back(static_cast<int>(types::as_int(a[i])));
        } else {
            fprintf(stderr, "c-func: third arg must be signature string or ret-type int\n");
            return make_int(0);
        }
        auto* fn_ptr = ::dlsym(lib, name.c_str());
        if (!fn_ptr) {
            auto* err = ::dlerror();
            fprintf(stderr, "c-func: symbol '%s' not found: %s\n", name.c_str(), err ? err : "unknown");
            return make_int(0);
        }
        auto fidx = g_ffi_funcs.size();
        g_ffi_funcs.push_back({fn_ptr, name, ret_type, std::move(arg_types)});
        auto closure_id = static_cast<std::uint64_t>(fidx) | (1ULL << 63);
        return types::make_closure(closure_id);
    });

    build_primitive_slots();

    // ── Environment + HTTP primitives ────────────────────────
    primitives_.add("getenv", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) return make_void();
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto* val = ::getenv(name.c_str());
        if (!val) return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::string(val));
        return types::make_string(sidx);
    });

    // ── HTTP primitives (via curl CLI) ─────────────────────
    primitives_.add("http-get", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) return make_void();
        auto url = string_heap_[types::as_string_idx(a[0])];
        std::string cmd = "curl -s -f \"" + url + "\" 2>/dev/null";
        std::array<char, 4096> buf;
        std::string result;
        auto fp = ::popen(cmd.c_str(), "r");
        if (!fp) return make_void();
        while (::fgets(buf.data(), static_cast<int>(buf.size()), fp))
            result += buf.data();
        auto rc = ::pclose(fp);
        if (rc != 0 && result.empty()) return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return types::make_string(sidx);
    });

    primitives_.add("http-post", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        auto url = string_heap_[types::as_string_idx(a[0])];
        auto body = string_heap_[types::as_string_idx(a[1])];
        // Write body to temp file to avoid shell escaping issues
        std::string tmpfile = "/tmp/_aura_body_" + std::to_string(::time(nullptr));
        {
            std::ofstream ofs(tmpfile);
            ofs << body;
        }
        std::string cmd = "curl -s -X POST --data-binary @" + tmpfile;
        cmd += " -H \"Content-Type: application/json\"";
        // Add auth if provided (3rd arg = auth token)
        if (a.size() >= 3 && types::is_string(a[2])) {
            auto auth = string_heap_[types::as_string_idx(a[2])];
            cmd += " -H \"Authorization: Bearer " + auth + "\"";
        }
        cmd += " \"" + url + "\" 2>/dev/null";
        std::array<char, 4096> buf;
        std::string result;
        auto fp = ::popen(cmd.c_str(), "r");
        if (!fp) { std::remove(tmpfile.c_str()); return make_void(); }
        while (::fgets(buf.data(), static_cast<int>(buf.size()), fp))
            result += buf.data();
        ::pclose(fp);
        std::remove(tmpfile.c_str());
        if (result.empty()) return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return types::make_string(sidx);
    });
    // ── TCP socket primitives ────────────────────────────────
    primitives_.add("tcp-connect", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_int(a[1]))
            return make_void();
        auto host = string_heap_[types::as_string_idx(a[0])];
        auto port_str = std::to_string(types::as_int(a[1]));
        struct addrinfo hints{}, *res;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
            return make_void();
        int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { ::freeaddrinfo(res); return make_void(); }
        struct timeval tv;
        tv.tv_sec = 10; tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // Non-blocking connect with 8s timeout
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int conn_ret = ::connect(fd, res->ai_addr, res->ai_addrlen);
        if (conn_ret < 0) {
            if (errno == EINPROGRESS) {
                // Wait for connection with timeout
                struct pollfd pfd = {fd, POLLOUT, 0};
                conn_ret = ::poll(&pfd, 1, 8000);
                if (conn_ret <= 0) {
                    ::close(fd); ::freeaddrinfo(res);
                    return make_void();  // timeout or error
                }
                // Check if connect succeeded
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                    ::close(fd); ::freeaddrinfo(res);
                    return make_void();
                }
            } else {
                ::close(fd); ::freeaddrinfo(res);
                return make_void();
            }
        }
        ::fcntl(fd, F_SETFL, flags);  // restore blocking
        ::freeaddrinfo(res);
        return types::make_int(static_cast<std::int64_t>(fd));
    });

    primitives_.add("tcp-send", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_int(a[0]) || !types::is_string(a[1]))
            return make_int(-1);
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto sidx = types::as_string_idx(a[1]);
        if (sidx >= string_heap_.size()) return types::make_int(0);
        auto& data = string_heap_[sidx];
        auto sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        return types::make_int(static_cast<std::int64_t>(sent));
    });

    primitives_.add("tcp-recv", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto maxlen = static_cast<std::size_t>(a.size() >= 2 && types::is_int(a[1]) ? types::as_int(a[1]) : 4096);
        if (maxlen > 65536) maxlen = 65536;
        std::string buf(maxlen, '\0');
        auto n = ::recv(fd, buf.data(), maxlen, 0);
        if (n <= 0) return make_void();
        buf.resize(static_cast<std::size_t>(n));
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::move(buf));
        return types::make_string(sidx);
    });

    primitives_.add("tcp-close", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0])) return make_void();
        ::close(static_cast<int>(types::as_int(a[0])));
        return make_void();
    });

    // ── intend — 纯循环管理器 ────────────────────────────────
    // (intend goal generator-fn verifier-fn [fixer-fn] [max-attempts])
    //
    // 不管理 LLM 调用、不构建 prompt、不做 JSON 解析。
    // 只做循环编排。LLM 交互通过传入的 Aura 函数完成。
    //
    // - generator-fn: (lambda (goal) → code-string)
    // - verifier-fn:  (lambda (code) → "#t" for pass, else error-string)
    // - fixer-fn:     (lambda (code error goal) → new-code-string, optional)
    // - max-attempts: int (optional, default 3)
    primitives_.add("intend", [this](const auto& a) -> EvalValue {
        if (a.size() < 3) return make_void();
        if (!types::is_string(a[0]) || !types::is_closure(a[1]) || !types::is_closure(a[2]))
            return make_void();

        auto goal = string_heap_[types::as_string_idx(a[0])];
        auto gen_cid = types::as_closure_id(a[1]);      // generator
        auto ver_cid = types::as_closure_id(a[2]);      // verifier

        // Optional fixer_fn
        bool has_fixer = a.size() >= 4 && types::is_closure(a[3]);
        auto fix_cid = has_fixer
            ? types::as_closure_id(a[3])
            : std::uint64_t{0};

        // Optional max_attempts
        int max_attempts = 3;
        if ((has_fixer && a.size() >= 5 && types::is_int(a[4]))
            || (!has_fixer && a.size() >= 4 && types::is_int(a[3])))
        {
            auto idx = has_fixer ? 4 : 3;
            max_attempts = static_cast<int>(types::as_int(a[idx]));
        }

        auto t0 = std::chrono::steady_clock::now();
        timeline_.clear();
        timeline_.push_back("start:" + goal);

        std::string current_code_str;
        std::string last_error;
        std::vector<std::string> errors;
        std::vector<std::string> error_types;
        std::vector<std::string> generated_codes;
        std::uint64_t llm_call_count = 0;

        auto classify_error = [](const std::string& err) -> std::string {
            if (err.find("unbound variable") != std::string::npos) return "unbound-variable";
            if (err.find("type mismatch") != std::string::npos) return "type-mismatch";
            if (err.find("division by zero") != std::string::npos) return "div-zero";
            if (err.find("syntax") != std::string::npos || err.find("unbalanced") != std::string::npos) return "syntax-error";
            if (err.find("timeout") != std::string::npos) return "timeout";
            if (err.find("recursion") != std::string::npos || err.find("stack") != std::string::npos) return "recursion-limit";
            if (err.find("bad-code") != std::string::npos) return "bad-code";
            if (err.find("verification failed") != std::string::npos) return "verification-failed";
            return "other";
        };

        // Call a closure, return string result
        auto call_fn = [&](std::uint64_t cid,
                           const std::vector<types::EvalValue>& args) -> std::string {
            auto opt = apply_closure(cid, args);
            if (!opt) return {};
            auto& val = *opt;
            if (types::is_string(val))
                return string_heap_[types::as_string_idx(val)];
            if (types::is_void(val)) return {};
            if (types::is_int(val)) return std::to_string(types::as_int(val));
            if (types::is_bool(val)) return types::as_bool(val) ? "#t" : "#f";
            return {};
        };

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            std::string code_str;
            if (attempt == 1 || current_code_str.empty()) {
                auto gs = string_heap_.size();
                string_heap_.push_back(goal);
                code_str = call_fn(gen_cid, {types::make_string(gs)});
                llm_call_count++;
                if (code_str.empty()) {
                    timeline_.push_back("attempt_" + std::to_string(attempt) + ":empty from generator");
                    errors.push_back("empty from generator");
                    error_types.push_back("empty");
                    continue;
                }
            } else {
                if (!has_fixer) {
                    timeline_.push_back("attempt_" + std::to_string(attempt) + ":no fixer, stopping");
                    break;
                }
                auto cs = string_heap_.size();
                string_heap_.push_back(current_code_str);
                auto es = string_heap_.size();
                string_heap_.push_back(last_error);
                auto gs = string_heap_.size();
                string_heap_.push_back(goal);
                code_str = call_fn(fix_cid, {
                    types::make_string(cs),
                    types::make_string(es),
                    types::make_string(gs)
                });
                llm_call_count++;
                if (code_str.empty()) {
                    timeline_.push_back("attempt_" + std::to_string(attempt) + ":empty from fixer");
                    errors.push_back("empty from fixer");
                    error_types.push_back("empty");
                    continue;
                }
            }
            generated_codes.push_back(code_str);

            auto cv = string_heap_.size();
            string_heap_.push_back(code_str);
            auto ver = call_fn(ver_cid, {types::make_string(cv)});

            if (ver.find("#t") == 0) {
                current_code_str = code_str;
                timeline_.push_back("attempt_" + std::to_string(attempt) + ":success");
                auto result = "#(status:\"ok\" goal:\"" + goal + "\" iterations:" + std::to_string(attempt) + ")";
                auto rs = string_heap_.size();
                string_heap_.push_back(result);

                auto t1 = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                IntendRecord rec;
                rec.record_id = next_record_id_++;
                rec.strategy_name = "default";
                rec.task_desc = goal;
                rec.success = true;
                rec.attempts = attempt;
                rec.errors = errors;
                rec.error_types = error_types;
                rec.generated_codes = generated_codes;
                rec.llm_call_count = llm_call_count;
                rec.llm_tokens = 0;
                rec.duration_ms = static_cast<std::uint64_t>(duration);
                rec.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
                rec.parent_record_id = 0;
                intend_history_.push_back(rec);
                if (intend_history_.size() > MAX_HISTORY_SIZE)
                    intend_history_.erase(intend_history_.begin());
                return types::make_string(rs);
            }

            current_code_str = code_str;
            last_error = ver.empty() ? "verification failed" : ver;
            errors.push_back(last_error);
            error_types.push_back(classify_error(last_error));
            timeline_.push_back("attempt_" + std::to_string(attempt) + ":" + last_error);
        }

        auto t1 = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        IntendRecord rec;
        rec.record_id = next_record_id_++;
        rec.strategy_name = "default";
        rec.task_desc = goal;
        rec.success = false;
        rec.attempts = max_attempts;
        rec.errors = errors;
        rec.error_types = error_types;
        rec.generated_codes = generated_codes;
        rec.llm_call_count = llm_call_count;
        rec.llm_tokens = 0;
        rec.duration_ms = static_cast<std::uint64_t>(duration);
        rec.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
        rec.parent_record_id = 0;
        intend_history_.push_back(rec);
        if (intend_history_.size() > MAX_HISTORY_SIZE)
            intend_history_.erase(intend_history_.begin());

        auto result = "#(status:\"failed\" goal:\"" + goal + "\" iterations:" + std::to_string(max_attempts)
                    + " last-error:\"" + last_error + "\")";
        timeline_.push_back("failed:" + last_error);
        auto rs = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(rs);
    });
// ── intend-history — 查询意图执行时间线 ────────────────────
    primitives_.add("intend-history", [this](const auto&) -> EvalValue {
        std::string result;
        for (std::size_t i = 0; i < timeline_.size(); ++i) {
            result += std::to_string(i) + ":" + timeline_[i] + "\n";
        }
        if (result.empty()) result = "(empty)";
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // ── intend-analytics — 聚合 intend 历史数据 ────────────────
    primitives_.add("intend-analytics", [this](const auto& a) -> EvalValue {
        std::string filter_strategy;
        std::string filter_field;
        std::string filter_value;
        std::size_t arg_idx = 0;

        if (a.size() > arg_idx && types::is_string(a[arg_idx])) {
            filter_strategy = string_heap_[types::as_string_idx(a[arg_idx])];
            arg_idx++;
        }
        if (a.size() > arg_idx + 2 && types::is_string(a[arg_idx])
            && string_heap_[types::as_string_idx(a[arg_idx])] == ":filter") {
            if (types::is_string(a[arg_idx + 1]))
                filter_field = string_heap_[types::as_string_idx(a[arg_idx + 1])];
            if (types::is_string(a[arg_idx + 2]))
                filter_value = string_heap_[types::as_string_idx(a[arg_idx + 2])];
        }

        std::uint64_t total = 0, successes = 0, total_attempts = 0;
        std::uint64_t total_llm_calls = 0, total_duration = 0;
        std::map<std::string, std::uint64_t> error_type_counts;
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> task_stats;

        for (auto& rec : intend_history_) {
            if (!filter_strategy.empty() && rec.strategy_name != filter_strategy) continue;
            if (!filter_field.empty()) {
                if (filter_field == "error-type") {
                    bool matches = false;
                    for (auto& et : rec.error_types) {
                        if (et.find(filter_value) != std::string::npos) { matches = true; break; }
                    }
                    if (!matches) continue;
                } else continue;
            }
            total++;
            if (rec.success) successes++;
            total_attempts += rec.attempts;
            total_llm_calls += rec.llm_call_count;
            total_duration += rec.duration_ms;
            for (auto& et : rec.error_types) error_type_counts[et]++;
            auto key = rec.task_desc.substr(0, std::min<std::size_t>(rec.task_desc.size(), std::size_t{60}));
            auto& ts = task_stats[key];
            ts.second++;
            if (rec.success) ts.first++;
        }

        std::string result = "#(analytics";
        result += " total-runs:" + std::to_string(total);
        result += " success-rate:";
        if (total > 0) result += std::to_string(static_cast<double>(successes) / static_cast<double>(total));
        else result += "0";
        result += " avg-attempts:";
        if (total > 0) result += std::to_string(static_cast<double>(total_attempts) / static_cast<double>(total));
        else result += "0";
        result += " total-llm-calls:" + std::to_string(total_llm_calls);
        result += " avg-duration-ms:";
        if (total > 0) result += std::to_string(total_duration / total);
        else result += "0";
        result += " top-errors:(";
        for (auto& [et, count] : error_type_counts) {
            result += " " + et + ":" + std::to_string(count);
        }
        result += ")";
        result += " by-task:(";
        for (auto& [task, stats] : task_stats) {
            result += " (" + task + " " + std::to_string(stats.first) + "/" + std::to_string(stats.second) + ")";
        }
        result += "))";

        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });


    // ── define-strategy — 定义策略 ──────────────────────────
    primitives_.add("define-strategy", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto body = string_heap_[types::as_string_idx(a[1])];
        for (auto& s : strategies_) {
            if (s.name == name) { s.body = body; return make_bool(true); }
        }
        strategies_.push_back({name, body});
        return make_bool(true);
    });
    // ── register-strategy! — 注册/更新策略 ──────────────
    primitives_.add("register-strategy!", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto body = string_heap_[types::as_string_idx(a[1])];
        for (auto& s : strategies_) {
            if (s.name == name) { s.body = body; return make_bool(true); }
        }
        strategies_.push_back({name, body});
        return make_bool(true);
    });
    // ── strategy-field — 读取策略字段 ──────────────────────
    primitives_.add("strategy-field", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto field = string_heap_[types::as_string_idx(a[1])];
        for (auto& s : strategies_) {
            if (s.name == name && field == "body") {
                auto sid = string_heap_.size();
                string_heap_.push_back(s.body);
                return types::make_string(sid);
            }
        }
        return make_void();
    });
    // ── strategy-set-field! — 修改策略字段（白名单）───────────
    primitives_.add("strategy-set-field!", [this](const auto& a) -> EvalValue {
        if (a.size() < 3 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto field = string_heap_[types::as_string_idx(a[1])];
        if (field != "body" || !types::is_string(a[2])) return make_bool(false);
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto new_body = string_heap_[types::as_string_idx(a[2])];
        for (auto& s : strategies_) {
            if (s.name == name) { s.body = new_body; return make_bool(true); }
        }
        return make_bool(false);
    });
    // ── strategy-inspect — 一键检视 ────────────────────────
    primitives_.add("strategy-inspect", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) return make_void();
        auto name = string_heap_[types::as_string_idx(a[0])];
        for (auto& s : strategies_) {
            if (s.name == name) {
                std::string result = "#(strategy-inspect name:\"";
                result += s.name + "\" body:\"";
                result += s.body + "\")";
                auto sid = string_heap_.size();
                string_heap_.push_back(result);
                return types::make_string(sid);
            }
        }
        return make_void();
    });

    // ── coverage-report — 编译器路径覆盖率 ──────────────────
    primitives_.add("coverage-report", [this](const auto&) -> EvalValue {
        std::string result = "#(coverage";
        for (int i = 0; i < 16; i++) {
            if (coverage_counters_[i] > 0) {
                std::string name;
                switch (i) {
                    case 0: name = "parser"; break;
                    case 1: name = "typecheck"; break;
                    case 2: name = "eval"; break;
                    case 3: name = "jit"; break;
                    case 4: name = "macro"; break;
                    case 5: name = "edsl-set-code"; break;
                    case 6: name = "edsl-query"; break;
                    case 7: name = "edsl-mutate"; break;
                    case 8: name = "ffi"; break;
                    default: name = "reserved-" + std::to_string(i); break;
                }
                result += " " + name + ":" + std::to_string(coverage_counters_[i]);
            }
        }
        result += ")";
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });


}

// slot_for_name: find the slot for a primitive name
std::size_t Primitives::slot_for_name(const std::string& name) const {
    for (std::size_t i = 0; i < ordered_names_.size(); ++i) {
        if (ordered_names_[i] == name) return i;
    }
    return std::numeric_limits<std::size_t>::max();
}

void Evaluator::build_primitive_slots() {
    // No longer needed — Primitives manages ordering internally
}

// ── Module path resolution ──────────────────────────────────
std::string Evaluator::resolve_module_path(const std::string& path) const {
    auto try_load = [](const std::string& full) -> std::optional<std::string> {
        for (auto candidate : {full, full + ".aura"}) {
            std::ifstream probe(candidate);
            if (probe) {
                probe.close();
                char real[4096];
                if (::realpath(candidate.c_str(), real))
                    return std::string(real);
                return candidate;
            }
        }
        return std::nullopt;
    };

    if (!path.empty() && path[0] == '/') {
        auto hit = try_load(path);
        if (hit) return *hit;
        return {};
    }

    // Search CWD first
    {
        char cwd_buf[4096];
        if (::getcwd(cwd_buf, sizeof(cwd_buf))) {
            auto hit = try_load(std::string(cwd_buf) + "/" + path);
            if (hit) return *hit;
        }
    }

    // Search AURA_PATH
    auto* env = ::getenv("AURA_PATH");
    if (env) {
        std::string aura_path(env);
        std::size_t start = 0, end;
        while ((end = aura_path.find(':', start)) != std::string::npos) {
            auto dir = aura_path.substr(start, end - start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit) return *hit;
            }
            start = end + 1;
        }
        if (start < aura_path.size()) {
            auto dir = aura_path.substr(start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit) return *hit;
            }
        }
    }

    // Auto-discover: try ../lib/ and ./lib/ (relative to executable / CWD)
    {
        // Try ../lib/ (common for build/aura → lib/ layout)
        auto hit = try_load("../lib/" + path);
        if (hit) return *hit;
    }
    {
        // Try ./lib/ (cwd-relative)
        auto hit = try_load("./lib/" + path);
        if (hit) return *hit;
    }

    return {};
}

// ── Load module file, return module object ────────────────
types::EvalValue Evaluator::load_module_file(const std::string& path) {
    // 1. Resolve path
    auto resolved = resolve_module_path(path);
    if (resolved.empty()) { std::println(std::cerr, "load_module_file: cannot resolve '{}'", path); return types::make_void(); }

    // 2. Check cache
    auto cache_it = module_cache_.find(resolved);
    if (cache_it != module_cache_.end()) {
        return types::make_module(cache_it->second);
    }

    // 3. Circular dependency detection
    if (loading_stack_.count(resolved)) {
        auto eidx = string_heap_.size();
        string_heap_.push_back("circular dependency: " + resolved);
        return types::make_void();
    }
    loading_stack_.insert(resolved);

    // 4. Read file
    std::ifstream f(resolved);
    if (!f) { loading_stack_.erase(resolved); return types::make_void(); }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    if (content.empty()) { loading_stack_.erase(resolved); return types::make_void(); }

    // 5. Parse
    if (!arena_) { loading_stack_.erase(resolved); std::println(std::cerr, "load_module_file: no arena"); return types::make_void(); }
    auto alloc = arena_->allocator();
    auto* pool_ptr = arena_->create<aura::ast::StringPool>(alloc);
    auto* flat_ptr = arena_->create<aura::ast::FlatAST>(alloc);
    auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: parse error for {}", resolved);
        if (!pr.error.empty()) std::println(std::cerr, "  {}", pr.error);
        return types::make_void();
    }
    flat_ptr->root = pr.root;

    // 6. Create isolated module env (child of top_ for primitive access)
    // Arena-allocate so closures captured during module eval stay valid
    auto* mod_env = arena_->create<Env>(&top_);
    mod_env->set_primitives(&primitives_);
    mod_env->set_cells(&cells_);

    // 7. Clear any stale export set from previous module loads
    if (current_export_set_) current_export_set_->clear();

    // 8. Evaluate module in its own env
    auto expanded = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
    auto result = eval_flat(*flat_ptr, *pool_ptr, expanded, *mod_env);

    // 9. Apply export filtering: if (export ...) was declared, remove unexported bindings
    if (current_export_set_ && !current_export_set_->empty()) {
        auto& bindings = mod_env->bindings();
        for (auto it = bindings.begin(); it != bindings.end(); ) {
            if (!current_export_set_->count(it->first)) {
                it = bindings.erase(it);
            } else {
                ++it;
            }
        }
        current_export_set_->clear();
    }

    // 10. Store module (pointer to arena-allocated env — persists forever)
    auto mod_idx = modules_.size();
    modules_.push_back(mod_env);
    module_cache_[resolved] = mod_idx;
    string_heap_.push_back(resolved);
    module_names_.push_back(resolved);

    // 10b. IR caching callback (registered by CompilerService)
    if (module_loaded_cb_) {
        module_loaded_cb_(content, resolved);
    }

    loading_stack_.erase(resolved);
    return types::make_module(mod_idx);
}

Env* Evaluator::copy_env(const Env& e) {
    return arena_ ? arena_->create<Env>(e) : nullptr;
}

// eval_in(ast::Expr*) removed — all evaluation uses eval_flat(FlatAST&) now

// apply_closure — looks up closures_, foreign functions, or IR bridge
std::optional<EvalValue> Evaluator::apply_closure(
    ClosureId cid,
    const std::vector<EvalValue>& args) {
    // Check for foreign function closure (high bit set)
    if (cid & (1ULL << 63)) {
        auto fidx = cid & ~(1ULL << 63);
        if (fidx < g_ffi_funcs.size()) {
            auto& ff = g_ffi_funcs[static_cast<std::size_t>(fidx)];
            void* fn_ptr = ff.fn_ptr;
            int ret_type = ff.ret_type;
            auto& arg_types = ff.arg_types;

            // Marshalling: dispatch based on type info
            std::int64_t i6[6] = {0,0,0,0,0,0};
            double d6[6] = {0,0,0,0,0,0};
            const char* s6[6] = {nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
            std::vector<std::string> str_bufs;  // keep strings alive
            bool any_float = false;

            for (std::size_t i = 0; i < args.size() && i < 6; ++i) {
                int atype = (i < arg_types.size()) ? arg_types[i] : 1;  // default Int
                if (atype == 2) {  // Float
                    if (types::is_float(args[i])) d6[i] = types::as_float(args[i]);
                    else if (types::is_int(args[i])) d6[i] = static_cast<double>(types::as_int(args[i]));
                    i6[i] = static_cast<std::int64_t>(d6[i]);
                    any_float = true;
                } else if (atype == 3) {  // String → char*
                    if (types::is_string(args[i])) {
                        auto idx = types::as_string_idx(args[i]);
                        if (idx < string_heap_.size()) {
                            str_bufs.push_back(string_heap_[idx]);
                            s6[i] = str_bufs.back().c_str();
                            i6[i] = reinterpret_cast<std::int64_t>(s6[i]);
                            d6[i] = 0.0;
                        }
                    }
                } else if (atype == 4) {  // Opaque (void*)
                    i6[i] = types::is_int(args[i]) ? types::as_int(args[i]) : 0;
                    d6[i] = 0.0;
                } else {  // Int (default)
                    if (types::is_int(args[i])) i6[i] = types::as_int(args[i]);
                    else if (types::is_float(args[i])) { i6[i] = static_cast<std::int64_t>(types::as_float(args[i])); any_float = true; }
                    d6[i] = static_cast<double>(i6[i]);
                }
            }

            std::int64_t result_i = 0;
            double result_f = 0.0;

            if (any_float) {
                auto f_fn = reinterpret_cast<double(*)(double,double,double,double,double,double)>(fn_ptr);
                result_f = f_fn(d6[0], d6[1], d6[2], d6[3], d6[4], d6[5]);
                if (ret_type == 2) return types::make_float(result_f);
                if (ret_type == 1) return types::make_int(static_cast<std::int64_t>(result_f));
                return types::make_float(result_f);
            } else {
                auto i_fn = reinterpret_cast<std::int64_t(*)(std::int64_t,std::int64_t,std::int64_t,std::int64_t,std::int64_t,std::int64_t)>(fn_ptr);
                result_i = i_fn(i6[0], i6[1], i6[2], i6[3], i6[4], i6[5]);
                if (ret_type == 2) return types::make_float(*reinterpret_cast<double*>(&result_i));
                if (ret_type == 3 && result_i != 0) {
                    // String return: char* → string_heap
                    auto s = reinterpret_cast<const char*>(static_cast<std::intptr_t>(result_i));
                    auto sidx = string_heap_.size();
                    string_heap_.emplace_back(s ? s : "");
                    return types::make_string(sidx);
                }
                if (ret_type == 4) return types::make_int(result_i);  // Opaque: pass as int
                return types::make_int(result_i);
            }
        }
        return std::nullopt;
    }

    // Try tree-walker closures first
    auto it = closures_.find(cid);
    if (it != closures_.end()) {
        auto& cl = it->second;
        Env ne(cl.env ? *cl.env : Env());
        ne.set_primitives(&primitives_);
        ne.set_cells(&cells_);
        if (cl.dotted) {
            // Dotted rest param: bind named params, collect rest into list
            std::size_t named_count = cl.params.size() - 1;
            for (std::size_t i = 0; i < named_count && i < args.size(); ++i)
                ne.bind(cl.params[i], args[i]);
            // Collect remaining args into a pair list for the rest param
            types::EvalValue rest = make_void();
            for (std::size_t i = args.size(); i > named_count; --i) {
                auto pid = pairs_.size();
                pairs_.push_back({args[i - 1], rest});
                rest = make_pair(pid);
            }
            ne.bind(cl.params.back(), rest);
        } else {
            for (std::size_t i = 0; i < cl.params.size() && i < args.size(); ++i)
                ne.bind(cl.params[i], args[i]);
        }
        if (cl.flat) {
            auto r = eval_flat(*cl.flat, *cl.pool, cl.body_id, ne);
            if (r) return *r;
        }
        return std::nullopt;
    }

    // Try IR bridge
    if (closure_bridge_) {
        return closure_bridge_(cid, args);
    }

    return std::nullopt;
}

// ── ast_to_data: convert AST subtree to EvalValue data ───────
EvalValue Evaluator::ast_to_data(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, aura::ast::NodeId nid) {
    if (nid == ast::NULL_NODE) return make_void();
    auto v = flat.get(nid);
    // Local helper: build (cons "fn-name" args)
    auto cd = [&](const std::string& fn, const EvalValue& args) -> EvalValue {
        auto fi = string_heap_.size(); string_heap_.push_back(fn);
        auto pi = pairs_.size(); pairs_.push_back({types::make_string(fi), args});
        return types::make_pair(pi);
    };

    switch (v.tag) {
    case ast::NodeTag::LiteralInt:
        return make_int(v.int_value);
    case ast::NodeTag::LiteralFloat:
        return make_float(v.float_value);
    case ast::NodeTag::LiteralString: {
        auto name = std::string(pool.resolve(v.sym_id));
        auto idx = string_heap_.size();
        string_heap_.push_back(std::move(name));
        return make_string(idx);
    }
    case ast::NodeTag::Variable: {
        auto name = std::string(pool.resolve(v.sym_id));
        auto idx = string_heap_.size();
        string_heap_.push_back(std::move(name));
        return make_string(idx);
    }
    case ast::NodeTag::Call: {
        EvalValue tail = make_void();
        for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
            auto item = ast_to_data(flat, pool, *it);
            auto pair_idx = pairs_.size();
            pairs_.push_back(Pair{std::move(item), tail});
            tail = make_pair(pair_idx);
        }
        return tail;
    }
    case ast::NodeTag::Begin: {
        EvalValue tail = make_void();
        for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
            auto item = ast_to_data(flat, pool, *it);
            auto pair_idx = pairs_.size();
            pairs_.push_back(Pair{std::move(item), tail});
            tail = make_pair(pair_idx);
        }
        return cd("begin", tail);
    }
    case ast::NodeTag::IfExpr: {
        auto cond = v.children.size() > 0 ? ast_to_data(flat, pool, v.child(0)) : make_void();
        auto then_b = v.children.size() > 1 ? ast_to_data(flat, pool, v.child(1)) : make_void();
        auto else_b = v.children.size() > 2 ? ast_to_data(flat, pool, v.child(2)) : make_void();
        auto tail = make_pair(pairs_.size()); pairs_.push_back({then_b, else_b});
        tail = make_pair(pairs_.size()); pairs_.push_back({cond, tail});
        return cd("if", tail);
    }
    case ast::NodeTag::Lambda: {
        EvalValue params_tail = make_void();
        for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
            auto pname = std::string(pool.resolve(*it));
            auto pidx = string_heap_.size(); string_heap_.push_back(pname);
            auto pair_idx = pairs_.size(); pairs_.push_back({make_string(pidx), params_tail});
            params_tail = make_pair(pair_idx);
        }
        auto body = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
        auto tail = make_pair(pairs_.size()); pairs_.push_back({params_tail, body});
        return cd("lambda", tail);
    }
    case ast::NodeTag::Define: {
        auto name_str = std::string(pool.resolve(v.sym_id));
        auto nidx = string_heap_.size(); string_heap_.push_back(name_str);
        auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
        auto tail = make_pair(pairs_.size()); pairs_.push_back({make_string(nidx), val});
        return cd("define", tail);
    }
    case ast::NodeTag::DefineType: {
        auto type_name = pool.resolve(v.sym_id);
        auto tnidx = string_heap_.size(); string_heap_.push_back(std::string(type_name));
        EvalValue params_tail = make_void();
        for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
            auto pname = std::string(pool.resolve(*it));
            auto pidx = string_heap_.size(); string_heap_.push_back(pname);
            auto pp = pairs_.size(); pairs_.push_back({make_string(pidx), params_tail});
            params_tail = make_pair(pp);
        }
        auto type_spec = make_pair(pairs_.size()); pairs_.push_back({make_string(tnidx), params_tail});
        EvalValue ctors_tail = make_void();
        for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
            auto ctor_data = ast_to_data(flat, pool, *it);
            auto pp = pairs_.size(); pairs_.push_back({ctor_data, ctors_tail});
            ctors_tail = make_pair(pp);
        }
        auto tail = make_pair(pairs_.size()); pairs_.push_back({type_spec, ctors_tail});
        return cd("define-type", tail);
    }
    case ast::NodeTag::Set: {
        auto name_str = std::string(pool.resolve(v.sym_id));
        auto nidx = string_heap_.size(); string_heap_.push_back(name_str);
        auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
        auto tail = make_pair(pairs_.size()); pairs_.push_back({make_string(nidx), val});
        return cd("set!", tail);
    }
    case ast::NodeTag::Let:
    case ast::NodeTag::LetRec: {
        auto body_start = v.child(0);
        EvalValue bindings_tail = make_void();
        for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
            if (*it == body_start) continue;
            auto cv = flat.get(*it);
            auto bname = std::string(pool.resolve(cv.sym_id));
            auto bni = string_heap_.size(); string_heap_.push_back(bname);
            auto bv = ast_to_data(flat, pool, *it);
            auto bp = pairs_.size(); pairs_.push_back({make_string(bni), bv});
            bindings_tail = make_pair(bp);
        }
        auto body = ast_to_data(flat, pool, body_start);
        auto full_bindings = make_pair(pairs_.size()); pairs_.push_back({bindings_tail, body});
        auto kind = v.tag == ast::NodeTag::LetRec ? "letrec" : "let";
        return cd(kind, full_bindings);
    }
    case ast::NodeTag::Quote: {
        if (!v.children.empty()) {
            auto quoted = ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size()); pairs_.push_back({quoted, make_void()});
            return cd("quote", tail);
        }
        return make_void();
    }
    case ast::NodeTag::Coercion: {
        if (!v.children.empty()) {
            auto expr = ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size()); pairs_.push_back({expr, make_void()});
            return cd("cast", tail);
        }
        return make_void();
    }
    case ast::NodeTag::Pair: {
        auto car = ast_to_data(flat, pool, v.child(0));
        auto cdr_val = ast_to_data(flat, pool, v.child(1));
        auto tail = make_pair(pairs_.size()); pairs_.push_back({cdr_val, make_void()});
        tail = make_pair(pairs_.size()); pairs_.push_back({car, tail});
        return cd("cons", tail);
    }
    default:
        return make_void();
    }
}
// Inverse of ast_to_data. Needed so lambda bodies from macro data
// can be converted to AST for closure creation.
ast::NodeId Evaluator::data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat, aura::ast::StringPool& pool, int depth) {
    using namespace types;
    if (depth > 256) return ast::NULL_NODE;
    if (is_int(data)) {
        return flat.add_literal(as_int(data));
    }
    if (is_float(data)) {
        return flat.add_literal_float(as_float(data));
    }
    if (is_bool(data)) {
        auto id = flat.add_literal(as_bool(data) ? 1 : 0);
        flat.set_marker(id, ast::SyntaxMarker::BoolLiteral);
        return id;
    }
    if (is_void(data)) {
        return flat.add_literal(0);  // () sentinel
    }
    if (is_string(data)) {
        auto idx = as_string_idx(data);
        if (idx < string_heap_.size()) {
            // Strings in code context are variable references
            auto name = string_heap_[idx];
            auto sid = pool.intern(name);
            return flat.add_variable(sid);
        }
        return ast::NULL_NODE;
    }
    if (is_pair(data)) {
        auto pair_idx = as_pair_idx(data);
        if (pair_idx >= pairs_.size()) return ast::NULL_NODE;

        auto car_data = pairs_[pair_idx].car;
        auto cdr_data = pairs_[pair_idx].cdr;

        if (is_string(car_data)) {
            auto fn_idx = as_string_idx(car_data);
            auto fn_name = fn_idx < string_heap_.size() ? string_heap_[fn_idx] : "";

            // Quote: (quote expr)
            if (fn_name == "quote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto quoted = pairs_[qp].car;
                auto quoted_node = data_to_flat(quoted, flat, pool, depth + 1);
                if (quoted_node != ast::NULL_NODE)
                    return flat.add_quote(quoted_node);
                return flat.add_literal(0);  // fallback
            }

            // Quasiquote: (quasiquote expr)
            if (fn_name == "quasiquote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto quoted = pairs_[qp].car;
                return data_to_flat(quoted, flat, pool, depth + 1);
            }

            // Unquote: (unquote expr) — just convert the inner expression
            if (fn_name == "unquote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto inner = pairs_[qp].car;
                return data_to_flat(inner, flat, pool, depth + 1);
            }

            // Define-type: (define-type (type-name params...) (ctor1 ctor2 ...))
            if (fn_name == "define-type") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto type_name_data = pairs_[np].car;
                    auto ctor_rest = pairs_[np].cdr;

                    aura::ast::SymId type_name_val = 0;
                    std::vector<aura::ast::SymId> params;
                    std::vector<ast::NodeId> ctors;

                    if (is_pair(type_name_data)) {
                        auto tnp = as_pair_idx(type_name_data);
                        if (is_string(pairs_[tnp].car)) {
                            auto ti = as_string_idx(pairs_[tnp].car);
                            auto ts = ti < string_heap_.size() ? string_heap_[ti] : "";
                            type_name_val = pool.intern(ts);
                            auto rest = pairs_[tnp].cdr;
                            while (is_pair(rest)) {
                                auto rp = as_pair_idx(rest);
                                if (is_string(pairs_[rp].car)) {
                                    auto pi = as_string_idx(pairs_[rp].car);
                                    auto ps = pi < string_heap_.size() ? string_heap_[pi] : "";
                                    params.push_back(pool.intern(ps));
                                }
                                rest = pairs_[rp].cdr;
                            }
                        }
                    } else if (is_string(type_name_data)) {
                        auto ti = as_string_idx(type_name_data);
                        auto ts = ti < string_heap_.size() ? string_heap_[ti] : "";
                        type_name_val = pool.intern(ts);
                    }

                    if (type_name_val != 0) {
                        auto cur = ctor_rest;
                        while (is_pair(cur)) {
                            auto cp = as_pair_idx(cur);
                            auto ctor_form = pairs_[cp].car;
                            cur = pairs_[cp].cdr;

                            if (is_pair(ctor_form)) {
                                auto ctor_pair = as_pair_idx(ctor_form);
                                auto ctor_car = pairs_[ctor_pair].car;
                                // Handle (quote Name) format
                                if (is_string(ctor_car)) {
                                    auto ci = as_string_idx(ctor_car);
                                    auto cs = ci < string_heap_.size() ? string_heap_[ci] : "";
                                    if (cs == "quote" && is_pair(pairs_[ctor_pair].cdr)) {
                                        // (quote Some) — extract "Some" from cdr
                                        auto qcdr = as_pair_idx(pairs_[ctor_pair].cdr);
                                        auto name_val = pairs_[qcdr].car;
                                        if (is_string(name_val)) {
                                            auto ni = as_string_idx(name_val);
                                            auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                                            auto ctor_var = flat.add_variable(pool.intern(ns));
                                            ctors.push_back(flat.add_quote(ctor_var));
                                        }
                                    }
                                } else {
                                    // Direct ctor descriptor — store as-is
                                    auto ctor_node = data_to_flat(ctor_form, flat, pool, depth + 1);
                                    if (ctor_node != ast::NULL_NODE)
                                        ctors.push_back(ctor_node);
                                }
                            }
                        }
                        return flat.add_define_type(type_name_val, params, ctors);
                    }
                }
                return ast::NULL_NODE;
            }

            // Begin: (begin ...)
            if (fn_name == "begin") {
                std::vector<ast::NodeId> exprs;
                auto cur = cdr_data;
                while (is_pair(cur)) {
                    auto cp = as_pair_idx(cur);
                    auto e = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
                    if (e != ast::NULL_NODE) exprs.push_back(e);
                    cur = pairs_[cp].cdr;
                }
                return flat.add_begin(exprs);
            }

            // If: (if cond then else)
            if (fn_name == "if") {
                ast::NodeId cond_node = ast::NULL_NODE, then_node = ast::NULL_NODE, else_node = ast::NULL_NODE;
                if (is_pair(cdr_data)) {
                    auto cp = as_pair_idx(cdr_data);
                    cond_node = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
                    auto rest = pairs_[cp].cdr;
                    if (is_pair(rest)) {
                        auto tp = as_pair_idx(rest);
                        then_node = data_to_flat(pairs_[tp].car, flat, pool, depth + 1);
                        auto erest = pairs_[tp].cdr;
                        if (is_pair(erest)) {
                            auto ep = as_pair_idx(erest);
                            else_node = data_to_flat(pairs_[ep].car, flat, pool, depth + 1);
                        }
                    }
                }
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE && else_node != ast::NULL_NODE)
                    return flat.add_if(cond_node, then_node, else_node);
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE)
                    return flat.add_if(cond_node, then_node, flat.add_literal(0));
                return cond_node != ast::NULL_NODE ? cond_node : flat.add_literal(0);
            }

            // Lambda: (lambda (args) body)
            if (fn_name == "lambda") {
                if (is_pair(cdr_data)) {
                    auto params_pair = as_pair_idx(cdr_data);
                    auto params_data = pairs_[params_pair].car;
                    auto body_rest = pairs_[params_pair].cdr;

                    std::vector<ast::SymId> params;
                    auto args_data = params_data;
                    while (is_pair(args_data)) {
                        auto ap = as_pair_idx(args_data);
                        auto arg = pairs_[ap].car;
                        if (is_string(arg)) {
                            auto aidx = as_string_idx(arg);
                            auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                            params.push_back(pool.intern(astr));
                        }
                        args_data = pairs_[ap].cdr;
                    }

                    ast::NodeId body_node = ast::NULL_NODE;
                    if (is_pair(body_rest)) {
                        auto bp = as_pair_idx(body_rest);
                        body_node = data_to_flat(pairs_[bp].car, flat, pool, depth + 1);
                    }
                    if (body_node == ast::NULL_NODE)
                        body_node = flat.add_literal(0);
                    return flat.add_lambda(params, body_node);
                }
                return ast::NULL_NODE;
            }

            // Define: (define name value)
            if (fn_name == "define") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto name_data = pairs_[np].car;
                    auto val_rest = pairs_[np].cdr;

                    if (is_string(name_data) && is_pair(val_rest)) {
                        auto ni = as_string_idx(name_data);
                        auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                        auto vp = as_pair_idx(val_rest);
                        auto val_node = data_to_flat(pairs_[vp].car, flat, pool, depth + 1);
                        return flat.add_define(pool.intern(ns), val_node);
                    }
                }
                return ast::NULL_NODE;
            }

            // Set!: (set! name value)
            if (fn_name == "set!") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto name_val = pairs_[np].car;
                    auto val_rest = pairs_[np].cdr;
                    if (is_string(name_val) && is_pair(val_rest)) {
                        auto ni = as_string_idx(name_val);
                        auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                        auto vp = as_pair_idx(val_rest);
                        auto val_node = data_to_flat(pairs_[vp].car, flat, pool, depth + 1);
                        return flat.add_set(pool.intern(ns), val_node);
                    }
                }
                return ast::NULL_NODE;
            }

            // Let: (let ((x val)) body)
            if (fn_name == "let") {
                // For data_to_flat, let is just a call node
                // (let ((x val)) body) is sugar and should already be expanded
                // Just treat as a general call
            }
        }

        // General function call: build Call(node func, [args...])
        auto func_node = data_to_flat(car_data, flat, pool, depth);
        if (func_node == ast::NULL_NODE) return ast::NULL_NODE;
        std::vector<ast::NodeId> args;
        auto cur = cdr_data;
        while (is_pair(cur)) {
            auto cp = as_pair_idx(cur);
            auto a = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
            if (a != ast::NULL_NODE) args.push_back(a);
            cur = pairs_[cp].cdr;
        }
        return flat.add_call(func_node, args);
    }
    if (is_cell(data)) {
        // Dereference cell and convert the inner value
        auto cid = as_cell_id(data);
        if (cid < cells_.size())
            return data_to_flat(cells_[cid], flat, pool, depth + 1);
    }
    return ast::NULL_NODE;
}

// ── eval_data_as_code: evaluate macro-expanded data as code ──
// Macro bodies produce data (lists) via cons/quote chains.
// This function interprets that data as code and evaluates it.
// flat/pool are needed for lambda and define-shorthand handling.
EvalResult Evaluator::eval_data_as_code(const types::EvalValue& data, const Env& env,
                                          ast::FlatAST* flat, ast::StringPool* pool) {
    // Not a pair → literal value (number, string, bool, void)
    if (!types::is_pair(data)) {
        // Strings are literal symbols/data, return as-is
        return data;
    }

    // Pair: (fn arg1 arg2 ...) or (special-form arg ...)
    auto pair_idx = types::as_pair_idx(data);
    if (pair_idx >= pairs_.size()) return make_void();

    auto car_val = pairs_[pair_idx].car;
    auto cdr_val = pairs_[pair_idx].cdr;

    // Handle special forms by name
    if (types::is_string(car_val)) {
        auto fn_idx = types::as_string_idx(car_val);
        auto fn_name = fn_idx < string_heap_.size() ? string_heap_[fn_idx] : "";

        // ── if: (if cond then else) ──
        if (fn_name == "if") {
            if (types::is_pair(cdr_val)) {
                auto cond_pair = types::as_pair_idx(cdr_val);
                auto cond_val = pairs_[cond_pair].car;
                auto rest = pairs_[cond_pair].cdr;
                auto cond_result = eval_data_as_code(cond_val, env, flat, pool);
                if (!cond_result) return cond_result;
                if (types::is_pair(rest)) {
                    auto then_pair = types::as_pair_idx(rest);
                    auto then_val = pairs_[then_pair].car;
                    auto else_rest = pairs_[then_pair].cdr;
                    if (types::is_truthy(*cond_result)) {
                        auto r = eval_data_as_code(then_val, env, flat, pool);
                        return r;
                    } else {
                        // Evaluate else branch
                        if (types::is_pair(else_rest)) {
                            auto else_pair = types::as_pair_idx(else_rest);
                            auto else_val = pairs_[else_pair].car;
                            return eval_data_as_code(else_val, env, flat, pool);
                        }
                    }
                }
            }
            return make_void();
        }

        // ── when: (when cond body...) — like (if cond (begin body...) (void))
        if (fn_name == "when" || fn_name == "unless") {
            if (types::is_pair(cdr_val)) {
                auto cond_pair = types::as_pair_idx(cdr_val);
                auto cond_val = pairs_[cond_pair].car;
                auto body_rest = pairs_[cond_pair].cdr;
                auto cond_result = eval_data_as_code(cond_val, env, flat, pool);
                if (!cond_result) return cond_result;
                if (types::is_truthy(*cond_result)) {
                    // Evaluate body expressions sequentially
                    EvalResult last = make_void();
                    while (types::is_pair(body_rest)) {
                        auto bp = types::as_pair_idx(body_rest);
                        last = eval_data_as_code(pairs_[bp].car, env, flat, pool);
                        if (!last) return last;
                        body_rest = pairs_[bp].cdr;
                    }
                    return last;
                }
            }
            return make_void();
        }

        // ── lambda: (lambda (params) body) ──
        // Needs flat/pool to create an AST closure
        if (fn_name == "lambda") {
            if (!flat || !pool) {
                // Without flat/pool, we can't create closures — return as-is
                return make_void();
            }
            if (types::is_pair(cdr_val)) {
                auto params_pair = types::as_pair_idx(cdr_val);
                auto params_data = pairs_[params_pair].car;  // (arg1 arg2 ...)
                auto body_rest = pairs_[params_pair].cdr;    // (body ...)

                // Extract param names
                std::vector<ast::SymId> param_syms;
                auto args_data = params_data;
                while (types::is_pair(args_data)) {
                    auto ap = types::as_pair_idx(args_data);
                    auto arg_data = pairs_[ap].car;
                    if (types::is_string(arg_data)) {
                        auto aidx = types::as_string_idx(arg_data);
                        auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                        param_syms.push_back(pool->intern(astr));
                    }
                    args_data = pairs_[ap].cdr;
                }

                // Extract and convert body data to FlatAST
                ast::NodeId body_node = ast::NULL_NODE;
                if (types::is_pair(body_rest)) {
                    auto bp = types::as_pair_idx(body_rest);
                    auto body_data = pairs_[bp].car;
                    body_node = data_to_flat(body_data, *flat, *pool);
                }
                if (body_node == ast::NULL_NODE)
                    body_node = flat->add_literal(0);

                // Create lambda node and closure
                auto lambda_id = flat->add_lambda(param_syms, body_node);
                auto cid = next_id();
                auto* copied_env = copy_env(env);

                // Clone closure body to arena-allocated FlatAST so it survives
                // the parent context (macro flat/pool is reused across expansions).
                if (!arena_) { return make_void(); }
                auto cl_alloc = arena_->allocator();
                auto* cl_flat = arena_->create<aura::ast::FlatAST>(cl_alloc);
                auto* cl_pool = arena_->create<aura::ast::StringPool>(cl_alloc);
                auto cloned_body = clone_macro_body(*cl_flat, *cl_pool, *flat, *pool,
                                                     body_node, nullptr);
                cl_flat->root = cloned_body;
                closures_[cid] = Closure{/*params*/{}, cl_flat, cl_pool, cloned_body, copied_env};
                // Store param names as strings for the Closure
                for (auto& ps : param_syms) {
                    std::string pname(pool->resolve(ps));
                    closures_[cid].params.push_back(std::move(pname));
                }
                return make_closure(cid);
            }
            return make_void();
        }

        // ── begin: (begin expr1 expr2 ...) ──
        if (fn_name == "begin") {
            auto current = cdr_val;
            EvalResult last = make_void();
            while (types::is_pair(current)) {
                auto elem_pair = types::as_pair_idx(current);
                last = eval_data_as_code(pairs_[elem_pair].car, env, flat, pool);
                if (!last) return last;
                current = pairs_[elem_pair].cdr;
            }
            return last;
        }

        // ── quote: (quote expr) ──
        if (fn_name == "quote") {
            if (types::is_pair(cdr_val)) {
                auto quote_pair = types::as_pair_idx(cdr_val);
                return pairs_[quote_pair].car;  // Return the quoted value as-is
            }
            return make_void();
        }

        // ── define: (define name value) or (define (name args) body) ──
        if (fn_name == "define") {
            if (types::is_pair(cdr_val)) {
                auto name_pair = types::as_pair_idx(cdr_val);
                auto name_val = pairs_[name_pair].car;
                auto val_rest = pairs_[name_pair].cdr;

                // (define name value)
                if (types::is_string(name_val) && types::is_pair(val_rest)) {
                    auto val_pair = types::as_pair_idx(val_rest);
                    auto val = eval_data_as_code(pairs_[val_pair].car, env, flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str = name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto ci = alloc_cell(make_void());
                        const_cast<Env&>(env).bind(name_str, make_cell(ci));
                        cells_[ci] = *val;
                        return *val;
                    }
                    return val;
                }

                // (define (name args...) body) — function shorthand
                if (types::is_pair(name_val) && types::is_pair(val_rest) && flat && pool) {
                    auto fn_pair = types::as_pair_idx(name_val);
                    auto fn_name_data = pairs_[fn_pair].car;
                    auto fn_args_data = pairs_[fn_pair].cdr;

                    if (types::is_string(fn_name_data)) {
                        auto ni = types::as_string_idx(fn_name_data);
                        auto fn_str = ni < string_heap_.size() ? string_heap_[ni] : "";

                        // Extract param names from (arg1 arg2 ...)
                        std::vector<ast::SymId> param_syms;
                        auto args_data = fn_args_data;
                        while (types::is_pair(args_data)) {
                            auto ap = types::as_pair_idx(args_data);
                            auto arg_data = pairs_[ap].car;
                            if (types::is_string(arg_data)) {
                                auto aidx = types::as_string_idx(arg_data);
                                auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                                param_syms.push_back(pool->intern(astr));
                            }
                            args_data = pairs_[ap].cdr;
                        }

                        // Extract and convert body data
                        ast::NodeId body_node = ast::NULL_NODE;
                        if (types::is_pair(val_rest)) {
                            auto bp = types::as_pair_idx(val_rest);
                            auto body_data = pairs_[bp].car;
                            body_node = data_to_flat(body_data, *flat, *pool);
                        }
                        if (body_node == ast::NULL_NODE)
                            body_node = flat->add_literal(0);

                        // Create lambda node and closure
                        auto lambda_id = flat->add_lambda(param_syms, body_node);
                        auto cid = next_id();
                        auto* copied_env = copy_env(env);
                        Closure cl;
                        for (auto& ps : param_syms) {
                            cl.params.push_back(std::string(pool->resolve(ps)));
                        }
                        cl.flat = flat;
                        cl.pool = pool;
                        cl.body_id = body_node;
                        cl.env = copied_env;
                        closures_[cid] = std::move(cl);

                        // Bind in env
                        auto ci = alloc_cell(make_void());
                        const_cast<Env&>(env).bind(fn_str, make_cell(ci));
                        cells_[ci] = make_closure(cid);
                        return make_closure(cid);
                    }
                }
            }
            return make_void();
        }

        // ── set!: (set! name value) ──
        if (fn_name == "set!") {
            if (types::is_pair(cdr_val)) {
                auto name_pair = types::as_pair_idx(cdr_val);
                auto name_val = pairs_[name_pair].car;
                auto val_rest = pairs_[name_pair].cdr;
                if (types::is_string(name_val) && types::is_pair(val_rest)) {
                    auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_rest)].car, env, flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str = name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto* cell_ptr = const_cast<Env&>(env).lookup_cell_ptr(name_str, &cells_);
                        if (cell_ptr) { *cell_ptr = *val; return *val; }
                    }
                }
            }
            return make_void();
        }

        // ── let: (let ((x val)) body) ──
        if (fn_name == "let") {
            if (types::is_pair(cdr_val)) {
                auto bindings_val = pairs_[types::as_pair_idx(cdr_val)].car;
                auto body_rest = pairs_[types::as_pair_idx(cdr_val)].cdr;
                // Collect bindings
                std::vector<std::pair<std::string, EvalValue>> bindings;
                auto current = bindings_val;
                while (types::is_pair(current)) {
                    auto binding_pair = pairs_[types::as_pair_idx(current)].car;
                    if (types::is_pair(binding_pair)) {
                        auto name_val = pairs_[types::as_pair_idx(binding_pair)].car;
                        auto val_expr = pairs_[types::as_pair_idx(binding_pair)].cdr;
                        if (types::is_string(name_val) && types::is_pair(val_expr)) {
                            auto name_idx = types::as_string_idx(name_val);
                            auto name_str = name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                            auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_expr)].car, env, flat, pool);
                            if (!val) return val;
                            bindings.emplace_back(name_str, *val);
                        }
                    }
                    current = pairs_[types::as_pair_idx(current)].cdr;
                }
                // Create new env and bind
                Env new_env(&env);
                new_env.set_primitives(&primitives_);
                new_env.set_cells(&cells_);
                for (auto& [n, v] : bindings)
                    new_env.bind(n, v);
                // Evaluate body in new env
                auto body_current = body_rest;
                EvalResult last = make_void();
                while (types::is_pair(body_current)) {
                    auto elem_pair = types::as_pair_idx(body_current);
                    last = eval_data_as_code(pairs_[elem_pair].car, new_env, flat, pool);
                    if (!last) return last;
                    body_current = pairs_[elem_pair].cdr;
                }
                return last;
            }
            return make_void();
        }

        // ── General function call ──
        // Look up the function in the environment or primitives
        auto prim = env.lookup_primitive(fn_name);
        if (prim) {
            std::vector<EvalValue> args;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val) return arg_val;
                args.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            return (*prim)(args);
        }

        // Look up in environment
        auto env_val = env.lookup(fn_name);
        if (env_val) {
            auto fn_val = *env_val;
            // Dereference cells — needed when lookup returned cell sentinel (cells_ not set on env)
            if (types::is_cell(fn_val)) {
                auto ci = types::as_cell_id(fn_val);
                if (ci < cells_.size()) fn_val = cells_[ci];
            }
            if (types::is_closure(fn_val)) {
                auto cid = types::as_closure_id(fn_val);
                auto it = closures_.find(cid);
                if (it != closures_.end()) {
                    auto& cl = it->second;
                    // Evaluate args and apply
                    std::vector<EvalValue> cargs;
                    auto current = cdr_val;
                    while (types::is_pair(current)) {
                        auto arg_pair = types::as_pair_idx(current);
                        auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                        if (!arg_val) return arg_val;
                        cargs.push_back(*arg_val);
                        current = pairs_[arg_pair].cdr;
                    }
                    // Create tail env and apply
                    Env tail_env(cl.env ? *cl.env : top_);
                    tail_env.set_primitives(&primitives_);
                    tail_env.set_cells(&cells_);
                    for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                        tail_env.bind(cl.params[i], std::move(cargs[i]));
                    if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                        return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id, tail_env);
                    return make_void();
                }
            }
        }
    }

    // Not a string function name — evaluate car and cdr, apply
    auto fn = eval_data_as_code(car_val, env, flat, pool);
    if (!fn) return fn;
    if (types::is_closure(*fn)) {
        auto cid = types::as_closure_id(*fn);
        // Try tree-walker closure first, then IR bridge
        auto result = apply_closure(cid, {});
        if (result) return *result;

        // Fallback: manual closure apply via eval_flat
        auto it = closures_.find(cid);
        if (it != closures_.end()) {
            auto& cl = it->second;
            std::vector<EvalValue> cargs;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val) return arg_val;
                cargs.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            Env tail_env(cl.env ? *cl.env : top_);
            tail_env.set_primitives(&primitives_);
            tail_env.set_cells(&cells_);
            for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                tail_env.bind(cl.params[i], std::move(cargs[i]));
            if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id, tail_env);
        }
    }

\
// ── Runtime type helpers for type annotation checking ────────\
static aura::core::TypeTag runtime_type_tag(const EvalValue\& v) {\
    if (types::is_int(v))     return aura::core::TypeTag::INT;\
    if (types::is_float(v))   return aura::core::TypeTag::FLOAT;\
    if (types::is_bool(v))    return aura::core::TypeTag::BOOL;\
    if (types::is_string(v))  return aura::core::TypeTag::STRING;\
    if (types::is_pair(v))    return aura::core::TypeTag::PAIR;\
    if (types::is_closure(v)) return aura::core::TypeTag::CLOSURE;\
    if (types::is_vector(v))  return aura::core::TypeTag::VECTOR;\
    if (types::is_hash(v))    return aura::core::TypeTag::HASH;\
    return aura::core::TypeTag::DYNAMIC;\
}\
\
static std::string type_tag_name(aura::core::TypeTag tag) {\
    switch (tag) {\
        case aura::core::TypeTag::INT:     return "Int";\
        case aura::core::TypeTag::FLOAT:   return "Float";\
        case aura::core::TypeTag::BOOL:    return "Bool";\
        case aura::core::TypeTag::STRING:  return "String";\
        case aura::core::TypeTag::PAIR:    return "Pair";\
        case aura::core::TypeTag::CLOSURE: return "Closure";\
        case aura::core::TypeTag::VECTOR:  return "Vector";\
        case aura::core::TypeTag::HASH:    return "Hash";\
        default: return "Dynamic";\
    }\
}\
\
static bool coerce_value(EvalValue\& val, aura::core::TypeTag from, aura::core::TypeTag to, std::vector<std::string>\& heap) {\
    if (from == to) return true;\
    if (from == aura::core::TypeTag::INT \&\& to == aura::core::TypeTag::FLOAT) {\
        val = types::make_float(static_cast<double>(types::as_int(val))); return true;\
    }\
    if (from == aura::core::TypeTag::FLOAT \&\& to == aura::core::TypeTag::INT) {\
        val = types::make_int(static_cast<std::int64_t>(types::as_float(val))); return true;\
    }\
    if (from == aura::core::TypeTag::INT \&\& to == aura::core::TypeTag::STRING) {\
        auto s = std::to_string(types::as_int(val));\
        auto id = heap.size(); heap.push_back(std::move(s));\
        val = types::make_string(id); return true;\
    }\
    if (from == aura::core::TypeTag::STRING \&\& to == aura::core::TypeTag::INT) {\
        auto idx = types::as_string_idx(val);\
        if (idx < heap.size()) {\
            try { val = types::make_int(static_cast<std::int64_t>(std::stoll(heap[idx]))); return true; }\
            catch (...) {}\
        }\
    }\
    if (from == aura::core::TypeTag::INT \&\& to == aura::core::TypeTag::BOOL) {\
        val = types::make_bool(types::as_int(val) != 0); return true;\
    }\
    if (from == aura::core::TypeTag::BOOL \&\& to == aura::core::TypeTag::INT) {\
        val = types::make_int(types::as_bool(val) ? 1 : 0); return true;\
    }\
    if (from == aura::core::TypeTag::FLOAT \&\& to == aura::core::TypeTag::STRING) {\
        auto s = std::to_string(types::as_float(val));\
        auto id = heap.size(); heap.push_back(std::move(s));\
        val = types::make_string(id); return true;\
    }\
    if (from == aura::core::TypeTag::STRING \&\& to == aura::core::TypeTag::FLOAT) {\
        auto idx = types::as_string_idx(val);\
        if (idx < heap.size()) {\
            try { val = types::make_float(std::stod(heap[idx])); return true; }\
            catch (...) {}\
        }\
    }\
    return false;  // non-coercible\
}
    return make_void();
}
// ── Runtime type helpers for type annotation checking ────────
static aura::core::TypeTag runtime_type_tag(const types::EvalValue& v) {
    if (types::is_int(v))     return aura::core::TypeTag::INT;
    if (types::is_float(v))   return aura::core::TypeTag::FLOAT;
    if (types::is_bool(v))    return aura::core::TypeTag::BOOL;
    if (types::is_string(v))  return aura::core::TypeTag::STRING;
    if (types::is_pair(v))    return aura::core::TypeTag::PAIR;
    if (types::is_closure(v)) return aura::core::TypeTag::CLOSURE;
    if (types::is_vector(v))  return aura::core::TypeTag::VECTOR;
    if (types::is_hash(v))    return aura::core::TypeTag::HASH;
    return aura::core::TypeTag::DYNAMIC;
}

static std::string type_tag_name(aura::core::TypeTag tag) {
    switch (tag) {
        case aura::core::TypeTag::INT:     return "Int";
        case aura::core::TypeTag::FLOAT:   return "Float";
        case aura::core::TypeTag::BOOL:    return "Bool";
        case aura::core::TypeTag::STRING:  return "String";
        case aura::core::TypeTag::PAIR:    return "Pair";
        case aura::core::TypeTag::CLOSURE: return "Closure";
        case aura::core::TypeTag::VECTOR:  return "Vector";
        case aura::core::TypeTag::HASH:    return "Hash";
        default: return "Dynamic";
    }
}

static bool coerce_value(types::EvalValue& val, aura::core::TypeTag from, aura::core::TypeTag to, std::vector<std::string>& heap) {
    if (from == to) return true;
    if (from == aura::core::TypeTag::INT && to == aura::core::TypeTag::FLOAT) {
        val = types::make_float(static_cast<double>(types::as_int(val))); return true;
    }
    if (from == aura::core::TypeTag::FLOAT && to == aura::core::TypeTag::INT) {
        val = types::make_int(static_cast<std::int64_t>(types::as_float(val))); return true;
    }
    if (from == aura::core::TypeTag::INT && to == aura::core::TypeTag::STRING) {
        auto s = std::to_string(types::as_int(val));
        auto id = static_cast<std::uint64_t>(heap.size()); heap.push_back(std::move(s));
        val = types::make_string(id); return true;
    }
    if (from == aura::core::TypeTag::STRING && to == aura::core::TypeTag::INT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try { val = types::make_int(static_cast<std::int64_t>(std::stoll(heap[static_cast<std::size_t>(idx)]))); return true; }
            catch (...) {}
        }
    }
    if (from == aura::core::TypeTag::INT && to == aura::core::TypeTag::BOOL) {
        val = types::make_bool(types::as_int(val) != 0); return true;
    }
    if (from == aura::core::TypeTag::BOOL && to == aura::core::TypeTag::INT) {
        val = types::make_int(types::as_bool(val) ? 1 : 0); return true;
    }
    if (from == aura::core::TypeTag::FLOAT && to == aura::core::TypeTag::STRING) {
        auto s = std::to_string(types::as_float(val));
        auto id = static_cast<std::uint64_t>(heap.size()); heap.push_back(std::move(s));
        val = types::make_string(id); return true;
    }
    if (from == aura::core::TypeTag::STRING && to == aura::core::TypeTag::FLOAT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try { val = types::make_float(std::stod(heap[static_cast<std::size_t>(idx)])); return true; }
            catch (...) {}
        }
    }
    return false;  // non-coercible
}

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────
EvalResult Evaluator::eval_flat(aura::ast::FlatAST& flat,
                                 aura::ast::StringPool& pool,
                                 aura::ast::NodeId id,
                                 const Env& env) {
    // Catch bad_variant_access and return friendly error instead of crash.
    // This happens when user code passes wrong argument types to primitives.
    try {
    // TCO loop state: f/p point to the current FlatAST/Pool,
    // which may change during closure/macro tail calls.
    aura::ast::FlatAST* f = &flat;
    aura::ast::StringPool* p = &pool;
    const Env* current_env = &env;
    aura::ast::NodeId current_id = id;
    std::optional<Env> tail_env;

    // Recursion depth guard: friendly error vs segfault
    // MAX_C_STACK_DEPTH must be low enough to fit in the C++ call stack (~550 frames)
    static constexpr std::size_t MAX_C_STACK_DEPTH = 400;
    struct DepthGuard { std::size_t& d; ~DepthGuard() { --d; } } _dg{eval_depth_};
    if (++eval_depth_ > MAX_C_STACK_DEPTH)
        return std::unexpected(Diagnostic{
            ErrorKind::InternalError,
            std::format("recursion depth exceeded (>{})", MAX_C_STACK_DEPTH)});

    while (true) {
        current_flat_ = f;
        current_pool_ = p;
        // Save the eval environment before any tail_env.emplace could corrupt current_env
        const Env& eval_env = *current_env;
        if (current_id >= f->size() || current_id == aura::ast::NULL_NODE)
            return std::unexpected(Diagnostic{ErrorKind::InternalError, "invalid node id"});
        auto v = f->get(current_id);
        switch (v.tag) {
        case aura::ast::NodeTag::LiteralInt:
            // #t/#f have BoolLiteral marker — convert to Bool at runtime
            if (v.marker == aura::ast::SyntaxMarker::BoolLiteral)
                return make_bool(v.int_value != 0);
            return make_int(v.int_value);
        case aura::ast::NodeTag::LiteralFloat:
            return make_float(v.float_value);
        case aura::ast::NodeTag::LiteralString: {
            auto sid = string_heap_.size();
            string_heap_.push_back(std::string(p->resolve(v.sym_id)));
            return make_string(sid);
        }
        case aura::ast::NodeTag::Variable: {
            auto name = p->resolve(v.sym_id);
            auto val = eval_env.lookup(std::string(name));
            if (val) return *val;
            std::string var_name(name);
            if (var_name.empty()) {
                var_name = std::format("<sym:{}>", v.sym_id);
            }
            std::vector<std::string> candidates;
            {
                const Env* e = &eval_env;
                while (e) {
                    for (auto& b : const_cast<Env&>(*e).bindings())
                        candidates.push_back(b.first);
                    e = e->parent();
                }
            }
            auto best = closest_match(var_name, candidates);
            Diagnostic d(ErrorKind::UnboundVariable, std::move(var_name));
            if (!best.empty())
                d.with_suggestion("did you mean '" + best + "'?");
            return std::unexpected(std::move(d));
        }
        case aura::ast::NodeTag::Call: {
            if (v.children.empty()) return EvalResult(make_void());
            auto callee_id = v.child(0);
            auto callee = f->get(callee_id);
            // Inline lambda (arg evals are recursive; body is tail)
            if (callee.tag == aura::ast::NodeTag::Lambda) {
                auto pspan = callee.params;
                bool dotted = callee.int_value != 0;
                std::size_t named_count = dotted && !pspan.empty() ? pspan.size() - 1 : pspan.size();
                // Evaluate named args
                std::vector<EvalValue> iargs;
                iargs.reserve(named_count);
                for (std::size_t i = 0; i < named_count && i+1 < v.children.size(); ++i) {
                    auto ar = eval_flat(*f, *p, v.child(i+1), eval_env);
                    if (!ar) return ar;
                    iargs.push_back(*ar);
                }
                tail_env.emplace(&eval_env);
                tail_env->set_primitives(&primitives_);
                tail_env->set_cells(&cells_);
                for (std::size_t i = 0; i < iargs.size(); ++i) {
                    tail_env->bind(std::string(p->resolve(pspan[i])), std::move(iargs[i]));
                }
                // Dotted rest: collect remaining args into a pair list
                if (dotted && !pspan.empty()) {
                    types::EvalValue rest = make_void();
                    for (std::size_t i = v.children.size() - 1; i > named_count; --i) {
                        auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                        if (!ar) return ar;
                        auto pid = pairs_.size();
                        pairs_.push_back({*ar, rest});
                        rest = make_pair(pid);
                    }
                    tail_env->bind(std::string(p->resolve(pspan.back())), rest);
                }
                auto body_id = callee.children.empty() ? aura::ast::NULL_NODE : callee.child(0);
                if (body_id != aura::ast::NULL_NODE)
                    return eval_flat(*f, *p, body_id, *tail_env);
                return make_void();
            }
            // Macro expansion: evaluate args, bind in env, evaluate body (produces template data),
            // then re-evaluate the data as code
            if (callee.tag == aura::ast::NodeTag::Variable) {
                auto cname = std::string(p->resolve(callee.sym_id));
                auto macro_it = macros_.find(cname);
                if (macro_it != macros_.end()) {
                    auto& md = macro_it->second;
                    // Convert AST args to data (NOT evaluate — macros receive syntax)
                    bool is_rest = md.dotted;
                    
                    // Bind regular params first (all but the last)
                    std::size_t regular_count = is_rest ? md.params.size() - 1 : md.params.size();
                    tail_env.emplace(&eval_env);
                    tail_env->set_primitives(&primitives_);
                    tail_env->set_cells(&cells_);
                    
                    for (std::size_t i = 0; i < regular_count && i+1 < v.children.size(); ++i) {
                        tail_env->bind(md.params[i], ast_to_data(*f, *p, v.child(i+1)));
                    }
                    
                    // Rest param: collect remaining args as a list
                    if (is_rest) {
                        auto& rest_name = md.params.back();
                        EvalValue rest_list = make_void();
                        for (std::size_t i = v.children.size() - 1; i >= regular_count + 1; --i) {
                            auto item = ast_to_data(*f, *p, v.child(i));
                            auto pid = pairs_.size();
                            pairs_.push_back(Pair{std::move(item), rest_list});
                            rest_list = make_pair(pid);
                        }
                        tail_env->bind(rest_name, std::move(rest_list));
                    }
                    // Evaluate macro body (quasiquote-expanded template) → produces data
                    auto template_result = eval_flat(*md.flat, md.pool ? *md.pool : *p, md.body_id, *tail_env);
                    if (!template_result) return template_result;
                    // Re-evaluate the template data as code
                    return eval_data_as_code(*template_result, eval_env, f, p);
                }
            }
            // Built-in require: (require mod-name) — symbol, not string
            // Phase 4: prefix by default. (require std/list) → (import "std/list" "list:")
            //          (require std/list all:) → (import "std/list")  (backward compat)
            if (callee.tag == aura::ast::NodeTag::Variable) {
                auto cname = std::string(p->resolve(callee.sym_id));
                if (cname == "require" && v.children.size() > 1) {
                    auto req_arg = v.child(1);
                    auto rv = f->get(req_arg);
                    std::string mod_path;
                    if (rv.tag == aura::ast::NodeTag::LiteralString) {
                        mod_path = std::string(p->resolve(rv.sym_id));
                    } else if (rv.tag == aura::ast::NodeTag::Variable) {
                        mod_path = std::string(p->resolve(rv.sym_id));
                    } else {
                        return std::unexpected(Diagnostic{ErrorKind::ParseError,
                            "require: expected a module name (symbol or string)"});
                    }

                    // Check for backward-compat flag: (require std/list all:)
                    bool use_prefix = true;
                    if (v.children.size() > 2) {
                        auto compat_arg = v.child(2);
                        auto cv = f->get(compat_arg);
                        if (cv.tag == aura::ast::NodeTag::Variable) {
                            auto compat_name = std::string(p->resolve(cv.sym_id));
                            if (compat_name == "all:") use_prefix = false;
                        }
                    }

                    // Derive prefix from module name (last path component)
                    std::string prefix;
                    if (use_prefix) {
                        auto slash = mod_path.rfind('/');
                        auto base = (slash == std::string::npos) ? mod_path : mod_path.substr(slash + 1);
                        prefix = base + ":";
                    }

                    // Build (import "path" "prefix:") or (import "path")
                    std::string import_expr;
                    if (prefix.empty()) {
                        import_expr = std::string("(import \"") + mod_path + "\")";
                    } else {
                        import_expr = std::string("(import \"") + mod_path + "\" \"" + prefix + "\")";
                    }

                    if (!arena_) return make_void();
                    auto alloc = arena_->allocator();
                    auto* ipool = arena_->create<aura::ast::StringPool>(alloc);
                    auto* iflat = arena_->create<aura::ast::FlatAST>(alloc);
                    auto pr = aura::parser::parse_to_flat(import_expr, *iflat, *ipool);
                    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                        return std::unexpected(Diagnostic{ErrorKind::ParseError,
                            "require: internal error"});
                    }
                    iflat->root = pr.root;
                    // Pre-expand macros so import primitive is recognized
                    auto expanded_root = aura::compiler::macro_expand_all(*iflat, *ipool, iflat->root);
                    return eval_flat(*iflat, *ipool, expanded_root, eval_env);
                }
            }
            // try/catch: (try body (catch (var) handler))
            // body is evaluated; if it returns an error, handler is evaluated with var bound
            if (callee.tag == aura::ast::NodeTag::Variable) {
                auto cname = std::string(p->resolve(callee.sym_id));
                // when: (when cond body...) — evaluate body only if cond is truthy
                if (cname == "when" && v.children.size() >= 2) {
                    auto cond_id = v.child(1);
                    auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                    if (!cond_result) return cond_result;
                    if (is_truthy(*cond_result)) {
                        // Evaluate all remaining children as body
                        EvalResult last = make_void();
                        for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                            last = eval_flat(*f, *p, v.child(ci), eval_env);
                            if (!last) return last;
                        }
                        return last;
                    }
                    return make_void();
                }
                // unless: (unless cond body...) — evaluate body only if cond is falsy
                if (cname == "unless" && v.children.size() >= 2) {
                    auto cond_id = v.child(1);
                    auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                    if (!cond_result) return cond_result;
                    if (!is_truthy(*cond_result)) {
                        EvalResult last = make_void();
                        for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                            last = eval_flat(*f, *p, v.child(ci), eval_env);
                            if (!last) return last;
                        }
                        return last;
                    }
                    return make_void();
                }
                if (cname == "try" && v.children.size() >= 2) {
                    auto body_id = v.child(1);
                    auto result = eval_flat(*f, *p, body_id, eval_env);
                    if (result && !is_error(*result)) {
                        // Body succeeded — return result as-is
                        return result;
                    }
                    // Body errored — find catch clause (child[2] or later)
                    if (v.children.size() < 3) return make_void();
                    for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                        auto catch_id = v.child(ci);
                        auto cv = f->get(catch_id);
                        if (cv.tag == aura::ast::NodeTag::Call) {
                            auto catch_fn = f->get(cv.child(0));
                            if (catch_fn.tag == aura::ast::NodeTag::Variable
                                && std::string(p->resolve(catch_fn.sym_id)) == "catch") {
                                // (catch (var) handler) — child[0]=catch, child[1]=(var), child[2]=handler
                                if (cv.children.size() < 3) continue;
                                auto var_form = f->get(cv.child(1));
                                // var_form is (var) — a Call where child[0]=Variable "var"
                                std::string var_name;
                                if (var_form.tag == aura::ast::NodeTag::Call && var_form.children.size() >= 1) {
                                    auto var_node = f->get(var_form.child(0));
                                    if (var_node.tag == aura::ast::NodeTag::Variable)
                                        var_name = std::string(p->resolve(var_node.sym_id));
                                }
                                auto handler_id = cv.child(2);
                                // Bind error value to var and evaluate handler
                                Env catch_env(&eval_env);
                                catch_env.set_primitives(&primitives_);
                                catch_env.set_cells(const_cast<std::vector<EvalValue>*>(&cells_));
                                if (!var_name.empty() && result) {
                                    catch_env.bind(var_name, *result);
                                }
                                return eval_flat(*f, *p, handler_id, catch_env);
                            }
                        }
                    }
                    // No matching catch — propagate error
                    return result;
                }
            }

            // Primitive call (all arg evals are recursive)
            if (callee.tag == aura::ast::NodeTag::Variable) {
                auto cname = std::string(p->resolve(callee.sym_id));
                auto prim = eval_env.lookup_primitive(cname);
                if (prim) {
                    std::vector<EvalValue> args;
                    for (std::size_t i = 1; i < v.children.size(); ++i) {
                        auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                        if (!ar) return ar;
                        // Propagate error values through normal eval
                        if (is_error(*ar)) return ar;
                        args.push_back(*ar);
                    }
                    return (*prim)(args);
                }
            }
            // Closure call (eval func + arg evals are recursive; body is tail)
            auto fn = eval_flat(*f, *p, callee_id, eval_env);
            if (!fn) return fn;
            if (is_closure(*fn)) {
                auto cid = as_closure_id(*fn);
                // Check for foreign function (high bit set)
                if (cid & (1ULL << 63)) {
                    // Dispatch FFI through apply_closure
                    std::size_t named_count = 0;
                    std::vector<EvalValue> cargs;
                    for (std::size_t i = 0; i+1 < v.children.size(); ++i) {
                        auto ar = eval_flat(*f, *p, v.child(i+1), eval_env);
                        if (!ar) return ar;
                        cargs.push_back(*ar);
                    }
                    auto result = apply_closure(cid, cargs);
                    if (result) return *result;
                    return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "eval_flat: foreign call failed"});
                }
                auto it = closures_.find(cid);
                if (it == closures_.end())
                    return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "eval_flat: invalid closure"});
                auto& cl = it->second;
                // Evaluate named args first
                std::size_t named_count = cl.dotted && !cl.params.empty() ? cl.params.size() - 1 : cl.params.size();
                std::vector<EvalValue> cargs;
                cargs.reserve(named_count);
                for (std::size_t i = 0; i < named_count && i+1 < v.children.size(); ++i) {
                    auto ar = eval_flat(*f, *p, v.child(i+1), eval_env);
                    if (!ar) return ar;
                    if (is_error(*ar)) return ar;
                    cargs.push_back(*ar);
                }
                tail_env.emplace(cl.env ? *cl.env : top_);
                tail_env->set_primitives(&primitives_);
                tail_env->set_cells(&cells_);
                for (std::size_t i = 0; i < cargs.size(); ++i) {
                    tail_env->bind(cl.params[i], std::move(cargs[i]));
                }
                // Dotted rest: collect remaining args into a pair list
                if (cl.dotted && !cl.params.empty()) {
                    types::EvalValue rest = make_void();
                    for (std::size_t i = v.children.size() - 1; i > named_count; --i) {
                        auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                        if (!ar) return ar;
                        if (is_error(*ar)) return ar;
                        auto pid = pairs_.size();
                        pairs_.push_back({*ar, rest});
                        rest = make_pair(pid);
                    }
                    tail_env->bind(cl.params.back(), rest);
                }
                if (cl.body_id != aura::ast::NULL_NODE)
                    return eval_flat(*cl.flat, cl.pool ? *cl.pool : *p, cl.body_id, *tail_env);
                return make_void();
            }
            // Primitive value call: callee is a PrimitiveRef (passed as value, not a Variable node)
            if (is_primitive(*fn)) {
                auto slot = as_primitive_slot(*fn);
                if (slot < primitives_.slot_count()) {
                    auto prim = eval_env.lookup_primitive(primitives_.name_for_slot(slot));
                    if (prim) {
                        std::vector<EvalValue> args;
                        for (std::size_t i = 1; i < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                            if (!ar) return ar;
                            args.push_back(*ar);
                        }
                        return (*prim)(args);
                    }
                }
            }
            auto callee_name = std::string(p->resolve(callee.sym_id));
            return std::unexpected(Diagnostic{ErrorKind::TypeError,
                                              "cannot call: " + callee_name}
                .with_suggestion("did you forget to define '" + callee_name + "'?"));
        }
        case aura::ast::NodeTag::IfExpr: {
            if (v.children.size() < 3) return EvalResult(make_void());
            auto c = eval_flat(*f, *p, v.child(0), eval_env);
            if (!c) return c;
            current_id = is_truthy(*c) ? v.child(1) : v.child(2);
            continue; // TCO: branch
        }
        case aura::ast::NodeTag::Lambda: {
            // Capture params from FlatAST directly
            auto pspan = v.params;
            std::vector<std::string> params;
            params.reserve(pspan.size());
            for (auto pid : pspan)
                params.push_back(std::string(p->resolve(pid)));
            bool dotted = v.int_value != 0;
            auto* cap = copy_env(*current_env);
            auto cid = next_id();
            auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            closures_[cid] = Closure{std::move(params), f, p, body_id, cap, dotted};
            return make_closure(cid);
        }
        case aura::ast::NodeTag::Let:
        case aura::ast::NodeTag::LetRec: {
            bool rec = (v.tag == aura::ast::NodeTag::LetRec);
            auto name = p->resolve(v.sym_id);
            auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
            if (rec) {
                // For letrec, the init value is evaluated in the new env (with cell binding)
                tail_env.emplace(&eval_env);
                tail_env->set_primitives(&primitives_);
                tail_env->set_cells(&cells_);
                std::size_t ci = cells_.size();
                cells_.push_back(make_void());
                tail_env->bind(std::string(name), make_cell(ci));
                // Evaluate value in *tail_env (has cell binding for self-reference)
                auto vv = eval_flat(*f, *p, val_id, *tail_env);
                if (!vv) return vv;
                cells_[ci] = *vv;
                // Body evaluated in *tail_env (recursive refs need the child env)
                if (body_id != aura::ast::NULL_NODE)
                    return eval_flat(*f, *p, body_id, *tail_env);
                return make_void();
            } else {
                // For let, bind directly to current eval_env (like define) to avoid
                // creating a stack-local child env whose parent_ pointer becomes
                // dangling when captured by a closure (bug: closure capture copies
                // the env but parent_ still points to the original stack env).
                auto vv = eval_flat(*f, *p, val_id, eval_env);
                if (!vv) return vv;
                auto& me = const_cast<Env&>(eval_env);
                me.set_cells(&cells_);
                auto ci = cells_.size();
                cells_.push_back(*vv);
                me.bind(std::string(name), make_cell(ci));
                if (body_id != aura::ast::NULL_NODE)
                    return eval_flat(*f, *p, body_id, eval_env);
                return make_void();
            }
        }
        case aura::ast::NodeTag::DefineType: {
            // (define-type (Name params...) (Ctor fields...) ...)
            // Bind each constructor by evaluating an Aura lambda:
            //   (define <Ctor> (lambda args (cons 'Ctor args)))
            // This avoids C++ complexity and works with existing pair infrastructure.
            auto type_name = p->resolve(v.sym_id);
            Env& me = const_cast<Env&>(eval_env);
            me.set_cells(&cells_);
            
            for (auto cid : v.children) {
                if (cid >= f->size()) continue;
                auto cv = f->get(cid);
                if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty()) continue;
                auto quoted = cv.child(0);
                if (quoted >= f->size()) continue;
                auto qv = f->get(quoted);
                // Constructor data is now (cons 'ctor-name (cons 'ft1 (cons 'ft2 ...)))
                // Extract the constructor name from the head of the list
                std::string ctor_name;
                aura::ast::NodeId current = quoted;
                auto cur_v = f->get(current);
                if (cur_v.tag == aura::ast::NodeTag::Pair) {
                    auto car_id = cur_v.child(0);
                    if (car_id < f->size()) {
                        auto car_v = f->get(car_id);
                        if (car_v.tag == aura::ast::NodeTag::Variable)
                            ctor_name = std::string(p->resolve(car_v.sym_id));
                    }
                }
                if (ctor_name.empty()) continue;
                
                // Register constructor as a primitive that creates tagged lists:
                // (Ctor arg1 arg2 ...) → (cons 'Ctor (cons arg1 (cons arg2 ...)))
                auto tag_slot = string_heap_.size();
                string_heap_.push_back(ctor_name);
                auto tag_str = make_string(tag_slot);
                
                // Register as primitive
                primitives_.add(ctor_name, [this, tag_str](const auto& args) -> EvalValue {
                    types::EvalValue rest = make_void();
                    for (auto it = args.rbegin(); it != args.rend(); ++it) {
                        auto pid = static_cast<std::uint64_t>(pairs_.size());
                        pairs_.push_back({*it, rest});
                        rest = make_pair(pid);
                    }
                    auto pid = static_cast<std::uint64_t>(pairs_.size());
                    pairs_.push_back({tag_str, rest});
                    return make_pair(pid);
                });
            }
            return EvalResult(make_void());
        }

        case aura::ast::NodeTag::Define: {
            auto name = p->resolve(v.sym_id);
            auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            Env& me = const_cast<Env&>(eval_env);
            me.set_cells(&cells_);
            
            // Check if already bound as a cell — update existing cell to maintain
            // sequential define chains across multiple eval calls
            // Use lookup_binding to get the raw cell sentinel (not dereferenced value)
            auto existing = eval_env.lookup_binding(std::string(name));
            if (existing && is_cell(*existing)) {
                auto ci = as_cell_id(*existing);
                auto vv = eval_flat(*f, *p, val_id, eval_env);
                if (!vv) return vv;
                cells_[ci] = *vv;
                return *vv;
            }
            
            // Create new cell binding
            auto ci = alloc_cell(make_void());
            me.bind(std::string(name), make_cell(ci));
            auto vv = eval_flat(*f, *p, val_id, eval_env);
            if (!vv) return vv;
            cells_[ci] = *vv;
            return *vv;
        }
        case aura::ast::NodeTag::Begin: {
            auto count = v.children.size();
            if (count == 0) return EvalResult(make_void());
            
            // Check if there are multiple define nodes → use letrec semantics
            // Phase 1: pre-allocate cells for all defines
            std::vector<std::pair<std::string, aura::ast::NodeId>> letrec_defs;
            bool has_multiple_defs = false;
            int define_count = 0;
            aura::ast::NodeId last_expr = v.child(count - 1);
            for (std::size_t i = 0; i < count; ++i) {
                auto child_node = f->get(v.child(i));
                if (child_node.tag == aura::ast::NodeTag::Define) {
                    define_count++;
                    if (define_count > 1) has_multiple_defs = true;
                    letrec_defs.push_back({std::string(p->resolve(child_node.sym_id)),
                                            child_node.children.empty() ? aura::ast::NULL_NODE : child_node.child(0)});
                }
            }
            
            if (has_multiple_defs) {
                // Phase 1: pre-allocate cells for all defines
                // This ensures all function names are visible to each other
                std::vector<std::size_t> cell_ids;
                {
                    auto& mutable_env = const_cast<Env&>(eval_env);
                    mutable_env.set_cells(&cells_);
                    for (auto& d : letrec_defs) {
                        auto ci = alloc_cell(make_void());
                        mutable_env.bind(d.first, make_cell(ci));
                        cell_ids.push_back(ci);
                    }
                }
                // Phase 2: evaluate values and set cells
                for (std::size_t i = 0; i < letrec_defs.size(); ++i) {
                    auto& d = letrec_defs[i];
                    if (d.second != aura::ast::NULL_NODE) {
                        auto val = eval_flat(*f, *p, d.second, eval_env);
                        if (!val) return val;
                        cells_[cell_ids[i]] = *val;
                    }
                }
                // Phase 3: evaluate remaining (non-define) expressions
                for (std::size_t i = 0; i < count - 1; ++i) {
                    auto child_node = f->get(v.child(i));
                    if (child_node.tag == aura::ast::NodeTag::Define) continue;
                    auto r = eval_flat(*f, *p, v.child(i), eval_env);
                    if (!r) return r;
                }
                // TCO: last expression
                current_id = last_expr;
                continue;
            }
            
            // Single define (or no defines) — sequential evaluation
            for (std::size_t i = 0; i < count - 1; ++i) {
                auto r = eval_flat(*f, *p, v.child(i), eval_env);
                if (!r) return r;
            }
            current_id = v.child(count - 1);
            continue; // TCO: last expression in begin
        }
        case aura::ast::NodeTag::Export: {
            // (export sym ...) — record module API during loading
            // No runtime effect; children are Variable nodes
            if (!current_export_set_) {
                current_export_set_ = std::make_unique<std::unordered_set<std::string>>();
            }
            for (auto cid : v.children) {
                auto cv = f->get(cid);
                if (cv.tag == aura::ast::NodeTag::Variable) {
                    current_export_set_->insert(std::string(p->resolve(cv.sym_id)));
                }
            }
            return types::make_void();
        }
        case aura::ast::NodeTag::Set: {
            auto name = p->resolve(v.sym_id);
            auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            auto val = eval_flat(*f, *p, val_id, eval_env);
            if (!val) return val;
            auto* cell_ptr = eval_env.lookup_cell_ptr(std::string(name), &cells_);
            if (cell_ptr) {
                *cell_ptr = *val;
                return *val;
            }
            for (auto& b : const_cast<Env&>(eval_env).bindings()) {
                if (b.first == name) {
                    b.second = *val;
                    return *val;
                }
            }
            // Suggest closest bound variables
            {
                std::vector<std::string> candidates;
                {
                    const Env* e = &eval_env;
                    while (e) {
                        for (auto& b : const_cast<Env&>(*e).bindings())
                            candidates.push_back(b.first);
                        e = e->parent();
                    }
                }
                auto best = closest_match(name, candidates);
                Diagnostic d(ErrorKind::UnboundVariable, "set!: " + std::string(name));
                if (!best.empty())
                    d.with_suggestion("did you mean '" + best + "'?");
                return std::unexpected(std::move(d));
            }
            return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                              "set!: " + std::string(name)});
        }
        case aura::ast::NodeTag::Quote: {
            if (v.children.empty()) return EvalResult(make_void());
            return EvalResult(ast_to_data(*f, *p, v.child(0)));
        }
        case aura::ast::NodeTag::TypeAnnotation: {
            if (v.children.empty()) return EvalResult(make_void());
            auto annot_id = current_id;
            auto child_result = eval_flat(*f, *p, v.child(0), eval_env);
            if (!child_result) return child_result;
            // Runtime type check: compare value type against annotation
            if (type_registry_ && annot_id < f->size()) {
                auto expected_type_id = f->type_id(annot_id);
                if (expected_type_id != 0) {
                    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                    auto expected_tag = treg.tag_of(aura::core::TypeId{expected_type_id, 1});
                    auto actual_tag = runtime_type_tag(*child_result);
                    if (actual_tag != expected_tag && actual_tag != aura::core::TypeTag::DYNAMIC) {
                        auto& val = *child_result;
                        // Attempt coercion at runtime
                        bool coerced = coerce_value(val, actual_tag, expected_tag, string_heap_);
                        if (!coerced) {
                            std::string expected_name(treg.format_type(aura::core::TypeId{expected_type_id, 1}));
                            std::string actual_name = type_tag_name(actual_tag);
                            std::println(std::cerr, "type warning: expected {}, got {}\n", expected_name, actual_name);
                        }
                    }
                }
            }
            return child_result;
        }
        case aura::ast::NodeTag::MacroDef: {
            auto name = p->resolve(v.sym_id);
            std::vector<std::string> param_names;
            for (auto pn : v.params)
                param_names.push_back(std::string(p->resolve(pn)));
            auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            if (body_id == aura::ast::NULL_NODE) return EvalResult(make_void());

            // ── Warn: unused macro parameters ──────────────────────────
            // Scan the body for variable references and compare with params.
            {
                // Collect all variable names referenced in the macro body
                std::unordered_set<std::string> used_vars;
                auto collect_vars = [&](this const auto& self, aura::ast::NodeId nid) -> void {
                    if (nid == aura::ast::NULL_NODE || nid >= f->size()) return;
                    auto nv = f->get(nid);
                    if (nv.tag == aura::ast::NodeTag::Variable && nv.sym_id != aura::ast::INVALID_SYM) {
                        used_vars.insert(std::string(p->resolve(nv.sym_id)));
                    }
                    for (auto c : nv.children)
                        self(c);
                };
                collect_vars(body_id);

                int used_count = 0;
                for (auto& pn : param_names) {
                    if (used_vars.count(pn) == 0) {
                        std::println(std::cerr, "warning: macro '{}': parameter '{}' never used",
                                     std::string(name), pn);
                    } else {
                        ++used_count;
                    }
                }
                if (used_count == 0) {
                    std::println(std::cerr, "warning: macro '{}': body does not reference any parameter",
                                 std::string(name));
                }
            }

            // Store macro definition with proper dotted flag
            bool is_dotted = (v.int_value != 0);
            macros_[std::string(name)] = MacroDef{std::move(param_names), is_dotted, f, p, body_id};
            return EvalResult(make_void());
        }
        default:
            return std::unexpected(Diagnostic{ErrorKind::InternalError,
                                              "eval_flat: unsupported node type"});
        }
    }
    } catch (const std::bad_variant_access& e) {
        return std::unexpected(Diagnostic{ErrorKind::TypeError,
            std::format("type mismatch (wrong argument type passed to primitive): {}", e.what())});
    }
}

// ── Macro expander (hygienic Phase 2) ────────────────────────
// Clone a FlatAST subtree with MacroIntroduced markers.
// When a Variable matches a macro param, substitute with the arg expression.
// All new nodes are marked MacroIntroduced for hygiene tracking.
//
// name_map: when non-null, enables hygienic renaming — template-introduced
// binding positions (let, lambda, define) auto-gensym to avoid capture.
// References to gensym'd names are updated via the name_map.
static aura::ast::NodeId clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
    aura::ast::FlatAST& source, aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst,
    std::unordered_map<std::string, std::string>* name_map) {
    using namespace aura::ast;
    if (body_id == NULL_NODE || body_id >= source.size()) return NULL_NODE;
    auto v = source.get(body_id);

    // Set of built-in names that should never be gensym'd
    static const std::unordered_set<std::string> builtins = {
        "if", "cond", "let", "let*", "letrec", "lambda", "define",
        "begin", "set!", "quote", "unquote", "quasiquote", "case", "when", "unless",
        "car", "cdr", "cons", "list", "pair?", "null?", "eq?", "equal?",
        "+", "-", "*", "/", "=", "<", ">", "<=", ">=",
        "not", "and", "or", "void", "display", "write", "newline",
        "number?", "integer?", "float?", "boolean?", "string?", "symbol?",
        "string-append", "string-length", "string-ref", "substring",
        "number->string", "string->number", "apply", "map", "filter", "foldl",
    };

    // Variable substitution: if this variable is a macro param, return the arg clone
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = std::string(source_pool.resolve(v.sym_id));
        auto it = subst->find(name);
        if (it != subst->end()) {
            return clone_macro_body(target, target_pool, source, source_pool, it->second, subst, name_map);
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Resolve a name through name_map (hygiene: renamed binding)
    auto resolve_name = [&](SymId sid) -> std::string {
        if (sid == INVALID_SYM) return "";
        auto name = std::string(source_pool.resolve(sid));
        if (name_map) {
            auto it = name_map->find(name);
            if (it != name_map->end()) return it->second;
        }
        return name;
    };

    // Rename a binding position for hygiene: gensym if macro-introduced
    static std::uint64_t hyg_ctr = 0;
    auto rename_binding = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map) return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Macro params, builtins, and already-renamed names keep their name
        if ((subst && subst->count(name)) || builtins.count(name)) return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end()) return target_pool.intern(it->second);
        // Gensym! Create fresh name and track in name_map
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Clone children recursively
    std::vector<aura::ast::NodeId> child_ids;
    for (std::uint32_t i = 0; i < v.children.size(); ++i)
        child_ids.push_back(clone_macro_body(target, target_pool, source, source_pool, v.child(i), subst, name_map));

    // Clone params (for Lambda nodes) — with hygienic renaming
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(rename_binding(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
    case NodeTag::LiteralInt:
        new_id = target.add_literal(v.int_value); break;
    case NodeTag::LiteralString:
        new_id = target.add_literalstring(transplant(v.sym_id)); break;
    case NodeTag::Variable: {
        // Hygienic: check name_map for renamed bindings
        if (name_map) {
            auto name = resolve_name(v.sym_id);
            new_id = target.add_variable(target_pool.intern(name));
        } else {
            new_id = target.add_variable(transplant(v.sym_id));
        }
        break;
    }
    case NodeTag::Call: {
        std::vector<aura::ast::NodeId> args(child_ids.begin() + 1, child_ids.end());
        if (!child_ids.empty()) new_id = target.add_call(child_ids[0], args);
        break;
    }
    case NodeTag::IfExpr:
        if (child_ids.size() >= 3) new_id = target.add_if(child_ids[0], child_ids[1], child_ids[2]);
        break;
    case NodeTag::Lambda:
        if (!child_ids.empty()) new_id = target.add_lambda(param_syms, child_ids[0]);
        break;
    case NodeTag::Let:
    case NodeTag::LetRec:
        if (child_ids.size() >= 2)
            new_id = (v.tag == NodeTag::Let)
                ? target.add_let(rename_binding(v.sym_id), child_ids[0], child_ids[1])
                : target.add_letrec(rename_binding(v.sym_id), child_ids[0], child_ids[1]);
        break;
    case NodeTag::Begin:
        if (!child_ids.empty())
            new_id = target.add_begin(child_ids);
        break;
    case NodeTag::Set:
        if (!child_ids.empty()) new_id = target.add_set(transplant(v.sym_id), child_ids[0]);
        break;
    case NodeTag::Quote:
        if (!child_ids.empty()) new_id = target.add_quote(child_ids[0]);
        break;
    case NodeTag::Define:
        if (!child_ids.empty()) new_id = target.add_define(rename_binding(v.sym_id), child_ids[0]);
        break;
    default: break;
    }

    if (new_id != NULL_NODE) {
        target.set_marker(new_id, SyntaxMarker::MacroIntroduced);
        target.set_loc(new_id, v.line, v.col);
    }
    return new_id;
}

// ── Pre-expand all macros in a FlatAST ─────────────────────────
// Scans for MacroDef nodes, collects them, then expands all macro calls.
// Returns the (possibly new) root node of the expanded tree.
// After this pass, the FlatAST contains no MacroDef or macro calls.
// Multiple passes handle nested macros.
aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                           aura::ast::NodeId root, int max_passes) {
    using namespace aura::ast;
    for (int pass = 0; pass < max_passes; ++pass) {
        // Phase 1: collect macro definitions
        struct MD { aura::ast::FlatAST* src_flat; aura::ast::StringPool* src_pool; std::vector<std::string> params; NodeId body_id; bool dotted; };
        std::unordered_map<std::string, MD> local_macros;
        bool has_macro_def = false;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::MacroDef) {
                has_macro_def = true;
                // Macro name is in sym_id; params follow
                auto macro_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> params;
                for (auto pid : v.params)
                    params.push_back(std::string(pool.resolve(pid)));
                auto body_id = v.children.empty() ? NULL_NODE : v.child(0);
                bool is_dotted = (v.int_value != 0);
                local_macros[macro_name] = MD{&flat, &pool, std::move(params), body_id, is_dotted};
            }
        }

        if (!has_macro_def) return root;  // no more macros to expand

        // Phase 2: find and expand macro calls
        bool expanded_any = false;
        NodeId new_root = root;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee_v = flat.get(v.child(0));
                if (callee_v.tag == NodeTag::Variable) {
                    auto cname = std::string(pool.resolve(callee_v.sym_id));
                    auto it = local_macros.find(cname);
                    if (it != local_macros.end()) {
                        // Build substitution: macro param → arg expression
                        auto& md = it->second;
                        std::unordered_map<std::string, aura::ast::NodeId> subst;
                        std::size_t regular_count = md.dotted && md.params.size() > 0
                            ? md.params.size() - 1 : md.params.size();
                        for (std::size_t ai = 0; ai < regular_count && ai + 1 < v.children.size(); ++ai)
                            subst[md.params[ai]] = v.child(ai + 1);
                        // Rest param: collect remaining args as a quoted list
                        if (md.dotted && !md.params.empty() && regular_count + 1 < v.children.size()) {
                            auto& rest_name = md.params.back();
                            // Build a data list of the remaining arg nodes as a quote
                            // Create () as the base, then cons each remaining arg
                            std::vector<aura::ast::NodeId> remaining;
                            for (std::size_t ai = regular_count + 1; ai < v.children.size(); ++ai)
                                remaining.push_back(v.child(ai));
                            // Create nested quote: (quote (arg1 arg2 ...)) using add_call chains
                            // Actually, clone_macro_body substitutes Variable nodes, so we need
                            // the rest arg as an expression node, not data.
                            // For simplicity: build a (begin remaining...) — but that evaluates them.
                            // Build as (quote (arg1 arg2 ...)) by creating a proper list:
                            // cons arg1 (cons arg2 (cons ... ())) then wrap in quote
                            // Since these are NodeIds in the SAME FlatAST, we can build an expression
                            // that produces a list: (list arg1 arg2 ...)
                            auto list_var = flat.add_variable(pool.intern("list"));
                            std::vector<aura::ast::NodeId> all_args;
                            all_args.push_back(list_var);
                            all_args.insert(all_args.end(), remaining.begin(), remaining.end());
                            auto list_call = flat.add_call(list_var, all_args);
                            // But this would be (list arg1 arg2 ...) which EVALUATES the args.
                            // Macros need syntax (unevaluated). We need (quote (arg1 arg2 ...)).
                            // Create a quoted version: for each remaining arg, convert to data via
                            // ast_to_data... but we don't have access to the evaluator's pairs_.
                            // For now: just use the (list ...) approach and note that rest args
                            // in macro_expand_all will be evaluated (same as the evaluator's version)
                            // Actually this is the same issue as the evaluator's macros_ expansion.
                            // The difference: in macro_expand_all we can directly substitute.
                            // Let me just not handle rest in macro_expand_all for now — the evaluator's
                            // macros_ handles it correctly. This path is only for same-expression macros.
                        }
                        // Clone macro body with substitution
                        std::unordered_map<std::string, std::string> rename_map;
                        auto expanded = clone_macro_body(flat, pool, *md.src_flat, *md.src_pool, md.body_id, &subst, &rename_map);
                        if (expanded != NULL_NODE) {
                            if (id == root) new_root = expanded;
                            expanded_any = true;
                        }
                    }
                }
            }
        }

        if (!expanded_any) return root;
        root = new_root;
    }
    return root;
}

} // namespace aura::compiler
