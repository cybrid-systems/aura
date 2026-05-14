module;
#include <cstdio>
module aura.compiler.frontend;
import std;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.compiler.types;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

// Forward declarations for single-node FlatAST→Expr* reconstruction
// (used by eval_flat for Lambda/MacroDef body reconstruction)
namespace {
    // Helper: create Expr and set source location from FlatAST NodeView
    template <typename T, typename... Args>
    static aura::ast::Expr* make_expr_with_loc(aura::ast::ASTArena& arena,
                                                 const aura::ast::NodeView& v,
                                                 Args&&... args) {
        auto* e = arena.create<aura::ast::Expr>(T{v.tag, std::forward<Args>(args)...});
        e->loc = {v.line, v.col, 0};
        return e;
    }

    static aura::ast::Expr* reconst_node(aura::ast::NodeId id,
                                          const aura::ast::FlatAST& flat,
                                          aura::ast::StringPool& pool,
                                          aura::ast::ASTArena& arena) {
        if (id == aura::ast::NULL_NODE || id >= flat.size()) return nullptr;
        auto v = flat.get(id);
        using ast::NodeTag;
        switch (v.tag) {
        case NodeTag::LiteralString: {
            auto name = pool.resolve(v.sym_id);
            return make_expr_with_loc<ast::LiteralStringNode>(arena, v, std::string(name));
        }
        case NodeTag::LiteralInt:
            return make_expr_with_loc<ast::LiteralIntNode>(arena, v, v.int_value);
        case NodeTag::Variable: {
            auto name = pool.resolve(v.sym_id);
            return make_expr_with_loc<ast::VariableNode>(arena, v, std::string(name));
        }
        case NodeTag::Call: {
            auto* func = reconst_node(v.child(0), flat, pool, arena);
            std::vector<ast::Expr*> args;
            for (std::size_t i = 1; i < v.children.size(); ++i)
                args.push_back(reconst_node(v.child(i), flat, pool, arena));
            return make_expr_with_loc<ast::CallNode>(arena, v, func, std::move(args));
        }
        case NodeTag::IfExpr: {
            auto* c = reconst_node(v.child(0), flat, pool, arena);
            auto* t = reconst_node(v.child(1), flat, pool, arena);
            auto* e = reconst_node(v.child(2), flat, pool, arena);
            return make_expr_with_loc<ast::IfExprNode>(arena, v, c, t, e);
        }
        case NodeTag::Lambda: {
            std::vector<std::string> params;
            for (auto p : v.params)
                params.push_back(std::string(pool.resolve(p)));
            auto* body = reconst_node(v.child(0), flat, pool, arena);
            return make_expr_with_loc<ast::LambdaNode>(arena, v, std::move(params), body);
        }
        case NodeTag::Let: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            auto* body = reconst_node(v.child(1), flat, pool, arena);
            return make_expr_with_loc<ast::LetNode>(arena, v, std::string(name), val, body);
        }
        case NodeTag::LetRec: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            auto* body = reconst_node(v.child(1), flat, pool, arena);
            return make_expr_with_loc<ast::LetRecNode>(arena, v, std::string(name), val, body);
        }
        case NodeTag::Define: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            return make_expr_with_loc<ast::DefineNode>(arena, v, std::string(name), val);
        }
        case NodeTag::MacroDef: {
            auto name = pool.resolve(v.sym_id);
            auto* body = reconst_node(v.child(0), flat, pool, arena);
            std::vector<std::string> params;
            for (auto p : v.params)
                params.push_back(std::string(pool.resolve(p)));
            return make_expr_with_loc<ast::MacroDefNode>(arena, v, std::string(name), std::move(params), body);
        }
        case NodeTag::Begin: {
            ast::BeginNode begin{v.tag, {}};
            for (std::size_t i = 0; i < v.children.size(); ++i)
                begin.exprs.push_back(reconst_node(v.child(i), flat, pool, arena));
            auto* e_b = arena.create<ast::Expr>(std::move(begin)); e_b->loc = {v.line, v.col, 0}; return e_b;
        }
        case NodeTag::Set: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            return make_expr_with_loc<ast::SetNode>(arena, v, std::string(name), val);
        }
        case NodeTag::Quote: {
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            return make_expr_with_loc<ast::QuoteNode>(arena, v, val);
        }
        case NodeTag::TypeAnnotation: {
            auto type_name = pool.resolve(v.sym_id);
            auto* inner = reconst_node(v.child(0), flat, pool, arena);
            return make_expr_with_loc<ast::TypeAnnotationNode>(arena, v, inner, std::string(type_name));
        }
        case NodeTag::Coercion: {
            auto* inner = reconst_node(v.child(0), flat, pool, arena);
            return make_expr_with_loc<ast::CoercionNode>(arena, v, inner, "");
        }
        default:
            return nullptr;
        }
    }
} // anonymous namespace


