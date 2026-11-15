// service_dispatch_define.cpp - CompilerService::dispatch_ir_define_closure
// moved out of the module interface (#5242). Move-only: the body is unchanged.

module;

module aura.compiler.service;

import std;

namespace aura::compiler {

std::optional<types::EvalValue>
CompilerService::dispatch_ir_define_closure(aura::compiler::ClosureId cid,
                                            std::span<const types::EvalValue> args) {
    // Issue #4717 (P4): the registry / owner-map / bridge / IR-cache
    // lookups below run under the shared IR cache gate; every path that
    // RUNS a body drops it first (unique_lock) and pins the binding by
    // shared_ptr, so no body ever executes under the cache gate.
    std::unique_lock<std::recursive_mutex> ir_gate(ir_cache_gate_, std::defer_lock);
    relock_ir_gate_(ir_gate); // #4765
    // Issue #4712: resolve the owning define by binding identity when
    // the cid->name index has no row. A workspace replace (set-code /
    // snapshot restore) clears the index wholesale in
    // mark_all_defines_dirty, because the previous workspace's ids must
    // not resurrect (#2026). The binding and its top_env cell survive
    // that clear, and an unchanged sibling define has no dirty row, so
    // the eval-current specialize pass never re-binds it: its own
    // bind-time id then had no index row and was also denied by the
    // strict runtime-mint rule below (#4450 / #4349), so a cell still
    // holding it (a fiber body spawned before the replace, or the
    // re-evaluated workspace) got "eval_flat: invalid closure". The
    // index is an optimisation; the binding's closure_id is the
    // authority the indexed branch re-checks anyway, so fall back to it
    // before the runtime-mint search. A replaced binding is gone from
    // the map, so a genuinely stale id still denies.
    const std::string* owner_name = nullptr;
    auto owner_it = ir_define_closure_owner_.find(cid);
    if (owner_it != ir_define_closure_owner_.end()) {
        owner_name = &owner_it->second;
    } else {
        for (auto& kv : ir_define_env_bindings_) {
            if (kv.second && kv.second->interpreter && kv.second->closure_id == cid) {
                owner_name = &kv.first;
                break;
            }
        }
    }
    if (owner_name == nullptr) {
        // Issue #4715 regression: this id was a define's own publication
        // a moment ago (see retire_ir_define_closure_). The caller
        // resolved it from the define's cell just before the re-bind took
        // over and can be parked behind ir_cache_gate_ across several
        // publications, so resolve it the way the live index would have:
        // run the define's CURRENT publication. Only ids this evaluator
        // really published resolve; anything else still takes the strict
        // runtime-mint decide below (#4349 / #4450).
        std::string retired_name;
        bool have_retired = false;
        if (auto retired = retired_define_closure_owner_.find(cid);
            retired != retired_define_closure_owner_.end()) {
            retired_name = retired->second;
            have_retired = true;
        } else if (auto tw_name = retired_tw_closure_name_(cid); tw_name.has_value()) {
            // Issue #4753: a dropped tree-walker generation of a define
            // (Issue #4583 must_deopt_closures_named drop). A caller that
            // sampled the define's cell before the drop still holds this
            // id; resolve it to the define's current publication exactly
            // like a retired IR publication id.
            retired_name = *tw_name;
            have_retired = true;
        }
        if (have_retired) {
            if (auto bit = ir_define_env_bindings_.find(retired_name);
                bit != ir_define_env_bindings_.end() && bit->second && bit->second->interpreter) {
                const std::shared_ptr<IRDefineEnvBinding> pinned = bit->second;
                if (ir_gate.owns_lock())
                    ir_gate.unlock();
                types::EvalValue out{};
                const auto ran = run_pinned_ir_closure_(pinned, pinned->closure_id, args, 0, &out);
                relock_ir_gate_(ir_gate); // #4765
                if (ran == PinnedRun4717::Ok)
                    return out;
            }
            // Issue #4753: the define may have no IR publication for the
            // moment (its binding is dropped while the cell already
            // advertises the next publication, or a re-bind is between
            // publications). The cell is the publication SSOT the caller
            // itself resolved from, so run whatever it advertises now -
            // that is the old-or-new half of the contract, and it never
            // runs a body the cell no longer points at.
            if (auto cell_cid = top_env_cell_closure_(retired_name);
                cell_cid && *cell_cid != cid && retired_resolve_depth_ < 4) {
                ++retired_resolve_depth_;
                if (ir_gate.owns_lock())
                    ir_gate.unlock();
                auto resolved = evaluator_.apply_closure(*cell_cid, args);
                relock_ir_gate_(ir_gate); // #4765
                --retired_resolve_depth_;
                if (resolved)
                    return resolved;
            }
            return std::nullopt;
        }
        // Issue #4450: a closure minted at RUNTIME by a define's body
        // (an escaping lambda) carries a fresh id that is in no owner
        // map — it lives in the minting interpreter's runtime_closures_.
        // Route the call there (cursor synced from the global monotonic
        // source, same as the owner path), consulting the strict
        // runtime-mint set: bind-time execute() mints must NOT resurrect
        // here after a workspace restore / rebind (#4349 stale call
        // denies). If no binding owns the mint, deny: the caller's
        // typed "invalid closure" face — never an unrelated define's
        // body.
        for (auto& kv : ir_define_env_bindings_) {
            if (!kv.second || !kv.second->interpreter)
                continue;
            // Issue #4626: the interpreter's strict runtime-mint set is
            // filled by a BOOL scope flag (IRInterpreter::call_closure
            // raises in_runtime_call_ on entry and lowers it on exit).
            // A NESTED call_closure - a tree-walker callback (map /
            // filter / for-each) re-entering this same interpreter -
            // lowers the flag when it returns, so every closure the
            // OUTER call mints AFTER that nested call is missing from
            // the strict set even though it IS a runtime mint. Such a
            // callback lost its route: dispatch denied, the primitive
            // saw an empty result and the element came back as ().
            //
            // The service keeps the same classification itself, from
            // the interpreter's public id cursor: a binding remembers
            // the cursor at the start of its first call
            // (runtime_mint_floor). Every id at or above that floor was
            // minted during a call; the bind-time execute() mint sits
            // below it, so the strict #4450 rule (a bind-time mint must
            // NOT resurrect after a workspace restore / rebind) still
            // holds.
            // Issue #4717: never run a body under the cache gate. Pin the
            // binding, drop the gate, then classify + run under exec_mtx.
            const std::shared_ptr<IRDefineEnvBinding> pinned = kv.second;
            if (ir_gate.owns_lock())
                ir_gate.unlock();
            types::EvalValue out{};
            const auto ran = run_pinned_ir_closure_(pinned, cid, args, 1, &out);
            relock_ir_gate_(ir_gate); // #4765
            if (ran == PinnedRun4717::Ok)
                return out;
            if (ran == PinnedRun4717::Failed)
                return std::nullopt;
        }
        // Issue #4815: a closure a top-level VALUE define minted while
        // computing its value (bind_value_define_via_ir keeps that
        // interpreter alive while it owns closures).
        // Issue #4904: snapshot the owners under the gate. The loop drops
        // the gate for every run, and a value define registers / erases
        // its run-scoped owner meanwhile (bind_value_define_via_ir), so
        // iterating the live map across the unlock could rehash under us.
        std::vector<std::shared_ptr<IRDefineEnvBinding>> value_owners_4904;
        value_owners_4904.reserve(ir_value_define_closure_owners_.size());
        for (auto& kv : ir_value_define_closure_owners_)
            value_owners_4904.push_back(kv.second);
        for (const auto& pinned : value_owners_4904) {
            if (!pinned || !pinned->interpreter)
                continue;
            if (ir_gate.owns_lock())
                ir_gate.unlock();
            types::EvalValue out{};
            const auto ran = run_value_define_closure_4815_(pinned, cid, args, &out);
            relock_ir_gate_(ir_gate); // #4765
            if (ran == PinnedRun4717::Ok)
                return out;
            if (ran == PinnedRun4717::Failed)
                return std::nullopt;
        }
        return std::nullopt;
    }
    // Issue #4717: copy the name and pin the binding before any unlock —
    // `owner_name` points into a container this function is about to
    // stop guarding.
    const std::string name = *owner_name;
    auto bind_it = ir_define_env_bindings_.find(name);
    if (bind_it == ir_define_env_bindings_.end() || !bind_it->second ||
        !bind_it->second->interpreter)
        return std::nullopt;
    const std::shared_ptr<IRDefineEnvBinding> binding = bind_it->second;
    // Note: the stale-entry detection that lived here previously
    // (binding->interpreter->flat.get() compare against current
    // workspace) was REMOVED because IRInterpreter doesn't expose
    // flat/pool directly — the targeted fix failed to compile.
    // The SIGSEGV is fixed by the surgical fix in
    // mark_all_defines_dirty (clears ir_define_closure_owner_
    // on set-code, the cid→name map that holds stale ClosureIds).

    // Higher-order cached defines (bridge:cached-fn): TW lambda args
    // are not in the IR interpreter's runtime_closures_. Run the cached
    // FlatAST body via eval_flat so (f ...) dispatches correctly.
    bool has_tw_closure_arg = false;
    for (auto& a : args) {
        if (!types::is_closure(a))
            continue;
        auto ac = types::as_closure_id(a);
        if (ir_define_closure_owner_.find(ac) == ir_define_closure_owner_.end())
            has_tw_closure_arg = true;
    }
    if (has_tw_closure_arg) {
        auto bridge_it = ir_cache_bridge_.find(name);
        auto func_it = ir_cache_.find(name);
        if (bridge_it != ir_cache_bridge_.end() && !bridge_it->second.empty() &&
            func_it != ir_cache_.end() && !func_it->second.empty()) {
            auto& bd = bridge_it->second[0];
            if (bd.flat && bd.pool && bd.body_id != aura::ast::NULL_NODE) {
                // Issue #4717: copy what the run needs, then drop the
                // gate — eval_flat runs a body and must never hold it.
                const auto flat_sp = bd.flat;
                const auto pool_sp = bd.pool;
                const auto body_id = bd.body_id;
                // Copy the params too: func_it->second is a cache row
                // that a sibling thread may rewrite once the gate drops.
                auto params = func_it->second[0].params;
                if (ir_gate.owns_lock())
                    ir_gate.unlock();
                aura::compiler::Env ne(&evaluator_.top_env());
                ne.set_primitives(&evaluator_.primitives());
                ne.set_pool(pool_sp.get());
                for (std::size_t i = 0; i < params.size() && i < args.size(); ++i)
                    ne.bind(params[i], args[i]);
                auto r = evaluator_.eval_flat(*const_cast<aura::ast::FlatAST*>(flat_sp.get()),
                                              *const_cast<aura::ast::StringPool*>(pool_sp.get()),
                                              body_id, ne);
                if (r)
                    return *r;
            }
        }
    }

    // Issue #4450: the interpreter's cursor was left where its binding
    // execute() stopped — inside the id range already handed to the
    // NEXT define. A body that mints a closure at call time (an
    // escaping lambda) therefore minted ids owned by another define,
    // and the call silently re-entered it (or hit "invalid closure"
    // when the aliased define was absent). Sync the cursor from the
    // global monotonic source around every runtime execution so
    // runtime mints never collide with any binding's reserved range;
    // drain the advanced cursor back afterwards.
    // Issue #4682 (6b): opt-in native tier (pure, guarded, exact).
    if (tier_jit_enabled_4682_() && binding->closure_id == cid) {
        // Issue #4717: the tier slot lives on the pinned binding; the run
        // is a body run, so drop the cache gate and hold exec_mtx.
        const std::shared_ptr<IRDefineEnvBinding> pinned = binding;
        if (ir_gate.owns_lock())
            ir_gate.unlock();
        std::optional<types::EvalValue> nr;
        {
            ExecRunLock4765 exec(pinned->exec_mtx); // #4765
            nr = tier_try_native_4682_(*pinned, name, args);
        }
        if (nr)
            return *nr;
        relock_ir_gate_(ir_gate); // #4765
    }
    // Issue #4717: the run itself never holds the cache gate (see
    // run_pinned_ir_closure_ — it re-syncs the closure-id cursor from
    // the shared monotonic source and drains it back afterwards).
    if (ir_gate.owns_lock())
        ir_gate.unlock();
    types::EvalValue out{};
    if (run_pinned_ir_closure_(binding, cid, args, 0, &out) != PinnedRun4717::Ok)
        return std::nullopt;
    return out;
}

} // namespace aura::compiler
