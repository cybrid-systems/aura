// service_module_io.cpp - CompilerService::cache_module and
// CompilerService::compile_module moved out of the module interface (#5242).
// Move-only: the bodies are unchanged.

module;

#include "core/transparent_string_hash.hh" // TransparentStringHash
#include <sys/stat.h>
#include <unistd.h>

module aura.compiler.service;

import std;

namespace aura::compiler {

void CompilerService::cache_module(const std::string& content, const std::string& path,
                                   aura::compiler::Env* module_env) {
    IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
    // don't survive re-evaluation via cache_define.

    // Issue #5374: the require path gets the same content-keyed disk cache
    // compile_module already uses. A hit pre-populates ir_cache_ so the
    // per-define lowering below is skipped; a miss writes the lowered
    // bundles at the end. The tree-walker module eval in load_module_file
    // still runs — the cache skips lowering, not evaluation.
    const auto disk_path = module_cache_path(path, content);
    aura::compiler::cache::MappedCache disk_cached;
    if (!disk_path.empty())
        disk_cached = aura::compiler::cache::open_cache(disk_path);
    const bool disk_hit = disk_cached.valid() && disk_cached.has_ir();
    std::unordered_set<std::string> disk_lowered;
    if (disk_hit) {
        // Group the whole bundle before inserting it. A disk snapshot names
        // every function define#pos; checking ir_cache_ per function would
        // keep only #0 and drop the inlined callees, so f's call to g became
        // a call to __top__ on the warm run.
        std::unordered_map<std::string, std::vector<aura::ir::IRFunction>> bundles;
        for (const auto& func : disk_cached.ir_functions()) {
            const auto hash = func.name.rfind('#');
            const std::string bare =
                hash == std::string::npos ? func.name : func.name.substr(0, hash);
            if (bare.empty())
                continue;
            auto& bundle = bundles[bare];
            const bool seen =
                std::ranges::any_of(bundle, [&](const auto& f) { return f.name == func.name; });
            if (!seen)
                bundle.push_back(func);
        }
        for (auto& [bare, bundle] : bundles) {
            if (ir_cache_.count(bare))
                continue;
            ir_cache_[bare] = std::move(bundle);
            ir_cache_strings_[bare] = disk_cached.ir_strings();
            disk_lowered.insert(bare);
        }
    }

    // Arena-allocate flat/pool so pointers survive (bridge data references them)
    auto alloc = arena_.allocator();
    aura::ast::StringPool* pool_ptr = nullptr;
    aura::ast::FlatAST* flat_ptr = nullptr;
    // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
    pool_ptr = arena_.create_with_cover<aura::ast::StringPool>(nullptr,
                                                               "stack-cover-dies-at-return", alloc);
    flat_ptr =
        arena_.create_with_cover<aura::ast::FlatAST>(nullptr, "stack-cover-dies-at-return", alloc);
    if (!pool_ptr || !flat_ptr)
        return;
    auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return;
    flat_ptr->root = pr.root;

    auto& flat = *flat_ptr;
    auto& pool = *pool_ptr;

    // Macro expand
    auto expanded = aura::compiler::macro_expand_all(flat, pool, flat.root);
    // Issue #4078: do not IR-cache defines from a refused half-expand.
    if (aura_hygiene_expand_deny_blocks_eval())
        return;

    // Walk top-level expressions to find (define ...) forms.
    // Issue #132: extracted to aura::compiler.find_top_level_defines
    // (was the inline DefineFinder struct in cache_define).
    auto finder = aura::compiler::find_top_level_defines(flat, pool, expanded);

    // Issue #5488: the module-env binding must not publish a body that the
    // IR lowering cannot represent. #4618: a non-local set! has no store in
    // IR Set. #4372: a lambda-form while applies closures from the
    // primitive, so the call returns the pre-loop value. Refuse the same
    // shapes specialize_one_lambda_via_ir refuses; the tree-walker closure
    // eval_flat installed stays authoritative.
    auto body_ir_safe = [&](aura::ast::NodeId def_id, const std::string& name) {
        if (define_body_sets_nonlocal_4618(flat, pool, def_id, name))
            return false;
        struct LambdaWhile {
            const aura::ast::FlatAST& f;
            const aura::ast::StringPool& p;
            bool hit = false;
            void walk(aura::ast::NodeId id) {
                if (hit || id == aura::ast::NULL_NODE || id >= f.size() || f.is_free_slot(id))
                    return;
                auto nv = f.get(id);
                if (nv.tag == aura::ast::NodeTag::Quote)
                    return;
                if (nv.tag == aura::ast::NodeTag::Call && nv.children.size() >= 3) {
                    auto callee_id = nv.child(0);
                    if (callee_id < f.size()) {
                        auto cal = f.get(callee_id);
                        if (cal.tag == aura::ast::NodeTag::Variable &&
                            p.resolve(cal.sym_id) == "while") {
                            const bool arm1 = nv.child(1) < f.size() &&
                                              f.get(nv.child(1)).tag ==
                                                  aura::ast::NodeTag::Lambda;
                            const bool arm2 = nv.child(2) < f.size() &&
                                              f.get(nv.child(2)).tag ==
                                                  aura::ast::NodeTag::Lambda;
                            if (arm1 || arm2)
                                hit = true;
                        }
                    }
                }
                if (hit)
                    return;
                for (auto c : nv.children)
                    walk(c);
            }
        } lambda_while{flat, pool};
        lambda_while.walk(def_id);
        return !lambda_while.hit;
    };

    // Issue #5281: export filtering removes an unexported define from
    // the importer's env, so the tree walker still sees the primitive.
    // The global IR cache is keyed by bare name and does not. std/hash
    // (and std/rule) define a private for-each; caching it made a
    // workspace lambda specialized under AURA_SANDBOX=off call that
    // helper. The helper returned #t and never applied the callback,
    // so set! of a let inside for-each was a no-op after set-code /
    // rebind. An exported shadow stays cached — the importer binds it
    // too. A module with no export form publishes every define.
    std::unordered_set<std::string> module_exports;
    bool module_has_export = false;
    // Names stay at least 3 characters and unchanged views stay const:
    // the module clang-tidy ratchet allows only +2 per (file, check).
    for (aura::ast::NodeId node_id = 0; node_id < flat.size(); ++node_id) {
        if (flat.is_free_slot(node_id))
            continue;
        const auto entry = flat.get(node_id);
        if (entry.tag != aura::ast::NodeTag::Export)
            continue;
        module_has_export = true;
        for (const auto child_id : entry.children) {
            if (child_id == aura::ast::NULL_NODE || child_id >= flat.size() ||
                flat.is_free_slot(child_id))
                continue;
            const auto child = flat.get(child_id);
            if (child.tag != aura::ast::NodeTag::Variable || child.sym_id == aura::ast::INVALID_SYM)
                continue;
            const auto exported_name = pool.resolve(child.sym_id);
            if (!exported_name.empty())
                module_exports.emplace(exported_name);
        }
    }

    // Issue #5488: module-env binding is for the library/exported face.
    // A project-private src/ module keeps the tree-walker closure, so a
    // type error in its body reports at the define instead of the call
    // site. lib/ modules keep the binding even without an export form
    // (lib/phys.aura is the motivating 156-define case).
    Env* binding_env = nullptr;
    if (module_env != nullptr &&
        (module_has_export || path.find("/lib/") != std::string::npos))
        binding_env = module_env;

    // Cache each define (IR only — tree-walker already evaluated the module)
    for (auto& [name, node_id] : finder) {
        if (ir_cache_.count(name)) {
            // Issue #5420: a disk-cache hit already has the IR bundle; the
            // module env still holds the tree-walker closure from eval_flat.
            // Publish the cached closure into that env before returning.
            if (binding_env != nullptr && disk_lowered.count(name) != 0) {
                if (body_ir_safe(node_id, name)) {
                    (void)bind_define_env_only_to_env(flat, pool, node_id, name, *binding_env);
                } else {
                    // Issue #5488: a bundle written by a build without the
                    // body gate must not replace the tree-walker closure.
                    ir_cache_.erase(name);
                    ir_cache_strings_.erase(name);
                    ir_cache_bridge_.erase(name);
                    disk_lowered.erase(name);
                }
            }
            // Issue #5464: the hit path must restore the same per-name
            // bookkeeping the lowering path below publishes. Without
            // function_sources_ the TW native tier has no source to
            // re-lower at the hot threshold, so the second (warm) run of a
            // module's self-recursive function stays on the tree walker.
            user_bindings_.insert(name);
            if (!is_workspace_lambda_define(name))
                function_sources_[name] = content;
            auto& module_names = module_functions_[path];
            if (std::ranges::find(module_names, name) == module_names.end())
                module_names.push_back(name);
            continue; // already cached
        }

        // Skip value defines (e.g., (define pi 3.14)) — only cache function defines
        // A function define's body is a Lambda node
        auto define_node = flat.get(node_id);
        if (define_node.children.empty())
            continue;
        auto body_node = flat.get(define_node.child(0));
        if (body_node.tag != aura::ast::NodeTag::Lambda)
            continue;

        // Issue #5281: private primitive shadow. See the export scan above.
        if (module_has_export && module_exports.count(name) == 0 &&
            evaluator_.primitives().slot_for_name(name) < evaluator_.primitives().slot_count()) {
            continue;
        }

        // Check: can this function be lowered to IR?
        // We skip functions where any variable reference in the body
        // isn't resolvable: not a parameter, not a primitive, not in ir_cache_.
        // This covers:
        //   1. Self-recursive calls (function not in ir_cache_ yet)
        //   2. Calls to other non-cached, non-primitive functions
        //   3. Any variable reference that would fall through to ConstI64 0
        bool skip_ir_cache_fn = false;
        {
            // Collect parameter names
            auto params_span = body_node.params;
            std::unordered_set<std::string> param_names;
            for (auto pid : params_span)
                param_names.insert(std::string(pool.resolve(pid)));

            struct FnCheck {
                const aura::ast::FlatAST& f;
                const aura::ast::StringPool& p;
                const std::unordered_set<std::string>& params;
                const std::string& self_name;
                const Evaluator& eval;
                const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                                         aura::core::TransparentStringHash, std::equal_to<>>&
                    ir_cache;
                const Env* module_env;
                bool skip = false;
                std::vector<std::unordered_set<std::string>> scopes;

                [[nodiscard]] bool local_bound(const std::string& nm) const {
                    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                        if (it->count(nm) != 0)
                            return true;
                    return false;
                }
                [[nodiscard]] bool resolvable(const std::string& nm) const {
                    if (nm.empty() || nm[0] == ':')
                        return true;
                    if (local_bound(nm) || params.count(nm) != 0 || nm == self_name)
                        return true;
                    if (eval.primitives().slot_for_name(nm) <
                        eval.primitives().slot_count())
                        return true;
                    if (ir_cache.count(nm) != 0)
                        return true;
                    // Issue #5488: value defines are not in ir_cache_, but
                    // eval_flat bound every top-level define in the module env.
                    // Lowering resolves an unknown name with a late-bound
                    // top-ref, so a module-level value is safe to accept.
                    if (module_env != nullptr && module_env->lookup_local_binding(nm).has_value())
                        return true;
                    return false;
                }
                void walk(aura::ast::NodeId id) {
                    if (skip || id == aura::ast::NULL_NODE || id >= f.size() ||
                        f.is_free_slot(id))
                        return;
                    auto nv = f.get(id);
                    if (nv.tag == aura::ast::NodeTag::Quote)
                        return;
                    if (nv.tag == aura::ast::NodeTag::Lambda) {
                        scopes.push_back({});
                        for (auto pid : nv.params) {
                            auto pn = std::string(p.resolve(pid));
                            if (!pn.empty())
                                scopes.back().insert(std::move(pn));
                        }
                        for (auto c : nv.children)
                            walk(c);
                        scopes.pop_back();
                        return;
                    }
                    if (nv.tag == aura::ast::NodeTag::Let ||
                        nv.tag == aura::ast::NodeTag::LetRec) {
                        const bool rec = nv.tag == aura::ast::NodeTag::LetRec;
                        std::string nm;
                        if (nv.sym_id != aura::ast::INVALID_SYM)
                            nm = std::string(p.resolve(nv.sym_id));
                        const bool named = !nm.empty();
                        if (rec && named) {
                            scopes.push_back({});
                            scopes.back().insert(nm);
                        }
                        if (!nv.children.empty())
                            walk(nv.child(0));
                        if (!rec && named) {
                            scopes.push_back({});
                            scopes.back().insert(std::move(nm));
                        }
                        for (std::size_t i = 1; i < nv.children.size(); ++i)
                            walk(nv.child(i));
                        if (named)
                            scopes.pop_back();
                        return;
                    }
                    if (nv.tag == aura::ast::NodeTag::Begin) {
                        scopes.push_back({});
                        for (auto c : nv.children) {
                            if (c >= f.size() || f.is_free_slot(c))
                                continue;
                            auto cv = f.get(c);
                            if (cv.tag != aura::ast::NodeTag::Define ||
                                cv.sym_id == aura::ast::INVALID_SYM)
                                continue;
                            auto dn = std::string(p.resolve(cv.sym_id));
                            if (!dn.empty())
                                scopes.back().insert(std::move(dn));
                        }
                        for (auto c : nv.children)
                            walk(c);
                        scopes.pop_back();
                        return;
                    }
                    if (nv.tag == aura::ast::NodeTag::Define) {
                        if (nv.sym_id != aura::ast::INVALID_SYM) {
                            if (scopes.empty())
                                scopes.push_back({});
                            auto dn = std::string(p.resolve(nv.sym_id));
                            if (!dn.empty())
                                scopes.back().insert(std::move(dn));
                        }
                        for (auto c : nv.children)
                            walk(c);
                        return;
                    }
                    if (nv.tag == aura::ast::NodeTag::Variable) {
                        auto var_name = std::string(p.resolve(nv.sym_id));
                        if (!resolvable(var_name))
                            skip = true;
                        return;
                    }
                    for (auto c : nv.children)
                        walk(c);
                }
            };
            FnCheck fc{flat, pool, param_names, name, evaluator_, ir_cache_, binding_env};
            if (!body_node.children.empty())
                fc.walk(body_node.child(0));
            skip_ir_cache_fn = fc.skip;
        }
        if (skip_ir_cache_fn) {
            continue;
        }

        // Issue #5488: the FnCheck only sees names; the body gate sees the
        // #4618 non-local set! and #4372 lambda-while shapes that lowering
        // cannot represent. Keep the tree-walker closure for those.
        if (!body_ir_safe(node_id, name))
            continue;

        // Skip functions with internal (define ...) — their cell setup is
        // in __top__ which isn't cached; the cached lambda can't create cells.
        bool has_nested_defines = false;
        {
            struct NestCheck {
                aura::ast::FlatAST& flat;
                bool found = false;
                void walk(aura::ast::NodeId id) {
                    if (found || id >= flat.size())
                        return;
                    auto v = flat.get(id);
                    if (v.tag == aura::ast::NodeTag::Define)
                        found = true;
                    for (auto c : v.children)
                        walk(c);
                }
            };
            NestCheck nc{flat, false};
            if (!body_node.children.empty())
                nc.walk(body_node.child(0));
            has_nested_defines = nc.found;
        }
        if (has_nested_defines)
            continue;

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
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        std::vector<std::string> cache_hits;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
            cache_strings_ptr, &name, &type_registry_, value_cells_for_lowering());
        flat.root = saved_root; // restore

        // Run passes
        {
            aura::compiler::ComputeKindWrap ck_pass;
            aura::compiler::ConstantFoldingWrap cf_pass;
            for (auto& func : ir_mod.functions) {
                if (func.id == ir_mod.entry_function_id)
                    continue;
                ck_pass.compute_function(func);
                cf_pass.fold_function(func);
            }
        }

        // Cache bundle
        std::vector<aura::ir::IRFunction> bundle;
        std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
        std::size_t own_pos = 0;
        for (auto& func : ir_mod.functions) {
            if (func.id == ir_mod.entry_function_id)
                continue;
            // Issue #660: assign a unique stable name (same pattern as cache_define)
            // so cross-module closure identity is preserved when the cache bundle
            // is loaded into a fresh module.
            if (func.name.empty() || func.name == "__lambda__") {
                func.name = name + std::string("#") + std::to_string(own_pos++);
            }
            const auto fid = func.id; // read before the move below
            bundle.push_back(std::move(func));
            // Save bridge data
            if (fid < ir_mod.closure_bridge.size())
                bridge_bundle.push_back(ir_mod.closure_bridge[fid]);
            else
                bridge_bundle.emplace_back();
        }
        ir_cache_[name] = std::move(bundle);
        ir_cache_bridge_[name] = std::move(bridge_bundle);
        // Issue #5420: eval_flat installed a tree-walker closure in the
        // module env. Publish the IR closure now so calling the export from
        // the entry program hits ir-calls instead of the walker.
        if (binding_env != nullptr)
            (void)bind_define_env_only_to_env(flat, pool, node_id, name, *binding_env);
        // Issue #2572: persist the module's ConstString pool with the
        // cached IR. Without this, call-site lower_to_ir_with_cache
        // copies the function but cannot remap ConstString indices
        // into the new IRModule::string_pool — body literals such as
        // "[flux] " resolve against the caller's pool and multi-
        // (display …) in exported helpers prints garbled/duplicated
        // args (e.g. "hello-oncehello-once" instead of "[flux] hello-once").
        // cache_define already stores ir_cache_strings_; mirror it here.
        ir_cache_strings_[name] = ir_mod.string_pool;
        // Self-referencing cached functions need tree-walker fallback
        user_bindings_.insert(name);
        // Issue #5384: a workspace define owns its canonical single-define
        // source row (set-code / eval-current publish it). A module define
        // with the same name must not clobber it with the WHOLE module text:
        // the specialize pass scopes lowering to the named define, but the
        // #4621 rest gate reads the FIRST root define, so a rest-param
        // workspace define was IR-published from the whole module and its
        // closure resolved to a sibling function ("invalid closure reference"
        // / foreign arity after a rebind). Module-only names keep the module
        // text row the relower cascade parses (#4547).
        if (!is_workspace_lambda_define(name))
            function_sources_[name] = content;
        module_functions_[path].push_back(name);
        // Issue #5374: snapshot for the require-path disk cache (same
        // pre-bind shape cache_define records for compile_module). Name every
        // function in the bundle so the disk loader can group it back.
        if (!disk_hit)
            ir_disk_snapshots_[name] = disk_bundle_for(name, ir_cache_[name]);

        for (auto& cn : cache_hits)
            record_dependency(name, cn);
        if (is_redefine)
            invalidate_function(name);
    }

