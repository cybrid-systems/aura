module;
#include <cstdio>
module aura.compiler.evaluator;
import std;
import aura.core.ast;
import aura.compiler.value;
import aura.parser.parser;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

using namespace aura::diag;

// Forward decl: macro body cloner (defined at end of file)
static aura::ast::NodeId clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
    aura::ast::FlatAST& source, aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst = nullptr);

// ── Env::lookup: returns EvalValue variant ─────────────────────
std::optional<EvalValue> Env::lookup(const std::string& n) const {
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
    return parent_ ? parent_->lookup(n) : std::nullopt;
}

// ── Helper: coerce EvalValue to int (string → int parsing) ────
namespace {
    static std::int64_t coerce_to_int(const EvalValue& v, const std::vector<std::string>* heap) {
        if (is_int(v)) return as_int(v);
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
}

// ── Primitives: EvalValue operations ──────────────────────────

Primitives::Primitives() {
    table_["+"]  = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) + coerce_to_int(a[1], string_heap_)); };
    table_["-"]  = [this](auto& a) {
        auto v0 = coerce_to_int(a[0], string_heap_);
        return a.size() == 1 ? make_int(-v0) : make_int(v0 - coerce_to_int(a[1], string_heap_));
    };
    table_["*"]  = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) * coerce_to_int(a[1], string_heap_)); };
    table_["/"]  = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) / coerce_to_int(a[1], string_heap_)); };
    table_["="]  = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) == coerce_to_int(a[1], string_heap_) ? 1 : 0); };
    table_["<"]  = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) < coerce_to_int(a[1], string_heap_) ? 1 : 0); };
    table_[">"]  = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) > coerce_to_int(a[1], string_heap_) ? 1 : 0); };
    table_["<="] = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) <= coerce_to_int(a[1], string_heap_) ? 1 : 0); };
    table_[">="] = [this](auto& a) { return make_int(coerce_to_int(a[0], string_heap_) >= coerce_to_int(a[1], string_heap_) ? 1 : 0); };
    // Ghuloum Step 9: booleans
    table_["not"]  = [](auto& a) { return make_int(!is_truthy(a[0]) ? 1 : 0); };
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
    table_["eq?"]  = [](auto& a) { return make_int(a[0] == a[1] ? 1 : 0); };
}

// ── I/O helper for EvalValue ──────────────────────────────────
namespace {
    static void io_print_val(const EvalValue& v, const std::vector<std::string>* heap, bool quote) {
        if (is_void(v))         { std::printf("()"); return; }
        if (is_bool(v))         { std::printf(as_bool(v) ? "#t" : "#f"); return; }
        if (is_int(v))          { std::printf("%ld", (long)as_int(v)); return; }
        if (is_string(v) && heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size()) {
                if (quote) std::printf("\"%s\"", (*heap)[idx].c_str());
                else       std::printf("%s",       (*heap)[idx].c_str());
                return;
            }
        }
        if (is_pair(v))         { std::printf("<pair[%zu]>", (size_t)as_pair_idx(v)); return; }
        if (is_closure(v))      { std::printf("<closure[%zu]>", (size_t)as_closure_id(v)); return; }
        if (is_cell(v))         { std::printf("<cell[%zu]>", (size_t)as_cell_id(v)); return; }
        std::printf("<unknown>");
    }
}