// Phase 3b D3: forward declaration for macro template validation
struct MacroValidation;
static void validate_macro_template(MacroValidation& result,
                                     const std::string& name,
                                     const std::vector<std::string>& params,
                                     ast::Expr* body,
                                     aura::diag::DiagnosticCollector* diag);

using namespace aura::diag;

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
    table_["and"]  = [](auto& a) { return make_int(is_truthy(a[0]) && is_truthy(a[1]) ? 1 : 0); };
    table_["or"]   = [](auto& a) { return make_int(is_truthy(a[0]) || is_truthy(a[1]) ? 1 : 0); };
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
        return a.empty() ? make_void() : a[0];
    });
    primitives_.add("filter", [this](const auto& a) {
        return a.empty() ? make_void() : a[0];
    });

    // ── L6.8: Runtime type introspection ────────────────────────────
    auto infer_type_name = [](const EvalValue& v) -> const char* {
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

// ── eval_in: EvalValue-based tree-walker ──────────────────────
EvalResult Evaluator::eval_in(const ast::Expr* e, const Env& env) {
    if (!e) return std::unexpected(Diagnostic{ErrorKind::InternalError, "null expression"});
    return std::visit([&](const auto& n) -> EvalResult {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
            return make_int(n.value);
        if constexpr (std::is_same_v<T, ast::LiteralStringNode>) {
            auto id = string_heap_.size();
            string_heap_.push_back(n.value);
            return make_string(id);
        }
        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            auto v = env.lookup(n.name);
            if (v.has_value()) return *v;
            return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                              "unbound variable: " + n.name});
        }
        if constexpr (std::is_same_v<T, ast::CallNode>) {
            // Inline lambda application
            if (auto* lam = std::get_if<ast::LambdaNode>(&n.function->payload)) {
                Env ne(&env);
                ne.set_primitives(&primitives_);
                for (std::size_t i = 0; i < lam->params.size() && i < n.args.size(); ++i) {
                    auto a = eval_in(n.args[i], env);
                    if (!a) return a;
                    ne.bind(lam->params[i], *a);
                }
                return eval_in(lam->body, ne);
            }
            // Macro expansion
            if (auto* var = std::get_if<ast::VariableNode>(&n.function->payload)) {
                auto macro_it = macros_.find(var->name);
                if (macro_it != macros_.end()) {
                    if (macro_it->second.params.size() != n.args.size())
                        return std::unexpected(Diagnostic{ErrorKind::ArityMismatch,
                            "macro " + var->name + ": expected " +
                            std::to_string(macro_it->second.params.size()) +
                            " args, got " + std::to_string(n.args.size())});
                    auto* expanded = expand_macro(var->name, n.args);
                    return eval_in(expanded, env);
                }
            }
            // Primitive call
            if (auto* var = std::get_if<ast::VariableNode>(&n.function->payload)) {
                auto p = env.lookup_primitive(var->name);
                if (p.has_value()) {
                    std::vector<EvalValue> va;
                    for (auto* a : n.args) {
                        auto r = eval_in(a, env);
                        if (!r) return r;
                        va.push_back(*r);
                    }
                    return (*p)(va);
                }
            }
            // Closure call
            auto fr = eval_in(n.function, env);
            if (!fr) return fr;
            if (is_closure(*fr))
                return apply_closure(as_closure_id(*fr), n.args, env);
            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "not callable"});
        }
        if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            auto c = eval_in(n.condition, env);
            if (!c) return c;
            return eval_in(is_truthy(*c) ? n.then_branch : n.else_branch, env);
        }
        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            auto* cap = copy_env(env);
            auto id = next_id();
            closures_[id] = {n.params, n.body, cap};
            return make_closure(id);
        }
        if constexpr (std::is_same_v<T, ast::LetNode>) {
            auto v = eval_in(n.value, env);
            if (!v) return v;
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.bind(n.name, *v);
            return eval_in(n.body, ne);
        }
        if constexpr (std::is_same_v<T, ast::DefineNode>) {
            Env& me = const_cast<Env&>(env);
            me.set_cells(&cells_);
            auto ci = alloc_cell(make_void());
            me.bind(n.name, make_cell(ci));
            auto v = eval_in(n.value, env);
            if (!v) return v;
            cells_[ci] = *v;
            return v;
        }
        if constexpr (std::is_same_v<T, ast::LetRecNode>) {
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            auto ci = alloc_cell(make_void());
            ne.bind(n.name, make_cell(ci));
            auto v = eval_in(n.value, ne);
            if (!v) return v;
            cells_[ci] = *v;
            return eval_in(n.body, ne);
        }
        // Ghuloum Steps 11-13
        if constexpr (std::is_same_v<T, ast::BeginNode>) {
            ast::Expr* last = nullptr;
            for (auto* e : n.exprs) {
                auto v = eval_in(e, env);
                if (!v) return v;
                last = e;
            }
            return last ? eval_in(last, env) : EvalResult(make_void());
        }
        if constexpr (std::is_same_v<T, ast::SetNode>) {
            auto v = eval_in(n.value, env);
            if (!v) return v;
            auto* cell_ptr = env.lookup_cell_ptr(n.name, &cells_);
            if (cell_ptr) {
                *cell_ptr = *v;
                return *v;
            }
            auto& write_env = const_cast<Env&>(env);
            for (auto& b : write_env.bindings()) {
                if (b.first == n.name) {
                    b.second = *v;
                    return *v;
                }
            }
            for (auto* p = env.parent(); p; p = p->parent()) {
                auto& parent_write = const_cast<Env&>(*p);
                for (auto& b : parent_write.bindings()) {
                    if (b.first == n.name) {
                        b.second = *v;
                        return *v;
                    }
                }
            }
            return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                              "set!: unbound variable: " + n.name});
        }
        if constexpr (std::is_same_v<T, ast::QuoteNode>) {
            return eval_in(n.value, env);
        }
        if constexpr (std::is_same_v<T, ast::TypeAnnotationNode>) {
            return eval_in(n.inner_expr, env);
        }
        // ── Helper: runtime type coercion for EvalValue ──────────
        auto coerce_value = [&](const EvalValue& val, const std::string& to_type) -> EvalValue {
            if (to_type == "Any" || to_type.empty()) return val;

            // Coerce to Int
            if (to_type == "Int") {
                if (is_int(val)) return val;
                if (is_string(val)) {
                    auto idx = as_string_idx(val);
                    if (idx < string_heap_.size()) {
                        try { return make_int(static_cast<std::int64_t>(std::stoll(string_heap_[idx]))); }
                        catch (...) { return make_int(0); }
                    }
                    return make_int(0);
                }
                if (is_bool(val)) return make_int(as_bool(val) ? 1 : 0);
                return make_int(0);
            }

            // Coerce to String
            if (to_type == "String") {
                if (is_string(val)) return val;
                if (is_int(val)) {
                    auto s = std::to_string(as_int(val));
                    auto id = string_heap_.size();
                    string_heap_.push_back(std::move(s));
                    return make_string(id);
                }
                if (is_bool(val)) {
                    auto s = as_bool(val) ? "#t" : "#f";
                    auto id = string_heap_.size();
                    string_heap_.push_back(std::move(s));
                    return make_string(id);
                }
                return val;
            }

            // Coerce to Bool
            if (to_type == "Bool") {
                return make_bool(is_truthy(val));
            }

            return val;
        };

        if constexpr (std::is_same_v<T, ast::CoercionNode>) {
            auto result = eval_in(n.inner_expr, env);
            if (result) {
                return coerce_value(*result, n.to_type_name);
            }
            return std::unexpected(result.error());
        }
        if constexpr (std::is_same_v<T, ast::MacroDefNode>) {
            static ast::ASTArena persistent_arena(64 * 1024);
            struct Cloner {
                ast::ASTArena* pa;
                ast::Expr* clone(ast::Expr* e) {
                    return std::visit([this](const auto& node) -> ast::Expr* {
                        using T = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
                            return pa->create<ast::Expr>(node);
                        if constexpr (std::is_same_v<T, ast::VariableNode>)
                            return pa->create<ast::Expr>(node);
                        if constexpr (std::is_same_v<T, ast::CallNode>) {
                            ast::CallNode c{node.tag, clone(node.function), {}};
                            for (auto* a : node.args) c.args.push_back(clone(a));
                            return pa->create<ast::Expr>(std::move(c));
                        }
                        if constexpr (std::is_same_v<T, ast::IfExprNode>)
                            return pa->create<ast::Expr>(ast::IfExprNode{node.tag, clone(node.condition), clone(node.then_branch), clone(node.else_branch)});
                        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
                            ast::LambdaNode l{node.tag, node.params, clone(node.body)};
                            return pa->create<ast::Expr>(std::move(l));
                        }
                        if constexpr (std::is_same_v<T, ast::LetNode>)
                            return pa->create<ast::Expr>(ast::LetNode{node.tag, node.name, clone(node.value), clone(node.body)});
                        if constexpr (std::is_same_v<T, ast::LetRecNode>)
                            return pa->create<ast::Expr>(ast::LetRecNode{node.tag, node.name, clone(node.value), clone(node.body)});
                        if constexpr (std::is_same_v<T, ast::DefineNode>)
                            return pa->create<ast::Expr>(ast::DefineNode{node.tag, node.name, clone(node.value)});
                        if constexpr (std::is_same_v<T, ast::BeginNode>) {
                            ast::BeginNode b{node.tag, {}};
                            for (auto* e : node.exprs) b.exprs.push_back(clone(e));
                            return pa->create<ast::Expr>(std::move(b));
                        }
                        if constexpr (std::is_same_v<T, ast::SetNode>)
                            return pa->create<ast::Expr>(ast::SetNode{node.tag, node.name, clone(node.value)});
                        if constexpr (std::is_same_v<T, ast::QuoteNode>)
                            return pa->create<ast::Expr>(ast::QuoteNode{node.tag, clone(node.value)});
                        if constexpr (std::is_same_v<T, ast::TypeAnnotationNode>)
                            return pa->create<ast::Expr>(ast::TypeAnnotationNode{node.tag, clone(node.inner_expr), node.type_name});
                        if constexpr (std::is_same_v<T, ast::CoercionNode>)
                            return pa->create<ast::Expr>(ast::CoercionNode{node.tag, clone(node.inner_expr), node.to_type_name});
                        return nullptr;
                    }, e->payload);
                }
            };
            Cloner cloner{&persistent_arena};
            auto* persistent_body = cloner.clone(n.body);

            aura::diag::DiagnosticCollector macro_diag;
            aura::compiler::MacroValidation mv;
            aura::compiler::validate_macro_template(mv, n.name, n.params, persistent_body, &macro_diag);
            (void)mv;

            const_cast<Evaluator*>(this)->macros_[n.name] = MacroDef{n.params, persistent_body};
            return EvalResult(make_void());
        }
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "unknown expression type"});
    }, e->payload);
}

