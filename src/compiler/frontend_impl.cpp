module;
#include <cstdio>
module aura.compiler.frontend;
import std;
import aura.core.ast_flat;
import aura.core.ast_pool;

namespace aura::compiler {

// Forward declarations for single-node FlatAST→Expr* reconstruction
// (used by eval_flat for Lambda/MacroDef body reconstruction)
namespace {
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
            return arena.create<ast::Expr>(ast::LiteralStringNode{v.tag, std::string(name)});
        }
        case NodeTag::LiteralInt:
            return arena.create<ast::Expr>(ast::LiteralIntNode{v.tag, v.int_value});
        case NodeTag::Variable: {
            auto name = pool.resolve(v.sym_id);
            return arena.create<ast::Expr>(ast::VariableNode{v.tag, std::string(name)});
        }
        case NodeTag::Call: {
            auto* func = reconst_node(v.child(0), flat, pool, arena);
            std::vector<ast::Expr*> args;
            for (std::size_t i = 1; i < v.children.size(); ++i)
                args.push_back(reconst_node(v.child(i), flat, pool, arena));
            return arena.create<ast::Expr>(ast::CallNode{v.tag, func, std::move(args)});
        }
        case NodeTag::IfExpr: {
            auto* c = reconst_node(v.child(0), flat, pool, arena);
            auto* t = reconst_node(v.child(1), flat, pool, arena);
            auto* e = reconst_node(v.child(2), flat, pool, arena);
            return arena.create<ast::Expr>(ast::IfExprNode{v.tag, c, t, e});
        }
        case NodeTag::Lambda: {
            std::vector<std::string> params;
            for (auto p : v.params)
                params.push_back(std::string(pool.resolve(p)));
            auto* body = reconst_node(v.child(0), flat, pool, arena);
            return arena.create<ast::Expr>(ast::LambdaNode{v.tag, std::move(params), body});
        }
        case NodeTag::Let: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            auto* body = reconst_node(v.child(1), flat, pool, arena);
            return arena.create<ast::Expr>(ast::LetNode{v.tag, std::string(name), val, body});
        }
        case NodeTag::LetRec: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            auto* body = reconst_node(v.child(1), flat, pool, arena);
            return arena.create<ast::Expr>(ast::LetRecNode{v.tag, std::string(name), val, body});
        }
        case NodeTag::Define: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            return arena.create<ast::Expr>(ast::DefineNode{v.tag, std::string(name), val});
        }
        case NodeTag::MacroDef: {
            auto name = pool.resolve(v.sym_id);
            auto* body = reconst_node(v.child(0), flat, pool, arena);
            std::vector<std::string> params;
            for (auto p : v.params)
                params.push_back(std::string(pool.resolve(p)));
            return arena.create<ast::Expr>(ast::MacroDefNode{v.tag, std::string(name), std::move(params), body});
        }
        case NodeTag::Begin: {
            ast::BeginNode begin{v.tag, {}};
            for (std::size_t i = 0; i < v.children.size(); ++i)
                begin.exprs.push_back(reconst_node(v.child(i), flat, pool, arena));
            return arena.create<ast::Expr>(std::move(begin));
        }
        case NodeTag::Set: {
            auto name = pool.resolve(v.sym_id);
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            return arena.create<ast::Expr>(ast::SetNode{v.tag, std::string(name), val});
        }
        case NodeTag::Quote: {
            auto* val = reconst_node(v.child(0), flat, pool, arena);
            return arena.create<ast::Expr>(ast::QuoteNode{v.tag, val});
        }
        case NodeTag::TypeAnnotation: {
            auto type_name = pool.resolve(v.sym_id);
            auto* inner = reconst_node(v.child(0), flat, pool, arena);
            return arena.create<ast::Expr>(ast::TypeAnnotationNode{v.tag, inner, std::string(type_name)});
        }
        case NodeTag::Coercion: {
            auto* inner = reconst_node(v.child(0), flat, pool, arena);
            return arena.create<ast::Expr>(ast::CoercionNode{v.tag, inner, ""});
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

std::optional<std::int64_t> Env::lookup(const std::string& n) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            auto v = it->second;
            if (cells_ && static_cast<std::uint64_t>(v) >= CELL_SENTINEL) {
                auto idx = static_cast<std::size_t>(v - CELL_SENTINEL);
                if (idx < cells_->size()) return (*cells_)[idx];
            }
            return v;
        }
    return parent_ ? parent_->lookup(n) : std::nullopt;
}

