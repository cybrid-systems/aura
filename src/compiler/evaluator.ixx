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
    // Return cell index (stable across vector reallocation) or nullopt if not a cell
    std::optional<std::uint64_t> lookup_cell_index(const std::string& n) const;
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
    // Issue #67: destructor walks modules_ and runs each env's
    // destructor to free their std::vector bindings_ heap allocations.
    // Without this, arena-allocated Envs leak at process exit (the
    // arena's bump-allocator doesn't run destructors).
    ~Evaluator();
    void set_arena(ast::ASTArena* a) { arena_ = a; }
    void set_temp_arena(ast::ASTArena* a) { temp_arena_ = a; }
    // Hot-swap callback (Issue #97 Action 1). Set by CompilerService
    // to enable the (hot-swap:fn "name" "new-source") primitive.
    // Returns true on success.
    using HotSwapFn = std::function<bool(const std::string& name,
                                        const std::string& new_source)>;
    void set_hot_swap_fn(HotSwapFn fn) { hot_swap_fn_ = std::move(fn); }
    // Per-module arena group: load_module_file allocates each module's
    // StringPool/FlatAST/mod_env in a dedicated arena so the whole module
    // can be freed in one shot via reset_module(path).
    ast::ArenaGroup& arena_group() { return *arena_group_; }
    const ast::ArenaGroup& arena_group() const { return *arena_group_; }
    // Free a module's arena + all closures it owns. Returns false if
    // the module is not in the cache.
    bool gc_module(const std::string& path);
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
    const std::vector<types::EvalValue>& cells() const { return cells_; }
    std::vector<types::EvalValue>& cells() { return cells_; }
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

    // Mutation typecheck error state (P2 #34)
    const std::string& last_mutate_error() const { return last_mutate_error_; }
    void clear_last_mutate_error() { last_mutate_error_.clear(); }
    bool has_type_error() const { return !last_mutate_error_.empty(); }

    // ── Panic auto-rollback (Issue #39) ─────────────────────
    // When enabled, runtime errors after mutations automatically
    // restore the workspace to the last known good state.
    void set_auto_rollback_on_panic(bool v) { panic_auto_rollback_ = v; }
    bool auto_rollback_on_panic() const { return panic_auto_rollback_; }

    // Save current source as a safe checkpoint. Returns true if saved.
    // Call before a mutation to ensure we can rollback on failure.
    bool save_panic_checkpoint();

    // Restore to the last safe checkpoint (ast:restore-like).
    // Returns true if restore was performed.
    bool restore_panic_checkpoint();

    // Clear the checkpoint (call after successful mutation commit).
    void commit_panic_checkpoint() { panic_safe_source_.clear(); }

    // Check if a safe checkpoint exists.
    bool has_panic_checkpoint() const { return !panic_safe_source_.empty(); }

    // Get the safe checkpoint source (for introspection).
    const std::string& panic_safe_source() const { return panic_safe_source_; }

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
    // Forward decl: MemoryPolicy is defined further down in the class
    // (after the long state-decl block). The build_policy_hash helper
    // (which needs it as a parameter) is also defined further down.
    struct MemoryPolicy;
    // Build a 6-key policy hash (for set-memory-policy and get-memory-policy).
    // Member function (not a local lambda) so it has proper lifetime when
    // captured by std::function in the primitive table. A captured local
    // lambda would dangle as soon as the enclosing function returns.
    [[nodiscard]] types::EvalValue build_policy_hash(const MemoryPolicy& p);
    // (apply_closure and expand_macro removed — use eval_flat directly)
    [[nodiscard]] EvalValue ast_to_data(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId nid);
    [[nodiscard]] ast::NodeId data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool, int depth = 0);
    [[nodiscard]] EvalResult eval_data_as_code(const types::EvalValue& data, const Env& env,
                                               aura::ast::FlatAST* flat = nullptr,
                                               aura::ast::StringPool* pool = nullptr);
    Env* copy_env(const Env& env, ast::ASTArena* target = nullptr)
        pre (target != nullptr);  // arena_ is private; impl also asserts via contract_assert
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
    // Owned multi-arena manager. Created in ctor so load_module_file can
    // hand each module its own arena without depending on a caller setting
    // it up. Lives for the Evaluator's whole lifetime.
    std::unique_ptr<ast::ArenaGroup> arena_group_;
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
    std::unordered_map<std::string, ast::ASTArena*> module_arena_ptrs_; // path → owning arena (for gc_module)
    std::vector<types::EvalValue> cells_;
    std::vector<Pair> pairs_;
    std::vector<types::EvalValue> error_values_; // error cause values (indexed by ErrorRef)
    std::vector<void*> opaque_heap_;             // opaque pointers (indexed by OpaqueRef)
    std::unique_ptr<std::unordered_set<std::string>> current_export_set_;
    // ── Strategy storage (E2) ──────────────────────────────────
    // Issue #63 Phase 3: extend with tunable fields.
    // `max_attempts_set`/`temperature_set` distinguish "not specified"
    // (-1 sentinel) from "explicitly 0" (also possible after evolve).
    struct StrategyDef {
        std::string name;
        std::string body; // strategy body as S-expression string
        int max_attempts = 3;        // tunable: 1..20
        double temperature = 0.3;    // tunable: 0.0..1.0
        std::string sys_prompt_template; // tunable: free-form
        int evolution = 0;           // generation counter
        std::string parent;         // parent strategy name
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

    // Hot-swap callback storage (Issue #97 Action 1)
    HotSwapFn hot_swap_fn_;

    // ── Auto-evolve state (Issue #97 Action 2) ──────────────
    // Background loop state for (auto-evolve-loop ...).
    // The two Aura callbacks (detect-fn, fix-fn) are stored as closure IDs
    // and invoked via apply_closure.
    bool auto_evolve_running_ = false;
    double auto_evolve_interval_ = 1.0;  // seconds between cycles
    std::uint64_t auto_evolve_detect_closure_ = 0;  // 0 = unset
    std::uint64_t auto_evolve_fix_closure_ = 0;
    std::uint64_t auto_evolve_cycle_count_ = 0;
    std::uint64_t auto_evolve_total_fixed_ = 0;

    // ── Panic auto-rollback (Issue #39) ─────────────────────────
    bool panic_auto_rollback_ = false;
    std::string panic_safe_source_;  // last known good source code

    // ── EDSL set-code error propagation ──────────────────────────
    // Stores (kind, message) for structured diagnostic return
    std::string last_set_code_error_kind_;
    std::string last_set_code_error_msg_;

    // Last mutate typecheck error (empty = no error). Set by auto-typecheck
    // after mutate:rebind etc. Cleared on next successful mutate.
    std::string last_mutate_error_;

    // ── Incremental eval cache ───────────────────────────────────
    // Caches the last eval-current result. Cleared when workspace dirty flags
    // are set (which happens on any mutation). This lets eval-current skip
    // full re-evaluation when nothing has changed.
    std::optional<types::EvalValue> last_eval_current_result_;

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
    // Short string cache: ≤6 byte strings are deduplicated via this hash
    // (avoids redundant string_heap_ pushes and enables faster equal?)
    std::unordered_map<std::string, types::EvalValue> short_str_cache_;
    std::vector<std::string> keyword_table_; // keyword name strings (indexed by KeywordRef)
    std::size_t eval_depth_ = 0; // recursion counter for friendly stack overflow
    static constexpr std::size_t MAX_EVAL_DEPTH = 50000;

    // ── Memory pressure observability (Issue #69) ───────────────
    // eval_depth_ snapshot at the last (gc-temp) call. Used by
    // memory-pressure to decide whether to suggest "gc-temp" in the
    // suggestions vector (only if no recent gc-temp call, i.e.
    // eval_depth_ - last_gc_temp_eval_depth_ > 100).
    std::size_t last_gc_temp_eval_depth_ = 0;

    // ── Memory pressure governance (P1) ─────────────────────
    // Policy is configured by (set-memory-policy hash). Default: no
    // auto-gc, warn at 80%, critical at 95%, sample every 1000 evals,
    // cooldown 5000 evals between auto-gc firings, gc-temp "recent"
    // window 100 evals. The auto-gc fires (gc-module top-arena) when
    // overall used-pct >= critical-pct AND the cooldown has elapsed
    // since the last auto-gc.
    struct MemoryPolicy {
        bool auto_gc = false;
        int warn_pct = 80;
        int critical_pct = 95;
        std::size_t sample_every = 1000;
        std::size_t cooldown_evals = 5000;
        std::size_t recent_gc_temp_window = 100;
    } memory_policy_;
    // Last eval_depth_ at which auto-gc fired (for cooldown).
    std::size_t last_auto_gc_eval_depth_ = 0;
    // Counter to implement "sample every N evals".
    std::size_t sample_counter_ = 0;
    // Last logged warning level (so we don't spam the same warning).
    std::string last_warn_level_;

    std::vector<std::vector<types::EvalValue>> vector_heap_;
    std::uint64_t next_id_ = 1;
    ClosureId gc_safe_closure_id_ = 0;
    // Issue #68: depth counter (was bool) so nested `intend` calls
    // correctly keep outer closures in the persistent arena. With
    // bool, an outer intend's closures could be allocated in the
    // temp arena when an inner intend flipped the flag, then
    // collected by gc-temp. With a counter, only depth > 0 (i.e.
    // inside at least one intend) routes to temp arena, and the
    // outer intend's depth-1 boundary still goes to persistent.
    int in_task_context_ = 0;

    // ── Capability 上下文栈 ─────────────────────────────────────
    // 每层包含当前作用域允许的 effect 名称列表
    std::vector<std::vector<std::string>> capability_stack_;

    // ── Concurrent Channels (fiber-safe message passing) ─────
    struct Channel {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::size_t buffer_size = 0; // 0 = rendezvous (unbuffered)
        bool closed = false;
    };
    std::vector<std::shared_ptr<Channel>> channels_;
    std::mutex channels_mtx_;

    // ── Heap mutex (P2 thread-safe GC) ────────────────────────
    // Protects string_heap_, pairs_, closures_, cells_,
    // vector_heap_, opaque_heap_, error_values_.
    // Locked during gc-heap and gc-temp operations.
    std::mutex heap_mtx_;
    std::mutex& heap_mutex() { return heap_mtx_; }
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
    // IMPORTANT: Check is_string BEFORE is_keyword (Issue #96 bug fix).
    // The STRING_BIAS - idx encoding can produce bit patterns that
    // overlap with RefKeyword (every 64th idx starting at 19).
    if (types::is_string(v)) {
        if (heap) {
            auto idx = types::as_string_idx(v);
            if (idx < heap->size())
                return std::format("\"{}\"", (*heap)[idx]);
        }
        return std::format("<string[{}]>", types::as_string_idx(v));
    }
    if (types::is_keyword(v)) {
        auto kidx = types::as_keyword_idx(v);
        if (keywords && kidx < keywords->size())
            return (*keywords)[kidx];
        return ":" + std::to_string(kidx);
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