void Evaluator::init_pair_primitives() {
    primitives_.add("cons", [this](const auto& a) {
        auto id = pairs_.size();
        pairs_.push_back({a[0], a[1]});
        return make_pair(id);
    });
    primitives_.add("car", [this](const auto& a) {
        if (!is_pair(a[0])) return make_int(0);
        auto id = as_pair_idx(a[0]);
        return id < pairs_.size() ? pairs_[id].car : make_int(0);
    });
    primitives_.add("cdr", [this](const auto& a) {
        if (!is_pair(a[0])) return make_int(0);
        auto id = as_pair_idx(a[0]);
        return id < pairs_.size() ? pairs_[id].cdr : make_int(0);
    });
    primitives_.add("pair?", [](const auto& a) {
        if (a.empty()) return make_int(0);
        return make_int(is_pair(a[0]) ? 1 : 0);
    });
    primitives_.add("string?", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        return make_int(is_string(a[0]) ? 1 : 0);
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
        if (a.size() < 2) return make_int(0);
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size()) s = string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        auto pos = static_cast<std::size_t>(as_int(a[1]));
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
        if (a.size() < 2) return make_int(0);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (is_int(v)) return std::to_string(as_int(v));
            return "";
        };
        return make_int(to_str(a[0]) == to_str(a[1]) ? 1 : 0);
    });
    primitives_.add("string<?", [this](const auto& a) {
        if (a.size() < 2) return make_int(0);
        auto to_str = [this](const EvalValue& v) -> std::string {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (is_int(v)) return std::to_string(as_int(v));
            return "";
        };
        return make_int(to_str(a[0]) < to_str(a[1]) ? 1 : 0);
    });
    primitives_.add("number->string", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        auto s = is_int(a[0]) ? std::to_string(as_int(a[0])) : "0";
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(s));
        return make_string(id);
    });
    primitives_.add("string->number", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_int(0);
        auto i = as_string_idx(a[0]);
        if (i >= string_heap_.size()) return make_int(0);
        try { return make_int(static_cast<std::int64_t>(std::stoll(string_heap_[i]))); }
        catch (...) { return make_int(0); }
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
        while (!is_void(v)) {
            if (!is_pair(v)) return make_bool(false);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return make_bool(false);
            v = pairs_[idx].cdr;  // follow cdr chain
        }
        return make_int(1);
    });
    primitives_.add("null?", [](const auto& a) {
        return make_int(a.empty() || is_void(a[0]) ? 1 : 0);
    });
    primitives_.add("length", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        auto v = a[0]; std::int64_t n = 0;
        while (!is_void(v)) {
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
        while (!is_void(v)) {
            if (!is_pair(v)) return make_int(0);
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return make_int(0);
            if (pairs_[idx].car == val) return v;
            v = pairs_[idx].cdr;
        }
        return make_int(0);
    });
    primitives_.add("append", [this](const auto& a) {
        if (a.empty()) return make_void();
        if (a.size() < 2) return a[0];
        auto list1 = a[0]; auto list2 = a[1];
        if (is_void(list1)) return list2;
        EvalValue result = make_void(); EvalValue tail = make_void();
        auto v = list1;
        while (!is_void(v)) {
            if (!is_pair(v)) return list1;
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size()) return list1;
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, make_void()});
            auto new_pair = make_pair(new_id);
            if (is_void(result)) result = new_pair;
            else {
                auto tidx = as_pair_idx(tail);
                pairs_[tidx].cdr = new_pair;
            }
            tail = new_pair;
            v = pairs_[idx].cdr;
        }
        // Set last cdr to list2
        if (!is_void(tail)) {
            auto tidx = as_pair_idx(tail);
            pairs_[tidx].cdr = list2;
        }
        return result;
    });
    primitives_.add("reverse", [this](const auto& a) {
        if (a.empty()) return make_int(0);
        auto v = a[0]; EvalValue result = make_void();
        while (!is_void(v)) {
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
        if (a.size() < 2 || !is_closure(a[0]) || is_void(a[1])) return make_void();
        auto cid = as_closure_id(a[0]);
        auto it = closures_.find(cid);
        if (it == closures_.end() || it->second.params.empty()) return make_void();
        auto& closure = it->second;
        auto param = closure.params[0];

        // Walk the list, apply func to each element, build result in order
        EvalValue result = make_void();
        EvalValue tail = make_void();
        bool first = true;
        EvalValue current = a[1];

        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size()) break;

            Env ne(closure.env ? *closure.env : Env());
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            ne.bind(param, pairs_[idx].car);

            auto r = closure.flat ? eval_flat(*closure.flat,
                                                *closure.pool,
                                                closure.body_id, ne) :
                                    EvalResult(make_void());
            if (!r) return make_void();

            auto new_id = pairs_.size();
            pairs_.push_back({*r, make_void()});
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
        if (a.size() < 2 || !is_closure(a[0]) || is_void(a[1])) return make_void();
        auto cid = as_closure_id(a[0]);
        auto it = closures_.find(cid);
        if (it == closures_.end() || it->second.params.empty()) return make_void();
        auto& closure = it->second;
        auto param = closure.params[0];

        EvalValue result = make_void();
        EvalValue tail = make_void();
        bool first = true;
        EvalValue current = a[1];

        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size()) break;

            Env ne(closure.env ? *closure.env : Env());
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            ne.bind(param, pairs_[idx].car);

            auto r = closure.flat ? eval_flat(*closure.flat,
                                                *closure.pool,
                                                closure.body_id, ne) :
                                    EvalResult(make_void());
            if (!r) break;

            // Check truthiness: keep if the predicate returns truthy
            bool keep = types::is_truthy(*r);
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

    // ── L6.8: Runtime type introspection ────────────────────────────
    auto infer_type_name = [](const EvalValue& v) -> const char* {
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
        if (a.size() < 2) return make_int(1);
        struct EqPair { EvalValue x, y; };
        std::vector<EqPair> stack;
        stack.push_back({a[0], a[1]});
        while (!stack.empty()) {
            auto p = stack.back(); stack.pop_back();
            if (p.x == p.y) continue;
            if (is_pair(p.x) && is_pair(p.y)) {
                auto ix = as_pair_idx(p.x);
                auto iy = as_pair_idx(p.y);
                if (ix >= pairs_.size() || iy >= pairs_.size()) return make_int(0);
                stack.push_back({pairs_[ix].cdr, pairs_[iy].cdr});
                stack.push_back({pairs_[ix].car, pairs_[iy].car});
                continue;
            }
            if (is_string(p.x) && is_string(p.y)) {
                auto six = as_string_idx(p.x);
                auto siy = as_string_idx(p.y);
                if (six >= string_heap_.size() || siy >= string_heap_.size()) return make_int(0);
                if (string_heap_[six] != string_heap_[siy]) return make_int(0);
                continue;
            }
            return make_int(0);
        }
        return make_int(1);
    });

    static std::atomic<std::uint64_t> gs_counter_{0};
    primitives_.add("gensym", [](const auto&) -> EvalValue {
        auto id = gs_counter_.fetch_add(1, std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(id));
    });
    primitives_.add("display", [this](const auto& a) {
        if (a.empty()) return make_int(1);
        io_print_val(a[0], &string_heap_, false);
        return make_int(1);
    });
    primitives_.add("write", [this](const auto& a) -> EvalValue {
        if (a.empty()) return make_int(1);
        io_print_val(a[0], &string_heap_, true);
        return make_int(1);
    });
    primitives_.add("newline", [](const auto&) { std::printf("\n"); return make_int(1); });
    primitives_.add("error", [](const auto& a) -> EvalValue {
        std::string msg = a.empty() ? "error" : (is_int(a[0]) ? std::to_string(as_int(a[0])) : "error");
        throw std::runtime_error(msg);
        return make_int(0);
    });
    primitives_.add("assert", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_truthy(a[0])) {
            std::string msg = "assertion failed";
            if (a.size() > 1) {
                if (is_string(a[1])) {
                    auto idx = as_string_idx(a[1]);
                    if (idx < string_heap_.size()) msg = string_heap_[idx];
                } else if (is_int(a[1])) {
                    msg = std::to_string(as_int(a[1]));
                }
            }
            throw std::runtime_error(msg);
        }
        return make_int(1);
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

    primitives_.add("load-module", [this](const auto& a) {
        if (a.empty() || !is_string(a[0])) return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size()) return make_void();
        auto& path = string_heap_[idx];

        // Read file content
        std::ifstream f(path);
        if (!f) return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) return make_void();

        // Parse in arena
        if (!arena_) return make_void();
        auto alloc = arena_->allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return make_void();
        flat.root = pr.root;

        // Evaluate in current top env
        auto result = eval_flat(flat, pool, flat.root, top_env());
        if (!result) return make_void();
        return *result;
    });
}

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
    primitives_.set_string_heap(&string_heap_);
    init_pair_primitives();
}

