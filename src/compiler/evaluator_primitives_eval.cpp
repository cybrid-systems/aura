// evaluator_primitives_eval.cpp — P0 step 21: set-code / eval / typecheck EDSL primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <fcntl.h>
#include <unistd.h>
#include "runtime_shared.h"
#include "messaging_bridge.h"
#include "prim_names.h" // #904

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.parser.parser;
import aura.diag;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using MakeErrorVal = std::function<EvalValue(const std::string&, const std::string&)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

void register_eval_primitives(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev,
                              std::function<void()> destroy_defuse_index) {

    add(aura::compiler::prim::kSetCode,
        [&ev, mev, destroy_defuse_index](const auto& a) -> EvalValue {
            std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);
            if (ev.workspace_read_only_)
                return mev("read-only", "workspace is read-only");
            // Clear any previous set-code error and eval-current cache
            ev.last_set_code_error_kind_.clear();
            ev.last_set_code_error_msg_.clear();
            ev.last_eval_current_result_.reset();
            ev.coverage_counters_[0]++;
            ev.coverage_counters_[5]++;
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg", "usage: (set-code code-string)");
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return mev("bad-arg", "code string index out of range");
            if (!ev.arena_)
                return mev("internal", "no arena allocator available");

            // Issue #230 #3: allocate fresh pool/flat on every set-code
            // (don't reuse the old one). Reason: registered macros
            // (via defmacro / define-hygienic-macro) hold raw pointers
            // into the old pool/flat for their body. Reusing the same
            // pool/flat and clearing it would invalidate those pointers
            // — old macros would silently fail to expand (the lookup
            // finds the macro in `ev.macros_` but clone_macro_body reads
            // from a cleared flat and returns NULL_NODE → macro
            // returns make_void()). Allocating fresh each call keeps
            // the old pool/flat intact in the arena, so previously
            // registered macros keep working after subsequent
            // set-codes.
            //
            // Trade-off: arena grows by one pool/flat per set-code
            // call. The previous OOM-fix comment about "750
            // consecutive set-codes" is no longer protected, but in
            // practice test scripts do < 10 set-codes and the arena
            // size stays bounded. The correctness win (macros
            // survive set-code) is worth the small arena growth.
            auto alloc = ev.arena_->allocator();
            auto* pool_ptr = ev.arena_->create<aura::ast::StringPool>(alloc);
            auto* flat_ptr = ev.arena_->create<aura::ast::FlatAST>(alloc);
            bool fresh_alloc = true;

            auto pr = aura::parser::parse_to_flat(ev.string_heap_[idx], *flat_ptr, *pool_ptr);
            if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                // Return structured parse error: ("parse" "message")
                std::string err;
                if (!pr.errors.empty()) {
                    for (auto& e : pr.errors) {
                        if (!err.empty())
                            err += "; ";
                        err += e.format();
                    }
                } else if (!pr.error.empty()) {
                    err = pr.error;
                } else {
                    err = "parse error";
                }
                // Store error for eval-current/eval-current-output to propagate
                ev.last_set_code_error_kind_ = "parse";
                ev.last_set_code_error_msg_ = err;
                ev.coverage_counters_[5]--;
                return mev("parse", err);
            }
            flat_ptr->root = pr.root;
            ev.workspace_flat_ = flat_ptr;
            ev.workspace_pool_ = pool_ptr;
            // Issue #1381: retain source for serialize-workspace.
            ev.workspace_source_text_ = ev.string_heap_[idx];
            // Issue #211: invalidate the (tag, arity) index
            // when the workspace changes. (The set_workspace_flat
            // hook would do this, but set-code assigns directly
            // here, so we invalidate inline.)
            ev.invalidate_tag_arity_index();
            // Issue #490: optional eager rebuild after workspace swap.
            ev.maybe_eager_rebuild_pattern_index_after_cow();
            ev.update_shared_tree_root();
            // Invalidate def-use index (new workspace)
            // (ASAN fix #107 leak) delete the old index; without this,
            // each set-code leaks the previous DefUseIndex (~3KB each).
            destroy_defuse_index();
            // Phase 2: a fresh workspace means every cached define is potentially
            // changed. Mark all dirty so the next (eval-current) re-evaluates.
            if (ev.mark_all_defines_dirty_fn_)
                ev.mark_all_defines_dirty_fn_();
            // Pre-populate the v2 IR cache from the new workspace's defines.
            // For unchanged defines, this is a cache hit (skip lowering).
            // For new/changed defines, this lowers them once and stores the result.
            if (ev.pre_cache_workspace_defines_fn_)
                ev.pre_cache_workspace_defines_fn_();
            // Issue #165 follow-up: extract MacroDef nodes from the new
            // workspace and register them in the runtime `ev.macros_` registry.
            // Without this, subsequent eval("(m)") parses a fresh FlatAST
            // that has no MacroDef node, so macro_expand_all finds nothing
            // and the call resolves to an undefined variable. The fix
            // walks the workspace and inserts every MacroDef (defmacro /
            // define-hygienic-macro) into ev.macros_, mirroring what eval_flat
            // would have done if the workspace had been evaluated inline.
            {
                // set-code is single-threaded at this point (we hold
                // ev.workspace_mtx_ above), so a plain unsynchronized
                // write to ev.macros_ is fine. If concurrent set-code
                // becomes a real scenario, add a ev.macros_mtx_ in
                // evaluator.ixx alongside ev.closures_mtx_.
                //
                // Issue #230 #3: DON'T clear ev.macros_ on set-code.
                // The loop below adds/overrides macros from the new
                // workspace, but the macros registered BEFORE this
                // set-code (e.g. via (require "std/test" all:))
                // need to survive so they can still be invoked
                // after the workspace is replaced. The new
                // pool/flat pointers are fresh, but the OLD
                // pool/flat (referenced by the surviving macros)
                // is still in the arena and untouched (we no
                // longer reuse/clear it on set-code), so the
                // stored MacroDef::flat/::pool pointers are
                // valid and the macro bodies can still be cloned
                // and expanded.
                for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
                    auto v = flat_ptr->get(id);
                    if (v.tag != aura::ast::NodeTag::MacroDef)
                        continue;
                    auto macro_name = std::string(pool_ptr->resolve(v.sym_id));
                    if (macro_name.empty())
                        continue;
                    std::vector<std::string> param_names;
                    param_names.reserve(v.params.size());
                    for (auto pid : v.params)
                        param_names.emplace_back(pool_ptr->resolve(pid));
                    auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    bool is_dotted = (v.int_value & 1) != 0;
                    bool is_hygienic = (v.int_value & 2) != 0;
                    // Issue #230 #2: bit 2 of int_val_ flags the
                    // `define-hygienic-macro*` (preserved-params) variant.
                    bool is_preserved = (v.int_value & 4) != 0;
                    ev.macros_[macro_name] = MacroDef{std::move(param_names),
                                                      is_dotted,
                                                      is_hygienic,
                                                      is_preserved,
                                                      flat_ptr,
                                                      pool_ptr,
                                                      body_id};
                }
            }
            return make_bool(true);
        });

    // (ir-cache-v2:dirty? name) — #t if the named define's IR cache entry is
    // marked dirty.
    add("ir-cache-v2:dirty?", [&ev, mev](const auto& a) -> EvalValue {
        if (a.size() < 1 || !is_string(a[0]))
            return mev("bad-arg", "usage: (ir-cache-v2:dirty? name)");
        if (!ev.is_define_dirty_fn_)
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= ev.string_heap_.size())
            return make_bool(false);
        auto name = ev.string_heap_[name_idx];
        return make_bool(ev.is_define_dirty_fn_(name));
    });

    // (ir-cache-v2:dependents name) — list of defines that reference this one.
    add("ir-cache-v2:dependents", [&ev, mev](const auto& a) -> EvalValue {
        if (a.size() < 1 || !is_string(a[0]))
            return mev("bad-arg", "usage: (ir-cache-v2:dependents name)");
        if (!ev.get_dependents_fn_)
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        auto name = ev.string_heap_[name_idx];
        auto names = ev.get_dependents_fn_(name);
        EvalValue list = make_void();
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            auto sid = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto head = make_string(sid);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({head, list});
            list = make_pair(pid);
        }
        return list;
    });

    // (current-source) — Return the current workspace AST as source code string
    // Implemented inline to avoid circular dependency with lowering module.
    // (eval code) — Parse and evaluate a string of Aura code
    // Issue #1071: bounds-check string_heap_ before indexing (same
    // OOB family as #1040). is_string only checks the tag.
    add("eval", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        const auto sidx = types::as_string_idx(a[0]);
        if (sidx >= ev.string_heap_.size())
            return make_void();
        auto code = ev.string_heap_[sidx];
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto pr = aura::parser::parse_to_flat(code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_void();
        flat.root = pr.root;
        auto result = ev.eval_flat(flat, pool, pr.root, ev.top_);
        if (result)
            return *result;
        return make_void();
    });

    // (load filename) — Load and evaluate a file of Aura code
    // Uses set-code + eval-current internally so definitions persist
    // in the workspace and closures are properly rooted.
    add("load", [&ev, mev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.string_heap_[idx];

        std::ifstream f(path);
        if (!f)
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        // Fresh workspace state for each load — resetting the pool corrupts
        // existing ev.top_ bindings that reference string indices from the old pool.
        ev.last_set_code_error_kind_.clear();
        ev.last_set_code_error_msg_.clear();
        ev.last_eval_current_result_.reset();

        // Use ev.temp_arena_ for the parse state so (gc-temp) reclaims it.
        // The ev.workspace_pool_ / ev.workspace_flat_ pointers below are the
        // long-lived handles; the temp allocation is just a parse scratch.
        auto alloc = ev.temp_arena_->allocator();
        auto* pool_ptr = ev.temp_arena_->create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = ev.temp_arena_->create<aura::ast::FlatAST>(alloc);

        auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return mev("parse", "load failed to parse file");
        }
        flat_ptr->root = pr.root;
        ev.workspace_flat_ = flat_ptr;
        ev.workspace_pool_ = pool_ptr;
        ev.update_shared_tree_root();
        // (ASAN fix #107 leak) delete the old index; see sibling site above.
        destroy_defuse_index();

        // Evaluate the workspace AST
        if (!ev.last_set_code_error_kind_.empty()) {
            auto msg = std::move(ev.last_set_code_error_msg_);
            return mev("load", msg);
        }
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*ev.workspace_flat_, *ev.workspace_pool_,
                                                         ev.workspace_flat_->root);
        auto result = ev.eval_flat(*ev.workspace_flat_, *ev.workspace_pool_, expanded, ev.top_);
        ev.workspace_flat_->clear_all_dirty();
        if (result)
            return *result;
        return make_void();
    });

    // (eval-expr value) — Evaluate any Aura value (not just strings)
    // Useful for evaluating stored expressions (e.g., from pipeline steps)
    add("eval-expr", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        // Convert the value to a FlatAST and evaluate.
        // Use ev.temp_arena_ so (gc-temp) reclaims the parse state per call
        // (was: ev.arena_ = monotonic = 1 FlatAST/StringPool leaked per call).
        if (!ev.arena_)
            return make_void();
        auto alloc = ev.temp_arena_->allocator();
        auto* pool = ev.temp_arena_->create<aura::ast::StringPool>(alloc);
        auto* flat = ev.temp_arena_->create<aura::ast::FlatAST>(alloc);
        auto root = ev.data_to_flat(a[0], *flat, *pool, 0);
        if (root == aura::ast::NULL_NODE)
            return make_void();
        flat->root = root;
        auto result = ev.eval_flat(*flat, *pool, root, ev.top_);
        if (result)
            return *result;
        return make_void();
    });

    add(aura::compiler::prim::kCurrentSource, [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Dual-workspace (Phase 1): default reads the per-eval current source
        // (the AST being evaluated right now), set by CompilerService::eval /
        // eval_ir / exec_jit. Optional :workspace keyword reads the persistent
        // EDSL workspace (set via (set-code ...)).
        // See dual-workspace design (archived: docs-archive-pre-2026-06)
        //
        // Use an enum to distinguish "user asked for :workspace" (even if
        // ev.workspace_flat_ is null) from "no preference" (use ev.current_flat_).
        // This matters because ev.workspace_flat_ is null until (set-code ...) is
        // called, and the test expects a different result in that case.
        enum class Source { Default, Workspace };
        Source which = Source::Default;
        if (a.size() >= 1 && types::is_keyword(a[0])) {
            auto kidx = types::as_keyword_idx(a[0]);
            if (kidx < ev.keyword_table_.size() && ev.keyword_table_[kidx] == ":workspace") {
                which = Source::Workspace;
            }
        }
        const aura::ast::FlatAST* flat = nullptr;
        const aura::ast::StringPool* pool = nullptr;
        switch (which) {
            case Source::Workspace:
                flat = ev.workspace_flat_;
                pool = ev.workspace_pool_;
                break;
            case Source::Default:
                flat = ev.current_flat_;
                pool = ev.current_pool_;
                break;
        }
        if (!flat || !pool)
            return make_string(0);

        // Inline unparse for the chosen root (captures flat/pool by ref).
        constexpr int kMaxUnparseDepth = 256;
        auto unparse = [&](this const auto& self, aura::ast::NodeId id, int indent,
                           int depth = 0) -> std::string {
            if (depth > kMaxUnparseDepth)
                return "...";
            if (id == aura::ast::NULL_NODE || id >= flat->size())
                return "()";
            auto v = flat->get(id);
            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt: {
                    if (flat->marker(id) == aura::ast::SyntaxMarker::BoolLiteral)
                        return v.int_value ? "#t" : "#f";
                    return std::to_string(v.int_value);
                }
                case aura::ast::NodeTag::LiteralFloat: {
                    auto s = std::to_string(v.float_value);
                    if (s.find('.') == std::string::npos)
                        s += ".0";
                    return s;
                }
                case aura::ast::NodeTag::LiteralString: {
                    auto raw = pool->resolve(v.sym_id);
                    std::string esc = "\"";
                    for (auto c : std::string_view(raw)) {
                        if (c == '\\' || c == '"')
                            esc += '\\';
                        esc += c;
                    }
                    esc += '"';
                    return esc;
                }
                case aura::ast::NodeTag::Variable:
                    return std::string(pool->resolve(v.sym_id));
                case aura::ast::NodeTag::Call: {
                    std::string s = "(";
                    for (std::size_t i = 0; i < v.children.size(); ++i) {
                        if (i > 0)
                            s += " ";
                        s += self(v.child(i), indent + 1, depth + 1);
                    }
                    return s + ")";
                }
                case aura::ast::NodeTag::Lambda: {
                    std::string s = "(lambda (";
                    for (std::size_t i = 0; i < v.params.size(); ++i) {
                        if (i > 0)
                            s += " ";
                        s += std::string(pool->resolve(v.params[i]));
                    }
                    s += ")";
                    if (!v.children.empty())
                        s += " " + self(v.child(0), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec: {
                    auto kw = (v.tag == aura::ast::NodeTag::LetRec) ? std::string("letrec")
                                                                    : std::string("let");
                    std::string s = "(" + kw + " ((" + std::string(pool->resolve(v.sym_id)) + " ";
                    if (!v.children.empty())
                        s += self(v.child(0), indent + 1, depth + 1);
                    s += "))";
                    if (v.children.size() > 1)
                        s += " " + self(v.child(1), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Define: {
                    return "(define " + std::string(pool->resolve(v.sym_id)) + " " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::IfExpr: {
                    std::string s = "(if";
                    for (std::size_t i = 0; i < v.children.size(); ++i)
                        s += " " + self(v.child(i), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Begin: {
                    std::string s = "(begin";
                    for (std::size_t i = 0; i < v.children.size(); ++i)
                        s += " " + self(v.child(i), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Set: {
                    return "(set! " + std::string(pool->resolve(v.sym_id)) + " " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::Quote: {
                    return "(quote " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::Pair: {
                    return "(" +
                           (v.children.empty() ? "()"
                                               : self(v.child(0), indent + 1, depth + 1) + " . " +
                                                     self(v.child(1), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::DefineModule: {
                    std::string s = "(define-module (" + std::string(pool->resolve(v.sym_id));
                    for (auto pid : v.params)
                        s += " " + std::string(pool->resolve(pid));
                    s += ")";
                    for (auto cid : v.children)
                        s += " " + self(cid, indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Export: {
                    std::string s = "(export";
                    for (auto pid : v.params)
                        s += " " + std::string(pool->resolve(pid));
                    return s + ")";
                }
                case aura::ast::NodeTag::MacroDef: {
                    std::string s = "(defmacro (" + std::string(pool->resolve(v.sym_id));
                    for (auto pid : v.params)
                        s += " " + std::string(pool->resolve(pid));
                    s += ")";
                    if (!v.children.empty())
                        s += " " + self(v.child(0), indent + 1, depth + 1);
                    return s + ")";
                }
                default: {
                    // Fallback: generic node dump for unknown types
                    return std::format("<{}>", static_cast<int>(v.tag));
                }
            }
        };

        auto src = unparse(flat->root, 0);
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(src));
        return make_string(id);
    });

    // (api-reference) — Return all registered primitives as a string for LLM reference
    // Issue #1434: list *deprecated* section (prefer engine:metrics) separately.
    add("api-reference", [&ev](const auto&) -> EvalValue {
        std::string out = "Available Aura primitives:\n\n";
        std::vector<std::string> deprecated;
        deprecated.reserve(32);
        for (std::size_t i = 0; i < ev.primitives_.slot_count(); ++i) {
            auto name = ev.primitives_.name_for_slot(i);
            if (name.empty())
                continue;
            const auto& meta = ev.primitives_.meta_for_slot(i);
            if (meta.deprecated) {
                deprecated.emplace_back(name);
                continue; // listed under *deprecated* section
            }
            out += "  " + std::string(name) + "\n";
        }
        if (!deprecated.empty()) {
            out += "\n*deprecated* (prefer (engine:metrics …) / std/engine-metrics):\n";
            for (const auto& n : deprecated)
                out += "  " + n + "  [use (engine:metrics \"" + n + "\")]\n";
        }
        out += "\nStandard library: (require std/name) loads with prefix (std/name:func-name)\n";
        out += "Or (require std/name all:) for bare names\n";
        out += "Try it: (require std/list all:) then (sort (list 3 1 2))\n";
        out += "Metrics facade: (engine:metrics) / (require \"std/engine-metrics\" all:)\n";
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(id);
    });

    // (eval-current) — Evaluate the current workspace AST
    add(aura::compiler::prim::kEvalCurrent, [&ev, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        // Phase 4: (eval-current :jit) — compile-and-run via the IR/JIT
        // pipeline. Falls back to tree-walker if the hook isn't installed
        // (e.g. unit tests without a CompilerService) or if the JIT
        // compile fails.
        if (a.size() == 1 && types::is_keyword(a[0])) {
            auto kidx = types::as_keyword_idx(a[0]);
            if (kidx < ev.keyword_table_.size() && ev.keyword_table_[kidx] == ":jit") {
                if (ev.try_jit_fn_ && ev.get_workspace_source_fn_) {
                    // Re-eval the workspace via the IR/JIT pipeline.
                    // The result is the workspace's last-expression value
                    // (no env sync back to the original workspace yet).
                    std::string src = ev.get_workspace_source_fn_();
                    if (!src.empty()) {
                        auto jit_result = ev.try_jit_fn_(src);
                        if (jit_result)
                            return *jit_result;
                        // JIT failed — fall through to tree-walker
                    }
                }
                // No service wired, or workspace empty, or JIT failed —
                // fall through to tree-walker
            }
        }
        ev.coverage_counters_[2]++;
        // If set-code failed on the last call, propagate the diagnostic immediately
        if (!ev.last_set_code_error_kind_.empty()) {
            auto kind = std::move(ev.last_set_code_error_kind_);
            auto msg = std::move(ev.last_set_code_error_msg_);
            return mev(kind, msg);
        }
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*ev.workspace_flat_, *ev.workspace_pool_,
                                                         ev.workspace_flat_->root);
        ev.coverage_counters_[4]++;

        // Incremental eval: if the workspace root is clean and we have a cached
        // result, skip full re-evaluation entirely (Issue #32b).
        // The root is marked dirty by mark_dirty_upward() on any mutation.
        // clear_cached_value is called in mark_dirty(), so we know the cache
        // is stale when dirty flags are set.
        //
        // Issue #159 Phase 2: instead of checking the root, check
        // the LAST form's subtree. The last form is what produces
        // the eval_current result; if its subtree is clean, the
        // cached result is still valid even if other parts of the
        // tree (e.g., earlier defines) are dirty. Win: mutating
        // `(define a 1)` doesn't invalidate the cache when the
        // result comes from a later form.
        {
            using aura::ast::NodeId;
            // Find the last top-level form. For a flat workspace
            // (root has children), it's the last child. For a
            // single-form workspace, it's the root itself.
            NodeId last_form = expanded;
            auto root_v = ev.workspace_flat_->get(expanded);
            if (!root_v.children.empty()) {
                last_form = root_v.child(root_v.children.size() - 1);
            }
            if (last_form != aura::ast::NULL_NODE &&
                !ev.workspace_flat_->has_dirty_subtree(last_form) && ev.last_eval_current_result_) {
                return *ev.last_eval_current_result_;
            }
        }

        auto result = ev.eval_flat(*ev.workspace_flat_, *ev.workspace_pool_, expanded, ev.top_);

        // Cache successful results for incremental reuse
        if (result)
            ev.last_eval_current_result_ = *result;

        // Issue #420: post-expand hygiene contract probe.
        ev.ensure_macro_hygiene_contract();

        // Clear dirty flags after successful eval
        ev.workspace_flat_->clear_all_dirty();
        if (!result) {
            // Return structured diagnostic: (kind-string message-string)
            auto& diag = result.error();
            auto kind = std::string(kind_name(diag.kind));
            auto msg = diag.format();
            return mev(kind, msg);
        }
        // ── Auto-fix closure: if result is an uncalled function, try (display ...) ──
        using aura::ast::NodeId;
        using aura::ast::NodeTag;
        if (is_closure(*result) && ev.workspace_flat_ && ev.workspace_pool_) {
            // Scan for Define nodes to extract function name + arity
            std::string fn_name;
            int arity = 0;
            for (NodeId nid = 0; nid < static_cast<NodeId>(ev.workspace_flat_->size()); ++nid) {
                auto nv = ev.workspace_flat_->get(nid);
                if (nv.tag == NodeTag::Define && nv.sym_id != aura::ast::INVALID_SYM) {
                    fn_name = std::string(ev.workspace_pool_->resolve(nv.sym_id));
                    arity = 0;
                    if (!nv.children.empty()) {
                        auto lambda_nv = ev.workspace_flat_->get(nv.child(0));
                        if (lambda_nv.tag == NodeTag::Lambda)
                            arity = static_cast<int>(lambda_nv.params.size());
                    }
                }
            }
            if (!fn_name.empty()) {
                // Build arg patterns based on arity
                std::vector<std::string> arg_pats;
                if (arity == 0) {
                    arg_pats = {""};
                } else if (arity == 1) {
                    // Simple scalars first, then list-based
                    // Use small ints to avoid exponential recursion (e.g., fib(42))
                    arg_pats = {"5", "0", "1", "\"test\"", "(list 3 1 4 1 5)"};
                } else if (arity == 2) {
                    // List+scalar for search, then plain scalars
                    // Try both (scalar list) and (list scalar) orderings
                    arg_pats = {"42 7",
                                "0 0",
                                "(list 1 3 5 7 9) 5",
                                "5 (list 1 3 5 7 9)",
                                "(list 3 1 4 1 5) 1",
                                "1 (list 3 1 4 1 5)"};
                } else {
                    arg_pats = {"1 2 3", "0 0 0"};
                }
                // Suppress stdout during auto-fix attempts to avoid polluting output
                std::fflush(stdout);
                int saved_stdout = ::dup(STDOUT_FILENO);
                int null_fd = ::open("/dev/null", O_WRONLY);
                if (null_fd >= 0)
                    ::dup2(null_fd, STDOUT_FILENO);
                bool auto_fixed = false;
                std::string winning_call; // the call that worked
                for (auto& args : arg_pats) {
                    std::string call_code = "(" + fn_name;
                    if (!args.empty())
                        call_code += " " + args;
                    call_code += ")";
                    aura::ast::StringPool temp_pool;
                    aura::ast::FlatAST temp_flat;
                    auto pr = aura::parser::parse_to_flat(call_code, temp_flat, temp_pool);
                    if (!pr.success || pr.root == aura::ast::NULL_NODE)
                        continue;
                    temp_flat.root = pr.root;
                    auto call_expanded =
                        aura::compiler::macro_expand_all(temp_flat, temp_pool, temp_flat.root);
                    auto call_result = ev.eval_flat(temp_flat, temp_pool, call_expanded, ev.top_);
                    if (!call_result || is_void(*call_result) || is_closure(*call_result))
                        continue;
                    auto_fixed = true;
                    winning_call = call_code;
                    break;
                }
                // Restore stdout
                std::fflush(stdout);
                ::dup2(saved_stdout, STDOUT_FILENO);
                ::close(saved_stdout);
                if (null_fd >= 0)
                    ::close(null_fd);
                // Return the auto-fix result silently (no display output)
                if (auto_fixed && !winning_call.empty()) {
                    // Winners are evaluated with original stdout restored, but we
                    // re-evaluate silently and return the raw value. Use (eval-current-output)
                    // if you need the display output for LLM consumption.
                    aura::ast::StringPool call_pool;
                    aura::ast::FlatAST call_flat;
                    auto call_pr = aura::parser::parse_to_flat(winning_call, call_flat, call_pool);
                    if (call_pr.success && call_pr.root != aura::ast::NULL_NODE) {
                        call_flat.root = call_pr.root;
                        auto call_expanded =
                            aura::compiler::macro_expand_all(call_flat, call_pool, call_flat.root);
                        auto call_result =
                            ev.eval_flat(call_flat, call_pool, call_expanded, ev.top_);
                        if (call_result) {
                            ev.coverage_counters_[9]++;
                            return *call_result;
                        }
                    }
                }
            }
        }
        return *result;
    });

    // (eval-current-output) — Evaluate workspace, return display output as string
    // Captures all display output during eval via fd-level redirection.
    add(aura::compiler::prim::kEvalCurrentOutput, [&ev, mev](const auto&) {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        // If set-code failed on the last call, propagate the diagnostic immediately
        if (!ev.last_set_code_error_kind_.empty()) {
            auto kind = std::move(ev.last_set_code_error_kind_);
            auto msg = std::move(ev.last_set_code_error_msg_);
            return mev(kind, msg);
        }
        if (!ev.workspace_flat_ || !ev.workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*ev.workspace_flat_, *ev.workspace_pool_,
                                                         ev.workspace_flat_->root);
        // Redirect stdout to a temp file (fd-level, catches fprintf too)
        std::fflush(stdout);
        auto* tmp = std::tmpfile();
        if (!tmp) {
            auto result = ev.eval_flat(*ev.workspace_flat_, *ev.workspace_pool_, expanded, ev.top_);
            ev.workspace_flat_->clear_all_dirty();
            if (!result) {
                auto msg = result.error().format();
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(msg);
                return make_string(sidx);
            }
            return *result;
        }
        int new_fd = ::fileno(tmp);
        int old_fd = ::dup(STDOUT_FILENO);
        ::dup2(new_fd, STDOUT_FILENO);
        // Run the eval
        auto result = ev.eval_flat(*ev.workspace_flat_, *ev.workspace_pool_, expanded, ev.top_);
        ev.workspace_flat_->clear_all_dirty();
        // Restore stdout
        std::fflush(stdout);
        ::dup2(old_fd, STDOUT_FILENO);
        ::close(old_fd);
        // Read captured output from temp file
        std::rewind(tmp);
        std::string captured;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
            captured.append(buf, n);
        std::fclose(tmp);
        // If eval failed, prepend structured diagnostic to captured output
        if (!result) {
            auto& diag = result.error();
            auto kind = std::string(kind_name(diag.kind));
            auto diag_str = diag.format();
            auto combined = "[" + std::string(kind) + "] " + diag_str;
            if (!captured.empty())
                combined = combined + "\n" + captured;
            captured = combined;
        }
        // Store captured output in string heap
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(captured);
        return make_string(sidx);
    });

    // (eval:async code) — Evaluate code asynchronously on the thread pool.
    // Returns the result as a string, or error message.
    // In serve-async mode, the eval runs on a background thread and the
    // calling fiber yields until the result is ready.
    // In stdin mode, falls back to synchronous eval (same as (eval code)).
    add("eval:async", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto& code = ev.string_heap_[as_string_idx(a[0])];
        if (aura::messaging::g_eval_async) {
            // Thread pool path: offload to background
            auto result_str = aura::messaging::g_eval_async(code);
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(result_str);
            return make_string(idx);
        }
        // Fallback: synchronous eval via the existing eval primitive
        auto eval_fn = ev.primitives_.lookup("eval");
        if (!eval_fn)
            return make_void();
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(code);
        auto result = (*eval_fn)({make_string(sidx)});
        // Format result as string
        auto formatted = aura::compiler::format_value(result, ev.string_heap_, ev.pairs_, 0,
                                                      &ev.primitives_, ev.keyword_table());
        auto ris = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(formatted));
        return make_string(ris);
    });


    // (typecheck-current) — Type check the workspace AST
    // Uses a persistent TypeRegistry across calls so type IDs are stable.
    //
    // Issue #159 Phase 5: skip the full traversal when the workspace
    // is clean. Caches the last result string. On a subsequent call
    // with a clean workspace (no dirty nodes anywhere), returns the
    // cached result without re-traversing. The cache is invalidated
    // implicitly by mutations: any mutation marks the root dirty via
    // mark_dirty_upward(), so has_dirty_subtree(root) becomes true
    // and we re-traverse.
    //
    // Win: repeated (typecheck-current) calls on an unchanged
    // workspace skip the O(N) tree walk entirely. Latency drops from
    // ~20us to ~1us in the cache-hit case.
    add("typecheck-current", [&ev](const auto&) {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        ev.coverage_counters_[1]++;
        if (!ev.workspace_flat_ || !ev.workspace_pool_) {
            auto eidx = ev.string_heap_.size();
            ev.string_heap_.push_back("no workspace");
            return make_string(eidx);
        }

        // Phase 5: cache check. If the workspace is clean (no dirty
        // nodes anywhere) AND we have a cached result, reuse it.
        // The cache is implicitly invalidated by mutations (which
        // mark the root dirty via mark_dirty_upward).
        if (!ev.workspace_flat_->has_dirty_subtree(ev.workspace_flat_->root) &&
            ev.last_typecheck_result_) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*ev.last_typecheck_result_);
            return make_string(sidx);
        }

        // Lazily create persistent type registry (stable TypeIds across calls)
        (void)ev.ensure_type_registry();
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        aura::compiler::TypeChecker tc(treg);
        if (ev.compiler_metrics())
            tc.set_metrics(ev.compiler_metrics());

        // 注入 declare-type 声明的自定义类型签名
        if (!ev.declared_type_sigs_.empty()) {
            std::unordered_map<std::string, std::string> sig_map;
            std::unordered_map<std::string, std::string> mod_src_map;
            for (auto& [name, decl] : ev.declared_type_sigs_) {
                sig_map[name] = decl.type_str;
                if (!decl.module_file.empty())
                    mod_src_map[name] = decl.module_file;
            }
            tc.inject_type_sigs(sig_map, mod_src_map);
        }

        aura::diag::DiagnosticCollector diag;

        auto result =
            tc.infer_flat(*ev.workspace_flat_, *ev.workspace_pool_, ev.workspace_flat_->root, diag);

        // TypeChecker now writes back normalized types via synthesize_flat + infer_flat,
        // and clears per-node dirty flags. No need for post-pass cache sync.
        // Safety clear for any nodes that may have been missed.
        ev.workspace_flat_->clear_all_dirty();

        std::string out = "type: " + treg.format_type(result) + "\n";
        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            out += "diagnostics:\n";
            for (auto& d : all_diags) {
                out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
            }
        }

        // Phase 5: cache the result for the next clean-workspace call.
        ev.last_typecheck_result_ = out;
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(out);
        return make_string(sidx);
    });

    // Issue #159 Phase 1: (typecheck-incremental) — incremental
    // typecheck using the most recent MutationRecord from the
    // mutation log. Routes through TypeChecker::infer_flat_partial
    // (the per-subtree re-inference path) instead of doing a full
    // traversal. Returns the number of nodes re-inferred
    // (cache hits don't count). On a workspace with no
    // recent mutations, returns 0 (no-op).
    //
    // Usage pattern in Aura:
    //   (mutate:rebind "f" "..." "...")
    //   (typecheck-incremental)   ; re-infer only what changed
    //   (query:type "f")
    //
    // Compared to (typecheck-current), this is faster for small
    // mutations because the type checker only walks the dirty
    // subtree. The win scales with workspace size.
    add("typecheck-incremental", [&ev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        ev.coverage_counters_[1]++;
        if (!ev.workspace_flat_ || !ev.workspace_pool_) {
            auto eidx = ev.string_heap_.size();
            ev.string_heap_.push_back("no workspace");
            return make_string(eidx);
        }
        // Find the most recent mutation record in the workspace's
        // mutation log. The log is append-only, so the last entry
        // is the most recent. If empty, no-op.
        const auto& log = ev.workspace_flat_->all_mutations();
        if (log.empty()) {
            auto ridx = ev.string_heap_.size();
            ev.string_heap_.push_back("no mutations recorded");
            return make_string(ridx);
        }
        const auto& rec = log.back();
        if (rec.target_node == aura::ast::NULL_NODE && rec.parent_id == aura::ast::NULL_NODE) {
            // Empty record — no-op.
            auto ridx = ev.string_heap_.size();
            ev.string_heap_.push_back("empty mutation record");
            return make_string(ridx);
        }
        // Lazily create persistent type registry (stable TypeIds
        // across calls) — same pattern as typecheck-current.
        (void)ev.ensure_type_registry();
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        // Run the partial re-inference. The per-node path uses
        // the cache (skip clean nodes) and re-infers only the
        // affected subtree (descendants of target_node + dirty
        // ancestors). See type_checker_impl.cpp:2901 for the
        // full algorithm.
        std::size_t re_inferred =
            tc.infer_flat_partial(*ev.workspace_flat_, *ev.workspace_pool_, rec, diag);
        std::string out = "re-inferred: " + std::to_string(re_inferred) + "\n";
        if (!diag.diagnostics().empty()) {
            out += "diagnostics:\n";
            for (auto& d : diag.diagnostics()) {
                out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
            }
        }
        auto ridx = ev.string_heap_.size();
        ev.string_heap_.push_back(out);
        return make_string(ridx);
    });

    // ── Issue #104: AuraQuery type-introspection primitives ───────────
    // These are the LLM-facing surface for "let me ask the type
    // system what it thinks". All four read from the same cache
    // populated by (typecheck-current) (the FlatAST stores a
    // normalized TypeId per node after infer_flat runs). They do
    // not trigger inference on their own — the LLM should call
    // typecheck-current first, then query.

    // (get-inferred-type node-id) — Return the cached inferred
    // type for a single AST node, formatted as a string. Returns
    // "unknown" if the node has no cached type (e.g. the LLM
    // queried before typecheck-current ran).
    add("get-inferred-type", [&ev, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (get-inferred-type node-id)");
        if (!ev.workspace_flat_ || !ev.type_registry_) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("no-typecheck-yet");
            return make_string(sidx);
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("out-of-range");
            return make_string(sidx);
        }
        auto type_idx = flat.type_id(node);
        if (type_idx == 0) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("unknown");
            return make_string(sidx);
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        std::string formatted = treg.format_type(aura::core::TypeId{type_idx, 1});
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(formatted);
        return make_string(sidx);
    });

    // (query-type-of name) — Look up a top-level definition by
    // name and return its inferred type. The Define node's child
    // (the value/lambda) holds the cached type. The LLM uses
    // this to ask "what's the type of foo?" after a typecheck.
    add("query-type-of", [&ev, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query-type-of name)");
        if (!ev.workspace_flat_ || !ev.workspace_pool_ || !ev.type_registry_) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("no-typecheck-yet");
            return make_string(sidx);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return mev("bad-arg", "string index out of range");
        auto name = ev.string_heap_[idx];
        // Phase 2.5.0: route via ev.canonical_pool() (== workspace_pool, explicit).
        auto sym = ev.canonical_pool()->intern(name);

        auto& flat = *ev.workspace_flat_;
        // Find a Define node with the matching symbol.
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                // The value of a Define is in v.children[0] (if
                // 1-arg form) or in the cached type_id of the
                // Define node itself (the 2-arg / 3-arg forms
                // also cache on the Define). Use whichever has a
                // type set.
                aura::ast::NodeId type_target = id;
                if (!v.children.empty()) {
                    auto child_type = flat.type_id(v.child(0));
                    if (child_type != 0)
                        type_target = v.child(0);
                }
                auto type_idx = flat.type_id(type_target);
                if (type_idx == 0) {
                    auto sidx = ev.string_heap_.size();
                    ev.string_heap_.push_back("unknown");
                    return make_string(sidx);
                }
                auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
                std::string formatted = treg.format_type(aura::core::TypeId{type_idx, 1});
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(formatted);
                return make_string(sidx);
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back("not-found");
        return make_string(sidx);
    });

    // (query-expected-type context) — Return the expected type
    // for a given position in the AST. The context is a small
    // s-expression describing the lookup (e.g. "param:foo" for
    // the foo parameter, "return" for the return type of the
    // current lambda, or a node-id). For now this is a thin
    // wrapper that says "Dynamic" — a real implementation would
    // inspect the surrounding lambda's param/return annotations.
    // The scaffolding here is so the LLM has a stable API name
    // to use; the implementation can deepen over time.
    add("query-expected-type", [&ev, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query-expected-type context)");
        // Placeholder: return "Dynamic" with a note. The LLM
        // sees a stable name + a sensible fallback rather than
        // an unbound-variable error.
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back("Dynamic (query-expected-type is scaffold; see #104)");
        return make_string(sidx);
    });

    // (suggest-annotation-at node-id) — Suggest a type annotation
    // for the given AST node. For Lambda nodes, emit a `[:x Int
    // body]` form with the inferred param types. For other nodes,
    // emit a wrapping `(check expr :<inferred-type>)` form. The
    // LLM can splice the suggestion into the source.
    add("suggest-annotation-at", [&ev, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (suggest-annotation-at node-id)");
        if (!ev.workspace_flat_ || !ev.type_registry_) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("no-typecheck-yet");
            return make_string(sidx);
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *ev.workspace_flat_;
        if (node >= flat.size()) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("out-of-range");
            return make_string(sidx);
        }
        auto v = flat.get(node);
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        std::string suggestion;
        if (v.tag == aura::ast::NodeTag::Lambda) {
            suggestion = "(: <params> :<inferred-fn-type> body)";
            suggestion += "  // LLM: replace with actual params and inferred type";
        } else {
            auto type_idx = flat.type_id(node);
            if (type_idx == 0) {
                suggestion = "(check <expr> :?)  // unknown type — use a type hole";
            } else {
                std::string t = treg.format_type(aura::core::TypeId{type_idx, 1});
                suggestion = "(check <expr> " + t + ")";
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(suggestion);
        return make_string(sidx);
    });

    // ── Issue #105: "Infer & Suggest Annotations" pass ──────────────────
    // (query-annotate-functions [filter])
    //
    // Issue #105 pass. Walks the FlatAST after a typecheck-current
    // has populated per-node type caches. For every top-level
    // Define whose value is a Lambda AND whose inferred function
    // type is "high confidence" (concrete — no Dynamic, no fresh
    // type variables), produces a suggested annotated signature
    // of the form:
    //
    //   (define (foo [x : Int] [y : String]) ...body)
    //
    // The body is left as "..." because the LLM is expected to
    // splice the suggested signature into the original source
    // (using the returned original-line number to locate the
    // Define). We return a list of (name . line . suggested-string)
    // triples — the LLM can iterate and apply each one.
    //
    // High-confidence = the function type's args and return are
    // NOT Dynamic (index 0) and NOT fresh type variables (rendered
    // with a leading "__t" or similar by the type formatter).
    // A function with `__t0` in its arg list is low-confidence
    // and skipped.
    //
    // Filter: optional. "all" (default) processes every top-level
    // Define. "public" processes only the ones with no underscores
    // in the name (the Aura convention for "public" is no leading
    // underscore, matching the existing private-underscore
    // convention). The filter is LLM-facing; we keep it simple.
    add("query-annotate-functions", [&ev, mev](const auto& a) -> EvalValue {
        if (!ev.workspace_flat_ || !ev.workspace_pool_ || !ev.type_registry_) {
            return mev("no-typecheck-yet",
                       "call (typecheck-current) before query-annotate-functions");
        }
        // Filter: default "all"
        std::string filter = "all";
        if (!a.empty() && is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < ev.string_heap_.size())
                filter = ev.string_heap_[idx];
        }

        auto& flat = *ev.workspace_flat_;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);

        // High-confidence gate: walk the function type's args and
        // return and reject any that is a TYPE_VAR (fresh or named)
        // or DYNAMIC. A function whose arg or return is a type
        // variable is polymorphic — there's no concrete annotation
        // to write. A function whose arg or return is Dynamic has
        // a hole somewhere in the inference; suggesting an
        // annotation would be a guess. We want concrete types only.
        std::function<bool(aura::core::TypeId)> arg_or_ret_is_open;
        arg_or_ret_is_open = [&](aura::core::TypeId t) -> bool {
            if (!t.valid())
                return true;
            if (treg.is_var(t) || t == treg.dynamic_type())
                return true;
            if (auto* f = treg.func_of(t)) {
                if (arg_or_ret_is_open(f->ret))
                    return true;
                for (auto a : f->args)
                    if (arg_or_ret_is_open(a))
                        return true;
            }
            return false;
        };
        auto is_high_conf = [&](aura::core::TypeId func_tid) -> bool {
            if (!func_tid.valid())
                return false;
            if (arg_or_ret_is_open(func_tid))
                return false;
            return true;
        };

        // Build a list of (name, line, suggestion) pairs as a
        // proper list. The Aura-side caller iterates with
        // map/filter/etc.
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            // The value is in v.children[0] (1-arg form) or the
            // cached type_id of the Define itself. Use whichever
            // is a Lambda with a cached function type.
            aura::ast::NodeId value_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            if (value_id == aura::ast::NULL_NODE)
                continue;
            auto vv = flat.get(value_id);
            if (vv.tag != aura::ast::NodeTag::Lambda)
                continue;

            // Get the function's cached type. Prefer the value's
            // cached type (more specific), fall back to the Define.
            aura::core::TypeId func_tid{0, 1};
            auto val_type_idx = flat.type_id(value_id);
            if (val_type_idx != 0) {
                func_tid = aura::core::TypeId{val_type_idx, 1};
            } else {
                auto def_type_idx = flat.type_id(id);
                if (def_type_idx != 0)
                    func_tid = aura::core::TypeId{def_type_idx, 1};
            }
            if (!is_high_conf(func_tid))
                continue;

            // Get the function structure
            auto* ft = treg.func_of(func_tid);
            if (!ft)
                continue;

            // Get the function name
            std::string fname = std::string(ev.workspace_pool_->resolve(v.sym_id));
            // Filter: "public" skips names with leading underscores
            if (filter == "public" && !fname.empty() && fname[0] == '_')
                continue;
            // Skip if the function already has param annotations
            // (no need to suggest). Check the first param's annot.
            bool already_annotated = false;
            for (auto annot_id : vv.param_annotations) {
                if (annot_id != aura::ast::NULL_NODE) {
                    already_annotated = true;
                    break;
                }
            }
            if (already_annotated)
                continue;

            // Build the suggested signature:
            //   (define (fname [p1 : T1] [p2 : T2]) ...body)
            // For now, body is "..." — the LLM splices this in
            // alongside the original body source.
            std::string sugg = "(define (" + fname;
            // params are stored as SymId spans; resolve each name
            // to its string for the suggestion.
            std::size_t n_params = vv.params.size();
            std::size_t n_args = ft->args.size();
            std::size_t count = std::min(n_params, n_args);
            for (std::size_t i = 0; i < count; ++i) {
                auto pname = std::string(ev.workspace_pool_->resolve(vv.params[i]));
                std::string ptype = treg.format_type(ft->args[i]);
                sugg += " [" + pname + " : " + ptype + "]";
            }
            sugg += ") ...body)  ;; #105 suggestion for '" + fname + "' (line ";
            sugg += std::to_string(v.line) + ")";
            if (n_params != n_args) {
                sugg += "  ;; WARNING: param/arg arity mismatch (";
                sugg += std::to_string(n_params) + " vs ";
                sugg += std::to_string(n_args) + ")";
            }

            // Push this entry as a triple: (name-line . suggestion)
            // We'll build a flat list of cons cells.
            auto fname_sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(fname);
            auto fname_v = make_string(fname_sidx);
            auto line_sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::to_string(v.line));
            auto line_v = make_string(line_sidx);
            auto sugg_sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(sugg);
            auto sugg_v = make_string(sugg_sidx);

            // cons: (sugg . (line . (name . ())))
            auto pid1 = ev.pairs_.size();
            ev.pairs_.push_back({line_v, sugg_v});
            auto pid0 = ev.pairs_.size();
            ev.pairs_.push_back({fname_v, make_pair(pid1)});
            auto head_pid = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(pid0), result});
            result = make_pair(head_pid);
        }
        return result;
    });
}

} // namespace aura::compiler::primitives_detail