Primitives::Primitives() {
    table_["+"]  = [this](auto& a) { return str_to_int(a[0]) + str_to_int(a[1]); };
    table_["-"]  = [this](auto& a) { return a.size() == 1 ? -str_to_int(a[0]) : str_to_int(a[0]) - str_to_int(a[1]); };
    table_["*"]  = [this](auto& a) { return str_to_int(a[0]) * str_to_int(a[1]); };
    table_["/"]  = [this](auto& a) { return str_to_int(a[0]) / str_to_int(a[1]); };
    table_["="]  = [this](auto& a) { return str_to_int(a[0]) == str_to_int(a[1]); };
    table_["<"]  = [this](auto& a) { return str_to_int(a[0]) < str_to_int(a[1]); };
    table_[">"]  = [this](auto& a) { return str_to_int(a[0]) > str_to_int(a[1]); };
    table_["<="] = [this](auto& a) { return str_to_int(a[0]) <= str_to_int(a[1]); };
    table_[">="] = [this](auto& a) { return str_to_int(a[0]) >= str_to_int(a[1]); };
    // Ghuloum Step 9: booleans
    table_["not"]  = [](auto& a) { return a[0] == 0 ? TRUE_VAL : FALSE_VAL; };
    table_["and"]  = [](auto& a) { return a[0] && a[1] ? TRUE_VAL : FALSE_VAL; };
    table_["or"]   = [](auto& a) { return a[0] || a[1] ? TRUE_VAL : FALSE_VAL; };
    table_["eq?"]  = [](auto& a) { return a[0] == a[1] ? TRUE_VAL : FALSE_VAL; };
}

std::int64_t Primitives::str_to_int(std::int64_t v) const {
    auto uv = static_cast<std::uint64_t>(v);
    if (uv < 0x1000000) return v;  // already int
    if (uv >= 0x8000000 && string_heap_ && uv - 0x8000000 < string_heap_->size()) {
        // String → Int: parse
        auto idx = static_cast<std::size_t>(uv - 0x8000000);
        try { return static_cast<std::int64_t>(std::stoll((*string_heap_)[idx])); }
        catch (...) { return 0; }
    }
    return v;
}

