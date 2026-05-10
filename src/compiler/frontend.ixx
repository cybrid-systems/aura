module;
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <utility>
#include <functional>
#include <unordered_map>
#include <memory>

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

export class Env final {
public:
    using CellTable = std::vector<int64_t>;

    Env() = default;
    explicit Env(const Env* parent) : parent_(parent) {}
    Env(const Env&) = default;
    Env& operator=(const Env&) = default;

    void set_parent(const Env* p) { parent_ = p; }
    void set_primitives(const Primitives* p) { primitives_ = p; }
    void set_cells(CellTable* c) { cells_ = c; }
    void bind(const std::string& name, int64_t value) { bindings_.emplace_back(name, value); }
    
    std::optional<int64_t> lookup(const std::string& name) const;

    std::optional<PrimitiveFn> lookup_primitive(const std::string& name) const {
        return primitives_ ? primitives_->lookup(name) : std::nullopt;
    }

private:
    const Env* parent_ = nullptr;
    const Primitives* primitives_ = nullptr;
    CellTable* cells_ = nullptr;
    std::vector<std::pair<std::string, int64_t>> bindings_;
};

export using ClosureId = uint64_t;
constexpr ClosureId CLOSURE_SENTINEL = 0x1000000;
constexpr int64_t CELL_SENTINEL = 0x2000000;

export struct Closure {
    std::vector<std::string> params;
    const ast::Expr* body = nullptr;
    // Points to an arena-allocated Env. Since arena is reset between compiles,
    // env data lives as long as the AST it references.
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
    void set_arena(ast::ASTArena* a) { arena_ = a; }
    EvalResult eval(const ast::Expr* expr) { return eval_in(expr, top_); }
    EvalResult eval_in(const ast::Expr* expr, const Env& env);

private:
    ClosureId next_id() { return next_id_++; }
    size_t alloc_cell(int64_t val) { cells_.push_back(val); return cells_.size() - 1; }
    EvalResult apply_closure(ClosureId id, const std::vector<ast::Expr*>& args, const Env& call_env);
    Env* copy_env(const Env& env);

    Env top_;
    Primitives primitives_;
    ast::ASTArena* arena_ = nullptr;
    std::unordered_map<ClosureId, Closure> closures_;
    Env::CellTable cells_;
    uint64_t next_id_ = 1;
};

} // namespace aura::compiler
