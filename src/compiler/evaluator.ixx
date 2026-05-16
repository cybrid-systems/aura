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
    void add(const std::string& name, PrimFn fn) { 
        auto slot = ordered_names_.size();
        table_[name] = std::move(fn);
        ordered_names_.push_back(name);
    }
    void set_string_heap(std::vector<std::string>* h) { string_heap_ = h; }
    const std::vector<std::string>& string_heap() const { return *string_heap_; }
    std::vector<std::string>& string_heap() { return *string_heap_; }
    // Slot-based lookup for primitive values
    const std::string& name_for_slot(std::size_t slot) const { return ordered_names_[slot]; }
    std::size_t slot_for_name(const std::string& name) const;
    std::size_t slot_count() const { return ordered_names_.size(); }
private:
    std::unordered_map<std::string, PrimFn> table_;
    std::vector<std::string>* string_heap_ = nullptr;
    std::vector<std::string> ordered_names_;
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
    [[nodiscard]] std::optional<types::EvalValue> lookup(const std::string& n) const;
    // Look up the raw binding without dereferencing cells (returns cell sentinel as-is)
    std::optional<types::EvalValue> lookup_binding(const std::string& n) const;
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

export struct MacroDef { std::vector<std::string> params; bool dotted=false; ast::FlatAST* flat=nullptr; ast::StringPool* pool=nullptr; ast::NodeId body_id=ast::NULL_NODE; };

export struct Closure { std::vector<std::string> params; ast::FlatAST* flat=nullptr; ast::StringPool* pool=nullptr; ast::NodeId body_id=ast::NULL_NODE; const Env* env=nullptr; };

export using EvalResult = std::expected<types::EvalValue, aura::diag::Diagnostic>;

export class Evaluator {
public:
    Evaluator();
    void set_arena(ast::ASTArena* a) { arena_=a; }
    // Set current FlatAST/Pool for mutation primitives
    void set_flat_pool(ast::FlatAST* f, ast::StringPool* p) { current_flat_ = f; current_pool_ = p; }
    [[nodiscard]] EvalResult eval_flat(aura::ast::FlatAST& flat,
                          aura::ast::StringPool& pool,
                          aura::ast::NodeId id,
                          const Env& env);
    const Primitives& primitives() const { return primitives_; }
    Primitives& primitives() { return primitives_; }
    const Env& top_env() const { return top_; }
    Env& top_env() { return top_; }
    const std::vector<Pair>& pairs() const { return pairs_; }
private:
    ClosureId next_id() { return next_id_++; }
    [[nodiscard]] std::size_t alloc_cell(const types::EvalValue& v) { cells_.push_back(v); return cells_.size()-1; }
    // (apply_closure and expand_macro removed — use eval_flat directly)
    [[nodiscard]] EvalValue ast_to_data(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, aura::ast::NodeId nid);
    [[nodiscard]] ast::NodeId data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat, aura::ast::StringPool& pool, int depth = 0);
    [[nodiscard]] EvalResult eval_data_as_code(const types::EvalValue& data, const Env& env,
                                                  aura::ast::FlatAST* flat = nullptr,
                                                  aura::ast::StringPool* pool = nullptr);
    Env* copy_env(const Env& env);
    void init_pair_primitives();
    void build_primitive_slots();
    Env top_; Primitives primitives_; ast::ASTArena* arena_=nullptr;
    ast::FlatAST* current_flat_ = nullptr;
    ast::StringPool* current_pool_ = nullptr;
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

// Pair-aware value formatting (recursively prints lists)
export inline std::string format_value(const types::EvalValue& v, const std::vector<std::string>* heap,
                                        const std::vector<Pair>* pairs, int depth = 0,
                                        const Primitives* primitives = nullptr) {
    const int max_depth = 64;
    if (depth > max_depth) return "...";
    if (types::is_void(v)) return "()";
    if (types::is_bool(v)) return types::as_bool(v) ? "#t" : "#f";
    if (types::is_int(v)) return std::to_string(types::as_int(v));
    if (types::is_float(v)) return std::to_string(types::as_float(v));
    if (types::is_string(v)) {
        if (heap) {
            auto idx = types::as_string_idx(v);
            if (idx < heap->size()) return std::format("\"{}\"", (*heap)[idx]);
        }
        return std::format("<string[{}]>", types::as_string_idx(v));
    }
    if (types::is_pair(v) && pairs) {
        auto idx = types::as_pair_idx(v);
        if (idx >= pairs->size()) return std::format("<pair[{}]>", idx);

        // Walk the cdr chain to collect all elements
        std::vector<std::string> elements;
        auto current = v;

        while (types::is_pair(current)) {
            auto cidx = types::as_pair_idx(current);
            if (cidx >= pairs->size()) { break; }
            elements.push_back(format_value((*pairs)[cidx].car, heap, pairs, depth + 1, primitives));
            current = (*pairs)[cidx].cdr;
            if (elements.size() > 256) { elements.push_back("..."); break; }
        }

        std::string result = "(";
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (i > 0) result += " ";
            result += elements[i];
        }
        if (!types::is_truthy(current)) {
            // proper list
        } else {
            if (!elements.empty()) result += " . ";
            result += format_value(current, heap, pairs, depth + 1, primitives);
        }
        result += ")";
        return result;
    }
    if (types::is_vector(v)) return std::format("<vector[{}]>", types::as_vector_idx(v));
    if (types::is_hash(v)) return std::format("<hash[{}]>", types::as_hash_idx(v));
    if (types::is_closure(v)) return std::format("<closure[{}]>", types::as_closure_id(v));
    if (types::is_cell(v)) return std::format("<cell[{}]>", types::as_cell_id(v));
    if (types::is_primitive(v)) {
        if (primitives) {
            auto slot = types::as_primitive_slot(v);
            if (slot < primitives->slot_count())
                return std::format("<primitive:{}>", primitives->name_for_slot(slot));
        }
        return "<primitive>";
    }
    return "<unknown>";
}

// Pre-expand all macros in a FlatAST. Returns (possibly new) root.
export aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                           aura::ast::NodeId root, int max_passes = 10);

} // namespace aura::compiler