// ── I/O helper (non-template, avoids generic lambda lookup issues) ──
namespace {
    static void io_print_val(std::int64_t v, const std::vector<std::string>* heap, bool quote) {
        auto uv = static_cast<std::uint64_t>(v);
        if (v == 0)       { std::printf("()"); return; }
        if (v == 1)       { std::printf("#t"); return; }
        if (uv >= static_cast<std::uint64_t>(0x8000000) && heap && uv - static_cast<std::uint64_t>(0x8000000) < heap->size()) {
            auto idx = static_cast<std::size_t>(v - static_cast<std::int64_t>(0x8000000));
            if (quote) std::printf("\"%s\"", (*heap)[idx].c_str());
            else       std::printf("%s",       (*heap)[idx].c_str());
            return;
        }
        std::printf("%ld", (long)v);
    }
}
void Evaluator::init_pair_primitives() {
    // Ghuloum Step 10: pairs — capture pairs_ by reference
    primitives_.add("cons", [this](const auto& a) {
        auto id = pairs_.size();
        pairs_.push_back({a[0], a[1]});
        return PAIR_SENTINEL + static_cast<std::int64_t>(id);
    });
    primitives_.add("car", [this](const auto& a) {
        auto id = static_cast<std::size_t>(a[0] - PAIR_SENTINEL);
        return id < pairs_.size() ? pairs_[id].car : 0;
    });
    primitives_.add("cdr", [this](const auto& a) {
        auto id = static_cast<std::size_t>(a[0] - PAIR_SENTINEL);
        return id < pairs_.size() ? pairs_[id].cdr : 0;
    });
    primitives_.add("pair?", [](const auto& a) {
        return static_cast<std::uint64_t>(a[0]) >= static_cast<std::uint64_t>(PAIR_SENTINEL) ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("string?", [this](const auto& a) {
        if (a.empty()) return FALSE_VAL;
        return static_cast<std::uint64_t>(a[0]) >= static_cast<std::uint64_t>(STRING_SENTINEL) ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("string-append", [this](const auto& a) {
        std::string result;
        for (auto v : a) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
                auto idx = static_cast<std::size_t>(v - STRING_SENTINEL);
                if (idx < string_heap_.size()) result += string_heap_[idx];
            } else if (uv < 0x1000000) {
                // Int → String coercion
                result += std::to_string(v);
            }
        }
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return STRING_SENTINEL + static_cast<std::int64_t>(id);
    });
    primitives_.add("string-length", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        auto uv = static_cast<std::uint64_t>(a[0]);
        if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
            auto idx = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
            return static_cast<std::int64_t>(idx < string_heap_.size() ? string_heap_[idx].size() : 0);
        } else if (uv < 0x1000000) {
            // Int → String coercion
            return static_cast<std::int64_t>(std::to_string(a[0]).size());
        }
        return std::int64_t(0);
    });
    primitives_.add("string-ref", [this](const auto& a) {
        if (a.size() < 2) return std::int64_t(0);
        // Coerce first arg: Int → String
        auto uv0 = static_cast<std::uint64_t>(a[0]);
        std::string s;
        if (uv0 >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
            auto idx = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
            if (idx < string_heap_.size()) s = string_heap_[idx];
        } else if (uv0 < 0x1000000) {
            s = std::to_string(a[0]);
        }
        auto pos = static_cast<std::size_t>(a[1]);
        if (pos < s.size())
            return static_cast<std::int64_t>(static_cast<unsigned char>(s[pos]));
        return std::int64_t(0);
    });
    primitives_.add("substring", [this](const auto& a) {
        if (a.size() < 3) return std::int64_t(0);
        // Coerce first arg: Int → String
        auto uv0 = static_cast<std::uint64_t>(a[0]);
        std::string s_buf;
        const std::string* sp = nullptr;
        if (uv0 >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
            auto idx = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
            if (idx < string_heap_.size()) sp = &string_heap_[idx];
        } else if (uv0 < 0x1000000) {
            s_buf = std::to_string(a[0]);
            sp = &s_buf;
        }
        if (!sp) return std::int64_t(0);
        const auto& s = *sp;
        auto start = static_cast<std::size_t>(a[1]);
        auto end   = static_cast<std::size_t>(a[2]);
        if (start > s.size()) start = s.size();
        if (end > s.size()) end = s.size();
        if (start >= end) {
            auto id = string_heap_.size();
            string_heap_.push_back("");
            return STRING_SENTINEL + static_cast<std::int64_t>(id);
        }
        auto sub = s.substr(start, end - start);
        auto nid = string_heap_.size();
        string_heap_.push_back(std::move(sub));
        return STRING_SENTINEL + static_cast<std::int64_t>(nid);
    });
    primitives_.add("string=?", [this](const auto& a) {
        if (a.size() < 2) return FALSE_VAL;
        auto to_str = [this](std::int64_t v) -> std::string {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
                auto idx = static_cast<std::size_t>(v - STRING_SENTINEL);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (uv < 0x1000000) return std::to_string(v);
            return "";
        };
        return to_str(a[0]) == to_str(a[1]) ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("string<?", [this](const auto& a) {
        if (a.size() < 2) return FALSE_VAL;
        auto to_str = [this](std::int64_t v) -> std::string {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
                auto idx = static_cast<std::size_t>(v - STRING_SENTINEL);
                return (idx < string_heap_.size()) ? string_heap_[idx] : "";
            }
            if (uv < 0x1000000) return std::to_string(v);
            return "";
        };
        return to_str(a[0]) < to_str(a[1]) ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("number->string", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        auto s = std::to_string(a[0]);
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(s));
        return STRING_SENTINEL + static_cast<std::int64_t>(id);
    });
    primitives_.add("string->number", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        auto i = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
        if (i >= string_heap_.size()) return std::int64_t(0);
        try { return static_cast<std::int64_t>(std::stoll(string_heap_[i])); }
        catch (...) { return std::int64_t(0); }
    });
    primitives_.add("list", [this](const auto& a) {
        // Build proper list (pair chain ending with 0)
        std::int64_t result = 0;
        for (auto it = a.rbegin(); it != a.rend(); ++it) {
            auto id = pairs_.size();
            pairs_.push_back({*it, result});
            result = PAIR_SENTINEL + static_cast<std::int64_t>(id);
        }
        return result;
    });
    primitives_.add("list?", [this](const auto& a) {
        if (a.empty()) return TRUE_VAL;
        auto v = a[0];
        while (v != 0) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL))
                return FALSE_VAL;
            auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
            if (idx >= pairs_.size()) return FALSE_VAL;
            v = pairs_[idx].cdr;  // follow cdr chain
        }
        return TRUE_VAL;  // reached null → proper list
    });
    primitives_.add("null?", [](const auto& a) {
        return a[0] == 0 ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("length", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        auto v = a[0]; std::int64_t n = 0;
        while (v != 0) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL)) return std::int64_t(0);
            auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
            if (idx >= pairs_.size()) return std::int64_t(0);
            v = pairs_[idx].cdr; n++;
        }
        return n;
    });
    primitives_.add("list-ref", [this](const auto& a) {
        if (a.size() < 2) return std::int64_t(0);
        auto v = a[0]; auto pos = static_cast<std::size_t>(a[1]);
        for (std::size_t i = 0; i < pos; ++i) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL)) return std::int64_t(0);
            auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
            if (idx >= pairs_.size()) return std::int64_t(0);
            v = pairs_[idx].cdr;
        }
        auto uv = static_cast<std::uint64_t>(v);
        if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL)) return v;
        auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
        return idx < pairs_.size() ? pairs_[idx].car : std::int64_t(0);
    });
    primitives_.add("member", [this](const auto& a) {
        if (a.size() < 2) return std::int64_t(0);
        auto val = a[0]; auto v = a[1];
        while (v != 0) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL)) return std::int64_t(0);
            auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
            if (idx >= pairs_.size()) return std::int64_t(0);
            if (pairs_[idx].car == val) return v;
            v = pairs_[idx].cdr;
        }
        return std::int64_t(0);
    });
    primitives_.add("append", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        // For now: two-arg append
        if (a.size() < 2) return a[0];
        // Copy first list, last cdr points to second list
        auto list1 = a[0]; auto list2 = a[1];
        if (list1 == 0) return list2;
        // Walk to end of list1, copying each pair
        std::int64_t result = 0; std::int64_t tail = 0;
        auto v = list1;
        while (v != 0) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL)) return list1;
            auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
            if (idx >= pairs_.size()) return list1;
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, 0});  // cdr = 0 for now
            auto new_pair = PAIR_SENTINEL + static_cast<std::int64_t>(new_id);
            if (result == 0) result = new_pair;
            else { auto tidx = static_cast<std::size_t>(tail - PAIR_SENTINEL);
                   pairs_[tidx].cdr = new_pair; }
            tail = new_pair;
            v = pairs_[idx].cdr;
        }
        // Set last cdr to list2
        if (tail != 0) {
            auto tidx = static_cast<std::size_t>(tail - PAIR_SENTINEL);
            pairs_[tidx].cdr = list2;
        }
        return result;
    });
    primitives_.add("reverse", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        auto v = a[0]; std::int64_t result = 0;
        while (v != 0) {
            auto uv = static_cast<std::uint64_t>(v);
            if (uv < static_cast<std::uint64_t>(PAIR_SENTINEL)) return a[0];
            auto idx = static_cast<std::size_t>(uv - PAIR_SENTINEL);
            if (idx >= pairs_.size()) return a[0];
            auto new_id = pairs_.size();
            pairs_.push_back({pairs_[idx].car, result});
            result = PAIR_SENTINEL + static_cast<std::int64_t>(new_id);
            v = pairs_[idx].cdr;
        }
        return result;
    });
    primitives_.add("map", [this](const auto& a) {
        // map takes a fn value (int64_t) and a list — simplified
        // For now: not supported in simplified form.
        // Full map needs higher-order function support.
        return a.empty() ? std::int64_t(0) : a[0];
    });
    primitives_.add("filter", [this](const auto& a) {
        return a.empty() ? std::int64_t(0) : a[0];
    });

    // ── L6.8: Runtime type introspection ────────────────────────────
    // Infer a human-readable type name from a sentinel-based int64_t.
    // Order matters: check highest sentinel first (non-overlapping ranges).
    auto infer_type_name = [this](std::int64_t v) -> const char* {
        auto uv = static_cast<std::uint64_t>(v);
        if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL)) return "String";
        if (uv >= static_cast<std::uint64_t>(PAIR_SENTINEL))   return "Pair";
        if (uv >= static_cast<std::uint64_t>(CELL_SENTINEL))   return "Cell";
        if (uv >= static_cast<std::uint64_t>(CLOSURE_SENTINEL)) return "Closure";
        if (v == 0 || v == 1) return "Bool";
        return "Int";
    };

    // (type-of val) → type name as string
    primitives_.add("type-of", [this, infer_type_name](const auto& a) -> std::int64_t {
        if (a.empty()) return std::int64_t(0);
        auto type_name = infer_type_name(a[0]);
        auto id = string_heap_.size();
        string_heap_.push_back(type_name);
        return STRING_SENTINEL + static_cast<std::int64_t>(id);
    });

    // (type? val type-name) → bool
    primitives_.add("type?", [this, infer_type_name](const auto& a) -> std::int64_t {
        if (a.size() < 2) return FALSE_VAL;
        auto val_type = infer_type_name(a[0]);
        // Get the expected type name from the second arg
        auto expected_idx = static_cast<std::size_t>(a[1] - STRING_SENTINEL);
        if (expected_idx >= string_heap_.size()) return FALSE_VAL;
        auto& expected = string_heap_[expected_idx];
        return (val_type == expected) ? TRUE_VAL : FALSE_VAL;
    });


    primitives_.add("equal?", [this](const auto& a) {
        if (a.size() < 2) return TRUE_VAL;
        std::vector<std::pair<std::int64_t, std::int64_t>> stack;
        stack.push_back({a[0], a[1]});
        while (!stack.empty()) {
            auto [x, y] = stack.back(); stack.pop_back();
            if (x == y) continue;
            auto ux = static_cast<std::uint64_t>(x);
            auto uy = static_cast<std::uint64_t>(y);
            bool px = ux >= static_cast<std::uint64_t>(PAIR_SENTINEL) && ux < static_cast<std::uint64_t>(PAIR_SENTINEL) + pairs_.size();
            bool py = uy >= static_cast<std::uint64_t>(PAIR_SENTINEL) && uy < static_cast<std::uint64_t>(PAIR_SENTINEL) + pairs_.size();
            if (px && py) {
                auto ix = static_cast<std::size_t>(x - PAIR_SENTINEL);
                auto iy = static_cast<std::size_t>(y - PAIR_SENTINEL);
                if (ix >= pairs_.size() || iy >= pairs_.size()) return FALSE_VAL;
                stack.push_back({pairs_[ix].cdr, pairs_[iy].cdr});
                stack.push_back({pairs_[ix].car, pairs_[iy].car});
                continue;
            }
            bool sx = ux >= static_cast<std::uint64_t>(STRING_SENTINEL) && ux < static_cast<std::uint64_t>(STRING_SENTINEL) + string_heap_.size();
            bool sy = uy >= static_cast<std::uint64_t>(STRING_SENTINEL) && uy < static_cast<std::uint64_t>(STRING_SENTINEL) + string_heap_.size();
            if (sx && sy) {
                auto six = static_cast<std::size_t>(x - STRING_SENTINEL);
                auto siy = static_cast<std::size_t>(y - STRING_SENTINEL);
                if (six >= string_heap_.size() || siy >= string_heap_.size()) return FALSE_VAL;
                if (string_heap_[six] != string_heap_[siy]) return FALSE_VAL;
                continue;
            }
            return FALSE_VAL;
        }
        return TRUE_VAL;
    });
    static std::atomic<std::uint64_t> gs_counter_{0};
    primitives_.add("gensym", [](const auto&) -> std::int64_t {
        // Generate unique symbol name (stored in string heap)
        auto id = gs_counter_.fetch_add(1, std::memory_order_relaxed);
        // Return unique string name — caller must use with intern
        // For now, return the counter as marker; actual usage in macro expander
        return static_cast<std::int64_t>(id);
    });
    primitives_.add("display", [this](const auto& a) {
        if (a.empty()) return TRUE_VAL;
        io_print_val(a[0], &string_heap_, false);
        return TRUE_VAL;
    });
    primitives_.add("write", [this](const auto& a) -> std::int64_t {
        if (a.empty()) return TRUE_VAL;
        io_print_val(a[0], &string_heap_, true);
        return TRUE_VAL;
    });
    primitives_.add("newline", [](const auto&) { std::printf("\n"); return TRUE_VAL; });
    primitives_.add("error", [](const auto& a) -> std::int64_t {
        std::string msg = a.empty() ? "error" : std::to_string(a[0]);
        throw std::runtime_error(msg);
        return FALSE_VAL;
    });
    primitives_.add("assert", [this](const auto& a) -> std::int64_t {
        if (a.empty() || a[0] == FALSE_VAL) {
            std::string msg = "assertion failed";
            if (a.size() > 1) {
                auto uv = static_cast<std::uint64_t>(a[1]);
                if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL) && uv < static_cast<std::uint64_t>(STRING_SENTINEL) + string_heap_.size())
                    msg = string_heap_[static_cast<std::size_t>(a[1] - STRING_SENTINEL)];
                else
                    msg = std::to_string(a[1]);
            }
            throw std::runtime_error(msg);
        }
        return TRUE_VAL;
    });
    primitives_.add("read", [this](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty()) return std::int64_t(0);
        // Parse and evaluate using the CompilerService
        // For now: store the line as a string and return it
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return STRING_SENTINEL + static_cast<std::int64_t>(id);
    });
}

