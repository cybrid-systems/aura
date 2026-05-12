module aura.compiler.frontend;
import std;

namespace aura::compiler {

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
    table_["+"]  = [](auto& a) { return a[0] + a[1]; };
    table_["-"]  = [](auto& a) { return a.size() == 1 ? -a[0] : a[0] - a[1]; };
    table_["*"]  = [](auto& a) { return a[0] * a[1]; };
    table_["/"]  = [](auto& a) { return a[0] / a[1]; };
    table_["="]  = [](auto& a) { return a[0] == a[1]; };
    table_["<"]  = [](auto& a) { return a[0] < a[1]; };
    table_[">"]  = [](auto& a) { return a[0] > a[1]; };
    table_["<="] = [](auto& a) { return a[0] <= a[1]; };
    table_[">="] = [](auto& a) { return a[0] >= a[1]; };
    // Ghuloum Step 9: booleans
    table_["not"]  = [](auto& a) { return a[0] == 0 ? TRUE_VAL : FALSE_VAL; };
    table_["and"]  = [](auto& a) { return a[0] && a[1] ? TRUE_VAL : FALSE_VAL; };
    table_["or"]   = [](auto& a) { return a[0] || a[1] ? TRUE_VAL : FALSE_VAL; };
    table_["eq?"]  = [](auto& a) { return a[0] == a[1] ? TRUE_VAL : FALSE_VAL; };
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
            if (static_cast<std::uint64_t>(v) >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
                auto idx = static_cast<std::size_t>(v - STRING_SENTINEL);
                if (idx < string_heap_.size()) result += string_heap_[idx];
            }
        }
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return STRING_SENTINEL + static_cast<std::int64_t>(id);
    });
    primitives_.add("string-length", [this](const auto& a) {
        if (a.empty()) return std::int64_t(0);
        auto idx = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
        return static_cast<std::int64_t>(idx < string_heap_.size() ? string_heap_[idx].size() : 0);
    });
    primitives_.add("string-ref", [this](const auto& a) {
        if (a.size() < 2) return std::int64_t(0);
        auto idx = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
        auto pos = static_cast<std::size_t>(a[1]);
        if (idx < string_heap_.size() && pos < string_heap_[idx].size())
            return static_cast<std::int64_t>(static_cast<unsigned char>(string_heap_[idx][pos]));
        return std::int64_t(0);
    });
    primitives_.add("substring", [this](const auto& a) {
        if (a.size() < 3) return std::int64_t(0);
        auto idx  = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
        auto start = static_cast<std::size_t>(a[1]);
        auto end   = static_cast<std::size_t>(a[2]);
        if (idx >= string_heap_.size()) return std::int64_t(0);
        auto& s = string_heap_[idx];
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
        auto i1 = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
        auto i2 = static_cast<std::size_t>(a[1] - STRING_SENTINEL);
        if (i1 >= string_heap_.size() || i2 >= string_heap_.size())
            return a[0] == a[1] ? TRUE_VAL : FALSE_VAL;
        return string_heap_[i1] == string_heap_[i2] ? TRUE_VAL : FALSE_VAL;
    });
    primitives_.add("string<?", [this](const auto& a) {
        if (a.size() < 2) return FALSE_VAL;
        auto i1 = static_cast<std::size_t>(a[0] - STRING_SENTINEL);
        auto i2 = static_cast<std::size_t>(a[1] - STRING_SENTINEL);
        if (i1 >= string_heap_.size() || i2 >= string_heap_.size())
            return FALSE_VAL;
        return string_heap_[i1] < string_heap_[i2] ? TRUE_VAL : FALSE_VAL;
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
    primitives_.add("display", [this](const auto& a) {
        if (a.empty() || a.size() < 1) return TRUE_VAL;
        auto v = a[0]; auto uv = static_cast<std::uint64_t>(v);
        if (v == 0) std::print("()");
        else if (v == TRUE_VAL) std::print("#t");
        else if (v == FALSE_VAL) std::print("#f");
        else if (uv >= static_cast<std::uint64_t>(PAIR_SENTINEL) && uv < static_cast<std::uint64_t>(PAIR_SENTINEL) + pairs_.size())
            std::print("<pair>");
        else if (uv >= static_cast<std::uint64_t>(STRING_SENTINEL)) {
            auto idx = static_cast<std::size_t>(v - STRING_SENTINEL);
            if (idx < string_heap_.size()) std::print("{}", string_heap_[idx]);
        }
        else std::print("{}", v);
        return TRUE_VAL;
    });
    primitives_.add("newline", [](const auto&) { std::println(""); return TRUE_VAL; });
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
                        return nullptr;
                    }, e->payload);
                }
            };
            Cloner cloner{&persistent_arena};
            auto* persistent_body = cloner.clone(n.body);
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
                    ast::LambdaNode lam{node.tag, node.params, clone(node.body)};
                    return arena->template create<ast::Expr>(std::move(lam));
                }

                if constexpr (std::is_same_v<T, ast::LetNode>) {
                    return arena->template create<ast::Expr>(
                        ast::LetNode{node.tag, node.name, clone(node.value), clone(node.body)});
                }

                if constexpr (std::is_same_v<T, ast::LetRecNode>) {
                    return arena->template create<ast::Expr>(
                        ast::LetRecNode{node.tag, node.name, clone(node.value), clone(node.body)});
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

                return nullptr;
            }, expr->payload);
        }
    };

    Expander expander{arena_, &mac.params, &args};
    return expander.clone(mac.body);
}

} // namespace aura::compiler
