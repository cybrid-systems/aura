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
    }

    void reset() { arena_.reset(); }

    // ---- Tree-walker evaluation --------------------------------------

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
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;
        // Pre-expand all macros in this expression
        auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
        return evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
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
        // Check if this expression is a define binding
        auto def = try_extract_define(*flat_ptr, *pool_ptr, flat_ptr->root);
        if (def) {
            auto& [name, _body_id] = *def;
            auto name_str = std::string(name);

            // Check if this is a redefinition before caching the new version
            bool is_redefine = ir_cache_.count(name_str) > 0;

            // Lower WITH cache so that the Variable handler creates MakeClosure
            // for cached dependencies (with proper func id remapping).
            auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
            std::vector<std::string> cache_hits;
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(*flat_ptr, *pool_ptr, arena_, cache_ptr_local, &cache_hits);

            // Phase 4: Run passes per-function on the new function bundle.
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

            // Extract any non-entry functions (lambda bodies) from the module
            // and store as a bundle (preserving func ids for dependent fns).
            std::vector<aura::ir::IRFunction> bundle;
            for (auto& func : ir_mod.functions) {
                if (func.id != ir_mod.entry_function_id) {
                    bundle.push_back(std::move(func));
                }
            }
            ir_cache_[name_str] = std::move(bundle);

            // Store source for re-lowering on dependency changes
            function_sources_[name_str] = std::string(input);

            // Record dependencies from cache hits detected during lowering
            for (auto& called_name : cache_hits) {
                record_dependency(name_str, called_name);
            }

            // If redefining, invalidate dependents AFTER new function is cached,
            // so they get re-lowered with the updated version
            if (is_redefine) {
                invalidate_function(name_str);
            }

            // Also evaluate via tree-walker for persistent runtime bindings
            // (no Expr* reconstruction needed -- eval_flat reads FlatAST directly)
            auto result = evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root, evaluator_.top_env());
            if (!result) return result;

            // Return void for define (no visible result)
            return EvalResult(types::make_void());
        }

        // === Normal IR path (with cache awareness) ===
        auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(*flat_ptr, *pool_ptr, arena_, cache_ptr_local);

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

        auto new_mod = aura::compiler::lower_to_ir(flat, pool, arena_);

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
            // Extract name for caching
            auto name = pool_ptr->resolve(flat_ptr->get(flat_ptr->root).sym_id);
            auto name_str = std::string(name);

            // Check if this is a redefinition before caching the new version
            bool is_redefine = ir_cache_.count(name_str) > 0;

            // Lower WITH cache so that the Variable handler creates MakeClosure
            // for cached dependencies (with proper func id remapping).
            auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
            std::vector<std::string> cache_hits;
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(*flat_ptr, *pool_ptr, arena_, cache_ptr_local, &cache_hits);

            // Phase 4: Run passes per-function on the new function bundle.
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

            // Cache all non-entry functions as a bundle (preserving func id ordering)
            std::vector<aura::ir::IRFunction> bundle;
            for (auto& func : ir_mod.functions) {
                if (func.id != ir_mod.entry_function_id) {
                    bundle.push_back(std::move(func));
                }
            }
            ir_cache_[name_str] = std::move(bundle);

            // Store source for re-lowering on dependency changes
            function_sources_[name_str] = std::string(code);

            // Record dependencies from cache hits detected during lowering
            for (auto& called_name : cache_hits) {
                record_dependency(name_str, called_name);
            }

            // If redefining, invalidate dependents AFTER new function is cached,
            // so they get re-lowered with the updated version
            if (is_redefine) {
                invalidate_function(name_str);
            }

            // Tree-walker eval for persistent runtime bindings (reads FlatAST directly)
            auto result = evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root, evaluator_.top_env());
            if (!result) return result;
            return result;
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

    // IR function cache: name → bundle of functions for cached defines.
    // Stores ALL non-entry functions from the original lowering module.
    // The LAST function in the bundle is the user-defined lambda itself.
    // When inlined, all functions are added to the current module in order,
    // preserving func id references across cached calls.
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>> ir_cache_;

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
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(flat, pool, arena_, cache_ptr, &cache_hits);

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
    std::vector<aura::compiler::ClosureSnapshot> last_closures_;
    std::vector<aura::compiler::CellSnapshot> last_cells_;
    std::optional<aura::ir::IRModule> last_ir_mod_;

    // Persistent AST for mutation workflows (set_code / typed_mutate).
    aura::ast::FlatAST* current_ast_ = nullptr;
    aura::ast::StringPool* current_pool_ = nullptr;
};

} // namespace aura::compiler