// ── apply_closure: EvalValue-based ────────────────────────────
EvalResult Evaluator::apply_closure(ClosureId id, const std::vector<ast::Expr*>& args, const Env& call_env) {
    auto it = closures_.find(id);
    if (it == closures_.end())
        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "invalid closure"});
    auto& cl = it->second;
    Env ne(cl.env ? cl.env : &top_);
    ne.set_primitives(&primitives_);
    for (std::size_t i = 0; i < cl.params.size() && i < args.size(); ++i) {
        auto v = eval_in(args[i], call_env);
        if (!v) return v;
        ne.bind(cl.params[i], *v);
    }
    return eval_in(cl.body, ne);
}

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
            auto* lambda_expr = reconst_node(callee_id, flat, pool, *arena_);
            if (!lambda_expr || lambda_expr->tag != aura::ast::NodeTag::Lambda)
                return std::unexpected(Diagnostic{ErrorKind::InternalError, "eval_flat: expected lambda"});
            auto& lam = std::get<aura::ast::LambdaNode>(lambda_expr->payload);
            Env ne(&env);
            ne.set_primitives(&primitives_);
            for (std::size_t i = 0; i < lam.params.size() && i+1 < v.children.size(); ++i) {
                auto ar = eval_flat(flat, pool, v.child(i+1), env);
                if (!ar) return ar;
                ne.bind(lam.params[i], *ar);
            }
            return eval_in(lam.body, ne);
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
                return eval_in(md.body, ne);
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
            return eval_in(cl.body, ne);
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
        auto* expr = reconst_node(id, flat, pool, *arena_);
        if (!expr || expr->tag != aura::ast::NodeTag::Lambda)
            return std::unexpected(Diagnostic{ErrorKind::InternalError, "eval_flat: lambda reconstruct failed"});
        auto& lam = std::get<aura::ast::LambdaNode>(expr->payload);
        auto* cap = copy_env(env);
        auto cid = next_id();
        closures_[cid] = Closure{lam.params, lam.body, cap};
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
        auto* body_expr = reconst_node(body_id, flat, pool, *arena_);
        if (!body_expr) return EvalResult(make_void());
        static aura::ast::ASTArena persistent_arena(64 * 1024);
        struct Cloner {
            aura::ast::ASTArena* pa;
            aura::ast::Expr* clone(aura::ast::Expr* e) {
                return std::visit([this](const auto& node) -> aura::ast::Expr* {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, aura::ast::LiteralIntNode>)
                        return pa->create<aura::ast::Expr>(node);
                    if constexpr (std::is_same_v<T, aura::ast::VariableNode>)
                        return pa->create<aura::ast::Expr>(node);
                    if constexpr (std::is_same_v<T, aura::ast::CallNode>) {
                        aura::ast::CallNode c{node.tag, clone(node.function), {}};
                        for (auto* a : node.args) c.args.push_back(clone(a));
                        return pa->create<aura::ast::Expr>(std::move(c));
                    }
                    if constexpr (std::is_same_v<T, aura::ast::IfExprNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::IfExprNode{node.tag, clone(node.condition), clone(node.then_branch), clone(node.else_branch)});
                    if constexpr (std::is_same_v<T, aura::ast::LambdaNode>) {
                        aura::ast::LambdaNode l{node.tag, node.params, clone(node.body)};
                        return pa->create<aura::ast::Expr>(std::move(l));
                    }
                    if constexpr (std::is_same_v<T, aura::ast::LetNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::LetNode{node.tag, node.name, clone(node.value), clone(node.body)});
                    if constexpr (std::is_same_v<T, aura::ast::LetRecNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::LetRecNode{node.tag, node.name, clone(node.value), clone(node.body)});
                    if constexpr (std::is_same_v<T, aura::ast::DefineNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::DefineNode{node.tag, node.name, clone(node.value)});
                    if constexpr (std::is_same_v<T, aura::ast::BeginNode>) {
                        aura::ast::BeginNode b{node.tag, {}};
                        for (auto* e : node.exprs) b.exprs.push_back(clone(e));
                        return pa->create<aura::ast::Expr>(std::move(b));
                    }
                    if constexpr (std::is_same_v<T, aura::ast::SetNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::SetNode{node.tag, node.name, clone(node.value)});
                    if constexpr (std::is_same_v<T, aura::ast::QuoteNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::QuoteNode{node.tag, clone(node.value)});
                    if constexpr (std::is_same_v<T, aura::ast::TypeAnnotationNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::TypeAnnotationNode{node.tag, clone(node.inner_expr), node.type_name});
                    if constexpr (std::is_same_v<T, aura::ast::CoercionNode>)
                        return pa->create<aura::ast::Expr>(aura::ast::CoercionNode{node.tag, clone(node.inner_expr), node.to_type_name});
                    return nullptr;
                }, e->payload);
            }
        };
        Cloner cloner{&persistent_arena};
        auto* persistent_body = cloner.clone(body_expr);
        if (body_expr) {
            bool has_param_ref = false;
            for (auto& p : param_names) {
                if (body_expr->tag == aura::ast::NodeTag::LiteralInt ||
                    body_expr->tag == aura::ast::NodeTag::LiteralString) {
                    has_param_ref = false;
                } else {
                    has_param_ref = true;
                }
            }
            if (!has_param_ref && param_names.empty() && body_expr->tag == aura::ast::NodeTag::LiteralInt) {
                std::println(std::cerr, "warning: macro '{}': body does not reference any parameters "
                             "— always expands to the same expression", std::string(name));
            }
            for (auto& p : param_names) {
                bool found = false;
                if (body_expr->tag == aura::ast::NodeTag::Variable) {
                    auto& var_node = std::get<aura::ast::VariableNode>(body_expr->payload);
                    if (var_node.name == p) found = true;
                }
                if (body_expr->tag == aura::ast::NodeTag::Call) {
                    auto& call_node = std::get<aura::ast::CallNode>(body_expr->payload);
                }
                std::println(std::cerr, "warning: macro '{}': parameter '{}' is never used in template",
                             std::string(name), p);
            }
        }
        macros_[std::string(name)] = MacroDef{std::move(param_names), persistent_body};
        return EvalResult(make_void());
    }
    default:
        return std::unexpected(Diagnostic{ErrorKind::InternalError,
                                          "eval_flat: unsupported node type"});
    }
}