std::int64_t* Env::lookup_cell_ptr(const std::string& n, std::vector<std::int64_t>* cells) const {
    if (!cells) return nullptr;
    for (auto& b : bindings_) {
        if (b.first == n) {
            auto cv = b.second;
            if (static_cast<std::uint64_t>(cv) >= static_cast<std::uint64_t>(CELL_SENTINEL)) {
                auto ci = static_cast<std::size_t>(cv - CELL_SENTINEL);
                if (ci < cells->size()) return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    // Walk up the parent chain
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                auto cv = b.second;
                if (static_cast<std::uint64_t>(cv) >= static_cast<std::uint64_t>(CELL_SENTINEL)) {
                    auto ci = static_cast<std::size_t>(cv - CELL_SENTINEL);
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

EvalResult Evaluator::eval_in(const ast::Expr* e, const Env& env) {
    if (!e) return std::unexpected(Diagnostic{ErrorKind::InternalError, "null expression"});
    return std::visit([&](const auto& n) -> EvalResult {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ast::LiteralIntNode>)
            return n.value;
        if constexpr (std::is_same_v<T, ast::LiteralStringNode>) {
            // Store string in heap, return sentinel
            auto id = string_heap_.size();
            string_heap_.push_back(n.value);
            return STRING_SENTINEL + static_cast<std::int64_t>(id);
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
                    std::vector<std::int64_t> va;
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
            if (static_cast<std::uint64_t>(*fr) >= CLOSURE_SENTINEL)
                return apply_closure(static_cast<ClosureId>(*fr - CLOSURE_SENTINEL), n.args, env);
            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure, "not callable"});
        }
        if constexpr (std::is_same_v<T, ast::IfExprNode>) {
            auto c = eval_in(n.condition, env);
            if (!c) return c;
            return eval_in(*c ? n.then_branch : n.else_branch, env);
        }
        if constexpr (std::is_same_v<T, ast::LambdaNode>) {
            auto* cap = copy_env(env);
            auto id = next_id();
            closures_[id] = {n.params, n.body, cap};
            return static_cast<std::int64_t>(CLOSURE_SENTINEL + id);
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
            auto ci = alloc_cell(0);
            me.bind(n.name, static_cast<std::int64_t>(CELL_SENTINEL + ci));
            auto v = eval_in(n.value, env);
            if (!v) return v;
            cells_[ci] = *v;
            return v;
        }
        if constexpr (std::is_same_v<T, ast::LetRecNode>) {
            Env ne(&env);
            ne.set_primitives(&primitives_);
            ne.set_cells(&cells_);
            auto ci = alloc_cell(0);
            ne.bind(n.name, static_cast<std::int64_t>(CELL_SENTINEL + ci));
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
            return last ? eval_in(last, env) : EvalResult(0);
        }
        if constexpr (std::is_same_v<T, ast::SetNode>) {
            auto v = eval_in(n.value, env);
            if (!v) return v;
            // Try cell-based mutation first (letrec/define bindings)
            auto* cell_ptr = env.lookup_cell_ptr(n.name, &cells_);
            if (cell_ptr) {
                *cell_ptr = *v;
                return *v;
            }
            // Fallback: direct env mutation (let bindings)
            // Walk the env chain and mutate the first matching binding
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
            // Strip type annotation, evaluate inner expression
            return eval_in(n.inner_expr, env);
        }
        // ── Helper: runtime type coercion ──────────────────────────────
        auto coerce_value = [&](std::int64_t val, const std::string& to_type) -> std::int64_t {
            auto uv = static_cast<std::uint64_t>(val);
            bool is_int    = (uv < 0x1000000);
            bool is_string = (uv >= 0x8000000);
            
            if (to_type == "Any" || to_type.empty()) return val;
            
            // Coerce to Int
            if (to_type == "Int") {
                if (is_int) return val;                       // already int
                if (is_string) {                               // string → int
                    auto idx = static_cast<std::size_t>(uv - STRING_SENTINEL);
                    if (idx < string_heap_.size()) {
                        try { return static_cast<std::int64_t>(std::stoll(string_heap_[idx])); }
                        catch (...) { return 0; }
                    }
                    return 0;
                }
                return val;  // other sentinel: pass through
            }
            
            // Coerce to String
            if (to_type == "String") {
                if (is_string) return val;                    // already string
                if (is_int) {                                  // int → string
                    auto s = std::to_string(val);
                    auto id = string_heap_.size();
                    string_heap_.push_back(std::move(s));
                    return STRING_SENTINEL + static_cast<std::int64_t>(id);
                }
                return val;
            }
            
            // Coerce to Bool
            if (to_type == "Bool") {
                return (val != 0) ? std::int64_t(1) : std::int64_t(0);
            }
            
            // Unknown type: pass through
            return val;
        };
        
        if constexpr (std::is_same_v<T, ast::CoercionNode>) {
            auto result = eval_in(n.inner_expr, env);
            if (result) {
                return coerce_value(*result, n.to_type_name);
            }
            // Propagate error
            return std::unexpected(result.error());
        }
        if constexpr (std::is_same_v<T, ast::MacroDefNode>) {
            // Clone the body into a persistent CloneNode-backed copy.
            // The original body is in arena_ which gets reset between evals.
            // We must deep-copy it now while arena_ is still alive.
            // Simplest approach: serialize → deserialize via expand_macro's clone
            // Keep the macro in a persistent AST by cloning into a static arena.
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

            // Phase 3b D3: Validate macro template at definition time
            aura::diag::DiagnosticCollector macro_diag;
            aura::compiler::MacroValidation mv;
            aura::compiler::validate_macro_template(mv, n.name, n.params, persistent_body, &macro_diag);
            // Validation does not block registration — warnings only
            (void)mv;

            const_cast<Evaluator*>(this)->macros_[n.name] = MacroDef{n.params, persistent_body};
            return 0;
        }
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "unknown expression type"});
    }, e->payload);
}

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

