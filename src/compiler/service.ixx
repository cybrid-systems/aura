export module aura.compiler.service;
import std;
import aura.core;
import aura.core.type;
import aura.parser.parser;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_executor;
import aura.compiler.pass_manager;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.diag;

namespace aura::compiler {

// CompilerService — owns a full compilation session's lifecycle.
//
// Each request creates a fresh AST in the arena; after eval, arena
// is reset for the next request. Evaluator state (closures, defines)
// persists across resets.
//
// For multi-module scenarios, use module_arena() to get an isolated
// arena that can be independently reset.
//
export class CompilerService {
public:
    CompilerService() {
        evaluator_.set_arena(&arena_);
        // Module caching disabled — recursive fn + bundle injection issues.
        // Python stdlib_inliner handles imports for Agent code.
        // evaluator_.set_module_loaded_callback(...)
    }

    void reset() { arena_.reset(); }

    // ---- Unified evaluation (IR-first with fallback) -----------------

    // Check if an expression needs the tree-walker evaluator.
    // IR pipeline cannot handle: EDSL primitives, quoted pairs, special forms,
    // macro definitions, error handling, or non-primitive variable references
    // (which may come from runtime imports).
    bool needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                     const aura::ast::StringPool& pool,
                                     aura::ast::NodeId root) const {
        if (root == aura::ast::NULL_NODE || root >= flat.size()) return false;

        static const std::unordered_set<std::string> special_forms = {
            "when", "unless", "try", "catch", "raise", "export",
            "and", "or", "cond", "case",
        };

        // Root-level bare variables (like `pi`, `sort`) may come from runtime imports.
        // The lowering doesn't know about them, so fallback to tree-walker.
        if (flat.get(root).tag == aura::ast::NodeTag::Variable) {
            auto root_name = pool.resolve(flat.get(root).sym_id);
            if (evaluator_.primitives().slot_for_name(std::string(root_name))
                    >= evaluator_.primitives().slot_count()
                && ir_cache_.count(std::string(root_name)) == 0)
                return true;
        }

        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto nv = flat.get(id);

            // MacroDef cannot be lowered to IR
            if (nv.tag == aura::ast::NodeTag::MacroDef) return true;



            if (nv.tag == aura::ast::NodeTag::Call) {
                auto callee = nv.child(0);
                if (callee != aura::ast::NULL_NODE && callee < flat.size()) {
                    auto callee_v = flat.get(callee);
                    if (callee_v.tag == aura::ast::NodeTag::Variable) {
                        auto name = pool.resolve(callee_v.sym_id);

                        // EDSL primitives — need evaluator state
                        if (name == "set-code" ||
                            name == "eval-current" ||
                            name == "typecheck-current" ||
                            name == "typed-mutate" ||
                            name == "rollback" ||
                            name == "mutation-log" ||
                            name == "query-mutation-log" ||
                            name.starts_with("query:") ||
                            name.starts_with("mutate:"))
                            return true;

                        // Special forms not available as primitives in IR
                        if (special_forms.count(std::string(name)))
                            return true;

                        // Import has env side-effects (binding names) that IR can't replicate
                        if (name == "import" || name == "use" || name == "require")
                            return true;

                        // Call callee that's not a known primitive or cached define
                        // may come from a runtime import — fallback to tree-walker.
                        // Only check call callee, not general variables (lambda params,
                        // let bindings are in scope during lowering).
                        if (evaluator_.primitives().slot_for_name(std::string(name))
                                >= evaluator_.primitives().slot_count()) {
                            if (ir_cache_.count(std::string(name)) == 0) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    [[nodiscard]] EvalResult eval(std::string_view input) {
        // Phase 4: parse directly into FlatAST, evaluator reads FlatAST directly.
        // Arena-allocate FlatAST/Pool so closures can reference them across calls.
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, pr.error.empty() ? "parse error" : pr.error});
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // Pre-expand all macros in this expression
        auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Check if we need the tree-walker fallback
        if (needs_tree_walker_fallback(*flat_ptr, *pool_ptr, expanded_root)) {
            return evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
        }

        // === Level 2: Type check via TypeCheckWrap pass ===
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, expanded_root, type_registry_, diags);
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError)
                    std::println(std::cerr, "type warning: {}", d.format());
            }
        }

