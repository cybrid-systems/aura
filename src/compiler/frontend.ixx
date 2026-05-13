export module aura.compiler.frontend;
import std;
import aura.core;
import aura.core.ast_flat;
import aura.core.ast_pool;
import aura.diag;

namespace aura::compiler {

using PrimFn = std::function<std::int64_t(const std::vector<std::int64_t>&)>;

export class Primitives {
public:
    Primitives();
    std::optional<PrimFn> lookup(const std::string& n) const;
    void add(const std::string& name, PrimFn fn) { table_[name] = std::move(fn); }
    void set_string_heap(std::vector<std::string>* h) { string_heap_ = h; }
    const std::vector<std::string>& string_heap() const { return *string_heap_; }
    std::vector<std::string>& string_heap() { return *string_heap_; }
    // Runtime coercion: String sentinel → int64_t value
    std::int64_t str_to_int(std::int64_t v) const;
private:
    std::unordered_map<std::string, PrimFn> table_;
    std::vector<std::string>* string_heap_ = nullptr;
};

export class Env final {
public:
    Env() = default;
    explicit Env(const Env* p) : parent_(p) {}
    Env(const Env&) = default; Env& operator=(const Env&) = default;
    void set_parent(const Env* p) { parent_=p; }
    void set_primitives(const Primitives* p) { primitives_=p; }
    void set_cells(std::vector<std::int64_t>* c) { cells_=c; }
    void bind(const std::string& n, std::int64_t v) { bindings_.emplace_back(n,v); }
    std::optional<std::int64_t> lookup(const std::string& n) const;
    std::optional<PrimFn> lookup_primitive(const std::string& n) const { return primitives_?primitives_->lookup(n):std::nullopt; }
    std::int64_t* lookup_cell_ptr(const std::string& n, std::vector<std::int64_t>* cells) const;
    const Env* parent() const { return parent_; }
    std::vector<std::pair<std::string,std::int64_t>>& bindings() { return bindings_; }
    const std::vector<std::pair<std::string,std::int64_t>>& bindings() const { return bindings_; }
private:
    const Env* parent_=nullptr; const Primitives* primitives_=nullptr;
    std::vector<std::int64_t>* cells_=nullptr;
    std::vector<std::pair<std::string,std::int64_t>> bindings_;
};

export using ClosureId = std::uint64_t;
constexpr ClosureId CLOSURE_SENTINEL = 0x1000000;
constexpr std::int64_t CELL_SENTINEL = 0x2000000;
constexpr std::int64_t PAIR_SENTINEL = 0x4000000;
constexpr std::int64_t STRING_SENTINEL = 0x8000000;
constexpr std::int64_t TRUE_VAL = 1;
constexpr std::int64_t FALSE_VAL = 0;

export struct Pair {
    std::int64_t car;
    std::int64_t cdr;
};

export struct MacroDef { std::vector<std::string> params; ast::Expr* body=nullptr; };

export struct Closure { std::vector<std::string> params; const ast::Expr* body=nullptr; const Env* env=nullptr; };

// EvalResult — now an alias for std::expected<int64_t, diag::Diagnostic>
// Replace .success with .has_value(), .int_value with .value(), .error with .error().message
export using EvalResult = std::expected<std::int64_t, aura::diag::Diagnostic>;

// Legacy EvalResult struct kept for binary compat during migration
// Will be removed after all callers migrate

export class Evaluator {
public:
    Evaluator();
    void set_arena(ast::ASTArena* a) { arena_=a; }
    EvalResult eval(const ast::Expr* e) { return eval_in(e,top_); }
    EvalResult eval_in(const ast::Expr* e, const Env& env);
    // Phase 4: tree-walker evaluator directly on FlatAST (no Expr* bridge)
    EvalResult eval_flat(aura::ast::FlatAST& flat,
                          aura::ast::StringPool& pool,
                          aura::ast::NodeId id,
                          const Env& env);
    const Primitives& primitives() const { return primitives_; }
    Env& top_env() { return top_; }
private:
    ast::Expr* expand_macro(const std::string& name, const std::vector<ast::Expr*>& args);
    ClosureId next_id() { return next_id_++; }
    std::size_t alloc_cell(std::int64_t v) { cells_.push_back(v); return cells_.size()-1; }
    EvalResult apply_closure(ClosureId id, const std::vector<ast::Expr*>& args, const Env& call_env);
    Env* copy_env(const Env& env);
    void init_pair_primitives();
    Env top_; Primitives primitives_; ast::ASTArena* arena_=nullptr;
    std::unordered_map<ClosureId,Closure> closures_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::vector<std::int64_t> cells_;
    std::vector<Pair> pairs_;
    std::vector<std::string> string_heap_;
    std::uint64_t next_id_=1;
};

} // namespace aura::compiler
