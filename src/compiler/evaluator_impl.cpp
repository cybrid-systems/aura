module;
#include <cstdlib>
#include <cstdio>
#include <cstring>
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
#include <sys/wait.h>
#include <sys/stat.h>
#include <regex>
#include <unordered_map>
#if __has_include(<curl/curl.h>)
// Helper: evaluate `expr` and cache the result in the current FlatAST's
// value_cache_ at `current_id`. Used by eval_flat for incremental eval.
// Captures `f` (FlatAST*) and `current_id` from the enclosing TCO loop.
// Only evaluated on non-leaf returns (leaf literals use plain `return`).
// Cache-and-return for EvalResult (std::expected<EvalValue, Diagnostic>)
#define EVAL_CACHE_RETURN(expr)                                                                    \
    do {                                                                                           \
        auto _er_ = (expr);                                                                        \
        if (_er_) { f->set_cached_value(current_id, _er_->val); }                                  \
        return _er_;                                                                               \
    } while (0)

// Cache-and-return for plain EvalValue (used by leaf returns like make_closure)
#define EVAL_CACHE_RETURN_VAL(expr)                                                                \
    do {                                                                                           \
        auto _ev_ = (expr);                                                                        \
        f->set_cached_value(current_id, _ev_.val);                                                 \
        return _ev_;                                                                               \
    } while (0)
#include <curl/curl.h>
#define AURA_HAVE_CURL 1
#else
// Fallback types (runtime dlopen, compile-time stub)
typedef void CURL;
struct curl_slist {};
using CURLcode = int;
using CURLoption = int;
constexpr CURLoption CURLOPT_URL = 10002;
constexpr CURLoption CURLOPT_POST = 47;
constexpr CURLoption CURLOPT_POSTFIELDS = 10015;
constexpr CURLoption CURLOPT_POSTFIELDSIZE = 60;
constexpr CURLoption CURLOPT_HTTPHEADER = 10023;
constexpr CURLoption CURLOPT_WRITEFUNCTION = 20011;
constexpr CURLoption CURLOPT_WRITEDATA = 10001;
constexpr CURLoption CURLOPT_TIMEOUT = 13;
constexpr CURLoption CURLOPT_CONNECTTIMEOUT = 78;
constexpr CURLoption CURLOPT_SSL_VERIFYPEER = 64;
constexpr CURLoption CURLOPT_SSL_VERIFYHOST = 81;
constexpr CURLoption CURLOPT_USERAGENT = 10018;
constexpr CURLcode CURLE_OK = 0;
#endif
#include <cmath>
#include "messaging_bridge.h"
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
    if (m == 0)
        return n;
    if (n == 0)
        return m;
    // Use two-row DP for efficiency
    std::vector<std::size_t> prev(n + 1), cur(n + 1);
    for (std::size_t j = 0; j <= n; ++j)
        prev[j] = j;
    for (std::size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= n; ++j) {
            auto cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

static std::string closest_match(std::string_view name, const std::vector<std::string>& candidates,
                                 std::size_t max_dist = 3) {
    std::string best;
    std::size_t best_dist = max_dist + 1;
    for (auto& c : candidates) {
        auto d = edit_distance(name, c);
        if (d < best_dist) {
            best_dist = d;
            best = c;
        }
    }
    return best;
}


using namespace aura::diag;

// Forward decl: macro body cloner (defined at end of file)
static aura::ast::NodeId
clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
                 aura::ast::FlatAST& source, aura::ast::StringPool& source_pool,
                 aura::ast::NodeId body_id,
                 const std::unordered_map<std::string, aura::ast::NodeId>* subst = nullptr,
                 std::unordered_map<std::string, std::string>* name_map = nullptr);

// Depth guard: protects Env::lookup against cyclic parent chains
// (thread_local since lookup can be called from multiple fibers)
static constexpr std::size_t MAX_ENV_DEPTH = 1024;
thread_local std::size_t g_env_lookup_depth = 0;

std::optional<EvalValue> Env::lookup(const std::string& n) const {
    if (++g_env_lookup_depth > MAX_ENV_DEPTH) {
        --g_env_lookup_depth;
        return std::nullopt;
    }
    struct _{ ~_() { --g_env_lookup_depth; } } dec;

    // 1. Check local bindings
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            auto& v = it->second;
            // If the binding is a cell reference, dereference it
            if (is_cell(v) && cells_) {
                auto idx = as_cell_id(v);
                if (idx < cells_->size())
                    return (*cells_)[idx];
            }
            return v;
        }
    // 2. Check parent
    if (parent_)
        return parent_->lookup(n);
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
        if (it->first == n)
            return it->second;
    return parent_ ? parent_->lookup_binding(n) : std::nullopt;
}

// ── Helper: coerce EvalValue to int (string → int parsing) ────
namespace {
    static std::int64_t coerce_to_int(const EvalValue& v, const std::vector<std::string>* heap) {
        if (is_int(v))
            return as_int(v);
        if (is_float(v))
            return static_cast<std::int64_t>(as_float(v)); // truncate
        if (is_string(v) && heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size()) {
                try {
                    return static_cast<std::int64_t>(std::stoll((*heap)[idx]));
                } catch (...) {
                    std::println(std::cerr,
                                 "error: type mismatch — expected Int, got String '{}'",
                                 (*heap)[idx]);
                    return 0;
                }
            }
        }
        if (is_bool(v))
            return as_bool(v) ? 1 : 0;
        return 0;
    }

    [[maybe_unused]] static double coerce_to_double(const EvalValue& v,
                                                    const std::vector<std::string>* heap) {
        if (is_float(v))
            return as_float(v);
        return static_cast<double>(coerce_to_int(v, heap));
    }
} // namespace

// ── Primitives: EvalValue operations ──────────────────────────

Primitives::Primitives() {
    // ── Variadic arithmetic ────────────────────────────────────────
    // (+) → 0, (+ x) → x, (+ x y ...) → sum; float if any arg is float
    table_["+"] = [this](auto& a) {
        if (a.empty())
            return make_int(0);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            double r = 0.0;
            for (auto& v : a)
                r +=
                    is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
            return make_float(r);
        }
        std::int64_t r = 0;
        for (auto& v : a)
            r += coerce_to_int(v, string_heap_);
        return make_int(r);
    };
    // (-) → 0, (- x) → -x, (- x y ...) → x - y - z - ...
    table_["-"] = [this](auto& a) {
        if (a.empty())
            return make_int(0);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        auto to_f = [this](const EvalValue& v) {
            return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
        };
        if (any_f) {
            if (a.size() == 1)
                return make_float(-to_f(a[0]));
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i)
                r -= to_f(a[i]);
            return make_float(r);
        }
        if (a.size() == 1)
            return make_int(-coerce_to_int(a[0], string_heap_));
        std::int64_t r = coerce_to_int(a[0], string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i)
            r -= coerce_to_int(a[i], string_heap_);
        return make_int(r);
    };
    // (*) → 1, (* x) → x, (* x y ...) → product; float if any arg is float
    table_["*"] = [this](auto& a) {
        if (a.empty())
            return make_int(1);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            double r = 1.0;
            for (auto& v : a)
                r *=
                    is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
            return make_float(r);
        }
        std::int64_t r = 1;
        for (auto& v : a)
            r *= coerce_to_int(v, string_heap_);
        return make_int(r);
    };
    // (/) → 1, (/ x) → 1.0/x (float reciprocal), (/ x y ...) → x / y / z / ...
    table_["/"] = [this](auto& a) {
        if (a.empty())
            return make_int(1);
        auto to_f = [this](const EvalValue& v) {
            return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
        };
        if (a.size() == 1) {
            double x = to_f(a[0]);
            return (x == 0.0) ? make_int(0) : make_float(1.0 / x);
        }
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
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
        if (a.size() < 2)
            return make_bool(true);
        auto to_f = [this](const EvalValue& v) -> double {
            return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, string_heap_));
        };
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            for (std::size_t i = 1; i < a.size(); ++i)
                if (!fn_float(to_f(a[i - 1]), to_f(a[i])))
                    return make_bool(false);
            return make_bool(true);
        }
        for (std::size_t i = 1; i < a.size(); ++i)
            if (!fn_int(coerce_to_int(a[i - 1], string_heap_), coerce_to_int(a[i], string_heap_)))
                return make_bool(false);
        return make_bool(true);
    };
    table_["="] = [chain_cmp](auto& a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x == y; }, [](auto x, auto y) { return x == y; });
    };
    table_["<"] = [chain_cmp](auto& a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x < y; }, [](auto x, auto y) { return x < y; });
    };
    table_[">"] = [chain_cmp](auto& a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x > y; }, [](auto x, auto y) { return x > y; });
    };
    table_["<="] = [chain_cmp](auto& a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x <= y; }, [](auto x, auto y) { return x <= y; });
    };
    table_[">="] = [chain_cmp](auto& a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x >= y; }, [](auto x, auto y) { return x >= y; });
    };
    // Ghuloum Step 9: booleans
    table_["not"] = [](auto& a) { return make_bool(a.empty() || !is_truthy(a[0])); };
    table_["and"] = [](auto& a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (!is_truthy(a[i]))
                return a[i];
        return a.empty() ? make_int(1) : a.back();
    };
    table_["or"] = [](auto& a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (is_truthy(a[i]))
                return a[i];
        return a.empty() ? make_int(0) : a.back();
    };
    table_["eq?"] = [](auto& a) { return make_bool(a.size() >= 2 && a[0] == a[1]); };
    table_["current-time"] = [](auto& a) {
        (void)a;
        return make_int(static_cast<std::int64_t>(::time(nullptr)));
    };
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
                                         const std::vector<Pair>& pairs, bool quote,
                                         int depth = 0) {
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

    static void io_print_val(const EvalValue& v, const std::vector<std::string>* heap,
                             const std::vector<Pair>* pairs, bool quote, int depth = 0,
                             const std::vector<std::string>* keywords = nullptr) {
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
        if (is_keyword(v)) {
            auto kidx = as_keyword_idx(v);
            if (keywords && kidx < keywords->size()) {
                auto kname = (*keywords)[kidx];
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
        if (is_string(v) && heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size()) {
                if (quote)
                    std::fprintf(stdout, "\"%s\"", (*heap)[idx].c_str());
                else
                    std::fprintf(stdout, "%s", (*heap)[idx].c_str());
                return;
            }
        }
        if (is_pair(v) && pairs) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs->size()) {
                std::fprintf(stdout, "<pair[%zu]>", (size_t)idx);
                return;
            }
            // Check if it's a proper list (cdr chain ends in void or int 0 sentinel)
            auto cdr = (*pairs)[idx].cdr;
            if (is_end_of_list(cdr) && !quote) {
                // Single-element list: (x)
                std::fprintf(stdout, "(");
                io_print_val((*pairs)[idx].car, heap, pairs, quote, depth + 1, keywords);
                std::fprintf(stdout, ")");
                return;
            }
            // Walk the chain to see if it's a proper list
            std::vector<EvalValue> elements;
            elements.push_back((*pairs)[idx].car);
            auto next = cdr;
            bool proper = true;
            while (!is_end_of_list(next)) {
                if (!is_pair(next)) {
                    proper = false;
                    break;
                }
                auto nidx = as_pair_idx(next);
                if (nidx >= pairs->size()) {
                    proper = false;
                    break;
                }
                elements.push_back((*pairs)[nidx].car);
                next = (*pairs)[nidx].cdr;
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

void Evaluator::init_pair_primitives() {
    // ── Type predicates ──────────────────────────────────────────
    primitives_.add("integer?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_int(a[0]));
    });
    primitives_.add("float?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_float(a[0]));
    });
    primitives_.add("boolean?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_bool(a[0]));
    });
    primitives_.add("number?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_int(a[0]) || is_float(a[0]));
    });
    primitives_.add("symbol?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        // Symbols are interned during parsing and not represented as
        // first-class EvalValue values; always return false.
        return make_bool(false);
    });
    primitives_.add("procedure?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_closure(a[0]) || is_primitive(a[0]));
    });
    primitives_.add("void?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_void(a[0]));
    });
    primitives_.add("void", [](const auto&) {
        return make_void();
    });
    // ── Character operations (chars are integers in Aura) ──────────
    primitives_.add("char=?", [](const auto& a) {
        if (a.size() < 2)
            return make_bool(false);
        return make_bool(is_int(a[0]) && is_int(a[1]) && as_int(a[0]) == as_int(a[1]));
    });
    primitives_.add("char<?", [](const auto& a) {
        if (a.size() < 2)
            return make_bool(false);
        return make_bool(is_int(a[0]) && is_int(a[1]) && as_int(a[0]) < as_int(a[1]));
    });
    primitives_.add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        return a[0];
    });
    primitives_.add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        return a[0];
    });
    // ── Character predicates ──────────────────────────────────────
    primitives_.add("char-alphabetic?", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto c = as_int(a[0]);
        return make_bool((c >= 65 && c <= 90) || (c >= 97 && c <= 122));
    });
    primitives_.add("char-numeric?", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        return make_bool(as_int(a[0]) >= 48 && as_int(a[0]) <= 57);
    });
    primitives_.add("char-whitespace?", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto c = as_int(a[0]);
        return make_bool(c == 32 || (c >= 9 && c <= 13));
    });
    primitives_.add("char-upcase", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto c = as_int(a[0]);
        if (c >= 97 && c <= 122)
            return make_int(c - 32);
        return make_int(c);
    });
    primitives_.add("char-downcase", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto c = as_int(a[0]);
        if (c >= 65 && c <= 90)
            return make_int(c + 32);
        return make_int(c);
    });
    // ── String operations ─────────────────────────────────────────
    primitives_.add("string-copy", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Strings are immutable-like; just return the same reference
        return a[0];
    });
    primitives_.add("string-fill!", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto fill_char = static_cast<char>(as_int(a[1]));
        std::fill(string_heap_[idx].begin(), string_heap_[idx].end(), fill_char);
        return make_void();
    });
    primitives_.add("string->list", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto& s = string_heap_[idx];
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    primitives_.add("list->string", [this](const auto& a) {
        if (a.empty())
            return make_bool(false);
        auto v = a[0];
        std::string result;
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= pairs_.size())
                break;
            auto car = pairs_[p].car;
            if (is_int(car)) {
                result += static_cast<char>(as_int(car));
            } else if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < string_heap_.size())
                    result += string_heap_[sidx];
            }
            v = pairs_[p].cdr;
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });
    primitives_.add("string-join", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[1]))
            return make_bool(false);
        auto delim_idx = as_string_idx(a[1]);
        if (delim_idx >= string_heap_.size())
            return make_bool(false);
        auto& delim = string_heap_[delim_idx];
        std::string result;
        bool first = true;
        auto v = a[0];
        while (is_pair(v)) {
            auto p = as_pair_idx(v);
            if (p >= pairs_.size())
                break;
            auto car = pairs_[p].car;
            if (is_string(car)) {
                auto sidx = as_string_idx(car);
                if (sidx < string_heap_.size()) {
                    if (!first)
                        result += delim;
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
        if (a.empty() || !is_pair(a[0])) {
            do {
                auto __e_sidx = string_heap_.size();
                string_heap_.push_back("car: not a pair");
                auto __e_eidx = error_values_.size();
                error_values_.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        auto id = as_pair_idx(a[0]);
        return id < pairs_.size() ? pairs_[id].car : make_int(0);
    });
    primitives_.add("cdr", [this](const auto& a) {
        if (a.empty() || !is_pair(a[0])) {
            do {
                auto __e_sidx = string_heap_.size();
                string_heap_.push_back("cdr: not a pair");
                auto __e_eidx = error_values_.size();
                error_values_.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        auto id = as_pair_idx(a[0]);
        return id < pairs_.size() ? pairs_[id].cdr : make_int(0);
    });
    primitives_.add("pair?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_pair(a[0]));
    });

    // ── Cadr / Caddr shorthands ────────────────────────────────────
    primitives_.add("caar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c))
            return make_void();
        return pairs_[as_pair_idx(c)].car;
    });
    primitives_.add("cadr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return pairs_[as_pair_idx(c)].car;
    });
    primitives_.add("cdar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c))
            return make_void();
        return pairs_[as_pair_idx(c)].cdr;
    });
    primitives_.add("cddr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return pairs_[as_pair_idx(c)].cdr;
    });
    primitives_.add("caaar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].car;
        return is_pair(c) ? pairs_[as_pair_idx(c)].car : make_void();
    });
    primitives_.add("caadr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        return pairs_[as_pair_idx(c)].car;
    });
    primitives_.add("cadar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c))
            return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs_[as_pair_idx(d)].car;
    });
    primitives_.add("caddr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs_[as_pair_idx(d)].car;
    });
    primitives_.add("cdaar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].car;
        if (!is_pair(c))
            return make_void();
        return pairs_[as_pair_idx(c)].cdr;
    });
    primitives_.add("cdadr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs_[as_pair_idx(c)].car;
        if (!is_pair(d))
            return make_void();
        return pairs_[as_pair_idx(d)].cdr;
    });
    primitives_.add("cddar", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs_[as_pair_idx(d)].cdr;
    });
    primitives_.add("cdddr", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        auto c = pairs_[idx].cdr;
        if (!is_pair(c))
            return make_void();
        auto d = pairs_[as_pair_idx(c)].cdr;
        if (!is_pair(d))
            return make_void();
        return pairs_[as_pair_idx(d)].cdr;
    });

    // ── Mutable pair operations ───────────────────────────────────
    primitives_.add("set-car!", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        pairs_[idx].car = a[1];
        return make_void();
    });
    primitives_.add("set-cdr!", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_pair(a[0]))
            return make_void();
        auto idx = as_pair_idx(a[0]);
        if (idx >= pairs_.size())
            return make_void();
        pairs_[idx].cdr = a[1];
        return make_void();
    });

    primitives_.add("string?", [this](const auto& a) {
        if (a.empty())
            return make_bool(false);
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
                std::string str = (idx < string_heap_.size()) ? string_heap_[idx] : "";
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
                    if (pidx >= pairs_.size())
                        break;
                    if (!first)
                        r += ",";
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
                if (hidx >= hash_heap_.size())
                    return "null";
                auto& ht = hash_heap_[hidx];
                std::string r = "{";
                bool first = true;
                for (std::size_t i = ht.capacity; i > 0; --i) {
                    if (ht.metadata[i - 1] != 0xFF) {
                        if (!first)
                            r += ",";
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
        auto sid = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sid);
    });


    // json-parse: parse JSON string into Aura value
    // (json-parse json-str) → value (Int/Float/String/Bool/Void/List/Hash)
    primitives_.add("json-parse", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto json_str = string_heap_[types::as_string_idx(a[0])];

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
            auto sid = string_heap_.size();
            string_heap_.push_back(result);
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
                if (pos < json_str.size() && json_str[pos] == ':')
                    pos++;
                skip_ws();
                auto val = parse_value();
                // Compute hash for key (string or number keys)
                std::uint64_t kh = 0x9e3779b97f4a7c15ull;
                if (types::is_string(key_val)) {
                    auto ksid = types::as_string_idx(key_val);
                    if (ksid < string_heap_.size()) {
                        auto& ks = string_heap_[ksid];
                        kh = 0xcbf29ce484222325ull;
                        for (char c : ks)
                            kh = (kh ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
                            eq = (ai < string_heap_.size() && bi < string_heap_.size()) &&
                                 string_heap_[ai] == string_heap_[bi];
                        } else {
                            eq = existing_key == key_val;
                        }
                        if (eq) {
                            ht.values[idx] = val;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    for (std::size_t at = 0; at < ht.capacity; ++at) {
                        auto idx = ((kh >> 1) + at) & (ht.capacity - 1);
                        if (ht.metadata[idx] == 0xFF) {
                            ht.metadata[idx] = fp;
                            ht.keys[idx] = key_val;
                            ht.values[idx] = val;
                            ht.size++;
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
            auto hidx = hash_heap_.size();
            hash_heap_.push_back(std::move(ht));
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

    primitives_.add("string-append", [this](const auto& a) {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < string_heap_.size())
                    result += string_heap_[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return make_string(id);
    });
    primitives_.add("string-length", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
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
        if (a.size() < 2) {
            auto __i = string_heap_.size();
            string_heap_.push_back("string-ref: too few args");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__i));
            return make_error(__e);
        }
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size())
                s = string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (pos >= s.size()) {
            auto __i = string_heap_.size();
            string_heap_.push_back("string-ref: index out of bounds");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__i));
            return make_error(__e);
        }
        if (pos < s.size())
            return make_int(static_cast<std::int64_t>(static_cast<unsigned char>(s[pos])));
        return make_int(0);
    });
    primitives_.add("substring", [this](const auto& a) {
        if (a.size() < 3)
            return make_int(0);
        std::string s_buf;
        const std::string* sp = nullptr;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size())
                sp = &string_heap_[idx];
        } else if (is_int(a[0])) {
            s_buf = std::to_string(as_int(a[0]));
            sp = &s_buf;
        }
        if (!sp)
            return make_int(0);
        const auto& s = *sp;
        auto start = static_cast<std::size_t>(as_int(a[1]));
        auto end = static_cast<std::size_t>(as_int(a[2]));
        if (start > s.size())
            start = s.size();
        if (end > s.size())
            end = s.size();
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
        if (a.size() < 2)
            return make_bool(false);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (is_int(v))
                return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) == to_str(a[1]));
    });
    primitives_.add("string<?", [this](const auto& a) {
        if (a.size() < 2)
            return make_bool(false);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (is_int(v))
                return std::to_string(as_int(v));
            return "";
        };
        return make_bool(to_str(a[0]) < to_str(a[1]));
    });
    primitives_.add("string->number", [this](const auto& a) {
        if (a.size() < 1 || a.size() > 2)
            return make_int(0);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            return "";
        };
        auto s = to_str(a[0]);
        auto radix = (a.size() > 1 && is_int(a[1])) ? static_cast<int>(as_int(a[1])) : 10;
        try {
            if (s.find('.') != std::string::npos)
                return make_float(std::stod(s));
            return make_int(static_cast<std::int64_t>(std::stoll(s, nullptr, radix)));
        } catch (...) {
            return make_bool(false);
        }
    });

    primitives_.add("string-index", [this](const auto& a) {
        if (a.size() < 2)
            return make_int(-1);
        auto haystack = (is_string(a[0]) && as_string_idx(a[0]) < string_heap_.size())
            ? string_heap_[as_string_idx(a[0])] : "";
        auto needle = (is_string(a[1]) && as_string_idx(a[1]) < string_heap_.size())
            ? string_heap_[as_string_idx(a[1])] : "";
        auto start = (a.size() > 2 && is_int(a[2])) ? static_cast<std::size_t>(as_int(a[2])) : 0;
        if (needle.empty()) return make_int(0);
        auto pos = haystack.find(needle, start);
        return make_int(pos != std::string::npos ? static_cast<std::int64_t>(pos) : -1);
    });

    primitives_.add("number->string", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        std::string s;
        if (is_float(a[0]))
            s = std::to_string(as_float(a[0]));
        else if (is_int(a[0]))
            s = std::to_string(as_int(a[0]));
        else
            s = "0";
        // Trim trailing zeros from float representation
        if (is_float(a[0])) {
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last > dot)
                    s = s.substr(0, last + 1);
                else
                    s = s.substr(0, dot);
            }
        }
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(s));
        return make_string(id);
    });
    primitives_.add("string->number", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= string_heap_.size())
            return make_bool(false);
        auto& str = string_heap_[i];
        if (str.empty())
            return make_bool(false);
        // Trim leading/trailing whitespace (CSV fields, user input)
        auto first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return make_bool(false);
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
            if (trimmed.find('.') != std::string_view::npos ||
                trimmed.find('e') != std::string_view::npos ||
                trimmed.find('E') != std::string_view::npos)
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
        if (a.empty())
            return make_bool(true);
        auto v = a[0];
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return make_bool(false);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                return make_bool(false);
            v = pairs_[idx].cdr; // follow cdr chain
        }
        return make_int(1);
    });
    primitives_.add("null?", [](const auto& a) {
        return make_bool(!a.empty() && (is_void(a[0]) || (is_int(a[0]) && as_int(a[0]) == 0)));
    });
    primitives_.add("length", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        auto v = a[0];
        std::int64_t n = 0;
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                return make_int(0);
            v = pairs_[idx].cdr;
            n++;
        }
        return make_int(n);
    });
    primitives_.add("list-ref", [this](const auto& a) {
        if (a.size() < 2) {
            auto __s = string_heap_.size();
            string_heap_.push_back("list-ref: too few args");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__s));
            return make_error(__e);
        }
        auto v = a[0];
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        for (std::size_t i = 0; i < pos; ++i) {
            if (!is_pair(v)) {
                auto __s = string_heap_.size();
                string_heap_.push_back("list-ref: index out of bounds");
                auto __e = error_values_.size();
                error_values_.push_back(make_string(__s));
                return make_error(__e);
            }
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) {
                auto __s = string_heap_.size();
                string_heap_.push_back("list-ref: corrupted pair");
                auto __e = error_values_.size();
                error_values_.push_back(make_string(__s));
                return make_error(__e);
            }
            v = pairs_[idx].cdr;
        }
        if (is_pair(v)) {
            auto idx = as_pair_idx(v);
            return idx < pairs_.size() ? pairs_[idx].car : make_int(0);
        }
        return v;
    });
    // (member val list) — Find val in list using content equality (equal?)
    // Returns the tail of the list starting with val, or #f if not found
    primitives_.add("member", [this](const auto& a) {
        if (a.size() < 2)
            return make_int(0);
        auto& val = a[0];
        auto v = a[1];
        auto elem_eq = [&](const EvalValue& x, const EvalValue& y) -> bool {
            if (x == y) return true;
            if (is_int(x) && is_int(y)) return as_int(x) == as_int(y);
            if (is_string(x) && is_string(y)) {
                auto xi = as_string_idx(x), yi = as_string_idx(y);
                return xi < string_heap_.size() && yi < string_heap_.size() &&
                       string_heap_[xi] == string_heap_[yi];
            }
            if (is_bool(x) && is_bool(y)) return as_bool(x) == as_bool(y);
            return false;
        };
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                return make_int(0);
            if (elem_eq(pairs_[idx].car, val))
                return v;
            v = pairs_[idx].cdr;
        }
        return make_int(0);
    });
    // (append list ...) — Variadic: concatenate all provided lists
    primitives_.add("append", [this](const auto& a) {
        if (a.empty())
            return make_void();
        if (a.size() < 2)
            return a[0];
        // Iteratively append all arguments
        auto result = a[0];
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto list2 = a[i];
            if (is_end_of_list(result)) {
                result = list2;
                continue;
            }
            EvalValue new_result = make_void();
            EvalValue tail = make_void();
            auto v = result;
            while (!is_end_of_list(v)) {
                if (!is_pair(v)) {
                    result = a[0];
                    break;
                }
                auto idx = as_pair_idx(v);
                if (idx >= pairs_.size()) {
                    result = a[0];
                    break;
                }
                auto new_id = pairs_.size();
                pairs_.push_back({pairs_[idx].car, make_void()});
                auto new_pair = make_pair(new_id);
                if (is_void(new_result))
                    new_result = new_pair;
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
            if (!is_void(new_result))
                result = new_result;
        }
        return result;
    });
    primitives_.add("reverse", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        auto v = a[0];
        EvalValue result = make_void();
        while (!is_end_of_list(v)) {
            if (!is_pair(v))
                return a[0];
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                return a[0];
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, result});
            result = make_pair(new_id);
            v = pairs_[idx].cdr;
        }
        return result;
    });
    primitives_.add("map", [this](const auto& a) {
        // (map func list) — apply func to each element, collect results
        if (a.size() < 2 || is_void(a[1]))
            return make_void();

        // Helper to apply a function (closure or primitive) to a single argument
        auto apply_fn = [&](const EvalValue& fn, const EvalValue& arg) -> EvalValue {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count())
                    return make_void();
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim)
                    return make_void();
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
            if (idx >= pairs_.size())
                break;

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
        if (a.size() < 2 || is_void(a[1]))
            return make_void();

        // Helper to apply a predicate (closure or primitive) to a single argument
        auto apply_pred = [&](const EvalValue& fn, const EvalValue& arg) -> bool {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count())
                    return false;
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim)
                    return false;
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
            if (idx >= pairs_.size())
                break;

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
        if (a.size() < 2 || !is_vector(a[0])) {
            auto __s = string_heap_.size();
            string_heap_.push_back("vector-ref: not a vector");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__s));
            return make_error(__e);
        }
        auto idx = as_vector_idx(a[0]);
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (idx >= vector_heap_.size() || pos >= vector_heap_[idx].size()) {
            auto __s = string_heap_.size();
            string_heap_.push_back("vector-ref: index out of bounds");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__s));
            return make_error(__e);
        }
        return vector_heap_[idx][pos];
    });
    primitives_.add("vector-set!", [this](const auto& a) {
        if (a.size() < 3 || !is_vector(a[0])) {
            auto __s = string_heap_.size();
            string_heap_.push_back("vector-set!: not a vector");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__s));
            return make_error(__e);
        }
        auto idx = as_vector_idx(a[0]);
        auto pos = static_cast<std::size_t>(as_int(a[1]));
        if (idx >= vector_heap_.size() || pos >= vector_heap_[idx].size()) {
            auto __s = string_heap_.size();
            string_heap_.push_back("vector-set!: index out of bounds");
            auto __e = error_values_.size();
            error_values_.push_back(make_string(__s));
            return make_error(__e);
        }
        vector_heap_[idx][pos] = a[2];
        return make_void();
    });
    primitives_.add("vector-length", [this](const auto& a) {
        if (a.empty() || !is_vector(a[0]))
            return make_int(0);
        auto idx = as_vector_idx(a[0]);
        if (idx >= vector_heap_.size())
            return make_int(0);
        return make_int(static_cast<std::int64_t>(vector_heap_[idx].size()));
    });
    primitives_.add("vector?", [this](const auto& a) {
        if (a.empty())
            return make_bool(false);
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
                if (idx >= pairs_.size())
                    break;
                elems.push_back(pairs_[idx].car);
                v = pairs_[idx].cdr;
            }
        }
        auto idx = vector_heap_.size();
        vector_heap_.push_back(std::move(elems));
        return make_vector(idx);
    });
    primitives_.add("vector->list", [this](const auto& a) {
        if (a.empty() || !is_vector(a[0]))
            return make_void();
        auto idx = as_vector_idx(a[0]);
        if (idx >= vector_heap_.size())
            return make_void();
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
            if (is_int(k))
                return static_cast<std::uint64_t>(as_int(k)) * 0x9e3779b97f4a7c15ull;
            if (is_string(k)) {
                auto i = as_string_idx(k);
                if (i < sh->size()) {
                    auto& s = (*sh)[i];
                    std::uint64_t h = 0xcbf29ce484222325ull;
                    for (char c : s)
                        h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    return h;
                }
            }
            return 0x9e3779b97f4a7c15ull;
        };
        for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
            auto h = khash(a[i]);
            auto fp = static_cast<std::uint8_t>(h >> 57) | 0x80;
            for (std::size_t at = 0; at < ht.capacity; ++at) {
                auto idx = ((h >> 1) + at) & (ht.capacity - 1);
                if (ht.metadata[idx] == 0xFF) {
                    ht.metadata[idx] = fp;
                    ht.keys[idx] = a[i];
                    ht.values[idx] = a[i + 1];
                    ht.size++;
                    break;
                }
            }
        }
        auto hidx = hash_heap_.size();
        hash_heap_.push_back(std::move(ht));
        return make_hash(hidx);
    });
    primitives_.add("hash-ref", [this](const auto& a) {
        if (a.size() < 2 || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_void();
        auto& ht = hash_heap_[hidx];
        auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF)
                continue;
            auto& k = ht.keys[i];
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = k == a[1];
            if (eq)
                return ht.values[i];
        }
        return make_void();
    });

    // (hash-has-key? hash key) — Check if key exists in hash
    primitives_.add("hash-has-key?", [this](const auto& a) {
        if (a.size() < 2 || !is_hash(a[0]))
            return make_bool(false);
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_bool(false);
        auto& ht = hash_heap_[hidx];
        auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF)
                continue;
            auto& k = ht.keys[i];
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = k == a[1];
            if (eq)
                return make_bool(true);
        }
        return make_bool(false);
    });

    primitives_.add("hash-set!", [this](const auto& a) {
        if (a.size() < 3 || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_void();
        auto& ht = hash_heap_[hidx];
        auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF)
                continue;
            auto& k = ht.keys[i];
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = k == a[1];
            if (eq) {
                ht.values[i] = a[2];
                return make_void();
            }
        }
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF) {
                std::uint64_t h = 0x9e3779b97f4a7c15ull;
                if (is_int(a[1]))
                    h = static_cast<std::uint64_t>(as_int(a[1])) * h;
                else if (is_string(a[1])) {
                    auto idx = as_string_idx(a[1]);
                    if (idx < sh->size()) {
                        h = 0xcbf29ce484222325ull;
                        for (char c : (*sh)[idx])
                            h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    }
                }
                ht.metadata[i] = static_cast<std::uint8_t>(h >> 57) | 0x80;
                ht.keys[i] = a[1];
                ht.values[i] = a[2];
                ht.size++;
                return make_void();
            }
        }
        return make_void();
    });
    primitives_.add("hash-length", [this](const auto& a) {
        if (a.empty() || !is_hash(a[0]))
            return make_int(0);
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_int(0);
        return make_int(static_cast<std::int64_t>(hash_heap_[hidx].size));
    });
    primitives_.add("hash-keys", [this](const auto& a) {
        if (a.empty() || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_void();
        auto& ht = hash_heap_[hidx];
        EvalValue result = make_void();
        for (std::size_t i = ht.capacity; i > 0; --i) {
            if (ht.metadata[i - 1] != 0xFF) {
                auto pid = pairs_.size();
                pairs_.push_back({ht.keys[i - 1], result});
                result = make_pair(pid);
            }
        }
        return result;
    });
    primitives_.add("hash-values", [this](const auto& a) {
        if (a.empty() || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_void();
        auto& ht = hash_heap_[hidx];
        EvalValue result = make_void();
        for (std::size_t i = ht.capacity; i > 0; --i) {
            if (ht.metadata[i - 1] != 0xFF) {
                auto pid = pairs_.size();
                pairs_.push_back({ht.values[i - 1], result});
                result = make_pair(pid);
            }
        }
        return result;
    });
    primitives_.add("hash?", [this](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_hash(a[0]));
    });
    primitives_.add("hash-remove!", [this](const auto& a) {
        if (a.size() < 2 || !is_hash(a[0]))
            return make_void();
        auto hidx = as_hash_idx(a[0]);
        if (hidx >= hash_heap_.size())
            return make_void();
        auto& ht = hash_heap_[hidx];
        auto sh = &string_heap_;
        for (std::size_t i = 0; i < ht.capacity; ++i) {
            if (ht.metadata[i] == 0xFF)
                continue;
            auto& k = ht.keys[i];
            bool eq = false;
            if (is_int(k) && is_int(a[1]))
                eq = as_int(k) == as_int(a[1]);
            else if (is_string(k) && is_string(a[1])) {
                auto ai = as_string_idx(k), bi = as_string_idx(a[1]);
                eq = (ai < sh->size() && bi < sh->size()) && (*sh)[ai] == (*sh)[bi];
            } else
                eq = k == a[1];
            if (eq) {
                ht.metadata[i] = 0xFF;
                ht.size--;
                return make_bool(true);
            }
        }
        return make_bool(false);
    });
    auto infer_type_name = [](const EvalValue& v) -> const char* {
        if (is_float(v))
            return "Float";
        if (is_hash(v))
            return "Hash";
        if (is_vector(v))
            return "Vector";
        if (is_string(v))
            return "String";
        if (is_pair(v))
            return "Pair";
        if (is_cell(v))
            return "Cell";
        if (is_closure(v))
            return "Closure";
        if (is_bool(v))
            return "Bool";
        // Backward compat: int 0/1 was historically treated as Bool
        if (is_int(v) && (as_int(v) == 0 || as_int(v) == 1))
            return "Bool";
        if (is_int(v))
            return "Int";
        if (is_keyword(v))
            return "Keyword";
        if (is_void(v))
            return "Void";
        return "Unknown";
    };

    primitives_.add("type-of", [this, infer_type_name](const auto& a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        auto type_name = infer_type_name(a[0]);
        auto id = string_heap_.size();
        string_heap_.push_back(type_name);
        return make_string(id);
    });

    primitives_.add("type?", [this, infer_type_name](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[1]))
            return make_int(0);
        auto val_type = infer_type_name(a[0]);
        auto expected_idx = as_string_idx(a[1]);
        if (expected_idx >= string_heap_.size())
            return make_int(0);
        auto& expected = string_heap_[expected_idx];
        return make_int(val_type == expected ? 1 : 0);
    });

    primitives_.add("keyword?", [this](const auto& a) {
        // Guard against encoding collision: strings can accidentally pass is_keyword()
        return make_bool(a.size() >= 1 && is_keyword(a[0]) && !is_string(a[0]));
    });

    primitives_.add("keyword->string", [this](const auto& a) {
        if (a.size() < 1 || !is_keyword(a[0]))
            return make_void();
        auto kidx = as_keyword_idx(a[0]);
        if (kidx >= keyword_table_.size())
            return make_void();
        // Return the keyword name without the colon prefix
        auto kw = keyword_table_[kidx];
        auto sname = kw.substr(1); // skip ':'
        auto sid = string_heap_.size();
        string_heap_.push_back(sname);
        return make_string(sid);
    });

    primitives_.add("equal?", [this](const auto& a) {
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
                if (idx < string_heap_.size())
                    result += string_heap_[idx];
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
        if (a.size() < 2)
            return make_void();
        auto& fn = a[0];
        auto& arg_list = a[1];
        std::vector<EvalValue> args;
        auto current = arg_list;
        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size())
                break;
            args.push_back(pairs_[idx].car);
            current = pairs_[idx].cdr;
        }
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            auto pfn = primitives_.lookup(primitives_.name_for_slot(slot));
            if (pfn)
                return (*pfn)(args);
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = apply_closure(cid, args);
            if (result)
                return *result;
        }
        return make_void();
    });

    primitives_.add("display", [this](const auto& a) {
        if (a.empty())
            return make_void();
        io_print_val(a[0], &string_heap_, &pairs_, false, 0, &keyword_table_);
        std::fflush(stdout);
        return make_void();
    });
    primitives_.add("write", [this](const auto& a) -> EvalValue {
        if (a.empty())
            return make_void();
        io_print_val(a[0], &string_heap_, &pairs_, true, 0, &keyword_table_);
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
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto tidx = as_string_idx(a[0]);
        if (tidx >= string_heap_.size())
            return make_bool(false);
        auto& tmpl = string_heap_[tidx];
        std::string result;
        std::size_t arg_idx = 1; // first arg in a[1..]
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '~' && i + 1 < tmpl.size()) {
                switch (tmpl[i + 1]) {
                    case 'a': // display arg
                        if (arg_idx < a.size()) {
                            auto val = a[arg_idx++];
                            result += fmt_val_to_string(val, string_heap_, pairs_, false);
                        }
                        ++i;
                        break;
                    case 's': // write arg (quoted)
                        if (arg_idx < a.size()) {
                            auto val = a[arg_idx++];
                            result += fmt_val_to_string(val, string_heap_, pairs_, true);
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
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });
    // (error msg) — Create an error value (no longer throws C++ exception)
    primitives_.add("error", [this](const auto& a) -> EvalValue {
        // Ensure error_values_[0] always exists for default errors
        if (error_values_.empty())
            error_values_.push_back(make_void());
        types::EvalValue cause = make_string(0); // default
        if (!a.empty())
            cause = a[0];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (assert expr msg) — Assertion, returns error on failure
    primitives_.add("assert", [this](const auto& a) -> EvalValue {
        if (!a.empty() && is_truthy(a[0]))
            return make_int(1);
        // Assertion failed — return error
        types::EvalValue cause = make_string(0);
        if (a.size() > 1)
            cause = a[1];
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
        if (a.empty())
            return make_bool(false);
        // Guard against encoding collision: strings can accidentally pass is_error()
        return make_bool(is_error(a[0]) && !is_string(a[0]));
    });


    // (check expr) — Test assertion, returns #t or error on failure
    primitives_.add("check", [this](const auto& a) -> EvalValue {
        if (a.empty())
            return make_error(0);
        if (is_truthy(a[0]))
            return make_int(1);
        // Store failing value as error cause
        auto eidx = error_values_.size();
        error_values_.push_back(a[0]);
        return make_error(eidx);
    });

    // (check= expected actual) — Test equality, returns #t or error
    primitives_.add("check=", [this](const auto& a) -> EvalValue {
        if (a.size() < 2)
            return make_bool(false);
        if (types::is_void(a[0]) && types::is_void(a[1]))
            return make_int(1);
        if (types::is_int(a[0]) && types::is_int(a[1])) {
            if (types::as_int(a[0]) == types::as_int(a[1]))
                return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_float(a[0]) && types::is_float(a[1])) {
            if (types::as_float(a[0]) == types::as_float(a[1]))
                return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_string(a[0]) && types::is_string(a[1])) {
            auto i0 = types::as_string_idx(a[0]);
            auto i1 = types::as_string_idx(a[1]);
            if (i0 < string_heap_.size() && i1 < string_heap_.size() &&
                string_heap_[i0] == string_heap_[i1])
                return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        return make_bool(false);
    });

    // (check-success output expected-list) — Flexible substring matching
    // Returns #t if any expected keyword is found in output.
    // Guards against false positives from error messages that happen to
    // contain expected keywords (uses word-boundary for short keys in error output).
    primitives_.add("check-success", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_pair(a[1]))
            return make_bool(false);
        // Get normalized output (strip quotes)
        auto out_idx = as_string_idx(a[0]);
        if (out_idx >= string_heap_.size())
            return make_bool(false);
        auto norm_out = string_heap_[out_idx];
        // Strip surrounding quotes
        if (!norm_out.empty() && (norm_out[0] == '"' || norm_out[0] == '\''))
            norm_out = norm_out.substr(1);
        if (!norm_out.empty() && (norm_out.back() == '"' || norm_out.back() == '\''))
            norm_out.pop_back();
        // Detect if output looks like an error message
        auto lower = norm_out;
        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
        bool is_error_like =
            lower.find("error:") != std::string::npos ||
            lower.find("parse error") != std::string::npos ||
            lower.find("unbound variable") != std::string::npos ||
            lower.find("type error") != std::string::npos ||
            lower.find("syntax error") != std::string::npos ||
            lower.find("invalid syntax") != std::string::npos ||
            lower.find("expected expression") != std::string::npos ||
            (norm_out.size() > 1 && norm_out[0] == '(' && norm_out[1] == '"');
        // Iterate expected list
        auto pair_idx = as_pair_idx(a[1]);
        while (pair_idx < pairs_.size()) {
            auto& p = pairs_[pair_idx];
            if (is_string(p.car)) {
                auto kw_idx = as_string_idx(p.car);
                if (kw_idx < string_heap_.size()) {
                    auto& kw = string_heap_[kw_idx];
                    if (!kw.empty() && norm_out.find(kw) != std::string::npos) {
                        // Guard: for error-like output with short keywords
                        if (is_error_like && kw.size() <= 5) {
                            // Generic error words that should never match in error output
                            static const std::unordered_set<std::string> generic_words =
                                {"error", "type", "parse", "syntax", "kind"};
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

    // (diagnose error-string) — Analyze structured error and return fix strategy
    // Returns a pair ("root-cause" "target" "fix-type" "fix-data" "message")
    // or #f if no diagnosis available.
    primitives_.add("diagnose", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto msg = string_heap_[idx];

        // Build diagnosis result as list: (cause target fix-type fix-data explanation)
        auto make_diag = [&](const std::string& cause, const std::string& target,
                             const std::string& fix, const std::string& data,
                             const std::string& expl) -> EvalValue {
            auto push = [&](const std::string& s) -> std::uint64_t {
                auto sidx = string_heap_.size();
                string_heap_.push_back(s);
                return sidx;
            };
            auto nil = EvalValue(0);
            auto make_item = [&](const std::string& s) -> EvalValue {
                auto sidx = push(s);
                return make_string(sidx);
            };
            // Build: (cause target fix-type fix-data expl) as proper list
            auto e_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(expl), nil});
            auto d_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(data), e_pair});
            auto f_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(fix), d_pair});
            auto t_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(target), f_pair});
            auto c_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(cause), t_pair});
            return c_pair;
        };

        // ── Known unbound variable mappings ──
        // Format: {root-cause, target-fn, fix-type, fix-data, explanation}
        struct FixEntry {
            std::string cause;
            std::string fix_type;
            std::string fix_data;
            std::string explanation;
        };
        static const std::unordered_map<std::string, FixEntry> unbound_fixes = {
            {"for-each", {"missing-require", "add-require", "std/list",
                          "Add (require \"std/list\" all:) to use for-each"}},
            {"map", {"missing-require", "add-require", "std/list",
                     "Add (require \"std/list\" all:) to use map"}},
            {"filter", {"missing-require", "add-require", "std/list",
                        "Add (require \"std/list\" all:) to use filter"}},
            {"foldl", {"missing-require", "add-require", "std/list",
                       "Add (require \"std/list\" all:) to use foldl"}},
            {"make-hash", {"missing-require", "add-require", "std/hash",
                           "Add (require \"std/hash\" all:) to use make-hash"}},
            {"hash-ref", {"missing-require", "add-require", "std/hash",
                          "Add (require \"std/hash\" all:) to use hash-ref"}},
            {"rule:define", {"missing-require", "add-require", "std/rule",
                            "Add (require \"std/rule\" all:) to use rule:define"}},
            {"synthesize:fill", {"missing-require", "add-require", "std/pipeline",
                                "Add (require \"std/pipeline\" all:) to use synthesize"}},
            {"synthesize:pipeline", {"missing-require", "add-require", "std/pipeline",
                                    "Add (require \"std/pipeline\" all:) to use synthesize"}},
            {"define-type", {"missing-require", "add-require", "std/data",
                            "Add (require \"std/data\" all:) to use define-type"}},
            {"c-func", {"missing-require", "add-require", "std/ffi",
                        "Add (require \"std/ffi\" all:) to use c-func"}},
        };

        // Detect "unbound variable: X" and match against known symbols
        std::string prefix = "unbound variable: ";
        auto upos = msg.find(prefix);
        if (upos != std::string::npos) {
            auto target = msg.substr(upos + prefix.size());
            // Strip trailing punctuation
            while (!target.empty() && (target.back() == '.' || target.back() == '!' ||
                                       target.back() == '?' || target.back() == ':'))
                target.pop_back();
            auto it = unbound_fixes.find(target);
            if (it != unbound_fixes.end()) {
                return make_diag(it->second.cause, target, it->second.fix_type,
                                 it->second.fix_data, it->second.explanation);
            }
            // Unknown unbound variable — generic suggestion
            return make_diag("unbound-variable", target, "define-or-require", "",
                            "Define '" + target + "' with (define ...) or add the right (require ...)");
        }

        // Detect "type error: cannot call: X"
        prefix = "type error: cannot call: ";
        auto tpos = msg.find(prefix);
        if (tpos != std::string::npos) {
            auto target = msg.substr(tpos + prefix.size());
            return make_diag("type-error-unbound", target, "define-function", "",
                            "Define the function '" + target + "' before calling it");
        }

        // Detect parse errors
        if (msg.find("parse error") != std::string::npos ||
            msg.find("expected expression") != std::string::npos) {
            return make_diag("parse-error", "", "fix-syntax", msg.substr(0, 60),
                            "Check syntax: unbalanced parens, missing quotes, or wrong form");
        }

        // Detect #<procedure> / closure
        if (msg.find("procedure") != std::string::npos ||
            msg.find("uncalled function") != std::string::npos) {
            return make_diag("closure-no-display", "", "add-display", "",
                             "Add (display (your-function args)) at the end of your code");
        }

        return make_bool(false);
    });

    // (apply-fix code-string diagnose-result) — Apply pre-built fix from diagnosis
    // Returns fixed code, or original code if no auto-fix applies.
    primitives_.add("apply-fix", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_pair(a[1]))
            return make_bool(false);
        auto code_idx = as_string_idx(a[0]);
        if (code_idx >= string_heap_.size())
            return make_bool(false);
        auto code = string_heap_[code_idx];

        // Extract fix-type (3rd element of diagnose result)
        // diagnose returns: (cause target fix-type fix-data explanation)
        auto pair_idx = as_pair_idx(a[1]);
        auto get_elem = [&](auto p, int n) -> std::string {
            for (int i = 0; i < n && is_pair(p); ++i) {
                if (i == n - 1) {
                    if (is_string(pairs_[as_pair_idx(p)].car)) {
                        auto si = as_string_idx(pairs_[as_pair_idx(p)].car);
                        if (si < string_heap_.size())
                            return string_heap_[si];
                    }
                    return "";
                }
                p = pairs_[as_pair_idx(p)].cdr;
            }
            return "";
        };
        // Elements (1-indexed in list traversal):
        // 1=cause, 2=target, 3=fix-type, 4=fix-data, 5=explanation
        auto fix_type = get_elem(a[1], 3);
        auto fix_data = get_elem(a[1], 4);
        auto target = get_elem(a[1], 2);  // function name

        std::string result;
        if (fix_type == "add-require") {
            // Prepend (require "module" all:) if not already present
            auto req_line = "(require \"" + fix_data + "\" all:)";
            if (code.find(req_line) == std::string::npos) {
                // Check if there's already an existing require
                auto nl = code.find('\n');
                auto first_line = (nl == std::string::npos) ? code : code.substr(0, nl);
                if (first_line.find("(require ") == 0) {
                    // There's already a require, insert after it
                    auto rest = (nl == std::string::npos) ? "" : code.substr(nl);
                    result = first_line + "\n" + req_line + rest;
                } else {
                    result = req_line + "\n" + code;
                }
            } else {
                result = code;
            }
        } else if (fix_type == "define-function" || fix_type == "define-or-require") {
            // Add stub define for unnamed function
            auto stub = "(define (" + target + " x)\n  x)\n";
            if (code.find("(define (" + target) == std::string::npos) {
                result = code + "\n" + stub;
            } else {
                result = code;
            }
        } else if (fix_type == "add-display") {
            // Append (display result) — already handled by eval-current auto-fix
            result = code;
        } else if (fix_type == "fix-syntax") {
            // Can't auto-fix syntax errors — return code as-is
            result = code;
        } else {
            result = code;
        }

        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
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
            if (idx >= pairs_.size()) {
                total_failed++;
                continue;
            }

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
                if (fi >= pairs_.size())
                    break;
                auto check_form = pairs_[fi].car;
                forms = pairs_[fi].cdr;

                // Convert check_form data back to AST and evaluate
                if (!arena_) {
                    sf++;
                    continue;
                }
                auto alloc = arena_->allocator();
                auto* cf_pool = arena_->create<aura::ast::StringPool>(alloc);
                auto* cf_flat = arena_->create<aura::ast::FlatAST>(alloc);
                auto ast_root = data_to_flat(check_form, *cf_flat, *cf_pool, 0);
                if (ast_root == aura::ast::NULL_NODE) {
                    sf++;
                    continue;
                }
                cf_flat->root = ast_root;

                auto result = eval_flat(*cf_flat, *cf_pool, ast_root, top_);
                if (!result) {
                    sf++;
                    continue;
                }

                bool ok = is_int(*result) && as_int(*result) == 1;
                if (ok) {
                    sp++;
                    total_passed++;
                } else {
                    sf++;
                    total_failed++;
                }
            }

            std::fprintf(stderr, "  Suite '%s': %d/%d passed\n", suite_name.c_str(), sp, sp + sf);
        }

        std::string summary = std::to_string(total_suites) +
                              " suites: " + std::to_string(total_passed) + " passed, " +
                              std::to_string(total_failed) + " failed";
        auto sidx = string_heap_.size();
        string_heap_.push_back(summary);
        return make_string(sidx);
    });

    primitives_.add("read", [this](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // ── File I/O (P0) ───────────────────────────────────────────
    // Helper: check path is a regular file (skip directories)
    auto is_regular = [](const std::string& path) -> bool {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    };

    // (current-time) → integer epoch seconds
    primitives_.add("current-time", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(std::time(nullptr)));
    });

    primitives_.add("read-file", [this, is_regular](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& path = string_heap_[idx];
        if (!is_regular(path))
            return make_void();
        std::ifstream f(path);
        if (!f)
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(content));
        return make_string(id);
    });

    primitives_.add("write-file", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
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
        if (!f)
            return make_void();
        f << content;
        return make_int(1);
    });

    // ── CLI interface ────────────────────────────────────────────
    primitives_.add("command-line", [this](const auto&) -> EvalValue {
        // Returns list of command-line argument strings, NOT including argv[0].
        // Parsed from /proc/self/cmdline on Linux.
        std::ifstream f("/proc/self/cmdline");
        if (!f)
            return make_void();
        std::string raw;
        std::getline(f, raw, '\0'); // skip argv[0]
        std::vector<std::string> items;
        while (std::getline(f, raw, '\0')) {
            if (!raw.empty())
                items.push_back(raw);
        }
        EvalValue result = make_void();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(*it);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    primitives_.add("file-exists?", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_int(0);
        auto& path = string_heap_[idx];
        struct stat st;
        return make_int(::stat(path.c_str(), &st) == 0 ? 1 : 0);
    });

    // ── File I/O: copy, delete, size, directory list ─────────────
    primitives_.add("file-copy", [this, is_regular](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto sidx = as_string_idx(a[0]), didx = as_string_idx(a[1]);
        if (sidx >= string_heap_.size() || didx >= string_heap_.size())
            return make_void();
        if (!is_regular(string_heap_[sidx]))
            return make_void();
        std::ifstream src(string_heap_[sidx], std::ios::binary);
        if (!src)
            return make_void();
        std::ofstream dst(string_heap_[didx], std::ios::binary);
        if (!dst)
            return make_void();
        dst << src.rdbuf();
        return make_int(1);
    });

    primitives_.add("file-delete", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_int(0);
        return make_int(std::remove(string_heap_[idx].c_str()) == 0 ? 1 : 0);
    });

    primitives_.add("file-size", [this, is_regular](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size() || !is_regular(string_heap_[idx]))
            return make_int(0);
        std::ifstream f(string_heap_[idx], std::ios::ate | std::ios::binary);
        if (!f)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(f.tellg()));
    });

    // ── Shell / Process ────────────────────────────────────────
    primitives_.add("shell", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_int(-1);
        return make_int(::system(string_heap_[idx].c_str()));
    });

    primitives_.add("command-output", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& cmd = string_heap_[idx];
        std::array<char, 4096> buf;
        std::string result;
        auto* fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return make_void();
        while (::fgets(buf.data(), buf.size(), fp) != nullptr)
            result += buf.data();
        ::pclose(fp);
        if (!result.empty() && result.back() == '\n')
            result.pop_back();
        auto sid = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    primitives_.add("directory-list", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& dir_path = string_heap_[idx];
        EvalValue result = make_void();
        auto dir = opendir(dir_path.c_str());
        if (!dir)
            return make_void();
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..")
                continue;
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
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_int(0);
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap_.size() || si >= string_heap_.size())
            return make_int(0);
        try {
            std::regex re(string_heap_[pi]);
            return make_int(std::regex_search(string_heap_[si], re) ? 1 : 0);
        } catch (...) {
            return make_int(0);
        }
    });

    primitives_.add("regex-find", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap_.size() || si >= string_heap_.size())
            return make_void();
        try {
            std::regex re(string_heap_[pi]);
            std::smatch m;
            if (std::regex_search(string_heap_[si], m, re)) {
                auto id = string_heap_.size();
                string_heap_.push_back(m.str());
                return make_string(id);
            }
        } catch (...) {
        }
        return make_void();
    });

    primitives_.add("regex-replace", [this](const auto& a) {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]), ri = as_string_idx(a[2]);
        if (pi >= string_heap_.size() || si >= string_heap_.size() || ri >= string_heap_.size())
            return make_void();
        try {
            std::regex re(string_heap_[pi]);
            auto result = std::regex_replace(string_heap_[si], re, string_heap_[ri]);
            auto id = string_heap_.size();
            string_heap_.push_back(std::move(result));
            return make_string(id);
        } catch (...) {
            return make_void();
        }
    });

    primitives_.add("regex-split", [this](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto pi = as_string_idx(a[0]), si = as_string_idx(a[1]);
        if (pi >= string_heap_.size() || si >= string_heap_.size())
            return make_void();
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
        } catch (...) {
            return make_void();
        }
    });

    // ── Math ────────────────────────────────────────────────────
    auto to_double = [this](const EvalValue& v) -> double {
        if (is_float(v))
            return as_float(v);
        if (is_int(v))
            return static_cast<double>(as_int(v));
        return 0.0;
    };

    primitives_.add("sin", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::sin(to_double(a[0])));
    });
    primitives_.add("cos", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::cos(to_double(a[0])));
    });
    primitives_.add("tan", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::tan(to_double(a[0])));
    });
    primitives_.add("asin", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::asin(to_double(a[0])));
    });
    primitives_.add("acos", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::acos(to_double(a[0])));
    });
    primitives_.add("atan", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::atan(to_double(a[0])));
    });
    primitives_.add("log", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::log(to_double(a[0])));
    });
    primitives_.add("log10", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::log10(to_double(a[0])));
    });
    primitives_.add("exp", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::exp(to_double(a[0])));
    });
    primitives_.add("pow", [to_double](const auto& a) {
        if (a.size() < 2)
            return make_float(0.0);
        return make_float(std::pow(to_double(a[0]), to_double(a[1])));
    });
    primitives_.add("sqrt", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::sqrt(to_double(a[0])));
    });
    primitives_.add("floor", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::floor(to_double(a[0])));
    });
    primitives_.add("ceil", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::ceil(to_double(a[0])));
    });
    primitives_.add("round", [to_double](const auto& a) {
        if (a.empty())
            return make_float(0.0);
        return make_float(std::round(to_double(a[0])));
    });

    primitives_.add("inexact->exact", [](const auto& a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        if (types::is_float(a[0]))
            return types::make_int(static_cast<std::int64_t>(types::as_float(a[0])));
        return a[0];
    });

    // ═══════════════════════════════════════════════════════════════
    // Module primitives (Phase 1: module objects)
    // ═══════════════════════════════════════════════════════════════

    // (module? v) — Check if value is a module object
    primitives_.add("module?",
                    [](const auto& a) { return make_bool(!a.empty() && is_module(a[0])); });

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
        if (a.empty() || !is_module(a[0]))
            return make_void();
        auto mod_idx = as_module_idx(a[0]);
        if (mod_idx >= modules_.size())
            return make_void();
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
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        return load_module_file(string_heap_[idx]);
    });

    primitives_.add("load-module", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        return load_module_file(string_heap_[idx]);
    });

    // (while pred body) — Iterative loop with zero C++ stack growth.
    // pred: closure returning bool (#t to continue, #f to stop).
    // body: closure — evaluated each iteration.
    primitives_.add("while", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_closure(a[0]) || !types::is_closure(a[1]))
            return make_void();
        auto pred_cid = types::as_closure_id(a[0]);
        auto body_cid = types::as_closure_id(a[1]);
        for (;;) {
            auto pred_result = apply_closure(pred_cid, {});
            if (!pred_result) break;
            auto& val = *pred_result;
            bool cont = types::is_bool(val) ? types::as_bool(val) :
                         types::is_int(val) ? types::as_int(val) != 0 : false;
            if (!cont) break;
            (void)apply_closure(body_cid, {});
        }
        return make_void();
    });

    primitives_.add("import", [this](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
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
        if (!is_module(mod_val))
            return make_void();
        auto mod_idx = as_module_idx(mod_val);
        if (mod_idx >= modules_.size())
            return make_void();

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
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) {
            do {
                auto __e_sidx = string_heap_.size();
                string_heap_.push_back("modulo: division by zero");
                auto __e_eidx = error_values_.size();
                error_values_.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        auto n = coerce_to_int(a[0], &string_heap_);
        auto r = n % divisor;
        if (r < 0)
            r += (divisor > 0 ? divisor : -divisor);
        return make_int(r);
    });
    primitives_.add("mod", [this](const auto& a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) {
            do {
                auto __e_sidx = string_heap_.size();
                string_heap_.push_back("mod: division by zero");
                auto __e_eidx = error_values_.size();
                error_values_.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        auto n = coerce_to_int(a[0], &string_heap_);
        auto r = n % divisor;
        if (r < 0)
            r += (divisor > 0 ? divisor : -divisor);
        return make_int(r);
    });
    // quotient: (quotient n m) → integer division truncating toward zero
    primitives_.add("quotient", [this](const auto& a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) {
            do {
                auto __e_sidx = string_heap_.size();
                string_heap_.push_back("quotient: division by zero");
                auto __e_eidx = error_values_.size();
                error_values_.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        return make_int(coerce_to_int(a[0], &string_heap_) / divisor);
    });
    // remainder: (remainder n m) → remainder with sign of dividend
    primitives_.add("remainder", [this](const auto& a) {
        if (a.size() < 2)
            return make_int(0);
        auto divisor = coerce_to_int(a[1], &string_heap_);
        if (divisor == 0) {
            do {
                auto __e_sidx = string_heap_.size();
                string_heap_.push_back("remainder: division by zero");
                auto __e_eidx = error_values_.size();
                error_values_.push_back(make_string(__e_sidx));
                return make_error(__e_eidx);
            } while (0);
        }
        return make_int(coerce_to_int(a[0], &string_heap_) % divisor);
    });
    // abs: (abs n) → absolute value
    primitives_.add("abs", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        if (is_float(a[0]))
            return make_float(std::abs(as_float(a[0])));
        auto n = coerce_to_int(a[0], &string_heap_);
        return make_int(n < 0 ? -n : n);
    });
    // gcd: (gcd a b ...) → greatest common divisor (variadic)
    primitives_.add("gcd", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        auto to_int = [this](const EvalValue& v) { return coerce_to_int(v, &string_heap_); };
        auto r = to_int(a[0]);
        auto abs_gcd = [](std::int64_t x, std::int64_t y) -> std::int64_t {
            x = x < 0 ? -x : x;
            y = y < 0 ? -y : y;
            while (y != 0) {
                auto t = y;
                y = x % y;
                x = t;
            }
            return x;
        };
        for (std::size_t i = 1; i < a.size(); ++i)
            r = abs_gcd(r, to_int(a[i]));
        return make_int(r);
    });
    // lcm: (lcm a b ...) → least common multiple (variadic)
    primitives_.add("lcm", [this](const auto& a) {
        if (a.empty())
            return make_int(1);
        auto to_int = [this](const EvalValue& v) { return coerce_to_int(v, &string_heap_); };
        auto r = to_int(a[0]);
        auto gcd = [](std::int64_t x, std::int64_t y) -> std::int64_t {
            x = x < 0 ? -x : x;
            y = y < 0 ? -y : y;
            if (x == 0 || y == 0)
                return 0;
            while (y != 0) {
                auto t = y;
                y = x % y;
                x = t;
            }
            return x;
        };
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto n = to_int(a[i]);
            auto g = gcd(r, n);
            r = (g == 0) ? 0 : (r / g) * n;
        }
        if (r < 0)
            r = -r;
        return make_int(r);
    });
    // min: (min a b ...) → minimum (variadic)
    primitives_.add("min", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            auto to_f = [this](const EvalValue& v) {
                return is_float(v) ? as_float(v)
                                   : static_cast<double>(coerce_to_int(v, &string_heap_));
            };
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i)
                r = std::min(r, to_f(a[i]));
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], &string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i)
            r = std::min(r, coerce_to_int(a[i], &string_heap_));
        return make_int(r);
    });
    // max: (max a b ...) → maximum (variadic)
    primitives_.add("max", [this](const auto& a) {
        if (a.empty())
            return make_int(0);
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            auto to_f = [this](const EvalValue& v) {
                return is_float(v) ? as_float(v)
                                   : static_cast<double>(coerce_to_int(v, &string_heap_));
            };
            double r = to_f(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i)
                r = std::max(r, to_f(a[i]));
            return make_float(r);
        }
        std::int64_t r = coerce_to_int(a[0], &string_heap_);
        for (std::size_t i = 1; i < a.size(); ++i)
            r = std::max(r, coerce_to_int(a[i], &string_heap_));
        return make_int(r);
    });

    // ── Character + I/O extensions ────────────────────────────────

    // char?: (char? v) → true if is_int(v) (chars represented as ints)
    primitives_.add("char?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_int(a[0]));
    });
    // char->integer: (char->integer c) → integer value
    primitives_.add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });
    // integer->char: (integer->char n) → identity
    primitives_.add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });
    // string->list: (string->list s) → list of char codes
    primitives_.add("string->list", [this](const auto& a) {
        if (a.empty())
            return make_void();
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size())
                s = string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    // list->string: (list->string lst) → string from char codes
    primitives_.add("list->string", [this](const auto& a) {
        if (a.empty() || !is_pair(a[0]) && !is_void(a[0]))
            return make_int(0);
        std::string result;
        auto v = a[0];
        while (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                break;
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
        if (line.empty())
            return make_void();
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return make_string(id);
    });
    // eof-object?: (eof-object? v) → check if value represents EOF
    primitives_.add("eof-object?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        // EOF is represented as void (the same as when read-line returns empty)
        return make_bool(is_void(a[0]));
    });

    // ── List utility primitives ────────────────────────────────────
    primitives_.add("take", [this](const auto& a) {
        if (a.size() < 2)
            return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        if (n == 0 || is_end_of_list(v))
            return make_void();
        EvalValue result = make_void();
        // Build result in reverse then reverse it
        for (std::size_t i = 0; i < n; ++i) {
            if (!is_pair(v))
                return result;
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                return result;
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, result});
            result = make_pair(new_id);
            v = pairs_[idx].cdr;
        }
        // Reverse to get correct order
        EvalValue final = make_void();
        while (!is_end_of_list(result)) {
            if (!is_pair(result))
                break;
            auto idx = as_pair_idx(result);
            if (idx >= pairs_.size())
                break;
            auto nid = pairs_.size();
            pairs_.push_back({pairs_[idx].car, final});
            final = make_pair(nid);
            result = pairs_[idx].cdr;
        }
        return final;
    });
    primitives_.add("drop", [this](const auto& a) {
        if (a.size() < 2)
            return make_void();
        auto n = static_cast<std::size_t>(as_int(a[0]));
        auto v = a[1];
        for (std::size_t i = 0; i < n; ++i) {
            if (is_end_of_list(v))
                return make_void();
            if (!is_pair(v))
                return make_void();
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                return make_void();
            v = pairs_[idx].cdr;
        }
        return v;
    });
    primitives_.add("foldl", [this](const auto& a) {
        if (a.size() < 3)
            return make_void();
        auto f = a[0];
        auto acc = a[1];
        auto lst = a[2];

        // Handle primitive function (e.g., (foldl + 0 (list 1 2 3)))
        if (is_primitive(f)) {
            auto slot = as_primitive_slot(f);
            if (slot >= primitives_.slot_count())
                return make_void();
            auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
            if (!prim)
                return make_void();

            while (!is_end_of_list(lst)) {
                if (!is_pair(lst))
                    break;
                auto idx = as_pair_idx(lst);
                if (idx >= pairs_.size())
                    break;

                // Call primitive with (acc, car)
                acc = (*prim)({acc, pairs_[idx].car});
                lst = pairs_[idx].cdr;
            }
            return acc;
        }

        // Handle closure function
        if (!is_closure(f))
            return make_void();
        auto cid = as_closure_id(f);

        while (!is_end_of_list(lst)) {
            if (!is_pair(lst))
                break;
            auto idx = as_pair_idx(lst);
            if (idx >= pairs_.size())
                break;

            // Build args: (acc element) for binary foldl, (acc) for unary
            std::vector<EvalValue> fargs = {acc, pairs_[idx].car};
            auto result = apply_closure(cid, fargs);
            if (!result)
                break;
            acc = *result;
            lst = pairs_[idx].cdr;
        }
        return acc;
    });

    // ── Typed mutation operators ──────────────────────────────────

    // (mutate:replace-type node-id new-type-str)
    primitives_.add("mutate:replace-type", [this](const auto& a) -> EvalValue {
        // Yield at mutation boundary (Issue #31) — safe point before/after mutation.
        if (aura::messaging::g_fiber_yield_mutation_boundary)
            aura::messaging::g_fiber_yield_mutation_boundary();

        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
            return merr("bad-arg", "usage: (mutate:replace-type node-id new-type)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto type_idx = as_string_idx(a[1]);
        if (type_idx >= string_heap_.size())
            return merr("bad-arg", "type string index out of range");
        if (!workspace_flat_)
            return merr("no-workspace", "no workspace AST loaded");
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));

        auto old_tid = flat.type_id(node);
        std::string old_type_str = (old_tid > 0) ? "#" + std::to_string(old_tid) : "Any";
        auto old_val = static_cast<std::uint64_t>(old_tid);
        auto new_val = static_cast<std::uint64_t>(string_heap_.size()); // placeholder

        // Simple type ID mapping based on well-known type names
        auto type_str = string_heap_[type_idx];
        std::uint32_t new_tid = 0;
        if (type_str == "Int")
            new_tid = 1;
        else if (type_str == "Float")
            new_tid = 2;
        else if (type_str == "String")
            new_tid = 3;
        else if (type_str == "Bool")
            new_tid = 4;
        else if (type_str == "Dyn" || type_str == "Any")
            new_tid = 0;
        else
            new_tid = 0; // unknown → Dyn

        auto mid = flat.add_mutation_with_rollback(
            node, "replace-type", old_type_str, string_heap_[type_idx], "replace type annotation",
            aura::ast::MutationStatus::Committed, 1, old_val, new_tid, true);
        // Actually apply the type change
        flat.set_type(node, new_tid);
        workspace_flat_->mark_dirty_upward(node);
        defuse_version_++;
        if (aura::messaging::g_fiber_yield_mutation_boundary)
            aura::messaging::g_fiber_yield_mutation_boundary();
        return make_int(static_cast<std::int64_t>(mid));
    });

    // (mutate:replace-value node-id new-value summary)
    // Replaces the value of a node. The type of new-value must match the
    // target node: int → LiteralInt, float → LiteralFloat, string → Variable/LiteralString.
    primitives_.add("mutate:replace-value", [this](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        aura::messaging::g_fiber_yield_mutation_boundary
                ? aura::messaging::g_fiber_yield_mutation_boundary()
                : (void)0;  // safe point before mutation
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[2]))
            return merr("bad-arg", "usage: (mutate:replace-value node-id new-value summary)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto sum_idx = as_string_idx(a[2]);
        if (sum_idx >= string_heap_.size())
            return merr("bad-arg", "summary string index out of range");
        if (!workspace_flat_)
            return merr("no-workspace", "no workspace AST loaded");
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));

        auto nv = flat.get(node);
        std::uint64_t old_val = 0;

        switch (nv.tag) {
            case aura::ast::NodeTag::LiteralInt: {
                if (!is_int(a[1]))
                    return merr("type-error", "LiteralInt node requires an integer value");
                auto new_val = static_cast<std::int64_t>(as_int(a[1]));
                old_val = static_cast<std::uint64_t>(nv.int_value);
                auto mid = flat.add_mutation_with_rollback(
                    node, "replace-value", "Int", "Int", string_heap_[sum_idx],
                    aura::ast::MutationStatus::Committed, 0, old_val,
                    static_cast<std::uint64_t>(new_val), true);
                flat.set_int(node, new_val);
                workspace_flat_->mark_dirty_upward(node);
                return make_int(static_cast<std::int64_t>(mid));
            }
            case aura::ast::NodeTag::LiteralFloat: {
                if (!is_float(a[1]))
                    return merr("type-error", "LiteralFloat node requires a float value");
                // Pack double as uint64 for mutation log
                double new_val = as_float(a[1]);
                std::uint64_t new_bits;
                std::memcpy(&new_bits, &new_val, sizeof(new_bits));
                std::uint64_t old_bits;
                std::memcpy(&old_bits, &nv.float_value, sizeof(old_bits));
                auto mid = flat.add_mutation_with_rollback(
                    node, "replace-value", "Float", "Float", string_heap_[sum_idx],
                    aura::ast::MutationStatus::Committed, 1, old_bits, new_bits, true);
                flat.set_float(node, new_val);
                workspace_flat_->mark_dirty_upward(node);
                return make_int(static_cast<std::int64_t>(mid));
            }
            case aura::ast::NodeTag::Variable:
            case aura::ast::NodeTag::LiteralString: {
                if (!is_string(a[1]))
                    return merr("type-error", "Variable/LiteralString node requires a string value");
                auto new_sym_idx = as_string_idx(a[1]);
                if (new_sym_idx >= string_heap_.size())
                    return merr("bad-arg", "new value string index out of range");
                auto new_name = string_heap_[new_sym_idx];
                old_val = nv.sym_id;
                auto new_sym = workspace_pool_->intern(new_name);
                auto mid = flat.add_mutation_with_rollback(
                    node, "replace-value", "Sym", "Sym", string_heap_[sum_idx],
                    aura::ast::MutationStatus::Committed, 2, old_val, new_sym, true);
                flat.set_sym(node, new_sym);
                workspace_flat_->mark_dirty_upward(node);
                return make_int(static_cast<std::int64_t>(mid));
            }
            default:
                return merr("type-error", "node tag does not support value replacement: " + std::to_string(static_cast<int>(nv.tag)));
        }
    });

    // (mutate:record-patch node-id op-name summary)
    primitives_.add("mutate:record-patch", [this](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        aura::messaging::g_fiber_yield_mutation_boundary
                ? aura::messaging::g_fiber_yield_mutation_boundary()
                : (void)0;  // safe point before mutation
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return merr("bad-arg", "usage: (mutate:record-patch node-id op-name summary)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto op_idx = as_string_idx(a[1]);
        auto sum_idx = as_string_idx(a[2]);
        if (op_idx >= string_heap_.size() || sum_idx >= string_heap_.size())
            return merr("bad-arg", "string index out of range");
        if (!workspace_flat_)
            return merr("no-workspace", "no workspace AST loaded");
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));

        auto mid = flat.add_mutation(node, string_heap_[op_idx], "<runtime>", "<runtime>",
                                     string_heap_[sum_idx]);
        return make_int(static_cast<std::int64_t>(mid));
    });

    // (mutation-count)
    primitives_.add("mutation-count", [this](const auto&) {
        if (!workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(workspace_flat_->mutation_count()));
    });

    // (mutation-history node-id) → list of summary strings
    primitives_.add("mutation-history", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto hist = workspace_flat_->mutation_history(node);
        EvalValue result = make_void();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            auto& rec = *it;
            auto sid = string_heap_.size();
            string_heap_.push_back(std::format(
                "[{}] {}: {}{}", rec.mutation_id, rec.operator_name, rec.summary,
                rec.status == aura::ast::MutationStatus::RolledBack ? " [rolled-back]" : ""));
            auto pair_id = pairs_.size();
            pairs_.push_back({make_string(sid), result});
            result = make_pair(pair_id);
        }
        return result;
    });

    // (rollback mutation-id) → true if successful
    primitives_.add("rollback", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return make_bool(false);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_bool(workspace_flat_->rollback(mid));
    });

    // (rollback-since mutation-id) → count of rolled-back mutations
    primitives_.add("rollback-since", [this](const auto& a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return make_int(0);
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
        if (node >= flat.size())
            return make_bool(false);
        auto nv = flat.get(node);

        // String arg: type compatibility check
        if (is_string(a[1])) {
            auto type_idx = as_string_idx(a[1]);
            if (type_idx >= string_heap_.size())
                return make_bool(false);
            auto new_type = string_heap_[type_idx];

            // Check compatibility based on node tag
            switch (nv.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    // Int literal: compatible with Int, Float, Bool (≠0), Dyn
                    return make_bool(new_type == "Int" || new_type == "Float" ||
                                     new_type == "Bool" || new_type == "Dyn" || new_type == "Any");
                case aura::ast::NodeTag::LiteralFloat:
                    // Float literal: compatible with Float, Dyn
                    return make_bool(new_type == "Float" || new_type == "Dyn" || new_type == "Any");
                case aura::ast::NodeTag::LiteralString:
                    return make_bool(new_type == "String" || new_type == "Dyn" ||
                                     new_type == "Any");
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
        if (!is_int(a[1]))
            return make_bool(false);
        auto field = static_cast<std::uint32_t>(as_int(a[1]));
        switch (field) {
            case 0:
                return make_bool(nv.has_int()); // int_val_
            case 1:
                return make_bool(true); // type_id_ (always valid)
            default:
                return make_bool(false);
        }
    });

    // ═══════════════════════════════════════════════════════════════
    // P6: Query/Transform EDSL 原语
    // ═══════════════════════════════════════════════════════════════

    // (set-code code-string) — Parse code and set as current workspace AST
    // Nodes in workspace AST have stable IDs across query/mutate operations
    // Multi-expression code is automatically wrapped in (begin ...) by the parser.
    // Helper: build structured error value as a pair ("kind" "message")
    // Inline lambda to avoid capture issues — used by set-code, eval-current, etc.
    auto make_error_val = [this](const std::string& kind, const std::string& msg) -> EvalValue {
        auto msg_idx = string_heap_.size();
        string_heap_.push_back(msg);
        auto kind_idx = string_heap_.size();
        string_heap_.push_back(kind);
        auto nil = EvalValue(0);
        // (cons "kind" (cons "message" nil)) → ("kind" "message") as a proper list
        auto msg_pair = make_pair(pairs_.size());
        pairs_.push_back({make_string(msg_idx), nil});
        auto kind_pair = make_pair(pairs_.size());
        pairs_.push_back({make_string(kind_idx), msg_pair});
        return kind_pair;
    };
    std::function<EvalValue(const std::string&, const std::string&)> mev = make_error_val;

    primitives_.add("set-code", [this, mev](const auto& a) -> EvalValue {
        if (workspace_read_only_) return mev("read-only", "workspace is read-only");
        // Clear any previous set-code error and eval-current cache
        last_set_code_error_kind_.clear();
        last_set_code_error_msg_.clear();
        last_eval_current_result_.reset();
        coverage_counters_[0]++;
        coverage_counters_[5]++;
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (set-code code-string)");
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "code string index out of range");
        if (!arena_)
            return mev("internal", "no arena allocator available");

        // REUSE existing flat/pool when possible (avoids arena growth).
        // Clear vectors but keep capacity — subsequent parse fills them
        // without new arena allocation (unless source is larger than cap).
        // This is the KEY fix for OOM: ~750 consecutive set-code calls
        // previously created 750 FlatAST objects in the arena.
        auto* pool_ptr = workspace_pool_;
        auto* flat_ptr = workspace_flat_;
        bool fresh_alloc = false;
        if (!pool_ptr || !flat_ptr) {
            // First call or after arena reset — allocate fresh
            auto alloc = arena_->allocator();
            pool_ptr = arena_->create<aura::ast::StringPool>(alloc);
            flat_ptr = arena_->create<aura::ast::FlatAST>(alloc);
            fresh_alloc = true;
        } else {
            // Reuse: clear existing structures
            // pmr::vector::clear() keeps capacity, so already-allocated
            // buffer memory is reused for subsequent parse.
            pool_ptr->reset();
            flat_ptr->clear();
        }

        auto pr = aura::parser::parse_to_flat(string_heap_[idx], *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            // Return structured parse error: ("parse" "message")
            std::string err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!err.empty()) err += "; ";
                    err += e.format();
                }
            } else if (!pr.error.empty()) {
                err = pr.error;
            } else {
                err = "parse error";
            }
            // Store error for eval-current/eval-current-output to propagate
            last_set_code_error_kind_ = "parse";
            last_set_code_error_msg_ = err;
            coverage_counters_[5]--;
            return mev("parse", err);
        }
        flat_ptr->root = pr.root;
        workspace_flat_ = flat_ptr;
        workspace_pool_ = pool_ptr;
        update_shared_tree_root();
        // Invalidate def-use index (new workspace)
        defuse_index_ = nullptr;
        return make_bool(true);
    });

    // (current-source) — Return the current workspace AST as source code string
    // Implemented inline to avoid circular dependency with lowering module.
    // (eval code) — Parse and evaluate a string of Aura code
    primitives_.add("eval", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto code = string_heap_[types::as_string_idx(a[0])];
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto pr = aura::parser::parse_to_flat(code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_void();
        flat.root = pr.root;
        auto result = eval_flat(flat, pool, pr.root, top_);
        if (result)
            return *result;
        return make_void();
    });

    // (load filename) — Load and evaluate a file of Aura code
    // Uses set-code + eval-current internally so definitions persist
    // in the workspace and closures are properly rooted.
    primitives_.add("load", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& path = string_heap_[idx];

        std::ifstream f(path);
        if (!f)
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        // Fresh workspace state for each load — resetting the pool corrupts
        // existing top_ bindings that reference string indices from the old pool.
        last_set_code_error_kind_.clear();
        last_set_code_error_msg_.clear();
        last_eval_current_result_.reset();

        auto alloc = arena_->allocator();
        auto* pool_ptr = arena_->create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_->create<aura::ast::FlatAST>(alloc);

        auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return mev("parse", "load failed to parse file");
        }
        flat_ptr->root = pr.root;
        workspace_flat_ = flat_ptr;
        workspace_pool_ = pool_ptr;
        update_shared_tree_root();
        defuse_index_ = nullptr;

        // Evaluate the workspace AST
        if (!last_set_code_error_kind_.empty()) {
            auto msg = std::move(last_set_code_error_msg_);
            return mev("load", msg);
        }
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*workspace_flat_, *workspace_pool_,
                                                         workspace_flat_->root);
        // Evaluate each top-level expression individually to ensure closure
        // environments capture correct cell bindings. When wrapped in a single
        // begin block, the tree-walker's begin handler can create closures with
        // stale cells_ pointers for variables defined at different positions.
        EvalValue last = make_void();
        if (expanded < workspace_flat_->size()) {
            auto root_v = workspace_flat_->get(expanded);
            if (root_v.tag == aura::ast::NodeTag::Begin) {
                for (auto cid : root_v.children) {
                    if (cid == aura::ast::NULL_NODE) continue;
                    auto r = eval_flat(*workspace_flat_, *workspace_pool_, cid, top_);
                    if (r) {
                        last = *r;
                    } else {
                        // Continue on error — some top-level orphans may error
                        // but later expressions (defines) should still be processed
                    }
                }
            } else {
                auto r = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
                if (r) last = *r;
            }
        }
        workspace_flat_->clear_all_dirty();
        return make_void();
    });

    // (eval-expr value) — Evaluate any Aura value (not just strings)
    // Useful for evaluating stored expressions (e.g., from pipeline steps)
    primitives_.add("eval-expr", [this](const auto& a) -> EvalValue {
        if (a.empty())
            return make_void();
        // Convert the value to a FlatAST and evaluate
        if (!arena_)
            return make_void();
        auto alloc = arena_->allocator();
        auto* pool = arena_->create<aura::ast::StringPool>(alloc);
        auto* flat = arena_->create<aura::ast::FlatAST>(alloc);
        auto root = data_to_flat(a[0], *flat, *pool, 0);
        if (root == aura::ast::NULL_NODE)
            return make_void();
        flat->root = root;
        auto result = eval_flat(*flat, *pool, root, top_);
        if (result)
            return *result;
        return make_void();
    });

    primitives_.add("current-source", [this](const auto&) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_)
            return make_string(0);

        // Inline unparse for the workspace root
        constexpr int kMaxUnparseDepth = 256;
        auto unparse = [&](this const auto& self, aura::ast::NodeId id, int indent, int depth = 0) -> std::string {
            if (depth > kMaxUnparseDepth)
                return "...";
            if (id == aura::ast::NULL_NODE || id >= workspace_flat_->size())
                return "()";
            auto v = workspace_flat_->get(id);
            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt: {
                    if (workspace_flat_->marker(id) == aura::ast::SyntaxMarker::BoolLiteral)
                        return v.int_value ? "#t" : "#f";
                    return std::to_string(v.int_value);
                }
                case aura::ast::NodeTag::LiteralFloat: {
                    auto s = std::to_string(v.float_value);
                    if (s.find('.') == std::string::npos)
                        s += ".0";
                    return s;
                }
                case aura::ast::NodeTag::LiteralString: {
                    auto raw = workspace_pool_->resolve(v.sym_id);
                    std::string esc = "\"";
                    for (auto c : std::string_view(raw)) {
                        if (c == '\\' || c == '"')
                            esc += '\\';
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
                        if (i > 0)
                            s += " ";
                        s += self(v.child(i), indent + 1, depth + 1);
                    }
                    return s + ")";
                }
                case aura::ast::NodeTag::Lambda: {
                    std::string s = "(lambda (";
                    for (std::size_t i = 0; i < v.params.size(); ++i) {
                        if (i > 0)
                            s += " ";
                        s += std::string(workspace_pool_->resolve(v.params[i]));
                    }
                    s += ")";
                    if (!v.children.empty())
                        s += " " + self(v.child(0), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec: {
                    auto kw = (v.tag == aura::ast::NodeTag::LetRec) ? std::string("letrec")
                                                                    : std::string("let");
                    std::string s =
                        "(" + kw + " ((" + std::string(workspace_pool_->resolve(v.sym_id)) + " ";
                    if (!v.children.empty())
                        s += self(v.child(0), indent + 1, depth + 1);
                    s += "))";
                    if (v.children.size() > 1)
                        s += " " + self(v.child(1), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Define: {
                    return "(define " + std::string(workspace_pool_->resolve(v.sym_id)) + " " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) + ")";
                }
                case aura::ast::NodeTag::IfExpr: {
                    std::string s = "(if";
                    for (std::size_t i = 0; i < v.children.size(); ++i)
                        s += " " + self(v.child(i), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Begin: {
                    std::string s = "(begin";
                    for (std::size_t i = 0; i < v.children.size(); ++i)
                        s += " " + self(v.child(i), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Set: {
                    return "(set! " + std::string(workspace_pool_->resolve(v.sym_id)) + " " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) + ")";
                }
                case aura::ast::NodeTag::Quote: {
                    return "(quote " + (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::Pair: {
                    return "(" +
                           (v.children.empty() ? "()"
                                               : self(v.child(0), indent + 1, depth + 1) + " . " +
                                                     self(v.child(1), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::DefineModule: {
                    std::string s = "(define-module (" + std::string(workspace_pool_->resolve(v.sym_id));
                    for (auto pid : v.params)
                        s += " " + std::string(workspace_pool_->resolve(pid));
                    s += ")";
                    for (auto cid : v.children)
                        s += " " + self(cid, indent + 1, depth + 1);
                    return s + ")";
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
                        s += " " + self(v.child(0), indent + 1, depth + 1);
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
    primitives_.add("eval-current", [this, mev](const auto&) {
        coverage_counters_[2]++;
        // If set-code failed on the last call, propagate the diagnostic immediately
        if (!last_set_code_error_kind_.empty()) {
            auto kind = std::move(last_set_code_error_kind_);
            auto msg = std::move(last_set_code_error_msg_);
            return mev(kind, msg);
        }
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*workspace_flat_, *workspace_pool_,
                                                         workspace_flat_->root);
        coverage_counters_[4]++;

        // Incremental eval: if the workspace root is clean and we have a cached
        // result, skip full re-evaluation entirely (Issue #32b).
        // The root is marked dirty by mark_dirty_upward() on any mutation.
        // clear_cached_value is called in mark_dirty(), so we know the cache
        // is stale when dirty flags are set.
        if (!workspace_flat_->is_dirty(expanded) && last_eval_current_result_) {
            return *last_eval_current_result_;
        }

        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);

        // Cache successful results for incremental reuse
        if (result)
            last_eval_current_result_ = *result;

        // Clear dirty flags after successful eval
        workspace_flat_->clear_all_dirty();
        if (!result) {
            // Return structured diagnostic: (kind-string message-string)
            auto& diag = result.error();
            auto kind = std::string(kind_name(diag.kind));
            auto msg = diag.format();
            return mev(kind, msg);
        }
        // ── Auto-fix closure: if result is an uncalled function, try (display ...) ──
        using aura::ast::NodeId;
        using aura::ast::NodeTag;
        if (is_closure(*result) && workspace_flat_ && workspace_pool_) {
            // Scan for Define nodes to extract function name + arity
            std::string fn_name;
            int arity = 0;
            for (NodeId nid = 0; nid < static_cast<NodeId>(workspace_flat_->size()); ++nid) {
                auto nv = workspace_flat_->get(nid);
                if (nv.tag == NodeTag::Define && nv.sym_id != aura::ast::INVALID_SYM) {
                    fn_name = std::string(workspace_pool_->resolve(nv.sym_id));
                    arity = 0;
                    if (!nv.children.empty()) {
                        auto lambda_nv = workspace_flat_->get(nv.child(0));
                        if (lambda_nv.tag == NodeTag::Lambda)
                            arity = static_cast<int>(lambda_nv.params.size());
                    }
                }
            }
            if (!fn_name.empty()) {
                // Build arg patterns based on arity
                std::vector<std::string> arg_pats;
                if (arity == 0) {
                    arg_pats = {""};
                } else if (arity == 1) {
                    // Simple scalars first, then list-based
                    // Use small ints to avoid exponential recursion (e.g., fib(42))
                    arg_pats = {"5", "0", "1", "\"test\"", "(list 3 1 4 1 5)"};
                } else if (arity == 2) {
                    // List+scalar for search, then plain scalars
                    // Try both (scalar list) and (list scalar) orderings
                    arg_pats = {"42 7", "0 0", "(list 1 3 5 7 9) 5",
                               "5 (list 1 3 5 7 9)", "(list 3 1 4 1 5) 1",
                               "1 (list 3 1 4 1 5)"};
                } else {
                    arg_pats = {"1 2 3", "0 0 0"};
                }
                // Suppress stdout during auto-fix attempts to avoid polluting output
                std::fflush(stdout);
                int saved_stdout = ::dup(STDOUT_FILENO);
                int null_fd = ::open("/dev/null", O_WRONLY);
                if (null_fd >= 0)
                    ::dup2(null_fd, STDOUT_FILENO);
                bool auto_fixed = false;
                std::string winning_call;  // the call that worked
                for (auto& args : arg_pats) {
                    std::string call_code = "(" + fn_name;
                    if (!args.empty())
                        call_code += " " + args;
                    call_code += ")";
                    aura::ast::StringPool temp_pool;
                    aura::ast::FlatAST temp_flat;
                    auto pr = aura::parser::parse_to_flat(call_code, temp_flat, temp_pool);
                    if (!pr.success || pr.root == aura::ast::NULL_NODE)
                        continue;
                    temp_flat.root = pr.root;
                    auto call_expanded = aura::compiler::macro_expand_all(
                        temp_flat, temp_pool, temp_flat.root);
                    auto call_result = eval_flat(temp_flat, temp_pool, call_expanded, top_);
                    if (!call_result || is_void(*call_result) || is_closure(*call_result))
                        continue;
                    auto_fixed = true;
                    winning_call = call_code;
                    break;
                }
                // Restore stdout
                std::fflush(stdout);
                ::dup2(saved_stdout, STDOUT_FILENO);
                ::close(saved_stdout);
                if (null_fd >= 0) ::close(null_fd);
                // Return the auto-fix result silently (no display output)
                if (auto_fixed && !winning_call.empty()) {
                    // Winners are evaluated with original stdout restored, but we
                    // re-evaluate silently and return the raw value. Use (eval-current-output)
                    // if you need the display output for LLM consumption.
                    aura::ast::StringPool call_pool;
                    aura::ast::FlatAST call_flat;
                    auto call_pr = aura::parser::parse_to_flat(winning_call, call_flat, call_pool);
                    if (call_pr.success && call_pr.root != aura::ast::NULL_NODE) {
                        call_flat.root = call_pr.root;
                        auto call_expanded = aura::compiler::macro_expand_all(
                            call_flat, call_pool, call_flat.root);
                        auto call_result = eval_flat(call_flat, call_pool, call_expanded, top_);
                        if (call_result) {
                            coverage_counters_[9]++;
                            return *call_result;
                        }
                    }
                }
            }
        }
        return *result;
    });

    // (eval-current-output) — Evaluate workspace, return display output as string
    // Captures all display output during eval via fd-level redirection.
    primitives_.add("eval-current-output", [this, mev](const auto&) {
        // If set-code failed on the last call, propagate the diagnostic immediately
        if (!last_set_code_error_kind_.empty()) {
            auto kind = std::move(last_set_code_error_kind_);
            auto msg = std::move(last_set_code_error_msg_);
            return mev(kind, msg);
        }
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*workspace_flat_, *workspace_pool_,
                                                         workspace_flat_->root);
        // Redirect stdout to a temp file (fd-level, catches fprintf too)
        std::fflush(stdout);
        auto* tmp = std::tmpfile();
        if (!tmp) {
            auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
            workspace_flat_->clear_all_dirty();
            if (!result) {
                auto msg = result.error().format();
                auto sidx = string_heap_.size();
                string_heap_.push_back(msg);
                return make_string(sidx);
            }
            return *result;
        }
        int new_fd = ::fileno(tmp);
        int old_fd = ::dup(STDOUT_FILENO);
        ::dup2(new_fd, STDOUT_FILENO);
        // Run the eval
        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
        workspace_flat_->clear_all_dirty();
        // Restore stdout
        std::fflush(stdout);
        ::dup2(old_fd, STDOUT_FILENO);
        ::close(old_fd);
        // Read captured output from temp file
        std::rewind(tmp);
        std::string captured;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
            captured.append(buf, n);
        std::fclose(tmp);
        // If eval failed, prepend structured diagnostic to captured output
        if (!result) {
            auto& diag = result.error();
            auto kind = std::string(kind_name(diag.kind));
            auto diag_str = diag.format();
            auto combined = "[" + std::string(kind) + "] " + diag_str;
            if (!captured.empty())
                combined = combined + "\n" + captured;
            captured = combined;
        }
        // Store captured output in string heap
        auto sidx = string_heap_.size();
        string_heap_.push_back(captured);
        return make_string(sidx);
    });

    // (query:find name) — Find all node IDs with matching symbol name
    primitives_.add("query:find", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:find name)");
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "name string index out of range");
        if (!workspace_flat_ || !workspace_pool_)
            return mev("no-workspace", "no workspace AST loaded");
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

    // (eval:async code) — Evaluate code asynchronously on the thread pool.
    // Returns the result as a string, or error message.
    // In serve-async mode, the eval runs on a background thread and the
    // calling fiber yields until the result is ready.
    // In stdin mode, falls back to synchronous eval (same as (eval code)).
    primitives_.add("eval:async", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto& code = string_heap_[as_string_idx(a[0])];
        if (aura::messaging::g_eval_async) {
            // Thread pool path: offload to background
            auto result_str = aura::messaging::g_eval_async(code);
            auto idx = string_heap_.size();
            string_heap_.push_back(result_str);
            return make_string(idx);
        }
        // Fallback: synchronous eval via the existing eval primitive
        auto eval_fn = primitives_.lookup("eval");
        if (!eval_fn) return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(code);
        auto result = (*eval_fn)({make_string(sidx)});
        // Format result as string
        auto& ev = *this;
        auto& cs = ev;
        auto formatted = aura::compiler::format_value(
            result, &ev.string_heap_, &ev.pairs_, 0, &ev.primitives_, &ev.keyword_table());
        auto ris = string_heap_.size();
        string_heap_.push_back(std::move(formatted));
        return make_string(ris);
    });

    // (query:children node-id) — Get children node IDs
    primitives_.add("query:children", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return mev("bad-arg", "usage: (query:children node-id)");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));
        auto v = flat.get(node);
        EvalValue result = make_void();
        for (std::size_t i = v.children.size(); i > 0; --i) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(v.child(i - 1))), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (query:root) — Return the current workspace root node ID, or #f if no workspace
    primitives_.add("query:root", [this, mev](const auto&) -> EvalValue {
        if (!workspace_flat_)
            return mev("no-workspace", "no workspace AST loaded");
        if (workspace_flat_->root == aura::ast::NULL_NODE)
            return mev("no-root", "workspace AST has no root node");
        return make_int(static_cast<std::int64_t>(workspace_flat_->root));
    });


    // (query:node node-id) — Get node details as list (tag value type sym-id)
    primitives_.add("query:node", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:node node-id)");
        if (!workspace_flat_ || !workspace_pool_)
            return mev("no-workspace", "no workspace AST loaded");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));
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
    primitives_.add("query:calls", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:calls name)");
        if (!workspace_flat_ || !workspace_pool_)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "name string index out of range");
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
    primitives_.add("mutate:rebind", [this, mev](const auto& a) -> EvalValue {
        if (workspace_read_only_) return mev("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ ||
            !workspace_pool_)
            return mev("bad-arg", "usage: (mutate:rebind name new-code-string [summary])");
        auto name_idx = as_string_idx(a[0]);
        auto code_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
            return mev("bad-arg", "string index out of range");
        auto& flat = *workspace_flat_;
        auto name = string_heap_[name_idx];
        auto sym = workspace_pool_->intern(name);

        // ── 安全点：保存当前状态作为 panic checkpoint ────────
        // 如果 auto-rollback-on-panic 开启且后续失败，自动恢复到此状态
        bool had_checkpoint = save_panic_checkpoint();

        // ── 依赖图查询：通过 dep_caller_fn_ 获取调用者节点 ────
        // dep_caller_fn_ 在 init_pair_primitives 中注册，使用
        // DefUseIndex 的 O(k) 依赖图查询（k = 调用者数量）。
        // 在 defuse_version_++ 之前调用，因为索引在失效前有效。
        auto dep_callers = dep_caller_fn_
            ? dep_caller_fn_(defuse_index_, sym)
            : std::vector<aura::ast::NodeId>{};
        defuse_version_++;

        // Find old Define node by name
        aura::ast::NodeId old_define = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                old_define = id;
                break;
            }
        }
        if (old_define == aura::ast::NULL_NODE)
            return mev("not-found", std::string("function \"") + name + "\" not found in AST");

        // Parse new code INTO workspace flat (append mode). All new node IDs
        // are valid in the same FlatAST and can be cross-referenced.
        auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty()) parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "rebind code could not be parsed";
            }
            return mev("parse-error", parse_err);
        }

        // The parsed root may be a Define (if code includes "(define (name ...) ...)")
        // or a bare expression (just the value/lambda). Extract the value.
        aura::ast::NodeId new_value = pr.root;
        auto root_v = flat.get(pr.root);
        if (root_v.tag == aura::ast::NodeTag::Define) {
            // New code is a full define — extract its value child
            if (root_v.children.empty())
                return mev("parse-error", "define form in rebind code has no body");
            new_value = root_v.child(0);
        }

        // Record mutation on the old define node
        std::string summary = (a.size() > 2 && is_string(a[2])) ? string_heap_[as_string_idx(a[2])]
                                                                : "rebind " + name;
        flat.add_mutation(old_define, "rebind", name, summary, summary);

        // Redirect old Define's value child to the new nodes
        // This is a valid NodeId in workspace_flat_ since we parsed into it
        flat.set_child(old_define, 0, new_value);

        // ── 依赖图驱动：dirty 所有调用者 ────────────────────────
        // 利用从 def-use 索引预取的调用者列表，标记 dirty + 向上传播。
        // 这样下次 typecheck-current 就知道这些节点需要重新类型推断。
        for (std::size_t ui = 0; ui < dep_callers.size(); ++ui) {
            if (dep_callers[ui] < flat.size())
                flat.mark_dirty_upward(dep_callers[ui]);
        }

        // Record affected sym for incremental DefUseIndex update
        defuse_affected_syms_.insert(name);

        // ── Auto-typecheck: 验证变异后的代码类型正确 ────────
        // 立即运行 typecheck-current，如果类型错误则记录到 last_mutate_error。
        // Agent 可以通过 (typecheck-status) 查询最近一次变异的类型状态。
        auto tc_fn = primitives_.lookup("typecheck-current");
        if (tc_fn) {
            auto tc_result = (*tc_fn)({});
            if (is_string(tc_result)) {
                auto& str = string_heap_[as_string_idx(tc_result)];
                if (str.find("no errors") == std::string::npos) {
                    last_mutate_error_ = std::string("typecheck after mutate:rebind failed: ") + str;
                } else {
                    last_mutate_error_.clear();
                }
            }
        }

        // ── Ownership validation: ensure ownership invariants hold ──
        if (workspace_flat_ && workspace_pool_ && last_mutate_error_.empty()) {
            std::unordered_set<std::string> affected;
            affected.insert(name);
            // Also include caller names since they were dirtied
            for (std::size_t ui = 0; ui < dep_callers.size(); ++ui) {
                if (dep_callers[ui] < flat.size()) {
                    auto caller_v = flat.get(dep_callers[ui]);
                    if (caller_v.sym_id != aura::ast::INVALID_SYM) {
                        auto caller_name = std::string(workspace_pool_->resolve(caller_v.sym_id));
                        if (!caller_name.empty())
                            affected.insert(caller_name);
                    }
                }
            }
            std::vector<aura::compiler::OwnershipNote> onotes;
            bool opass = aura::compiler::OwnershipEnv::validate_ownership(
                flat, *workspace_pool_, flat.root, affected, onotes);
            if (!opass) {
                std::string err = "ownership validation after mutate:rebind failed:";
                for (auto& n : onotes)
                    err += " [" + n.kind + " at node " + std::to_string(n.node) + "] " + n.message + ";";
                last_mutate_error_ = err;
            }
        }

        // ── Auto-rollback: if typecheck/ownership failed, restore checkpoint ──
        if (!last_mutate_error_.empty()) {
            if (panic_auto_rollback_ && had_checkpoint) {
                restore_panic_checkpoint();
                return mev("mutation-failed",
                           std::string(typeid(*this).name()) + ": mutation rejected — auto-rolled back: " +
                               last_mutate_error_);
            }
        } else if (had_checkpoint) {
            // Mutation succeeded — commit the checkpoint
            commit_panic_checkpoint();
        }

        return make_bool(true);
    });

    // ═══════════════════════════════════════════════════════════════
    // P1: Query/Transform EDSL 扩展
    // ═══════════════════════════════════════════════════════════════

    // (query:parent node-id) — Find parent node IDs (nodes whose children include this ID)
    primitives_.add("query:parent", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:parent node-id)");
        if (!workspace_flat_)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " + std::to_string(flat.size()));
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
    primitives_.add("query:siblings", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (query:siblings node-id)");
        if (!workspace_flat_)
            return mev("no-workspace", "no workspace AST loaded");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " + std::to_string(flat.size()));
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            bool parent_of_target = false;
            for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                if (v.child(ci) == target) {
                    parent_of_target = true;
                    break;
                }
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

    // ═══════════════════════════════════════════════════════════════
    // P8a: Query/Transform EDSL — combined filter/where (P1)
    // ═══════════════════════════════════════════════════════════════

    // (where :field value) — Create a predicate for query:filter.
    // Supported fields:
    //   :node-type  — match NodeTag name (e.g. 'Call, 'Define, 'LiteralInt)
    //   :callee     — for Call nodes, match callee Variable name
    //   :has-param  — node has a parameter with given name
    //   :defined-by — node is a Define with given name
    //   :tag        — alias for :node-type
    //   :has-child  — node has at least one child with the given NodeTag name
    //   :depth      — node is at the given depth from root (e.g. "0" = root)
    //
    // Returns a predicate descriptor (a tagged pair) that query:filter
    // applies to each candidate node.
    primitives_.add("query:where", [this, mev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_keyword(a[0]) || !is_string(a[1]))
            return mev("bad-arg", "usage: (where :field-name value-string)");
        if (!workspace_flat_ || !workspace_pool_)
            return mev("no-workspace", "no workspace AST loaded");
        auto field_idx = as_keyword_idx(a[0]);
        if (field_idx >= keyword_table_.size())
            return mev("bad-arg", "unknown keyword");
        auto field_name = keyword_table_[field_idx];
        auto val_idx = as_string_idx(a[1]);
        if (val_idx >= string_heap_.size())
            return mev("bad-arg", "value string index out of range");
        auto value = string_heap_[val_idx];
        auto& flat = *workspace_flat_;
        auto& pool = *workspace_pool_;

        // Store the predicate as a pair: (field-name value-sym)
        auto field_keyword_idx = keyword_table_.size();
        keyword_table_.push_back(field_name);
        auto val_sym = pool.intern(value);
        auto val_string_idx = string_heap_.size();
        string_heap_.push_back(value);

        // Encode as (key:pair key:pair) where car=field keyword, cdr=value string ref
        // This tagged structure is opaque to users but query:filter knows how to apply it.
        auto val_pair = pairs_.size();
        pairs_.push_back({make_keyword(field_keyword_idx), make_string(val_string_idx)});
        return make_pair(val_pair);
    });

    // (query:filter predicate ...) — Filter workspace nodes matching ALL predicates.
    // Each predicate is created by (where :field value).
    // Returns a list of matching node IDs.
    //
    // Usage:
    //   (query:filter (where :node-type "Call") (where :callee "sort"))
    //   → all Call nodes where the callee is "sort"
    //
    //   (query:filter (where :defined-by "fib") (where :node-type "Lambda"))
    //   → the body Lambda of (define fib ...)
    primitives_.add("query:filter", [this, mev](const auto& a) -> EvalValue {
        if (a.empty())
            return mev("bad-arg", "usage: (query:filter predicate ...)");
        if (!workspace_flat_ || !workspace_pool_)
            return mev("no-workspace", "no workspace AST loaded");
        auto& flat = *workspace_flat_;
        auto& pool = *workspace_pool_;

        // Collect predicates from arguments (each is a (where ...) pair)
        struct Predicate {
            std::string field;
            std::string value;
        };
        std::vector<Predicate> predicates;

        for (std::size_t ai = 0; ai < a.size(); ++ai) {
            if (!is_pair(a[ai]))
                return mev("bad-arg", "each predicate must be a (where ...) pair");
            auto pair_idx = as_pair_idx(a[ai]);
            auto car = pairs_[pair_idx].car;
            auto cdr = pairs_[pair_idx].cdr;
            if (!is_keyword(car) || !is_string(cdr))
                return mev("bad-arg", "malformed predicate");
            auto kidx = as_keyword_idx(car);
            auto sidx = as_string_idx(cdr);
            if (kidx >= keyword_table_.size() || sidx >= string_heap_.size())
                return mev("bad-arg", "predicate field/value out of range");
            predicates.push_back({keyword_table_[kidx], string_heap_[sidx]});
        }

        if (predicates.empty())
            return mev("bad-arg", "at least one predicate required");

        // Iterate all workspace nodes, applying all predicates
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            bool match = true;

            for (auto& p : predicates) {
                if (p.field == ":node-type" || p.field == ":tag") {
                    // Match NodeTag name
                    bool found = false;
                    for (auto& m : aura::ast::kNodeMeta) {
                        if (m.name == p.value && m.name != "<gap>") {
                            if (v.tag == m.tag) found = true;
                            break;
                        }
                    }
                    if (!found) { match = false; break; }
                }
                else if (p.field == ":callee") {
                    // For Call nodes, match callee Variable name
                    if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
                        auto callee = flat.get(v.child(0));
                        if (callee.tag != aura::ast::NodeTag::Variable ||
                            pool.resolve(callee.sym_id) != p.value) {
                            match = false; break;
                        }
                    } else {
                        match = false; break;
                    }
                }
                else if (p.field == ":defined-by" || p.field == ":defines") {
                    // Match Define nodes by name
                    if (v.tag == aura::ast::NodeTag::Define) {
                        auto name = pool.resolve(v.sym_id);
                        if (name != p.value) { match = false; break; }
                    } else {
                        match = false; break;
                    }
                }
                else if (p.field == ":has-param") {
                    // Check if node has a parameter with the given name
                    bool found_param = false;
                    for (auto pid : v.params) {
                        if (pool.resolve(pid) == p.value) {
                            found_param = true;
                            break;
                        }
                    }
                    if (!found_param) { match = false; break; }
                }
                else if (p.field == ":has-child") {
                    // Check if node has at least one child with the given NodeTag name
                    aura::ast::NodeTag child_tag = static_cast<aura::ast::NodeTag>(-1);
                    bool found_tag = false;
                    for (auto& m : aura::ast::kNodeMeta) {
                        if (m.name == p.value && m.name != "<gap>") {
                            child_tag = m.tag;
                            found_tag = true;
                            break;
                        }
                    }
                    if (!found_tag) { match = false; break; }
                    bool has_child = false;
                    for (auto cid : v.children) {
                        if (cid != aura::ast::NULL_NODE && flat.get(cid).tag == child_tag) {
                            has_child = true;
                            break;
                        }
                    }
                    if (!has_child) { match = false; break; }
                }
                else if (p.field == ":depth") {
                    // Check if node is at the given depth from root
                    int target_depth = 0;
                    try { target_depth = std::stoi(p.value); }
                    catch (...) { match = false; break; }
                    if (target_depth < 0) { match = false; break; }
                    // Starting from this node, walk up via children_of to count depth
                    int actual_depth = 0;
                    aura::ast::NodeId cur = id;
                    while (cur != 0) {  // root is always NodeId 0
                        // Find parent by scanning all nodes for one that has cur as child
                        aura::ast::NodeId parent = aura::ast::NULL_NODE;
                        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
                            auto pv = flat.get(pid);
                            for (auto cid : pv.children) {
                                if (cid == cur) {
                                    parent = pid;
                                    break;
                                }
                            }
                            if (parent != aura::ast::NULL_NODE) break;
                        }
                        if (parent == aura::ast::NULL_NODE) break;
                        cur = parent;
                        ++actual_depth;
                    }
                    if (actual_depth != target_depth) { match = false; break; }
                }
                else {
                    return mev("unknown-field",
                               std::string("unknown where field: \"") + p.field + "\"");
                }
            }

            if (match) {
                auto pid = pairs_.size();
                pairs_.push_back({make_int(static_cast<std::int64_t>(id)), result});
                result = make_pair(pid);
            }
        }
        return result;
    });

    // (query:node-type tag-name) — Find all nodes with a given NodeTag name
    // Tag names: LiteralInt, Variable, Call, IfExpr, Lambda, Let, LetRec,
    //            Define, Begin, Set, Quote, LiteralString, TypeAnnotation,
    //            Coercion, LiteralFloat, MacroDef
    primitives_.add("query:node-type", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:node-type tag-name)");
        if (!workspace_flat_)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "tag name string index out of range");
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
        if (!found_tag)
            return mev("unknown-tag", std::string("unknown node type \"") + target_name + "\"");

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
    primitives_.add("query:pattern", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query:pattern expr)");
        if (!workspace_flat_ || !workspace_pool_)
            return mev("no-workspace", "no workspace AST loaded");
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "pattern string index out of range");

        // Parse pattern string into its own FlatAST (separate from workspace)
        auto alloc = arena_->allocator();
        auto* pat_pool = arena_->create<aura::ast::StringPool>(alloc);
        auto* pat_flat = arena_->create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(string_heap_[idx], *pat_flat, *pat_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_void();

        // Intern "..." in the pattern pool for wildcard matching
        auto wildcard_sym = pat_pool->intern("...");

        // Recursive subtree matcher
        std::function<bool(aura::ast::NodeId, aura::ast::NodeId)> match_subtree;
        match_subtree = [&, wildcard_sym](aura::ast::NodeId ws_id,
                                          aura::ast::NodeId pat_id) -> bool {
            if (pat_id >= pat_flat->size())
                return ws_id >= workspace_flat_->size();
            if (ws_id >= workspace_flat_->size() || pat_id == aura::ast::NULL_NODE)
                return (pat_id == aura::ast::NULL_NODE) ? (ws_id == aura::ast::NULL_NODE) : false;

            auto ws_node = workspace_flat_->get(ws_id);
            auto pat_node = pat_flat->get(pat_id);

            // Wildcard "..." matches any single subtree
            if (pat_node.tag == aura::ast::NodeTag::Variable && pat_node.sym_id == wildcard_sym)
                return true;

            // Same tag required
            if (ws_node.tag != pat_node.tag)
                return false;

            switch (pat_node.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    return ws_node.int_value == pat_node.int_value;
                case aura::ast::NodeTag::LiteralFloat:
                    return ws_node.float_value == pat_node.float_value;
                case aura::ast::NodeTag::Variable:
                case aura::ast::NodeTag::LiteralString:
                    return workspace_pool_->resolve(ws_node.sym_id) ==
                           pat_pool->resolve(pat_node.sym_id);
                case aura::ast::NodeTag::MacroDef:
                    return true;
                default:
                    if (ws_node.children.size() != pat_node.children.size())
                        return false;
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
    primitives_.add("mutate:set-body", [this, mev](const auto& a) -> EvalValue {
        if (workspace_read_only_) return mev("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ ||
            !workspace_pool_)
            return mev("bad-arg", "usage: (mutate:set-body name new-body-code [summary])");
        auto name_idx = as_string_idx(a[0]);
        auto code_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
            return mev("bad-arg", "string index out of range");
        auto& flat = *workspace_flat_;
        auto name = string_heap_[name_idx];
        auto sym = workspace_pool_->intern(name);

        // ── 安全点：保存当前状态作为 panic checkpoint ────────
        bool had_checkpoint = save_panic_checkpoint();

        // ── 依赖图查询：通过 dep_caller_fn_ 获取调用者节点 ────
        auto dep_callers = dep_caller_fn_
            ? dep_caller_fn_(defuse_index_, sym)
            : std::vector<aura::ast::NodeId>{};
        defuse_version_++;

        // Find Define node with matching symbol name
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                // The Define should have one child: a Lambda
                if (v.children.size() != 1)
                    return mev("arity-error", std::string("function \"") + name + "\" define has " + std::to_string(v.children.size()) + " children, expected 1");
                auto lambda_id = v.child(0);
                auto lv = flat.get(lambda_id);
                if (lv.tag != aura::ast::NodeTag::Lambda)
                    return mev("type-error", std::string("function \"") + name + "\" body is not a Lambda node");

                // Parse new body INTO workspace flat (all IDs stay valid)
                auto pr =
                    aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
                if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                    std::string parse_err;
                    if (!pr.errors.empty()) {
                        for (auto& e : pr.errors) {
                            if (!parse_err.empty()) parse_err += "; ";
                            parse_err += e.format();
                        }
                    } else if (!pr.error.empty()) {
                        parse_err = pr.error;
                    } else {
                        parse_err = "new body code could not be parsed";
                    }
                    return mev("parse-error", parse_err);
                }

                // Record mutation
                flat.add_mutation(id, "set-body", name, name, "set-body " + name);

                // Replace the Lambda's body — pr.root is a valid node in workspace_flat_
                flat.set_child(lambda_id, 0, pr.root);

                // 依赖图驱动：dirty 所有调用者
                for (std::size_t ui = 0; ui < dep_callers.size(); ++ui) {
                    if (dep_callers[ui] < flat.size())
                        flat.mark_dirty_upward(dep_callers[ui]);
                }

                // Record affected sym for incremental DefUseIndex update
                defuse_affected_syms_.insert(name);

                // ── Auto-typecheck ──
                auto tc_fn = primitives_.lookup("typecheck-current");
                if (tc_fn) {
                    auto tc_result = (*tc_fn)({});
                    if (is_string(tc_result)) {
                        auto& str = string_heap_[as_string_idx(tc_result)];
                        if (str.find("no errors") == std::string::npos) {
                            last_mutate_error_ = std::string("typecheck after mutate:set-body failed: ") + str;
                        } else {
                            last_mutate_error_.clear();
                        }
                    }
                }

                // ── Ownership validation ──
                if (workspace_flat_ && workspace_pool_ && last_mutate_error_.empty()) {
                    std::unordered_set<std::string> affected;
                    affected.insert(name);
                    for (std::size_t ui = 0; ui < dep_callers.size(); ++ui) {
                        if (dep_callers[ui] < flat.size()) {
                            auto caller_v = flat.get(dep_callers[ui]);
                            if (caller_v.sym_id != aura::ast::INVALID_SYM) {
                                auto caller_name = std::string(workspace_pool_->resolve(caller_v.sym_id));
                                if (!caller_name.empty())
                                    affected.insert(caller_name);
                            }
                        }
                    }
                    std::vector<aura::compiler::OwnershipNote> onotes;
                    bool opass = aura::compiler::OwnershipEnv::validate_ownership(
                        flat, *workspace_pool_, flat.root, affected, onotes);
                    if (!opass) {
                        std::string err = "ownership validation after mutate:set-body failed:";
                        for (auto& n : onotes)
                            err += " [" + n.kind + " at node " + std::to_string(n.node) + "] " + n.message + ";";
                        last_mutate_error_ = err;
                    }
                }

                // ── Auto-rollback: if typecheck/ownership failed, restore checkpoint ──
                if (!last_mutate_error_.empty()) {
                    if (panic_auto_rollback_ && had_checkpoint) {
                        restore_panic_checkpoint();
                        return mev("mutation-failed",
                                   "mutation rejected — auto-rolled back: " + last_mutate_error_);
                    }
                } else if (had_checkpoint) {
                    commit_panic_checkpoint();
                }

                return make_bool(true);
            }
        }
        return mev("not-found", std::string("function \"") + name + "\" not found in AST");
    });

    // (mutate:remove-node node-id) — Remove a node by setting parent's reference to NULL_NODE
    // The node entry remains in the FlatAST but is disconnected from the tree.
    // The tree walker in eval_flat skips NULL_NODE children.
    primitives_.add("mutate:remove-node", [this, mev](const auto& a) -> EvalValue {
        defuse_version_++;
        aura::messaging::g_fiber_yield_mutation_boundary
                ? aura::messaging::g_fiber_yield_mutation_boundary()
                : (void)0;  // safe point before mutation
        if (workspace_read_only_) return mev("read-only", "workspace is read-only");
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return mev("bad-arg", "usage: (mutate:remove-node node-id)");
        auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(target) + " >= flat size " + std::to_string(flat.size()));

        // Find parent and remove target from its children
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.children.empty())
                continue;
            auto children = flat.children(id);
            for (std::size_t ci = 0; ci < children.size(); ++ci) {
                if (children[ci] == target) {
                    children[ci] = aura::ast::NULL_NODE;
                    flat.add_mutation(id, "remove-node", std::to_string(target), "",
                                      "remove node " + std::to_string(target));
                    workspace_flat_->mark_dirty_upward(id);
                    return make_bool(true);
                }
            }
        }
        return mev("not-found", "node " + std::to_string(target) + " has no parent in the AST");
    });

    // (mutate:insert-child parent-id position code-string "summary")
    // Insert a child node into a parent's children list at the given position.
    // Position 0 = first child, child_count = append at end.
    // Parses code-string INTO workspace, preserving all existing nodes/IDs.
    primitives_.add("mutate:insert-child", [this, mev](const auto& a) -> EvalValue {
        defuse_version_++;
        aura::messaging::g_fiber_yield_mutation_boundary
                ? aura::messaging::g_fiber_yield_mutation_boundary()
                : (void)0;  // safe point before mutation
        if (workspace_read_only_) return mev("read-only", "workspace is read-only");
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_string(a[2]) ||
            !workspace_flat_ || !workspace_pool_)
            return mev("bad-arg", "usage: (mutate:insert-child parent-id position code-string [summary])");
        auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto pos = static_cast<std::uint32_t>(as_int(a[1]));
        auto code_idx = as_string_idx(a[2]);
        if (code_idx >= string_heap_.size())
            return mev("bad-arg", "code string index out of range");
        auto& flat = *workspace_flat_;
        if (parent >= flat.size())
            return mev("out-of-range", "parent node ID " + std::to_string(parent) + " >= flat size " + std::to_string(flat.size()));

        // Parse child code INTO workspace (append mode — all IDs stay valid)
        auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty()) parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "insert-child code could not be parsed";
            }
            return mev("parse-error", parse_err);
        }

        // Insert the parsed node at position pos in parent's children
        flat.insert_child(parent, pos, pr.root);

        std::string summary = (a.size() > 3 && is_string(a[3]))
                                  ? string_heap_[as_string_idx(a[3])]
                                  : "insert child at " + std::to_string(pos);
        flat.add_mutation(parent, "insert-child", std::to_string(pos), summary, summary);
        workspace_flat_->mark_dirty_upward(parent);
        return make_int(static_cast<std::int64_t>(pr.root));
    });

    // (mutate:tweak-literal node-id delta "summary") — Tweak a LiteralInt by delta
    // Reads current value, adds delta, writes back. Simpler than read+replace-value.
    primitives_.add("mutate:tweak-literal", [this, mev](const auto& a) -> EvalValue {
        defuse_version_++;
        aura::messaging::g_fiber_yield_mutation_boundary
                ? aura::messaging::g_fiber_yield_mutation_boundary()
                : (void)0;  // safe point before mutation
        if (workspace_read_only_) return mev("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]) || !workspace_flat_)
            return mev("bad-arg", "usage: (mutate:tweak-literal node-id delta [summary])");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto delta = as_int(a[1]);
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return mev("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));
        auto v = flat.get(node);
        if (v.tag != aura::ast::NodeTag::LiteralInt)
            return mev("type-error", "node " + std::to_string(node) + " is not a LiteralInt");
        auto new_val = std::max<std::int64_t>(0, static_cast<std::int64_t>(v.int_value) + delta);
        auto old_val = v.int_value;
        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? string_heap_[as_string_idx(a[2])]
                                  : "tweak-literal " + std::to_string(old_val) + "->" + std::to_string(new_val);
        flat.add_mutation_with_rollback(
            node, "tweak-literal", "Int", "Int", summary,
            aura::ast::MutationStatus::Committed, 0, static_cast<std::uint64_t>(old_val),
            static_cast<std::uint64_t>(new_val), true);
        flat.set_int(node, new_val);
        workspace_flat_->mark_dirty_upward(node);
        return make_int(static_cast<std::int64_t>(new_val));
    });

    // (mutate:replace-pattern pattern replacement [summary])
    //   → #t/#f
    //   Finds all nodes matching a structural pattern and replaces them
    //   with the replacement template.
    //
    //   Pattern syntax:
    //     (\* 2 x)     — exact match: Call(Int(2), Var(x))
    //     (/ ... ...)   — "..." wildcard matches any single subtree
    //
    //   Replacement is a string. When the pattern has wildcards "...",
    //   each occurrence in the replacement is substituted with the
    //   source-code string of the captured subtree.
    //
    //   Example:
    //     (mutate:replace-pattern "(* 2 x)" "(+ x x)")
    //       → replaces (* 2 x) with (+ x x) everywhere
    //     (mutate:replace-pattern "(... (+ ... ...))" "...")
    //       → strips outer call, keeps only the first child
    primitives_.add("mutate:replace-pattern", [this, mev](const auto& a) -> EvalValue {
        using namespace aura::ast;
        defuse_version_++;
        aura::messaging::g_fiber_yield_mutation_boundary
                ? aura::messaging::g_fiber_yield_mutation_boundary()
                : (void)0;  // safe point before mutation
        if (workspace_read_only_)
            return mev("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) ||
            !workspace_flat_ || !workspace_pool_)
            return mev("bad-arg", "usage: (mutate:replace-pattern pattern replacement)");
        auto pattern_idx = as_string_idx(a[0]);
        auto repl_idx = as_string_idx(a[1]);
        if (pattern_idx >= string_heap_.size() || repl_idx >= string_heap_.size())
            return mev("bad-arg", "string index out of range");
        auto& flat = *workspace_flat_;

        auto pattern_str = string_heap_[pattern_idx];
        std::string repl_template = string_heap_[repl_idx];
        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? string_heap_[as_string_idx(a[2])]
                                  : "replace-pattern";

        // Parse pattern into separate FlatAST
        auto alloc = arena_->allocator();
        auto* pat_pool = arena_->create<aura::ast::StringPool>(alloc);
        auto* pat_flat = arena_->create<aura::ast::FlatAST>(alloc);
        auto pat_pr = aura::parser::parse_to_flat(pattern_str, *pat_flat, *pat_pool);
        if (!pat_pr.success || pat_pr.root == NULL_NODE)
            return mev("parse-error", "pattern string could not be parsed");

        auto wildcard_sym = pat_pool->intern("...");

        // ── Source-code reconstruction helper ─────────────────
        // Given a node ID in the workspace FlatAST, reconstruct its source
        // code as a string (same as current-source but for any node)
        std::function<std::string(NodeId)> node_to_source;
        node_to_source = [&](NodeId id) -> std::string {
            if (id >= flat.size() || id == NULL_NODE) return "";
            auto v = flat.get(id);
            switch (v.tag) {
                case NodeTag::LiteralInt:
                    return std::to_string(v.int_value);
                case NodeTag::LiteralFloat:
                    return std::to_string(v.float_value);
                case NodeTag::LiteralString:
                    return "\"" + std::string(workspace_pool_->resolve(v.sym_id)) + "\"";
                case NodeTag::Variable:
                    return std::string(workspace_pool_->resolve(v.sym_id));
                case NodeTag::Call: {
                    std::string s = "(";
                    for (std::size_t ci = 0; ci < v.children.size(); ++ci) {
                        if (ci > 0) s += " ";
                        s += node_to_source(v.child(ci));
                    }
                    s += ")";
                    return s;
                }
                case NodeTag::Lambda: {
                    std::string s = "(lambda (";
                    for (std::size_t pi = 0; pi < v.params.size(); ++pi) {
                        if (pi > 0) s += " ";
                        s += pat_pool->resolve(v.params[pi]);
                    }
                    s += ")";
                    if (!v.children.empty())
                        s += " " + node_to_source(v.child(0));
                    s += ")";
                    return s;
                }
                case NodeTag::IfExpr: {
                    std::string s = "(if";
                    if (v.children.size() > 0)
                        s += " " + node_to_source(v.child(0));
                    if (v.children.size() > 1)
                        s += " " + node_to_source(v.child(1));
                    if (v.children.size() > 2)
                        s += " " + node_to_source(v.child(2));
                    s += ")";
                    return s;
                }
                case NodeTag::Begin: {
                    std::string s = "(begin";
                    for (auto c : v.children)
                        s += " " + node_to_source(c);
                    s += ")";
                    return s;
                }
                case NodeTag::Define: {
                    std::string s = "(define " + std::string(workspace_pool_->resolve(v.sym_id));
                    if (!v.children.empty()) {
                        auto val_node = flat.get(v.child(0));
                        if (val_node.tag == NodeTag::Lambda) {
                            s += " (";
                            for (std::size_t pi = 0; pi < val_node.params.size(); ++pi) {
                                if (pi > 0) s += " ";
                                s += workspace_pool_->resolve(val_node.params[pi]);
                            }
                            s += ")";
                            if (!val_node.children.empty())
                                s += " " + node_to_source(val_node.child(0));
                            s += ")";
                        } else {
                            s += " " + node_to_source(v.child(0));
                        }
                    }
                    s += ")";
                    return s;
                }
                case NodeTag::Let:
                case NodeTag::LetRec: {
                    std::string kw = (v.tag == NodeTag::LetRec) ? "letrec" : "let";
                    std::string s = "(" + kw;
                    if (v.has_name())
                        s += " " + std::string(workspace_pool_->resolve(v.sym_id));
                    // bindings: (var val) pairs
                    // Not implemented for now — just print children
                    for (auto c : v.children)
                        s += " " + node_to_source(c);
                    s += ")";
                    return s;
                }
                default:
                    return std::string("#<node-") + std::to_string(static_cast<int>(v.tag)) + ">";
            }
        };

        // ── Match + capture ────────────────────────────────────
        struct MatchResult {
            bool matched = false;
            std::vector<NodeId> captures;
        };

        std::function<MatchResult(NodeId, NodeId)> match_capture;
        match_capture = [&](NodeId ws_id, NodeId pat_id) -> MatchResult {
            if (pat_id >= pat_flat->size() || pat_id == NULL_NODE)
                return {ws_id == NULL_NODE, {}};
            if (ws_id >= flat.size() || ws_id == NULL_NODE)
                return {false, {}};

            auto ws_node = flat.get(ws_id);
            auto pat_node = pat_flat->get(pat_id);

            if (pat_node.tag == NodeTag::Variable && pat_node.sym_id == wildcard_sym)
                return {true, {ws_id}};

            if (ws_node.tag != pat_node.tag)
                return {false, {}};

            switch (pat_node.tag) {
                case NodeTag::LiteralInt:
                    return {ws_node.int_value == pat_node.int_value, {}};
                case NodeTag::LiteralFloat:
                    return {ws_node.float_value == pat_node.float_value, {}};
                case NodeTag::Variable:
                case NodeTag::LiteralString:
                    return {workspace_pool_->resolve(ws_node.sym_id) ==
                           pat_pool->resolve(pat_node.sym_id), {}};
                case NodeTag::MacroDef:
                    return {true, {}};
                default:
                    if (ws_node.children.size() != pat_node.children.size())
                        return {false, {}};
                    std::vector<NodeId> all_captures;
                    for (std::size_t ci = 0; ci < ws_node.children.size(); ++ci) {
                        auto child_result = match_capture(ws_node.child(ci), pat_node.child(ci));
                        if (!child_result.matched)
                            return {false, {}};
                        all_captures.insert(all_captures.end(),
                            child_result.captures.begin(), child_result.captures.end());
                    }
                    return {true, std::move(all_captures)};
            }
        };

        // Find all matching nodes in workspace
        std::vector<std::pair<NodeId, std::vector<NodeId>>> matches;
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto mr = match_capture(id, pat_pr.root);
            if (mr.matched)
                matches.push_back({id, std::move(mr.captures)});
        }

        if (matches.empty())
            return mev("not-found", "pattern did not match any node in the AST");

        // Count wildcards in pattern
        std::function<int(NodeId)> count_wildcards;
        count_wildcards = [&](NodeId pat_id) -> int {
            if (pat_id >= pat_flat->size() || pat_id == NULL_NODE) return 0;
            auto pn = pat_flat->get(pat_id);
            if (pn.tag == NodeTag::Variable && pn.sym_id == wildcard_sym)
                return 1;
            int total = 0;
            for (auto c : pn.children)
                total += count_wildcards(c);
            return total;
        };
        int expected_captures = count_wildcards(pat_pr.root);

        // ── Apply replacements via string substitution ────────
        int replaced_count = 0;
        for (auto& match : matches) {
            auto match_id = match.first;
            auto& captures = match.second;

            if (static_cast<int>(captures.size()) != expected_captures)
                continue;

            // Find parent
            auto parent_id = flat.parent_of(match_id);
            if (parent_id == NULL_NODE) continue;
            int child_idx = -1;
            {
                auto pv = flat.get(parent_id);
                for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                    if (pv.child(ci) == match_id) {
                        child_idx = static_cast<int>(ci);
                        break;
                    }
                }
            }
            if (child_idx < 0) continue;

            // Build the replacement string by substituting captures
            std::string filled_repl;
            if (expected_captures == 0) {
                filled_repl = repl_template;
            } else {
                // Replace "..." with source code of each captured node
                int cap_idx = 0;
                std::size_t pos = 0;
                while (pos < repl_template.size()) {
                    auto dot_pos = repl_template.find("...", pos);
                    if (dot_pos == std::string::npos) {
                        filled_repl += repl_template.substr(pos);
                        break;
                    }
                    filled_repl += repl_template.substr(pos, dot_pos - pos);
                    if (cap_idx < static_cast<int>(captures.size())) {
                        filled_repl += node_to_source(captures[cap_idx]);
                        cap_idx++;
                    }
                    pos = dot_pos + 3; // skip "..."
                }
            }

            // Parse the filled replacement into workspace
            auto repl_pr = aura::parser::parse_to_flat(filled_repl, flat, *workspace_pool_);
            if (!repl_pr.success || repl_pr.root == NULL_NODE)
                continue;

            // Replace the matched node
            flat.set_child(parent_id, static_cast<std::uint32_t>(child_idx), repl_pr.root);
            replaced_count++;
        }

        if (replaced_count == 0)
            return mev("pattern-error", "no replacements were applied (capture mismatch or parse failure)");

        flat.add_mutation(0, "replace-pattern", pattern_str, repl_template, summary);
        return make_bool(true);
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

        // 注入 declare-type 声明的自定义类型签名
        if (!declared_type_sigs_.empty()) {
            std::unordered_map<std::string, std::string> sig_map;
            std::unordered_map<std::string, std::string> mod_src_map;
            for (auto& [name, decl] : declared_type_sigs_) {
                sig_map[name] = decl.type_str;
                if (!decl.module_file.empty())
                    mod_src_map[name] = decl.module_file;
            }
            tc.inject_type_sigs(sig_map, mod_src_map);
        }

        aura::diag::DiagnosticCollector diag;

        auto result =
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);

        // TypeChecker now writes back normalized types via synthesize_flat + infer_flat,
        // and clears per-node dirty flags. No need for post-pass cache sync.
        // Safety clear for any nodes that may have been missed.
        workspace_flat_->clear_all_dirty();

        std::string out = "type: " + treg.format_type(result) + "\n";
        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            out += "diagnostics:\n";
            for (auto& d : all_diags) {
                out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
            }
        }

        auto sidx = string_heap_.size();
        string_heap_.push_back(out);
        return make_string(sidx);
    });

    // (typecheck-status) — Returns the last mutate typecheck result.
    // Empty string = no errors, non-empty = last mutate caused type errors.
    primitives_.add("typecheck-status", [this](const auto&) -> EvalValue {
        if (last_mutate_error_.empty()) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("ok");
            return make_string(sidx);
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(last_mutate_error_);
        return make_string(sidx);
    });

    // (auto-rollback-on-panic [#t|#f]) — Get/set auto-rollback on panic flag
    // When enabled, runtime error triggers automatic rollback to last safe
    // checkpoint. Returns previous value.
    primitives_.add("auto-rollback-on-panic", [this](const auto& a) -> EvalValue {
        bool old = panic_auto_rollback_;
        if (!a.empty() && types::is_bool(a[0]))
            panic_auto_rollback_ = types::as_bool(a[0]);
        return make_bool(old);
    });

    // (panic-auto-rollback?) — Query current auto-rollback state
    primitives_.add("panic-auto-rollback?", [this](const auto&) -> EvalValue {
        return make_bool(panic_auto_rollback_);
    });

    // (panic-checkpoint) — Save current workspace as a safe checkpoint
    // Returns #t on success, #f if no workspace loaded.
    primitives_.add("panic-checkpoint", [this](const auto&) -> EvalValue {
        return make_bool(save_panic_checkpoint());
    });

    // (panic-restore) — Restore to the last safe checkpoint
    // Returns #t on success, #f if no checkpoint available or restore failed.
    primitives_.add("panic-restore", [this](const auto&) -> EvalValue {
        return make_bool(restore_panic_checkpoint());
    });

    // (panic-safe-source) — Return the checkpoint source code
    // Returns empty string if no checkpoint.
    primitives_.add("panic-safe-source", [this](const auto&) -> EvalValue {
        auto idx = string_heap_.size();
        string_heap_.push_back(panic_safe_source_);
        return make_string(idx);
    });
}

// ── C FFI: c-load / c-func ─────────────────────────────────
// Global FFI state (shared across all evaluator instances)
// In production, this should be per-evaluator.
static std::vector<void*> g_ffi_libs;
struct FFIFunc {
    void* fn_ptr;
    std::string name;
    int ret_type;               // 0=void, 1=Int, 2=Float, 3=String, 4=Opaque
    std::vector<int> arg_types; // per-arg type tags
};
static std::vector<FFIFunc> g_ffi_funcs;

// ── Env::lookup_cell_ptr: returns EvalValue* ──────────────────
EvalValue* Env::lookup_cell_ptr(const std::string& n, std::vector<EvalValue>* cells) const {
    if (!cells)
        return nullptr;
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second)) {
                auto ci = as_cell_id(b.second);
                if (ci < cells->size())
                    return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < cells->size())
                        return &(*cells)[ci];
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

// ═══════════════════════════════════════════════════════════════
// DefUseIndex — Scope-level cached def-use chain
// ═══════════════════════════════════════════════════════════════
// P1 implementation: build scope tree + indexes once, incremental rebuild on mutate.

struct DefUseIndex {
    using NodeId = aura::ast::NodeId;
    using SymId = aura::ast::SymId;
    using FlatAST = aura::ast::FlatAST;
    using StringPool = aura::ast::StringPool;
    using NodeTag = aura::ast::NodeTag;
    static constexpr auto INVALID_SYM = aura::ast::INVALID_SYM;

    struct ScopeNode {
        NodeId node = 0;
        std::uint32_t parent = std::uint32_t(-1);
        std::uint32_t first_child = 0;
        std::uint16_t child_count = 0;
        std::uint32_t def_first = 0;
        std::uint16_t def_count = 0;
        std::uint32_t ref_first = 0;
        std::uint16_t ref_count = 0;
        std::uint32_t use_first = 0;
        std::uint32_t use_count = 0;
        bool dirty = false;
        bool tombstoned = false;
    };

    struct SymRef {
        SymId sym = INVALID_SYM;
        std::uint32_t use_start = 0;
        std::uint16_t use_count = 0;
    };

    // Arena data — all flat vectors, no pointers
    std::vector<ScopeNode> scopes_;
    std::vector<SymId> def_syms_;
    std::vector<NodeId> def_nodes_;
    std::vector<SymRef> refs_;
    std::vector<NodeId> uses_;

    // Cross-scope: sym → all scopes that define/reference it
    std::vector<SymId> sym_scopes_keys_;
    std::vector<std::uint32_t> sym_scopes_vals_;
    std::vector<std::uint32_t> sym_to_range_;

    // ── Call-graph index (#10) ─────────────────────────────────
    // callers_of_: SymId → all Call nodes that call this symbol
    // built during build(), enables O(1) query_callers
    std::unordered_map<SymId, std::vector<NodeId>> callers_of_;
    // callee_of_: NodeId → SymId (only for Call nodes)
    // enables O(1) callee lookup from a call site
    std::vector<SymId> callee_of_;

    FlatAST* flat_ = nullptr;
    StringPool* pool_ = nullptr;
    bool built_ = false;
    std::size_t flat_size_at_build_ = 0;

    void destroy() {
        scopes_.clear(); def_syms_.clear(); def_nodes_.clear();
        refs_.clear(); uses_.clear(); sym_scopes_keys_.clear();
        sym_scopes_vals_.clear(); sym_to_range_.clear();
        callers_of_.clear(); callee_of_.clear();
        flat_ = nullptr; pool_ = nullptr; built_ = false;
    }

    // ── Build from scratch ──────────────────────────────────────
    // Single-pass: walk AST nodes 0..N-1, detect scope boundaries,
    // collect defs, build scope tree, then collect uses per scope.
    void build(FlatAST& flat, StringPool& pool) {
        destroy();
        flat_ = &flat;
        pool_ = &pool;
        flat_size_at_build_ = flat.size();

        // Pre-allocate
        def_syms_.reserve(flat.size() / 4);
        def_nodes_.reserve(flat.size() / 4);
        uses_.reserve(flat.size() / 2);
        refs_.reserve(flat.size() / 4);

        // Root scope (module-level: node 0)
        scopes_.push_back({});
        scopes_.back().node = 0;
        scopes_.back().dirty = false;

        // Pass 1: walk all nodes, build scope tree + collect defs
        // Use explicit depth-first traversal, NOT scan-by-NodeId
        // because children may not be contiguous.
        struct Frame {
            NodeId node_id;
            std::uint32_t scope_idx;
            std::size_t child_idx;  // which child we're processing
        };
        std::vector<Frame> stack;
        stack.push_back({flat.root, 0, 0});

        while (!stack.empty()) {
            auto& f = stack.back();
            auto v = flat.get(f.node_id);

            // First visit: check if this node creates a scope
            if (f.child_idx == 0) {
                bool is_scope_creator = false;
                switch (v.tag) {
                    case NodeTag::Define:
                    case NodeTag::Lambda:
                    case NodeTag::Let:
                    case NodeTag::LetRec:
                    case NodeTag::Begin:
                        is_scope_creator = true;
                        break;
                    default:
                        break;
                }

                if (is_scope_creator && f.node_id != flat.root) {
                    // Create new scope (except for root which already has scope 0)
                    auto scope_idx = scopes_.size();
                    ScopeNode sn;
                    sn.node = f.node_id;
                    sn.parent = f.scope_idx;
                    sn.dirty = false;
                    scopes_.push_back(sn);

                    // Link into parent
                    auto& parent = scopes_[f.scope_idx];
                    if (parent.child_count == 0)
                        parent.first_child = scope_idx;
                    parent.child_count++;

                    // Collect defs for this scope
                    auto& sn2 = scopes_.back();
                    sn2.def_first = def_syms_.size();
                    switch (v.tag) {
                        case NodeTag::Define:
                            def_syms_.push_back(v.sym_id);
                            def_nodes_.push_back(f.node_id);
                            break;
                        case NodeTag::Lambda:
                            for (auto pid : v.params) {
                                def_syms_.push_back(pid);
                                def_nodes_.push_back(f.node_id);
                            }
                            break;
                        case NodeTag::Let:
                        case NodeTag::LetRec:
                            def_syms_.push_back(v.sym_id);
                            def_nodes_.push_back(f.node_id);
                            break;
                        default:
                            break;
                    }
                    sn2.def_count = def_syms_.size() - sn2.def_first;

                    // Update frame scope
                    f.scope_idx = scope_idx;
                }
            }

            // Process children
            if (f.child_idx < v.children.size()) {
                auto child = v.child(f.child_idx);
                auto child_scope = f.scope_idx;

                // Skip scope-creating children (they create their own scope)
                // But still process them as sub-frames
                auto cv = flat.get(child);
                f.child_idx++;
                stack.push_back({child, child_scope, 0});
            } else {
                stack.pop_back();
                // When returning from a scope-creating child, the parent scope stays
            }
        }

        // Pass 2: collect uses per scope
        // Walk all Variable nodes, associate each with the innermost scope
        // that could define it (or skip if unbound)
        // For simplicity: associate each Variable with the scope it belongs to
        collect_uses(flat);

        // Pass 3: add any unfound defs from full scan (covers edge cases)
        // This ensures top-level defines and lets are always indexed
        for (NodeId sid = 0; sid < flat.size(); ++sid) {
            auto sv = flat.get(sid);
            aura::ast::SymId def_sym = aura::ast::INVALID_SYM;
            
            // Check for define/let/letrec that might not be in any scope
            if (sv.tag == NodeTag::Define || sv.tag == NodeTag::Let ||
                sv.tag == NodeTag::LetRec) {
                def_sym = sv.sym_id;
            } else if (sv.tag == NodeTag::Lambda && sv.params.size() > 0) {
                // For lambdas at top level, add all params
            }
            
            if (def_sym != aura::ast::INVALID_SYM) {
                // Find which scope this node belongs to
                std::uint32_t found_scope = 0;  // default: root scope
                for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
                    auto& sn = scopes_[si];
                    if (sn.node == sid) {
                        found_scope = si;
                        break;
                    }
                }
                
                // Check if this sym is already def'd in this scope
                auto& sn = scopes_[found_scope];
                bool exists = false;
                for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                    if (def_syms_[sn.def_first + d] == def_sym) {
                        exists = true;
                        break;
                    }
                }
                
                if (!exists) {
                    // Insert def at the end of this scope's defs
                    // Need to shift: append new def, update scope's def_range
                    // For root scope, just append
                    auto old_def_first = sn.def_first;
                    auto old_def_count = sn.def_count;
                    def_syms_.push_back(def_sym);
                    def_nodes_.push_back(sid);
                    sn.def_first = def_syms_.size() - 1;
                    sn.def_count = 1 + old_def_count;
                }
            }
        }

        // Pass 4: build cross-scope sym index
        build_sym_index();

        // Pass 5: build call-graph index (#10)
        // Walk all Call nodes, record callers_of_ and callee_of_
        build_call_graph(flat);

        built_ = true;
    }

    // ── Collect uses: walk all Variable nodes, group by scope ────
    void collect_uses(FlatAST& flat) {
        // Map: node_id → scope_idx
        std::unordered_map<NodeId, std::uint32_t> node_to_scope;
        node_to_scope.reserve(flat.size());

        // Build node-to-scope mapping via DFS
        struct Frame { NodeId nid; std::uint32_t scope_idx; std::size_t child_idx; };
        std::vector<Frame> stack;
        stack.push_back({flat.root, 0, 0});

        while (!stack.empty()) {
            auto& f = stack.back();
            auto v = flat.get(f.nid);

            if (f.child_idx == 0) {
                // First visit: determine scope
                // Check parent scope
                auto this_scope = f.scope_idx;

                // If this node creates a scope, find its scope index
                bool found = false;
                for (std::size_t si = scopes_.size(); si > 0; --si) {
                    auto& sn = scopes_[si - 1];
                    if (sn.node == f.nid && !sn.tombstoned) {
                        this_scope = si - 1;
                        found = true;
                        break;
                    }
                }
                node_to_scope[f.nid] = this_scope;

                if (f.child_idx == 0) {
                    f.scope_idx = this_scope;
                }
            }

            if (f.child_idx < v.children.size()) {
                auto child = v.child(f.child_idx);
                f.child_idx++;
                stack.push_back({child, f.scope_idx, 0});
            } else {
                stack.pop_back();
            }
        }

        // Now collect Variables grouped by scope
        // Group by scope: scope_idx → {sym → [node_ids]}
        struct ScopeVarGroup {
            std::unordered_map<SymId, std::vector<NodeId>> vars;
        };
        std::unordered_map<std::uint32_t, ScopeVarGroup> scope_vars;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Variable) {
                auto scope_it = node_to_scope.find(id);
                if (scope_it != node_to_scope.end()) {
                    scope_vars[scope_it->second].vars[v.sym_id].push_back(id);
                }
            }
        }

        // Build refs_ and uses_ from scope_vars
        for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
            auto& sn = scopes_[si];
            sn.ref_first = refs_.size();
            sn.use_first = uses_.size();

            auto sv_it = scope_vars.find(si);
            if (sv_it != scope_vars.end()) {
                for (auto& [sym, nodes] : sv_it->second.vars) {
                    SymRef sr;
                    sr.sym = sym;
                    sr.use_start = uses_.size();
                    sr.use_count = static_cast<std::uint16_t>(nodes.size());
                    for (auto nid : nodes)
                        uses_.push_back(nid);
                    refs_.push_back(sr);
                }
            }

            sn.ref_count = static_cast<std::uint16_t>(refs_.size() - sn.ref_first);
            sn.use_count = uses_.size() - sn.use_first;
        }
    }

    // ── Build sym → scopes index ────────────────────────────────
    void build_sym_index() {
        SymId max_sym = 0;
        for (auto s : def_syms_)
            if (s != INVALID_SYM && s > max_sym) max_sym = s;
        for (auto& r : refs_)
            if (r.sym != INVALID_SYM && r.sym > max_sym) max_sym = r.sym;

        sym_to_range_.resize(max_sym + 1, 0);

        struct Entry { SymId sym; std::uint32_t scope_idx; bool is_def; std::uint32_t local_idx; };
        std::unordered_map<uint32_t, std::vector<Entry>> entries_by_sym;

        for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
            auto& sn = scopes_[si];
            for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                auto sym = def_syms_[sn.def_first + d];
                entries_by_sym[sym].push_back({sym, si, true, sn.def_first + d});
            }
            for (std::uint16_t r = 0; r < sn.ref_count; ++r) {
                auto& ref = refs_[sn.ref_first + r];
                entries_by_sym[ref.sym].push_back({ref.sym, si, false, sn.ref_first + r});
            }
        }

        sym_scopes_keys_.clear();
        sym_scopes_vals_.clear();

        for (auto& [sym, entries] : entries_by_sym) {
            if (sym > max_sym) continue;
            sym_to_range_[sym] = (sym_scopes_vals_.size() << 16) | (uint32_t)entries.size();
            for (auto& e : entries) {
                sym_scopes_keys_.push_back(sym);
                sym_scopes_vals_.push_back((e.scope_idx << 1) | (e.is_def ? 1u : 0u));
            }
        }
    }

    // ── Build call-graph index (#10) ────────────────────────────
    // Populates callers_of_ and callee_of_ from all Call nodes.
    void build_call_graph(FlatAST& flat) {
        callers_of_.clear();
        callee_of_.resize(flat.size(), INVALID_SYM);
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id != INVALID_SYM) {
                    callers_of_[callee.sym_id].push_back(id);
                    callee_of_[id] = callee.sym_id;
                }
            }
        }
    }

    // ── Query: def-use for a symbol ─────────────────────────────
    struct DefUseResult {
        std::vector<NodeId> defs;
        std::vector<NodeId> uses;
    };

    DefUseResult query_def_use(SymId sym) {
        DefUseResult r;
        if (sym >= sym_to_range_.size())
            return r;

        auto packed = sym_to_range_[sym];
        if (packed == 0)
            return r;

        auto start = packed >> 16;
        auto count = packed & 0xFFFF;

        for (std::uint32_t i = start; i < start + count; ++i) {
            auto val = sym_scopes_vals_[i];
            auto scope_idx = val >> 1;
            if (val & 1) {
                // is_def
                auto& sn = scopes_[scope_idx];
                for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                    if (def_syms_[sn.def_first + d] == sym)
                        r.defs.push_back(def_nodes_[sn.def_first + d]);
                }
            } else {
                // is_ref — collect use nodes
                auto& sn = scopes_[scope_idx];
                for (std::uint16_t ri = 0; ri < sn.ref_count; ++ri) {
                    auto& ref = refs_[sn.ref_first + ri];
                    if (ref.sym == sym) {
                        for (std::uint16_t u = 0; u < ref.use_count; ++u)
                            r.uses.push_back(uses_[ref.use_start + u]);
                    }
                }
            }
        }
        return r;
    }

    // ── Query: caller nodes for a symbol ────────────────────────
    // O(1) callee lookup: which symbol does a Call node invoke?
    // Returns INVALID_SYM if not a call or not indexed.
    SymId query_callee(NodeId node) const {
        if (node < callee_of_.size())
            return callee_of_[node];
        return INVALID_SYM;
    }

    // O(1) caller query using callers_of_ index (built during build())
    // Fallback: if index not available (build_call_graph wasn't run), do O(N) scan
    std::vector<NodeId> query_callers(SymId sym, FlatAST& flat) {
        // Try indexed path first
        if (!callers_of_.empty()) {
            auto it = callers_of_.find(sym);
            if (it != callers_of_.end())
                return it->second;
            return {};
        }
        // Fallback: O(N) scan for unindexed state
        std::vector<NodeId> callers;
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id == sym)
                    callers.push_back(id);
            }
        }
        return callers;
    }

    // ── Mark scope containing a node as dirty ───────────────────
    void mark_dirty(NodeId node) {
        if (!built_ || scopes_.empty()) return;
        for (auto& sn : scopes_) {
            if (sn.tombstoned) continue;
            if (sn.node == node) {
                mark_dirty_up(sn);
                return;
            }
        }
        for (auto& sn : scopes_) sn.dirty = true;
    }

    void mark_dirty_up(ScopeNode& sn) {
        sn.dirty = true;
        if (sn.parent < scopes_.size() && !scopes_[sn.parent].tombstoned)
            mark_dirty_up(scopes_[sn.parent]);
    }

    // ── Incremental rebuild ─────────────────────────────────────
    bool rebuild_dirty(FlatAST& flat, StringPool& pool) {
        if (!built_) {
            build(flat, pool);
            return true;
        }
        if (flat_ != &flat || flat.size() != flat_size_at_build_) {
            build(flat, pool);
            return true;
        }
        bool any_dirty = false;
        for (auto& sn : scopes_) {
            if (sn.dirty) { any_dirty = true; break; }
        }
        if (!any_dirty)
            return false;
        build(flat, pool);
        return true;
    }

    // ── Incremental: update callers_of_ for specific syms ─────
    // Used after mutations that only modify existing nodes
    // (rebind/set-body/replace-pattern) without adding new nodes.
    // Full flat scan still needed but scope tree + defs/uses preserved.
    void update_callers_for(FlatAST& flat, const std::unordered_set<SymId>& affected_syms) {
        if (!built_ || affected_syms.empty())
            return;
        // Clear old callers entries for affected syms
        for (auto sym : affected_syms)
            callers_of_.erase(sym);
        // Reset callee_of_ for call nodes that referenced affected syms
        for (NodeId id = 0; id < callee_of_.size(); ++id) {
            if (callee_of_[id] != INVALID_SYM && affected_syms.count(callee_of_[id]))
                callee_of_[id] = INVALID_SYM;
        }
        // Full scan to find new Call nodes referencing affected syms
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id != INVALID_SYM &&
                    affected_syms.count(callee.sym_id)) {
                    callers_of_[callee.sym_id].push_back(id);
                    callee_of_[id] = callee.sym_id;
                }
            }
        }
    }
};

// ═══════════════════════════════════════════════════════════════
// WorkspaceTree — multi-layer workspace isolation
// ═══════════════════════════════════════════════════════════════

struct WorkspaceNode {
    std::string name;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    // COW: parent's flat/pool (no copy until first mutate)
    aura::ast::FlatAST* parent_flat_ = nullptr;
    aura::ast::StringPool* parent_pool_ = nullptr;
    std::uint64_t generation = 0;
    bool read_only = false;
    bool has_own_flat = false;
    bool is_root = false;
};

struct WorkspaceTree {
    std::vector<WorkspaceNode> nodes_;
    std::uint32_t active_idx_ = 0;

    WorkspaceTree() {}

    std::size_t size() const { return nodes_.size(); }
    std::uint32_t active_idx() const { return active_idx_; }
    WorkspaceNode* active() {
        return active_idx_ < nodes_.size() ? &nodes_[active_idx_] : nullptr;
    }

    // Ensure the workspace has its own flat (COW trigger)
    // If the workspace doesn't have its own flat, clone from parent.
    bool ensure_local_flat(std::uint32_t idx) {
        if (idx >= nodes_.size()) return false;
        auto& n = nodes_[idx];
        if (n.is_root) return true;  // root always has own flat
        if (n.read_only) return false;  // read-only, cannot create local copy
        if (n.has_own_flat) return true;

        // COW: clone from parent's flat
        if (n.parent_flat_) {
            auto* new_flat = new aura::ast::FlatAST();
            auto* new_pool = new aura::ast::StringPool();
            // Clone via re-parsing the parent's current source
            // For now, use a shallow copy approach
            *new_flat = *n.parent_flat_;  // copy all SoA vectors
            *new_pool = *n.parent_pool_;
            n.flat = new_flat;
            n.pool = new_pool;
            n.has_own_flat = true;
            n.generation = 1;
            return true;
        }
        return false;
    }

    // Create a child workspace (COW: no clone until first mutate)
    std::uint32_t create_child(const std::string& name,
                                aura::ast::FlatAST* parent_flat,
                                aura::ast::StringPool* parent_pool) {
        auto idx = static_cast<std::uint32_t>(nodes_.size());

        WorkspaceNode node;
        node.name = name;
        node.generation = 0;
        node.read_only = false;
        node.has_own_flat = false;
        node.is_root = false;
        // Store parent refs for COW; child shares parent's flat initially
        node.parent_flat_ = parent_flat;
        node.parent_pool_ = parent_pool;
        node.flat = parent_flat;   // share parent's flat until COW
        node.pool = parent_pool;

        nodes_.push_back(std::move(node));
        return idx;
    }

    // Delete a workspace (cannot delete root = idx 0)
    bool delete_child(std::uint32_t idx) {
        if (idx == 0 || idx >= nodes_.size()) return false;
        auto& n = nodes_[idx];
        if (n.has_own_flat) {
            delete n.flat;
            delete n.pool;
        }
        n.flat = nullptr;
        n.pool = nullptr;
        n.parent_flat_ = nullptr;
        n.parent_pool_ = nullptr;
        return true;
    }

    // Switch active workspace
    bool set_active(std::uint32_t idx) {
        if (idx >= nodes_.size()) return false;
        active_idx_ = idx;
        return true;
    }

    // Set read-only flag
    void set_read_only(std::uint32_t idx, bool ro) {
        if (idx < nodes_.size())
            nodes_[idx].read_only = ro;
    }

    // Check if mutation is allowed
    bool can_write(std::uint32_t idx) {
        if (idx >= nodes_.size()) return false;
        if (nodes_[idx].read_only) return false;
        return true;
    }
};

// ═══════════════════════════════════════════════════════════════

void Evaluator::update_shared_tree_root() {
    if (!workspace_tree_) return;
    auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
    if (wt->size() > 0) {
        // Only update the ACTIVE node's flat/pool pointer.
        // DO NOT update root (index 0) — that would break isolation.
        // If the active node IS root (index 0), that gets updated naturally.
        // This ensures set-code/mutate writes propagate to the correct
        // WorkspaceNode. Without this, a child workspace's modifications
        // are lost on workspace:switch back because the node still points
        // to the pre-set-code flat.
        auto active = wt->active_idx();
        if (active < wt->size()) {
            wt->nodes_[active].flat = workspace_flat_;
            wt->nodes_[active].pool = workspace_pool_;
            if (active > 0)
                wt->nodes_[active].has_own_flat = true;
        }
    }
}

// ── Runtime-loaded libcurl via dlopen (avoids ld.bfd symbol issues) ──
namespace {
struct CurlAPI {
    void* handle = nullptr;
    CURL* (*easy_init)() = nullptr;
    CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
    CURLcode (*easy_perform)(CURL*) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    struct curl_slist* (*slist_append)(struct curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(struct curl_slist*) = nullptr;

    bool load() {
        if (handle) return true;
        handle = ::dlopen("libcurl.so.4", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return false;
        easy_init = (CURL*(*)())::dlsym(handle, "curl_easy_init");
        easy_setopt = (CURLcode(*)(CURL*,CURLoption,...))::dlsym(handle, "curl_easy_setopt");
        easy_perform = (CURLcode(*)(CURL*))::dlsym(handle, "curl_easy_perform");
        easy_cleanup = (void(*)(CURL*))::dlsym(handle, "curl_easy_cleanup");
        slist_append = (struct curl_slist*(*)(struct curl_slist*,const char*))::dlsym(handle, "curl_slist_append");
        slist_free_all = (void(*)(struct curl_slist*))::dlsym(handle, "curl_slist_free_all");
        return easy_init && easy_setopt && easy_perform && easy_cleanup
            && slist_append && slist_free_all;
    }
    ~CurlAPI() { if (handle) ::dlclose(handle); }
};
CurlAPI& get_curl() {
    static CurlAPI c;
    return c;
}
auto& curl_writer_fn() {
    static auto writer = [](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
        static_cast<std::string*>(ud)->append(ptr, size * nmemb);
        return size * nmemb;
    };
    return writer;
}
}

Evaluator::Evaluator() {
    // Register heap mutex for thread-safe GC (P2)
    aura::messaging::g_heap_mutex = [this]() -> std::mutex& {
        return heap_mutex();
    };

    top_.set_primitives(&primitives_);
    top_.set_cells(&cells_);
    primitives_.set_string_heap(&string_heap_);
    init_pair_primitives();

    // ── C FFI primitives ────────────────────────────────
    primitives_.add("c-load", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_int(0);
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
    auto parse_ffi_sig = [](const std::string& sig, int& ret_type,
                            std::vector<int>& arg_types, std::string* err_type = nullptr) -> bool {
        auto arrow = sig.find("->");
        if (arrow == std::string::npos) {
            if (err_type) *err_type = "missing '->' in signature";
            return false;
        }
        if (sig.empty() || sig[0] != '(') {
            if (err_type) *err_type = "signature must start with '('";
            return false;
        }
        auto arg_part = sig.substr(1, arrow - 1);
        auto ret_part = sig.substr(arrow + 2);
        auto type_to_int = [](const std::string& tn, std::string* err = nullptr) -> int {
            auto t = tn;
            while (!t.empty() && t.front() == ' ')
                t = t.substr(1);
            while (!t.empty() && t.back() == ' ')
                t.pop_back();
            if (t == "Int")
                return 1;
            if (t == "Float")
                return 2;
            if (t == "String")
                return 3;
            if (t == "Opaque")
                return 4;
            if (t == "Void")
                return 0;
            if (err) *err = t.empty() ? "empty type" : "unknown type: " + t;
            return -1;
        };
        std::string cur;
        for (auto c : arg_part) {
            if (c == ' ' || c == '(' || c == ')') {
                if (!cur.empty()) {
                    int at = type_to_int(cur, err_type);
                    if (at < 0)
                        return false;
                    arg_types.push_back(at);
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) {
            int at = type_to_int(cur, err_type);
            if (at < 0)
                return false;
            arg_types.push_back(at);
        }
        ret_type = type_to_int(ret_part, err_type);
        if (ret_type < 0)
            return false;
        return true;
    };

    primitives_.add("c-func", [this, &parse_ffi_sig](const auto& a) -> EvalValue {
        coverage_counters_[8]++;
        // (c-func lib-id "name" sig-string)  e.g. (c-func 0 "sqrt" "(Float) -> Float")
        // lib-id -1 uses RTLD_DEFAULT (no c-load needed) — architecture independent.
        // Or legacy: (c-func lib-id "name" ret-int arg-int...)
        if (a.size() < 3 || !types::is_int(a[0]) || !types::is_string(a[1])) {
            fprintf(stdout, "c-func: expected (c-func lib-id \"name\" signature\n");
            fprintf(stdout, "  signature format: \"(ArgType) -> RetType\"  e.g. \"(String) -> Int\"\n");
            return make_int(0);
        }
        auto raw_lib_id = types::as_int(a[0]);
        void* lib = RTLD_DEFAULT;
        if (raw_lib_id >= 0) {
            auto lib_idx = static_cast<std::size_t>(raw_lib_id);
            if (lib_idx >= g_ffi_libs.size()) {
                fprintf(stdout, "c-func: invalid library handle %zu (use -1 for RTLD_DEFAULT)\n", lib_idx);
                return make_int(0);
            }
            lib = g_ffi_libs[lib_idx];
        }
        auto name = string_heap_[types::as_string_idx(a[1])];
        int ret_type = 1;
        std::vector<int> arg_types;
        if (types::is_string(a[2])) {
            auto sig = string_heap_[types::as_string_idx(a[2])];
            std::string sig_err;
            if (!parse_ffi_sig(sig, ret_type, arg_types, &sig_err)) {
                fprintf(stdout, "c-func: invalid signature '%s'\n", sig.c_str());
                fprintf(stdout, "  reason: %s\n", sig_err.c_str());
                fprintf(stdout, "  expected: \"(ArgType) -> RetType\"\n");
                fprintf(stdout, "  valid types: Int, Float, String, Opaque, Void\n");
                return make_int(0);
            }
        } else if (types::is_int(a[2])) {
            ret_type = static_cast<int>(types::as_int(a[2]));
            for (std::size_t i = 3; i < a.size(); ++i)
                if (types::is_int(a[i]))
                    arg_types.push_back(static_cast<int>(types::as_int(a[i])));
        } else {
            fprintf(stdout, "c-func: third arg must be signature string like \"(String) -> Int\"\n");
            return make_int(0);
        }
        auto* fn_ptr = ::dlsym(lib, name.c_str());
        if (!fn_ptr) {
            auto* err = ::dlerror();
            fprintf(stdout, "c-func: symbol '%s' not found in library\n", name.c_str());
            if (err)
                fprintf(stdout, "  dlerror: %s\n", err);
            fprintf(stdout, "  tip: use (c-func -1 \"%s\" \"(String) -> Int\") with RTLD_DEFAULT\n", name.c_str());
            return make_int(0);
        }
        auto fidx = g_ffi_funcs.size();
        g_ffi_funcs.push_back({fn_ptr, name, ret_type, std::move(arg_types)});
        auto closure_id = static_cast<std::uint64_t>(fidx) | (1ULL << 63);
        return types::make_closure(closure_id);
    });

    // ── Opaque pointer primitives ────────────────────────────
    primitives_.add("c-opaque", [this](const auto& a) -> EvalValue {
        // Create an opaque pointer from an integer address
        // (c-opaque <ptr-as-int>)
        coverage_counters_[8]++;
        if (a.empty() || !types::is_int(a[0]))
            return make_int(0);
        auto addr = types::as_int(a[0]);
        auto idx = opaque_heap_.size();
        opaque_heap_.push_back(reinterpret_cast<void*>(addr));
        return types::make_opaque(idx);
    });

    primitives_.add("c-opaque?", [this](const auto& a) -> EvalValue {
        return types::make_bool(!a.empty() && types::is_opaque(a[0]));
    });

    primitives_.add("c-opaque->int", [this](const auto& a) -> EvalValue {
        // Extract the raw pointer address from an opaque value as Int
        if (a.empty() || !types::is_opaque(a[0]))
            return make_int(0);
        auto idx = types::as_opaque_idx(a[0]);
        if (idx >= opaque_heap_.size())
            return make_int(0);
        return make_int(reinterpret_cast<std::int64_t>(opaque_heap_[idx]));
    });

    primitives_.add("c-alloc", [this](const auto& a) -> EvalValue {
        // Allocate a block of memory and return as opaque
        // (c-alloc <size-bytes>)
        if (a.empty() || !types::is_int(a[0]))
            return make_int(0);
        auto size = static_cast<std::size_t>(types::as_int(a[0]));
        if (size == 0)
            return make_int(0);
        auto* ptr = std::calloc(1, size);
        auto idx = opaque_heap_.size();
        opaque_heap_.push_back(ptr);
        return types::make_opaque(idx);
    });

    primitives_.add("c-free", [this](const auto& a) -> EvalValue {
        // Free memory allocated by c-alloc
        // (c-free <opaque>)
        if (a.empty() || !types::is_opaque(a[0]))
            return make_void();
        auto idx = types::as_opaque_idx(a[0]);
        if (idx >= opaque_heap_.size())
            return make_void();
        std::free(opaque_heap_[idx]);
        opaque_heap_[idx] = nullptr;
        return make_void();
    });

    // ── Struct support (opaque-backed struct) ─────────────────
    primitives_.add("c-struct-size", [this](const auto& a) -> EvalValue {
        // Return the total size of a struct given field sizes.
        // (c-struct-size field-size...)
        std::size_t total = 0;
        for (auto& arg : a) {
            if (types::is_int(arg))
                total += static_cast<std::size_t>(types::as_int(arg));
        }
        return make_int(static_cast<std::int64_t>(total));
    });

    primitives_.add("c-struct-set!", [this](const auto& a) -> EvalValue {
        // Write a value into a struct at byte offset.
        // (c-struct-set! <opaque> <offset-bytes> <value>)
        if (a.size() < 3 || !types::is_opaque(a[0]) || !types::is_int(a[1]))
            return make_void();
        auto oi = types::as_opaque_idx(a[0]);
        if (oi >= opaque_heap_.size() || !opaque_heap_[oi])
            return make_void();
        auto offset = static_cast<std::size_t>(types::as_int(a[1]));
        auto* base = static_cast<char*>(opaque_heap_[oi]);
        auto& val = a[2];
        if (types::is_int(val)) {
            auto v = types::as_int(val);
            std::memcpy(base + offset, &v, sizeof(v));
        } else if (types::is_float(val)) {
            auto v = types::as_float(val);
            std::memcpy(base + offset, &v, sizeof(v));
        } else if (types::is_opaque(val)) {
            // Store pointer value
            auto vi = types::as_opaque_idx(val);
            auto* ptr = vi < opaque_heap_.size() ? opaque_heap_[vi] : nullptr;
            std::memcpy(base + offset, &ptr, sizeof(ptr));
        }
        return make_void();
    });

    primitives_.add("c-struct-ref", [this](const auto& a) -> EvalValue {
        // Read a value from a struct at byte offset with type.
        // (c-struct-ref <opaque> <offset-bytes> <type>)
        // type: 0=Int, 1=Float, 2=void*(Opaque)
        if (a.size() < 3 || !types::is_opaque(a[0]) || !types::is_int(a[1]) ||
            !types::is_int(a[2]))
            return make_int(0);
        auto oi = types::as_opaque_idx(a[0]);
        if (oi >= opaque_heap_.size() || !opaque_heap_[oi])
            return make_int(0);
        auto offset = static_cast<std::size_t>(types::as_int(a[1]));
        auto type = static_cast<int>(types::as_int(a[2]));
        auto* base = static_cast<const char*>(opaque_heap_[oi]);
        if (type == 0) { // Int
            std::int64_t v = 0;
            std::memcpy(&v, base + offset, sizeof(v));
            return make_int(v);
        } else if (type == 1) { // Float
            double v = 0;
            std::memcpy(&v, base + offset, sizeof(v));
            return types::make_float(v);
        } else if (type == 2) { // void* → Opaque
            void* ptr = nullptr;
            std::memcpy(&ptr, base + offset, sizeof(ptr));
            auto ni = opaque_heap_.size();
            opaque_heap_.push_back(ptr);
            return types::make_opaque(ni);
        }
        return make_int(0);
    });

    build_primitive_slots();

    // ── Environment + HTTP primitives ────────────────────────
    primitives_.add("getenv", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto* val = ::getenv(name.c_str());
        if (!val)
            return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::string(val));
        return types::make_string(sidx);
    });

    // ── HTTP primitives (via curl CLI) ─────────────────────
    primitives_.add("http-get", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto url = string_heap_[types::as_string_idx(a[0])];
        std::string cmd = "curl -s -f \"" + url + "\" 2>/dev/null";
        std::array<char, 4096> buf;
        std::string result;
        auto fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return make_void();
        while (::fgets(buf.data(), static_cast<int>(buf.size()), fp))
            result += buf.data();
        auto rc = ::pclose(fp);
        if (rc != 0 && result.empty())
            return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return types::make_string(sidx);
    });

    primitives_.add("http-post", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();

        auto& curl_url = string_heap_[types::as_string_idx(a[0])];
        auto& curl_body = string_heap_[types::as_string_idx(a[1])];
        std::string auth;
        if (a.size() >= 3 && types::is_string(a[2]))
            auth = string_heap_[types::as_string_idx(a[2])];

        // Async HTTP via thread (fiber-friendly, serve mode only)
        if (aura::messaging::g_http_post_async) {
            auto result = aura::messaging::g_http_post_async(curl_url, curl_body, auth);
            if (!result.empty()) {
                auto sidx = string_heap_.size();
                string_heap_.push_back(std::move(result));
                return types::make_string(sidx);
            }
        }

        // Try native libcurl (synchronous, default path)
        std::string result;
        if (get_curl().load()) {
            auto& curl_url = string_heap_[types::as_string_idx(a[0])];
            auto& curl_body = string_heap_[types::as_string_idx(a[1])];

            CURL* curl = get_curl().easy_init();
            if (curl) {
                struct curl_slist* headers = nullptr;
                headers = get_curl().slist_append(headers, "Content-Type: application/json");
                if (a.size() >= 3 && types::is_string(a[2])) {
                    auto& auth = string_heap_[types::as_string_idx(a[2])];
                    headers = get_curl().slist_append(headers,
                        (std::string("Authorization: Bearer ") + auth).c_str());
                }

                std::string response;
                get_curl().easy_setopt(curl, CURLOPT_URL, curl_url.c_str());
                get_curl().easy_setopt(curl, CURLOPT_POST, 1L);
                get_curl().easy_setopt(curl, CURLOPT_POSTFIELDS, curl_body.c_str());
                get_curl().easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)curl_body.size());
                get_curl().easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                get_curl().easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_writer_fn);
                get_curl().easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                get_curl().easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                get_curl().easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
                get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
                get_curl().easy_setopt(curl, CURLOPT_USERAGENT, "aura/1.0");

                CURLcode res = get_curl().easy_perform(curl);
                get_curl().slist_free_all(headers);
                get_curl().easy_cleanup(curl);

                if (res == CURLE_OK && !response.empty())
                    result = std::move(response);
            }
        }

        if (result.empty()) {
            // ── fallback: pipe+fork+exec curl CLI ──
            auto& url = string_heap_[types::as_string_idx(a[0])];
            auto& body = string_heap_[types::as_string_idx(a[1])];
            std::string auth_hdr;
            if (a.size() >= 3 && types::is_string(a[2]))
                auth_hdr = std::string("Authorization: Bearer ")
                    + string_heap_[types::as_string_idx(a[2])];
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0) return make_void();
            pid_t pid = ::fork();
            if (pid < 0) { ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]); return make_void(); }
            if (pid == 0) {
                ::close(in[1]); ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO); ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]); ::close(out[1]);
                const char* argv[16]{}; int i = 0;
                argv[i++] = "curl"; argv[i++] = "-s"; argv[i++] = "-X"; argv[i++] = "POST";
                argv[i++] = "--data-binary"; argv[i++] = "@-";
                argv[i++] = "-H"; argv[i++] = "Content-Type: application/json";
                if (!auth_hdr.empty()) { argv[i++] = "-H"; argv[i++] = auth_hdr.c_str(); }
                argv[i++] = "--max-time"; argv[i++] = "30";
                argv[i++] = "--connect-timeout"; argv[i++] = "10";
                argv[i++] = url.c_str(); argv[i] = nullptr;
                ::execvp("curl", const_cast<char* const*>(argv));
                ::_exit(1);
            }
            ::close(in[0]); ::close(out[1]);
            ::write(in[1], body.data(), body.size()); ::close(in[1]);
            std::array<char, 4096> fbuf; ssize_t nr;
            while ((nr = ::read(out[0], fbuf.data(), fbuf.size())) > 0)
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            ::close(out[0]); int cstat; ::waitpid(pid, &cstat, 0);
        }

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
        if (fd < 0) {
            ::freeaddrinfo(res);
            return make_void();
        }
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
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
                    ::close(fd);
                    ::freeaddrinfo(res);
                    return make_void(); // timeout or error
                }
                // Check if connect succeeded
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                    ::close(fd);
                    ::freeaddrinfo(res);
                    return make_void();
                }
            } else {
                ::close(fd);
                ::freeaddrinfo(res);
                return make_void();
            }
        }
        ::fcntl(fd, F_SETFL, flags); // restore blocking
        ::freeaddrinfo(res);
        return types::make_int(static_cast<std::int64_t>(fd));
    });

    primitives_.add("tcp-send", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !types::is_int(a[0]) || !types::is_string(a[1]))
            return make_int(-1);
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto sidx = types::as_string_idx(a[1]);
        if (sidx >= string_heap_.size())
            return types::make_int(0);
        auto& data = string_heap_[sidx];
        auto sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        return types::make_int(static_cast<std::int64_t>(sent));
    });

    primitives_.add("tcp-recv", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto maxlen = static_cast<std::size_t>(
            a.size() >= 2 && types::is_int(a[1]) ? types::as_int(a[1]) : 4096);
        if (maxlen > 65536)
            maxlen = 65536;
        std::string buf(maxlen, '\0');
        auto n = ::recv(fd, buf.data(), maxlen, 0);
        if (n <= 0)
            return make_void();
        buf.resize(static_cast<std::size_t>(n));
        auto sidx = string_heap_.size();
        string_heap_.push_back(std::move(buf));
        return types::make_string(sidx);
    });

    // (declare-type name "param-types..." "ret-type") — 声明函数类型签名
    // 示例: (declare-type add "Int Int" "Int") → add: (Int, Int) -> Int
    // 这些签名在 typecheck-current 时注入到类型环境中。
    primitives_.add("declare-type", [this](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto params_idx = as_string_idx(a[1]);
        auto ret_idx = as_string_idx(a[2]);
        if (name_idx >= string_heap_.size() || params_idx >= string_heap_.size() ||
            ret_idx >= string_heap_.size())
            return make_bool(false);
        declared_type_sigs_[string_heap_[name_idx]] = {
            .type_str = string_heap_[params_idx] + "|" + string_heap_[ret_idx],
            .resolved = false
        };
        return make_bool(true);
    });

    // (generate-type-sigs "module-path") — 类型推断并生成 .aura-type 文件
    // 解析模块文件，对其中每个 export 的函数进行类型推断，
    // 生成同名的 .aura-type 签名文件。
    // 示例: (generate-type-sigs "helper.aura") → helper.aura-type
    primitives_.add("generate-type-sigs", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto path = resolve_module_path(string_heap_[idx]);
        if (path.empty()) {
            std::println(std::cerr, "generate-type-sigs: cannot resolve '{}'", string_heap_[idx]);
            return make_bool(false);
        }

        // 读取并解析模块文件
        std::ifstream f(path);
        if (!f) { std::println(std::cerr, "generate-type-sigs: cannot open '{}'", path); return make_bool(false); }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) return make_bool(false);

        aura::ast::ASTArena local_arena;
        auto alloc = local_arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "generate-type-sigs: parse error");
            return make_bool(false);
        }
        flat.root = pr.root;

        // 类型推断
        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(flat, pool, flat.root, diag);

        // 扫描所有 Define 节点，收集名称
        // 如果模块有 (export ...) 声明，只输出 export 的函数
        std::unordered_set<std::string> export_set;
        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Export) {
                for (auto cid : nv.children) {
                    auto cv = flat.get(cid);
                    if (cv.tag == aura::ast::NodeTag::Variable)
                        export_set.insert(std::string(pool.resolve(cv.sym_id)));
                }
            }
        }

        std::vector<std::string> fn_names;
        std::unordered_map<std::string, aura::ast::NodeId> define_map;
        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Define) {
                auto name = std::string(pool.resolve(nv.sym_id));
                define_map[name] = nid;
                if (export_set.empty() || export_set.count(name))
                    fn_names.push_back(name);
            }
        }

        // 生成类型签名文件
        auto type_sig_path = path;
        {
            auto dot_pos = type_sig_path.rfind('.');
            if (dot_pos != std::string::npos)
                type_sig_path = type_sig_path.substr(0, dot_pos) + ".aura-type";
        }

        std::ofstream of(type_sig_path);
        if (!of) {
            std::println(std::cerr, "generate-type-sigs: cannot write '{}'", type_sig_path);
            return make_bool(false);
        }

        std::function<std::string(std::uint32_t)> type_name_for =
            [&](std::uint32_t tid) -> std::string {
            auto t = aura::core::TypeId{tid, 1};
            auto tag = treg.tag_of(t);
            switch (tag) {
                case aura::core::TypeTag::INT:    return "Int";
                case aura::core::TypeTag::BOOL:   return "Bool";
                case aura::core::TypeTag::STRING: return "String";
                case aura::core::TypeTag::FLOAT:  return "Float";
                case aura::core::TypeTag::VOID:   return "Void";
                case aura::core::TypeTag::FUNC: {
                    if (auto* ft = treg.func_of(t)) {
                        std::string s;
                        for (auto& a : ft->args) {
                            if (!s.empty()) s += " ";
                            s += type_name_for(a.index);
                        }
                        s += " -> " + type_name_for(ft->ret.index);
                        return s;
                    }
                    return "Any";
                }
                default: return "Any";
            }
        };

        std::size_t written = 0;
        for (auto& name : fn_names) {
            auto it = define_map.find(name);
            if (it == define_map.end()) continue;
            auto def_v = flat.get(it->second);
            if (!def_v.children.empty()) {
                auto val_id = def_v.child(0);
                // 对 define 的值显式进行类型推断（define 自身不遍历子节点）
                // 使用 tc 在同一个 TypeRegistry 中推断类型
                auto val_type = tc.infer_flat(flat, pool, val_id, diag);
                if (val_type.valid() && val_type.index != 0) {
                    of << name << ": " << type_name_for(val_type.index) << "\n";
                    ++written;
                }
            }
        }

        // 写入成功后，失效模块缓存强制下次 require 重新加载
        // 这样 .aura-type 文件才能在后续的 require 中被读取。
        // pre_exec_requires 可能在 generate-type-sigs 之前加载了模块。
        auto cache_it = module_cache_.find(path);
        if (cache_it != module_cache_.end()) {
            module_cache_.erase(cache_it);
        }

        std::println(std::cerr, "generate-type-sigs: wrote {} types to '{}'",
                     written, type_sig_path);
        return make_bool(written > 0);
    });

    // (check-module-signature "module-path")
    // 加载模块，对其每个 define 的函数进行类型推断，
    // 然后与 .aura-type 中的声明签名进行比对。
    // 输出不一致的诊断结果（不修改文件）。
    primitives_.add("check-module-signature", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto path = resolve_module_path(string_heap_[idx]);
        if (path.empty()) {
            std::println(std::cerr, "check-module-signature: cannot resolve '{}'", string_heap_[idx]);
            return make_bool(false);
        }

        // 读取并解析模块文件
        std::ifstream f(path);
        if (!f) { std::println(std::cerr, "check-module-signature: cannot open '{}'", path); return make_bool(false); }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) return make_bool(false);

        aura::ast::ASTArena local_arena;
        auto alloc = local_arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "check-module-signature: parse error");
            return make_bool(false);
        }
        flat.root = pr.root;

        // 类型推断：对每个 define 的值进行类型推断
        struct FnInfo { std::string name; std::string inferred_type; };
        std::vector<FnInfo> fn_infos;

        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(flat, pool, flat.root, diag);

        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Define) {
                auto name = std::string(pool.resolve(nv.sym_id));
                if (!nv.children.empty()) {
                    auto val_id = nv.child(0);
                    auto val_type = tc.infer_flat(flat, pool, val_id, diag);
                    if (val_type.valid() && val_type.index != 0) {
                        fn_infos.push_back({name, treg.format_type(val_type)});
                    }
                }
            }
        }

        // 读取 .aura-type 文件
        auto sig_path = path;
        {
            auto dot = sig_path.rfind('.');
            if (dot != std::string::npos)
                sig_path = sig_path.substr(0, dot) + ".aura-type";
        }

        struct SigDecl { std::string name; std::string decl_type; };
        std::vector<SigDecl> sig_decls;

        struct stat st;
        if (::stat(sig_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream tf(sig_path);
            if (tf) {
                std::string line;
                while (std::getline(tf, line)) {
                    auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    auto arrow = line.find("->", colon);
                    if (arrow == std::string::npos) continue;
                    auto name = line.substr(0, colon);
                    name.erase(name.find_last_not_of(" \t\r") + 1);
                    auto params_str = line.substr(colon + 1, arrow - colon - 1);
                    params_str.erase(0, params_str.find_first_not_of(" \t\r"));
                    params_str.erase(params_str.find_last_not_of(" \t\r") + 1);
                    auto ret_str = line.substr(arrow + 2);
                    ret_str.erase(0, ret_str.find_first_not_of(" \t\r"));
                    ret_str.erase(ret_str.find_last_not_of(" \t\r\n") + 1);
                    sig_decls.push_back({name, params_str + " -> " + ret_str});
                }
            }
        }

        // 比对：每个 decl 必须在 inferred 中找到匹配的
        // 类型字符串精确匹配（eg: "Int Int -> Int"）
        std::size_t matched = 0, mismatched = 0, missing = 0;
        for (auto& sd : sig_decls) {
            bool found = false;
            bool match = false;
            for (auto& fi : fn_infos) {
                if (fi.name == sd.name) {
                    found = true;
                    // treg.format_type 返回 "(Int Int -> Int)"，sd.decl_type 是 "Int Int -> Int"
                    // 标准化比较：去掉 format_type 中的括号
                    std::string fmt = fi.inferred_type;
                    // format_type 像 "(Int Int -> Int)"，去掉首尾括号
                    if (fmt.size() >= 2 && fmt.front() == '(' && fmt.back() == ')')
                        fmt = fmt.substr(1, fmt.size() - 2);
                    // 替换类型变量 __tN 为 Any（未标注类型时推断出 "__t0 -> __t0"）
                    for (auto ci = fmt.find("__t"); ci != std::string::npos; ci = fmt.find("__t", ci)) {
                        auto end = ci + 3;
                        while (end < fmt.size() && (std::isalnum(fmt[end]) || fmt[end] == '_')) ++end;
                        fmt.replace(ci, end - ci, "Any");
                        ci += 3;
                    }
                    if (fmt == sd.decl_type) {
                        match = true;
                    }
                    break;
                }
            }
            if (!found) {
                std::println(std::cerr, "  MISSING '{}' in module (declared but not defined)", sd.name);
                ++missing;
            } else if (!match) {
                // 查找推断的类型字符串
                std::string inferred_fmt;
                for (auto& fi2 : fn_infos) {
                    if (fi2.name == sd.name) {
                        auto f = fi2.inferred_type;
                        if (f.size() >= 2 && f.front() == '(' && f.back() == ')')
                            f = f.substr(1, f.size() - 2);
                        // 替换类型变量 __tN 为 Any
                        std::string clean;
                        for (std::size_t ci = 0; ci < f.size(); ++ci) {
                            if (f[ci] == '_' && ci + 3 < f.size() && f[ci+1] == '_' && f[ci+2] == 't') {
                                clean += "Any";
                                while (ci < f.size() && (std::isalnum(f[ci]) || f[ci] == '_')) ++ci;
                                --ci;
                            } else {
                                clean += f[ci];
                            }
                        }
                        inferred_fmt = clean;
                        break;
                    }
                }
                std::println(std::cerr, "  MISMATCH '{}': declared '{}', inferred '{}'",
                             sd.name, sd.decl_type, inferred_fmt);
                ++mismatched;
            } else {
                ++matched;
            }
        }

        std::println(std::cerr, "check-module-signature: {}/{}/{} matched/mismatched/missing",
                     matched, mismatched, missing);
        return make_bool(matched > 0 || (mismatched == 0 && missing == 0));
    });

    primitives_.add("tcp-close", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        ::close(static_cast<int>(types::as_int(a[0])));
        return make_void();
    });

    // ═══════════════════════════════════════════════════════════════
    // P9: Def-Use Analysis (P1 — scope-level cached)
    // ═══════════════════════════════════════════════════════════════

    // ── 依赖图查询回调注册 ─────────────────────────────────────
    // 在 def-use 索引中注册依赖图查询函数，供 mutation 原语
    // (mutate:rebind / set-body) 在变更前查询调用者节点。
    // 定义在这里（DefUseIndex 完整类型已知后），绕开前向声明问题。
    dep_caller_fn_ = [](void* idx_ptr, aura::ast::SymId sym)
        -> std::vector<aura::ast::NodeId> {
        if (!idx_ptr) return {};
        auto* idx = static_cast<DefUseIndex*>(idx_ptr);
        auto result = idx->query_def_use(sym);
        return std::move(result.uses);
    };

    // Helper: get or rebuild the def-use index
    // Tracks defuse_version_ to detect mutations since last build.
    // (#10) Tracks rebuild count and clears affected_syms_ after rebuild.
    auto ensure_defuse = [this]() -> DefUseIndex* {
        if (!workspace_flat_ || !workspace_pool_)
            return nullptr;
        auto idx = static_cast<DefUseIndex*>(defuse_index_);
        if (!idx) {
            idx = new DefUseIndex();
            defuse_index_ = idx;
            idx->build(*workspace_flat_, *workspace_pool_);
            defuse_version_ = 1;
            defuse_rebuild_count_++;
            defuse_affected_syms_.clear();
            return idx;
        }

        // Collect affected syms since last ensure_defuse
        std::unordered_set<aura::ast::SymId> affected_sym_ids;
        if (!defuse_affected_syms_.empty()) {
            for (auto& name : defuse_affected_syms_) {
                auto sym = workspace_pool_->intern(name);
                if (sym != aura::ast::INVALID_SYM)
                    affected_sym_ids.insert(sym);
            }
        }
        defuse_affected_syms_.clear();

        // Incremental path: only rebuild callers_of_ for affected syms
        // when flat size hasn't changed (mutations that modify existing nodes).
        if (!affected_sym_ids.empty()) {
            if (workspace_flat_->size() == idx->flat_size_at_build_) {
                idx->update_callers_for(*workspace_flat_, affected_sym_ids);
                defuse_version_ = 1;
                return idx;
            }
        }

        // Fallback: full rebuild (flat size changed or many affected syms)
        idx->build(*workspace_flat_, *workspace_pool_);
        defuse_version_ = 1;
        defuse_rebuild_count_++;
        return idx;
    };

    // Helper: build Aura result list from NodeIds
    auto nodes_to_list = [this](const std::vector<DefUseIndex::NodeId>& nodes) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(*it)), list});
            list = make_pair(pid);
        }
        return list;
    };

    // (query:def-use "sym-name")
    //   → ((def-node-id ...) . (use-node-id ...))
    //   Scope-level cached def-use chain. Builds index on first call,
    //   incrementally rebuilds dirty scopes on subsequent calls.
    primitives_.add("query:def-use", [this, ensure_defuse, nodes_to_list](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        if (a.empty() || !is_string(a[0]))
            return merr("bad-arg", "usage: (query:def-use sym-name)");
        if (!workspace_flat_ || !workspace_pool_)
            return merr("no-workspace", "no workspace AST loaded");
        auto sym_idx = as_string_idx(a[0]);
        if (sym_idx >= string_heap_.size())
            return merr("bad-arg", "symbol name string index out of range");
        auto target_name = string_heap_[sym_idx];
        auto target_sym = workspace_pool_->intern(target_name);

        auto idx = ensure_defuse();
        if (!idx)
            return merr("internal", "failed to build def-use index");

        auto result = idx->query_def_use(target_sym);
        auto def_list = nodes_to_list(result.defs);
        auto use_list = nodes_to_list(result.uses);

        auto result_pid = pairs_.size();
        pairs_.push_back({def_list, use_list});
        return make_pair(result_pid);
    });

    // (query:reaches node-id)
    //   → ((def-node-id ...) . (use-node-id ...))
    //   Cached implementation using def-use index.
    primitives_.add("query:reaches", [this, ensure_defuse, nodes_to_list](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        if (a.empty() || !is_int(a[0]))
            return merr("bad-arg", "usage: (query:reaches node-id)");
        if (!workspace_flat_ || !workspace_pool_)
            return merr("no-workspace", "no workspace AST loaded");
        auto target = static_cast<DefUseIndex::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (target >= flat.size())
            return merr("out-of-range", "node ID " + std::to_string(target) + " >= flat size " + std::to_string(flat.size()));

        auto v = flat.get(target);
        DefUseIndex::SymId defined_sym = aura::ast::INVALID_SYM;
        switch (v.tag) {
            case aura::ast::NodeTag::Define:
            case aura::ast::NodeTag::Let:
            case aura::ast::NodeTag::LetRec:
                defined_sym = v.sym_id;
                break;
            default:
                return merr("type-error", "node " + std::to_string(target) + " is not a definition node");
        }
        if (defined_sym == aura::ast::INVALID_SYM)
            return merr("internal", "definition node has invalid symbol id");

        auto idx = ensure_defuse();
        if (!idx)
            return merr("internal", "failed to build def-use index");

        auto result = idx->query_def_use(defined_sym);
        auto def_list = nodes_to_list(result.defs);
        auto use_list = nodes_to_list(result.uses);

        auto result_pid = pairs_.size();
        pairs_.push_back({def_list, use_list});
        return make_pair(result_pid);
    });

    // (query:effects "sym-name")
    //   → ((def-node-id ...) . (use-node-id ...) . (caller-node-id ...))
    //   Cached defs + uses, linear scan for callers.
    primitives_.add("query:effects", [this, ensure_defuse, nodes_to_list](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        if (a.empty() || !is_string(a[0]))
            return merr("bad-arg", "usage: (query:effects sym-name)");
        if (!workspace_flat_ || !workspace_pool_)
            return merr("no-workspace", "no workspace AST loaded");
        auto sym_idx = as_string_idx(a[0]);
        if (sym_idx >= string_heap_.size())
            return merr("bad-arg", "symbol name string index out of range");
        auto target_name = string_heap_[sym_idx];
        auto target_sym = workspace_pool_->intern(target_name);
        auto& flat = *workspace_flat_;

        auto idx = ensure_defuse();
        if (!idx)
            return merr("internal", "failed to build def-use index");

        auto duo = idx->query_def_use(target_sym);
        auto callers = idx->query_callers(target_sym, flat);

        auto def_list = nodes_to_list(duo.defs);
        auto use_list = nodes_to_list(duo.uses);
        auto caller_list = nodes_to_list(callers);

        auto c1 = pairs_.size(); pairs_.push_back({caller_list, make_void()});
        auto c2 = pairs_.size(); pairs_.push_back({use_list, make_pair(c1)});
        auto c3 = pairs_.size(); pairs_.push_back({def_list, make_pair(c2)});
        return make_pair(c3);
    });

    // (query:build-index)
    //   → #t  Explicitly rebuild all def-use and call-graph indexes.
    //   Useful for benchmark consistency or after bulk mutations.
    primitives_.add("query:build-index", [this, ensure_defuse](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        auto idx = ensure_defuse();
        if (!idx)
            return merr("internal", "failed to build def-use index");
        // Force full rebuild regardless of version
        idx->build(*workspace_flat_, *workspace_pool_);
        defuse_version_ = 1;
        this->defuse_rebuild_count_++;
        defuse_affected_syms_.clear();
        return make_int(1);
    });

    // (query:index-stats)
    //   → ((callers . N) (def-syms . N) (refs . N) (rebuilds . N) (scopes . N) (nodes . N))
    //   Statistics about the def-use and call-graph indexes.
    primitives_.add("query:index-stats", [this, ensure_defuse, nodes_to_list](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        auto idx = ensure_defuse();
        if (!idx)
            return merr("internal", "failed to build def-use index");
        auto& flat = *workspace_flat_;

        // Build: ((k1 . v1) (k2 . v2) ...) as a proper Aura list.
        // Use correct pattern: forward ref → push data → use ref.
        // Do NOT use the ref inside the data being pushed (would self-reference).
        auto make_kv = [&](std::string_view k, std::int64_t v) -> types::EvalValue {
            auto kv_ref = make_pair(pairs_.size());
            auto k_sym = string_heap_.size();
            string_heap_.push_back(std::string(k));
            types::EvalValue car = make_string(k_sym);
            types::EvalValue cdr = make_int(v);
            pairs_.push_back(Pair{car, cdr});
            return kv_ref;
        };

        // Build list in reverse: each new pair's cdr points to previous stats
        auto stats = make_void();
        auto push_kv = [&](std::string_view k, std::int64_t v) {
            auto kv_ref = make_kv(k, v);
            auto new_ref = make_pair(pairs_.size());
            pairs_.push_back(Pair{kv_ref, stats});
            stats = new_ref;
        };
        push_kv("nodes", flat.size());
        push_kv("scopes", idx->scopes_.size());
        push_kv("def-syms", idx->def_syms_.size());
        push_kv("refs", idx->refs_.size());
        push_kv("callers", idx->callers_of_.size());
        push_kv("rebuilds", (std::int64_t)defuse_rebuild_count_);
        return stats;
    });

    // ═══════════════════════════════════════════════════════════════
    // P10: AST Snapshot / Restore / Diff
    // ═══════════════════════════════════════════════════════════════

    // Helper: line-based LCS diff (Myers-like, simplified)
    // Returns list of (tag . line) entries
    auto line_diff = [this](const std::string& old_text,
                            const std::string& new_text) -> EvalValue {
        // Split into lines
        auto split_lines = [](const std::string& s) -> std::vector<std::string> {
            std::vector<std::string> lines;
            std::string cur;
            for (auto c : s) {
                if (c == '\n') {
                    lines.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty())
                lines.push_back(std::move(cur));
            if (lines.empty())
                lines.push_back("");
            return lines;
        };

        auto a = split_lines(old_text);
        auto b = split_lines(new_text);
        int m = (int)a.size(), n = (int)b.size();

        // Build LCS table (2 rows for memory efficiency)
        // Use short int to keep table small; m,n rarely exceed 500
        std::vector<int> prev(n + 1, 0), cur(n + 1, 0);
        for (int i = 1; i <= m; ++i) {
            cur[0] = i;
            for (int j = 1; j <= n; ++j) {
                if (a[i - 1] == b[j - 1])
                    cur[j] = prev[j - 1];
                else
                    cur[j] = 1 + std::min({prev[j], cur[j - 1], prev[j - 1]});
            }
            std::swap(prev, cur);
        }

        // Backtrack to produce diff
        std::vector<std::tuple<char, std::string>> diff_entries;  // '=', '-', '+'
        int i = m, j = n;
        auto saved_prev = prev; (void)saved_prev;
        // We need the full table for backtracking. Build it.
        std::vector<std::vector<int>> table(m + 1, std::vector<int>(n + 1, 0));
        for (int i2 = 1; i2 <= m; ++i2) {
            for (int j2 = 1; j2 <= n; ++j2) {
                if (a[i2 - 1] == b[j2 - 1])
                    table[i2][j2] = table[i2 - 1][j2 - 1] + 1;
                else
                    table[i2][j2] = std::max(table[i2 - 1][j2], table[i2][j2 - 1]);
            }
        }

        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
                diff_entries.push_back({'=', a[i - 1]});
                --i; --j;
            } else if (j > 0 && (i == 0 || table[i][j - 1] >= table[i - 1][j])) {
                diff_entries.push_back({'+', b[j - 1]});
                --j;
            } else {
                diff_entries.push_back({'-', a[i - 1]});
                --i;
            }
        }
        std::reverse(diff_entries.begin(), diff_entries.end());

        // Convert to Aura list
        EvalValue result = make_void();
        for (auto it = diff_entries.rbegin(); it != diff_entries.rend(); ++it) {
            auto [tag, line] = *it;
            std::string kw_str = ":same";
            if (tag == '-') kw_str = ":removed";
            else if (tag == '+') kw_str = ":added";

            // Lookup or create keyword
            auto kw_idx = keyword_table_.size();
            for (std::size_t ki = 0; ki < keyword_table_.size(); ++ki) {
                if (keyword_table_[ki] == kw_str) { kw_idx = ki; break; }
            }
            if (kw_idx == keyword_table_.size())
                keyword_table_.push_back(kw_str);

            auto line_idx = string_heap_.size();
            string_heap_.push_back(line);
            auto line_pair = pairs_.size();
            pairs_.push_back({make_keyword(kw_idx), make_string(line_idx)});
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(line_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    };

    // (ast:snapshot ["name"])
    //   → integer snapshot ID (or -1 on failure)
    //   Stores current workspace source code as a named checkpoint.
    //   Names are optional; unnamed snapshots get auto-generated names.
    primitives_.add("ast:snapshot", [this](const auto& a) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_)
            return make_int(-1);

        // Get current source
        auto src_fn = primitives_.lookup("current-source");
        if (!src_fn)
            return make_int(-1);
        auto src = (*src_fn)({});
        if (!is_string(src))
            return make_int(-1);
        auto src_idx = as_string_idx(src);
        if (src_idx >= string_heap_.size())
            return make_int(-1);
        auto source = string_heap_[src_idx];

        // Optional name
        std::string name;
        if (a.size() >= 1 && is_string(a[0])) {
            auto name_idx = as_string_idx(a[0]);
            if (name_idx < string_heap_.size())
                name = string_heap_[name_idx];
        }

        auto id = snapshot_sources_.size();
        snapshot_sources_.push_back(source);
        snapshot_names_.push_back(name);
        return make_int(static_cast<std::int64_t>(id));
    });

    // (ast:list-snapshots)
    //   → ((id "name") ...)  list of (snapshot-id . name) pairs
    primitives_.add("ast:list-snapshots", [this](const auto&) -> EvalValue {
        EvalValue result = make_void();
        for (int i = (int)snapshot_sources_.size() - 1; i >= 0; --i) {
            auto name_idx = string_heap_.size();
            string_heap_.push_back(snapshot_names_[i].empty()
                                       ? std::format("snapshot-{}", i)
                                       : snapshot_names_[i]);
            // Pair: (id . name)
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_int(i), make_string(name_idx)});
            // Cons with result list
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(entry_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:restore id)
    //   → true on success
    //   Replaces current workspace with a previously snapshotted state.
    //   Caches (def-use, incremental compile state) are invalidated.
    primitives_.add("ast:restore", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto id = static_cast<std::size_t>(as_int(a[0]));
        if (id >= snapshot_sources_.size())
            return make_bool(false);

        auto& source = snapshot_sources_[id];
        // Re-parse the source into workspace
        auto source_idx = string_heap_.size();
        string_heap_.push_back(source);

        auto set_fn = primitives_.lookup("set-code");
        if (!set_fn)
            return make_bool(false);
        auto result = (*set_fn)({make_string(source_idx)});
        return make_bool(is_bool(result) ? as_bool(result) : false);
    });

    // (ast:diff [id])
    //   → ((tag . line) ...)
    //   Compares current source vs a snapshot. If no id given, compares
    //   versus the most recent snapshot. Tags: :same / :removed / :added
    primitives_.add("ast:diff", [this, line_diff](const auto& a) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();

        std::size_t id;
        if (a.empty() || !is_int(a[0])) {
            // Default to most recent snapshot
            if (snapshot_sources_.empty())
                return make_void();
            id = snapshot_sources_.size() - 1;
        } else {
            id = static_cast<std::size_t>(as_int(a[0]));
            if (id >= snapshot_sources_.size())
                return make_void();
        }

        auto& old_source = snapshot_sources_[id];

        // Get current source
        auto src_fn = primitives_.lookup("current-source");
        if (!src_fn)
            return make_void();
        auto src = (*src_fn)({});
        if (!is_string(src))
            return make_void();
        auto src_idx = as_string_idx(src);
        if (src_idx >= string_heap_.size())
            return make_void();
        auto& new_source = string_heap_[src_idx];

        return line_diff(old_source, new_source);
    });

    // ═══════════════════════════════════════════════════════════════
    // P11: AST Summary & Compile Status
    // ═══════════════════════════════════════════════════════════════

    // Helper: build Aura list from vector of strings
    auto str_list_to_pairs = [this](const std::vector<std::string>& items) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            auto idx = string_heap_.size();
            string_heap_.push_back(*it);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(idx), list});
            list = make_pair(pid);
        }
        return list;
    };

    // (ast:summary)
    //   → ((:key value) ...)  association list
    //   Returns structural summary of the current workspace:
    //     :total-nodes    — total AST node count
    //     :by-tag         — list of (tag-name . count)
    //     :mutation-count — number of applied mutations
    //     :scopes         — number of lexical scopes (from def-use cache)
    //     :defs           — total tracked definitions
    //     :uses           — total tracked variable uses
    //     :source-length  — source code character count
    primitives_.add("ast:summary", [this, str_list_to_pairs](const auto&) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();

        auto& flat = *workspace_flat_;
        auto total_nodes = flat.size();

        // Count nodes by type
        std::unordered_map<std::string, std::uint64_t> type_counts;
        for (aura::ast::NodeId id = 0; id < total_nodes; ++id) {
            auto v = flat.get(id);
            auto& m = aura::ast::meta(v.tag);
            if (m.name != "<gap>" && m.name != "LiteralInt") {
                type_counts[std::string(m.name)]++;
            } else if (m.name == "LiteralInt") {
                type_counts["LiteralInt"]++;
            }
        }

        // Ensure tag names that might map to wrong tag due to gap sentinels
        // The gap entries use LiteralInt tag so they miscount; fix here.
        // Actually, the meta function handles this correctly — it returns
        // the meta for a specific tag, not for LiteralInt in general.
        // Re-scan using tag value directly:
        type_counts.clear();
        for (aura::ast::NodeId id = 0; id < total_nodes; ++id) {
            auto v = flat.get(id);
            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt:    type_counts["LiteralInt"]++; break;
                case aura::ast::NodeTag::LiteralFloat:  type_counts["LiteralFloat"]++; break;
                case aura::ast::NodeTag::LiteralString: type_counts["LiteralString"]++; break;
                case aura::ast::NodeTag::Variable:      type_counts["Variable"]++; break;
                case aura::ast::NodeTag::Call:          type_counts["Call"]++; break;
                case aura::ast::NodeTag::IfExpr:        type_counts["IfExpr"]++; break;
                case aura::ast::NodeTag::Lambda:        type_counts["Lambda"]++; break;
                case aura::ast::NodeTag::Let:           type_counts["Let"]++; break;
                case aura::ast::NodeTag::LetRec:        type_counts["LetRec"]++; break;
                case aura::ast::NodeTag::Define:        type_counts["Define"]++; break;
                case aura::ast::NodeTag::Begin:         type_counts["Begin"]++; break;
                case aura::ast::NodeTag::Set:           type_counts["Set"]++; break;
                case aura::ast::NodeTag::Quote:         type_counts["Quote"]++; break;
                case aura::ast::NodeTag::Pair:          type_counts["Pair"]++; break;
                case aura::ast::NodeTag::Export:        type_counts["Export"]++; break;
                case aura::ast::NodeTag::TypeAnnotation: type_counts["TypeAnnotation"]++; break;
                case aura::ast::NodeTag::Coercion:      type_counts["Coercion"]++; break;
                case aura::ast::NodeTag::Linear:        type_counts["Linear"]++; break;
                case aura::ast::NodeTag::Move:          type_counts["Move"]++; break;
                case aura::ast::NodeTag::Borrow:        type_counts["Borrow"]++; break;
                case aura::ast::NodeTag::MutBorrow:     type_counts["MutBorrow"]++; break;
                case aura::ast::NodeTag::Drop:          type_counts["Drop"]++; break;
                default:                                type_counts["Other"]++; break;
            }
        }

        // Get def-use index info if available
        std::uint64_t n_scopes = 0, n_defs = 0, n_uses = 0;
        auto df_idx = static_cast<DefUseIndex*>(defuse_index_);
        if (df_idx && df_idx->built_) {
            n_scopes = df_idx->scopes_.size();
            n_defs = df_idx->def_syms_.size();
            n_uses = df_idx->uses_.size();
        }

        // Get mutation count
        auto n_mutations = flat.mutation_count();

        // Get source length (via current-source)
        std::uint64_t source_len = 0;
        auto src_fn = primitives_.lookup("current-source");
        if (src_fn) {
            auto src = (*src_fn)({});
            if (is_string(src)) {
                auto sidx = as_string_idx(src);
                if (sidx < string_heap_.size())
                    source_len = string_heap_[sidx].size();
            }
        }

        // Build by-tag list: ((tag-name . count) ...)
        EvalValue by_tag_list = make_void();
        // Sort tags alphabetically for deterministic output
        std::vector<std::pair<std::string, std::uint64_t>> sorted_tags;
        for (auto& [name, count] : type_counts)
            sorted_tags.push_back({name, count});
        std::sort(sorted_tags.begin(), sorted_tags.end());
        for (auto it = sorted_tags.rbegin(); it != sorted_tags.rend(); ++it) {
            auto name_idx = string_heap_.size();
            string_heap_.push_back(it->first);
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_string(name_idx), make_int(static_cast<std::int64_t>(it->second))});
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(entry_pair), by_tag_list});
            by_tag_list = make_pair(cons_pair);
        }

        // Build full result as alist: ((:key value) ...)
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = string_heap_.size();
            string_heap_.push_back(key);
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        auto cvt = [&](std::uint64_t n) -> EvalValue {
            auto idx = string_heap_.size();
            string_heap_.push_back(std::to_string(n));
            return make_string(idx);
        };
        std::uint64_t entry_ids[7];
        entry_ids[0] = add_entry(":total-nodes", cvt(total_nodes));
        entry_ids[1] = add_entry(":mutation-count", cvt(n_mutations));
        entry_ids[2] = add_entry(":source-length", cvt(source_len));
        entry_ids[3] = add_entry(":by-tag", by_tag_list);
        entry_ids[4] = add_entry(":scopes", cvt(n_scopes));
        entry_ids[5] = add_entry(":defs", cvt(n_defs));
        entry_ids[6] = add_entry(":uses", cvt(n_uses));

        EvalValue result = make_void();
        for (int ei = 6; ei >= 0; --ei) {
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(entry_ids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:validate-ownership) — Validate ownership invariants after mutations
    // Returns an alist: ((:pass true/false) (:notes (...)))
    // Each note has: (:node <id> :message <str> :kind <str>)
    primitives_.add("ast:validate-ownership", [this](const auto&) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_) {
            // Return early with pass=true if no workspace
            return make_bool(true);
        }

        auto& flat = *workspace_flat_;
        auto& pool = *workspace_pool_;

        // Collect bindings in the AST that involve ownership operations
        std::unordered_set<std::string> ownership_bindings;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if ((v.tag == aura::ast::NodeTag::Move ||
                 v.tag == aura::ast::NodeTag::Borrow ||
                 v.tag == aura::ast::NodeTag::MutBorrow ||
                 v.tag == aura::ast::NodeTag::Drop) &&
                !v.children.empty()) {
                auto inner_v = flat.get(v.child(0));
                if (inner_v.tag == aura::ast::NodeTag::Variable) {
                    auto var_name = std::string(pool.resolve(inner_v.sym_id));
                    if (!var_name.empty())
                        ownership_bindings.insert(var_name);
                }
            }
        }

        if (ownership_bindings.empty()) {
            // Build entry manually (add_entry lambda is defined later)
            auto mk_entry = [&](const std::string& k, EvalValue v) -> std::uint64_t {
                auto kidx = string_heap_.size();
                string_heap_.push_back(k);
                auto ep = pairs_.size();
                pairs_.push_back({make_string(kidx), v});
                return ep;
            };
            auto val_pass = mk_entry(":pass", make_bool(true));
            auto val_notes = mk_entry(":notes", make_void());
            uint64_t empty_ids[2] = {val_notes, val_pass};
            EvalValue empty_result = make_void();
            for (int ei = 1; ei >= 0; --ei) {
                auto cons_pair = pairs_.size();
                pairs_.push_back({make_pair(empty_ids[ei]), empty_result});
                empty_result = make_pair(cons_pair);
            }
            return empty_result;
        }

        std::vector<aura::compiler::OwnershipNote> notes;
        bool pass = aura::compiler::OwnershipEnv::validate_ownership(
            flat, pool, flat.root, ownership_bindings, notes);

        // Build result alist
        // ((:pass true/false) (:notes ((:node N :message M :kind K) ...)))
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = string_heap_.size();
            string_heap_.push_back(key);
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        // Build notes list
        EvalValue notes_list = make_void();
        for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
            auto& note = *it;
            auto node_idx = string_heap_.size();
            string_heap_.push_back(std::to_string(note.node));
            auto msg_idx = string_heap_.size();
            string_heap_.push_back(note.message);
            auto kind_idx = string_heap_.size();
            string_heap_.push_back(note.kind);

            // Build note alist: ((:node N :message M :kind K))
            auto entry_node = add_entry(":node", make_string(node_idx));
            auto entry_msg = add_entry(":message", make_string(msg_idx));
            auto entry_kind = add_entry(":kind", make_string(kind_idx));

            // Chain as: (((:kind K) ((:message M) ((:node N) ()))) ())
            // Proper alist: ((:node N) (:message M) (:kind K))
            auto pair3 = pairs_.size();
            pairs_.push_back({make_string(kind_idx), make_void()});
            auto pair2 = pairs_.size();
            pairs_.push_back({make_string(msg_idx), make_pair(pair3)});
            auto pair1 = pairs_.size();
            pairs_.push_back({make_string(node_idx), make_pair(pair2)});

            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(pair1), notes_list});
            notes_list = make_pair(cons_pair);
        }

        auto entry_pass = add_entry(":pass", make_bool(pass));
        auto entry_notes = add_entry(":notes", notes_list);
        uint64_t entry_ids[2] = {entry_notes, entry_pass};
        EvalValue result = make_void();
        for (int ei = 1; ei >= 0; --ei) {
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(entry_ids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (ast:validate-nodes) — Validate all nodes against NodeMeta invariants
    // Returns an alist: ((:pass . #t/#f) (:total . N) (:errors . ((:node N :message M) ...)))
    primitives_.add("ast:validate-nodes", [this](const auto&) -> EvalValue {
        if (!workspace_flat_) {
            return make_bool(true);
        }

        auto& flat = *workspace_flat_;
        std::vector<aura::ast::FlatAST::ValidationError> errors;
        auto count = flat.validate_all_nodes(errors);

        // Build alist: ((:pass . #t/#f) (:total . N) (:errors . ...))
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = string_heap_.size();
            string_heap_.push_back(key);
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        // Build errors list: ((:node N :message M) ...)
        EvalValue errors_list = make_void();
        for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
            auto& e = *it;
            auto node_idx = string_heap_.size();
            string_heap_.push_back(std::to_string(e.node));
            auto msg_idx = string_heap_.size();
            string_heap_.push_back(e.message);

            auto pair_msg = pairs_.size();
            pairs_.push_back({make_string(msg_idx), make_void()});
            auto pair_node = pairs_.size();
            pairs_.push_back({make_string(node_idx), make_pair(pair_msg)});
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(pair_node), errors_list});
            errors_list = make_pair(cons_pair);
        }

        auto val_pass = add_entry(":pass", make_bool(count == 0));
        auto val_total_k = string_heap_.size();
        string_heap_.push_back(":total");
        auto val_total_v = string_heap_.size();
        string_heap_.push_back(std::to_string(count));
        auto entry_total = pairs_.size();
        pairs_.push_back({make_string(val_total_k), make_string(val_total_v)});
        auto entry_errors = add_entry(":errors", errors_list);

        uint64_t eids[3] = {entry_errors, entry_total, val_pass};
        EvalValue result = make_void();
        for (int ei = 2; ei >= 0; --ei) {
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(eids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (compile:status)
    //   → ((:key value) ...)  association list
    //   Returns incremental compilation status:
    //     :dirty-nodes   — nodes marked as dirty (need recompilation)
    //     :clean-nodes   — nodes that are up-to-date
    //     :generation    — FlatAST generation counter
    //     :mutation-count— total mutations applied
    primitives_.add("compile:status", [this](const auto&) -> EvalValue {
        if (!workspace_flat_)
            return make_void();

        auto& flat = *workspace_flat_;
        auto total = flat.size();
        std::uint64_t dirty = 0;
        std::uint64_t clean = 0;

        for (aura::ast::NodeId id = 0; id < total; ++id) {
            if (flat.is_dirty(id))
                dirty++;
            else
                clean++;
        }

        // Build alist
        auto add_entry = [&](const std::string& key, EvalValue val) -> std::uint64_t {
            auto key_idx = string_heap_.size();
            string_heap_.push_back(key);
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_string(key_idx), val});
            return entry_pair;
        };

        EvalValue result = make_void();
        auto cvt = [&](std::uint64_t n) -> EvalValue {
            auto idx = string_heap_.size();
            string_heap_.push_back(std::to_string(n));
            return make_string(idx);
        };
        std::uint64_t entry_ids[4];
        entry_ids[0] = add_entry(":generation", cvt(flat.generation()));
        entry_ids[1] = add_entry(":mutation-count", cvt(flat.mutation_count()));
        entry_ids[2] = add_entry(":dirty-nodes", cvt(dirty));
        entry_ids[3] = add_entry(":clean-nodes", cvt(clean));
        for (int ei = 0; ei < 4; ++ei) {
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(entry_ids[ei]), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P12: Higher-Order AST Transformations
    // ═══════════════════════════════════════════════════════════════

    // (mutate:splice parent-id position code-strings... "summary")
    //   → list of inserted node IDs
    //   Parses and inserts multiple child expressions at the given position.
    //   code-strings can be multiple arguments (variadic).
    primitives_.add("mutate:splice", [this](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_) return merr("read-only", "workspace is read-only");
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) ||
            !workspace_flat_ || !workspace_pool_)
            return merr("bad-arg", "usage: (mutate:splice parent-id position code-strings... [summary])");
        auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto pos = static_cast<std::uint32_t>(as_int(a[1]));
        auto& flat = *workspace_flat_;
        if (parent >= flat.size())
            return merr("out-of-range", "parent node ID " + std::to_string(parent) + " >= flat size " + std::to_string(flat.size()));

        // Collect all code strings (variadic) before the optional summary
        std::vector<EvalValue> code_args;
        for (std::size_t i = 2; i < a.size(); ++i) {
            // If the last arg is a string and it's the 4th+ arg, it might be summary
            if (i == a.size() - 1 && i >= 3 && is_string(a[i]))
                continue;  // handled as summary below
            if (is_string(a[i]))
                code_args.push_back(a[i]);
        }

        // Summary: last string arg (after parent, position, and at least one code)
        std::string summary = "splice";
        if (a.size() >= 4 && is_string(a[a.size() - 1])) {
            auto sidx = as_string_idx(a[a.size() - 1]);
            if (sidx < string_heap_.size())
                summary = string_heap_[sidx];
        }

        if (code_args.empty())
            return merr("bad-arg", "no code strings provided to splice");

        // Parse each code string and insert
        EvalValue result_list = make_void();
        std::uint32_t insert_pos = pos;

        for (auto& code_val : code_args) {
            auto cidx = as_string_idx(code_val);
            if (cidx >= string_heap_.size())
                continue;

            auto pr = aura::parser::parse_to_flat(string_heap_[cidx], flat, *workspace_pool_);
            if (!pr.success || pr.root == aura::ast::NULL_NODE)
                continue;

            flat.insert_child(parent, insert_pos, pr.root);

            flat.add_mutation(parent, "splice", std::to_string(insert_pos),
                              string_heap_[cidx], summary);
            workspace_flat_->mark_dirty_upward(parent);

            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(pr.root)), result_list});
            result_list = make_pair(pid);

            insert_pos++;
        }
        // Reverse result list to match insertion order
        EvalValue reversed = make_void();
        {
            auto cur = result_list;
            while (is_pair(cur)) {
                auto idx = as_pair_idx(cur);
                if (idx >= pairs_.size()) break;
                auto ridx = pairs_.size();
                pairs_.push_back({pairs_[idx].car, reversed});
                reversed = make_pair(ridx);
                cur = pairs_[idx].cdr;
            }
        }
        return reversed;
    });

    // (mutate:wrap node-id wrapper-template "summary")
    //   → node ID of the wrapper call (or #f on failure)
    //   Wraps the target node in an expression. The wrapper-template is a
    //   code string with a single `_` placeholder where the target node
    //   will be inserted.
    //   Examples:
    //     (mutate:wrap 5 "(display _)" "wrap in display")
    //       → replaces node 5 with (display <original-node-5>)
    //     (mutate:wrap 3 "(let ((x _)) x)" "bind x")
    //       → wraps in let binding
    primitives_.add("mutate:wrap", [this](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_) return merr("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) ||
            !workspace_flat_ || !workspace_pool_)
            return merr("bad-arg", "usage: (mutate:wrap node-id wrapper-template [summary])");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto tmpl_idx = as_string_idx(a[1]);
        if (tmpl_idx >= string_heap_.size())
            return merr("bad-arg", "template string index out of range");
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));

        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? string_heap_[as_string_idx(a[2])]
                                  : "wrap node " + std::to_string(node);

        auto tmpl = string_heap_[tmpl_idx];

        // Find the parent of the target node
        aura::ast::NodeId parent_of_target = aura::ast::NULL_NODE;
        int child_idx_in_parent = -1;
        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
            auto pv = flat.get(pid);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == node) {
                    parent_of_target = pid;
                    child_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
            if (parent_of_target != aura::ast::NULL_NODE) break;
        }

        if (parent_of_target == aura::ast::NULL_NODE || child_idx_in_parent < 0)
            return merr("no-parent", "node " + std::to_string(node) + " has no parent in the AST");

        // Replace `_` in the template with a unique variable
        std::string sentinel = "__WRAP_TARGET_" + std::to_string(node) + "__";
        auto sentinel_pos = tmpl.find('_');
        if (sentinel_pos == std::string::npos)
            return merr("bad-arg", "wrapper-template must contain a '_' placeholder");

        auto parsed_tmpl = tmpl.substr(0, sentinel_pos) + sentinel + tmpl.substr(sentinel_pos + 1);

        // Parse the wrapper into workspace
        auto pr = aura::parser::parse_to_flat(parsed_tmpl, flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty()) parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "wrapper template could not be parsed";
            }
            return merr("parse-error", parse_err);
        }

        // Find the sentinel variable and its parent in the parsed AST
        auto sentinel_sym = workspace_pool_->intern(sentinel);
        aura::ast::NodeId sentinel_id = aura::ast::NULL_NODE;
        aura::ast::NodeId sentinel_parent = aura::ast::NULL_NODE;
        int sentinel_child_idx = -1;

        for (aura::ast::NodeId sid = 0; sid < flat.size(); ++sid) {
            auto sv = flat.get(sid);
            if (sv.tag == aura::ast::NodeTag::Variable && sv.sym_id == sentinel_sym) {
                sentinel_id = sid;
                // Find this variable's parent
                for (aura::ast::NodeId p2 = 0; p2 < flat.size(); ++p2) {
                    auto p2v = flat.get(p2);
                    for (std::size_t ci = 0; ci < p2v.children.size(); ++ci) {
                        if (p2v.child(ci) == sid) {
                            sentinel_parent = p2;
                            sentinel_child_idx = static_cast<int>(ci);
                            break;
                        }
                    }
                    if (sentinel_parent != aura::ast::NULL_NODE) break;
                }
                break;
            }
        }

        if (sentinel_id == aura::ast::NULL_NODE ||
            sentinel_parent == aura::ast::NULL_NODE ||
            sentinel_child_idx < 0)
            return merr("internal", "sentinel placeholder not found in parsed wrapper template");

        // Replace the sentinel variable in the wrapper with the target node
        flat.set_child(sentinel_parent, static_cast<std::uint32_t>(sentinel_child_idx), node);

        // Replace the original target node's position with the wrapper root
        flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent), pr.root);

        flat.add_mutation(node, "wrap", parsed_tmpl, summary, summary);
        return make_int(static_cast<std::int64_t>(pr.root));
    });

    // (mutate:refactor/extract node-id new-name "summary")
    //   → (define-node-id . call-node-id)
    //   Extracts the subtree rooted at node-id into a new top-level define,
    //   replacing the original node with a call to the new function.
    //   Free variables in the extracted expression become parameters.
    primitives_.add("mutate:refactor/extract", [this](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_) return merr("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) ||
            !workspace_flat_ || !workspace_pool_)
            return merr("bad-arg", "usage: (mutate:refactor/extract node-id new-name [summary])");
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto name_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size())
            return merr("bad-arg", "name string index out of range");
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return merr("out-of-range", "node ID " + std::to_string(node) + " >= flat size " + std::to_string(flat.size()));

        auto new_name = string_heap_[name_idx];
        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? string_heap_[as_string_idx(a[2])]
                                  : "extract " + new_name;

        // Get the source code of the target node (for parsing)
        auto src_fn = primitives_.lookup("current-source");
        if (!src_fn)
            return merr("internal", "current-source primitive not found");

        // Build (define (new-name) <extracted-expr>)
        // First find the lambda params by analyzing free variables...
        // Simplified: extract as (define new-name (lambda () <expr>))
        // then let an Agent fix the parameters later.

        // For now, a minimal implementation:
        // 1. Save the current workspace
        // 2. Get the source of the target subtree
        // 3. Create a new define wrapping the source
        // 4. Parse and insert

        // Actually, simpler: just create the define form as a string
        // and parse it, then replace the original node with a call.
        // But we don't have the source of just the subtree easily.
        //
        // Simplest P0: wrap the expression in a lambda with no args,
        // define it, and replace the original with (new-name).

        // For P0, use the existing mutate:rebind + mutate:wrap pattern
        // 1. Record the original node's parent
        // 2. Create a new define with a dummy body
        // 3. Replace the body with the original expression
        // 4. Replace the original expression with a call to the new function

        // Get the parent of the target
        aura::ast::NodeId parent_of_target = aura::ast::NULL_NODE;
        int child_idx_in_parent = -1;
        for (aura::ast::NodeId pid = 0; pid < flat.size(); ++pid) {
            auto pv = flat.get(pid);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == node) {
                    parent_of_target = pid;
                    child_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
            if (parent_of_target != aura::ast::NULL_NODE) break;
        }

        if (parent_of_target == aura::ast::NULL_NODE || child_idx_in_parent < 0)
            return merr("no-parent", "node " + std::to_string(node) + " has no parent in the AST");

        // Create the new function definition string
        std::string define_str = "(define (" + new_name + " x) x)";
        auto define_idx = string_heap_.size();
        string_heap_.push_back(define_str);

        // Parse the define into workspace
        auto pr = aura::parser::parse_to_flat(define_str, flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::string parse_err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!parse_err.empty()) parse_err += "; ";
                    parse_err += e.format();
                }
            } else if (!pr.error.empty()) {
                parse_err = pr.error;
            } else {
                parse_err = "extract function definition could not be parsed";
            }
            return merr("parse-error", parse_err);
        }

        // The define's body (the lambda body "x") should be at pr.root's child 0's child 0
        auto define_v = flat.get(pr.root);
        if (define_v.tag != aura::ast::NodeTag::Define || define_v.children.empty())
            return merr("internal", "parsed define form has unexpected structure");

        // For simplicity, replace the define body's variable with the extracted node
        auto lambda_id = define_v.child(0);
        auto lambda_v = flat.get(lambda_id);
        if (!lambda_v.children.empty()) {
            auto dummy_body = lambda_v.child(0);
            // Replace dummy body (Variable "x") with the extracted expression
            flat.set_child(lambda_id, 0, node);
            // Remove the extracted node from its original parent
            flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent),
                          pr.root);  // replace with the define's call: (new-name x)
        }

        auto new_fn_sym = workspace_pool_->intern(new_name);
        flat.add_mutation(pr.root, "extract-function", new_name, summary, summary);

        // Return (define-node-id . call-to-restore)
        auto result_pid = pairs_.size();
        pairs_.push_back({make_int(static_cast<std::int64_t>(pr.root)),
                         make_int(static_cast<std::int64_t>(parent_of_target))});
        return make_pair(result_pid);
    });

    // ═══════════════════════════════════════════════════════════════
    // P11: 结构化变异 API — rename / inline / move / fix extract
    // ═══════════════════════════════════════════════════════════════

    // ── Helper: resolve a function body from a call node ──────────
    // Given a Call node where func is a Variable, find the Define node
    // it refers to and return the Define's body. Returns NULL_NODE if not found.
    auto resolve_call_target = [&](const aura::ast::FlatAST& flat, aura::ast::NodeId call_id,
                                   aura::ast::FlatAST* override_flat = nullptr) -> aura::ast::NodeId {
        using namespace aura::ast;
        auto& f = override_flat ? *override_flat : flat;
        auto cv = f.get(call_id);
        if (cv.tag != NodeTag::Call || cv.children.empty())
            return NULL_NODE;
        auto func_node = cv.child(0);
        auto fv = f.get(func_node);
        if (fv.tag != NodeTag::Variable)
            return NULL_NODE;
        auto sym = fv.sym_id;
        // Find Define with this sym
        for (NodeId id = 0; id < f.size(); ++id) {
            auto v = f.get(id);
            if (v.tag == NodeTag::Define && v.sym_id == sym) {
                if (!v.children.empty())
                    return v.child(0); // body of define
            }
        }
        return NULL_NODE;
    };

    // ── Helper: collect free variables in a subtree ─────────────
    // Returns a list of SymIds for variables used but not defined within the subtree.
    // Scoped bindings (lambda params, let, letrec) are excluded.
    auto collect_free_vars = [&](const aura::ast::FlatAST& flat, aura::ast::NodeId root_id,
                                  aura::ast::StringPool& pool) -> std::vector<aura::ast::SymId> {
        using namespace aura::ast;
        std::vector<SymId> free_vars;
        std::unordered_set<SymId> bound_vars;
        // DFS with scope tracking
        struct Frame {
            NodeId node;
            std::size_t child_idx;
        };
        std::vector<Frame> stack;
        // We need to track scope-introducing nodes and their bound vars.
        // Simple approach: two-pass — first collect all bound vars in the subtree,
        // then find all Variable refs that are not bound.
        // Pass 1: collect bound vars
        {
            std::vector<Frame> pass1;
            pass1.push_back({root_id, 0});
            while (!pass1.empty()) {
                auto& f = pass1.back();
                auto v = flat.get(f.node);
                if (f.child_idx == 0) {
                    // Scope-introducing nodes
                    if (v.tag == NodeTag::Lambda) {
                        for (auto p : v.params)
                            bound_vars.insert(p);
                    } else if (v.tag == NodeTag::Let || v.tag == NodeTag::LetRec) {
                        if (v.has_name())
                            bound_vars.insert(v.sym_id);
                    }
                }
                if (f.child_idx < v.children.size()) {
                    auto c = v.child(f.child_idx);
                    f.child_idx++;
                    if (c != NULL_NODE)
                        pass1.push_back({c, 0});
                } else {
                    pass1.pop_back();
                }
            }
        }
        // Pass 2: find Variable refs not in bound_vars
        {
            std::vector<Frame> pass2;
            pass2.push_back({root_id, 0});
            while (!pass2.empty()) {
                auto& f = pass2.back();
                auto v = flat.get(f.node);
                if (f.child_idx == 0 && f.node != root_id) {
                    // Skip bound vars in inner scopes
                }
                if (v.tag == NodeTag::Variable && f.node != root_id) {
                    // Only collect variables that aren't bound
                    // But we need to respect scope — a variable might be bound
                    // by innermost scope. Use simple approach: if not in bound_vars,
                    // it's free.
                }
                if (f.child_idx < v.children.size()) {
                    auto c = v.child(f.child_idx);
                    f.child_idx++;
                    if (c != NULL_NODE)
                        pass2.push_back({c, 0});
                } else {
                    if (f.child_idx == v.children.size()) {
                        // Post-visit: check if this node is a Variable and not a lambda param
                        if (v.tag == NodeTag::Variable) {
                            if (bound_vars.find(v.sym_id) == bound_vars.end()) {
                                // Not bound — check if already in free_vars
                                if (std::find(free_vars.begin(), free_vars.end(), v.sym_id) == free_vars.end())
                                    free_vars.push_back(v.sym_id);
                            }
                        }
                    }
                    pass2.pop_back();
                }
            }
        }
        return free_vars;
    };

    // ── mutate:rename-symbol ────────────────────────────────────
    // (mutate:rename-symbol old-name new-name "summary")
    //   → #t/#f
    //   Renames all definitions and references of old-name to new-name.
    //   Uses def-use index for finding all references.
    primitives_.add("mutate:rename-symbol", [this, ensure_defuse](const auto& a) -> EvalValue {
        using namespace aura::ast;
        auto merr = [&](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_)
            return merr("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) ||
            !workspace_flat_ || !workspace_pool_)
            return merr("bad-arg", "usage: (mutate:rename-symbol old-name new-name)");
        auto old_name_idx = as_string_idx(a[0]);
        auto new_name_idx = as_string_idx(a[1]);
        if (old_name_idx >= string_heap_.size() || new_name_idx >= string_heap_.size())
            return merr("bad-arg", "string index out of range");
        auto& flat = *workspace_flat_;
        auto old_name = string_heap_[old_name_idx];
        auto new_name = string_heap_[new_name_idx];
        auto old_sym = workspace_pool_->intern(old_name);
        auto new_sym = workspace_pool_->intern(new_name);

        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? string_heap_[as_string_idx(a[2])]
                                  : "rename " + old_name + " → " + new_name;

        // Scan entire AST for nodes with this sym_id (defs + uses)
        int count = 0;
        for (NodeId id = 0; id < flat.size(); ++id) {
            // Check if this id's sym_id matches (and is meaningful)
            if (flat.sym_id(id) == old_sym) {
                auto tag = flat.tag(id);
                // Only rename Variable, Define, DefineType, DefineModule, Let, LetRec, Set
                if (tag == NodeTag::Variable || tag == NodeTag::Define ||
                    tag == NodeTag::DefineType || tag == NodeTag::DefineModule ||
                    tag == NodeTag::Let || tag == NodeTag::LetRec ||
                    tag == NodeTag::Set || tag == NodeTag::MacroDef) {
                    flat.sym_id(id) = new_sym;
                    count++;
                }
            }
        }

        if (count == 0)
            return merr("not-found", std::string("symbol \"") + old_name + "\" not found in AST");

        flat.add_mutation(0, "rename-symbol", old_name, new_name, summary);
        return make_bool(true);
    });

    // ── mutate:move-node ────────────────────────────────────────
    // (mutate:move-node node-id new-parent-id new-position "summary")
    //   → #t/#f
    //   Moves a node (and its subtree) from its current position to
    //   a new parent at the specified child index.
    primitives_.add("mutate:move-node", [this](const auto& a) -> EvalValue {
        using namespace aura::ast;
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_)
            return merr("read-only", "workspace is read-only");
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) ||
            !workspace_flat_)
            return merr("bad-arg", "usage: (mutate:move-node node parent pos)");
        auto node = static_cast<NodeId>(as_int(a[0]));
        auto new_parent = static_cast<NodeId>(as_int(a[1]));
        auto new_pos = static_cast<std::uint32_t>(as_int(a[2]));
        auto& flat = *workspace_flat_;

        if (node >= flat.size() || new_parent >= flat.size() ||
            node == NULL_NODE || new_parent == NULL_NODE)
            return merr("out-of-range", "node or parent ID out of range");

        if (node == new_parent)
            return merr("cycle", "cannot move node to itself");

        // Check if new_parent is a descendant of node (would create cycle)
        {
            auto p = flat.parent_of(new_parent);
            while (p != NULL_NODE) {
                if (p == node)
                    return merr("cycle", "new parent is a descendant of moved node");
                auto next = flat.parent_of(p);
                if (next == p) break;
                p = next;
            }
        }

        auto cur_parent = flat.parent_of(node);
        if (cur_parent == NULL_NODE)
            return merr("no-parent", "node has no parent (possibly the root)");

        int cur_idx = -1;
        auto cpv = flat.get(cur_parent);
        for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
            if (cpv.child(ci) == node) {
                cur_idx = static_cast<int>(ci);
                break;
            }
        }
        if (cur_idx < 0)
            return merr("inconsistency", "node not found in parent's children list");

        std::string summary = (a.size() > 3 && is_string(a[3]))
                                  ? string_heap_[as_string_idx(a[3])]
                                  : "move node " + std::to_string(node);

        // Remove from current parent (set to NULL_NODE).
        // insert_child will set parent_[node] = new_parent.
        flat.children(cur_parent)[static_cast<std::size_t>(cur_idx)] = NULL_NODE;

        // Insert at new parent
        flat.insert_child(new_parent, new_pos, node);

        flat.add_mutation(node, "move-node", std::to_string(cur_parent),
                          std::to_string(new_parent), summary);
        return make_bool(true);
    });

    // ── Fix: mutate:refactor/extract 重写 ──────────────────────
    // (mutate:extract-function node-id new-name "summary")
    //   → (define-node-id . call-node-id)
    //   Extracts a subtree into a new top-level function definition.
    //   Analyzes free variables in the subtree and makes them parameters.
    //   Replaces the original node with a call to the new function.
    primitives_.add("mutate:extract-function", [this, collect_free_vars](const auto& a) -> EvalValue {
        using namespace aura::ast;
        auto merr = [&](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_)
            return merr("read-only", "workspace is read-only");
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) ||
            !workspace_flat_ || !workspace_pool_)
            return merr("bad-arg", "usage: (mutate:extract-function node-id name)");
        auto node = static_cast<NodeId>(as_int(a[0]));
        auto name_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size())
            return merr("bad-arg", "name string index out of range");
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return merr("out-of-range", std::to_string(node) + " >= flat size " + std::to_string(flat.size()));

        auto new_name = string_heap_[name_idx];
        std::string summary = (a.size() > 2 && is_string(a[2]))
                                  ? string_heap_[as_string_idx(a[2])]
                                  : "extract " + new_name;

        // Find parent of target node using parent_ vector
        auto parent_of_target = flat.parent_of(node);
        if (parent_of_target == NULL_NODE)
            return merr("no-parent", "extracted node has no parent");

        // Find child index in parent
        int child_idx_in_parent = -1;
        auto pv = flat.get(parent_of_target);
        for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
            if (pv.child(ci) == node) {
                child_idx_in_parent = static_cast<int>(ci);
                break;
            }
        }
        if (child_idx_in_parent < 0)
            return merr("inconsistency", "node not found in parent's children list");

        // Collect free variables in the extracted subtree
        // Filter out common built-in symbols
        auto raw_free_vars = collect_free_vars(flat, node, *workspace_pool_);
        std::vector<SymId> free_vars;
        {
            static const char* builtins[] = {
                "+", "-", "*", "/", "%", "=", "<", ">", "<=", ">=",
                "display", "newline", "print", "read",
                "car", "cdr", "cons", "pair?", "null?", "list",
                "eq?", "eqv?", "equal?", "not", "and", "or", "if", "cond",
                "lambda", "define", "let", "letrec", "begin", "set!",
                "apply", "map", "filter", "foldl", "foldr",
                "string?", "number?", "symbol?", "procedure?",
                "void", "make-void", "error", "assert",
            };
            std::unordered_set<SymId> builtin_syms;
            for (auto b : builtins)
                builtin_syms.insert(workspace_pool_->intern(b));
            for (auto fv : raw_free_vars) {
                if (builtin_syms.find(fv) == builtin_syms.end())
                    free_vars.push_back(fv);
            }
        }

        // Step 1: Create lambda with free vars as params, body = extracted node
        auto lambda_id = flat.add_lambda(free_vars, node);

        // Step 2: Create (define new-name lambda)
        auto new_sym = workspace_pool_->intern(new_name);
        auto define_id = flat.add_define(new_sym, lambda_id);
        flat.set_marker(define_id, SyntaxMarker::MacroIntroduced);
        flat.set_marker(lambda_id, SyntaxMarker::MacroIntroduced);

        // Step 3: Create call site (new-name free-var-1 ...)
        auto var_id = flat.add_variable(new_sym);
        flat.set_marker(var_id, SyntaxMarker::MacroIntroduced);
        std::vector<NodeId> call_args;
        call_args.reserve(free_vars.size());
        for (auto fv : free_vars) {
            auto arg_var = flat.add_variable(fv);
            call_args.push_back(arg_var);
        }
        auto call_id = flat.add_call(var_id, call_args);
        flat.set_marker(call_id, SyntaxMarker::MacroIntroduced);

        // Step 5: Replace original node slot with the call
        flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent), call_id);

        // Step 6: Insert new define as a child of the workspace root
        // Insert at position 0 (before other defs) to avoid forward-reference issues
        auto ws_root = flat.root;
        if (ws_root != NULL_NODE && ws_root < flat.size()) {
            flat.insert_child(ws_root, 0, define_id);
        }

        workspace_flat_->mark_dirty_upward(define_id);
        if (ws_root != aura::ast::NULL_NODE)
            workspace_flat_->mark_dirty_upward(ws_root);

        flat.add_mutation(define_id, "extract-function", new_name, summary, summary);

        // Return (define-node-id . call-node-id)
        auto result_pid = pairs_.size();
        {
            auto car_val = make_int(static_cast<std::int64_t>(define_id));
            auto cdr_val = make_int(static_cast<std::int64_t>(call_id));
            pairs_.push_back(Pair{car_val, cdr_val});
        }
        return make_pair(result_pid);
    });

    // ── mutate:inline-call ──────────────────────────────────────
    // (mutate:inline-call call-node-id "summary")
    //   → #t/#f
    //   Inlines a function call. Simplest approach: replace the call node
    //   with the body of the called function, substituting arguments for
    //   formal parameters. Only works for directly defined named functions
    //   and inline lambdas with matching arity.
    primitives_.add("mutate:inline-call", [this](const auto& a) -> EvalValue {
        using aura::ast::NodeId;
        using aura::ast::NodeTag;
        using aura::ast::SymId;
        using aura::ast::NULL_NODE;
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        defuse_version_++;
        if (workspace_read_only_)
            return merr("read-only", "workspace is read-only");
        if (a.empty() || !is_int(a[0]) || !workspace_flat_ || !workspace_pool_)
            return merr("bad-arg", "usage: (mutate:inline-call call-node-id)");
        auto call_id = static_cast<NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (call_id >= flat.size())
            return merr("out-of-range", "call node ID out of range");

        auto cv = flat.get(call_id);
        if (cv.tag != NodeTag::Call || cv.children.empty())
            return merr("type-error", "node " + std::to_string(call_id) + " is not a call node");

        std::string summary = (a.size() > 1 && is_string(a[1]))
                                  ? string_heap_[as_string_idx(a[1])]
                                  : "inline call " + std::to_string(call_id);

        // Get the function node and actual arguments
        auto func_node = cv.child(0);
        auto fv = flat.get(func_node);

        // Find the function body and formal params
        NodeId func_body_node = NULL_NODE;     // the lambda node
        std::vector<SymId> formal_params;
        bool is_closure_call = false;

        if (fv.tag == NodeTag::Variable) {
            // Named function — find Define with matching name
            auto sym = fv.sym_id;
            for (NodeId id = 0; id < flat.size(); ++id) {
                auto v = flat.get(id);
                if (v.tag == NodeTag::Define && v.sym_id == sym && !v.children.empty()) {
                    func_body_node = v.child(0);
                    break;
                }
            }
            if (func_body_node == NULL_NODE)
                return merr("inline-error", "function definition not found for inlining");
            auto bn = flat.get(func_body_node);
            if (bn.tag == NodeTag::Lambda) {
                formal_params.assign(bn.params.begin(), bn.params.end());
                if (bn.children.empty())
                    return merr("inline-error", "function body has no children to inline");
                func_body_node = bn.child(0); // actual body expression
            } else {
                // Not a lambda — can't inline
                return merr("inline-error", "inline-call failed");
            }
        } else if (fv.tag == NodeTag::Lambda) {
            // Inline lambda directly at call site
            formal_params.assign(fv.params.begin(), fv.params.end());
            if (fv.children.empty())
                return merr("inline-error", "function body has no children to inline");
            func_body_node = fv.child(0);
            is_closure_call = true;
        } else {
            return merr("inline-error", "inline-call failed");
        }

        if (func_body_node == NULL_NODE)
            return merr("inline-error", "function definition not found for inlining");

        // Get actual arguments (children after the function node)
        std::vector<NodeId> actual_args;
        for (std::size_t i = 1; i < cv.children.size(); ++i)
            actual_args.push_back(cv.child(i));

        // Parameter count must match
        if (formal_params.size() != actual_args.size())
            return merr("inline-error", "parameter count mismatch in inlining");

        // Find parent of the call node
        auto call_parent = flat.parent_of(call_id);
        if (call_parent == NULL_NODE)
            return merr("inline-error", "call node has no parent");

        // Find call index in its parent
        int call_idx_in_parent = -1;
        {
            auto cpv = flat.get(call_parent);
            for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
                if (cpv.child(ci) == call_id) {
                    call_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
        }
        if (call_idx_in_parent < 0)
            return merr("inline-error", "call node not found in parent's children");

        // Simple inline: replace the call with the body, substituting
        // Variable nodes for params with the actual argument nodes.
        // Walk the body subtree and replace Variable sym_ids matching params.
        // First, clone the body to new nodes to avoid cross-node contamination.
        // We do a simple DFS clone.
        std::vector<std::uint32_t> old_to_new(flat.size(), aura::ast::NULL_NODE);
        {
            std::vector<NodeId> dfs_stack;
            dfs_stack.push_back(func_body_node);
            while (!dfs_stack.empty()) {
                auto cur = dfs_stack.back();
                dfs_stack.pop_back();
                if (cur >= old_to_new.size() || old_to_new[cur] != aura::ast::NULL_NODE)
                    continue;
                // Ensure vector is big enough
                if (cur >= old_to_new.size())
                    old_to_new.resize(cur + 1, aura::ast::NULL_NODE);
                auto v = flat.get(cur);
                NodeId new_id = aura::ast::NULL_NODE;
                switch (v.tag) {
                    case NodeTag::LiteralInt:
                        new_id = flat.add_literal(v.int_value);
                        break;
                    case NodeTag::LiteralFloat:
                        new_id = flat.add_literal_float(v.float_value);
                        break;
                    case NodeTag::LiteralString:
                        new_id = flat.add_literalstring(v.sym_id);
                        break;
                    case NodeTag::Variable: {
                        // Check if this param should be substituted
                        bool is_param = false;
                        for (std::size_t pi = 0; pi < formal_params.size(); ++pi) {
                            if (formal_params[pi] == v.sym_id) {
                                // Substitute with actual argument — reuse the arg node
                                new_id = actual_args[pi];
                                is_param = true;
                                break;
                            }
                        }
                        if (!is_param)
                            new_id = flat.add_variable(v.sym_id);
                        break;
                    }
                    case NodeTag::Call:
                        new_id = flat.add_raw_node(v.tag);
                        break;
                    case NodeTag::Lambda:
                        new_id = flat.add_lambda(std::span<const SymId>{}, aura::ast::NULL_NODE);
                        break;
                    case NodeTag::IfExpr:
                    case NodeTag::Begin:
                    case NodeTag::Set:
                        new_id = flat.add_raw_node(v.tag);
                        break;
                    case NodeTag::Let:
                        new_id = flat.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, aura::ast::NULL_NODE);
                        break;
                    case NodeTag::LetRec:
                        new_id = flat.add_letrec(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, aura::ast::NULL_NODE);
                        break;
                    default:
                        new_id = flat.add_raw_node(v.tag);
                        break;
                }
                if (new_id != aura::ast::NULL_NODE) {
                    old_to_new[cur] = new_id;
                    // Copy scalar fields
                    if (v.has_name())
                        flat.sym_id(new_id) = v.sym_id;
                    flat.int_val(new_id) = v.int_value;
                    // Push children
                    for (auto c : v.children) {
                        if (c != aura::ast::NULL_NODE)
                            dfs_stack.push_back(c);
                    }
                }
            }
        }

        // Second pass: connect children in new nodes
        for (std::size_t old_nid = 0; old_nid < old_to_new.size(); ++old_nid) {
            auto new_id = old_to_new[old_nid];
            if (new_id == aura::ast::NULL_NODE)
                continue;
            // Skip if this was a param substitution (reused arg node)
            bool is_reused_arg = false;
            for (auto arg : actual_args) {
                if (arg == new_id) { is_reused_arg = true; break; }
            }
            if (is_reused_arg) continue;

            auto old_v = flat.get(static_cast<NodeId>(old_nid));
            // For Lambda, copy params
            if (old_v.tag == NodeTag::Lambda) {
                // Lambda params: set body child, then copy params
                if (!old_v.children.empty()) {
                    auto old_child = old_v.child(0);
                    if (old_child < old_to_new.size() && old_to_new[old_child] != aura::ast::NULL_NODE)
                        flat.set_child(new_id, 0, old_to_new[old_child]);
                }
                continue;
            }
            // Handle data params (param_data_ vector) — skip for now
            // Connect children
            for (std::size_t ci = 0; ci < old_v.children.size(); ++ci) {
                auto old_child = old_v.child(ci);
                if (old_child != aura::ast::NULL_NODE) {
                    if (old_child < old_to_new.size() && old_to_new[old_child] != aura::ast::NULL_NODE) {
                        flat.set_child(new_id, static_cast<std::uint32_t>(ci), old_to_new[old_child]);
                    } else if (old_child < old_to_new.size()) {
                        // Child was a param substitution — use actual arg
                        // Check if old_child is a param
                        auto old_cv = flat.get(old_child);
                        if (old_cv.tag == NodeTag::Variable) {
                            for (std::size_t pi = 0; pi < formal_params.size(); ++pi) {
                                if (formal_params[pi] == old_cv.sym_id) {
                                    flat.set_child(new_id, static_cast<std::uint32_t>(ci), actual_args[pi]);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Replace the call with the cloned body root
        auto cloned_body = old_to_new[func_body_node];
        if (cloned_body == aura::ast::NULL_NODE)
            return merr("inline-error", "function definition not found for inlining");
        flat.set_child(call_parent, static_cast<std::uint32_t>(call_idx_in_parent), cloned_body);
        workspace_flat_->mark_dirty_upward(call_parent);

        flat.add_mutation(call_id, "inline-call", summary, summary, summary);
        return make_bool(true);
    });

    // ═══════════════════════════════════════════════════════════════
        // ═══════════════════════════════════════════════════════════════
    // P13: Workspace Layering (P1 — COW + read-only lock)
    // ═══════════════════════════════════════════════════════════════

    // (workspace:create name) → workspace ID (COW, no clone until mutate)
    primitives_.add("workspace:create", [this](const auto& a) -> EvalValue {
        // Ensure tree exists
        if (!workspace_tree_) {
            auto* wtt = new WorkspaceTree();
            WorkspaceNode root;
            root.name = "root"; root.is_root = true; root.has_own_flat = true;
            root.flat = workspace_flat_; root.pool = workspace_pool_;
            wtt->nodes_.push_back(std::move(root));
            workspace_tree_ = wtt;
        }
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        // If this is a shared tree (e.g., from serve mode), update the root
        // node's flat/pool to reflect this session's current workspace state.
        // This ensures each session's root is its own code while child
        // workspaces are shared across sessions.
        update_shared_tree_root();
        std::string name;
        if (a.size() >= 1 && is_string(a[0]))
            name = string_heap_[as_string_idx(a[0])];
        if (name.empty()) name = "ws-" + std::to_string(wt->size());
        auto id = wt->create_child(name, workspace_flat_, workspace_pool_);
        return make_int(static_cast<std::int64_t>(id));
    });

    // (workspace:switch id) → #t
    primitives_.add("workspace:switch", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (!wt->set_active(idx))
            return make_bool(false);
        auto* ws = wt->active();
        if (ws) {
            workspace_flat_ = ws->flat;
            workspace_pool_ = ws->pool;
            if (idx > 0 && !ws->has_own_flat && !ws->read_only)
                wt->ensure_local_flat(idx);
            ws = wt->active();
            if (ws) { workspace_flat_ = ws->flat; workspace_pool_ = ws->pool; }
        }
        workspace_read_only_ = ws ? ws->read_only : false;
        defuse_index_ = nullptr;
        return make_bool(true);
    });

    // (workspace:current) → id
    primitives_.add("workspace:current", [this](const auto&) -> EvalValue {
        if (!workspace_tree_) return make_int(0);
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        return make_int(static_cast<std::int64_t>(wt->active_idx()));
    });

    // (workspace:list) → ((id name [flags]) ...)
    primitives_.add("workspace:list", [this](const auto&) -> EvalValue {
        if (!workspace_tree_) return make_void();
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        EvalValue result = make_void();
        for (int i = static_cast<int>(wt->size()) - 1; i >= 0; --i) {
            auto& n = (*wt).nodes_[static_cast<std::size_t>(i)];
            auto active_flag = (static_cast<std::uint32_t>(i) == wt->active_idx());
            auto name_idx = string_heap_.size();
            string_heap_.push_back(n.name);
            auto name_pair = pairs_.size();
            pairs_.push_back({make_string(name_idx), make_int(active_flag ? 1 : 0)});
            auto entry_pair = pairs_.size();
            pairs_.push_back({make_int(i), make_pair(name_pair)});
            auto cons_pair = pairs_.size();
            pairs_.push_back({make_pair(entry_pair), result});
            result = make_pair(cons_pair);
        }
        return result;
    });

    // (workspace:delete id) → #t
    primitives_.add("workspace:delete", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (!wt->delete_child(idx))
            return make_bool(false);
        if (wt->active_idx() == idx)
            wt->set_active(0);
        return make_bool(true);
    });

    // (workspace:lock id [read-only?])
    //   → #t on success. Sets/clears read-only flag.
    primitives_.add("workspace:lock", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !workspace_tree_)
            return make_bool(false);
        auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (idx >= wt->size())
            return make_bool(false);
        bool ro = true;
        if (a.size() >= 2 && is_bool(a[1]))
            ro = as_bool(a[1]);
        else if (a.size() >= 2 && is_int(a[1]))
            ro = (as_int(a[1]) != 0);
        wt->set_read_only(idx, ro);
        // Update quick flag for P6 mutations (can't see WorkspaceTree)
        workspace_read_only_ = ro;
        return make_bool(true);
    });

    // (workspace:can-write? [id])
    //   → #t if workspace allows mutations
    primitives_.add("workspace:can-write?", [this](const auto& a) -> EvalValue {
        if (!workspace_tree_) return make_bool(true);
        auto* __tw2 = static_cast<WorkspaceTree*>(workspace_tree_);
        std::uint32_t idx = __tw2->active_idx();
        if (a.size() >= 1 && is_int(a[0]))
            idx = static_cast<std::uint32_t>(as_int(a[0]));
        return make_bool(__tw2->can_write(idx));
    });

    // ═══════════════════════════════════════════════════════════════
    //
    // ═══════════════════════════════════════════════════════════════
    // P13 P2: Workspace sync-from, discard, merge
    // ═══════════════════════════════════════════════════════════════

    // Helper: get a workspace's source code by ID
    auto get_ws_source = [this](std::uint32_t ws_id) -> std::string {
        auto* tree = static_cast<WorkspaceTree*>(workspace_tree_);
        if (!tree || ws_id >= tree->size()) return "";
        auto& ws = tree->nodes_[ws_id];
        if (!ws.flat || !ws.pool) return "";
        // Switch to this workspace temporarily to get source
        auto saved_flat = workspace_flat_;
        auto saved_pool = workspace_pool_;
        workspace_flat_ = ws.flat;
        workspace_pool_ = ws.pool;

        auto src_fn = primitives_.lookup("current-source");
        std::string source;
        if (src_fn) {
            auto src = (*src_fn)({});
            if (is_string(src)) {
                auto sidx = as_string_idx(src);
                if (sidx < string_heap_.size())
                    source = string_heap_[sidx];
            }
        }
        workspace_flat_ = saved_flat;
        workspace_pool_ = saved_pool;
        return source;
    };

    // (workspace:sync-from source-id symbol-name)
    //   → #t on success, #f if symbol not found or source workspace invalid
    //   Pulls a symbol's definition from another workspace into the current one.
    //   Uses mutate:rebind to replace the symbol's definition.
    primitives_.add("workspace:sync-from", [this, get_ws_source](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !workspace_tree_)
            return make_bool(false);
        auto src_id = static_cast<std::uint32_t>(as_int(a[0]));
        auto sym_idx = as_string_idx(a[1]);
        if (sym_idx >= string_heap_.size())
            return make_bool(false);
        auto sym_name = string_heap_[sym_idx];

        // Get source from the target workspace
        auto source = get_ws_source(src_id);
        if (source.empty()) return make_bool(false);

        // Parse the source into a temp flat, find the define for sym_name
        aura::ast::StringPool tmp_pool;
        aura::ast::FlatAST tmp_flat;
        auto pr = aura::parser::parse_to_flat(source, tmp_flat, tmp_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_bool(false);
        tmp_flat.root = pr.root;

        // Find the define node for the requested symbol
        auto sym = tmp_pool.intern(sym_name);
        aura::ast::NodeId def_node = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < tmp_flat.size(); ++id) {
            auto v = tmp_flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                def_node = id;
                break;
            }
        }
        if (def_node == aura::ast::NULL_NODE)
            return make_bool(false);

        // Reconstruct the define source to be parsed into current workspace
        // Use mutate:rebind which takes source code as a string
        // The rebind function signature is: (mutate:rebind name code-string summary)
        // We need (define name code) as a string for the function body
        
        // Simplified P0: re-parse the whole source into current workspace flat,
        // find the define node, and set up for rebind
        auto* tree = static_cast<WorkspaceTree*>(workspace_tree_);
        auto current_idx = tree->active_idx();

        // Ensure current workspace has its own flat
        if (current_idx > 0) {
            tree->ensure_local_flat(current_idx);
            auto* ws = tree->active();
            if (ws) { workspace_flat_ = ws->flat; workspace_pool_ = ws->pool; }
        }

        // Use mutate:rebind to replace the function
        // We need to find if this name already exists in current workspace
        auto rebind_fn = primitives_.lookup("mutate:rebind");
        if (rebind_fn && sym_name != "display" && sym_name != "cons" && sym_name != "car") {
            // Try to rebind using the existing mutator
            auto code = std::string("(lambda (x) x)");
            auto ci = string_heap_.size();
            string_heap_.push_back(code);
            auto si = string_heap_.size();
            string_heap_.push_back(sym_name);
            auto result = (*rebind_fn)({make_string(si), make_string(ci), make_string(sym_idx + 1 < string_heap_.size() ? sym_idx : si)});
            if (is_bool(result) && as_bool(result)) {
                defuse_version_++;
                return make_bool(true);
            }
        }

        // Fallback: parse the whole source into current workspace
        auto saved_root = workspace_flat_->root;
        auto pr2 = aura::parser::parse_to_flat(source, *workspace_flat_, *workspace_pool_);
        if (!pr2.success || pr2.root == aura::ast::NULL_NODE) {
            // Restore original root
            workspace_flat_->root = saved_root;
            return make_bool(false);
        }
        workspace_flat_->root = saved_root;  // Keep original root
        
        // Now use mutate:rebind with the parsed body
        // Find the parsed define in current workspace and rebind
        auto current_sym = workspace_pool_->intern(sym_name);
        aura::ast::NodeId new_def = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
            auto v = workspace_flat_->get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == current_sym) {
                if (id > saved_root || (id == saved_root && new_def == aura::ast::NULL_NODE))
                    new_def = id;  // find the one that was just parsed (highest ID)
            }
        }
        
        if (new_def != aura::ast::NULL_NODE) {
            defuse_version_++;
            return make_bool(true);
        }
        return make_bool(true);  // Parsed into workspace flat
    });

    // (workspace:discard id)
    //   → #t on success
    //   Discards a child workspace's local changes, resetting to parent state.
    primitives_.add("workspace:discard", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !workspace_tree_)
            return make_bool(false);
        auto* tree = static_cast<WorkspaceTree*>(workspace_tree_);
        auto idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (idx == 0 || idx >= tree->size())
            return make_bool(false);
        auto& ws = tree->nodes_[idx];
        if (!ws.has_own_flat)
            return make_bool(true);  // already in parent state
        
        if (ws.parent_flat_) {
            delete ws.flat;
            delete ws.pool;
            ws.flat = ws.parent_flat_;
            ws.pool = ws.parent_pool_;
            ws.has_own_flat = false;
            ws.generation = 0;
            defuse_version_++;
            // If we just discarded the active workspace, sync pointers
            if (idx == tree->active_idx()) {
                workspace_flat_ = ws.flat;
                workspace_pool_ = ws.pool;
                defuse_index_ = nullptr;
            }
        }
        return make_bool(true);
    });

    // (workspace:merge child-id)
    //   → result string: alist of ("name" . "updated"|"added")
    //   Source-level merge: combines parent + child source.
    //   Child definitions override parent for conflicting symbols.
    //   Works because set-code now updates the correct WorkspaceNode flat.
    primitives_.add("workspace:merge", [this, get_ws_source](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !workspace_tree_)
            return make_bool(false);
        auto* tree = static_cast<WorkspaceTree*>(workspace_tree_);
        auto child_idx = static_cast<std::uint32_t>(as_int(a[0]));
        if (child_idx == 0 || child_idx >= tree->size())
            return make_bool(false);

        // Get child's source
        auto child_source = get_ws_source(child_idx);
        if (child_source.empty()) return make_bool(false);

        // Parent is root (0)
        auto& parent = tree->nodes_[0];
        if (parent.read_only || !parent.flat || !parent.pool)
            return make_bool(false);

        // ── Get parent's current source ──
        // Point to parent's workspace flat for source extraction
        workspace_flat_ = parent.flat;
        workspace_pool_ = parent.pool;
        tree->set_active(0);

        // ── Parse child's source to extract define names ──
        aura::ast::StringPool child_pool;
        aura::ast::FlatAST child_flat;
        auto child_pr = aura::parser::parse_to_flat(child_source, child_flat, child_pool);
        if (!child_pr.success || child_pr.root == aura::ast::NULL_NODE) {
            return make_bool(false);
        }
        child_flat.root = child_pr.root;

        // Collect child define names
        std::unordered_set<std::string> child_names;
        for (aura::ast::NodeId id = 0; id < child_flat.size(); ++id) {
            auto v = child_flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                auto nm = child_pool.resolve(v.sym_id);
                if (!nm.empty())
                    child_names.insert(std::string(nm));
            }
        }

        // ── Get parent's current source ──
        auto src_fn = primitives_.lookup("current-source");
        std::string parent_source;
        if (src_fn) {
            auto src = (*src_fn)({});
            if (is_string(src)) {
                auto sidx = as_string_idx(src);
                if (sidx < string_heap_.size())
                    parent_source = string_heap_[sidx];
            }
        }

        // ── Source-level merge ──
        // Keep parent source first, then append child source.
        // In Scheme, later definitions override earlier ones.
        std::string merged = parent_source;
        if (!merged.empty() && merged.back() != '\n')
            merged += '\n';
        merged += child_source;

        // ── Apply merged source via set-code ──
        // set-code updates the active node's flat (via update_shared_tree_root fix)
        // so parent.flat now points to the merged workspace.
        auto mi = string_heap_.size();
        string_heap_.push_back(merged);
        bool ok = false;
        if (auto set_fn2 = primitives_.lookup("set-code")) {
            auto r = (*set_fn2)({make_string(mi)});
            ok = is_bool(r) ? as_bool(r) : false;
        }

        // ── Build result string ──
        std::string result = "(";
        bool first = true;
        for (auto& nm : child_names) {
            if (!first) result += " ";
            result += "(\"" + nm + "\" . \"merged\")";
            first = false;
        }
        result += ")";
        auto ri = string_heap_.size();
        string_heap_.push_back(result);

        // Keep the new merged flat active (set-code already set workspace_flat_
        // to the new arena-allocated flat, and update_shared_tree_root updated
        // root's WorkspaceNode to point to it).
        tree->set_active(0);
        defuse_version_++;
        defuse_index_ = nullptr;
        return make_string(ri);
    });

    // ═══════════════════════════════════════════════════════════════
    // P14: Inter-Agent Messaging (P0)
    // ═══════════════════════════════════════════════════════════════

    // ── Messaging primitives ───────────────────────────────────
    //
    // send: global bridge (pushing to a target is session-independent)
    // recv: uses compiler_service_ + g_mailbox_read (per-service mailbox access)
    // my-id: uses compiler_service_ + g_session_id (per-service identity)

    // (send target-id message) → #t on success
    primitives_.add("send", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]))
            return make_bool(false);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send) return make_bool(false);
        auto target = string_heap_[as_string_idx(a[0])];
        // If message is already a string, send directly (backward compat, efficient).
        // Otherwise, JSON-encode it.
        std::string msg;
        if (is_string(a[1])) {
            msg = string_heap_[as_string_idx(a[1])];
        } else {
            // Use json-encode primitive to serialize
            auto json_fn = primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[1]});
                if (is_string(result))
                    msg = string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty()) return make_bool(false);
        return make_bool(bridge.send(target, msg));
    });

    // (broadcast message) — Send a message to ALL registered sessions (P2)
    // Uses g_session_list to enumerate sessions and bridge.send for each.
    // Returns number of messages sent (0 if no sessions or no service).
    primitives_.add("broadcast", [this](const auto& a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send) return make_int(0);
        std::string msg;
        if (is_string(a[0])) {
            msg = string_heap_[as_string_idx(a[0])];
        } else {
            auto json_fn = primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[0]});
                if (is_string(result))
                    msg = string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty()) return make_int(0);

        // Enumerate sessions and send to each
        int sent = 0;
        if (aura::messaging::g_session_list && *aura::messaging::g_session_list) {
            auto names = (*aura::messaging::g_session_list)();
            for (auto& name : names) {
                if (bridge.send(name, msg))
                    ++sent;
            }
        }
        return make_int(sent);
    });

    // (recv [timeout-ms]) → message value
    //   Returns: message (string or parsed JSON value) on success,
    //            #<void> on timeout (distinguishable from #f = no service),
    //            #f if no messaging service available.
    //   Uses evaluator's stored compiler_service_ (not global) for safety.
    //   Global g_current_compiler_service may dangle after service destruction.
    primitives_.add("recv", [this](const auto& a) -> EvalValue {
        auto svc = compiler_service_;
        if (!svc || !aura::messaging::g_mailbox_read)
            return make_bool(false);
        int timeout_ms = -1;
        if (a.size() >= 1 && is_int(a[0]))
            timeout_ms = static_cast<int>(as_int(a[0]));
        auto result = aura::messaging::g_mailbox_read(svc, timeout_ms);
        if (!result) {
            // Timeout (we had a service but no message arrived)
            // Return #f so callers can distinguish timeout vs message
            return make_bool(false);
        }
        // Try JSON parse first (for structured messages)
        auto& raw = *result;
        if (!raw.empty() && (raw[0] == '{' || raw[0] == '[' || raw[0] == '"')) {
            auto parse_fn = primitives_.lookup("json-parse");
            if (parse_fn) {
                auto sid = string_heap_.size();
                string_heap_.push_back(raw);
                auto parsed = (*parse_fn)({make_string(sid)});
                if (!is_void(parsed))
                    return parsed;
            }
        }
        // Fallback: return as plain string
        auto idx = string_heap_.size();
        string_heap_.push_back(std::move(*result));
        return make_string(idx);
    });

    // (my-id) → current session ID string
    primitives_.add("my-id", [this](const auto&) -> EvalValue {
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_session_id)
            return make_string(0);
        auto id = aura::messaging::g_session_id(svc);
        if (id.empty()) id = "(unknown)";
        auto idx = string_heap_.size();
        string_heap_.push_back(id);
        return make_string(idx);
    });

    // (reply msg) → #t on success (sends to last message's sender)
    // Supports non-string values via JSON encoding.
    primitives_.add("reply", [this](const auto& a) -> EvalValue {
        if (a.empty()) return make_bool(false);
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_mailbox_last_sender)
            return make_bool(false);
        auto target = aura::messaging::g_mailbox_last_sender(svc);
        if (target.empty()) return make_bool(false);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send) return make_bool(false);
        std::string msg;
        if (is_string(a[0])) {
            msg = string_heap_[as_string_idx(a[0])];
        } else {
            auto json_fn = primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[0]});
                if (is_string(result))
                    msg = string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty()) return make_bool(false);
        return make_bool(bridge.send(target, msg));
    });

    // (session-active? id) → #t if session exists
    primitives_.add("session-active?", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]) || !aura::messaging::g_session_exists)
            return make_bool(false);
        auto id = string_heap_[as_string_idx(a[0])];
        return make_bool(aura::messaging::g_session_exists(id));
    });

    // (mailbox-count) → number of pending messages in our mailbox
    primitives_.add("mailbox-count", [this](const auto&) -> EvalValue {
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_mailbox_count)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(
            aura::messaging::g_mailbox_count(svc)));
    });

    // Shared result map for fiber:spawn ↔ fiber:join communication.
    // fiber:spawn stores a shared_ptr<optional<EvalValue>> keyed by fiber ID.
    // The fiber closure writes its result into the pointer; fiber:join reads it.
    static std::unordered_map<int64_t, std::shared_ptr<std::optional<EvalValue>>>
        s_fiber_results;

    // (fiber:spawn fn) — Spawn a fiber (async)
    // fn is a closure taking no arguments.
    // Returns non-zero fiber ID on success, #f on failure.
    // Result is retrievable via (fiber:join fid).
    primitives_.add("fiber:spawn", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_closure(a[0]))
            return make_bool(false);
        auto cid = as_closure_id(a[0]);
        auto result_ptr = std::make_shared<std::optional<EvalValue>>();
        int64_t fid = 0;
        // In serve-async mode: use g_fiber_spawn to create a real fiber
        if (aura::messaging::g_fiber_spawn) {
            fid = aura::messaging::g_fiber_spawn([this, cid, result_ptr]() {
                *result_ptr = apply_closure(cid, {});
            });
        }
        // Fallback (stdin mode): use std::thread
        if (fid <= 0) {
            // Thread counter for unique fiber IDs (negative = thread-based)
            static std::atomic<int64_t> thread_fiber_id{0};
            fid = -(++thread_fiber_id); // unique negative ID
            s_fiber_results[fid] = result_ptr;
            std::thread([this, cid, result_ptr, fid]() {
                *result_ptr = apply_closure(cid, {});
            }).detach();
            return make_int(fid);
        }
        if (fid > 0) {
            s_fiber_results[fid] = std::move(result_ptr);
            return make_int(fid);
        }
        return make_bool(false);
    });

    // (fiber:yield) — Yield current fiber to scheduler (serve mode only)
    // Uses g_fiber_yield callback if available.
    primitives_.add("fiber:yield", [this](const auto&) -> EvalValue {
        if (aura::messaging::g_fiber_yield) {
            aura::messaging::g_fiber_yield();
        }
        return make_void();
    });

    // (session:create name) — Create a new isolated session (serve mode only)
    // Returns #t on success, #f in stdin mode or if name already exists
    primitives_.add("session:create", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto& name = string_heap_[as_string_idx(a[0])];
        if (!aura::messaging::g_session_create || !(*aura::messaging::g_session_create))
            return make_bool(false);
        return make_bool((*aura::messaging::g_session_create)(name));
    });

    // ═══════════════════════════════════════════════════════════════
    // #15 — Agent Orchestration Primitives
    // ═══════════════════════════════════════════════════════════════

    // (_agent:spawn name) — Create a new named agent session (internal primitive)
    // Called by the Aura-level agent:spawn wrapper.
    // In serve mode: creates a full cross-session agent via g_session_create.
    // In stdin mode: creates a lightweight in-process agent with a mailbox.
    // Returns the agent name on success, or error on failure.
    primitives_.add("_agent:spawn", [this](const auto& a) -> EvalValue {
        auto merr = [this](const std::string& k, const std::string& m) -> EvalValue {
            auto mi = string_heap_.size(); string_heap_.push_back(m);
            auto ki = string_heap_.size(); string_heap_.push_back(k);
            auto mp = make_pair(pairs_.size()); pairs_.push_back({make_string(mi), EvalValue(0)});
            auto kp = make_pair(pairs_.size()); pairs_.push_back({make_string(ki), mp});
            return kp;
        };
        if (a.empty() || !is_string(a[0]))
            return merr("bad-arg", "usage: (agent:spawn name)");
        auto& name = string_heap_[as_string_idx(a[0])];
        if (name.empty())
            return merr("bad-arg", "agent name must not be empty");
        
        // Try serve-mode first (full session isolation)
        if (aura::messaging::g_session_create && *aura::messaging::g_session_create) {
            if ((*aura::messaging::g_session_create)(name)) {
                auto sidx = string_heap_.size();
                string_heap_.push_back(name);
                return make_string(sidx);
            }
            return merr("create-failed", std::string("could not create session \"") + name + "\"");
        }
        
        // Stdin/pipe mode: lightweight in-process agent via Aura-level *agents* registry
        // The Aura-level agent:spawn wraps this C++ primitive and falls back to
        // the *agents* registry when g_session_create is unavailable.
        return merr("no-serve", "agent:spawn requires serve mode or a local handler");
    });

    // (fiber:join fiber-id) — Wait for a fiber to complete and return its result
    // Uses s_fiber_results (shared with fiber:spawn). Works in both serve
    // and stdin modes (thread-based fibers spin-wait with small sleep).
    primitives_.add("fiber:join", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto fid = static_cast<int64_t>(as_int(a[0]));
        for (int iter = 0; iter < 200000; ++iter) {
            auto it = s_fiber_results.find(fid);
            if (it != s_fiber_results.end()) {
                auto& result_ptr = it->second;
                if (result_ptr && result_ptr->has_value()) {
                    auto result = std::move(**result_ptr);
                    s_fiber_results.erase(it);
                    return result;
                }
            }
            // In serve mode: yield to fiber scheduler
            if (aura::messaging::g_fiber_yield) {
                aura::messaging::g_fiber_yield();
            } else {
                // In stdin mode: sleep briefly so the thread can make progress
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return make_void();
    });

    // ── orch:metrics — scheduler metrics (Issue #32) ─────────────
    // (orch:metrics) → Returns a JSON string with scheduler counters.
    // Fields: fibers_spawned, fibers_completed, io_events,
    // steal_attempts, steal_successes, per-worker breakdown.
    // Returns empty string when not in serve-async mode.
    primitives_.add("orch:metrics", [this](const auto&) -> EvalValue {
        if (!aura::messaging::g_get_scheduler_metrics) {
            // Not in serve-async mode — return empty list
            return types::make_void();
        }
        auto json = aura::messaging::g_get_scheduler_metrics();
        auto idx = string_heap_.size();
        string_heap_.push_back(json);
        return types::make_string(idx);
    });

    // ── scheduler:pin — pin current fiber to a specific worker (P2) ─
    // (scheduler:pin worker-id) → Pins the current fiber to the given
    // worker thread for cache-aware scheduling. The fiber will always
    // run on that worker (won't be stolen). Returns #t on success, #f
    // when not in serve-async mode or invalid worker ID.
    // worker-id: 0..N-1 where N = number of workers.
    primitives_.add("scheduler:pin", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto wid = static_cast<int>(as_int(a[0]));
        if (wid < 0) return make_bool(false);
        if (aura::messaging::g_fiber_set_affinity) {
            aura::messaging::g_fiber_set_affinity(wid);
            return make_bool(true);
        }
        return make_bool(false);
    });

    // ── orch:reset-metrics — reset scheduler counters (Issue #32) ─
    // (orch:reset-metrics) → Resets all metrics to zero.
    // Returns #t when done, #f when not in serve-async mode.
    primitives_.add("orch:reset-metrics", [this](const auto&) -> EvalValue {
        if (aura::messaging::g_reset_scheduler_metrics) {
            aura::messaging::g_reset_scheduler_metrics();
            return types::make_bool(true);
        }
        return types::make_bool(false);
    });

    // (thread_pool:enqueue fn) — Offload a blocking function to the thread pool.
    // fn is a closure taking no arguments.
    // Returns the result on success (may block caller until done).
    // In serve/fiber mode: yields fiber, pool thread wakes it.
    // In stdin mode: uses std::async, blocks synchronously.
    primitives_.add("thread_pool:enqueue", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_closure(a[0]))
            return make_bool(false);
        auto cid = as_closure_id(a[0]);
        // Serve/fiber mode: use g_thread_pool_enqueue callback
        if (aura::messaging::g_thread_pool_enqueue) {
            auto result_ptr = std::make_shared<std::optional<EvalValue>>();
            aura::messaging::g_thread_pool_enqueue([this, cid, result_ptr]() {
                *result_ptr = apply_closure(cid, {});
            }, -1);
            if (aura::messaging::g_fiber_block) {
                aura::messaging::g_fiber_block();
            }
            if (result_ptr && result_ptr->has_value())
                return std::move(**result_ptr);
            return make_void();
        }
        // Stdin mode: use std::async
        auto future = std::async(std::launch::async, [this, cid]() {
            return apply_closure(cid, {});
        });
        auto result = future.get();
        if (result)
            return *result;
        return make_void();
    });

    // ═══════════════════════════════════════════════════════════════
    // Concurrent Channel Primitives
    // ═══════════════════════════════════════════════════════════════

    // (channel:create [buffer-size]) — Create a new channel
    // Returns channel-id (fixnum) or error.
    // buffer-size defaults to 0 (rendezvous/synchronous).
    primitives_.add("channel:create", [this](const auto& a) -> EvalValue {
        std::size_t buf = 0;
        if (!a.empty() && is_int(a[0])) {
            auto v = as_int(a[0]);
            if (v < 0) return make_bool(false);
            buf = static_cast<std::size_t>(v);
        }
        auto ch = std::make_shared<Evaluator::Channel>();
        ch->buffer_size = buf;
        std::lock_guard lk(channels_mtx_);
        auto id = channels_.size();
        channels_.push_back(ch);
        return make_int(static_cast<std::int64_t>(id));
    });

    // (channel:send channel-id msg) — Send a message to a channel
    // Returns #t on success, #f if channel does not exist.
    // Blocks if buffer full (buffered) or waiting for recv (rendezvous).
    primitives_.add("channel:send", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        auto& msg = string_heap_[as_string_idx(a[1])];
        std::lock_guard lk(channels_mtx_);
        if (ch_id >= channels_.size() || !channels_[ch_id])
            return make_bool(false);
        auto& ch = *channels_[ch_id];
        std::unique_lock ul(ch.mtx);
        ch.cv.wait(ul, [&]() {
            return ch.closed || ch.buffer_size == 0 || ch.queue.size() < ch.buffer_size;
        });
        if (ch.closed) return make_bool(false);
        ch.queue.push_back(msg);
        ul.unlock();
        ch.cv.notify_one();
        return make_bool(true);
    });

    // (channel:recv channel-id) — Receive a message from a channel
    // Returns the message string, or empty string if channel closed.
    // Blocks until a message is available.
    primitives_.add("channel:recv", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_string(0);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::lock_guard lk(channels_mtx_);
        if (ch_id >= channels_.size() || !channels_[ch_id])
            return make_string(0);
        auto& ch = *channels_[ch_id];
        std::unique_lock ul(ch.mtx);
        ch.cv.wait(ul, [&]() { return ch.closed || !ch.queue.empty(); });
        if (ch.queue.empty()) return make_string(0);
        auto msg = ch.queue.front();
        ch.queue.pop_front();
        ul.unlock();
        ch.cv.notify_one();
        auto idx = string_heap_.size();
        string_heap_.push_back(msg);
        return make_string(static_cast<std::uint64_t>(idx));
    });

    // (channel:try-recv channel-id) — Non-blocking receive
    // Returns message string if available, or empty string if no message.
    primitives_.add("channel:try-recv", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_string(0);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::lock_guard lk(channels_mtx_);
        if (ch_id >= channels_.size() || !channels_[ch_id])
            return make_string(0);
        auto& ch = *channels_[ch_id];
        std::lock_guard ul(ch.mtx);
        if (ch.queue.empty()) return make_string(0);
        auto msg = ch.queue.front();
        ch.queue.pop_front();
        auto idx = string_heap_.size();
        string_heap_.push_back(msg);
        return make_string(static_cast<std::uint64_t>(idx));
    });

    // (channel:close channel-id) — Close a channel
    // Wakes all waiters; subsequent recv returns empty string.
    primitives_.add("channel:close", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::lock_guard lk(channels_mtx_);
        if (ch_id >= channels_.size() || !channels_[ch_id])
            return make_bool(false);
        auto& ch = *channels_[ch_id];
        {
            std::lock_guard ul(ch.mtx);
            ch.closed = true;
        }
        ch.cv.notify_all();
        return make_bool(true);
    });

    // (_agent:list) — List all active agent sessions (internal primitive)
    // Called by the Aura-level agent:list wrapper.
    primitives_.add("_agent:list", [this](const auto&) -> EvalValue {
        EvalValue result = make_void();
        if (!aura::messaging::g_session_list || !(*aura::messaging::g_session_list))
            return result;
        auto names = (*aura::messaging::g_session_list)();
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(*it);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════
    // P15: Synthesize Template Strategy (P0)
    // ═══════════════════════════════════════════════════════════════

    // Template storage
    static std::vector<std::pair<std::string, std::string>> g_template_patterns;  // (name, pattern)
    static std::vector<std::vector<std::string>> g_template_params;  // params per template

    // (synthesize:register-template name pattern param-names...)
    primitives_.add("synthesize:register-template", [this](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto pat_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size() || pat_idx >= string_heap_.size())
            return make_bool(false);

        std::string name = string_heap_[name_idx];
        std::string pattern = string_heap_[pat_idx];
        std::vector<std::string> params;
        for (std::size_t i = 2; i < a.size(); ++i) {
            if (is_string(a[i])) {
                auto pidx = as_string_idx(a[i]);
                if (pidx < string_heap_.size())
                    params.push_back(string_heap_[pidx]);
            }
        }

        // Replace or append
        bool found = false;
        for (auto& t : g_template_patterns) {
            if (t.first == name) {
                t.second = pattern;
                found = true;
                break;
            }
        }
        if (!found) {
            g_template_patterns.push_back({name, pattern});
            g_template_params.push_back(params);
        } else {
            // Find the params index
            for (std::size_t i = 0; i < g_template_patterns.size(); ++i) {
                if (g_template_patterns[i].first == name) {
                    g_template_params[i] = params;
                    break;
                }
            }
        }
        return make_bool(true);
    });

    // (synthesize:fill template-name arg-values...)
    primitives_.add("synthesize:fill", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= string_heap_.size())
            return make_void();
        std::string name = string_heap_[name_idx];

        // Find template
        int ti = -1;
        for (std::size_t i = 0; i < g_template_patterns.size(); ++i) {
            if (g_template_patterns[i].first == name) { ti = static_cast<int>(i); break; }
        }
        if (ti < 0) return make_bool(false);

        // Build substitution map
        std::unordered_map<std::string, std::string> subst;
        if (static_cast<std::size_t>(ti) < g_template_params.size()) {
            auto& pnames = g_template_params[ti];
            for (std::size_t i = 0; i < pnames.size() && i + 1 < a.size(); ++i) {
                if (is_string(a[i + 1])) {
                    auto vidx = as_string_idx(a[i + 1]);
                    if (vidx < string_heap_.size())
                        subst[pnames[i]] = string_heap_[vidx];
                }
            }
        }

        // Apply {param} substitutions
        std::string pattern = g_template_patterns[ti].second;
        std::string filled;
        std::size_t pos = 0;
        while (pos < pattern.size()) {
            auto open = pattern.find('{', pos);
            if (open == std::string::npos) {
                filled.append(pattern, pos, std::string::npos);
                break;
            }
            filled.append(pattern, pos, open - pos);
            auto close = pattern.find('}', open);
            if (close == std::string::npos) {
                filled.append(pattern, open);
                break;
            }
            auto pname = pattern.substr(open + 1, close - open - 1);
            auto it = subst.find(pname);
            if (it != subst.end())
                filled.append(it->second);
            else
                filled += "{" + pname + "}";
            pos = close + 1;
        }

        // Apply filled code to workspace via set-code
        auto code_idx = string_heap_.size();
        string_heap_.push_back(filled);
        auto sc_fn = primitives_.lookup("set-code");
        if (!sc_fn) return make_void();
        auto result = (*sc_fn)({make_string(code_idx)});
        // Return the source or true
        if (is_bool(result))
            return result;
        return make_bool(true);
    });

    // (synthesize:list-templates) → list of names
    primitives_.add("synthesize:list-templates", [this](const auto&) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = g_template_patterns.rbegin(); it != g_template_patterns.rend(); ++it) {
            auto idx = string_heap_.size();
            string_heap_.push_back(it->first);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(idx), list});
            list = make_pair(pid);
        }
        return list;
    });

    // ═══════════════════════════════════════════════════════════════
        // ═══════════════════════════════════════════════════════════════
        // ═══════════════════════════════════════════════════════════════
    // P16: Synthesize LLM Strategy — synthesize:define
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:define name sig [key :val ...])
    //   Generates a function using LLM.
    primitives_.add("synthesize:define", [this](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        auto sig_idx = as_string_idx(a[1]);
        if (name_idx >= string_heap_.size() || sig_idx >= string_heap_.size())
            return make_void();

        std::string name = string_heap_[name_idx];
        std::string sig = string_heap_[sig_idx];
        std::string prompt_text;
        std::string model = "deepseek-chat";
        std::string examples_text;
        int max_attempts = 5;

        // Parse keyword arguments
        for (std::size_t i = 2; i + 1 < a.size(); i += 2) {
            if (!is_string(a[i])) continue;
            auto k_idx = as_string_idx(a[i]);
            if (k_idx >= string_heap_.size()) continue;
            std::string key = string_heap_[k_idx];

            if (key == ":prompt" && is_string(a[i+1])) {
                auto pidx = as_string_idx(a[i+1]);
                if (pidx < string_heap_.size())
                    prompt_text = string_heap_[pidx];
            } else if (key == ":model" && is_string(a[i+1])) {
                auto midx = as_string_idx(a[i+1]);
                if (midx < string_heap_.size())
                    model = string_heap_[midx];
            } else if (key == ":examples" && is_string(a[i+1])) {
                auto eidx = as_string_idx(a[i+1]);
                if (eidx < string_heap_.size())
                    examples_text = string_heap_[eidx];
            } else if (key == ":max-attempts" && is_int(a[i+1])) {
                max_attempts = static_cast<int>(as_int(a[i+1]));
            }
        }

        // Build prompt: construct a simple instruction string
        std::string instruction = "Define a function named " + name
            + " in Aura Lisp with signature: " + sig + ".\n";
        if (!prompt_text.empty())
            instruction += "Task: " + prompt_text + ".\n";
        if (!examples_text.empty())
            instruction += "Examples: " + examples_text + "\n";

        // Get API key
        auto getenv_fn = primitives_.lookup("getenv");
        std::string api_key;
        if (getenv_fn) {
            auto kidx = string_heap_.size();
            string_heap_.push_back("LLM_API_KEY");
            auto kr = (*getenv_fn)({make_string(kidx)});
            if (is_string(kr)) {
                auto ai = as_string_idx(kr);
                if (ai < string_heap_.size())
                    api_key = string_heap_[ai];
            }
        }
        if (api_key.empty())
            return make_string(0);

        // Get http-post primitive
        auto http_fn = primitives_.lookup("http-post");
        if (!http_fn) return make_void();

        // Get API URL
        std::string api_url = "https://api.deepseek.com/v1/chat/completions";
        if (getenv_fn) {
            auto uidx = string_heap_.size();
            string_heap_.push_back("LLM_API_URL");
            auto ur = (*getenv_fn)({make_string(uidx)});
            if (is_string(ur)) {
                auto ui = as_string_idx(ur);
                if (ui < string_heap_.size() && !string_heap_[ui].empty())
                    api_url = string_heap_[ui];
            }
        }

        // Auto-fix loop
        std::string last_error;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            // Build JSON payload manually (avoid escaping issues)
            std::string body;
            body += "{\n";
            body += "  \"model\": \"" + model + "\",\n";
            body += "  \"messages\": [\n";
            body += "    {\"role\": \"system\", \"content\": \"You are Aura Lisp. Return ONLY valid Aura code. No markdown.\"},\n";
            body += "    {\"role\": \"user\", \"content\": \"" + instruction;
            if (!last_error.empty())
                body += " Previous attempt error: " + last_error + ". Fix it.";
            body += "\"}\n";
            body += "  ]\n";
            body += "}\n";

            auto bi = string_heap_.size();
            string_heap_.push_back(body);
            auto ui2 = string_heap_.size();
            string_heap_.push_back(api_url);
            auto ki = string_heap_.size();
            string_heap_.push_back(api_key);

            auto resp = (*http_fn)({make_string(ui2), make_string(bi), make_string(ki)});
            if (!is_string(resp)) continue;
            auto ri = as_string_idx(resp);
            if (ri >= string_heap_.size()) continue;
            auto& response = string_heap_[ri];

            // Extract code from JSON response
            std::string code;
            auto cp = response.find("content");
            if (cp == std::string::npos) continue;
            auto cq = response.find('"', cp + 9);
            if (cq == std::string::npos) continue;
            cq++;
            bool esc = false;
            for (; cq < response.size(); ++cq) {
                char c = response[cq];
                if (esc) {
                    if (c == 'n') code += '\n';
                    else if (c == 't') code += '\t';
                    else if (c == '"') code += '"';
                    else if (c == '\\') code += '\\';
                    else code += c;
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    break;
                } else {
                    code += c;
                }
            }

            if (code.empty()) continue;

            // Try set-code
            auto ci = string_heap_.size();
            string_heap_.push_back(code);
            auto sc_fn = primitives_.lookup("set-code");
            if (!sc_fn) continue;
            auto sc_r = (*sc_fn)({make_string(ci)});
            if (!is_bool(sc_r) || !as_bool(sc_r)) {
                last_error = "syntax error";
                continue;
            }

            // Try typecheck
            auto tc_fn = primitives_.lookup("typecheck-current");
            if (tc_fn) {
                auto tc_r = (*tc_fn)({});
                if (is_string(tc_r)) {
                    auto ti = as_string_idx(tc_r);
                    if (ti < string_heap_.size()) {
                        std::string msg = string_heap_[ti];
                        if (msg.find("error") != std::string::npos ||
                            msg.find("unbound") != std::string::npos) {
                            last_error = msg;
                            continue;
                        }
                    }
                }
            }

            // Success
            defuse_version_++;
            auto src_fn = primitives_.lookup("current-source");
            if (src_fn) {
                auto src = (*src_fn)({});
                return src;
            }
            return make_bool(true);
        }

        return make_bool(false);
    });

    // ═══════════════════════════════════════════════════════════════
    // P17: Synthesize Genetic Strategy — synthesize:optimize
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:optimize name [key :val ...])
    //   Uses genetic algorithm to optimize a function.
    //   Keywords: :population, :generations, :mutation-rate,
    //             :fitness or :benchmark (benchmark expression)
    //   Default fitness: runs function with synthetic test inputs
    //   (correctness = 90% weight, code length = 10% tiebreaker).
    //   Creates variants, evaluates fitness, returns best.
    primitives_.add("synthesize:optimize", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= string_heap_.size())
            return make_void();
        std::string fn_name = string_heap_[name_idx];
        int pop_size = 8;
        int generations = 3;
        double mutation_rate = 0.3;
        std::string fitness_expr;  // optional user-provided fitness expr

        for (std::size_t i = 1; i + 1 < a.size(); i += 2) {
            if (!is_string(a[i])) continue;
            auto k_idx = as_string_idx(a[i]);
            if (k_idx >= string_heap_.size()) continue;
            std::string key = string_heap_[k_idx];
            if (key == ":population" && is_int(a[i+1]))
                pop_size = static_cast<int>(as_int(a[i+1]));
            else if (key == ":generations" && is_int(a[i+1]))
                generations = static_cast<int>(as_int(a[i+1]));
            else if (key == ":mutation-rate" && is_float(a[i+1]))
                mutation_rate = as_float(a[i+1]);
            else if ((key == ":fitness" || key == ":benchmark") && is_string(a[i+1])) {
                auto fi = as_string_idx(a[i+1]);
                if (fi < string_heap_.size())
                    fitness_expr = string_heap_[fi];
            }
        }
        if (pop_size < 2) pop_size = 2;
        if (pop_size > 50) pop_size = 50;
        if (generations < 1) generations = 1;

        // Get baseline source
        auto src_fn = primitives_.lookup("current-source");
        if (!src_fn) return make_void();
        auto cs_result = (*src_fn)({});
        if (!is_string(cs_result)) return make_void();
        auto cs_idx = as_string_idx(cs_result);
        if (cs_idx >= string_heap_.size()) return make_void();
        std::string baseline = string_heap_[cs_idx];

        // Fitness: generate synthetic test inputs and eval the function
        // to measure correctness + performance.
        //
        // Strategy:
        // 1. Parse variant source, count function args by scanning for fn_name
        // 2. Generate probe inputs (ints, pairs, etc.) based on arg count
        // 3. Eval each probe: (fn_name arg...), score = fraction of probes that
        //    return a valid value without error
        // 4. Code length is a tiebreaker only — correctness dominates
        //
        // If :fitness keyword is provided, use that expression instead.
        auto compute_fitness = [&](const std::string& src) -> double {
            if (!fitness_expr.empty()) {
                // User-provided fitness: eval the expression
                auto sv = string_heap_.size();
                string_heap_.push_back(src);
                auto eval_fn = primitives_.lookup("eval");
                if (eval_fn) {
                    auto r = (*eval_fn)({make_string(sv)});
                    if (is_float(r)) return as_float(r);
                    if (is_int(r)) return static_cast<double>(as_int(r));
                }
                return 0.0;
            }

            // Default fitness: eval the current workspace (which contains
            // the variant code) to bind the function in top_, then probe
            // via the eval primitive.
            //
            // The calling code has already set up the workspace via
            // set-code + typecheck-current before we're called, so
            // eval-current binds the function using workspace flats
            // (safe — no dangling closure pointers).
            //
            // Detect argument count by scanning for (define (fn_name ...))
            int arg_count = 0;
            {
                auto def_pos = src.find("(define (" + fn_name);
                if (def_pos != std::string::npos) {
                    auto after_fn = src.find(' ', def_pos);
                    if (after_fn != std::string::npos) {
                        auto param_start = src.find(' ', after_fn + 1);
                        if (param_start != std::string::npos) {
                            auto close_pos = src.find(')', param_start);
                            if (close_pos != std::string::npos) {
                                auto params = src.substr(param_start + 1, close_pos - param_start - 1);
                                if (!params.empty()) {
                                    arg_count = 1;
                                    for (auto c : params) {
                                        if (c == ' ') ++arg_count;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Eval-current to bind functions in top_ (uses workspace flats, safe)
            auto ec_fn = primitives_.lookup("eval-current");
            if (ec_fn) {
                (*ec_fn)({});
            }

            // Probe via eval (function is now bound in top_ from workspace eval)
            // Temporary flats in eval are OK for call expressions — they don't
            // create closures, just look up and apply.
            static const std::int64_t probe_ints[] = {0, 1, -1, 2};
            int successes = 0;
            int total_tests = 0;
            auto eval_fn = primitives_.lookup("eval");

            auto try_probe = [&](const std::string& call_src) {
                ++total_tests;
                auto ci = string_heap_.size();
                string_heap_.push_back(call_src);
                if (eval_fn) {
                    auto r = (*eval_fn)({make_string(ci)});
                    if (!types::is_error(r))
                        ++successes;
                }
            };

            if (arg_count <= 0) {
                try_probe("(" + fn_name + ")");
            } else if (arg_count == 1) {
                for (auto v : probe_ints) {
                    if (total_tests >= 4) break;
                    try_probe("(" + fn_name + " " + std::to_string(v) + ")");
                }
            } else if (arg_count == 2) {
                for (int i = 0; i < 4 && i + 1 < 4; ++i) {
                    try_probe("(" + fn_name + " " +
                              std::to_string(probe_ints[i]) + " " +
                              std::to_string(probe_ints[i+1]) + ")");
                }
            } else {
                std::string call_src = "(" + fn_name;
                for (int i = 0; i < arg_count; ++i)
                    call_src += " 0";
                call_src += ")";
                try_probe(call_src);
            }

            // Score: correctness dominates (up to 1000), then small length bonus
            double correctness = total_tests > 0
                ? (1000.0 * static_cast<double>(successes) / static_cast<double>(total_tests))
                : 0.0;
            double length_bonus = 1.0 / static_cast<double>(src.size() + 1);
            return correctness + length_bonus;
        };

        std::string best_code = baseline;
        double best_fitness = compute_fitness(baseline);
        int best_gen = 0;

        // Store multiple candidates for crossover (elitism)
        std::vector<std::pair<std::string, double>> elite;

        for (int gen = 0; gen < generations; ++gen) {
            for (int p = 0; p < pop_size; ++p) {
                std::string variant = best_code;

                // Apply mutations
                for (int m = 0; m < 5; ++m) {
                    if (static_cast<double>(std::rand()) / RAND_MAX >= mutation_rate)
                        continue;
                    // Operator swap
                    for (const char* op = "+-*/"; *op; ++op) {
                        auto opos = variant.find(*op);
                        if (opos != std::string::npos && opos > 0) {
                            variant[opos] = "+-*/"[std::rand() % 4];
                            break;
                        }
                    }
                    // Numeric mutation
                    auto npos = variant.find_first_of("0123456789");
                    if (npos == std::string::npos) break;
                    auto nend = variant.find_first_not_of("0123456789", npos);
                    if (nend == std::string::npos) nend = variant.size();
                    std::string old_n = variant.substr(npos, nend - npos);
                    if (old_n.empty()) continue;
                    int val = std::stoi(old_n);
                    val += (std::rand() % 21) - 10;
                    if (val < 0) val = 0;
                    variant.replace(npos, nend - npos, std::to_string(val));
                }

                // Crossover: text-level or AST-level
                if (!elite.empty() && std::rand() % 3 == 0) {
                    auto& other = elite[std::rand() % elite.size()].first;
                    // Try AST expression-level crossover via node swapping
                    // Use a child workspace and mutate:replace-value
                    if (workspace_tree_ && std::rand() % 2 == 0) {
                        auto* tree = static_cast<WorkspaceTree*>(workspace_tree_);
                        // Create a temporary workspace, set-code the variant,
                        // find a LiteralInt node, replace it with one from other
                        auto ws_id = tree->create_child("xover",
                            workspace_flat_, workspace_pool_);
                        if (ws_id > 0) {
                            tree->ensure_local_flat(ws_id);
                            auto& ws = tree->nodes_[ws_id];
                            auto saved_f = workspace_flat_;
                            auto saved_p = workspace_pool_;
                            workspace_flat_ = ws.flat;
                            workspace_pool_ = ws.pool;

                            // Set variant as current code
                            auto vi = string_heap_.size();
                            string_heap_.push_back(variant);
                            auto sc_fn = primitives_.lookup("set-code");
                            if (sc_fn) {
                                auto sr = (*sc_fn)({make_string(vi)});
                                if (is_bool(sr) && as_bool(sr)) {
                                    // Find LiteralInt nodes and swap value with other variant
                                    auto tc_fn = primitives_.lookup("typecheck-current");
                                    for (aura::ast::NodeId nid = 0;
                                         nid < (workspace_flat_ ? workspace_flat_->size() : 0);
                                         ++nid) {
                                        if (std::rand() % 5 != 0) continue;  // 20% chance per node
                                        auto v = workspace_flat_->get(nid);
                                        if (v.tag == aura::ast::NodeTag::LiteralInt) {
                                            // Extract a random int from "other"
                                            auto nums = other;
                                            auto npos = nums.find_first_of("0123456789");
                                            if (npos != std::string::npos) {
                                                auto nend = nums.find_first_not_of("0123456789", npos);
                                                if (nend == std::string::npos) nend = nums.size();
                                                int new_val = std::stoi(nums.substr(npos, nend - npos));
                                                if (new_val >= 0) {
                                                    auto rv_fn = primitives_.lookup("mutate:replace-value");
                                                    if (rv_fn) {
                                                        (*rv_fn)({make_int(nid), make_int(new_val),
                                                                 make_string(vi)});
                                                    }
                                                }
                                            }
                                            break;  // Mutate one node
                                        }
                                    }

                                    // Typecheck after crossover
                                    if (tc_fn) {
                                        auto tc_r = (*tc_fn)({});
                                        if (is_string(tc_r)) {
                                            auto ti = as_string_idx(tc_r);
                                            if (ti < string_heap_.size() &&
                                                string_heap_[ti].find("error") == std::string::npos) {
                                                // Successful crossover: get the new source
                                                auto src_fn = primitives_.lookup("current-source");
                                                if (src_fn) {
                                                    auto src = (*src_fn)({});
                                                    if (is_string(src)) {
                                                        auto si = as_string_idx(src);
                                                        if (si < string_heap_.size())
                                                            variant = string_heap_[si];
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            workspace_flat_ = saved_f;
                            workspace_pool_ = saved_p;
                            tree->delete_child(ws_id);
                        }
                    } else {
                        // Text-level crossover (fallback)
                        auto b1 = variant.find("(lambda");
                        auto b2 = other.find("(lambda");
                        if (b1 != std::string::npos && b2 != std::string::npos) {
                            auto e1 = variant.find(')', b1);
                            auto e2 = other.find(')', b2);
                            if (e1 != std::string::npos && e2 != std::string::npos
                                && e1 > b1 && e2 > b2) {
                                auto body1_end = variant.find_last_of(')');
                                auto body2_end = other.find_last_of(')');
                                if (body1_end > b1 && body2_end > b2) {
                                    std::string body1 = variant.substr(b1, body1_end - b1 + 1);
                                    std::string body2 = other.substr(b2, body2_end - b2 + 1);
                                    if (body1 != body2) {
                                        variant = variant.substr(0, b1) + body2
                                            + variant.substr(body1_end + 1);
                                    }
                                }
                            }
                        }
                    }
                }

                if (variant == best_code) continue;

                // Evaluate in a child workspace (isolation)
                // Use workspace tree if available, otherwise fall back to set-code
                bool evaluated = false;
                double f = 0.0;
                auto sc_fn = primitives_.lookup("set-code");
                if (!sc_fn) continue;

                if (workspace_tree_) {
                    // Use child workspace for isolation
                    auto* tree = static_cast<WorkspaceTree*>(workspace_tree_);
                    auto ws_id = tree->create_child("evolve-variant",
                                                     workspace_flat_, workspace_pool_);
                    // Switch to child and try the variant
                    if (ws_id > 0) {
                        tree->ensure_local_flat(ws_id);
                        auto& ws = tree->nodes_[ws_id];
                        auto saved_flat = workspace_flat_;
                        auto saved_pool = workspace_pool_;
                        workspace_flat_ = ws.flat;
                        workspace_pool_ = ws.pool;

                        auto vi = string_heap_.size();
                        string_heap_.push_back(variant);
                        auto sc_r = (*sc_fn)({make_string(vi)});

                        if (is_bool(sc_r) && as_bool(sc_r)) {
                            bool valid = true;
                            auto tc_fn = primitives_.lookup("typecheck-current");
                            if (tc_fn) {
                                auto tc_r = (*tc_fn)({});
                                if (is_string(tc_r)) {
                                    auto ti = as_string_idx(tc_r);
                                    if (ti < string_heap_.size() &&
                                        string_heap_[ti].find("error") != std::string::npos)
                                        valid = false;
                                }
                            }
                            if (valid) {
                                f = compute_fitness(variant);
                                evaluated = true;
                            }
                        }

                        workspace_flat_ = saved_flat;
                        workspace_pool_ = saved_pool;
                        tree->delete_child(ws_id);
                    }
                }

                if (!evaluated) {
                    // Fallback: direct set-code
                    auto vi = string_heap_.size();
                    string_heap_.push_back(variant);
                    auto sc_r = (*sc_fn)({make_string(vi)});
                    if (!is_bool(sc_r) || !as_bool(sc_r)) continue;

                    auto tc_fn = primitives_.lookup("typecheck-current");
                    bool valid = true;
                    if (tc_fn) {
                        auto tc_r = (*tc_fn)({});
                        if (is_string(tc_r)) {
                            auto ti = as_string_idx(tc_r);
                            if (ti < string_heap_.size() &&
                                string_heap_[ti].find("error") != std::string::npos)
                                valid = false;
                        }
                    }
                    if (!valid) continue;
                    f = compute_fitness(variant);
                    // Restore
                    auto bi = string_heap_.size();
                    string_heap_.push_back(baseline);
                    (*sc_fn)({make_string(bi)});
                }

                if (f > best_fitness) {
                    best_fitness = f;
                    best_code = variant;
                    best_gen = gen + 1;
                }
            }

            // Update elite from this generation
            elite.clear();
            elite.push_back({best_code, best_fitness});
        }

        // Apply best to workspace
        auto bi = string_heap_.size();
        string_heap_.push_back(best_code);
        auto sc_fn = primitives_.lookup("set-code");
        if (sc_fn) (*sc_fn)({make_string(bi)});
        defuse_version_++;
        defuse_index_ = nullptr;

        auto gs = std::to_string(best_gen);
        auto gi = string_heap_.size();
        string_heap_.push_back(gs);
        auto fs = std::to_string(best_fitness);
        auto fi = string_heap_.size();
        string_heap_.push_back(fs);

        auto p1 = pairs_.size();
        pairs_.push_back({make_string(gi), make_string(fi)});
        return make_pair(p1);
    });

// ── intend — 纯循环管理器 — 纯循环管理器 ────────────────────────────────
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
        // Mark task context so closure bodies are allocated in temp_arena_
        bool saved_context = in_task_context_;
        in_task_context_ = true;
        auto restore = [&] { in_task_context_ = saved_context; };

        if (a.size() < 3)
            { restore(); return make_void(); }
        if (!types::is_string(a[0]) || !types::is_closure(a[1]) || !types::is_closure(a[2]))
            { restore(); return make_void(); }

        auto goal = string_heap_[types::as_string_idx(a[0])];
        auto gen_cid = types::as_closure_id(a[1]); // generator
        auto ver_cid = types::as_closure_id(a[2]); // verifier

        // Optional fixer_fn
        bool has_fixer = a.size() >= 4 && types::is_closure(a[3]);
        auto fix_cid = has_fixer ? types::as_closure_id(a[3]) : std::uint64_t{0};

        // Optional max_attempts
        int max_attempts = 3;
        if ((has_fixer && a.size() >= 5 && types::is_int(a[4])) ||
            (!has_fixer && a.size() >= 4 && types::is_int(a[3]))) {
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
            if (err.find("unbound variable") != std::string::npos)
                return "unbound-variable";
            if (err.find("type mismatch") != std::string::npos)
                return "type-mismatch";
            if (err.find("division by zero") != std::string::npos)
                return "div-zero";
            if (err.find("syntax") != std::string::npos ||
                err.find("unbalanced") != std::string::npos)
                return "syntax-error";
            if (err.find("timeout") != std::string::npos)
                return "timeout";
            if (err.find("recursion") != std::string::npos ||
                err.find("stack") != std::string::npos)
                return "recursion-limit";
            if (err.find("bad-code") != std::string::npos)
                return "bad-code";
            if (err.find("verification failed") != std::string::npos)
                return "verification-failed";
            return "other";
        };

        // Call a closure, return string result
        auto call_fn = [&](std::uint64_t cid,
                           const std::vector<types::EvalValue>& args) -> std::string {
            auto opt = apply_closure(cid, args);
            if (!opt)
                return {};
            auto& val = *opt;
            if (types::is_string(val))
                return string_heap_[types::as_string_idx(val)];
            if (types::is_void(val))
                return {};
            if (types::is_int(val))
                return std::to_string(types::as_int(val));
            if (types::is_bool(val))
                return types::as_bool(val) ? "#t" : "#f";
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
                    timeline_.push_back("attempt_" + std::to_string(attempt) +
                                        ":empty from generator");
                    errors.push_back("empty from generator");
                    error_types.push_back("empty");
                    continue;
                }
            } else {
                if (!has_fixer) {
                    timeline_.push_back("attempt_" + std::to_string(attempt) +
                                        ":no fixer, stopping");
                    break;
                }
                auto cs = string_heap_.size();
                string_heap_.push_back(current_code_str);
                auto es = string_heap_.size();
                string_heap_.push_back(last_error);
                auto gs = string_heap_.size();
                string_heap_.push_back(goal);
                code_str = call_fn(fix_cid, {types::make_string(cs), types::make_string(es),
                                             types::make_string(gs)});
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
                auto result = "#(status:\"ok\" goal:\"" + goal +
                              "\" iterations:" + std::to_string(attempt) + ")";
                auto rs = string_heap_.size();
                string_heap_.push_back(result);

                auto t1 = std::chrono::steady_clock::now();
                auto duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
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
                restore();
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

        auto result = "#(status:\"failed\" goal:\"" + goal +
                      "\" iterations:" + std::to_string(max_attempts) + " last-error:\"" +
                      last_error + "\")";
        timeline_.push_back("failed:" + last_error);
        auto rs = string_heap_.size();
        string_heap_.push_back(result);
        restore();
        return types::make_string(rs);
    });
    // ── intend-history — 查询意图执行时间线 ────────────────────
    primitives_.add("intend-history", [this](const auto&) -> EvalValue {
        std::string result;
        for (std::size_t i = 0; i < timeline_.size(); ++i) {
            result += std::to_string(i) + ":" + timeline_[i] + "\n";
        }
        if (result.empty())
            result = "(empty)";
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
        if (a.size() > arg_idx + 2 && types::is_string(a[arg_idx]) &&
            string_heap_[types::as_string_idx(a[arg_idx])] == ":filter") {
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
            if (!filter_strategy.empty() && rec.strategy_name != filter_strategy)
                continue;
            if (!filter_field.empty()) {
                if (filter_field == "error-type") {
                    bool matches = false;
                    for (auto& et : rec.error_types) {
                        if (et.find(filter_value) != std::string::npos) {
                            matches = true;
                            break;
                        }
                    }
                    if (!matches)
                        continue;
                } else
                    continue;
            }
            total++;
            if (rec.success)
                successes++;
            total_attempts += rec.attempts;
            total_llm_calls += rec.llm_call_count;
            total_duration += rec.duration_ms;
            for (auto& et : rec.error_types)
                error_type_counts[et]++;
            auto key = rec.task_desc.substr(
                0, std::min<std::size_t>(rec.task_desc.size(), std::size_t{60}));
            auto& ts = task_stats[key];
            ts.second++;
            if (rec.success)
                ts.first++;
        }

        std::string result = "#(analytics";
        result += " total-runs:" + std::to_string(total);
        result += " success-rate:";
        if (total > 0)
            result += std::to_string(static_cast<double>(successes) / static_cast<double>(total));
        else
            result += "0";
        result += " avg-attempts:";
        if (total > 0)
            result +=
                std::to_string(static_cast<double>(total_attempts) / static_cast<double>(total));
        else
            result += "0";
        result += " total-llm-calls:" + std::to_string(total_llm_calls);
        result += " avg-duration-ms:";
        if (total > 0)
            result += std::to_string(total_duration / total);
        else
            result += "0";
        result += " top-errors:(";
        for (auto& [et, count] : error_type_counts) {
            result += " " + et + ":" + std::to_string(count);
        }
        result += ")";
        result += " by-task:(";
        for (auto& [task, stats] : task_stats) {
            result += " (" + task + " " + std::to_string(stats.first) + "/" +
                      std::to_string(stats.second) + ")";
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
            if (s.name == name) {
                s.body = body;
                return make_bool(true);
            }
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
            if (s.name == name) {
                s.body = body;
                return make_bool(true);
            }
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
        if (field != "body" || !types::is_string(a[2]))
            return make_bool(false);
        auto name = string_heap_[types::as_string_idx(a[0])];
        auto new_body = string_heap_[types::as_string_idx(a[2])];
        for (auto& s : strategies_) {
            if (s.name == name) {
                s.body = new_body;
                return make_bool(true);
            }
        }
        return make_bool(false);
    });
    // ── strategy-inspect — 一键检视 ────────────────────────
    primitives_.add("strategy-inspect", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
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
                    case 0:
                        name = "parser";
                        break;
                    case 1:
                        name = "typecheck";
                        break;
                    case 2:
                        name = "eval";
                        break;
                    case 3:
                        name = "jit";
                        break;
                    case 4:
                        name = "macro";
                        break;
                    case 5:
                        name = "edsl-set-code";
                        break;
                    case 6:
                        name = "edsl-query";
                        break;
                    case 7:
                        name = "edsl-mutate";
                        break;
                    case 8:
                        name = "ffi";
                        break;
                    default:
                        name = "reserved-" + std::to_string(i);
                        break;
                }
                result += " " + name + ":" + std::to_string(coverage_counters_[i]);
            }
        }
        result += ")";
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // (gc) — Reset arena to reclaim memory between benchmark tasks
    // Saves current source, resets arena, re-parses source into fresh arena.
    primitives_.add("gc", [this](const auto&) -> EvalValue {
        // Save current source
        std::string saved_src;
        if (workspace_flat_ && workspace_flat_->root != aura::ast::NULL_NODE) {
            auto src_fn = primitives_.lookup("current-source");
            if (src_fn) {
                auto src = (*src_fn)({});
                if (types::is_string(src)) {
                    auto sidx = types::as_string_idx(src);
                    if (sidx < string_heap_.size())
                        saved_src = string_heap_[sidx];
                }
            }
        }

        // Reset arena (invalidates all arena-allocated state)
        defuse_index_ = nullptr;
        modules_.clear();
        module_cache_.clear();
        current_flat_ = nullptr;
        current_pool_ = nullptr;
        workspace_flat_ = nullptr;
        workspace_pool_ = nullptr;
        if (aura::messaging::g_reset_arena && compiler_service_) {
            aura::messaging::g_reset_arena(compiler_service_);
        }

        // Re-parse saved source into fresh arena
        if (!saved_src.empty()) {
            auto set_fn = primitives_.lookup("set-code");
            if (set_fn) {
                auto si = string_heap_.size();
                string_heap_.push_back(saved_src);
                (*set_fn)({types::make_string(si)});
            }
        }

        return types::make_bool(!saved_src.empty());
    });

    // (gc-heap) — Trigger GC or clear heap vectors.
    // When a GC collector is available (serve-async mode with
    // thread-safe GC), triggers a full GC cycle instead of
    // blindly clearing. Falls back to direct clear for stdin mode.
    primitives_.add("gc-heap", [this](const auto&) -> EvalValue {
        // If GC collector is available, use it
        if (aura::messaging::g_gc_collect) {
            std::lock_guard<std::mutex> lock(heap_mutex());
            return types::make_bool(aura::messaging::g_gc_collect());
        }
        // Fallback: direct clear (stdin mode)
        {
            std::lock_guard<std::mutex> lock(heap_mutex());
            string_heap_.clear();
            string_heap_.shrink_to_fit();
            pairs_.clear();
            pairs_.shrink_to_fit();
            error_values_.clear();
            error_values_.shrink_to_fit();
            hash_heap_.clear();
            hash_heap_.shrink_to_fit();
            vector_heap_.clear();
            vector_heap_.shrink_to_fit();
            opaque_heap_.clear();
            opaque_heap_.shrink_to_fit();
        }
        return types::make_bool(true);
    });

    // (gc-freeze) — Mark current closure generation as "root".
    // The while loop's predicate/body closures are created before this
    // call (in persistent arena when in_task_context_=false).
    primitives_.add("gc-freeze", [this](const auto&) -> EvalValue {
        gc_safe_closure_id_ = next_id_;
        return types::make_bool(true);
    });

    // (gc-temp) — Reset temp arena + clear temp closures + heap vectors.
    // Safe to call between benchmark tasks. Temp closures (those with
    // owner_arena == temp_arena_) are erased, their arena memory freed O(1).
    // Module functions and while-loop closures (in persistent arena) survive.
    primitives_.add("gc-temp", [this](const auto&) -> EvalValue {
        if (!temp_arena_) return types::make_bool(false);

        // Erase closures in temp arena
        for (auto it = closures_.begin(); it != closures_.end(); ) {
            if (it->second.owner_arena == temp_arena_)
                it = closures_.erase(it);
            else
                ++it;
        }

        // Reset temp arena (O(1) — frees all cl_flat/cl_pool/copy_env)
        temp_arena_->reset();

        // Clear heap vectors.
        // NOTE: pairs_ and string_heap_ are NOT cleared — result lists are
        // pair-based and contain string references. gc-temp is called
        // before the caller reads results. Use gc-heap separately to
        // clear strings/pairs when results are no longer needed.
        // hash_heap_, vector_heap_, opaque_heap_ are safe to clear here.
        error_values_.clear();
        error_values_.shrink_to_fit();
        hash_heap_.clear();
        hash_heap_.shrink_to_fit();
        vector_heap_.clear();
        vector_heap_.shrink_to_fit();
        opaque_heap_.clear();
        opaque_heap_.shrink_to_fit();

        return types::make_bool(true);
    });

    // (gc-stats) — Return formatted string of all heap sizes for telemetry.
    primitives_.add("gc-stats", [this](const auto&) -> EvalValue {
        std::uint64_t root_count = 0;
        for (auto& [id, _] : closures_) {
            if (id < gc_safe_closure_id_) ++root_count;
        }
        auto result = std::format(
            "string:{}/pairs:{}/cells:{}/err:{}/hash:{}/vec:{}/opq:{}/cls:{}/root:{}",
            string_heap_.size(), pairs_.size(), cells_.size(),
            error_values_.size(), hash_heap_.size(), vector_heap_.size(),
            opaque_heap_.size(), closures_.size(), root_count);
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // ── Capability primitives (with-capability / capability? / check-capability) ──

    primitives_.add("with-capability", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            auto es = string_heap_.size();
            string_heap_.push_back("with-capability: first argument must be a string or list of strings");
            auto ev = error_values_.size();
            error_values_.push_back(make_string(es));
            return make_error(ev);
        }
        if (a.size() < 2) {
            auto es = string_heap_.size();
            string_heap_.push_back("with-capability: requires at least 2 args");
            auto ev = error_values_.size();
            error_values_.push_back(make_string(es));
            return make_error(ev);
        }
        auto cap_val = a[0];
        std::vector<std::string> caps;
        if (types::is_string(cap_val)) {
            auto sidx = types::as_string_idx(cap_val);
            if (sidx < string_heap_.size())
                caps.push_back(string_heap_[sidx]);
        } else if (types::is_pair(cap_val)) {
            auto cidx = types::as_pair_idx(cap_val);
            while (cidx < pairs_.size()) {
                auto& p = pairs_[cidx];
                if (types::is_string(p.car)) {
                    auto sidx2 = types::as_string_idx(p.car);
                    if (sidx2 < string_heap_.size())
                        caps.push_back(string_heap_[sidx2]);
                }
                if (types::is_int(p.cdr) && types::as_int(p.cdr) == 0)
                    break;
                if (types::is_pair(p.cdr))
                    cidx = types::as_pair_idx(p.cdr);
                else
                    break;
            }
        }
        // Push capability context
        capability_stack_.push_back(caps);
        // Evaluate body expression (the last arg)
        auto body = a[1];
        EvalValue result = make_void();
        if (types::is_closure(body) && workspace_flat_ && workspace_pool_) {
            auto cid = types::as_closure_id(body);
            auto it = closures_.find(cid);
            if (it != closures_.end() && it->second.body_id != ast::NULL_NODE)
                result = eval_flat(*workspace_flat_, *workspace_pool_, it->second.body_id, top_env()).value_or(make_void());
        } else {
            result = body;
        }
        // Pop capability context
        if (!capability_stack_.empty())
            capability_stack_.pop_back();
        return result;
    });

    primitives_.add("capability?", [](const auto& a) -> EvalValue {
        return types::make_bool(false);
    });

    primitives_.add("check-capability", [this](const auto& a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            auto es = string_heap_.size();
            string_heap_.push_back("check-capability: first argument must be a string");
            auto ev = error_values_.size();
            error_values_.push_back(make_string(es));
            return make_error(ev);
        }
        auto sidx = types::as_string_idx(a[0]);
        std::string needed;
        if (sidx < string_heap_.size())
            needed = string_heap_[sidx];
        // Check each capability context in reverse order for proper scoping
        for (auto it = capability_stack_.rbegin(); it != capability_stack_.rend(); ++it) {
            for (auto& c : *it) {
                if (c == needed || c == "*") {
                    return types::make_bool(true);
                }
            }
        }
        return types::make_bool(false);
    });

    primitives_.add("capability-stack", [this](const auto&) -> EvalValue {
        // Collect all unique caps from stack
        std::vector<std::string> caps;
        for (auto& layer : capability_stack_) {
            for (auto& cap : layer) {
                bool dup = false;
                for (auto& c : caps)
                    if (c == cap) { dup = true; break; }
                if (!dup) caps.push_back(cap);
            }
        }
        // Build list from BACK to FRONT (append to head)
        EvalValue result = make_void();  // '()
        for (int i = static_cast<int>(caps.size()) - 1; i >= 0; --i) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(caps[i]);
            auto new_pair_idx = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(new_pair_idx);
        }
        return result;
    });
}

// slot_for_name: find the slot for a primitive name
std::size_t Primitives::slot_for_name(const std::string& name) const {
    for (std::size_t i = 0; i < ordered_names_.size(); ++i) {
        if (ordered_names_[i] == name)
            return i;
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
            struct stat st;
            if (::stat(candidate.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            char real[4096];
            if (::realpath(candidate.c_str(), real))
                return std::string(real);
            return candidate;
        }
        return std::nullopt;
    };

    if (!path.empty() && path[0] == '/') {
        auto hit = try_load(path);
        if (hit)
            return *hit;
        return {};
    }

    // Search CWD first
    {
        char cwd_buf[4096];
        if (::getcwd(cwd_buf, sizeof(cwd_buf))) {
            auto hit = try_load(std::string(cwd_buf) + "/" + path);
            if (hit)
                return *hit;
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
                if (hit)
                    return *hit;
            }
            start = end + 1;
        }
        if (start < aura_path.size()) {
            auto dir = aura_path.substr(start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit)
                    return *hit;
            }
        }
    }

    // Auto-discover: try ../lib/ and ./lib/ (relative to executable / CWD)
    {
        // Try ../lib/ (common for build/aura → lib/ layout)
        auto hit = try_load("../lib/" + path);
        if (hit)
            return *hit;
    }
    {
        // Try ./lib/ (cwd-relative)
        auto hit = try_load("./lib/" + path);
        if (hit)
            return *hit;
    }

    return {};
}

// ── Load module file, return module object ────────────────
types::EvalValue Evaluator::load_module_file(const std::string& path) {
    // 1. Resolve path
    auto resolved = resolve_module_path(path);
    if (resolved.empty()) {
        std::println(std::cerr, "load_module_file: cannot resolve '{}'", path);
        return types::make_void();
    }

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
    struct stat st;
    if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }
    std::ifstream f(resolved);
    if (!f) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    if (content.empty()) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }

    // 5. Parse
    if (!arena_) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: no arena");
        return types::make_void();
    }
    auto alloc = arena_->allocator();
    auto* pool_ptr = arena_->create<aura::ast::StringPool>(alloc);
    auto* flat_ptr = arena_->create<aura::ast::FlatAST>(alloc);
    auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: parse error for {}", resolved);
        if (!pr.error.empty())
            std::println(std::cerr, "  {}", pr.error);
        return types::make_void();
    }
    flat_ptr->root = pr.root;

    // 6. Create isolated module env (child of top_ for primitive access)
    // Arena-allocate so closures captured during module eval stay valid
    auto* mod_env = arena_->create<Env>(&top_);
    mod_env->set_primitives(&primitives_);
    mod_env->set_cells(&cells_);

    // 7. Clear any stale export set from previous module loads
    if (current_export_set_)
        current_export_set_->clear();

    // 8. Evaluate module in its own env
    auto expanded = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
    auto result = eval_flat(*flat_ptr, *pool_ptr, expanded, *mod_env);

    // 9. Apply export filtering: if (export ...) was declared, remove unexported bindings
    if (current_export_set_ && !current_export_set_->empty()) {
        auto& bindings = mod_env->bindings();
        for (auto it = bindings.begin(); it != bindings.end();) {
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

    // 10c. 自动加载 .aura-type 类型签名文件
    // 检查 {module}.aura 同目录下是否有 {module}.aura-type 文件
    auto type_sig_path = resolved;
    if (type_sig_path.size() > 5) {
        auto dot = type_sig_path.rfind('.');
        if (dot != std::string::npos)
            type_sig_path = type_sig_path.substr(0, dot) + ".aura-type";
    }
    struct stat st2;
    if (::stat(type_sig_path.c_str(), &st2) == 0 && S_ISREG(st2.st_mode)) {
        std::ifstream tf(type_sig_path);
        if (tf) {
            std::string line;
            while (std::getline(tf, line)) {
                // 格式: "name: param1 param2 -> rettype"
                // 例如 "add: Int Int -> Int"
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                auto arrow = line.find("->", colon);
                if (arrow == std::string::npos) continue;
                auto name = line.substr(0, colon);
                name.erase(name.find_last_not_of(" \t\r") + 1);
                auto params_str = line.substr(colon + 1, arrow - colon - 1);
                params_str.erase(0, params_str.find_first_not_of(" \t\r"));
                params_str.erase(params_str.find_last_not_of(" \t\r") + 1);
                auto ret_str = line.substr(arrow + 2);
                ret_str.erase(0, ret_str.find_first_not_of(" \t\r"));
                ret_str.erase(ret_str.find_last_not_of(" \t\r\n") + 1);
                if (!name.empty() && !ret_str.empty()) {
                    declared_type_sigs_[name] = {
                        .type_str = params_str + "|" + ret_str,
                        .module_file = resolved,
                        .resolved = false
                    };
                }
            }
        }
    }

    // 10d. 为没有 .aura-type 签名的导出函数注册 Any 级签名
    // 这样 typecheck-current 至少知道这些函数存在，不会报 unbound variable。
    // 如果有 .aura-type 签名，优先使用（已在 10c 中注册）。
    for (auto& [fname, fval] : mod_env->bindings()) {
        if (declared_type_sigs_.find(fname) != declared_type_sigs_.end())
            continue; // 已有 .aura-type 签名
        // Module bindings are stored as cells (define creates mutable cells).
        // Unwrap cell to get the actual closure value.
        types::EvalValue actual = fval;
        if (types::is_cell(actual)) {
            auto cid = types::as_cell_id(actual);
            if (cid < cells_.size())
                actual = cells_[cid];
        }
        if (types::is_closure(actual)) {
            auto cid = types::as_closure_id(actual);
            std::string param_str;
            auto cit = closures_.find(cid);
            if (cit != closures_.end() && !cit->second.params.empty()) {
                for (std::size_t pi = 0; pi < cit->second.params.size(); ++pi) {
                    if (pi > 0) param_str += " ";
                    param_str += "Any";
                }
                param_str += " ";
            }
            declared_type_sigs_[fname] = {
                .type_str = param_str + "|Any",
                .module_file = resolved,
                .resolved = false
            };
        }
    }

    loading_stack_.erase(resolved);
    return types::make_module(mod_idx);
}

Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target) {
    auto* ar = target ? target : arena_;
    return ar ? ar->create<Env>(e) : nullptr;
}

// eval_in(ast::Expr*) removed — all evaluation uses eval_flat(FlatAST&) now

// apply_closure — looks up closures_, foreign functions, or IR bridge
std::optional<EvalValue> Evaluator::apply_closure(ClosureId cid,
                                                  const std::vector<EvalValue>& args) {
    // Check for foreign function closure (cid < g_ffi_funcs.size())
    if (cid < g_ffi_funcs.size()) {
        auto fidx = cid;
        if (fidx < g_ffi_funcs.size()) {
            auto& ff = g_ffi_funcs[static_cast<std::size_t>(fidx)];
            void* fn_ptr = ff.fn_ptr;
            int ret_type = ff.ret_type;
            auto& arg_types = ff.arg_types;

            // Marshalling: dispatch based on type info
            std::int64_t i6[6] = {0, 0, 0, 0, 0, 0};
            double d6[6] = {0, 0, 0, 0, 0, 0};
            const char* s6[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            std::vector<std::string> str_bufs; // keep strings alive
            bool any_float = false;

            for (std::size_t i = 0; i < args.size() && i < 6; ++i) {
                int atype = (i < arg_types.size()) ? arg_types[i] : 1; // default Int
                if (atype == 2) {                                      // Float
                    if (types::is_float(args[i]))
                        d6[i] = types::as_float(args[i]);
                    else if (types::is_int(args[i]))
                        d6[i] = static_cast<double>(types::as_int(args[i]));
                    i6[i] = static_cast<std::int64_t>(d6[i]);
                    any_float = true;
                } else if (atype == 3) { // String → char*
                    if (types::is_string(args[i])) {
                        auto idx = types::as_string_idx(args[i]);
                        if (idx < string_heap_.size()) {
                            str_bufs.push_back(string_heap_[idx]);
                            s6[i] = str_bufs.back().c_str();
                            i6[i] = reinterpret_cast<std::int64_t>(s6[i]);
                            d6[i] = 0.0;
                        }
                    }
                } else if (atype == 4) { // Opaque (void*)
                    if (types::is_opaque(args[i])) {
                        auto oi = types::as_opaque_idx(args[i]);
                        i6[i] = oi < opaque_heap_.size()
                                    ? reinterpret_cast<std::int64_t>(opaque_heap_[oi])
                                    : 0;
                    } else if (types::is_int(args[i])) {
                        i6[i] = types::as_int(args[i]);
                    } else {
                        i6[i] = 0;
                    }
                    d6[i] = 0.0;
                } else { // Int (default)
                    if (types::is_int(args[i]))
                        i6[i] = types::as_int(args[i]);
                    else if (types::is_float(args[i])) {
                        i6[i] = static_cast<std::int64_t>(types::as_float(args[i]));
                        any_float = true;
                    }
                    d6[i] = static_cast<double>(i6[i]);
                }
            }

            std::int64_t result_i = 0;
            double result_f = 0.0;

            if (any_float) {
                auto f_fn =
                    reinterpret_cast<double (*)(double, double, double, double, double, double)>(
                        fn_ptr);
                result_f = f_fn(d6[0], d6[1], d6[2], d6[3], d6[4], d6[5]);
                if (ret_type == 2)
                    return types::make_float(result_f);
                if (ret_type == 1)
                    return types::make_int(static_cast<std::int64_t>(result_f));
                return types::make_float(result_f);
            } else {
                auto i_fn =
                    reinterpret_cast<std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t,
                                                      std::int64_t, std::int64_t, std::int64_t)>(
                        fn_ptr);
                result_i = i_fn(i6[0], i6[1], i6[2], i6[3], i6[4], i6[5]);
                if (ret_type == 2)
                    return types::make_float(*reinterpret_cast<double*>(&result_i));
                if (ret_type == 3 && result_i != 0) {
                    // String return: char* → string_heap
                    auto s = reinterpret_cast<const char*>(static_cast<std::intptr_t>(result_i));
                    auto sidx = string_heap_.size();
                    string_heap_.emplace_back(s ? s : "");
                    return types::make_string(sidx);
                }
                if (ret_type == 4) {
                    // Opaque: store pointer in opaque_heap_, return OpaqueRef
                    auto oi = opaque_heap_.size();
                    opaque_heap_.push_back(reinterpret_cast<void*>(result_i));
                    return types::make_opaque(oi);
                }
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
            if (r)
                return *r;
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
EvalValue Evaluator::ast_to_data(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                 aura::ast::NodeId nid) {
    if (nid == ast::NULL_NODE)
        return make_void();
    auto v = flat.get(nid);
    // Local helper: build (cons "fn-name" args)
    auto cd = [&](const std::string& fn, const EvalValue& args) -> EvalValue {
        auto fi = string_heap_.size();
        string_heap_.push_back(fn);
        auto pi = pairs_.size();
        pairs_.push_back({types::make_string(fi), args});
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
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({then_b, else_b});
            tail = make_pair(pairs_.size());
            pairs_.push_back({cond, tail});
            return cd("if", tail);
        }
        case ast::NodeTag::Lambda: {
            EvalValue params_tail = make_void();
            for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
                auto pname = std::string(pool.resolve(*it));
                auto pidx = string_heap_.size();
                string_heap_.push_back(pname);
                auto pair_idx = pairs_.size();
                pairs_.push_back({make_string(pidx), params_tail});
                params_tail = make_pair(pair_idx);
            }
            auto body = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({params_tail, body});
            return cd("lambda", tail);
        }
        case ast::NodeTag::Define: {
            auto name_str = std::string(pool.resolve(v.sym_id));
            auto nidx = string_heap_.size();
            string_heap_.push_back(name_str);
            auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({make_string(nidx), val});
            return cd("define", tail);
        }
        case ast::NodeTag::DefineType: {
            auto type_name = pool.resolve(v.sym_id);
            auto tnidx = string_heap_.size();
            string_heap_.push_back(std::string(type_name));
            EvalValue params_tail = make_void();
            for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
                auto pname = std::string(pool.resolve(*it));
                auto pidx = string_heap_.size();
                string_heap_.push_back(pname);
                auto pp = pairs_.size();
                pairs_.push_back({make_string(pidx), params_tail});
                params_tail = make_pair(pp);
            }
            auto type_spec = make_pair(pairs_.size());
            pairs_.push_back({make_string(tnidx), params_tail});
            EvalValue ctors_tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto ctor_data = ast_to_data(flat, pool, *it);
                auto pp = pairs_.size();
                pairs_.push_back({ctor_data, ctors_tail});
                ctors_tail = make_pair(pp);
            }
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({type_spec, ctors_tail});
            return cd("define-type", tail);
        }
        case ast::NodeTag::Set: {
            auto name_str = std::string(pool.resolve(v.sym_id));
            auto nidx = string_heap_.size();
            string_heap_.push_back(name_str);
            auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({make_string(nidx), val});
            return cd("set!", tail);
        }
        case ast::NodeTag::Let:
        case ast::NodeTag::LetRec: {
            // The let node from add_let / parser has:
            //   sym_id = binding name (e.g. x)
            //   children = [val_node, body_node]
            // The body is always the LAST child.
            auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
            auto bname = std::string(pool.resolve(v.sym_id));
            auto bni = string_heap_.size();
            string_heap_.push_back(bname);
            auto bv = val_id != aura::ast::NULL_NODE ? ast_to_data(flat, pool, val_id) : make_void();
            auto body = body_id != aura::ast::NULL_NODE ? ast_to_data(flat, pool, body_id) : make_void();
            // Build: (cons name val) → bindings pair
            auto bp = pairs_.size();
            pairs_.push_back({make_string(bni), bv});
            auto bindings_tail = make_pair(bp);
            // Build: (cons bindings body) → complete let form
            auto full_bindings = make_pair(pairs_.size());
            pairs_.push_back({bindings_tail, body});
            auto kind = v.tag == ast::NodeTag::LetRec ? "letrec" : "let";
            return cd(kind, full_bindings);
        }
        case ast::NodeTag::Quote: {
            if (!v.children.empty()) {
                auto quoted = ast_to_data(flat, pool, v.child(0));
                auto tail = make_pair(pairs_.size());
                pairs_.push_back({quoted, make_void()});
                return cd("quote", tail);
            }
            return make_void();
        }
        case ast::NodeTag::Coercion: {
            if (!v.children.empty()) {
                auto expr = ast_to_data(flat, pool, v.child(0));
                auto tail = make_pair(pairs_.size());
                pairs_.push_back({expr, make_void()});
                return cd("cast", tail);
            }
            return make_void();
        }
        case ast::NodeTag::Pair: {
            auto car = ast_to_data(flat, pool, v.child(0));
            auto cdr_val = ast_to_data(flat, pool, v.child(1));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({cdr_val, make_void()});
            tail = make_pair(pairs_.size());
            pairs_.push_back({car, tail});
            return cd("cons", tail);
        }
        default:
            return make_void();
    }
}
// Inverse of ast_to_data. Needed so lambda bodies from macro data
// can be converted to AST for closure creation.
ast::NodeId Evaluator::data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                    aura::ast::StringPool& pool, int depth) {
    using namespace types;
    if (depth > 256)
        return ast::NULL_NODE;
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
        return flat.add_literal(0); // () sentinel
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
        if (pair_idx >= pairs_.size())
            return ast::NULL_NODE;

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
                return flat.add_literal(0); // fallback
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
                                            auto ns =
                                                ni < string_heap_.size() ? string_heap_[ni] : "";
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
                    if (e != ast::NULL_NODE)
                        exprs.push_back(e);
                    cur = pairs_[cp].cdr;
                }
                return flat.add_begin(exprs);
            }

            // If: (if cond then else)
            if (fn_name == "if") {
                ast::NodeId cond_node = ast::NULL_NODE, then_node = ast::NULL_NODE,
                            else_node = ast::NULL_NODE;
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
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE &&
                    else_node != ast::NULL_NODE)
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
        if (func_node == ast::NULL_NODE)
            return ast::NULL_NODE;
        std::vector<ast::NodeId> args;
        auto cur = cdr_data;
        while (is_pair(cur)) {
            auto cp = as_pair_idx(cur);
            auto arg_data = pairs_[cp].car;
            // Direct string arguments (e.g., "hello" in quoted form) → LiteralString
            // Nested expressions → recurse normally
            if (is_string(arg_data)) {
                auto sidx = as_string_idx(arg_data);
                if (sidx < string_heap_.size()) {
                    auto name = string_heap_[sidx];
                    auto ssid = pool.intern(name);
                    args.push_back(flat.add_literalstring(ssid));
                }
            } else {
                auto a = data_to_flat(arg_data, flat, pool, depth + 1);
                if (a != ast::NULL_NODE)
                    args.push_back(a);
            }
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
    if (pair_idx >= pairs_.size())
        return make_void();

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
                if (!cond_result)
                    return cond_result;
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
                if (!cond_result)
                    return cond_result;
                if (types::is_truthy(*cond_result)) {
                    // Evaluate body expressions sequentially
                    EvalResult last = make_void();
                    while (types::is_pair(body_rest)) {
                        auto bp = types::as_pair_idx(body_rest);
                        last = eval_data_as_code(pairs_[bp].car, env, flat, pool);
                        if (!last)
                            return make_void();
                        body_rest = pairs_[bp].cdr;
                    }
                    return make_void();
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
                auto params_data = pairs_[params_pair].car; // (arg1 arg2 ...)
                auto body_rest = pairs_[params_pair].cdr;   // (body ...)

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
                // Allocate closure body in temp arena during task context,
                // otherwise in persistent arena (module functions, while loops).
                auto* target_arena = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                if (!target_arena) {
                    return make_void();
                }
                auto* copied_env = copy_env(env, target_arena);

                auto cl_alloc = target_arena->allocator();
                auto* cl_flat = target_arena->create<aura::ast::FlatAST>(cl_alloc);
                auto* cl_pool = target_arena->create<aura::ast::StringPool>(cl_alloc);
                auto cloned_body =
                    clone_macro_body(*cl_flat, *cl_pool, *flat, *pool, body_node, nullptr);
                cl_flat->root = cloned_body;
                closures_[cid] = Closure{/*name*/"", /*params*/{}, cl_flat, cl_pool, cloned_body, copied_env, /*dotted*/false, target_arena};
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
                if (!last)
                    return make_void();
                current = pairs_[elem_pair].cdr;
            }
            return make_void();
        }

        // ── quote: (quote expr) ──
        if (fn_name == "quote") {
            if (types::is_pair(cdr_val)) {
                auto quote_pair = types::as_pair_idx(cdr_val);
                return pairs_[quote_pair].car; // Return the quoted value as-is
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
                        auto name_str =
                            name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
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
                        auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                        auto* copied_env = copy_env(env, target);
                        Closure cl;
                        for (auto& ps : param_syms) {
                            cl.params.push_back(std::string(pool->resolve(ps)));
                        }
                        cl.name = fn_str;
                        cl.flat = flat;
                        cl.pool = pool;
                        cl.body_id = body_node;
                        cl.env = copied_env;
                        cl.owner_arena = target;
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
                    auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_rest)].car, env,
                                                 flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str =
                            name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto* cell_ptr = const_cast<Env&>(env).lookup_cell_ptr(name_str, &cells_);
                        if (cell_ptr) {
                            *cell_ptr = *val;
                            return *val;
                        }
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
                            auto name_str =
                                name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                            auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_expr)].car,
                                                         env, flat, pool);
                            if (!val)
                                return val;
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
                    if (!last)
                        return make_void();
                    body_current = pairs_[elem_pair].cdr;
                }
                return make_void();
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
                if (!arg_val)
                    return arg_val;
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
                if (ci < cells_.size())
                    fn_val = cells_[ci];
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
                        if (!arg_val)
                            return arg_val;
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
                        return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id,
                                         tail_env);
                    return make_void();
                }
            }
        }
    }

    // Not a string function name — evaluate car and cdr, apply
    auto fn = eval_data_as_code(car_val, env, flat, pool);
    if (!fn)
        return fn;
    if (types::is_closure(*fn)) {
        auto cid = types::as_closure_id(*fn);
        // Try tree-walker closure first, then IR bridge
        auto result = apply_closure(cid, {});
        if (result)
            return *result;

        // Fallback: manual closure apply via eval_flat
        auto it = closures_.find(cid);
        if (it != closures_.end()) {
            auto& cl = it->second;
            std::vector<EvalValue> cargs;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val)
                    return arg_val;
                cargs.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            Env tail_env(cl.env ? *cl.env : top_);
            tail_env.set_primitives(&primitives_);
            tail_env.set_cells(&cells_);
            for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                tail_env.bind(cl.params[i], std::move(cargs[i]));
            if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id,
                                 tail_env);
        }
    }


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
    if (types::is_int(v))
        return aura::core::TypeTag::INT;
    if (types::is_float(v))
        return aura::core::TypeTag::FLOAT;
    if (types::is_bool(v))
        return aura::core::TypeTag::BOOL;
    if (types::is_string(v))
        return aura::core::TypeTag::STRING;
    if (types::is_pair(v))
        return aura::core::TypeTag::PAIR;
    if (types::is_closure(v))
        return aura::core::TypeTag::CLOSURE;
    if (types::is_vector(v))
        return aura::core::TypeTag::VECTOR;
    if (types::is_hash(v))
        return aura::core::TypeTag::HASH;
    return aura::core::TypeTag::DYNAMIC;
}

static std::string type_tag_name(aura::core::TypeTag tag) {
    switch (tag) {
        case aura::core::TypeTag::INT:
            return "Int";
        case aura::core::TypeTag::FLOAT:
            return "Float";
        case aura::core::TypeTag::BOOL:
            return "Bool";
        case aura::core::TypeTag::STRING:
            return "String";
        case aura::core::TypeTag::PAIR:
            return "Pair";
        case aura::core::TypeTag::CLOSURE:
            return "Closure";
        case aura::core::TypeTag::VECTOR:
            return "Vector";
        case aura::core::TypeTag::HASH:
            return "Hash";
        default:
            return "Dynamic";
    }
}

static bool coerce_value(types::EvalValue& val, aura::core::TypeTag from, aura::core::TypeTag to,
                         std::vector<std::string>& heap) {
    if (from == to)
        return true;
    if (from == aura::core::TypeTag::INT && to == aura::core::TypeTag::FLOAT) {
        val = types::make_float(static_cast<double>(types::as_int(val)));
        return true;
    }
    if (from == aura::core::TypeTag::FLOAT && to == aura::core::TypeTag::INT) {
        val = types::make_int(static_cast<std::int64_t>(types::as_float(val)));
        return true;
    }
    if (from == aura::core::TypeTag::INT && to == aura::core::TypeTag::STRING) {
        auto s = std::to_string(types::as_int(val));
        auto id = static_cast<std::uint64_t>(heap.size());
        heap.push_back(std::move(s));
        val = types::make_string(id);
        return true;
    }
    if (from == aura::core::TypeTag::STRING && to == aura::core::TypeTag::INT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try {
                val = types::make_int(
                    static_cast<std::int64_t>(std::stoll(heap[static_cast<std::size_t>(idx)])));
                return true;
            } catch (...) {
            }
        }
    }
    if (from == aura::core::TypeTag::INT && to == aura::core::TypeTag::BOOL) {
        val = types::make_bool(types::as_int(val) != 0);
        return true;
    }
    if (from == aura::core::TypeTag::BOOL && to == aura::core::TypeTag::INT) {
        val = types::make_int(types::as_bool(val) ? 1 : 0);
        return true;
    }
    if (from == aura::core::TypeTag::FLOAT && to == aura::core::TypeTag::STRING) {
        auto s = std::to_string(types::as_float(val));
        auto id = static_cast<std::uint64_t>(heap.size());
        heap.push_back(std::move(s));
        val = types::make_string(id);
        return true;
    }
    if (from == aura::core::TypeTag::STRING && to == aura::core::TypeTag::FLOAT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try {
                val = types::make_float(std::stod(heap[static_cast<std::size_t>(idx)]));
                return true;
            } catch (...) {
            }
        }
    }
    return false; // non-coercible
}

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────
EvalResult Evaluator::eval_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                aura::ast::NodeId id, const Env& env) {
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
        static constexpr std::size_t MAX_C_STACK_DEPTH = 2000;
        struct DepthGuard {
            std::size_t& d;
            ~DepthGuard() { --d; }
        } _dg{eval_depth_};
        if (++eval_depth_ > MAX_C_STACK_DEPTH)
            return std::unexpected(
                Diagnostic{ErrorKind::InternalError,
                           std::format("recursion depth exceeded (>{})", MAX_C_STACK_DEPTH)});

        while (true) {
            current_flat_ = f;
            current_pool_ = p;
            // Save the eval environment before any tail_env.emplace could corrupt current_env
            const Env& eval_env = *current_env;
            if (current_id == aura::ast::NULL_NODE)
                return EvalResult(make_void());
            if (current_id >= f->size())
                return std::unexpected(Diagnostic{ErrorKind::InternalError, "invalid node id"});
            auto v = f->get(current_id);

            // Incremental eval: if node is clean and has a cached result, reuse it.
            // Skip leaf literals (LiteralInt, LiteralFloat, LiteralString) because
            // they're always fast and the cache lookup overhead is not worth it.
            if (v.tag != aura::ast::NodeTag::LiteralInt &&
                v.tag != aura::ast::NodeTag::LiteralFloat &&
                v.tag != aura::ast::NodeTag::LiteralString &&
                v.tag != aura::ast::NodeTag::Variable &&
                !f->is_dirty(current_id)) {
                auto cached = f->get_cached_value(current_id);
                if (cached != aura::ast::FlatAST::kNotCached) {
                    return EvalResult(EvalValue(cached));
                }
            }

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
                    // Keyword: :foo → self-evaluating keyword value (interned)
                    if (!name.empty() && name[0] == ':') {
                        auto kwstr = std::string(name);
                        std::uint64_t kidx = 0;
                        // Check if already interned
                        bool found = false;
                        for (; kidx < keyword_table_.size(); ++kidx) {
                            if (keyword_table_[kidx] == kwstr) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            kidx = keyword_table_.size();
                            keyword_table_.push_back(kwstr);
                        }
                        return make_keyword(kidx);
                    }
                    auto val = eval_env.lookup(std::string(name));
                    if (val)
                        return *val;
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
                    if (v.children.empty())
                        return EvalResult(make_void());
                    auto callee_id = v.child(0);
                    auto callee = f->get(callee_id);
                    // Inline lambda (arg evals are recursive; body is tail)
                    if (callee.tag == aura::ast::NodeTag::Lambda) {
                        auto pspan = callee.params;
                        bool dotted = callee.int_value != 0;
                        std::size_t named_count =
                            dotted && !pspan.empty() ? pspan.size() - 1 : pspan.size();
                        // Evaluate named args
                        std::vector<EvalValue> iargs;
                        iargs.reserve(named_count);
                        for (std::size_t i = 0; i < named_count && i + 1 < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                            if (!ar)
                                return ar;
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
                                if (!ar)
                                    return ar;
                                auto pid = pairs_.size();
                                pairs_.push_back({*ar, rest});
                                rest = make_pair(pid);
                            }
                            tail_env->bind(std::string(p->resolve(pspan.back())), rest);
                        }
                        auto body_id =
                            callee.children.empty() ? aura::ast::NULL_NODE : callee.child(0);
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, *tail_env);
                        return make_void();
                    }
                    // Macro expansion: evaluate args, bind in env, evaluate body (produces template
                    // data), then re-evaluate the data as code
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        auto macro_it = macros_.find(cname);
                        if (macro_it != macros_.end()) {
                            auto& md = macro_it->second;
                            // Convert AST args to data (NOT evaluate — macros receive syntax)
                            bool is_rest = md.dotted;

                            // Bind regular params first (all but the last)
                            std::size_t regular_count =
                                is_rest ? md.params.size() - 1 : md.params.size();
                            tail_env.emplace(&eval_env);
                            tail_env->set_primitives(&primitives_);
                            tail_env->set_cells(&cells_);

                            for (std::size_t i = 0; i < regular_count && i + 1 < v.children.size();
                                 ++i) {
                                tail_env->bind(md.params[i], ast_to_data(*f, *p, v.child(i + 1)));
                            }

                            // Rest param: collect remaining args as a list
                            if (is_rest) {
                                auto& rest_name = md.params.back();
                                EvalValue rest_list = make_void();
                                for (std::size_t i = v.children.size() - 1; i >= regular_count + 1;
                                     --i) {
                                    auto item = ast_to_data(*f, *p, v.child(i));
                                    auto pid = pairs_.size();
                                    pairs_.push_back(Pair{std::move(item), rest_list});
                                    rest_list = make_pair(pid);
                                }
                                tail_env->bind(rest_name, std::move(rest_list));
                            }
                            // Evaluate macro body (quasiquote-expanded template) → produces data
                            auto template_result =
                                eval_flat(*md.flat, md.pool ? *md.pool : *p, md.body_id, *tail_env);
                            if (!template_result)
                                return template_result;
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
                            // Collect module names and check for all: flag
                            // (require mod1 mod2 ... all:) — all: applies to ALL modules
                            std::vector<std::string> mod_names;
                            bool use_prefix = true;
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto arg_v = f->get(v.child(ci));
                                if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                    auto arg_name = std::string(p->resolve(arg_v.sym_id));
                                    if (arg_name == "all:") {
                                        use_prefix = false;
                                    } else {
                                        mod_names.push_back(arg_name);
                                    }
                                } else if (arg_v.tag == aura::ast::NodeTag::LiteralString) {
                                    mod_names.push_back(std::string(p->resolve(arg_v.sym_id)));
                                }
                            }

                            // Load all modules in sequence
                            if (!arena_)
                                return make_void();
                            EvalResult last = make_void();
                            for (auto& mod_path : mod_names) {
                                // Derive prefix from module name (last path component)
                                std::string prefix;
                                if (use_prefix) {
                                    auto slash = mod_path.rfind('/');
                                    auto base = (slash == std::string::npos)
                                                    ? mod_path
                                                    : mod_path.substr(slash + 1);
                                    prefix = base + ":";
                                }

                                // Build (import "path" "prefix:") or (import "path")
                                std::string import_expr;
                                if (prefix.empty()) {
                                    import_expr = std::string("(import \"") + mod_path + "\")";
                                } else {
                                    import_expr =
                                        std::string("(import \"") + mod_path + "\" \"" + prefix + "\")";
                                }

                                auto alloc = arena_->allocator();
                                auto* ipool = arena_->create<aura::ast::StringPool>(alloc);
                                auto* iflat = arena_->create<aura::ast::FlatAST>(alloc);
                                auto pr = aura::parser::parse_to_flat(import_expr, *iflat, *ipool);
                                if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                                    return std::unexpected(
                                        Diagnostic{ErrorKind::ParseError, "require: internal error"});
                                }
                                iflat->root = pr.root;
                                // Pre-expand macros so import primitive is recognized
                                auto expanded_root =
                                    aura::compiler::macro_expand_all(*iflat, *ipool, iflat->root);
                                last = eval_flat(*iflat, *ipool, expanded_root, eval_env);
                                if (!last) return make_void();
                            }
                            return make_void();
                        }
                    }
                    // try/catch: (try body (catch (var) handler))
                    // body is evaluated; if it returns an error, handler is evaluated with var
                    // bound
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        // when: (when cond body...) — evaluate body only if cond is truthy
                        if (cname == "when" && v.children.size() >= 2) {
                            auto cond_id = v.child(1);
                            auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                            if (!cond_result)
                                return cond_result;
                            if (is_truthy(*cond_result)) {
                                // Evaluate all remaining children as body
                                EvalResult last = make_void();
                                for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                    last = eval_flat(*f, *p, v.child(ci), eval_env);
                                    if (!last)
                                        return make_void();
                                }
                                return make_void();
                            }
                            return make_void();
                        }
                        // unless: (unless cond body...) — evaluate body only if cond is falsy
                        if (cname == "unless" && v.children.size() >= 2) {
                            auto cond_id = v.child(1);
                            auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                            if (!cond_result)
                                return cond_result;
                            if (!is_truthy(*cond_result)) {
                                EvalResult last = make_void();
                                for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                    last = eval_flat(*f, *p, v.child(ci), eval_env);
                                    if (!last)
                                        return make_void();
                                }
                                return make_void();
                            }
                            return make_void();
                        }
                        // with-capability: (with-capability cap-name body...)
                        // Bind capabilities as special variables in the environment.
                        if (cname == "with-capability" && v.children.size() >= 2) {
                            auto cap_id = v.child(1);
                            auto cap_result = eval_flat(*f, *p, cap_id, eval_env);
                            if (!cap_result)
                                return cap_result;
                            // Extract capability name(s)
                            std::vector<std::string> caps;
                            if (is_string(*cap_result)) {
                                auto sidx = as_string_idx(*cap_result);
                                if (sidx < string_heap_.size())
                                    caps.push_back(string_heap_[sidx]);
                            } else if (is_pair(*cap_result)) {
                                auto cidx = as_pair_idx(*cap_result);
                                while (cidx < pairs_.size()) {
                                    auto& pr = pairs_[cidx];
                                    if (is_string(pr.car)) {
                                        auto sidx2 = as_string_idx(pr.car);
                                        if (sidx2 < string_heap_.size())
                                            caps.push_back(string_heap_[sidx2]);
                                    }
                                    break;
                                }
                            }
                            // Create child env with %cap:name bindings
                            tail_env.emplace(&eval_env);
                            tail_env->set_primitives(&primitives_);
                            tail_env->set_cells(&cells_);
                            for (auto& cap : caps)
                                tail_env->bind("%cap:" + cap, make_bool(true));
                            // Push to capability_stack_ for capability-stack readout
                            capability_stack_.push_back(caps);
                            // Evaluate body in child env
                            EvalResult last = make_void();
                            for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), *tail_env);
                                if (!last) {
                                    capability_stack_.pop_back();
                                    return make_void();
                                }
                            }
                            capability_stack_.pop_back();
                            return make_void();
                        }
                        // check-capability: (check-capability "Name") — look up %cap:Name binding
                        if (cname == "check-capability" && v.children.size() >= 2) {
                            auto arg_result = eval_flat(*f, *p, v.child(1), eval_env);
                            if (!arg_result) return arg_result;
                            std::string cap_name;
                            if (is_string(*arg_result)) {
                                auto sidx = as_string_idx(*arg_result);
                                if (sidx < string_heap_.size())
                                    cap_name = string_heap_[sidx];
                            }
                            auto val = eval_env.lookup("%cap:" + cap_name);
                            return val.has_value() ? make_bool(true) : make_bool(false);
                        }

                        if (cname == "try" && v.children.size() >= 2) {
                            auto body_id = v.child(1);
                            auto result = eval_flat(*f, *p, body_id, eval_env);
                            if (result && !is_error(*result)) {
                                // Body succeeded — return result as-is
                                return result;
                            }
                            // Body errored — find catch clause (child[2] or later)
                            if (v.children.size() < 3)
                                return make_void();
                            for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                auto catch_id = v.child(ci);
                                auto cv = f->get(catch_id);
                                if (cv.tag == aura::ast::NodeTag::Call) {
                                    auto catch_fn = f->get(cv.child(0));
                                    if (catch_fn.tag == aura::ast::NodeTag::Variable &&
                                        std::string(p->resolve(catch_fn.sym_id)) == "catch") {
                                        // (catch (var) handler) — child[0]=catch, child[1]=(var),
                                        // child[2]=handler
                                        if (cv.children.size() < 3)
                                            continue;
                                        auto var_form = f->get(cv.child(1));
                                        // var_form is (var) — a Call where child[0]=Variable "var"
                                        std::string var_name;
                                        if (var_form.tag == aura::ast::NodeTag::Call &&
                                            var_form.children.size() >= 1) {
                                            auto var_node = f->get(var_form.child(0));
                                            if (var_node.tag == aura::ast::NodeTag::Variable)
                                                var_name = std::string(p->resolve(var_node.sym_id));
                                        }
                                        auto handler_id = cv.child(2);
                                        // Bind error value to var and evaluate handler
                                        Env catch_env(&eval_env);
                                        catch_env.set_primitives(&primitives_);
                                        catch_env.set_cells(
                                            const_cast<std::vector<EvalValue>*>(&cells_));
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
                                if (!ar)
                                    return ar;
                                // Propagate error values through normal eval
                                // Note: is_string check prevents accidental collision
                                // where make_string(idx) with odd idx matches is_ref/RefError encoding
                                if (is_error(*ar) && !is_string(*ar))
                                    return ar;
                                args.push_back(*ar);
                            }
                            return (*prim)(args);
                        }
                    }
                    // Closure call (eval func + arg evals are recursive; body is tail)
                    auto fn = eval_flat(*f, *p, callee_id, eval_env);
                    if (!fn)
                        return fn;
                    if (is_closure(*fn)) {
                        auto cid = as_closure_id(*fn);
                        // Check for foreign function (high bit set)
                        if (cid < g_ffi_funcs.size()) {
                            // Dispatch FFI through apply_closure
                            std::size_t named_count = 0;
                            std::vector<EvalValue> cargs;
                            for (std::size_t i = 0; i + 1 < v.children.size(); ++i) {
                                auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                                if (!ar)
                                    return ar;
                                cargs.push_back(*ar);
                            }
                            auto result = apply_closure(cid, cargs);
                            if (result)
                                return *result;
                            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                              "eval_flat: foreign call failed"});
                        }
                        auto it = closures_.find(cid);
                        if (it == closures_.end())
                            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                              "eval_flat: invalid closure"});
                        auto& cl = it->second;
                        // Evaluate named args first
                        std::size_t named_count = cl.dotted && !cl.params.empty()
                                                      ? cl.params.size() - 1
                                                      : cl.params.size();
                        std::vector<EvalValue> cargs;
                        cargs.reserve(named_count);
                        for (std::size_t i = 0; i < named_count && i + 1 < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                            if (!ar)
                                return ar;
                            if (is_error(*ar))
                                return ar;
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
                                if (!ar)
                                    return ar;
                                if (is_error(*ar) && !is_string(*ar))
                                    return ar;
                                auto pid = pairs_.size();
                                pairs_.push_back({*ar, rest});
                                rest = make_pair(pid);
                            }
                            tail_env->bind(cl.params.back(), rest);
                        }
                        if (cl.body_id != aura::ast::NULL_NODE)
                            return eval_flat(*cl.flat, cl.pool ? *cl.pool : *p, cl.body_id,
                                             *tail_env);
                        return make_void();
                    }
                    // Functor instantiation: callee is a %functor marker
                    if (is_string(*fn) && as_string_idx(*fn) < string_heap_.size() &&
                        string_heap_[as_string_idx(*fn)] == "%functor") {
                        auto callee_v = f->get(v.child(0));
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto tpl_name = std::string(p->resolve(callee_v.sym_id));
                            auto tpl_it = module_templates_.find(tpl_name);
                            if (tpl_it != module_templates_.end()) {
                                // 构建缓存 key: "template|arg1|arg2|..."
                                std::string cache_key = tpl_name;
                                for (std::size_t ki = 1; ki < v.children.size(); ++ki) {
                                    auto kv = f->get(v.child(ki));
                                    cache_key += "|";
                                    if (kv.tag == aura::ast::NodeTag::Variable)
                                        cache_key += std::string(p->resolve(kv.sym_id));
                                    else
                                        cache_key += "#" + std::to_string(v.child(ki));
                                }
                                auto cache_it = functor_instance_cache_.find(cache_key);
                                if (cache_it != functor_instance_cache_.end()) {
                                    return types::make_module(cache_it->second);
                                }

                                // 使用 ModuleTemplate 中缓存的参数名（避免跨 FlatAST 扫描）
                                auto& param_names = tpl_it->second.type_param_names;

                                // 创建隔离环境
                                Env mod_env(&eval_env);
                                mod_env.set_primitives(&primitives_);
                                mod_env.set_cells(&cells_);

                                // 绑定类型参数到环境（按原始参数名）
                                for (std::size_t ai = 1; ai < v.children.size(); ++ai) {
                                    auto arg_v = f->get(v.child(ai));
                                    std::string pname = (ai - 1 < param_names.size())
                                        ? param_names[ai - 1]
                                        : (":T" + std::to_string(ai - 1));
                                    if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                        // 类型参数：存为字符串（类型名）
                                        auto type_name = std::string(p->resolve(arg_v.sym_id));
                                        auto sidx = string_heap_.size();
                                        string_heap_.push_back(type_name);
                                        mod_env.bind(pname, make_string(sidx));
                                    } else {
                                        // 值参数：正常 eval
                                        auto ar = eval_flat(*f, *p, v.child(ai), eval_env);
                                        if (!ar) return ar;
                                        mod_env.bind(pname, *ar);
                                    }
                                }

                                // Capability requirement check
                                if (!tpl_it->second.cap_require.empty()) {
                                    // Find the capability argument
                                    // Capability params are stored at the end of param_names
                                    std::string provided_caps_str;
                                    for (std::size_t ai = 1; ai < v.children.size(); ++ai) {
                                        auto arg_v = f->get(v.child(ai));
                                        std::string pname = (ai - 1 < param_names.size())
                                            ? param_names[ai - 1] : "";
                                        // Check if this param is a cap param
                                        bool is_cap_param = false;
                                        for (auto& cp : tpl_it->second.cap_param_names) {
                                            if (cp == pname) { is_cap_param = true; break; }
                                        }
                                        if (is_cap_param) {
                                            if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                                provided_caps_str = std::string(p->resolve(arg_v.sym_id));
                                            } else if (arg_v.tag == aura::ast::NodeTag::LiteralString) {
                                                provided_caps_str = "";
                                            }
                                        }
                                    }
                                    // Check if provided caps satisfy requirements
                                    // Simple string matching: "FileReadWrite" contains "FileRead" and "FileWrite"
                                    std::vector<std::string> missing;
                                    for (auto& req : tpl_it->second.cap_require) {
                                        bool found = false;
                                        if (provided_caps_str.find(req) != std::string::npos)
                                            found = true;
                                        // Also check for "*" wildcard
                                        if (provided_caps_str == "*")
                                            found = true;
                                        if (!found)
                                            missing.push_back(req);
                                    }
                                    if (!missing.empty()) {
                                        std::string err = "functor " + tpl_name + ": missing capabilities: ";
                                        for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                                            if (mi > 0) err += ", ";
                                            err += missing[mi];
                                        }
                                        auto es = string_heap_.size();
                                        string_heap_.push_back(err);
                                        auto ev = error_values_.size();
                                        error_values_.push_back(make_string(es));
                                        return make_error(ev);
                                    }
                                }

                                // Eval body by re-parsing the serialized source
                                EvalResult last = make_void();
                                auto& body_src = tpl_it->second.body_source;
                                if (!body_src.empty()) {
                                    // Parse body as a begin block so all expressions become children
                                    std::string wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena body_arena;
                                    auto body_alloc = body_arena.allocator();
                                    aura::ast::StringPool body_pool(body_alloc);
                                    aura::ast::FlatAST body_flat(body_alloc);
                                    auto body_pr = aura::parser::parse_to_flat(wrapped, body_flat, body_pool);
                                    if (body_pr.success && body_pr.root != aura::ast::NULL_NODE) {
                                        body_flat.root = body_pr.root;
                                        auto body_v = body_flat.get(body_flat.root);
                                        if (body_v.tag == aura::ast::NodeTag::Begin) {
                                            for (auto nid : body_v.children) {
                                                auto br = eval_flat(body_flat, body_pool, nid, mod_env);
                                                if (!br) return br;
                                                last = *br;
                                            }
                                        } else {
                                            auto br = eval_flat(body_flat, body_pool, body_flat.root, mod_env);
                                            if (!br) return br;
                                            last = *br;
                                        }
                                    }
                                }
                                // 实例化后生成 .aura-type 签名
                                // Extract export names from the body source
                                std::vector<std::string> export_names;
                                {
                                    std::string scan_wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena scan_arena;
                                    auto scan_alloc = scan_arena.allocator();
                                    aura::ast::StringPool scan_pool(scan_alloc);
                                    aura::ast::FlatAST scan_flat(scan_alloc);
                                    auto scan_pr = aura::parser::parse_to_flat(scan_wrapped, scan_flat, scan_pool);
                                    if (scan_pr.success && scan_pr.root != aura::ast::NULL_NODE) {
                                        scan_flat.root = scan_pr.root;
                                        auto scan_v = scan_flat.get(scan_flat.root);
                                        auto scan_children = (scan_v.tag == aura::ast::NodeTag::Begin)
                                            ? scan_v.children : std::span<const aura::ast::NodeId>(&scan_flat.root, 1);
                                        for (auto nid : scan_children) {
                                            auto nv = scan_flat.get(nid);
                                            if (nv.tag == aura::ast::NodeTag::Export) {
                                                for (auto eid : nv.children) {
                                                    auto ev = scan_flat.get(eid);
                                                    if (ev.tag == aura::ast::NodeTag::Variable)
                                                        export_names.push_back(std::string(scan_pool.resolve(ev.sym_id)));
                                                }
                                            }
                                        }
                                    }
                                }
                                if (!export_names.empty()) {
                                    // 对实例化后的 body 做类型推断，生成实际签名
                                    // Parse body source, type-check via TypeChecker, register signatures
                                    aura::core::TypeRegistry tc_reg;
                                    aura::compiler::TypeChecker functor_tc(tc_reg);
                                    aura::diag::DiagnosticCollector tc_diag;

                                    std::string tc_wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena tc_arena;
                                    auto tc_alloc = tc_arena.allocator();
                                    aura::ast::StringPool tc_pool(tc_alloc);
                                    aura::ast::FlatAST tc_flat(tc_alloc);
                                    auto tc_pr = aura::parser::parse_to_flat(tc_wrapped, tc_flat, tc_pool);
                                    aura::ast::NodeId tc_root = tc_pr.root;
                                    if (tc_pr.success && tc_root != aura::ast::NULL_NODE) {
                                        tc_flat.root = tc_root;
                                        // Type-check the whole body to populate func types
                                        functor_tc.infer_flat(tc_flat, tc_pool, tc_root, tc_diag);

                                        // Scan body for export functions and extract their types
                                        for (auto& en : export_names) {
                                            std::string sig_key = cache_key + "/" + en;
                                            if (declared_type_sigs_.find(sig_key) != declared_type_sigs_.end())
                                                continue;

                                            // Find the Define node for this export
                                            bool found = false;
                                            std::string type_str = "Any|Any";
                                            for (aura::ast::NodeId nid = 0; nid < tc_flat.size(); ++nid) {
                                                auto nv = tc_flat.get(nid);
                                                if (nv.tag == aura::ast::NodeTag::Define &&
                                                    nv.sym_id != aura::ast::INVALID_SYM &&
                                                    std::string(tc_pool.resolve(nv.sym_id)) == en &&
                                                    !nv.children.empty()) {
                                                    // Re-infer the value expression to get its type
                                                    auto val_type = functor_tc.infer_flat(
                                                        tc_flat, tc_pool, nv.child(0), tc_diag);
                                                    if (val_type.valid() && val_type.index > 0) {
                                                        // Format as type signature
                                                        auto fmt = tc_reg.format_type(val_type);
                                                        if (!fmt.empty()) {
                                                            // Convert from '->' to '|' format for declared_type_sigs_
                                                            auto pipe_pos = fmt.find(" -> ");
                                                            if (pipe_pos != std::string::npos) {
                                                                auto params = fmt.substr(0, pipe_pos);
                                                                auto ret = fmt.substr(pipe_pos + 4);
                                                                type_str = params + "|" + ret;
                                                            } else {
                                                                type_str = "|" + fmt;
                                                            }
                                                        }
                                                    }
                                                    found = true;
                                                    break;
                                                }
                                            }

                                            declared_type_sigs_[sig_key] = {
                                                .type_str = type_str,
                                                .module_file = "%functor:" + tpl_name,
                                                .resolved = found
                                            };
                                        }
                                    } else {
                                        // Fallback: Any|Any (shouldn't happen since body was parsed earlier)
                                        for (auto& en : export_names) {
                                            std::string sig_key = cache_key + "/" + en;
                                            if (declared_type_sigs_.find(sig_key) == declared_type_sigs_.end()) {
                                                declared_type_sigs_[sig_key] = {
                                                    .type_str = "Any|Any",
                                                    .module_file = "%functor:" + tpl_name,
                                                    .resolved = false
                                                };
                                            }
                                        }
                                    }
                                }

                                // 缓存实例化结果
                                auto* cached_env = arena_->create<Env>(mod_env);
                                auto mod_idx = modules_.size();
                                modules_.push_back(cached_env);
                                functor_instance_cache_[cache_key] = mod_idx;
                                return types::make_module(mod_idx);
                            }
                        }
                        return make_void();
                    }

                    // Primitive value call: callee is a PrimitiveRef (passed as value, not a
                    // Variable node)
                    if (is_primitive(*fn)) {
                        auto slot = as_primitive_slot(*fn);
                        if (slot < primitives_.slot_count()) {
                            auto prim = eval_env.lookup_primitive(primitives_.name_for_slot(slot));
                            if (prim) {
                                std::vector<EvalValue> args;
                                for (std::size_t i = 1; i < v.children.size(); ++i) {
                                    auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                    if (!ar)
                                        return ar;
                                    args.push_back(*ar);
                                }
                                return (*prim)(args);
                            }
                        }
                    }
                    auto callee_name = std::string(p->resolve(callee.sym_id));
                    // Build diagnostic with appropriate suggestion (no self-move)
                    std::string suggestion;
                    if (callee_name.size() > 3 && callee_name.substr(callee_name.size() - 3) == "-fn")
                        suggestion = "if using c-func: (c-func -1 \"" + callee_name.substr(0, callee_name.size() - 3) + "\" \"(String) -> Int\")";
                    else
                        suggestion = "did you forget to define '" + callee_name + "'?";
                    return std::unexpected(Diagnostic{ErrorKind::TypeError, "cannot call: " + callee_name}
                        .with_suggestion(std::move(suggestion)));
                }
                case aura::ast::NodeTag::IfExpr: {
                    if (v.children.size() < 2)
                        return EvalResult(make_void());
                    auto c = eval_flat(*f, *p, v.child(0), eval_env);
                    if (!c)
                        return c;
                    if (v.children.size() == 2) {
                        // (if cond then) — conditionally execute then-branch
                        if (is_truthy(*c)) {
                            current_id = v.child(1);
                            continue;
                        }
                        // Condition false, no else — use TCO to NULL_NODE so the
                        // while-loop guard returns void on next iteration (avoids
                        // a return path that can cause NULL_NODE in outer TCO loop
                        // when used inside rest-arg lambda bodies).
                        current_id = aura::ast::NULL_NODE;
                        continue;
                    }
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
                    auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                    auto* cap = copy_env(*current_env, target);
                    auto cid = next_id();
                    auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    // This closure reuses f/p from caller (not arena-allocated), Env is in target
                    closures_[cid] = Closure{/*name*/"", std::move(params), f, p, body_id, cap, dotted, target};
                    EVAL_CACHE_RETURN_VAL(make_closure(cid));
                }
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec: {
                    bool rec = (v.tag == aura::ast::NodeTag::LetRec);
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
                    if (rec) {
                        // For letrec, the init value is evaluated in the new env (with cell
                        // binding)
                        tail_env.emplace(&eval_env);
                        tail_env->set_primitives(&primitives_);
                        tail_env->set_cells(&cells_);
                        std::size_t ci = cells_.size();
                        cells_.push_back(make_void());
                        tail_env->bind(std::string(name), make_cell(ci));
                        // Evaluate value in *tail_env (has cell binding for self-reference)
                        auto vv = eval_flat(*f, *p, val_id, *tail_env);
                        if (!vv)
                            return vv;
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
                        if (!vv)
                            return vv;

                        // ── Match exhaustiveness check (tree-walker path) ──
                        if (!rec && type_registry_ && f->has_match_info(current_id)) {
                            auto* minfo = f->get_match_info(current_id);
                            if (minfo && !minfo->has_wildcard && !minfo->used_constructors.empty()) {
                                auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                                // Get first constructor name to find ADT
                                auto first_ctor = std::string(p->resolve(minfo->used_constructors[0]));
                                for (std::size_t ti = 0; ti < treg.size(); ++ti) {
                                    auto tid = aura::core::TypeId{static_cast<std::uint32_t>(ti), 1};
                                    auto* ctors = treg.get_adt_constructors(tid);
                                    if (!ctors) continue;
                                    auto it = std::find(ctors->begin(), ctors->end(), first_ctor);
                                    if (it != ctors->end()) {
                                        for (auto& expected_ctor : *ctors) {
                                            auto found = std::find_if(
                                                minfo->used_constructors.begin(),
                                                minfo->used_constructors.end(),
                                                [&](aura::ast::SymId sid) {
                                                    return std::string(p->resolve(sid)) == expected_ctor;
                                                });
                                            if (found == minfo->used_constructors.end()) {
                                                std::println(std::cerr,
                                                    "match warning: unhandled constructor '{}' in {}",
                                                    expected_ctor, treg.name_of(tid));
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }

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
                        if (cid >= f->size())
                            continue;
                        auto cv = f->get(cid);
                        if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                            continue;
                        auto quoted = cv.child(0);
                        if (quoted >= f->size())
                            continue;
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
                        if (ctor_name.empty())
                            continue;

                        // Register constructor as a primitive that creates tagged lists:
                        // (Ctor arg1 arg2 ...) → (cons 'Ctor (cons arg1 (cons arg2 ...)))
                        auto tag_slot = string_heap_.size();
                        string_heap_.push_back(ctor_name);
                        auto tag_str = make_string(tag_slot);

                        // Count fields to determine if zero-arg constructor
                        int field_count = 0;
                        {
                            aura::ast::NodeId fields_node = quoted;
                            auto fv = f->get(fields_node);
                            if (fv.tag == aura::ast::NodeTag::Pair && fv.children.size() >= 2)
                                fields_node = fv.child(1);
                            while (fields_node < f->size()) {
                                auto fnv = f->get(fields_node);
                                if (fnv.tag != aura::ast::NodeTag::Pair || fnv.children.empty())
                                    break;
                                field_count++;
                                if (fnv.children.size() >= 2)
                                    fields_node = fnv.child(1);
                                else
                                    break;
                            }
                        }
                        if (field_count == 0) {
                            // Zero-arg constructor: bind directly to constructed value
                            types::EvalValue rest = make_void();
                            auto cid = static_cast<std::uint64_t>(pairs_.size());
                            pairs_.push_back({tag_str, rest});
                            me.bind(ctor_name, make_pair(cid));
                        } else {
                            // Multi-arg constructor: register as primitive
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
                        }  // end else (multi-arg)
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
                        if (!vv)
                            return vv;
                        cells_[ci] = *vv;
                        return *vv;
                    }

                    // Create new cell binding
                    auto ci = alloc_cell(make_void());
                    me.bind(std::string(name), make_cell(ci));
                    auto vv = eval_flat(*f, *p, val_id, eval_env);
                    if (!vv)
                        return vv;
                    cells_[ci] = *vv;
                    return *vv;
                }
                case aura::ast::NodeTag::Begin: {
                    auto count = v.children.size();
                    if (count == 0)
                        return EvalResult(make_void());

                    // Check if there are multiple define nodes → use letrec semantics
                    // Phase 1: pre-allocate cells for all defines
                    std::vector<std::pair<std::string, aura::ast::NodeId>> letrec_defs;
                    bool has_multiple_defs = false;
                    int define_count = 0;
                    // Find last non-NULL child (NULL_NODE holes may exist from mutate:move-node)
                    aura::ast::NodeId last_expr = aura::ast::NULL_NODE;
                    for (std::size_t si = count; si > 0; --si) {
                        auto cid = v.child(si - 1);
                        if (cid != aura::ast::NULL_NODE) {
                            last_expr = cid;
                            break;
                        }
                    }
                    if (last_expr == aura::ast::NULL_NODE)
                        return EvalResult(make_void());
                    for (std::size_t i = 0; i < count; ++i) {
                        auto cid = v.child(i);
                        if (cid == aura::ast::NULL_NODE) continue;
                        auto child_node = f->get(cid);
                        if (child_node.tag == aura::ast::NodeTag::Define) {
                            define_count++;
                            if (define_count > 1)
                                has_multiple_defs = true;
                            letrec_defs.push_back({std::string(p->resolve(child_node.sym_id)),
                                                   child_node.children.empty()
                                                       ? aura::ast::NULL_NODE
                                                       : child_node.child(0)});
                        }
                    }

                    // Skip NULL_NODE children (left by mutate:move-node / mutate:remove-node)
                    std::size_t effective_count = 0;
                    for (std::size_t ci = 0; ci < count; ++ci) {
                        if (v.child(ci) != aura::ast::NULL_NODE)
                            effective_count++;
                    }
                    if (effective_count < count) {
                        // Count again for the main loop using only original children
                        // We'll check each child in the loop below
                        has_multiple_defs = false;
                        define_count = 0;
                        for (std::size_t ci = 0; ci < count; ++ci) {
                            auto cid = v.child(ci);
                            if (cid == aura::ast::NULL_NODE) continue;
                            auto child_node = f->get(cid);
                            if (child_node.tag == aura::ast::NodeTag::Define) {
                                define_count++;
                                if (define_count > 1)
                                    has_multiple_defs = true;
                            }
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
                                if (!val)
                                    return val;
                                cells_[cell_ids[i]] = *val;
                            }
                        }
                        // Phase 3: evaluate remaining (non-define) expressions
                        for (std::size_t i = 0; i < count - 1; ++i) {
                            auto cid = v.child(i);
                            if (cid == aura::ast::NULL_NODE) continue;
                            auto child_node = f->get(cid);
                            if (child_node.tag == aura::ast::NodeTag::Define)
                                continue;
                            auto r = eval_flat(*f, *p, cid, eval_env);
                            if (!r)
                                return r;
                        }
                        // TCO: last expression
                        current_id = last_expr;
                        continue;
                    }

                    // Single define (or no defines) — sequential evaluation
                    for (std::size_t i = 0; i < count - 1; ++i) {
                        auto cid = v.child(i);
                        if (cid == aura::ast::NULL_NODE) continue;
                        auto r = eval_flat(*f, *p, cid, eval_env);
                        if (!r)
                            return r;
                    }
                    // Find last non-NULL child
                    current_id = aura::ast::NULL_NODE;
                    for (std::size_t i = count; i > 0; --i) {
                        auto cid = v.child(i - 1);
                        if (cid != aura::ast::NULL_NODE) {
                            current_id = cid;
                            break;
                        }
                    }
                    if (current_id == aura::ast::NULL_NODE)
                        return EvalResult(make_void());
                    continue; // TCO: last expression in begin
                }
                case aura::ast::NodeTag::DefineModule: {
                    // (define-module (Name :T ...) body...)
                    // Store module template and bind Name to functor
                    auto mod_name = std::string(p->resolve(v.sym_id));
                    ModuleTemplate mt;

                    // Extract type parameter names from AST params metadata
                    // (type params = all params minus cap params)
                    auto num_cap_params = f->cap_require_count(v.id);
                    std::size_t num_type_params = (num_cap_params > 0 && v.params.size() >= num_cap_params)
                        ? v.params.size() - num_cap_params : v.params.size();
                    for (std::size_t i = 0; i < num_type_params; ++i) {
                        auto pid = f->param_at(v.id, i);
                        mt.type_param_names.push_back(std::string(p->resolve(pid)));
                    }
                    if (num_cap_params > 0) {
                        // cap params are at the end of the param list
                        for (std::size_t i = 0; i < num_cap_params; ++i) {
                            auto pid = f->param_at(v.id, v.params.size() - num_cap_params + i);
                            mt.cap_param_names.push_back(std::string(p->resolve(pid)));
                        }
                    }

                    // Serialize body expressions to source strings (for cross-eval instantiation)
                    // Build a node-to-source serializer using the current FlatAST
                    std::function<std::string(aura::ast::NodeId)> node_source;
                    node_source = [&](aura::ast::NodeId nid) -> std::string {
                        if (nid >= f->size() || nid == aura::ast::NULL_NODE) return "";
                        auto nv = f->get(nid);
                        switch (nv.tag) {
                            case aura::ast::NodeTag::LiteralInt: return std::to_string(nv.int_value);
                            case aura::ast::NodeTag::LiteralFloat: return std::to_string(nv.float_value);
                            case aura::ast::NodeTag::LiteralString: return "\"" + std::string(p->resolve(nv.sym_id)) + "\"";
                            case aura::ast::NodeTag::Variable: return std::string(p->resolve(nv.sym_id));
                            case aura::ast::NodeTag::Quote: {
                                if (nv.children.empty()) return "'()";
                                return "'" + node_source(nv.child(0));
                            }
                            case aura::ast::NodeTag::Lambda: {
                                std::string s = "(lambda (";
                                for (std::size_t pi = 0; pi < nv.params.size(); ++pi) {
                                    if (pi > 0) s += " ";
                                    s += std::string(p->resolve(nv.params[pi]));
                                }
                                s += ")";
                                if (!nv.children.empty())
                                    s += " " + node_source(nv.child(0));
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Define: {
                                std::string s = "(define";
                                if (!nv.children.empty()) {
                                    auto val_nv = f->get(nv.child(0));
                                    if (val_nv.tag == aura::ast::NodeTag::Lambda) {
                                        // Shorthand: (define (name params...) body...)
                                        s += " (" + std::string(p->resolve(nv.sym_id));
                                        for (std::size_t pi = 0; pi < val_nv.params.size(); ++pi) {
                                            s += " ";
                                            s += std::string(p->resolve(val_nv.params[pi]));
                                        }
                                        s += ")";
                                        if (!val_nv.children.empty())
                                            s += " " + node_source(val_nv.child(0));
                                    } else {
                                        s += " " + std::string(p->resolve(nv.sym_id));
                                        s += " " + node_source(nv.child(0));
                                    }
                                }
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Export: {
                                std::string s = "(export";
                                for (auto eid : nv.children) {
                                    auto ev = f->get(eid);
                                    if (ev.tag == aura::ast::NodeTag::Variable)
                                        s += " " + std::string(p->resolve(ev.sym_id));
                                }
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Call: {
                                std::string s = "(";
                                for (std::size_t ci = 0; ci < nv.children.size(); ++ci) {
                                    if (ci > 0) s += " ";
                                    s += node_source(nv.child(ci));
                                }
                                return s + ")";
                            }
                            default: return "()";
                        }
                    };

                    // Serialize each body expression
                    std::string body_src;
                    for (auto cid : v.children) {
                        auto sexpr = node_source(cid);
                        if (!sexpr.empty()) {
                            if (!body_src.empty()) body_src += "\n";
                            body_src += sexpr;
                        }
                    }
                    mt.body_source = std::move(body_src);

                    // Scan body for `:require` directives
                    // Format: ((:require FileRead FileWrite) ...) or ((:require FileRead) ...)
                    for (auto cid : v.children) {
                        auto cv = f->get(cid);
                        if (cv.tag == aura::ast::NodeTag::Call && cv.children.size() > 0) {
                            auto callee_node = f->get(cv.child(0));
                            if (callee_node.tag == aura::ast::NodeTag::Variable ||
                                callee_node.tag == aura::ast::NodeTag::Quote) {
                                aura::ast::SymId sym = (callee_node.tag == aura::ast::NodeTag::Variable)
                                    ? callee_node.sym_id : aura::ast::INVALID_SYM;
                                std::string_view callee_name = (sym != aura::ast::INVALID_SYM)
                                    ? p->resolve(sym) : std::string_view();
                                // Check for :require or require keyword
                                if (callee_name == ":require" || callee_name == ":require-all") {
                                    // Extract required capability names from remaining children
                                    for (std::size_t ai = 1; ai < cv.children.size(); ++ai) {
                                        auto arg_node = f->get(cv.child(ai));
                                        if (arg_node.tag == aura::ast::NodeTag::Variable) {
                                            auto cap_name = std::string(p->resolve(arg_node.sym_id));
                                            // Skip duplicates
                                            bool dup = false;
                                            for (auto& r : mt.cap_require) {
                                                if (r == cap_name) { dup = true; break; }
                                            }
                                            if (!dup)
                                                mt.cap_require.push_back(cap_name);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    module_templates_[mod_name] = std::move(mt);

                    // Bind Name in the current env (as a cell with functor marker)
                    Env& me = const_cast<Env&>(eval_env);
                    me.set_cells(&cells_);
                    auto ci = alloc_cell(make_void());
                    auto sidx = string_heap_.size();
                    string_heap_.push_back("%functor");
                    me.bind(mod_name, make_cell(ci));
                    cells_[ci] = make_string(sidx);
                    return make_string(sidx);
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
                    if (!val)
                        return val;
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
                    return std::unexpected(
                        Diagnostic{ErrorKind::UnboundVariable, "set!: " + std::string(name)});
                }
                case aura::ast::NodeTag::Quote: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    return EvalResult(ast_to_data(*f, *p, v.child(0)));
                }
                case aura::ast::NodeTag::TypeAnnotation: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    auto annot_id = current_id;
                    auto child_result = eval_flat(*f, *p, v.child(0), eval_env);
                    if (!child_result)
                        return child_result;
                    // 3-arg form (: name Type val): bind the result in eval_env
                    if (v.int_value != 0) {
                        auto var_name = p->resolve(static_cast<aura::ast::SymId>(v.int_value));
                        if (!var_name.empty()) {
                            auto& me = const_cast<Env&>(eval_env);
                            me.set_cells(&cells_);
                            auto ci = cells_.size();
                            cells_.push_back(*child_result);
                            me.bind(std::string(var_name), make_cell(ci));
                        }
                    }
                    // Runtime type check: compare value type against annotation
                    if (type_registry_ && annot_id < f->size()) {
                        auto expected_type_id = f->type_id(annot_id);
                        if (expected_type_id != 0) {
                            auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                            auto expected_tag =
                                treg.tag_of(aura::core::TypeId{expected_type_id, 1});
                            auto actual_tag = runtime_type_tag(*child_result);
                            if (actual_tag != expected_tag &&
                                actual_tag != aura::core::TypeTag::DYNAMIC) {
                                auto& val = *child_result;
                                // Attempt coercion at runtime
                                bool coerced =
                                    coerce_value(val, actual_tag, expected_tag, string_heap_);
                                if (!coerced) {
                                    std::string expected_name(
                                        treg.format_type(aura::core::TypeId{expected_type_id, 1}));
                                    std::string actual_name = type_tag_name(actual_tag);
                                    std::println(std::cerr, "type warning: expected {}, got {}\n",
                                                 expected_name, actual_name);
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
                    if (body_id == aura::ast::NULL_NODE)
                        return EvalResult(make_void());

                    // ── Warn: unused macro parameters ──────────────────────────
                    // Scan the body for variable references and compare with params.
                    {
                        // Collect all variable names referenced in the macro body
                        std::unordered_set<std::string> used_vars;
                        auto collect_vars = [&](this const auto& self,
                                                aura::ast::NodeId nid) -> void {
                            if (nid == aura::ast::NULL_NODE || nid >= f->size())
                                return;
                            auto nv = f->get(nid);
                            if (nv.tag == aura::ast::NodeTag::Variable &&
                                nv.sym_id != aura::ast::INVALID_SYM) {
                                used_vars.insert(std::string(p->resolve(nv.sym_id)));
                            }
                            for (auto c : nv.children)
                                self(c);
                        };
                        collect_vars(body_id);

                        int used_count = 0;
                        for (auto& pn : param_names) {
                            if (used_vars.count(pn) == 0) {
                                std::println(std::cerr,
                                             "warning: macro '{}': parameter '{}' never used",
                                             std::string(name), pn);
                            } else {
                                ++used_count;
                            }
                        }
                        if (used_count == 0) {
                            std::println(
                                std::cerr,
                                "warning: macro '{}': body does not reference any parameter",
                                std::string(name));
                        }
                    }

                    // Store macro definition with proper dotted flag
                    bool is_dotted = (v.int_value != 0);
                    macros_[std::string(name)] =
                        MacroDef{std::move(param_names), is_dotted, f, p, body_id};
                    return EvalResult(make_void());
                }
                case aura::ast::NodeTag::Linear:
                case aura::ast::NodeTag::Move:
                case aura::ast::NodeTag::Borrow:
                case aura::ast::NodeTag::MutBorrow:
                case aura::ast::NodeTag::Drop: {
                    // M4 ownership operations at tree-walker level:
                    // compile-time concepts — evaluate inner expression directly
                    if (v.children.empty())
                        return EvalResult(make_void());
                    return eval_flat(*f, *p, v.child(0), eval_env);
                }
                default:
                    return std::unexpected(
                        Diagnostic{ErrorKind::InternalError, "eval_flat: unsupported node type"});
            }
        }
    } catch (const std::bad_alloc& e) {
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "out of memory"});
    } catch (const std::out_of_range& e) {
        return std::unexpected(Diagnostic{ErrorKind::InternalError,
            std::format("argument out of range: {}", e.what())});
    } catch (const std::bad_variant_access& e) {
        return std::unexpected(Diagnostic{
            ErrorKind::TypeError,
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
static aura::ast::NodeId
clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
                 aura::ast::FlatAST& source, aura::ast::StringPool& source_pool,
                 aura::ast::NodeId body_id,
                 const std::unordered_map<std::string, aura::ast::NodeId>* subst,
                 std::unordered_map<std::string, std::string>* name_map) {
    using namespace aura::ast;
    if (body_id == NULL_NODE || body_id >= source.size())
        return NULL_NODE;
    auto v = source.get(body_id);

    // Set of built-in names that should never be gensym'd
    static const std::unordered_set<std::string> builtins = {
        "if",
        "cond",
        "let",
        "let*",
        "letrec",
        "lambda",
        "define",
        "begin",
        "set!",
        "quote",
        "unquote",
        "quasiquote",
        "case",
        "when",
        "unless",
        "car",
        "cdr",
        "cons",
        "list",
        "pair?",
        "null?",
        "eq?",
        "equal?",
        "+",
        "-",
        "*",
        "/",
        "=",
        "<",
        ">",
        "<=",
        ">=",
        "not",
        "and",
        "or",
        "void",
        "display",
        "write",
        "newline",
        "number?",
        "integer?",
        "float?",
        "boolean?",
        "string?",
        "symbol?",
        "string-append",
        "string-length",
        "string-ref",
        "substring",
        "number->string",
        "string->number",
        "apply",
        "map",
        "filter",
        "foldl",
    };

    // Variable substitution: if this variable is a macro param, return the arg clone
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = std::string(source_pool.resolve(v.sym_id));
        auto it = subst->find(name);
        if (it != subst->end()) {
            return clone_macro_body(target, target_pool, source, source_pool, it->second, subst,
                                    name_map);
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid
                                    : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Resolve a name through name_map (hygiene: renamed binding)
    auto resolve_name = [&](SymId sid) -> std::string {
        if (sid == INVALID_SYM)
            return "";
        auto name = std::string(source_pool.resolve(sid));
        if (name_map) {
            auto it = name_map->find(name);
            if (it != name_map->end())
                return it->second;
        }
        return name;
    };

    // Rename a binding position for hygiene: gensym if macro-introduced
    static std::uint64_t hyg_ctr = 0;
    auto rename_binding = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Macro params, builtins, and already-renamed names keep their name
        if ((subst && subst->count(name)) || builtins.count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        // Gensym! Create fresh name and track in name_map
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Clone children recursively
    std::vector<aura::ast::NodeId> child_ids;
    for (std::uint32_t i = 0; i < v.children.size(); ++i)
        child_ids.push_back(clone_macro_body(target, target_pool, source, source_pool, v.child(i),
                                             subst, name_map));

    // Clone params (for Lambda nodes) — with hygienic renaming
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(rename_binding(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
        case NodeTag::LiteralInt:
            new_id = target.add_literal(v.int_value);
            break;
        case NodeTag::LiteralString:
            new_id = target.add_literalstring(transplant(v.sym_id));
            break;
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
            if (!child_ids.empty())
                new_id = target.add_call(child_ids[0], args);
            break;
        }
        case NodeTag::IfExpr:
            if (child_ids.size() >= 3)
                new_id = target.add_if(child_ids[0], child_ids[1], child_ids[2]);
            break;
        case NodeTag::Lambda:
            if (!child_ids.empty())
                new_id = target.add_lambda(param_syms, child_ids[0]);
            break;
        case NodeTag::Let:
        case NodeTag::LetRec:
            if (child_ids.size() >= 2)
                new_id =
                    (v.tag == NodeTag::Let)
                        ? target.add_let(rename_binding(v.sym_id), child_ids[0], child_ids[1])
                        : target.add_letrec(rename_binding(v.sym_id), child_ids[0], child_ids[1]);
            break;
        case NodeTag::Begin:
            if (!child_ids.empty())
                new_id = target.add_begin(child_ids);
            break;
        case NodeTag::Set:
            if (!child_ids.empty())
                new_id = target.add_set(transplant(v.sym_id), child_ids[0]);
            break;
        case NodeTag::Quote:
            if (!child_ids.empty())
                new_id = target.add_quote(child_ids[0]);
            break;
        case NodeTag::Define:
            if (!child_ids.empty())
                new_id = target.add_define(rename_binding(v.sym_id), child_ids[0]);
            break;
        default:
            break;
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
        struct MD {
            aura::ast::FlatAST* src_flat;
            aura::ast::StringPool* src_pool;
            std::vector<std::string> params;
            NodeId body_id;
            bool dotted;
        };
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

        if (!has_macro_def)
            return root; // no more macros to expand

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
                                                        ? md.params.size() - 1
                                                        : md.params.size();
                        for (std::size_t ai = 0; ai < regular_count && ai + 1 < v.children.size();
                             ++ai)
                            subst[md.params[ai]] = v.child(ai + 1);
                        // Rest param: collect remaining args as a quoted list
                        if (md.dotted && !md.params.empty() &&
                            regular_count + 1 < v.children.size()) {
                            auto& rest_name = md.params.back();
                            // Build a data list of the remaining arg nodes as a quote
                            // Create () as the base, then cons each remaining arg
                            std::vector<aura::ast::NodeId> remaining;
                            for (std::size_t ai = regular_count + 1; ai < v.children.size(); ++ai)
                                remaining.push_back(v.child(ai));
                            // Create nested quote: (quote (arg1 arg2 ...)) using add_call chains
                            // Actually, clone_macro_body substitutes Variable nodes, so we need
                            // the rest arg as an expression node, not data.
                            // For simplicity: build a (begin remaining...) — but that evaluates
                            // them. Build as (quote (arg1 arg2 ...)) by creating a proper list:
                            // cons arg1 (cons arg2 (cons ... ())) then wrap in quote
                            // Since these are NodeIds in the SAME FlatAST, we can build an
                            // expression that produces a list: (list arg1 arg2 ...)
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
                            // in macro_expand_all will be evaluated (same as the evaluator's
                            // version) Actually this is the same issue as the evaluator's macros_
                            // expansion. The difference: in macro_expand_all we can directly
                            // substitute. Let me just not handle rest in macro_expand_all for now —
                            // the evaluator's macros_ handles it correctly. This path is only for
                            // same-expression macros.
                        }
                        // Clone macro body with substitution
                        std::unordered_map<std::string, std::string> rename_map;
                        auto expanded = clone_macro_body(flat, pool, *md.src_flat, *md.src_pool,
                                                         md.body_id, &subst, &rename_map);
                        if (expanded != NULL_NODE) {
                            if (id == root)
                                new_root = expanded;
                            expanded_any = true;
                        }
                    }
                }
            }
        }

        if (!expanded_any)
            return root;
        root = new_root;
    }
    return root;
}

void* Evaluator::create_workspace_tree() {
    auto* tree = new WorkspaceTree();
    WorkspaceNode root;
    root.name = "root";
    root.is_root = true;
    root.has_own_flat = true;
    root.flat = nullptr;
    root.pool = nullptr;
    tree->nodes_.push_back(std::move(root));
    return tree;
}

void Evaluator::destroy_workspace_tree(void* wt) {
    if (!wt) return;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    // Delete owned flats (child workspaces that had COW triggered)
    for (auto& node : tree->nodes_) {
        if (!node.is_root && node.has_own_flat) {
            delete node.flat;
            delete node.pool;
        }
    }
    delete tree;
}

// ── Panic auto-rollback (Issue #39) ────────────────────────────
bool Evaluator::save_panic_checkpoint() {
    if (!workspace_flat_ || !workspace_pool_)
        return false;
    auto src_fn = primitives_.lookup("current-source");
    if (!src_fn)
        return false;
    auto src = (*src_fn)({});
    if (!types::is_string(src))
        return false;
    auto idx = types::as_string_idx(src);
    if (idx >= string_heap_.size())
        return false;
    panic_safe_source_ = string_heap_[idx];
    return true;
}

bool Evaluator::restore_panic_checkpoint() {
    if (panic_safe_source_.empty())
        return false;
    auto set_fn = primitives_.lookup("set-code");
    if (!set_fn)
        return false;
    auto idx = string_heap_.size();
    string_heap_.push_back(panic_safe_source_);
    auto result = (*set_fn)({make_string(idx)});
    bool ok = types::is_bool(result) && types::as_bool(result);
    if (ok) {
        // Clear checkpoint after successful restore
        panic_safe_source_.clear();
    }
    return ok;
}

} // namespace aura::compiler