// ── Macro template validation (Phase 3b D3) ──────────────────
struct MacroValidation {
    bool body_valid = true;
    std::vector<std::string> unused_params;
    std::vector<std::string> free_vars;
    bool constant_body = false;
};

static void collect_var_refs(const ast::Expr* e, std::unordered_set<std::string>& vars) {
    if (!e) return;
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VariableNode>) {
            vars.insert(node.name);
        } else if constexpr (std::is_same_v<T, ast::CallNode>) {
            collect_var_refs(node.function, vars);
            for (auto* a : node.args) collect_var_refs(a, vars);
        } else if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            collect_var_refs(node.condition, vars);
            collect_var_refs(node.then_branch, vars);
            collect_var_refs(node.else_branch, vars);
        } else if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            collect_var_refs(node.body, vars);
            for (auto& p : node.params) vars.erase(p);
        } else if constexpr (std::is_same_v<T, ast::LetNode>) {
            collect_var_refs(node.value, vars);
            collect_var_refs(node.body, vars);
            vars.erase(node.name);
        } else if constexpr (std::is_same_v<T, ast::LetRecNode>) {
            collect_var_refs(node.value, vars);
            collect_var_refs(node.body, vars);
            vars.erase(node.name);
        } else if constexpr (std::is_same_v<T, ast::DefineNode>) {
            collect_var_refs(node.value, vars);
            vars.erase(node.name);
        } else if constexpr (std::is_same_v<T, ast::BeginNode>) {
            for (auto* x : node.exprs) collect_var_refs(x, vars);
        } else if constexpr (std::is_same_v<T, ast::SetNode>) {
            collect_var_refs(node.value, vars);
        } else if constexpr (std::is_same_v<T, ast::QuoteNode>) {
        } else if constexpr (std::is_same_v<T, ast::TypeAnnotationNode>) {
            collect_var_refs(node.inner_expr, vars);
        } else if constexpr (std::is_same_v<T, ast::CoercionNode>) {
            collect_var_refs(node.inner_expr, vars);
        }
    }, e->payload);
}