        // Check for top-level (define ...) — cache IR + eval tree-walker for env persistence
        auto def = try_extract_define(*flat_ptr, *pool_ptr, expanded_root);
        if (def) {
            auto& [name, _body_id] = *def;
            auto result = cache_define(input, *flat_ptr, *pool_ptr, expanded_root, std::string(name));
            if (!result) return result;
            return EvalResult(types::make_void());
        }

        // ========== IR pipeline (default path for non-define expressions) ==========
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(
            *flat_ptr, *pool_ptr, arena_, cache_ptr, nullptr, &evaluator_.primitives());

        // Run passes (silent in default path — use eval_ir for debug)
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);

        if (ar.has_error()) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        last_ir_mod_ = ir_mod;

        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives());
        ir_interp.set_strategy(strategy_);

        // Set IR closure bridge: enables tree-walker primitives (map/filter/foldl)
        // to call IR-produced closures.
        evaluator_.set_closure_bridge(
            [this, &ir_interp](aura::compiler::ClosureId cid,
                                const std::vector<types::EvalValue>& args)
                -> std::optional<types::EvalValue> {
            auto snap = ir_interp.inspect_closure(cid);
            if (!snap) return std::nullopt;

            aura::compiler::Env ne;
            ne.set_primitives(&evaluator_.primitives());
            for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size(); ++i)
                ne.bind(snap->func_free_vars[i], snap->env[i]);
            for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                ne.bind(snap->func_params[i], args[i]);

            // Try fast path: bridge data from current module
            if (snap->func_id < last_ir_mod_->closure_bridge.size()) {
                auto& bd = last_ir_mod_->closure_bridge[snap->func_id];
                if (bd.flat && bd.pool) {
                    auto r = evaluator_.eval_flat(
                        *const_cast<ast::FlatAST*>(bd.flat),
                        *const_cast<ast::StringPool*>(bd.pool),
                        bd.body_id, ne);
                    if (r) return *r;
                }
            }

            // Fallback: re-parse from function_sources_ (survives arena resets)
            auto func_name = snap->func_name;
            auto src_it = function_sources_.find(func_name);
            if (src_it != function_sources_.end()) {
                auto fallback_alloc = arena_.allocator();
                auto* f_pool = arena_.create<aura::ast::StringPool>(fallback_alloc);
                auto* f_flat = arena_.create<aura::ast::FlatAST>(fallback_alloc);
                auto f_pr = aura::parser::parse_to_flat(src_it->second, *f_flat, *f_pool);
                if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                    f_flat->root = f_pr.root;
                    // The source is a (define name body) — body is child 0
                    auto define_v = f_flat->get(f_pr.root);
                    if (define_v.tag == aura::ast::NodeTag::Define && !define_v.children.empty()) {
                        auto r = evaluator_.eval_flat(
                            *f_flat, *f_pool, define_v.child(0), ne);
                        if (r) return *r;
                    }
                }
            }
            return std::nullopt;
        });

        auto result = ir_interp.execute();

        // Clear bridge after execution to avoid dangling references
        evaluator_.set_closure_bridge(aura::compiler::Evaluator::ClosureBridgeFn());

        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();
        return result;
    }

    // ---- IR pipeline ------------------------------------------------

    [[nodiscard]] EvalResult eval_ir(std::string_view input) {
        // Phase 4: parse directly into FlatAST (bypasses Expr* entirely)
        // Arena-allocate FlatAST/Pool so closures can reference them across calls.
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, pr.error});
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // IR pipeline doesn't support macros — fall back to tree-walker evaluator
        for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
            if (flat_ptr->get(id).tag == aura::ast::NodeTag::MacroDef) {
                return evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root, evaluator_.top_env());
            }
        }

        // === Phase 1: Define separation (IR caching) ===
        auto def = try_extract_define(*flat_ptr, *pool_ptr, flat_ptr->root);
        if (def) {
            auto& [name, _body_id] = *def;
            auto result = cache_define(input, *flat_ptr, *pool_ptr, flat_ptr->root, std::string(name));
            if (!result) return result;
            return EvalResult(types::make_void());
        }

        // === Normal IR path (with cache awareness) ===
        auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(*flat_ptr, *pool_ptr, arena_, cache_ptr_local, nullptr, &evaluator_.primitives());

        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;

        std::println(std::cerr, "PM: running {}->{}->{}",
                     ck.name(), ar.name(), cf.name());

        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);

        if (ar.has_error()) {
            for (auto& d : ar.result().diagnostics) {
                std::println(std::cerr, "arith: {}", d.message);
            }
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        if (cf.folded_count() > 0) {
            std::println(std::cerr, "PM: folded {} instructions", cf.folded_count());
        }

        last_ir_mod_ = ir_mod;

        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives());
        ir_interp.set_strategy(strategy_);
        auto result = ir_interp.execute();

        // Capture runtime state for --inspect
        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();

        return result;
    }
    // ---- Type checking (L6.x) ----------------------------------------

    // Run the TypeChecker on a input expression.
    // Returns a string with the inferred type or error messages.
    std::string typecheck(std::string_view input) {
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::string("parse error: ") + pr.error;
        }
        flat.root = pr.root;

        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;

        auto result = tc.infer_flat(flat, pool, pr.root, diag);

        std::string out;
        out += "type: " + treg.format_type(result) + "\n";

        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            out += "diagnostics:\n";
            for (auto& d : all_diags) {
                out += "  [" + std::to_string(static_cast<int>(d.kind))
                     + "] " + d.format() + "\n";
            }
        }

        return out;
    }

    // ---- Multi-module arena support ----------------------------------

    ast::ASTArena& module_arena(const std::string& name,
                                std::size_t initial_size = 8 * 1024 * 1024) {
        return arena_group_.module_arena(name, initial_size);
    }

    void reset_module(const std::string& name) {
        arena_group_.reset_module(name);
    }

    // ---- Diagnostics ------------------------------------------------

    ast::ArenaStats memory_stats() const {
        auto s = arena_.stats();
        s.merge(arena_group_.total_stats());
        return s;
    }

    std::vector<std::pair<std::string, ast::ArenaStats>>
    module_memory_stats() const {
        return arena_group_.module_stats();
    }

    // ---- Hot swap (M2.6) ----------------------------------------------

    EvalResult hot_swap(std::string_view new_code) {
        if (!last_ir_mod_) {
            // No cache yet — seed it with a regular eval_ir first
            return eval_ir(new_code);
        }

        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(new_code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, pr.error});
        }
        flat.root = pr.root;

        auto new_mod = aura::compiler::lower_to_ir(flat, pool, arena_, &evaluator_.primitives());

        // Hot-swap each function from new_mod into the cached module
        for (auto& new_func : new_mod.functions) {
            auto func_id = new_func.id;
            if (func_id < last_ir_mod_->functions.size()) {
                new_func.id = func_id;
                (*last_ir_mod_).functions[func_id] = std::move(new_func);
            } else {
                last_ir_mod_->functions.push_back(std::move(new_func));
            }
        }
        last_ir_mod_->entry_function_id = new_mod.entry_function_id;

        // Re-run passes on the hot-swapped module
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        ck.run(*last_ir_mod_);
        ar.run(*last_ir_mod_);
        cf.run(*last_ir_mod_);

        if (ar.has_error()) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives());
        ir_interp.set_strategy(strategy_);
        auto result = ir_interp.execute();

        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();
        return result;
    }

    // ---- Runtime reflection (M3 Phase 2) ------------------------------

    // Closures persisted from last IR execution
    std::vector<aura::compiler::ClosureSnapshot> last_closures() const {
        return last_closures_;
    }
    std::vector<aura::compiler::CellSnapshot> last_cells() const {
        return last_cells_;
    }
    const aura::compiler::EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const aura::compiler::EvalStrategy& s) { strategy_ = s; }

    // ---- Module caching (for on_module_loaded callback) ---------------

    // Parse module content and cache all top-level defines in ir_cache_.
    // Called by Evaluator after each successful module load.
    void cache_module(const std::string& content, const std::string& path) {
        // Arena-allocate flat/pool so pointers survive (bridge data references them)
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) return;
        flat_ptr->root = pr.root;

        auto& flat = *flat_ptr;
        auto& pool = *pool_ptr;

        // Macro expand
        auto expanded = aura::compiler::macro_expand_all(flat, pool, flat.root);

        // Walk top-level expressions to find (define ...) forms
        struct DefineFinder {
            aura::ast::FlatAST& flat;
            aura::ast::StringPool& pool;
            std::vector<std::pair<std::string, aura::ast::NodeId>> found;

            void walk(aura::ast::NodeId id) {
                if (id == aura::ast::NULL_NODE || id >= flat.size()) return;
                auto v = flat.get(id);
                if (v.tag == aura::ast::NodeTag::Define) {
                    auto name = pool.resolve(v.sym_id);
                    found.emplace_back(std::string(name), id);
                }
                if (v.tag == aura::ast::NodeTag::Begin) {
                    for (auto c : v.children) walk(c);
                }
            }
        };

        DefineFinder finder{flat, pool, {}};
        if (expanded != aura::ast::NULL_NODE) {
            auto expanded_v = flat.get(expanded);
            if (expanded_v.tag == aura::ast::NodeTag::Begin) {
                for (auto c : expanded_v.children)
                    finder.walk(c);
            } else {
                finder.walk(expanded);
            }
        }

        // Cache each define (IR only — tree-walker already evaluated the module)
        for (auto& [name, node_id] : finder.found) {
            if (ir_cache_.count(name)) continue;  // already cached

            // Skip value defines (e.g., (define pi 3.14)) — only cache function defines
            // A function define's body is a Lambda node
            auto define_node = flat.get(node_id);
            if (define_node.children.empty()) continue;
            auto body_node = flat.get(define_node.child(0));
            if (body_node.tag != aura::ast::NodeTag::Lambda) continue;

            // Create a temporary flat with just this define as root
            auto def_alloc = arena_.allocator();
            aura::ast::FlatAST def_flat(def_alloc);
            aura::ast::StringPool def_pool(def_alloc);

            // Re-parse just the define expression for a clean flat
            // We use define source extraction: walk the content s-exprs
            // Actually, easier: use the existing define by setting flat.root to the define node
            // lower_to_ir_with_cache starts from flat.root
            aura::ast::NodeId saved_root = flat.root;
            flat.root = node_id;

            bool is_redefine = ir_cache_.count(name) > 0;
            auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
            std::vector<std::string> cache_hits;
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(
                flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives());
            flat.root = saved_root;  // restore

            // Run passes
            {
                aura::compiler::ComputeKindWrap ck_pass;
                aura::compiler::ConstantFoldingWrap cf_pass;
                for (auto& func : ir_mod.functions) {
                    if (func.id == ir_mod.entry_function_id) continue;
                    ck_pass.compute_function(func);
                    cf_pass.fold_function(func);
                }
            }

            // Cache bundle
            std::vector<aura::ir::IRFunction> bundle;
            for (auto& func : ir_mod.functions) {
                if (func.id != ir_mod.entry_function_id)
                    bundle.push_back(std::move(func));
            }
            ir_cache_[name] = std::move(bundle);
            function_sources_[name] = content;

            for (auto& cn : cache_hits)
                record_dependency(name, cn);
            if (is_redefine)
                invalidate_function(name);
        }
    }

    // ---- Define caching (shared by eval, eval_ir, define_function) -----

    // Lower a define expression to IR, cache it, and eval tree-walker for env.
    // Returns tree-walker result (or void for success).
    EvalResult cache_define(std::string_view source,
                             aura::ast::FlatAST& flat,
                             aura::ast::StringPool& pool,
                             aura::ast::NodeId expanded_root,
                             const std::string& name_str) {
        bool is_redefine = ir_cache_.count(name_str) > 0;

        // === Level 2: Type check via TypeCheckWrap pass ===
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.check_before_lowering(flat, pool, expanded_root, type_registry_, diags);
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError)
                    std::println(std::cerr, "type warning ({}): {}", name_str, d.format());
            }
        }

        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        std::vector<std::string> cache_hits;
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), cache_bridge_ptr);

        // Run passes per-function on the new function bundle
        {
            aura::compiler::ComputeKindWrap ck_pass;
            aura::compiler::ConstantFoldingWrap cf_pass;
            for (auto& func : ir_mod.functions) {
                if (func.id == ir_mod.entry_function_id) continue;
                ck_pass.compute_function(func);
                cf_pass.fold_function(func);
            }
        }

        // Cache all non-entry functions as a bundle (preserving func id ordering)
        std::vector<aura::ir::IRFunction> bundle;
        std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
        for (auto& func : ir_mod.functions) {
            if (func.id != ir_mod.entry_function_id) {
                bundle.push_back(std::move(func));
                // Also save bridge data
                if (func.id < ir_mod.closure_bridge.size())
                    bridge_bundle.push_back(ir_mod.closure_bridge[func.id]);
                else
                    bridge_bundle.emplace_back();
            }
        }
        ir_cache_[name_str] = std::move(bundle);
        ir_cache_bridge_[name_str] = std::move(bridge_bundle);
        function_sources_[name_str] = std::string(source);

        for (auto& called_name : cache_hits) {
            record_dependency(name_str, called_name);
        }

        if (is_redefine) {
            invalidate_function(name_str);
        }

        // Eval tree-walker for persistent runtime bindings
        return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
    }

    // ---- Accessors ---------------------------------------------------

    ast::ASTArena& arena() { return arena_; }
    Evaluator& evaluator() { return evaluator_; }

    // Return current number of cached define functions
    std::size_t cached_function_count() const { return ir_cache_.size(); }

    // Check if a cached function exists
    bool has_cached_function(const std::string& name) const {
        return ir_cache_.find(name) != ir_cache_.end();
    }

    // ---- Phase 5: serve integration (define/exec JSON protocol) ----

    // Define a function: both tree-walker eval (for env persistence) and IR cache.
    // Returns the tree-walker evaluation result (for backward compat).
    // Define a function: both tree-walker eval (for env persistence) and IR cache.
    // Returns the tree-walker evaluation result (for backward compat).
    //
    // Dependency tracking: when lowering with cache, records which cached functions
    // this new definition calls. On redefinition, invalidates all transitive dependents.
    EvalResult define_function(std::string_view code) {
        // Arena-allocate FlatAST/Pool so closures can reference them across calls.
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(code, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, pr.error.empty() ? "parse error" : pr.error});
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // Check if root is a Define node
        if (flat_ptr->get(flat_ptr->root).tag == aura::ast::NodeTag::Define) {
            auto name = pool_ptr->resolve(flat_ptr->get(flat_ptr->root).sym_id);
            auto result = cache_define(code, *flat_ptr, *pool_ptr, flat_ptr->root, std::string(name));
            return result;  // tree-walker result (not void — serve protocol needs return value)
        }

        // Not a define -- just tree-walker eval
        return evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root, evaluator_.top_env());
    }

    EvalResult exec_with_cache(std::string_view code) {
        // Use tree-walker (full language support including strings, modules)
        return eval(code);
    }

    // ── Persistent AST for mutation workflows ───────────────────

    // Parse input into a persistent AST (stored in the arena).
    // Subsequent typed_mutate / query_mutation_log calls operate on this AST.
    // Call set_code() again to replace the program.
    void set_code(std::string_view input) {
        auto alloc = arena_.allocator();
        current_ast_ = arena_.create<aura::ast::FlatAST>(alloc);
        current_pool_ = arena_.create<aura::ast::StringPool>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *current_ast_, *current_pool_);
        if (pr.success && pr.root != aura::ast::NULL_NODE) {
            current_ast_->root = pr.root;
        } else {
            current_ast_ = nullptr;
            current_pool_ = nullptr;
        }
    }

    // Evaluate the persistent AST (tree-walker only).
    EvalResult eval_current() {
        if (!current_ast_ || !current_pool_ || current_ast_->root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "no code loaded — call set_code() first"});
        }
        return evaluator_.eval_flat(*current_ast_, *current_pool_,
                                     current_ast_->root, evaluator_.top_env());
    }

    // Result of a mutation operation.
    struct MutationResult {
        std::uint64_t mutation_id;
        bool success;
        std::string error;
    };

    // Mutation log entry (for JSON serialization).
    struct MutationLogEntry {
        std::uint64_t mutation_id;
        std::uint64_t timestamp_ms;
        std::uint32_t target_node;
        std::string operator_name;
        std::string old_type;
        std::string new_type;
        std::string summary;
        std::string status;  // "Committed" or "RolledBack"
    };

    // Evaluate an S-expression by parsing it INTO persistent AST (current_ast_).
    // This makes all nodes co-exist in one FlatAST, so mutation primitives
    // correctly read/write the original program's nodes.
    EvalResult eval_on_current(std::string_view sexpr) {
        if (!current_ast_ || !current_pool_) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "no AST loaded"});
        }
        auto pr = aura::parser::parse_to_flat(sexpr, *current_ast_, *current_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError,
                pr.error.empty() ? "parse error" : pr.error});
        }
        return evaluator_.eval_flat(*current_ast_, *current_pool_,
                                     pr.root, evaluator_.top_env());
    }

    // Apply a mutation expression by parsing it INTO the persistent AST.
    // Returns the mutation ID (0 on failure).
    [[nodiscard]] MutationResult typed_mutate(std::string_view sexpr) {
        if (!current_ast_ || !current_pool_) {
            return {0, false, "no AST loaded"};
        }
        auto pr = aura::parser::parse_to_flat(sexpr, *current_ast_, *current_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return {0, false, pr.error.empty() ? "parse error" : pr.error};
        }
        auto result = evaluator_.eval_flat(*current_ast_, *current_pool_,
                                            pr.root, evaluator_.top_env());
        if (!result) {
            return {0, false, result.error().message};
        }
        auto& val = *result;
        auto mid = static_cast<std::uint64_t>(
            aura::compiler::types::is_int(val) ? aura::compiler::types::as_int(val) : 0);
        return {mid, mid > 0, ""};
    }

    // Query mutation log for a specific node (or all nodes if NULL_NODE).
    std::vector<MutationLogEntry> query_mutation_log(
        aura::ast::NodeId node = aura::ast::NULL_NODE) const {
        std::vector<MutationLogEntry> result;
        if (!current_ast_) return result;
        auto hist = (node == aura::ast::NULL_NODE)
            ? current_ast_->all_mutations()
            : current_ast_->mutation_history(node);
        for (auto& rec : hist) {
            result.push_back({
                rec.mutation_id,
                rec.timestamp_ms,
                rec.target_node,
                rec.operator_name,
                rec.old_type_str,
                rec.new_type_str,
                rec.summary,
                rec.status == aura::ast::MutationStatus::Committed ? "Committed" : "RolledBack"
            });
        }
        return result;
    }

    // Get the current persistent AST (for direct inspection).
    aura::ast::FlatAST* current_ast() const { return current_ast_; }
    aura::ast::StringPool* current_pool() const { return current_pool_; }