Env* Evaluator::copy_env(const Env& e) {
    return arena_ ? arena_->create<Env>(e) : nullptr;
}

// eval_in(ast::Expr*) removed — all evaluation uses eval_flat(FlatAST&) now

// apply_closure removed — closure calls use eval_flat directly

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────
EvalResult Evaluator::eval_flat(aura::ast::FlatAST& flat,
                                 aura::ast::StringPool& pool,
                                 aura::ast::NodeId id,
                                 const Env& env) {
    if (id >= flat.size())
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "invalid node id"});
    auto v = flat.get(id);
    switch (v.tag) {
    case aura::ast::NodeTag::LiteralInt:
        return make_int(v.int_value);
    case aura::ast::NodeTag::LiteralString: {
        auto sid = string_heap_.size();
        string_heap_.push_back(std::string(pool.resolve(v.sym_id)));
        return make_string(sid);
    }
    case aura::ast::NodeTag::Variable: {
        auto name = pool.resolve(v.sym_id);
        auto val = env.lookup(std::string(name));
        if (val) return *val;
        return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                          "unbound variable: " + std::string(name)});
    }
    case aura::ast::NodeTag::Call: {
        if (v.children.empty()) return EvalResult(make_void());
        auto callee_id = v.child(0);
        auto callee = flat.get(callee_id);
        // Inline lambda
        if (callee.tag == aura::ast::NodeTag::Lambda) {
            auto pspan = callee.params;
            Env ne(&env);
            ne.set_primitives(&primitives_);
            for (std::size_t i = 0; i < pspan.size() && i+1 < v.children.size(); ++i) {
                auto ar = eval_flat(flat, pool, v.child(i+1), env);
                if (!ar) return ar;
                ne.bind(std::string(pool.resolve(pspan[i])), *ar);
            }
            auto body_id = callee.children.empty() ? aura::ast::NULL_NODE : callee.child(0);
            return eval_flat(flat, pool, body_id, ne);
        }
        // Macro expansion
        if (callee.tag == aura::ast::NodeTag::Variable) {
            auto cname = std::string(pool.resolve(callee.sym_id));
            auto macro_it = macros_.find(cname);
            if (macro_it != macros_.end()) {
                auto& md = macro_it->second;
                Env ne(&env);
                ne.set_primitives(&primitives_);
                for (std::size_t i = 0; i < md.params.size() && i+1 < v.children.size(); ++i) {
                    auto ar = eval_flat(flat, pool, v.child(i+1), env);
                    if (!ar) return ar;
                    ne.bind(md.params[i], *ar);
                }
                return eval_flat(*md.flat, pool, md.body_id, ne);
            }
        }
        // Primitive call
        if (callee.tag == aura::ast::NodeTag::Variable) {
            auto cname = std::string(pool.resolve(callee.sym_id));
            auto prim = env.lookup_primitive(cname);
            if (prim) {
                std::vector<EvalValue> args;
                for (std::size_t i = 1; i < v.children.size(); ++i) {
                    auto ar = eval_flat(flat, pool, v.child(i), env);
                    if (!ar) return ar;
                    args.push_back(*ar);
                }
                return (*prim)(args);
            }
        }
        // Closure call
        auto fn = eval_flat(flat, pool, callee_id, env);
        if (!fn) return fn;
        if (is_closure(*fn)) {
            auto cid = as_closure_id(*fn);
            auto it = closures_.find(cid);
            if (it == closures_.end())
                return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "eval_flat: invalid closure"});
            auto& cl = it->second;
            Env ne(cl.env ? cl.env : &top_);
            ne.set_primitives(&primitives_);
            for (std::size_t i = 0; i < cl.params.size() && i+1 < v.children.size(); ++i) {
                auto ar = eval_flat(flat, pool, v.child(i+1), env);
                if (!ar) return ar;
                ne.bind(cl.params[i], *ar);
            }
            return eval_flat(*cl.flat, pool, cl.body_id, ne);
        }
        return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                          "cannot call: " + std::string(pool.resolve(callee.sym_id))});
    }
    case aura::ast::NodeTag::IfExpr: {
        if (v.children.size() < 3) return EvalResult(make_void());
        auto c = eval_flat(flat, pool, v.child(0), env);
        if (!c) return c;
        return eval_flat(flat, pool, is_truthy(*c) ? v.child(1) : v.child(2), env);
    }
    case aura::ast::NodeTag::Lambda: {
        // Capture params from FlatAST directly
        auto pspan = v.params;
        std::vector<std::string> params;
        params.reserve(pspan.size());
        for (auto pid : pspan)
            params.push_back(std::string(pool.resolve(pid)));
        auto* cap = copy_env(env);
        auto cid = next_id();
        auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        closures_[cid] = Closure{std::move(params), &flat, &pool, body_id, cap};
        return make_closure(cid);
    }
    case aura::ast::NodeTag::Let:
    case aura::ast::NodeTag::LetRec: {
        bool rec = (v.tag == aura::ast::NodeTag::LetRec);
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
        if (rec) {
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            std::size_t ci = cells_.size();
            cells_.push_back(make_void());
            ne.bind(std::string(name), make_cell(ci));
            auto vv = eval_flat(flat, pool, val_id, ne);
            if (!vv) return vv;
            cells_[ci] = *vv;
            return eval_flat(flat, pool, body_id, ne);
        } else {
            auto vv = eval_flat(flat, pool, val_id, env);
            if (!vv) return vv;
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.bind(std::string(name), *vv);
            return eval_flat(flat, pool, body_id, ne);
        }
    }
    case aura::ast::NodeTag::Define: {
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        Env& me = const_cast<Env&>(env);
        me.set_cells(&cells_);
        auto ci = alloc_cell(make_void());
        me.bind(std::string(name), make_cell(ci));
        auto vv = eval_flat(flat, pool, val_id, env);
        if (!vv) return vv;
        cells_[ci] = *vv;
        return *vv;
    }
    case aura::ast::NodeTag::Begin: {
        EvalResult last = EvalResult(make_void());
        for (auto c : v.children) {
            auto r = eval_flat(flat, pool, c, env);
            if (!r) return r;
            last = *r;
        }
        return last;
    }
    case aura::ast::NodeTag::Set: {
        auto name = pool.resolve(v.sym_id);
        auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        auto val = eval_flat(flat, pool, val_id, env);
        if (!val) return val;
        auto* cell_ptr = env.lookup_cell_ptr(std::string(name), &cells_);
        if (cell_ptr) {
            *cell_ptr = *val;
            return *val;
        }
        for (auto& b : const_cast<Env&>(env).bindings()) {
            if (b.first == name) {
                b.second = *val;
                return *val;
            }
        }
        return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                          "set!: unbound variable: " + std::string(name)});
    }
    case aura::ast::NodeTag::Quote: {
        if (v.children.empty()) return EvalResult(make_void());
        return eval_flat(flat, pool, v.child(0), env);
    }
    case aura::ast::NodeTag::TypeAnnotation: {
        if (v.children.empty()) return EvalResult(make_void());
        return eval_flat(flat, pool, v.child(0), env);
    }
    case aura::ast::NodeTag::MacroDef: {
        auto name = pool.resolve(v.sym_id);
        std::vector<std::string> param_names;
        for (auto p : v.params)
            param_names.push_back(std::string(pool.resolve(p)));
        auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        if (body_id == aura::ast::NULL_NODE) return EvalResult(make_void());
        // Store FlatAST pointer + NodeId directly -- no Expr* reconstruction needed
        macros_[std::string(name)] = MacroDef{std::move(param_names), &flat, &pool, body_id};
        return EvalResult(make_void());
    }
    default:
        return std::unexpected(Diagnostic{ErrorKind::InternalError,
                                          "eval_flat: unsupported node type"});
    }
}

