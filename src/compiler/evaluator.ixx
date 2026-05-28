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
    explicit Env(const Env* p)
        : parent_(p) {}
    Env(const Env&) = default;
    Env& operator=(const Env&) = default;
    void set_parent(const Env* p) { parent_ = p; }
    void set_primitives(const Primitives* p) { primitives_ = p; }
    void set_cells(std::vector<types::EvalValue>* c) { cells_ = c; }
    void bind(const std::string& n, types::EvalValue v) { bindings_.emplace_back(n, std::move(v)); }
    [[nodiscard]] std::optional<types::EvalValue> lookup(const std::string& n) const;
    // Look up the raw binding without dereferencing cells (returns cell sentinel as-is)
    std::optional<types::EvalValue> lookup_binding(const std::string& n) const;
    std::optional<PrimFn> lookup_primitive(const std::string& n) const {
        return primitives_ ? primitives_->lookup(n) : std::nullopt;
    }
    types::EvalValue* lookup_cell_ptr(const std::string& n,
                                      std::vector<types::EvalValue>* cells) const;
    const Env* parent() const { return parent_; }
    std::vector<std::pair<std::string, types::EvalValue>>& bindings() { return bindings_; }
    const std::vector<std::pair<std::string, types::EvalValue>>& bindings() const {
        return bindings_;
    }

private:
    const Env* parent_ = nullptr;
    const Primitives* primitives_ = nullptr;
    std::vector<types::EvalValue>* cells_ = nullptr;
    std::vector<std::pair<std::string, types::EvalValue>> bindings_;
};

export using ClosureId = std::uint64_t;

export struct Pair {
    types::EvalValue car;
    types::EvalValue cdr;
};

export struct MacroDef {
    std::vector<std::string> params;
    bool dotted = false;
    ast::FlatAST* flat = nullptr;
    ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
};

export struct Closure {
    std::string name = "";  // function name (empty for lambdas)
    std::vector<std::string> params;
    ast::FlatAST* flat = nullptr;
    ast::StringPool* pool = nullptr;
    ast::NodeId body_id = ast::NULL_NODE;
    const Env* env = nullptr;
    bool dotted = false;
    ast::ASTArena* owner_arena = nullptr;  // arena where flat/pool/env lives
};

export using EvalResult = std::expected<types::EvalValue, aura::diag::Diagnostic>;

export class Evaluator {
public:
    Evaluator();
    void set_arena(ast::ASTArena* a) { arena_ = a; }
    void set_temp_arena(ast::ASTArena* a) { temp_arena_ = a; }
    // Set current FlatAST/Pool for mutation primitives
    void set_flat_pool(ast::FlatAST* f, ast::StringPool* p) {
        current_flat_ = f;
        current_pool_ = p;
    }
    [[nodiscard]] EvalResult eval_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                       aura::ast::NodeId id, const Env& env);
    const Primitives& primitives() const { return primitives_; }
    Primitives& primitives() { return primitives_; }
    const Env& top_env() const { return top_; }
    Env& top_env() { return top_; }
    const std::vector<Pair>& pairs() const { return pairs_; }
    const std::vector<std::string>& keyword_table() const { return keyword_table_; }

    // IR closure bridge: called when a closure id is not in closures_.
    using ClosureBridgeFn = std::function<std::optional<EvalValue>(
        ClosureId closure_id, const std::vector<EvalValue>& args)>;

    // Set the IR closure bridge for cross-evaluator closure calls
    void set_closure_bridge(ClosureBridgeFn bridge) { closure_bridge_ = std::move(bridge); }

    // Look up a closure and apply it with given args.
    // Tries closures_ first, then IR bridge.
    std::optional<EvalValue> apply_closure(ClosureId cid, const std::vector<EvalValue>& args);

    // Module loaded callback: called after a module file is successfully loaded.
    using ModuleLoadedFn = std::function<void(const std::string& source, const std::string& path)>;

    void set_module_loaded_callback(ModuleLoadedFn cb) { module_loaded_cb_ = std::move(cb); }
    void set_type_registry(void* reg) { type_registry_ = reg; }
    void set_compiler_service(void* svc) { compiler_service_ = svc; }
    void set_session_id(const std::string& id) { session_id_ = id; }

    // Set/get a shared workspace tree (for cross-session workspace sharing in serve mode).
    void set_workspace_tree(void* wt) { workspace_tree_ = wt; }
    void* workspace_tree() const { return workspace_tree_; }
    // Update the shared tree's root node to point to this evaluator's workspace.
    void update_shared_tree_root();

    // Create a new workspace tree with a root node (for serve mode sharing).
    // The caller owns the returned pointer.
    static void* create_workspace_tree();

    // Free a workspace tree created by create_workspace_tree().
    static void destroy_workspace_tree(void* wt);
    const std::string& session_id() const { return session_id_; }
    void set_messaging_callbacks(
        std::function<bool(const std::string&, const std::string&)>* send_fn,
        std::function<std::optional<std::string>(int)>* recv_fn,
        std::function<std::string()>* id_fn) {
        msg_send_fn_ = send_fn;
        msg_recv_fn_ = recv_fn;
        msg_id_fn_ = id_fn;
    }