// ── Phase 4: FlatAST tree-walker evaluator ─────────────────────
// Evaluates FlatAST nodes directly, bypassing Expr* reconstruction.
// Uses reconstruct_expr only for Lambda body (needed by Closure table).
EvalResult Evaluator::eval_flat(aura::ast::FlatAST& flat,
                                 aura::ast::StringPool& pool,
                                 aura::ast::NodeId id,
                                 const Env& env) {
    if (id >= flat.size())
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "invalid node id"});
    auto v = flat.get(id);
    switch (v.tag) {
    case aura::ast::NodeTag::LiteralInt:
        return v.int_value;
    case aura::ast::NodeTag::LiteralString: {
        auto sid = string_heap_.size();
        string_heap_.push_back(std::string(pool.resolve(v.sym_id)));
        return STRING_SENTINEL + static_cast<std::int64_t>(sid);
    }
    case aura::ast::NodeTag::Variable: {
        auto name = pool.resolve(v.sym_id);
        auto val = env.lookup(std::string(name));
        if (val) return *val;
        return std::unexpected(Diagnostic{ErrorKind::UnboundVariable,
                                          "unbound variable: " + std::string(name)});
    }
    case aura::ast::NodeTag::Call: {
        if (v.children.empty()) return EvalResult(0);
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
        // Primitive call (check BEFORE evaluating callee name)
        if (callee.tag == aura::ast::NodeTag::Variable) {
            auto cname = std::string(pool.resolve(callee.sym_id));
            auto prim = env.lookup_primitive(cname);
            if (prim) {
                std::vector<std::int64_t> args;
                for (std::size_t i = 1; i < v.children.size(); ++i) {
                    auto ar = eval_flat(flat, pool, v.child(i), env);
                    if (!ar) return ar;
                    args.push_back(*ar);
                }
                return (*prim)(args);
            }
        }
        // Closure call (evaluate callee, apply)
        auto fn = eval_flat(flat, pool, callee_id, env);
        if (!fn) return fn;
        auto fn_uv = static_cast<std::uint64_t>(*fn);
        if (fn_uv >= CLOSURE_SENTINEL) {
            auto cid = static_cast<ClosureId>(*fn - static_cast<std::int64_t>(CLOSURE_SENTINEL));
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
        if (v.children.size() < 3) return EvalResult(0);
        auto c = eval_flat(flat, pool, v.child(0), env);
        if (!c) return c;
        return eval_flat(flat, pool, *c ? v.child(1) : v.child(2), env);
    }
    case aura::ast::NodeTag::Lambda: {
        // Reconstruct lambda expression for closure storage
        auto* expr = reconst_node(id, flat, pool, *arena_);
        if (!expr || expr->tag != aura::ast::NodeTag::Lambda)
            return std::unexpected(Diagnostic{ErrorKind::InternalError, "eval_flat: lambda reconstruct failed"});
        auto& lam = std::get<aura::ast::LambdaNode>(expr->payload);
        auto* cap = copy_env(env);
        auto cid = next_id();
        closures_[cid] = Closure{lam.params, lam.body, cap};
        return static_cast<std::int64_t>(CLOSURE_SENTINEL + cid);
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
            cells_.push_back(0);
            ne.bind(std::string(name), CELL_SENTINEL + static_cast<std::int64_t>(ci));
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
        auto ci = alloc_cell(0);
        me.bind(std::string(name), static_cast<std::int64_t>(CELL_SENTINEL + static_cast<std::int64_t>(ci)));
        auto vv = eval_flat(flat, pool, val_id, env);
        if (!vv) return vv;
        cells_[ci] = *vv;
        return *vv;
    }
    case aura::ast::NodeTag::Begin: {
        EvalResult last = EvalResult(0);
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
        if (v.children.empty()) return EvalResult(0);
        return eval_flat(flat, pool, v.child(0), env);
    }
    case aura::ast::NodeTag::TypeAnnotation: {
        if (v.children.empty()) return EvalResult(0);
        return eval_flat(flat, pool, v.child(0), env);
    }
    case aura::ast::NodeTag::MacroDef: {
        auto name = pool.resolve(v.sym_id);
        std::vector<std::string> param_names;
        for (auto p : v.params)
            param_names.push_back(std::string(pool.resolve(p)));
        auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        if (body_id == aura::ast::NULL_NODE) return EvalResult(0);
        // Reconstruct body to Expr* for persistent storage
        auto* body_expr = reconst_node(body_id, flat, pool, *arena_);
        if (!body_expr) return EvalResult(0);
        // Clone to persistent arena
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
        // Phase 3b D3: Compile-time macro template validation
        // Inline validation checks (MacroValidation struct is incomplete here)
        if (body_expr) {
            // Check for constant body (no param references)
            bool has_param_ref = false;
            for (auto& p : param_names) {
                // Simple check: scan body for VariableNodes with matching name
                // Use string matching on the body (sufficient for now)
                struct Checker { std::vector<std::string> params; bool found = false; };
                // Walk the Expr* body to check param references
                // For now, just warn if body is a literal
                if (body_expr->tag == aura::ast::NodeTag::LiteralInt ||
                    body_expr->tag == aura::ast::NodeTag::LiteralString) {
                    has_param_ref = false;
                } else {
                    has_param_ref = true;  // assume params are used
                }
            }
            if (!has_param_ref && param_names.empty() && body_expr->tag == aura::ast::NodeTag::LiteralInt) {
                std::println(std::cerr, "warning: macro '{}': body does not reference any parameters "
                             "— always expands to the same expression", std::string(name));
            }
            // Check for unused params
            for (auto& p : param_names) {
                // Check if param name appears in the body's variable references
                // This is a simplified check; full tree walk needs MacroValidation
                bool found = false;
                if (body_expr->tag == aura::ast::NodeTag::Variable) {
                    auto& var_node = std::get<aura::ast::VariableNode>(body_expr->payload);
                    if (var_node.name == p) found = true;
                }
                if (body_expr->tag == aura::ast::NodeTag::Call) {
                    auto& call_node = std::get<aura::ast::CallNode>(body_expr->payload);
                    // Check function and args
                    // This is simplified — full check uses collect_var_refs
                }
                // Simplified unused param warning
                std::println(std::cerr, "warning: macro '{}': parameter '{}' is never used in template",
                             std::string(name), p);
            }
        }
        macros_[std::string(name)] = MacroDef{std::move(param_names), persistent_body};
        return EvalResult(0);
    }
    default:
        return std::unexpected(Diagnostic{ErrorKind::InternalError,
                                          "eval_flat: unsupported node type"});
    }
}