private:
    // Try to extract a define/let/letrec binding from the FlatAST root.
    // Returns {name, body_node_id} if root is a Define node.
    static std::optional<std::pair<std::string, aura::ast::NodeId>>
    try_extract_define(aura::ast::FlatAST& flat,
                       aura::ast::StringPool& pool,
                       aura::ast::NodeId root) {
        if (root == aura::ast::NULL_NODE) return std::nullopt;
        auto v = flat.get(root);
        if (v.tag == aura::ast::NodeTag::Define) {
            auto name = pool.resolve(v.sym_id);
            aura::ast::NodeId body = v.children.empty()
                ? aura::ast::NULL_NODE : v.child(0);
            return std::make_pair(std::string(name), body);
        }
        return std::nullopt;
    }

    // IR function cache: name → bundle of IR functions for cached defines.
    // The LAST function in the bundle is the user-defined lambda itself.
    // When inlined, all functions are added to the current module in order,
    // preserving func id references across cached calls.
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>> ir_cache_;

    // Bridge data cached alongside ir_cache_ (same keys, parallel indices).
    std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>> ir_cache_bridge_;

    // Source code for each cached function, used for re-lowering on dependency changes.
    std::unordered_map<std::string, std::string> function_sources_;

    // Dependency tracking for incremental compilation.
    // DepEntry.calls = functions this one calls; DepEntry.called_by = functions that call this one.
    // When a function is redefined, all transitively dependent functions are invalidated.
    struct DepEntry {
        std::vector<std::string> calls;
        std::vector<std::string> called_by;
    };
    std::unordered_map<std::string, DepEntry> dep_graph_;

    void record_dependency(const std::string& caller, const std::string& callee) {
        dep_graph_[caller].calls.push_back(callee);
        dep_graph_[callee].called_by.push_back(caller);
    }

    // Scan FlatAST from the given node for Variable nodes that reference cached functions.
    // Records these as dependencies of `def_name`.
    void track_define_dependencies(const std::string& def_name,
                                   aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool) {
        if (ir_cache_.empty()) return;

        struct DepWalker {
            const std::string& def_name;
            aura::ast::FlatAST& flat;
            aura::ast::StringPool& pool;
            CompilerService* self;

            void walk(aura::ast::NodeId id) {
                if (id == aura::ast::NULL_NODE || id >= flat.size()) return;
                auto nv = flat.get(id);
                if (nv.tag == aura::ast::NodeTag::Variable) {
                    auto name = pool.resolve(nv.sym_id);
                    auto name_str = std::string(name);
                    // Don't record self-reference
                    if (name_str != def_name && self->ir_cache_.count(name_str)) {
                        // Check if we already recorded this dep
                        auto& calls = self->dep_graph_[def_name].calls;
                        if (std::find(calls.begin(), calls.end(), name_str) == calls.end()) {
                            self->record_dependency(def_name, name_str);
                        }
                    }
                }
                for (auto c : nv.children) walk(c);
            }
        };

        DepWalker{def_name, flat, pool, this}.walk(flat.root);
    }

    // Invalidate a function and all its transitive dependents (called_by chain).
    // Instead of removing from cache, re-lowers each dependent with the current cache
    // so they stay resolvable in the IR pipeline with updated dependencies.
    void invalidate_function(const std::string& name) {
        // BFS to find all transitively dependent functions
        std::vector<std::string> dependents;
        std::vector<std::string> queue;
        std::unordered_set<std::string> visited;

        queue.push_back(name);
        visited.insert(name);

        while (!queue.empty()) {
            auto current = queue.back();
            queue.pop_back();

            auto it = dep_graph_.find(current);
            if (it == dep_graph_.end()) continue;

            for (auto& dependent : it->second.called_by) {
                if (visited.count(dependent)) continue;
                visited.insert(dependent);
                dependents.push_back(dependent);
                queue.push_back(dependent);
            }
        }

        // Debug: check if any dependents found
        if (dependents.empty()) {
            // No dependents, nothing to re-lower
        }

        // Clean up old dependency info for all affected functions
        // (the redefined function and all its transitives)
        for (auto& f : dependents) {
            auto fit = dep_graph_.find(f);
            if (fit != dep_graph_.end()) {
                for (auto& callee : fit->second.calls) {
                    auto& cb = dep_graph_[callee].called_by;
                    cb.erase(std::remove(cb.begin(), cb.end(), f), cb.end());
                }
                dep_graph_.erase(f);
            }
        }
        // Clean up the original function's dep info
        auto it = dep_graph_.find(name);
        if (it != dep_graph_.end()) {
            for (auto& callee : it->second.calls) {
                auto& cb = dep_graph_[callee].called_by;
                cb.erase(std::remove(cb.begin(), cb.end(), name), cb.end());
            }
            dep_graph_.erase(name);
        }

            // Re-lower each dependent with current cache (nearest to redefined first = natural BFS order)
        for (auto& dep_name : dependents) {
            auto src_it = function_sources_.find(dep_name);
            if (src_it == function_sources_.end()) continue;

            // Re-parse the function source
            auto alloc = arena_.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE) continue;
            flat.root = pr.root;

            // Re-lower with current cache to detect new dependencies
            auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
            std::vector<std::string> cache_hits;
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives());

            // Phase 4: Run passes per-function on the re-lowered function bundle.
            {
                ComputeKindWrap ck_pass;
                ConstantFoldingWrap cf_pass;
                for (auto& func : ir_mod.functions) {
                    if (func.id == ir_mod.entry_function_id) continue;
                    ck_pass.compute_function(func);
                    auto nf = cf_pass.fold_function(func);
                    if (nf > 0) {
                        std::println(std::cerr, "PM: folded {} instructions in function '{}'",
                                     nf, func.name);
                    }
                }
            }

            // Update cache with new IR (store full bundle)
            std::vector<aura::ir::IRFunction> bundle;
            for (auto& func : ir_mod.functions) {
                if (func.id != ir_mod.entry_function_id) {
                    bundle.push_back(std::move(func));
                }
            }
            ir_cache_[dep_name] = std::move(bundle);

            // Re-record dependencies
            for (auto& called_name : cache_hits) {
                record_dependency(dep_name, called_name);
            }
        }
    }

    ast::ASTArena arena_;
    ast::ArenaGroup arena_group_;
    Evaluator evaluator_;
    aura::compiler::EvalStrategy strategy_;
    aura::core::TypeRegistry type_registry_;  // persistent type registry (L6)
    std::vector<aura::compiler::ClosureSnapshot> last_closures_;
    std::vector<aura::compiler::CellSnapshot> last_cells_;
    std::optional<aura::ir::IRModule> last_ir_mod_;

    // Persistent AST for mutation workflows (set_code / typed_mutate).
    aura::ast::FlatAST* current_ast_ = nullptr;
    aura::ast::StringPool* current_pool_ = nullptr;
};

} // namespace aura::compiler
