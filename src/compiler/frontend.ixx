module;
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <utility>
#include <functional>
#include <unordered_map>

export module aura.compiler.frontend;

import aura.core;

namespace aura::compiler {

using PrimitiveFn = std::function<int64_t(const std::vector<int64_t>&)>;

export class Primitives {
public:
    Primitives();
    std::optional<PrimitiveFn> lookup(const std::string& name) const;
private:
    std::unordered_map<std::string, PrimitiveFn> table_;
};

export class Env {
public:
    Env() = default;
    explicit Env(const Env* parent) : parent_(parent), primitives_(parent ? parent->primitives_ : nullptr) {}
    void set_primitives(const Primitives* p) { primitives_ = p; }
    void bind(const std::string& name, int64_t value) { bindings_.emplace_back(name, value); }
    std::optional<int64_t> lookup(const std::string& name) const;
    std::optional<PrimitiveFn> lookup_primitive(const std::string& name) const {
        return primitives_ ? primitives_->lookup(name) : std::nullopt;
    }
private:
    const Env* parent_ = nullptr;
    const Primitives* primitives_ = nullptr;
    std::vector<std::pair<std::string, int64_t>> bindings_;
};

export struct Closure {
    std::vector<std::string> params;
    const ast::Expr* body = nullptr;
    const Env* env = nullptr;
};

export struct EvalResult {
    bool success = false;
    int64_t int_value = 0;
    std::string error;
};

export class Evaluator {
public:
    Evaluator();
    EvalResult eval(const ast::Expr* expr) { return eval_in(expr, top_); }
    EvalResult eval_in(const ast::Expr* expr, const Env& env);
private:
    Env top_;
    Primitives primitives_;
};

} // namespace aura::compiler