static void validate_macro_template(MacroValidation& mv,
                                     const std::string& name,
                                     const std::vector<std::string>& params,
                                     ast::Expr* body,
                                     aura::diag::DiagnosticCollector* diag) {
    mv = MacroValidation{};
    mv.body_valid = (body != nullptr);

    if (!body) {
        if (diag) diag->report(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ParseError,
            std::format("macro '{}': body is null", name)});
        return;
    }

    std::unordered_set<std::string> body_vars;
    collect_var_refs(body, body_vars);

    for (auto& p : params) {
        if (body_vars.find(p) == body_vars.end()) {
            mv.unused_params.push_back(p);
            if (diag) {
                std::println(std::cerr, "warning: macro '{}': parameter '{}' is never used in template",
                             name, p);
            }
        }
    }

    std::unordered_set<std::string> param_set(params.begin(), params.end());
    for (auto& v : body_vars) {
        if (param_set.find(v) == param_set.end()) {
            mv.free_vars.push_back(v);
        }
    }

    bool has_param_ref = false;
    for (auto& v : body_vars) {
        if (param_set.find(v) != param_set.end()) {
            has_param_ref = true;
            break;
        }
    }
    mv.constant_body = !has_param_ref;
    if (mv.constant_body && diag) {
        std::println(std::cerr, "warning: macro '{}': body does not reference any parameters "
                     "— always expands to the same expression", name);
    }
}