// ── Macro template validation (Phase 3b D3) ──────────────────
// Runs at defmacro definition time to catch errors early.
struct MacroValidation {
    bool body_valid = true;        // body is non-null
    std::vector<std::string> unused_params;   // params never referenced
    std::vector<std::string> free_vars;       // vars in body not in param list
    bool constant_body = false;    // no param references → always same result
};

// Recursively collect all VariableNode names from an Expr tree.
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
            // Lambda params shadow outer scope — don't collect them as free
            collect_var_refs(node.body, vars);
            for (auto& p : node.params) vars.erase(p);
        } else if constexpr (std::is_same_v<T, ast::LetNode>) {
            collect_var_refs(node.value, vars);
            collect_var_refs(node.body, vars);
            vars.erase(node.name);  // let binding shadows
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
            // Quoted expressions don't introduce free references
        } else if constexpr (std::is_same_v<T, ast::TypeAnnotationNode>) {
            collect_var_refs(node.inner_expr, vars);
        } else if constexpr (std::is_same_v<T, ast::CoercionNode>) {
            collect_var_refs(node.inner_expr, vars);
        }
        // LiteralIntNode, LiteralStringNode, MacroDefNode: no variables
    }, e->payload);
}

// Validate a macro template body at definition time.
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

    // Collect all variable references in the body
    std::unordered_set<std::string> body_vars;
    collect_var_refs(body, body_vars);

    // Check for unused params
    for (auto& p : params) {
        if (body_vars.find(p) == body_vars.end()) {
            mv.unused_params.push_back(p);
            if (diag) {
                // Warning only — unused param is not fatal
                std::println(std::cerr, "warning: macro '{}': parameter '{}' is never used in template",
                             name, p);
            }
        }
    }

    // Collect free variables (vars in body that aren't params)
    std::unordered_set<std::string> param_set(params.begin(), params.end());
    for (auto& v : body_vars) {
        if (param_set.find(v) == param_set.end()) {
            mv.free_vars.push_back(v);
        }
    }

    // Check for constant body (no param references)
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