private:
    ClosureId next_id() { return next_id_++; }
    [[nodiscard]] std::size_t alloc_cell(const types::EvalValue& v) {
        cells_.push_back(v);
        return cells_.size() - 1;
    }
    // (apply_closure and expand_macro removed — use eval_flat directly)
    [[nodiscard]] EvalValue ast_to_data(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId nid);
    [[nodiscard]] ast::NodeId data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool, int depth = 0);
    [[nodiscard]] EvalResult eval_data_as_code(const types::EvalValue& data, const Env& env,
                                               aura::ast::FlatAST* flat = nullptr,
                                               aura::ast::StringPool* pool = nullptr);
    Env* copy_env(const Env& env, ast::ASTArena* target = nullptr);
    void init_pair_primitives();
    void build_primitive_slots();
    // Load a module file, return module object (or void on failure)
    types::EvalValue load_module_file(const std::string& path);
    // Resolve a module path (supports AURA_PATH, .aura extension)
    std::string resolve_module_path(const std::string& path) const;
    Env top_;
    Primitives primitives_;
    ast::ASTArena* arena_ = nullptr;
    ast::ASTArena* temp_arena_ = nullptr;
    ast::FlatAST* current_flat_ = nullptr;
    ast::StringPool* current_pool_ = nullptr;
    ast::FlatAST* workspace_flat_ = nullptr;
    ast::StringPool* workspace_pool_ = nullptr;
    void* type_registry_ = nullptr; // points to aura::core::TypeRegistry
    std::unordered_map<ClosureId, Closure> closures_;
    ClosureBridgeFn closure_bridge_;
    ModuleLoadedFn module_loaded_cb_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::vector<Env*> modules_; // module objects (arena-allocated, indexed by ModuleRef.index)
    std::unordered_map<std::string, std::uint64_t> module_cache_; // path → index
    std::unordered_set<std::string> loading_stack_;               // circular dep detection
    std::vector<std::string> module_names_;                       // display names for modules
    std::vector<types::EvalValue> cells_;
    std::vector<Pair> pairs_;
    std::vector<types::EvalValue> error_values_; // error cause values (indexed by ErrorRef)
    std::vector<void*> opaque_heap_;             // opaque pointers (indexed by OpaqueRef)
    std::unique_ptr<std::unordered_set<std::string>> current_export_set_;
    // ── Strategy storage (E2) ──────────────────────────────────
    struct StrategyDef {
        std::string name;
        std::string body; // strategy body as S-expression string
    };
    std::vector<StrategyDef> strategies_;
    // ── Intend history (E4 Phase 1) ────────────────────────────
    struct IntendRecord {
        std::uint64_t record_id;
        std::string strategy_name;
        std::string task_desc;
        bool success;
        int attempts;
        std::vector<std::string> errors;
        std::vector<std::string> error_types;
        std::vector<std::string> generated_codes;
        std::uint64_t llm_call_count;
        std::uint64_t llm_tokens;
        std::uint64_t duration_ms;
        std::uint64_t timestamp;
        std::uint64_t parent_record_id;
    };
    std::vector<IntendRecord> intend_history_;
    std::uint64_t next_record_id_ = 1;
    static constexpr std::size_t MAX_HISTORY_SIZE = 1000;
    // ── Coverage counters (fuzz Phase 3) ──────────────────────
    // 0=parser, 1=typecheck, 2=eval, 3=jit, 4=macro, 5=edsl-set-code,
    // 6=edsl-query, 7=edsl-mutate, 8=ffi, 9-15=reserved
    std::array<std::uint64_t, 16> coverage_counters_ = {};
    // ── Workspace Tree (P13) ───────────────────────────────────
    void* workspace_tree_ = nullptr;  // WorkspaceTree*
    bool workspace_read_only_ = false;  // quick lock flag for P6 mutations
    // ── CompilerService pointer (for messaging) ─────────────────
    void* compiler_service_ = nullptr;  // CompilerService*
    // Function pointer callbacks (set by CompilerService to avoid circular deps)
    std::function<bool(const std::string&, const std::string&)>* msg_send_fn_ = nullptr;
    std::function<std::optional<std::string>(int)>* msg_recv_fn_ = nullptr;
    std::function<std::string()>* msg_id_fn_ = nullptr;
    std::string session_id_;  // from CompilerService (for my-id)
    // ── Snapshot storage (ast:snapshot / ast:restore) ───────────
    std::vector<std::string> snapshot_sources_;  // source code per snapshot
    std::vector<std::string> snapshot_names_;    // optional names

    // ── EDSL set-code error propagation ──────────────────────────
    // Stores (kind, message) for structured diagnostic return
    std::string last_set_code_error_kind_;
    std::string last_set_code_error_msg_;

    // ── Def-Use Analysis (P1) ───────────────────────────────────
    void* defuse_index_ = nullptr;
    std::uint64_t defuse_version_ = 0;  // incremented on each mutation
    // (#10) Track mutation-affected symbols for targeted index rebuild
    // Mutation primitives push affected sym names here; ensure_defuse
    // uses them to avoid full rebuild when only a few symbols changed.
    std::unordered_set<std::string> defuse_affected_syms_;
    // (#10) Number of times the def-use index has been rebuilt (for stats)
    std::uint64_t defuse_rebuild_count_ = 0;

    // ── 依赖图查询回调 ─────────────────────────────────────────
    // 在 mutation 原语中查询调用者节点，绕开 DefUseIndex 前向声明问题。
    // 在 init_pair_primitives 末尾（DefUseIndex 定义完成后）注册。
    // 签名: (defuse_index, sym_id) → [caller node IDs]
    // 用 std::function 而非函数指针，避免不完整类型问题。
    std::function<std::vector<aura::ast::NodeId>(void*, aura::ast::SymId)>
        dep_caller_fn_ = nullptr;

    // ── 模块类型签名（#8 跨模块类型检查） ──────────────────────
    // (declare-type "name" "param-types" "ret-type") 存储的签名，
    // 在 typecheck-current 时注入到类型环境中。
    // 格式: type_str = "param1 param2|rettype" | 分隔
    struct DeclaredType {
        std::string type_str;
        std::string module_file;  // 来源模块文件（用于跨模块错误定位）
        bool resolved = false;
    };
    std::unordered_map<std::string, DeclaredType> declared_type_sigs_;

    // ── Functor 泛型模块模板 ────────────────────────────────────
    struct ModuleTemplate {
        std::string body_source;                       // body source code (re-parsed at instantiation)
        std::vector<std::string> type_param_names;     // type parameter names (e.g., ["T", "K"])
        std::vector<std::string> cap_param_names;      // capability parameter names (e.g., ["cap"])
        std::vector<std::string> cap_require;           // required capabilities (e.g., ["FileRead", "FileWrite"])
    };
    std::unordered_map<std::string, ModuleTemplate> module_templates_;

    // ── Functor 实例化缓存 ──────────────────────────────────────
    // key = "template_name|arg1|arg2|..."
    // value = 实例化后的 env（指针通过 module 索引引用）
    std::unordered_map<std::string, std::uint64_t> functor_instance_cache_;

    // ── Timeline for intend (E2, backward compat) ───────────────
    std::vector<std::string> timeline_; //
    std::vector<std::string> string_heap_;
    std::vector<std::string> keyword_table_; // keyword name strings (indexed by KeywordRef)
    std::size_t eval_depth_ = 0; // recursion counter for friendly stack overflow
    static constexpr std::size_t MAX_EVAL_DEPTH = 50000;
    struct HashTable {
        std::vector<std::uint8_t> metadata; // 0xFF=empty, 0x00-0x7F=occupied(7-bit fingerprint)
        std::vector<types::EvalValue> keys;
        std::vector<types::EvalValue> values;
        std::size_t size = 0;     // live entries
        std::size_t capacity = 0; // power of 2
    };
    std::vector<HashTable> hash_heap_;
    std::vector<std::vector<types::EvalValue>> vector_heap_;
    std::uint64_t next_id_ = 1;
    ClosureId gc_safe_closure_id_ = 0;
    bool in_task_context_ = false;

    // ── Capability 上下文栈 ─────────────────────────────────────
    // 每层包含当前作用域允许的 effect 名称列表
    std::vector<std::vector<std::string>> capability_stack_;
};