// ── Macro expansion ───────────────────────────────────────────
ast::Expr* Evaluator::expand_macro(const std::string& name,
                                    const std::vector<ast::Expr*>& args) {
    auto it = macros_.find(name);
    if (it == macros_.end()) return nullptr;
    auto& mac = it->second;

    struct Expander {
        ast::ASTArena* arena;
        const std::vector<std::string>* params;
        const std::vector<ast::Expr*>* args;
        std::unordered_map<std::string, std::string> rename_;
        std::uint64_t gs_counter_ = 0;

        std::string fresh_name(const std::string& base) {
            auto id = gs_counter_++;
            return "__gs_" + base + "_" + std::to_string(id);
        }

        ast::Expr* clone(ast::Expr* expr) {
            return std::visit([this](const auto& node) -> ast::Expr* {
                using T = std::decay_t<decltype(node)>;

                if constexpr (std::is_same_v<T, ast::VariableNode>) {
                    for (std::size_t i = 0; i < params->size(); ++i) {
                        if (node.name == (*params)[i]) {
                            return clone((*args)[i]);
                        }
                    }
                    auto it = rename_.find(node.name);
                    if (it != rename_.end())
                        return arena->template create<ast::Expr>(ast::VariableNode{node.tag, it->second});
                    return arena->template create<ast::Expr>(ast::VariableNode{node.tag, node.name});
                }

                if constexpr (std::is_same_v<T, ast::LiteralIntNode>) {
                    return arena->template create<ast::Expr>(ast::LiteralIntNode{node.tag, node.value});
                }

                if constexpr (std::is_same_v<T, ast::LiteralStringNode>) {
                    return arena->template create<ast::Expr>(ast::LiteralStringNode{node.tag, node.value});
                }

                if constexpr (std::is_same_v<T, ast::CallNode>) {
                    ast::CallNode call{node.tag, clone(node.function), {}};
                    for (auto* a : node.args)
                        call.args.push_back(clone(a));
                    return arena->template create<ast::Expr>(std::move(call));
                }

                if constexpr (std::is_same_v<T, ast::IfExprNode>) {
                    return arena->template create<ast::Expr>(
                        ast::IfExprNode{node.tag, clone(node.condition), clone(node.then_branch), clone(node.else_branch)});
                }

                if constexpr (std::is_same_v<T, ast::LambdaNode>) {
                    std::vector<std::string> new_params;
                    for (auto& p : node.params) {
                        auto fresh = fresh_name(p);
                        rename_[p] = fresh;
                        new_params.push_back(std::move(fresh));
                    }
                    ast::LambdaNode lam{node.tag, std::move(new_params), clone(node.body)};
                    for (auto& p : node.params) rename_.erase(p);
                    return arena->template create<ast::Expr>(std::move(lam));
                }

                if constexpr (std::is_same_v<T, ast::LetNode>) {
                    auto fresh = fresh_name(node.name);
                    rename_[node.name] = fresh;
                    auto* cloned = arena->template create<ast::Expr>(
                        ast::LetNode{node.tag, fresh, clone(node.value), clone(node.body)});
                    rename_.erase(node.name);
                    return cloned;
                }

                if constexpr (std::is_same_v<T, ast::LetRecNode>) {
                    auto fresh = fresh_name(node.name);
                    rename_[node.name] = fresh;
                    auto* cloned = arena->template create<ast::Expr>(
                        ast::LetRecNode{node.tag, fresh, clone(node.value), clone(node.body)});
                    rename_.erase(node.name);
                    return cloned;
                }

                if constexpr (std::is_same_v<T, ast::DefineNode>) {
                    return arena->template create<ast::Expr>(
                        ast::DefineNode{node.tag, node.name, clone(node.value)});
                }

                if constexpr (std::is_same_v<T, ast::BeginNode>) {
                    ast::BeginNode begin{node.tag, {}};
                    for (auto* e : node.exprs)
                        begin.exprs.push_back(clone(e));
                    return arena->template create<ast::Expr>(std::move(begin));
                }

                if constexpr (std::is_same_v<T, ast::SetNode>) {
                    return arena->template create<ast::Expr>(
                        ast::SetNode{node.tag, node.name, clone(node.value)});
                }

                if constexpr (std::is_same_v<T, ast::QuoteNode>) {
                    return arena->template create<ast::Expr>(
                        ast::QuoteNode{node.tag, clone(node.value)});
                }

                if constexpr (std::is_same_v<T, ast::TypeAnnotationNode>) {
                    return arena->template create<ast::Expr>(
                        ast::TypeAnnotationNode{node.tag, clone(node.inner_expr), node.type_name});
                }

                if constexpr (std::is_same_v<T, ast::CoercionNode>) {
                    return arena->template create<ast::Expr>(
                        ast::CoercionNode{node.tag, clone(node.inner_expr), node.to_type_name});
                }

                return nullptr;
            }, expr->payload);
        }
    };

    Expander expander{arena_, &mac.params, &args, {}, 0};
    return expander.clone(mac.body);
}

} // namespace aura::compiler
