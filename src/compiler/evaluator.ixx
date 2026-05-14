export module aura.compiler.evaluator;
import std;
import aura.core;
import aura.diag;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(const std::vector<EvalValue>&)>;

export class Primitives {
public:
    Primitives();
    std::optional<PrimFn> lookup(const std::string& n) const;
    void add(const std::string& name, PrimFn fn) { table_[name] = std::move(fn); }
    void set_string_heap(std::vector<std::string>* h) { string_heap_ = h; }
    const std::vector<std::string>& string_heap() const { return *string_heap_; }
    std::vector<std::string>& string_heap() { return *string_heap_; }
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
    void set_cells(std::vector<types::EvalValue>* c) { cells_=c; }
    void bind(const std::string& n, types::EvalValue v) { bindings_.emplace_back(n,std::move(v)); }
    std::optional<types::EvalValue> lookup(const std::string& n) const;
    std::optional<PrimFn> lookup_primitive(const std::string& n) const { return primitives_?primitives_->lookup(n):std::nullopt; }
    types::EvalValue* lookup_cell_ptr(const std::string& n, std::vector<types::EvalValue>* cells) const;
    const Env* parent() const { return parent_; }
    std::vector<std::pair<std::string,types::EvalValue>>& bindings() { return bindings_; }
    const std::vector<std::pair<std::string,types::EvalValue>>& bindings() const { return bindings_; }
private:
    const Env* parent_=nullptr; const Primitives* primitives_=nullptr;
    std::vector<types::EvalValue>* cells_=nullptr;
    std::vector<std::pair<std::string,types::EvalValue>> bindings_;
};

export using ClosureId = std::uint64_t;

export struct Pair {
    types::EvalValue car;
    types::EvalValue cdr;
};

export struct MacroDef { std::vector<std::string> params; ast::FlatAST* flat=nullptr; ast::StringPool* pool=nullptr; ast::NodeId body_id=ast::NULL_NODE; };

export struct Closure { std::vector<std::string> params; ast::FlatAST* flat=nullptr; ast::StringPool* pool=nullptr; ast::NodeId body_id=ast::NULL_NODE; const Env* env=nullptr; };

export using EvalResult = std::expected<types::EvalValue, aura::diag::Diagnostic>;

export class Evaluator {
public:
    Evaluator();
    void set_arena(ast::ASTArena* a) { arena_=a; }
    EvalResult eval_flat(aura::ast::FlatAST& flat,
                          aura::ast::StringPool& pool,
                          aura::ast::NodeId id,
                          const Env& env);
    const Primitives& primitives() const { return primitives_; }
    Env& top_env() { return top_; }
private:
    ClosureId next_id() { return next_id_++; }
    std::size_t alloc_cell(const types::EvalValue& v) { cells_.push_back(v); return cells_.size()-1; }
    // (apply_closure and expand_macro removed — use eval_flat directly)
    Env* copy_env(const Env& env);
    void init_pair_primitives();
    Env top_; Primitives primitives_; ast::ASTArena* arena_=nullptr;
    std::unordered_map<ClosureId,Closure> closures_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::unordered_set<std::string> loaded_modules_;
    std::vector<types::EvalValue> cells_;
    std::vector<Pair> pairs_;
    std::vector<std::string> string_heap_;
    struct HashTable {
        std::vector<std::uint8_t> metadata;  // 0xFF=empty, 0x00-0x7F=occupied(7-bit fingerprint)
        std::vector<types::EvalValue> keys;
        std::vector<types::EvalValue> values;
        std::size_t size = 0;      // live entries
        std::size_t capacity = 0;  // power of 2
    };
    std::vector<HashTable> hash_heap_;
    std::vector<std::vector<types::EvalValue>> vector_heap_;
    std::uint64_t next_id_=1;
};

// Pre-expand all macros in a FlatAST. Returns (possibly new) root.
export aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                           aura::ast::NodeId root, int max_passes = 10);

} // namespace aura::compiler