// ── Macro expansion: substitute params with args in body AST ──
ast::Expr* Evaluator::expand_macro(const std::string& name,
                                    const std::vector<ast::Expr*>& args) {
    auto it = macros_.find(name);
    if (it == macros_.end()) return nullptr;
    auto& mac = it->second;

    // Clone the body AST, replacing VariableNodes that match params
    // with the corresponding argument expressions.
    // Uses a recursive walk with arena allocation.
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
                    // Check if this variable matches a macro parameter
                    for (std::size_t i = 0; i < params->size(); ++i) {
                        if (node.name == (*params)[i]) {
                            // Substitute: return cloned arg
                            return clone((*args)[i]);
                        }
                    }
                    // Check rename map (hygienic binding)
                    auto it = rename_.find(node.name);
                    if (it != rename_.end())
                        return arena->template create<ast::Expr>(ast::VariableNode{node.tag, it->second});
                    // Not a param — clone as-is
                    return arena->template create<ast::Expr>(ast::VariableNode{node.tag, node.name});
                }

                if constexpr (std::is_same_v<T, ast::LiteralIntNode>) {
                    return arena->template create<ast::Expr>(ast::LiteralIntNode{node.tag, node.value});
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
                    // Hygienic: rename all lambda params with fresh names
                    std::vector<std::string> new_params;
                    for (auto& p : node.params) {
                        auto fresh = fresh_name(p);
                        rename_[p] = fresh;
                        new_params.push_back(std::move(fresh));
                    }
                    ast::LambdaNode lam{node.tag, std::move(new_params), clone(node.body)};
                    // Restore original names (scoped rename)
                    for (auto& p : node.params) rename_.erase(p);
                    return arena->template create<ast::Expr>(std::move(lam));
                }

                if constexpr (std::is_same_v<T, ast::LetNode>) {
                    // Hygienic: rename let binding
                    auto fresh = fresh_name(node.name);
                    rename_[node.name] = fresh;
                    auto* cloned = arena->template create<ast::Expr>(
                        ast::LetNode{node.tag, fresh, clone(node.value), clone(node.body)});
                    rename_.erase(node.name);
                    return cloned;
                }

                if constexpr (std::is_same_v<T, ast::LetRecNode>) {
                    // Hygienic: rename letrec binding
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