// ── Macro expander (hygienic Phase 2) ────────────────────────
// Clone a FlatAST subtree with MacroIntroduced markers.
// When a Variable matches a macro param, substitute with the arg expression.
// All new nodes are marked MacroIntroduced for hygiene tracking.
static aura::ast::NodeId clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
    aura::ast::FlatAST& source, aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst) {
    using namespace aura::ast;
    if (body_id == NULL_NODE || body_id >= source.size()) return NULL_NODE;
    auto v = source.get(body_id);

    // Variable substitution: if this variable is a macro param, return the arg clone
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = source_pool.resolve(v.sym_id);
        auto it = subst->find(std::string(name));
        if (it != subst->end()) {
            // Clone the argument expression from source FlatAST
            return clone_macro_body(target, target_pool, source, source_pool, it->second, subst);
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Clone children recursively
    std::vector<aura::ast::NodeId> child_ids;
    for (std::uint32_t i = 0; i < v.children.size(); ++i)
        child_ids.push_back(clone_macro_body(target, target_pool, source, source_pool, v.child(i), subst));

    // Clone params (for Lambda nodes)
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(transplant(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
    case NodeTag::LiteralInt:
        new_id = target.add_literal(v.int_value); break;
    case NodeTag::LiteralString:
        new_id = target.add_literalstring(transplant(v.sym_id)); break;
    case NodeTag::Variable:
        new_id = target.add_variable(transplant(v.sym_id)); break;
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
                ? target.add_let(transplant(v.sym_id), child_ids[0], child_ids[1])
                : target.add_letrec(transplant(v.sym_id), child_ids[0], child_ids[1]);
        break;
    case NodeTag::Begin:
        if (!child_ids.empty())
            new_id = target.add_begin(child_ids.data(), static_cast<std::uint32_t>(child_ids.size()));
        break;
    case NodeTag::Set:
        if (!child_ids.empty()) new_id = target.add_set(transplant(v.sym_id), child_ids[0]);
        break;
    case NodeTag::Quote:
        if (!child_ids.empty()) new_id = target.add_quote(child_ids[0]);
        break;
    case NodeTag::Define:
        if (!child_ids.empty()) new_id = target.add_define(transplant(v.sym_id), child_ids[0]);
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
        struct MD { aura::ast::FlatAST* src_flat; aura::ast::StringPool* src_pool; std::vector<std::string> params; NodeId body_id; };
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
                local_macros[macro_name] = MD{&flat, &pool, std::move(params), body_id};
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
                        for (std::size_t ai = 0; ai < md.params.size() && ai + 1 < v.children.size(); ++ai)
                            subst[md.params[ai]] = v.child(ai + 1);
                        // Clone macro body with substitution
                        auto expanded = clone_macro_body(flat, pool, *md.src_flat, *md.src_pool, md.body_id, &subst);
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