// Pair-aware value formatting (recursively prints lists)
export inline std::string format_value(const types::EvalValue& v,
                                       const std::vector<std::string>* heap,
                                       const std::vector<Pair>* pairs, int depth = 0,
                                       const Primitives* primitives = nullptr,
                                       const std::vector<std::string>* keywords = nullptr) {
    const int max_depth = 64;
    if (depth > max_depth)
        return "...";
    if (types::is_void(v))
        return "()";
    if (types::is_bool(v))
        return types::as_bool(v) ? "#t" : "#f";
    if (types::is_int(v))
        return std::to_string(types::as_int(v));
    if (types::is_float(v))
        return std::to_string(types::as_float(v));
    if (types::is_keyword(v)) {
        auto kidx = types::as_keyword_idx(v);
        if (keywords && kidx < keywords->size())
            return (*keywords)[kidx];
        return ":" + std::to_string(kidx);
    }
    if (types::is_string(v)) {
        if (heap) {
            auto idx = types::as_string_idx(v);
            if (idx < heap->size())
                return std::format("\"{}\"", (*heap)[idx]);
        }
        return std::format("<string[{}]>", types::as_string_idx(v));
    }
    if (types::is_pair(v) && pairs) {
        auto idx = types::as_pair_idx(v);
        if (idx >= pairs->size())
            return std::format("<pair[{}]>", idx);

        // Walk the cdr chain to collect all elements
        std::vector<std::string> elements;
        auto current = v;

        while (types::is_pair(current)) {
            auto cidx = types::as_pair_idx(current);
            if (cidx >= pairs->size()) {
                break;
            }
            elements.push_back(
                format_value((*pairs)[cidx].car, heap, pairs, depth + 1, primitives, keywords));
            current = (*pairs)[cidx].cdr;
            if (elements.size() > 256) {
                elements.push_back("...");
                break;
            }
        }

        std::string result = "(";
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (i > 0)
                result += " ";
            result += elements[i];
        }
        // Check end-of-list: void sentinel (11) or fixnum 0 are list terminators
        bool is_proper = (current.val == 11) || (current.val == 0) ||
                         (types::is_int(current) && types::as_int(current) == 0);
        if (is_proper) {
            // proper list
        } else {
            if (!elements.empty())
                result += " . ";
            result += format_value(current, heap, pairs, depth + 1, primitives, keywords);
        }
        result += ")";
        return result;
    }
    if (types::is_vector(v))
        return std::format("<vector[{}]>", types::as_vector_idx(v));
    if (types::is_hash(v))
        return std::format("<hash[{}]>", types::as_hash_idx(v));
    if (types::is_closure(v)) {
        std::println("⚠ program returned an uncalled function");
        return "#<procedure>";
    }
    if (types::is_cell(v))
        return std::format("<cell[{}]>", types::as_cell_id(v));
    if (types::is_primitive(v)) {
        if (primitives) {
            auto slot = types::as_primitive_slot(v);
            if (slot < primitives->slot_count())
                return std::format("<primitive:{}>", primitives->name_for_slot(slot));
        }
        return "<primitive>";
    }
    if (types::is_module(v))
        return "<module>";
    if (types::is_error(v))
        return "<error>";
    return "<unknown>";
}

// Pre-expand all macros in a FlatAST. Returns (possibly new) root.
export aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeId root, int max_passes = 10);

} // namespace aura::compiler