    // Issue #5374: write the require-path disk cache on a miss. Only the
    // bundles lowered in THIS load are written; a hit never rewrites.
    if (!disk_hit && !disk_path.empty() && ensure_cache_dir()) {
        aura::ir::IRModule disk_mod;
        for (auto& [fname, _] : finder) {
            if (disk_lowered.count(fname))
                continue; // came from disk, not lowered here
            auto it = ir_disk_snapshots_.find(fname);
            if (it == ir_disk_snapshots_.end())
                continue;
            auto sit = ir_cache_strings_.find(fname);
            for (const auto& func : it->second) {
                // Each bundle's ConstString operands index its OWN lowering
                // pool. Re-intern every string into the shared disk pool and
                // remap the operands — without this the written module fails
                // validate_ir_module on read-back and the entry never hits.
                auto remapped = func;
                if (sit != ir_cache_strings_.end()) {
                    const auto& strs = sit->second;
                    for (auto& block : remapped.blocks) {
                        for (auto& instr : block.instructions) {
                            if (instr.opcode != aura::ir::IROpcode::ConstString ||
                                instr.operands.size() < 2 || instr.operands[1] >= strs.size())
                                continue;
                            instr.operands[1] = disk_mod.add_string(strs[instr.operands[1]]);
                        }
                    }
                }
                disk_mod.functions.push_back(std::move(remapped));
            }
        }
        if (!disk_mod.functions.empty())
            aura::compiler::cache::write_cache(disk_path, flat, pool, flat.root, 0, &disk_mod,
                                               nullptr);
    }
}

