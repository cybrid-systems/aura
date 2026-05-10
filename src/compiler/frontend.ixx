module;
#include <cstdint>
#include <string>
#include <variant>

export module aura.compiler.frontend;

import aura.core;

namespace aura::compiler {

export struct EvalResult {
    bool success = false;
    int64_t int_value = 0;
    std::string error;
};

export class Evaluator {
public:
    explicit Evaluator() {}
    EvalResult eval(const ast::Expr* expr);
};

} // namespace aura::compiler
