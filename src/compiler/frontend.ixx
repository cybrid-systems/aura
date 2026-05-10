export module aura.compiler.frontend;

import <cstdint>;
import <string>;
import <variant>;
import <vector>;
import <optional>;
import <utility>;

import aura.core;

namespace aura::compiler {

export class Env {
public:
    Env() = default;
    explicit Env(const Env* parent) : parent_(parent) {}
    void bind(const std::string& name, int64_t value) { bindings_.emplace_back(name, value); }
    std::optional<int64_t> lookup(const std::string& name) const {
        for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
            if (it->first == name) return it->second;
        return parent_ ? parent_->lookup(name) : std::nullopt;
    }
private:
    const Env* parent_ = nullptr;
    std::vector<std::pair<std::string, int64_t>> bindings_;
};

export struct EvalResult {
    bool success = false;
    int64_t int_value = 0;
    std::string error;
};

export class Evaluator {
public:
    EvalResult eval(const ast::Expr* expr) { return eval_in(expr, top_); }
    EvalResult eval_in(const ast::Expr* expr, const Env& env);
    Env& top_env() { return top_; }
private:
    Env top_;
};

} // namespace aura::compiler