EvalResult CompilerService::compile_module(const std::string& name, const std::string& source) {
    // Save source for future dirty checks / reloads
    module_states_[name].source = source;

    auto& mod_arena = arena_group_.module_arena(name);
    mod_arena.reset();

    auto alloc = mod_arena.allocator();
    aura::ast::StringPool* pool_ptr = nullptr;
    aura::ast::FlatAST* flat_ptr = nullptr;
    // Issue #4070: mod_arena outlives this frame. EXEMPT the stack
    // covers; do not note &pool_ptr / &flat_ptr into the slot vector.
    pool_ptr = mod_arena.create_with_cover<aura::ast::StringPool>(
        nullptr, "stack-cover-dies-at-return", alloc);
    flat_ptr = mod_arena.create_with_cover<aura::ast::FlatAST>(nullptr,
                                                               "stack-cover-dies-at-return", alloc);
    auto pr = aura::parser::parse_to_flat(source, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        return std::unexpected(parse_error_diag(pr));
    }
    flat_ptr->root = pr.root;

    auto& flat = *flat_ptr;
    auto& pool = *pool_ptr;

    // Macro expand
    auto expanded = aura::compiler::macro_expand_all(flat, pool, flat.root);
    // Issue #4078: do not cache or lower a refused half-expand.
    if (aura_hygiene_expand_deny_blocks_eval())
        return std::unexpected(hygiene_expand_deny_diag_4078(&flat));

    // Walk top-level defines. Issue #132: extracted to
    // aura::compiler.find_top_level_defines (was the
    // inline DefFinder struct in compile_module).
    auto finder = aura::compiler::find_top_level_defines(flat, pool, expanded);

    // Try disk cache: load cached IR bundles to skip lowering.
    // An empty path (HOME and XDG unset) is a miss, not /tmp.
    auto cache_path = module_cache_path(name, source);
    aura::compiler::cache::MappedCache cached;
    if (!cache_path.empty())
        cached = aura::compiler::cache::open_cache(cache_path);
    bool cache_hit = cached.valid() && cached.has_ir();
    if (cache_hit) {
        auto& all_funcs = cached.ir_functions();
        for (auto& [fname, node_id] : finder) {
            if (ir_cache_.count(fname))
                continue;
            auto dv = flat.get(node_id);
            if (dv.children.empty())
                continue;
            if (flat.get(dv.child(0)).tag != aura::ast::NodeTag::Lambda)
                continue;
            // A bundle is the define plus the callees it inlined. Disk
            // snapshots name every member fname#pos, so collect the whole
            // prefix group; keeping only the entry function lost the callee
            // and left a warm require with a call to __top__.
            std::vector<aura::ir::IRFunction> bundle;
            for (auto& func : all_funcs) {
                const auto hash = func.name.rfind('#');
                const std::string bare =
                    hash == std::string::npos ? func.name : func.name.substr(0, hash);
                if (bare == fname)
                    bundle.push_back(func);
            }
            if (!bundle.empty()) {
                ir_cache_[fname] = std::move(bundle);
                ir_cache_strings_[fname] = cached.ir_strings();
                // Issue #5384: same rule as cache_module — a workspace
                // define keeps its canonical source row.
                if (!is_workspace_lambda_define(fname))
                    function_sources_[fname] = source;
                module_functions_[name].push_back(fname);
            }
            // Issue #2572: bind_in_env=false — load_module_file already
            // injects TW closures for exports. Only cache IR (+ strings)
            // here; do not overwrite env bindings. ConstString remap
            // depends on ir_cache_strings_ (saved by cache_define).
            auto bind_result = cache_define(source, flat, pool, node_id, fname,
                                            /*bind_in_env=*/false, name);
            if (!bind_result)
                return bind_result;
            user_bindings_.insert(fname);
        }
    }

    // Cache each define (only if not loaded from disk cache)
    if (!cache_hit) {
        for (auto& [fname, node_id] : finder) {
            auto def_node = flat.get(node_id);
            if (def_node.children.empty())
                continue;
            auto body_id = def_node.child(0);
            if (body_id >= flat.size())
                continue;

            if (flat.get(body_id).tag == aura::ast::NodeTag::Lambda) {
                if (ir_cache_.count(fname))
                    continue;
                // Issue #2572: bind_in_env=false (module already injected TW).
                auto result = cache_define(source, flat, pool, node_id, fname,
                                           /*bind_in_env=*/false, name);
                if (!result)
                    return result;
                user_bindings_.insert(fname);
                continue;
            }

            // Issue #272 Cycle 3: value defines via IR when lowerable.
            if (!define_body_needs_tree_walker_fallback(flat, pool, body_id, fname) &&
                bind_value_define_via_ir(flat, pool, node_id, fname)) {
                continue;
            }
            auto result = evaluator_.eval_flat(flat, pool, node_id, evaluator_.top_env());
            if (!result)
                return result;
            user_bindings_.insert(fname);
        }
    } // if (!cache_hit) — skip lowering when loaded from disk

    // Mark module as loaded
    loaded_modules_.insert(name);

    // Mark module clean and record dependencies from dep_graph_
    auto& state = module_states_[name];
    state.dirty = false;
    state.deps.clear();
    {
        std::shared_lock dep_read(dep_graph_mtx_);
        for (auto& [fname, _] : finder) {
            auto dit = dep_graph_.find(fname);
            if (dit != dep_graph_.end()) {
                for (auto& callee : dit->second.calls)
                    state.deps.insert(callee);
            }
        }
    }

    // Write disk cache (only when not loaded from cache).
    // Issue #272 Cycle 4: use pre-bind snapshots (captured in cache_define).
    if (!cache_hit && ensure_cache_dir()) {
        auto cache_path = module_cache_path(name, source);
        aura::ir::IRModule disk_mod;
        for (auto& [fname, _] : finder) {
            auto it = ir_disk_snapshots_.find(fname);
            if (it == ir_disk_snapshots_.end())
                continue;
            for (const auto& func : it->second)
                disk_mod.functions.push_back(func);
        }
        // 生成类型签名数据嵌入 ABF
        std::string sig_embed;
        // 从 export 声明收集已注册的类型签名
        // 直接从模块的 FlatAST 推断类型
        auto sig_embed_path = source;
        if (sig_embed_path.ends_with(".aura"))
            sig_embed_path = sig_embed_path.substr(0, sig_embed_path.size() - 5) + ".aura-type";
        struct stat sig_st;
        if (::stat(sig_embed_path.c_str(), &sig_st) == 0 && S_ISREG(sig_st.st_mode)) {
            std::ifstream sf(sig_embed_path);
            if (sf) {
                sig_embed.assign((std::istreambuf_iterator<char>(sf)), {});
            }
        }
        aura::compiler::cache::write_cache(cache_path, flat, pool, flat.root, 0,
                                           disk_mod.functions.empty() ? nullptr : &disk_mod,
                                           sig_embed.empty() ? nullptr : &sig_embed);
    }

    return EvalResult(types::make_void());
}

} // namespace aura::compiler
