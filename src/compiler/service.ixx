module;
#include <cstdint>
#include "aura_jit.h"
#include <atomic>
#include "messaging_bridge.h"
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>
#include "serve/fiber.h"

extern "C" std::int64_t aura_jit_test();
extern "C" const char* aura_jit_string_content(std::int64_t val);
extern "C" void aura_set_prim_dispatcher(std::int64_t (*fn)(std::int64_t, std::int64_t*,
                                                            std::int32_t));

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
import aura.compiler.cache;
import aura.diag;

// ── JIT primitive call dispatcher ────────────────────────
// Bridges OpPrimCall/OpPrimitive from JIT code to evaluator PrimFn table.
// The PrimId enum value is used as an index into kPrimNames to find
// the primitive name, then looked up in the evaluator's primitives table.
// Uses a global primitives pointer set by CompilerService::register_jit_primitives().

// PrimId name table (mirrors ir.ixx kPrimNames — must stay in sync)
static constexpr const char* kPrimNameTable[] = {
    "string-append", "string-length",  "string-ref",     "substring",     "string=?",
    "string<?",      "number->string", "string->number", "display",       "write",
    "newline",       "error",          "assert",         "read",          "read-file",
    "write-file",    "file-exists?",   "gensym",         "apply",         "vector",
    "vector-ref",    "vector-set!",    "vector-length",  "vector?",       "make-vector",
    "import",        "char=?",         "char<?",         "char->integer", "integer->char",
    "quotient",      "remainder",
    "length",        "list-ref",       "reverse",
    "raise",         "error?",
    "pair?",         "null?",
};

static std::atomic<const aura::compiler::Primitives*> g_jit_prim_ctx{nullptr};

extern "C" std::int64_t aura_jit_prim_dispatch(std::int64_t prim_id, std::int64_t* args,
                                               std::int32_t argc) {
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    if (!prims)
        return 0;

    // Look up primitive by PrimId → kPrimNameTable → evaluator name
    std::string_view pname;
    if (prim_id >= 0 && static_cast<std::size_t>(prim_id) < std::size(kPrimNameTable))
        pname = kPrimNameTable[static_cast<std::size_t>(prim_id)];
    if (pname.empty())
        return 0;

    auto pfn = prims->lookup(std::string(pname));
    if (!pfn)
        return 0;

    // Convert int64_t args to EvalValue vector.
    // After JIT encoding unification: args are pointer-tagged (fixnum=val<<1,
    // bool=7/3, void=11). EvalValue uses identical encoding, so pass through directly.
    std::vector<aura::compiler::types::EvalValue> eval_args;
    eval_args.reserve(static_cast<std::size_t>(argc));
    for (std::int32_t i = 0; i < argc; ++i)
        eval_args.emplace_back(args[i]);

    // Call the primitive function
    auto result = (*pfn)(eval_args);

    // Convert result back to int64_t.
    // EvalValue uses same pointer tagging, return the raw tagged value.
    return result.val;
}

namespace aura::compiler {

// Convert FlatParseResult to Diagnostic with structured location.
// Falls back to a generic parse error if no structured errors exist.
static aura::diag::Diagnostic parse_error_diag(const aura::parser::FlatParseResult& pr) {
    if (!pr.errors.empty()) {
        return {aura::diag::ErrorKind::ParseError, pr.errors[0].message, pr.errors[0].location};
    }
    return {aura::diag::ErrorKind::ParseError,
            pr.error.empty() ? "parse error" : pr.error};
}

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
    CompilerService()
        : user_bindings_{"#t", "#f", "nil"}, session_id_("default") {
        evaluator_.set_arena(&arena_);
        evaluator_.set_temp_arena(&temp_arena_);
        evaluator_.set_type_registry(&type_registry_);
        evaluator_.set_compiler_service(this);
        evaluator_.set_session_id(session_id_);
        aura::messaging::g_current_compiler_service = this;
        // Setup messaging bridge (avoids circular module dependency)
        aura::messaging::g_messaging_bridge.send =
            [](const std::string& target, const std::string& msg) -> bool {
                auto* svc = CompilerService::lookup(target);
                if (!svc) return false;
                auto* self = static_cast<CompilerService*>(
                    aura::messaging::g_current_compiler_service);
                auto sender = self ? self->session_id() : std::string("(unknown)");
                svc->push_message(sender, msg);
                return true;
            };
        aura::messaging::g_messaging_bridge.recv =
            [](int timeout_ms) -> std::optional<std::string> {
                // This relies on the compiler_service_ being set correctly,
                // which doesn't work across sessions. Instead, use the
                // current CompilerService from context.
                // For now, return empty — recv is session-specific.
                return std::nullopt;
            };
        aura::messaging::g_messaging_bridge.my_id =
            []() -> std::string {
                return "(unknown)";
            };
        // Set per-service access functions
        // Arena reset callback for benchmark task cleanup
        aura::messaging::g_reset_arena = [](void* svc) {
            if (!svc) return;
            static_cast<CompilerService*>(svc)->reset();
        };

        aura::messaging::g_mailbox_read =
            [](void* svc, int timeout_ms) -> std::optional<std::string> {
                if (!svc) return std::nullopt;
                return static_cast<CompilerService*>(svc)->pop_message(timeout_ms);
            };
        aura::messaging::g_mailbox_last_sender =
            [](void* svc) -> std::string {
                if (!svc) return "";
                return static_cast<CompilerService*>(svc)->last_sender();
            };
        aura::messaging::g_mailbox_count =
            [](void* svc) -> std::size_t {
                if (!svc) return 0;
                return static_cast<CompilerService*>(svc)->mailbox_size();
            };
        aura::messaging::g_session_id =
            [](void* svc) -> std::string {
                if (!svc) return "";
                return static_cast<CompilerService*>(svc)->session_id();
            };
        aura::messaging::g_session_exists =
            [](const std::string& id) -> bool {
                return CompilerService::lookup(id) != nullptr;
            };
        // Cache module defines in IR after each import (incl. recursive fns)
        evaluator_.set_module_loaded_callback(
            [this](const std::string& content, const std::string& path) {
                cache_module(content, path);
            });
    }

    // ── Inter-agent messaging (P0) ──────────────────────────
    void set_session_id(const std::string& id) { session_id_ = id; evaluator_.set_session_id(id); }
    std::string session_id() const { return session_id_; }
    void set_wake_eventfd(int fd) { wake_eventfd_ = fd; }

    void push_message(const std::string& sender, const std::string& msg) {
        mailbox_.push_back({sender, msg});
        if (wake_eventfd_ >= 0) {
            uint64_t _val = 1;
            ::write(wake_eventfd_, &_val, sizeof(_val));
        }
    }

    std::optional<std::string> pop_message(int timeout_ms = -1) {
        if (!mailbox_.empty()) {
            last_sender_ = mailbox_.front().first;
            auto msg = std::move(mailbox_.front().second);
            mailbox_.erase(mailbox_.begin());
            return msg;
        }
        if (timeout_ms == 0) return std::nullopt;
        // Use poll() on wake_eventfd_ for timeout-capable wait
        if (wake_eventfd_ >= 0 && timeout_ms != 0) {
            struct pollfd pfd;
            pfd.fd = wake_eventfd_;
            pfd.events = POLLIN;
            int pret = ::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : -1);
            if (pret > 0) {
                // Drain the eventfd
                uint64_t val = 0;
                ::read(wake_eventfd_, &val, sizeof(val));
            } else if (pret == 0) {
                return std::nullopt;  // timeout
            }
            // (pret < 0) = error, fall through to yield
        }
        // Fallback: try fiber yield (scheduler-based wake)
        if (!mailbox_.empty()) {
            last_sender_ = mailbox_.front().first;
            auto msg = std::move(mailbox_.front().second);
            mailbox_.erase(mailbox_.begin());
            return msg;
        }
        if (aura::serve::g_current_fiber && timeout_ms != 0) {
            aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
            aura::serve::Fiber::yield();
            if (!mailbox_.empty()) {
                last_sender_ = mailbox_.front().first;
                auto msg = std::move(mailbox_.front().second);
                mailbox_.erase(mailbox_.begin());
                return msg;
            }
        }
        return std::nullopt;
    }

    std::string last_sender() const { return last_sender_; }
    std::size_t mailbox_size() const { return mailbox_.size(); }

    static void register_session(const std::string& id, CompilerService* svc) {
        std::lock_guard lk(registry_mtx());
        registry()[id] = svc;
    }

    static void unregister_session(const std::string& id) {
        std::lock_guard lk(registry_mtx());
        registry().erase(id);
    }

    static CompilerService* lookup(const std::string& id) {
        std::lock_guard lk(registry_mtx());
        auto it = registry().find(id);
        return it != registry().end() ? it->second : nullptr;
    }

    void reset() {
        arena_.reset();
        // IR cache references arena-allocated FlatAST data;
        // must be cleared after arena reset to avoid dangling pointers.
        ir_cache_.clear();
        ir_cache_strings_.clear();
    }

    // ---- Strict mode (type errors → rejected) ------------------------
    void set_strict_mode(bool s) { strict_mode_ = s; }
    bool strict_mode() const { return strict_mode_; }

    // ---- Unified evaluation (IR-first with fallback) -----------------

    // Check if an expression needs the tree-walker evaluator.
    // IR pipeline cannot handle: EDSL primitives, quoted pairs, special forms,
    // macro definitions, error handling, or non-primitive variable references
    // (which may come from runtime imports).
    bool needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                    const aura::ast::StringPool& pool,
                                    aura::ast::NodeId root) const {
        if (root == aura::ast::NULL_NODE || root >= flat.size())
            return false;

        // ── Known names that must go through tree-walker ───────────────
        // Includes: EDSL primitives, special forms, module system operations.
        static const std::unordered_set<std::string> tree_walker_only = {
            // EDSL / AI agent primitives
            "define-type",
            "set-code",
            "eval-current",
            "apply",
            "typecheck-current",
            "typed-mutate",
            "rollback",
            "mutation-log",
            "query-mutation-log",
            "intend",
            "fiber:spawn",
            "define-strategy",
            "register-strategy!",
            "intend-history",
            "intend-analytics",
            "strategy-field",
            "strategy-set-field!",
            "strategy-inspect",
            "coverage-report",
            // C FFI — tree-walker needed to dispatch closure calls
            "c-func",
            "c-load",
            // Messaging primitives (tree-walker for argument passing)
            "send",
            "recv",
            "my-id",
            "json-encode",
            "json-get-string",
            "json-parse",
            // Special forms not in IR
            "when",
            "unless",
            "export",
            "and",
            "or",
            "cond",
            "case",
            // Capability special forms
            "with-capability",
            "check-capability",
            "capability-stack",  // DEPRECATED — uses primitive path instead
            // Module system (env side-effects)
            // "import", "use", "require" — now in lowering_known
        };

        // Root-level bare variables (like `pi`, `sort`) may come from runtime imports.
        // The lowering doesn't know about them, so fallback to tree-walker.
        if (flat.get(root).tag == aura::ast::NodeTag::Variable) {
            auto root_name = pool.resolve(flat.get(root).sym_id);
            if (evaluator_.primitives().slot_for_name(std::string(root_name)) >=
                    evaluator_.primitives().slot_count() &&
                ir_cache_.count(std::string(root_name)) == 0)
                return true;
        }

        // Type annotations: if storing a variable name (3-arg form),
        // tree-walker handles variable binding; IR/JIT cannot.
        if (flat.get(root).tag == aura::ast::NodeTag::TypeAnnotation) {
            auto root_v = flat.get(root);
            if (!root_v.children.empty()) {
                if (root_v.int_value != 0 ||
                    flat.get(root_v.child(0)).tag == aura::ast::NodeTag::Variable)
                    return true;
            }
        }

        // Names that lowering explicitly handles (special forms lowered to IR)
        // These should NOT trigger tree-walker fallback even though they're
        // not primitives or cached defines.
        static const std::unordered_set<std::string> lowering_known = {
            "try", "catch", "raise", "require", "import", "use",
        };

        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto nv = flat.get(id);

            // MacroDef cannot be lowered to IR
            if (nv.tag == aura::ast::NodeTag::MacroDef)
                return true;
            if (nv.tag == aura::ast::NodeTag::DefineType)
                return true;
            if (nv.tag == aura::ast::NodeTag::DefineModule)
                return true;

            // Dotted rest lambda cannot be lowered to IR (rest param is
            // lowered as single Arg slot, not as pair list)
            if (nv.tag == aura::ast::NodeTag::Lambda && nv.int_value != 0)
                return true;

            // TypeAnnotation with var_annot (3-arg form (: name Type val)):
            // tree-walker handles variable binding; IR/JIT cannot.
            if (nv.tag == aura::ast::NodeTag::TypeAnnotation && nv.int_value != 0)
                return true;

            // General variable references to user-defined top-level values
            // (from (define name val) where val is not a lambda) need tree-walker
            // because IR lowering can't resolve them (they live in evaluator's env).
            // Lambda params and let-bound vars are NOT in user_bindings_, so they
            // won't trigger fallback.
            // Keyword variables (:foo) are self-evaluating — need tree-walker
            if (nv.tag == aura::ast::NodeTag::Variable) {
                auto var_name = pool.resolve(nv.sym_id);
                if (!var_name.empty() && var_name[0] == ':')
                    return true;
                if (user_bindings_.count(std::string(var_name)))
                    return true;
                // Unknown variable — IR silently returns 0, tree-walker reports
                // proper unbound-variable error. Trigger fallback for correct errors.
                auto vn = std::string(var_name);
                if (!vn.empty() &&
                    evaluator_.primitives().slot_for_name(vn) >= evaluator_.primitives().slot_count() &&
                    ir_cache_.count(vn) == 0) {
                    return true;
                }
            }

            if (nv.tag == aura::ast::NodeTag::Call) {
                auto callee = nv.child(0);
                if (callee != aura::ast::NULL_NODE && callee < flat.size()) {
                    auto callee_v = flat.get(callee);
                    if (callee_v.tag == aura::ast::NodeTag::Variable) {
                        auto name = std::string(pool.resolve(callee_v.sym_id));

                        // Names starting with query: / mutate:[ ] trigger AST server fallback
                        if (name.starts_with("query:") || name.starts_with("mutate:"))
                            return true;

                        // Known tree-walker-only names (EDSL, special forms, module)
                        if (tree_walker_only.count(name))
                            return true;

                        // Catch binding forms like (catch (e) handler) have the
                        // variable binding (e) as a Call node with callee="e".
                        // These should not trigger fallback — the try lowering
                        // handles them explicitly. Skip fallback check for nodes
                        // whose parent is a catch form.
                        if (name == "catch")
                            continue;

                        // Call callee that's not a known primitive or cached define
                        // may come from a runtime import — fallback to tree-walker.
                        if (evaluator_.primitives().slot_for_name(name) >=
                                evaluator_.primitives().slot_count() &&
                            ir_cache_.count(name) == 0 && !lowering_known.count(name)) {
                            return true;
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
        if (pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        // If there were parse errors but we recovered, log them and continue
        if (!pr.success && !pr.errors.empty()) {
            for (auto& e : pr.errors)
                std::println(std::cerr, "parse warning: {}", e.format());
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // Pre-expand all macros in this expression
        auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Pre-execute top-level require/import/use calls to fill ir_cache_
        // so the remaining expression can go through the IR path without fallback.
        pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);

        // Compile-time AST validation (structural correctness)
        validate_ast(*flat_ptr, *pool_ptr, expanded_root);

        // Register ADT constructors from define-type forms (for match exhaustiveness)
        register_adt_from_define_types(*flat_ptr, *pool_ptr, expanded_root);

        // Collect match clause metadata on expanded flat (post-macro-expansion,
        // where node IDs are stable for the type checker and tree-walker)
        collect_match_info(*flat_ptr, *pool_ptr, expanded_root);

                // Check if we need the tree-walker fallback
        if (needs_tree_walker_fallback(*flat_ptr, *pool_ptr, expanded_root)) {
            auto result =
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
            // Track all bound names so subsequent eval calls don't fall
            // through to the IR pipeline (which silently returns 0 for unknown vars).
            auto track_names = [&](aura::ast::NodeId nid, auto& self) -> void {
                if (nid >= flat_ptr->size()) return;
                auto nv = flat_ptr->get(nid);
                if (nv.tag == aura::ast::NodeTag::Define)
                    user_bindings_.insert(std::string(pool_ptr->resolve(nv.sym_id)));
                if (nv.tag == aura::ast::NodeTag::TypeAnnotation && nv.int_value != 0) {
                    user_bindings_.insert(
                        std::string(pool_ptr->resolve(static_cast<aura::ast::SymId>(nv.int_value))));
                }
                if (nv.tag == aura::ast::NodeTag::Begin)
                    for (auto c : nv.children)
                        self(c, self);
            };
            track_names(expanded_root, track_names);
            return result;
        }

        // === Level 2: Type check via TypeCheckWrap pass ===
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, expanded_root, type_registry_,
                                          diags);
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError ||
                    d.kind == aura::diag::ErrorKind::Note) {
                    std::println(std::cerr, "type: {}", d.format());
                    if (d.kind == aura::diag::ErrorKind::TypeError)
                        has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                              "type error (strict mode)"});
            }
        }

        // Check for top-level (define ...) — cache IR + eval tree-walker for env persistence
        auto def = try_extract_define(*flat_ptr, *pool_ptr, expanded_root);
        if (def) {
            auto& [name, body_id] = *def;
            // Only cache function defines (Lambda body) are cached as IR
            // Value defines must go through tree-walker for env persistence
            auto body_node =
                body_id < flat_ptr->size() ? flat_ptr->get(body_id) : aura::ast::NodeView{};
            if (body_node.tag == aura::ast::NodeTag::Lambda) {
                // Function define: cache IR + eval tree-walker for env persistence
                auto result =
                    cache_define(input, *flat_ptr, *pool_ptr, expanded_root, std::string(name));
                if (!result)
                    return result;
                return EvalResult(types::make_void());
            }
            // Value define: send through tree-walker for env persistence,
            // then track the name for subsequent IR fallback detection.
            auto result =
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
            user_bindings_.insert(std::string(name));
            return result;
        }

        // ========== IR pipeline (default path for non-define expressions) ==========
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(
            *flat_ptr, *pool_ptr, arena_, cache_ptr, nullptr, &evaluator_.primitives(), nullptr,
            cache_strings_ptr);

        // Run passes (silent in default path — use eval_ir for debug)
        TypeSpecializationWrap ts(&type_registry_);
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        DeadCoercionEliminationPass dce(&type_registry_);
        ts.run(ir_mod);
        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);
        dce.run(ir_mod);

        if (ar.has_error()) {
            for (auto& d : ar.result().diagnostics) {
                std::println(std::cerr, "arity warning: {}", d.message);
            }
        }

        last_ir_mod_ = ir_mod;

// ── Try JIT execution when LLVM available ──────────────
#ifdef AURA_HAVE_LLVM
        {
            if (!jit_initialized_) {
                register_jit_primitives();
                jit_initialized_ = true;
            }

            // JIT now produces pointer-tagged values (fixnum=val<<1, bool=7/3, void=11)
            // matching EvalValue encoding, so all arithmetic and comparison ops are safe.
            static const std::unordered_set<std::string> jit_safe_primitives = {
                "+", "-", "*", "/",
                "=", "<", ">", "<=", ">=",
            };
            bool has_non_int_nodes = false;
            for (aura::ast::NodeId nid = 0; nid < flat_ptr->size(); ++nid) {
                auto nv = flat_ptr->get(nid);
                auto tag = nv.tag;
                if (tag == aura::ast::NodeTag::Lambda || tag == aura::ast::NodeTag::LiteralString ||
                    tag == aura::ast::NodeTag::LiteralFloat ||
                    tag == aura::ast::NodeTag::Coercion || tag == aura::ast::NodeTag::Quote ||
                    tag == aura::ast::NodeTag::MacroDef || tag == aura::ast::NodeTag::Let ||
                    tag == aura::ast::NodeTag::LetRec) {
                    has_non_int_nodes = true;
                    break;
                }
                if (tag == aura::ast::NodeTag::LiteralInt &&
                    nv.marker == aura::ast::SyntaxMarker::BoolLiteral) {
                    has_non_int_nodes = true;
                    break;
                }
                // Check Call nodes for non-arithmetic callees (cons, eq?, display, etc.)
                if (tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                    auto callee_id = nv.child(0);
                    if (callee_id < flat_ptr->size()) {
                        auto callee_v = flat_ptr->get(callee_id);
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto callee_name = pool_ptr->resolve(callee_v.sym_id);
                            if (!jit_safe_primitives.count(std::string(callee_name))) {
                                has_non_int_nodes = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (!has_non_int_nodes) {
                auto jit_result = try_jit_execute(ir_mod);
                if (jit_result) {
                    return *jit_result;
                }
            }
        }
#endif

        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives(),
                                                &type_registry_);
        ir_interp.set_strategy(strategy_);
        if (strict_mode_)
            ir_interp.set_strict_mode(true);

        try {

            // Set IR closure bridge: enables tree-walker primitives (map/filter/foldl)
            // to call IR-produced closures.
            evaluator_.set_closure_bridge([this,
                                           &ir_interp, &ir_mod](aura::compiler::ClosureId cid,
                                                       const std::vector<types::EvalValue>& args)
                                              -> std::optional<types::EvalValue> {
                auto snap = ir_interp.inspect_closure(cid);
                if (!snap)
                    return std::nullopt;

                aura::compiler::Env ne;
                ne.set_primitives(&evaluator_.primitives());
                for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size();
                     ++i)
                    ne.bind(snap->func_free_vars[i], snap->env[i]);
                for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                    ne.bind(snap->func_params[i], args[i]);

                // Try fast path: bridge data from current module
                if (snap->func_id < last_ir_mod_->closure_bridge.size()) {
                    auto& bd = last_ir_mod_->closure_bridge[snap->func_id];
                    if (bd.flat && bd.pool) {
                        auto r = evaluator_.eval_flat(*const_cast<ast::FlatAST*>(bd.flat),
                                                      *const_cast<ast::StringPool*>(bd.pool),
                                                      bd.body_id, ne);
                        if (r) {
                            return *r;
                        }
                    }
                    // Fallback: re-parse lambda body from body_source
                    // (needed when cached function's inner lambda has stale FlatAST ptr)
                    if (!bd.body_source.empty()) {
                        auto fallback_alloc = arena_.allocator();
                        auto* f_pool = arena_.create<aura::ast::StringPool>(fallback_alloc);
                        auto* f_flat = arena_.create<aura::ast::FlatAST>(fallback_alloc);
                        auto f_pr = aura::parser::parse_to_flat(bd.body_source, *f_flat, *f_pool);
                        if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                            f_flat->root = f_pr.root;
                            auto r = evaluator_.eval_flat(*f_flat, *f_pool, f_pr.root, ne);
                            if (r)
                                return *r;
                        }
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
                        if (define_v.tag == aura::ast::NodeTag::Define &&
                            !define_v.children.empty()) {
                            auto r = evaluator_.eval_flat(*f_flat, *f_pool, define_v.child(0), ne);
                            if (r)
                                return *r;
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
        } catch (const std::exception& e) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::TypeError, std::string("runtime type error: ") + e.what()});
        }
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
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // IR pipeline doesn't support macros — fall back to tree-walker evaluator
        for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
            if (flat_ptr->get(id).tag == aura::ast::NodeTag::MacroDef) {
                return evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root,
                                            evaluator_.top_env());
            }
        }

        // Pre-expand all macros in this expression
        auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Pre-execute top-level require/import/use calls to fill ir_cache_
        // so cached define functions are available during lowering.
        pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);

        // Update root to expanded version
        flat_ptr->root = expanded_root;

        // Compile-time AST validation
        validate_ast(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Pre-evaluate define-type forms to register constructor primitives
        // before IR lowering, so constructor calls in the code are resolvable.
        for (aura::ast::NodeId nid = 0; nid < flat_ptr->size(); ++nid) {
            if (flat_ptr->get(nid).tag == aura::ast::NodeTag::DefineType) {
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, nid, evaluator_.top_env());
            }
        }

        // Re-register ADT constructors from define-types for match exhaustiveness
        register_adt_from_define_types(*flat_ptr, *pool_ptr, flat_ptr->root);

        // === Phase 1: Define separation (IR caching) ===
        auto def = try_extract_define(*flat_ptr, *pool_ptr, flat_ptr->root);
        if (def) {
            auto& [name, _body_id] = *def;
            auto result =
                cache_define(input, *flat_ptr, *pool_ptr, flat_ptr->root, std::string(name));
            if (!result)
                return result;
            return EvalResult(types::make_void());
        }

        // === Normal IR path (with cache awareness) ===
        auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(
            *flat_ptr, *pool_ptr, arena_, cache_ptr_local, nullptr, &evaluator_.primitives(),
            nullptr, cache_strings_ptr);

        TypeSpecializationWrap ts(&type_registry_);
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;

        std::println(std::cerr, "PM: running {}->{}->{}->{}", ts.name(), ck.name(), ar.name(),
                     cf.name());

        ts.run(ir_mod);
        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);

        if (ar.has_error()) {
            for (auto& d : ar.result().diagnostics) {
                std::println(std::cerr, "arith: {}", d.message);
            }
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        if (cf.folded_count() > 0) {
            std::println(std::cerr, "PM: folded {} instructions", cf.folded_count());
        }

        last_ir_mod_ = ir_mod;

        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives(),
                                                &type_registry_);
        ir_interp.set_strategy(strategy_);
        if (strict_mode_)
            ir_interp.set_strict_mode(true);

        // Set IR closure bridge — enables tree-walker primitives (map/filter/foldl)
        // to call IR-produced closures.
        evaluator_.set_closure_bridge([this,
                                       &ir_interp](aura::compiler::ClosureId cid,
                                                   const std::vector<types::EvalValue>& args)
                                          -> std::optional<types::EvalValue> {
            auto snap = ir_interp.inspect_closure(cid);
            if (!snap)
                return std::nullopt;
            aura::compiler::Env ne;
            ne.set_primitives(&evaluator_.primitives());
            for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size(); ++i)
                ne.bind(snap->func_free_vars[i], snap->env[i]);
            for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                ne.bind(snap->func_params[i], args[i]);
            // Try bridge data from IR module
            if (snap->func_id < last_ir_mod_->closure_bridge.size()) {
                auto& bd = last_ir_mod_->closure_bridge[snap->func_id];
                if (bd.flat && bd.pool) {
                    auto r = evaluator_.eval_flat(*const_cast<ast::FlatAST*>(bd.flat),
                                                  *const_cast<ast::StringPool*>(bd.pool),
                                                  bd.body_id, ne);
                    if (r) return *r;
                }
                if (!bd.body_source.empty()) {
                    auto fallback_alloc = arena_.allocator();
                    auto* f_pool = arena_.create<aura::ast::StringPool>(fallback_alloc);
                    auto* f_flat = arena_.create<aura::ast::FlatAST>(fallback_alloc);
                    auto f_pr = aura::parser::parse_to_flat(bd.body_source, *f_flat, *f_pool);
                    if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                        f_flat->root = f_pr.root;
                        auto r = evaluator_.eval_flat(*f_flat, *f_pool, f_pr.root, ne);
                        if (r) return *r;
                    }
                }
            }
            return std::nullopt;
        });

        auto result = ir_interp.execute();

        // Clear bridge after execution
        evaluator_.set_closure_bridge(aura::compiler::Evaluator::ClosureBridgeFn());

        // Capture runtime state for --inspect
        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();

        return result;
    }

    // ── --jit: compile via LLVM ORC JIT and execute ──────────────
    // --jit: compile via LLVM ORC JIT and execute
    [[nodiscard]] EvalResult exec_jit(std::string_view input) {
#ifdef AURA_HAVE_LLVM
        // Parse expression
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // Lower to IR
        auto ir_mod = aura::compiler::lower_to_ir(*flat_ptr, *pool_ptr, arena_,
                                                  &evaluator_.primitives(), &type_registry_);

        // Run passes
        {
            aura::compiler::TypeSpecializationWrap ts(&type_registry_);
            aura::compiler::ComputeKindWrap ck;
            aura::compiler::ArityWrap ar;
            aura::compiler::ConstantFoldingWrap cf;
            ts.run(ir_mod);
            ck.run(ir_mod);
            ar.run(ir_mod);
            cf.run(ir_mod);
        }

        if (ir_mod.functions.empty()) {
            return EvalResult(types::make_void());
        }



        // Register primitives with JIT runtime (first call only)
        if (!jit_initialized_) {
            register_jit_primitives();
            jit_initialized_ = true;
        }

        // Pass string pool to JIT compiler for OpConstString support
        jit_.set_string_pool(&ir_mod.string_pool);

        // Helper: convert IR function to FlatFunction
        struct FlatFnBuilder {
            std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs;
            std::vector<aura::jit::FlatBlock> flat_blocks;
            aura::jit::FlatFunction flat_fn;
            std::string name_storage;

            FlatFnBuilder(const aura::ir::IRFunction& ir_fn) {
                flat_instrs.resize(ir_fn.blocks.size());
                flat_blocks.resize(ir_fn.blocks.size());

                for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                    auto& block = ir_fn.blocks[bi];
                    for (auto& instr : block.instructions) {
                        flat_instrs[bi].push_back({static_cast<std::uint32_t>(instr.opcode),
                                                   {instr.operands[0], instr.operands[1],
                                                    instr.operands[2], instr.operands[3]}});
                    }
                    flat_blocks[bi] = {block.id, flat_instrs[bi].data(),
                                       static_cast<std::uint32_t>(flat_instrs[bi].size())};
                }

                name_storage = ir_fn.name;
                flat_fn = {
                    name_storage.c_str(),
                    ir_fn.entry_block,
                    ir_fn.local_count,
                    ir_fn.arg_count,
                    flat_blocks.data(),
                    static_cast<std::uint32_t>(flat_blocks.size()),
                    nullptr,
                    0 // func_id_map not used for entry
                };
            }
        };

        // Compile ALL functions (with JIT cache) and register with runtime
        int64_t entry_func_id = -1;
        for (auto& ir_fn : ir_mod.functions) {
            if (ir_fn.id == ir_mod.entry_function_id) {
                entry_func_id = static_cast<int64_t>(ir_fn.id);
            }

            // env_count = number of captured free variables
            std::uint32_t env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());

            // Check JIT cache
            aura::jit::ScalarFn fn_ptr = nullptr;
            auto cache_it = jit_cache_.find(ir_fn.name);
            if (cache_it != jit_cache_.end()) {
                fn_ptr = cache_it->second.fn_ptr;
            } else {
                FlatFnBuilder builder(ir_fn);
                fn_ptr = jit_.compile(builder.flat_fn);
                if (!fn_ptr) {
                    return std::unexpected(aura::diag::Diagnostic{
                        aura::diag::ErrorKind::InternalError,
                        std::string("JIT compilation failed for function '") + ir_fn.name + "'"});
                }
                // Cache compiled function
                jit_cache_[ir_fn.name] = {fn_ptr, ir_fn.local_count, ir_fn.arg_count, env_count};
            }

            // Register with runtime for closure calls
            jit_.register_function(static_cast<int64_t>(ir_fn.id), fn_ptr, ir_fn.local_count,
                                   ir_fn.arg_count, env_count);
        }

        // Find entry function and execute it
        auto entry_it = std::find_if(
            ir_mod.functions.begin(), ir_mod.functions.end(),
            [&](const aura::ir::IRFunction& f) { return f.id == ir_mod.entry_function_id; });
        if (entry_it == ir_mod.functions.end()) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "Entry function not found"});
        }

        auto& entry = *entry_it;
        std::vector<std::int64_t> locals(entry.local_count, 0);
        auto fn_ptr = jit_.get_function_ptr(entry.name.c_str());
        if (!fn_ptr) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "JIT entry function lookup failed"});
        }

        auto raw_result =
            reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)(locals.data(), entry.arg_count);

        // ── Convert JIT result to proper EvalValue type ──
        types::EvalValue ev_result;
        std::uint32_t ret_slot = std::numeric_limits<std::uint32_t>::max();
        for (auto& block : entry.blocks)
            for (auto& instr : block.instructions)
                if (instr.opcode == aura::ir::IROpcode::Return)
                    ret_slot = instr.operands[0];
        // Follow data flow through OpLocal to find the actual producer instruction
        if (ret_slot != std::numeric_limits<std::uint32_t>::max()) {
            uint32_t search_slot = ret_slot;
            bool chasing = true;
            while (chasing) {
                chasing = false;
                for (auto& block : ir_mod.functions) {
                    for (auto& iblock : block.blocks) {
                        for (auto& instr : iblock.instructions) {
                            if (instr.operands[0] == search_slot &&
                                instr.opcode != aura::ir::IROpcode::Return) {
                                // Follow Local passthrough
                                if (instr.opcode == aura::ir::IROpcode::Local) {
                                    search_slot = instr.operands[1];
                                    chasing = true;
                                    goto found_chain;
                                }
                                switch (instr.opcode) {
                                    case aura::ir::IROpcode::ConstBool:
                                    case aura::ir::IROpcode::Eq:
                                    case aura::ir::IROpcode::Lt:
                                    case aura::ir::IROpcode::Gt:
                                    case aura::ir::IROpcode::Le:
                                    case aura::ir::IROpcode::Ge:
                                    case aura::ir::IROpcode::And:
                                    case aura::ir::IROpcode::Or:
                                    case aura::ir::IROpcode::Not:
                                        ev_result = types::make_bool(raw_result == 7);
                                        goto done;
                                    case aura::ir::IROpcode::ConstI64:
                                        ev_result = types::EvalValue(raw_result);
                                        goto done;
                                    case aura::ir::IROpcode::ConstVoid:
                                        ev_result = types::make_void();
                                        goto done;
                                    case aura::ir::IROpcode::MakePair:
                                        if (raw_result < 0)
                                            ev_result = types::make_pair(
                                                static_cast<std::uint64_t>(-raw_result - 1));
                                        else
                                            ev_result = types::make_pair(
                                                static_cast<std::uint64_t>(raw_result >> 2));
                                        goto done;
                                    case aura::ir::IROpcode::NewCell:
                                        ev_result = types::make_cell(
                                            static_cast<std::uint64_t>(raw_result));
                                        goto done;
                                    case aura::ir::IROpcode::MakeClosure:
                                        ev_result = types::make_closure(
                                            static_cast<std::uint64_t>(raw_result));
                                        goto done;
                                    case aura::ir::IROpcode::ConstF64:
                                        ev_result = types::EvalValue(raw_result);
                                        goto done;
                                    case aura::ir::IROpcode::ConstString: {
                                        auto* str_content = aura_jit_string_content(raw_result);
                                        if (str_content) {
                                            auto& sh = evaluator_.primitives().string_heap();
                                            auto new_idx = sh.size();
                                            sh.push_back(str_content);
                                            ev_result = types::make_string(new_idx);
                                        } else {
                                            ev_result = types::EvalValue(raw_result);
                                        }
                                        goto done;
                                    }
                                    case aura::ir::IROpcode::PrimCall:
                                        ev_result = types::EvalValue(raw_result);
                                        goto done;
                                    default:
                                        break;
                                }
                                goto done;  // unknown producer, fallback to value decode
                            }
                        }
                    }
                }
                break;  // no more matches
            found_chain:;
            }
        }
    done:
        // Fallback: try to decode by value pattern if IR scan didn't determine type
        if (ev_result.val == 0 && raw_result != 0) {
            if (raw_result == 11)
                ev_result = types::make_void();
            else if (raw_result == 3 || raw_result == 7)
                ev_result = types::make_bool(raw_result == 7);
            else if ((raw_result & 1) == 0 && raw_result > -10000000000000000LL)
                ev_result = types::EvalValue(raw_result); // tagged fixnum
            else
                ev_result = types::EvalValue(raw_result);
        }
        // PrimCall void-returning prims (Display/Write/Newline): void result
        if (types::is_int(ev_result) && types::as_int(ev_result) == 0 &&
            ret_slot != std::numeric_limits<std::uint32_t>::max()) {
            for (auto& block : ir_mod.functions) {
                for (auto& iblock : block.blocks) {
                    for (auto& instr : iblock.instructions) {
                        if (instr.opcode == aura::ir::IROpcode::PrimCall &&
                            (instr.operands[0] == ret_slot || instr.operands[3] == ret_slot)) {
                            auto prim_id = static_cast<aura::ir::PrimId>(instr.operands[0]);
                            if (prim_id == aura::ir::PrimId::Display ||
                                prim_id == aura::ir::PrimId::Write ||
                                prim_id == aura::ir::PrimId::Newline) {
                                ev_result = types::make_void();
                                break;
                            }
                        }
                    }
                    if (types::is_void(ev_result)) break;
                }
                if (types::is_void(ev_result)) break;
            }
        }
        return EvalResult(ev_result);
#else
        (void)input;
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "JIT not available — rebuild with LLVM"});
#endif
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
            auto diag = parse_error_diag(pr);
            return std::string("parse error: ") + diag.format();
        }
        flat.root = pr.root;

        // Use the CompilerService's persistent type_registry_ so that ADT
        // constructors registered during eval (via define-type) are visible
        // to the match exhaustiveness check.
        aura::compiler::TypeChecker tc(type_registry_);
        aura::diag::DiagnosticCollector diag;

        auto result = tc.infer_flat(flat, pool, pr.root, diag);

        std::string out;
        out += "type: " + type_registry_.format_type(result) + "\n";

        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            for (auto& d : all_diags) {
                out += d.format() + "\n";
            }
        }

        return out;
    }

    // ---- Multi-module arena support ----------------------------------

    // ---- Multi-module compilation (ArenaGroup) ----------------------

    // Get or create a per-module arena.
    ast::ASTArena& module_arena(const std::string& name,
                                std::size_t initial_size = 8 * 1024 * 1024) {
        return arena_group_.module_arena(name, initial_size);
    }

    // Reset a specific module's arena.
    void reset_module(const std::string& name) { arena_group_.reset_module(name); }

    // ── Module-level state for incremental compilation ──────────────

    // Per-module state: source content + dirty flag + dependency set.
    // When any cached function that this module depends on is redefined,
    // the module is marked dirty and will be recompiled on next access.
    struct ModuleState {
        std::string source;
        std::unordered_set<std::string> deps; // cached functions this module depends on
        bool dirty = true;                    // initially dirty (needs compile)
    };

    // ── Cache helpers ────────────────────────────────────────────

    // Cache directory for compiled modules (~/.cache/aura/modules/)
    static std::string module_cache_dir() {
        auto home = std::getenv("HOME");
        if (!home)
            return "/tmp/aura-cache/modules/";
        return std::string(home) + "/.cache/aura/modules/";
    }

    // Cache file path for a module name + source content hash.
    // The hash prevents loading stale cache when source changes.
    static std::string module_cache_path(const std::string& name, const std::string& source = "") {
        auto sanitized = name;
        if (sanitized.empty())
            sanitized = "__default__";
        for (auto& c : sanitized) {
            if (c == '/' || c == '\\' || c == ':' || c == ' ')
                c = '_';
        }
        // Append a hash of the source to invalidate on source change
        if (!source.empty()) {
            auto h = std::hash<std::string>{}(source);
            sanitized += "_" + std::to_string(h);
        }
        return module_cache_dir() + sanitized + ".abfc";
    }

    // Ensure cache directory exists
    static void ensure_cache_dir() {
        std::error_code ec;
        std::filesystem::create_directories(module_cache_dir(), ec);
    }

    // Mark a module dirty when one of its IR dependencies changes.
    void mark_module_dirty(const std::string& changed_fn) {
        for (auto& [mname, state] : module_states_) {
            if (state.deps.count(changed_fn)) {
                state.dirty = true;
            }
        }
    }

    // Check if a module is dirty and needs recompilation.
    bool is_module_dirty(const std::string& name) const {
        auto it = module_states_.find(name);
        return it == module_states_.end() || it->second.dirty;
    }

    // Recompile a module only if it's dirty.
    EvalResult reload_module(const std::string& name) {
        auto it = module_states_.find(name);
        if (it == module_states_.end()) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "module not found: " + name});
        }
        if (!it->second.dirty) {
            // Already up to date
            return EvalResult(types::make_void());
        }
        return compile_module(name, it->second.source);
    }

    // Compile a module into its own arena. Parses source, finds all
    // top-level (define ...) forms, caches each as IR, and evaluates
    // via tree-walker for environment persistence.
    //
    // Uses the module's dedicated arena instead of the main arena_.
    // On success marks the module as clean and records its dependencies.
    // Subsequent calls with the same name will detect dirty state
    // and skip recompilation if nothing changed.
    EvalResult compile_module(const std::string& name, const std::string& source) {
        // Save source for future dirty checks / reloads
        module_states_[name].source = source;

        auto& mod_arena = arena_group_.module_arena(name);
        mod_arena.reset();

        auto alloc = mod_arena.allocator();
        auto* pool_ptr = mod_arena.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = mod_arena.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(source, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;

        auto& flat = *flat_ptr;
        auto& pool = *pool_ptr;

        // Macro expand
        auto expanded = aura::compiler::macro_expand_all(flat, pool, flat.root);

        // Walk top-level defines
        struct DefFinder {
            aura::ast::FlatAST& f;
            aura::ast::StringPool& p;
            std::vector<std::pair<std::string, aura::ast::NodeId>> defs;
            void walk(aura::ast::NodeId id) {
                if (id == aura::ast::NULL_NODE || id >= f.size())
                    return;
                auto v = f.get(id);
                if (v.tag == aura::ast::NodeTag::Define) {
                    defs.emplace_back(std::string(p.resolve(v.sym_id)), id);
                }
                if (v.tag == aura::ast::NodeTag::Begin) {
                    for (auto c : v.children)
                        walk(c);
                }
            }
        };

        DefFinder finder{flat, pool, {}};
        if (expanded != aura::ast::NULL_NODE) {
            auto ev = flat.get(expanded);
            if (ev.tag == aura::ast::NodeTag::Begin)
                for (auto c : ev.children)
                    finder.walk(c);
            else
                finder.walk(expanded);
        }

        // Try disk cache: load cached IR bundles to skip lowering
        auto cache_path = module_cache_path(name, source);
        auto cached = aura::compiler::cache::open_cache(cache_path);
        bool cache_hit = cached.valid() && cached.has_ir();
        if (cache_hit) {
            auto& all_funcs = cached.ir_functions();
            for (auto& [fname, node_id] : finder.defs) {
                if (ir_cache_.count(fname))
                    continue;
                auto dv = flat.get(node_id);
                if (dv.children.empty())
                    continue;
                if (flat.get(dv.child(0)).tag != aura::ast::NodeTag::Lambda)
                    continue;
                for (auto& func : all_funcs) {
                    if (func.name == fname && func.id != cached.ir_entry()) {
                        ir_cache_[fname] = std::vector<aura::ir::IRFunction>{func};
                        function_sources_[fname] = source;
                        module_functions_[name].push_back(fname);
                        break;
                    }
                }
                (void)evaluator_.eval_flat(flat, pool, node_id, evaluator_.top_env());
            }
        }

        // Cache each define (only if not loaded from disk cache)
        if (!cache_hit) {
            // Reuse the existing cache_define logic by using main arena for
            // the define-specific lowering (lowering doesn't depend on module arena).
            // After caching, the define is available in ir_cache_.
            for (auto& [fname, node_id] : finder.defs) {
                // Only cache function defines (Lambda body)
                auto def_node = flat.get(node_id);
                if (def_node.children.empty())
                    continue;
                auto body = flat.get(def_node.child(0));
                if (body.tag != aura::ast::NodeTag::Lambda)
                    continue;

                if (ir_cache_.count(fname))
                    continue; // already cached

                // Evaluate via tree-walker for env side-effects
                auto result = evaluator_.eval_flat(flat, pool, node_id, evaluator_.top_env());
                if (!result)
                    return result;

                // Cache IR: use the define's source s-expr
                // Extract just this define expression from the source
                auto alloc2 = arena_.allocator();
                auto* p2 = arena_.create<aura::ast::StringPool>(alloc2);
                auto* f2 = arena_.create<aura::ast::FlatAST>(alloc2);
                auto pr2 = aura::parser::parse_to_flat(source, *f2, *p2);
                if (!pr2.success)
                    continue;

                // Walk to find the matching define
                auto walk_for_name = [&](aura::ast::NodeId rid,
                                         auto& self_ref) -> std::optional<aura::ast::NodeId> {
                    if (rid >= f2->size())
                        return std::nullopt;
                    auto vv = f2->get(rid);
                    if (vv.tag == aura::ast::NodeTag::Define) {
                        if (p2->resolve(vv.sym_id) == fname)
                            return rid;
                    }
                    if (vv.tag == aura::ast::NodeTag::Begin) {
                        for (auto c : vv.children) {
                            auto r = self_ref(c, self_ref);
                            if (r)
                                return r;
                        }
                    }
                    return std::nullopt;
                };
                auto define_id = walk_for_name(f2->root, walk_for_name);
                if (!define_id)
                    continue;

                f2->root = *define_id;

                auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
                auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
                std::vector<std::string> hits;
                auto ir_mod = aura::compiler::lower_to_ir_with_cache(
                    *f2, *p2, arena_, cache_ptr, &hits, &evaluator_.primitives(), nullptr,
                    cache_strings_ptr);

                // Run passes on non-entry functions
                {
                    aura::compiler::ComputeKindWrap ck;
                    aura::compiler::ConstantFoldingWrap cf;
                    for (auto& func : ir_mod.functions) {
                        if (func.id == ir_mod.entry_function_id)
                            continue;
                        ck.compute_function(func);
                        cf.fold_function(func);
                    }
                }

                std::vector<aura::ir::IRFunction> bundle;
                for (auto& func : ir_mod.functions) {
                    if (func.id != ir_mod.entry_function_id)
                        bundle.push_back(std::move(func));
                }
                ir_cache_[fname] = std::move(bundle);
                function_sources_[fname] = source;
                module_functions_[name].push_back(fname);

                for (auto& cn : hits)
                    record_dependency(fname, cn);
            }
        } // if (!cache_hit) — skip lowering when loaded from disk

        // Mark module as loaded
        loaded_modules_.insert(name);

        // Mark module clean and record dependencies from dep_graph_
        auto& state = module_states_[name];
        state.dirty = false;
        state.deps.clear();
        for (auto& [fname, _] : finder.defs) {
            auto dit = dep_graph_.find(fname);
            if (dit != dep_graph_.end()) {
                for (auto& callee : dit->second.calls)
                    state.deps.insert(callee);
            }
        }

        // Write disk cache (only when not loaded from cache)
        if (!cache_hit) {
            ensure_cache_dir();
            auto cache_path = module_cache_path(name, source);
            aura::ir::IRModule disk_mod;
            for (auto& [fname, _] : finder.defs) {
                auto it = ir_cache_.find(fname);
                if (it != ir_cache_.end()) {
                    for (auto& func : it->second)
                        disk_mod.functions.push_back(func);
                }
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
            aura::compiler::cache::write_cache(cache_path, flat, pool, flat.root, 0, &disk_mod,
                                                sig_embed.empty() ? nullptr : &sig_embed);
        }

        return EvalResult(types::make_void());
    }

    // Unload a module: reset its arena and remove cached defines.
    // Does NOT remove evaluator env bindings (they persist for the session).
    void unload_module(const std::string& name) {
        arena_group_.reset_module(name);

        // Collect all cached defines belonging to this module and remove them.
        // Since function_sources_ stores per-define source, we rebuild:
        // find all cached functions whose source matches the module source.
        std::vector<std::string> to_remove;
        for (auto& [fname, src] : function_sources_) {
            // Simple heuristic: check if this function was cached from this module.
            // We track module_name → function names via module_functions_ map.
            (void)src;
        }

        // Track module function membership via a reverse map
        if (auto it = module_functions_.find(name); it != module_functions_.end()) {
            for (auto& fname : it->second)
                to_remove.push_back(fname);
            module_functions_.erase(it);
        }

        for (auto& fname : to_remove) {
            ir_cache_.erase(fname);
            ir_cache_bridge_.erase(fname);
            ir_cache_strings_.erase(fname);
            jit_cache_.erase(fname);
            function_sources_.erase(fname);
            // Clean dep_graph
            auto dit = dep_graph_.find(fname);
            if (dit != dep_graph_.end()) {
                for (auto& callee : dit->second.calls) {
                    dep_graph_[callee].called_by.erase(
                        std::remove(dep_graph_[callee].called_by.begin(),
                                    dep_graph_[callee].called_by.end(), fname),
                        dep_graph_[callee].called_by.end());
                }
                dep_graph_.erase(dit);
            }
        }

        loaded_modules_.erase(name);
        module_states_.erase(name);

        // Remove disk cache (find by name prefix, any hash)
        auto sanitized = name;
        for (auto& c : sanitized) {
            if (c == '/' || c == '\\' || c == ':' || c == ' ')
                c = '_';
        }
        if (sanitized.empty())
            sanitized = "__default__";
        auto dir = module_cache_dir();
        try {
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                auto fn = entry.path().filename().string();
                if (fn.starts_with(sanitized) && fn.ends_with(".abfc")) {
                    aura::compiler::cache::remove_cache(entry.path().string());
                }
            }
        } catch (...) {
        }
        // Also try without hash (legacy format)
        aura::compiler::cache::remove_cache(module_cache_dir() + sanitized + ".abfc");
    }

    // Check if a module is loaded
    bool is_module_loaded(const std::string& name) const { return loaded_modules_.count(name) > 0; }

    // List loaded module names
    std::vector<std::string> loaded_modules() const {
        std::vector<std::string> result;
        for (auto& n : loaded_modules_)
            result.push_back(n);
        return result;
    }

    // ---- Diagnostics ------------------------------------------------

    ast::ArenaStats memory_stats() const {
        auto s = arena_.stats();
        s.merge(arena_group_.total_stats());
        return s;
    }

    std::vector<std::pair<std::string, ast::ArenaStats>> module_memory_stats() const {
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
            return std::unexpected(parse_error_diag(pr));
        }
        flat.root = pr.root;

        auto new_mod = aura::compiler::lower_to_ir(flat, pool, arena_, &evaluator_.primitives(),
                                                   &type_registry_);

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
        TypeSpecializationWrap ts(&type_registry_);
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        ts.run(*last_ir_mod_);
        ck.run(*last_ir_mod_);
        ar.run(*last_ir_mod_);
        cf.run(*last_ir_mod_);

        if (ar.has_error()) {
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, evaluator_.primitives(),
                                                &type_registry_);
        ir_interp.set_strategy(strategy_);
        if (strict_mode_)
            ir_interp.set_strict_mode(true);
        auto result = ir_interp.execute();

        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();
        return result;
    }

    // ---- Runtime reflection (M3 Phase 2) ------------------------------

    // Closures persisted from last IR execution
    std::vector<aura::compiler::ClosureSnapshot> last_closures() const { return last_closures_; }
    std::vector<aura::compiler::CellSnapshot> last_cells() const { return last_cells_; }
    const aura::compiler::EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const aura::compiler::EvalStrategy& s) { strategy_ = s; }

    // ---- Module caching (for on_module_loaded callback) ---------------

    // Parse module content and cache all top-level defines in ir_cache_.
    // Called by Evaluator after each successful module load.
    void cache_module(const std::string& content, const std::string& path) {
        // don't survive re-evaluation via cache_define.


        // Arena-allocate flat/pool so pointers survive (bridge data references them)
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return;
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
                if (id == aura::ast::NULL_NODE || id >= flat.size())
                    return;
                auto v = flat.get(id);
                if (v.tag == aura::ast::NodeTag::Define) {
                    auto name = pool.resolve(v.sym_id);
                    found.emplace_back(std::string(name), id);
                }
                if (v.tag == aura::ast::NodeTag::Begin) {
                    for (auto c : v.children)
                        walk(c);
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
            if (ir_cache_.count(name))
                continue; // already cached

            // Skip value defines (e.g., (define pi 3.14)) — only cache function defines
            // A function define's body is a Lambda node
            auto define_node = flat.get(node_id);
            if (define_node.children.empty())
                continue;
            auto body_node = flat.get(define_node.child(0));
            if (body_node.tag != aura::ast::NodeTag::Lambda)
                continue;

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
                    const Evaluator& eval;
                    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>&
                        ir_cache;
                    bool skip = false;
                    void walk(aura::ast::NodeId id) {
                        if (skip || id >= f.size())
                            return;
                        auto nv = f.get(id);
                        if (nv.tag == aura::ast::NodeTag::Variable) {
                            auto var_name = std::string(p.resolve(nv.sym_id));
                            if (params.count(var_name))
                                return;
                            if (eval.primitives().slot_for_name(var_name) <
                                eval.primitives().slot_count())
                                return;
                            if (ir_cache.count(var_name))
                                return;
                            skip = true;
                        }
                        for (auto c : nv.children)
                            walk(c);
                    }
                };
                FnCheck fc{flat, pool, param_names, evaluator_, ir_cache_};
                if (!body_node.children.empty())
                    fc.walk(body_node.child(0));
                skip_ir_cache_fn = fc.skip;
            }
            if (skip_ir_cache_fn) {
                continue;
            }

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
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(
                flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
                cache_strings_ptr);
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
            for (auto& func : ir_mod.functions) {
                if (func.id != ir_mod.entry_function_id)
                    bundle.push_back(std::move(func));
            }
            ir_cache_[name] = std::move(bundle);
            // Self-referencing cached functions need tree-walker fallback
            user_bindings_.insert(name);
            function_sources_[name] = content;
            module_functions_[path].push_back(name);

            for (auto& cn : cache_hits)
                record_dependency(name, cn);
            if (is_redefine)
                invalidate_function(name);
        }
    }

    // ---- Define caching (shared by eval, eval_ir, define_function) -----

    // Lower a define expression to IR, cache it, and eval tree-walker for env.
    // Returns tree-walker result (or void for success).
    EvalResult cache_define(std::string_view source, aura::ast::FlatAST& flat,
                            aura::ast::StringPool& pool, aura::ast::NodeId expanded_root,
                            const std::string& name_str) {
        bool is_redefine = ir_cache_.count(name_str) > 0;

        // === Level 2: Type check via TypeCheckWrap pass ===
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.check_before_lowering(flat, pool, expanded_root, type_registry_, diags);
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError) {
                    std::println(std::cerr, "type warning ({}): {}", name_str, d.format());
                    has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                              "type error (strict mode)"});
            }
        }

        // Check for reasons to skip IR caching:
        // 1. `require` inside function body — IR lowering treats it as ConstI64 0
        // 2. Self-recursive calls — function not in ir_cache_ yet → ConstI64 0
        // (mitigated: self_name passed to lowering for MakeClosure pre-allocation)
        // 3. Calls to non-cached, non-primitive variables — not resolvable in IR
        bool skip_ir_cache = false;
        if (expanded_root < flat.size()) {
            auto def_v = flat.get(expanded_root);
            if (def_v.tag == aura::ast::NodeTag::Define) {
                auto body_id = def_v.children.empty() ? aura::ast::NULL_NODE : def_v.child(0);
                if (body_id < flat.size()) {
                    auto body_v = flat.get(body_id);
                    if (body_v.tag == aura::ast::NodeTag::Lambda) {
                        auto lambda_body =
                            body_v.children.empty() ? aura::ast::NULL_NODE : body_v.child(0);
                        // Collect parameter names to distinguish them from external references
                        auto params_list = body_v.params;
                        std::unordered_set<std::string> param_names;
                        for (auto pid : params_list)
                            param_names.insert(std::string(pool.resolve(pid)));

                        // Walk the lambda body for variables that can't be lowered
                        struct LambdaBodyWalker {
                            const aura::ast::FlatAST& f;
                            const aura::ast::StringPool& p;
                            const std::string& self_name;
                            const std::unordered_set<std::string>& param_names;
                            const Evaluator& eval;
                            const std::unordered_map<std::string,
                                                     std::vector<aura::ir::IRFunction>>& ir_cache;
                            bool skip = false;
                            void walk(aura::ast::NodeId id) {
                                if (skip || id == aura::ast::NULL_NODE || id >= f.size())
                                    return;
                                auto nv = f.get(id);
                                if (nv.tag == aura::ast::NodeTag::Variable) {
                                    auto var_name = std::string(p.resolve(nv.sym_id));
                                    // Skip params, the function's own name (self-reference handled
                                    // by lowering), primitives, cached functions
                                    if (param_names.count(var_name))
                                        return;
                                    if (var_name == self_name)
                                        return;
                                    if (eval.primitives().slot_for_name(var_name) <
                                        eval.primitives().slot_count())
                                        return;
                                    if (ir_cache.count(var_name))
                                        return;
                                    // Unknown variable — IR will emit ConstI64 0
                                    skip = true;
                                }
                                for (auto c : nv.children)
                                    walk(c);
                            }
                        };
                        LambdaBodyWalker lbw{flat,        pool,       name_str,
                                             param_names, evaluator_, ir_cache_};
                        lbw.walk(lambda_body);
                        skip_ir_cache = lbw.skip;
                    }
                }
            }
        }

        if (skip_ir_cache) {
            // Skip IR caching — use tree-walker only. The define has already been
            // evaluated via tree-walker below, so the env bindings are correct.
            function_sources_[name_str] = std::string(source);
            module_functions_["__repl__"].push_back(name_str);
            return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
        }

        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        std::vector<std::string> cache_hits;
        // Pass self_name so lowering can emit correct MakeClosure for self-references
        auto ir_mod = aura::compiler::lower_to_ir_with_cache(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, &name_str);

        // Run passes per-function on the new function bundle
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
        ir_cache_strings_[name_str] = ir_mod.string_pool;
        function_sources_[name_str] = std::string(source);
        module_functions_["__repl__"].push_back(name_str);

        for (auto& called_name : cache_hits) {
            record_dependency(name_str, called_name);
        }

        if (is_redefine) {
            invalidate_function(name_str);
            mark_module_dirty(name_str);
        }

        // Eval tree-walker for persistent runtime bindings
        return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
    }

    // ---- Accessors ---------------------------------------------------

    ast::ASTArena& arena() { return arena_; }
    Evaluator& evaluator() { return evaluator_; }
    void set_workspace_tree(void* wt) { evaluator_.set_workspace_tree(wt); }

    // Return current number of cached define functions
    std::size_t cached_function_count() const { return ir_cache_.size(); }

    // Inspect support: expose last parsed AST (for --inspect typecheck etc.)
    const aura::ast::FlatAST& last_flat() const { return *current_ast_; }
    const aura::ast::StringPool& last_pool() const { return *current_pool_; }

    // Expose evaluator env for --inspect evaluator
    std::string inspect_env() const {
        // Format: var_name → type_name per binding
        std::string out;
        auto& env = evaluator_.top_env();
        std::size_t count = 0;
        const aura::compiler::Env* e = &env;
        while (e) {
            for (auto& b : const_cast<aura::compiler::Env&>(*e).bindings()) {
                out += "  " + b.first + " → " +
                       aura::compiler::types::format_value(b.second) + "\n";
                ++count;
            }
            e = e->parent();
        }
        return "env: " + std::to_string(count) + " bindings\n" + out;
    }

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
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // Check if root is a Define node
        if (flat_ptr->get(flat_ptr->root).tag == aura::ast::NodeTag::Define) {
            auto name = pool_ptr->resolve(flat_ptr->get(flat_ptr->root).sym_id);
            auto result =
                cache_define(code, *flat_ptr, *pool_ptr, flat_ptr->root, std::string(name));
            return result; // tree-walker result (not void — serve protocol needs return value)
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
        return evaluator_.eval_flat(*current_ast_, *current_pool_, current_ast_->root,
                                    evaluator_.top_env());
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
        std::string status; // "Committed" or "RolledBack"
    };

    // RAII transaction guard for mutation operations.
    // Records the current mutation state on construction.
    // If commit() is not called before destruction, automatically
    // rolls back all mutations since construction point.
    struct MutationTransaction {
        aura::ast::FlatAST* ast;
        std::uint64_t snapshot_id;
        bool committed = false;

        MutationTransaction(aura::ast::FlatAST& a)
            : ast(&a), snapshot_id(a.next_mutation_id()) {}

        void commit() { committed = true; }

        ~MutationTransaction() {
            if (!committed && ast) {
                ast->rollback_since(snapshot_id);
            }
        }

        // Disallow copy
        MutationTransaction(const MutationTransaction&) = delete;
        MutationTransaction& operator=(const MutationTransaction&) = delete;
        // Allow move
        MutationTransaction(MutationTransaction&& other) noexcept
            : ast(other.ast), snapshot_id(other.snapshot_id), committed(other.committed) {
            other.ast = nullptr;
        }
    };

    // Evaluate an S-expression by parsing it INTO persistent AST (current_ast_).
    // This makes all nodes co-exist in one FlatAST, so mutation primitives
    // correctly read/write the original program's nodes.
    // Uses a transaction guard: if eval fails, all side-effect mutations
    // are automatically rolled back.
    EvalResult eval_on_current(std::string_view sexpr) {
        if (!current_ast_ || !current_pool_) {
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError, "no AST loaded"});
        }
        MutationTransaction tx(*current_ast_);
        auto pr = aura::parser::parse_to_flat(sexpr, *current_ast_, *current_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        auto result =
            evaluator_.eval_flat(*current_ast_, *current_pool_, pr.root, evaluator_.top_env());
        if (result)
            tx.commit();
        return result;
    }

    // Apply a mutation expression by parsing it INTO the persistent AST.
    // Returns the mutation ID (0 on failure).
    [[nodiscard]] MutationResult typed_mutate(std::string_view sexpr) {
        if (!current_ast_ || !current_pool_) {
            return {0, false, "no AST loaded"};
        }
        // Wrap in a transaction: if evaluation fails (parse or runtime),
        // all mutations performed by the sexpr are automatically rolled back.
        MutationTransaction tx(*current_ast_);
        auto pr = aura::parser::parse_to_flat(sexpr, *current_ast_, *current_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            auto diag = parse_error_diag(pr);
            return {0, false, diag.format()};
        }
        auto result =
            evaluator_.eval_flat(*current_ast_, *current_pool_, pr.root, evaluator_.top_env());
        if (!result) {
            return {0, false, result.error().message};
        }
        auto& val = *result;
        auto mid = static_cast<std::uint64_t>(
            aura::compiler::types::is_int(val) ? aura::compiler::types::as_int(val) : 0);
        if (mid > 0) {
            tx.commit();
            return {mid, true, ""};
        }
        // If mutation returned 0, it indicates failure — transaction auto-rollbacks
        return {0, false, "mutation returned zero (failed)"};
    }

    // Query mutation log for a specific node (or all nodes if NULL_NODE).
    std::vector<MutationLogEntry>
    query_mutation_log(aura::ast::NodeId node = aura::ast::NULL_NODE) const {
        std::vector<MutationLogEntry> result;
        if (!current_ast_)
            return result;
        auto hist = (node == aura::ast::NULL_NODE) ? current_ast_->all_mutations()
                                                   : current_ast_->mutation_history(node);
        for (auto& rec : hist) {
            result.push_back(
                {rec.mutation_id, rec.timestamp_ms, rec.target_node, rec.operator_name,
                 rec.old_type_str, rec.new_type_str, rec.summary,
                 rec.status == aura::ast::MutationStatus::Committed ? "Committed" : "RolledBack"});
        }
        return result;
    }

    // Get the current persistent AST (for direct inspection).
    aura::ast::FlatAST* current_ast() const { return current_ast_; }
    aura::ast::StringPool* current_pool() const { return current_pool_; }

    // Get last compiled IR module (for --inspect dump).
    const std::optional<aura::ir::IRModule>& last_ir_module() const { return last_ir_mod_; }

private:
    // Try to extract a define/let/letrec binding from the FlatAST root.
    // Returns {name, body_node_id} if root is a Define node.
    static std::optional<std::pair<std::string, aura::ast::NodeId>>
    try_extract_define(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                       aura::ast::NodeId root) {
        if (root == aura::ast::NULL_NODE)
            return std::nullopt;
        auto v = flat.get(root);
        if (v.tag == aura::ast::NodeTag::Define) {
            auto name = pool.resolve(v.sym_id);
            aura::ast::NodeId body = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            return std::make_pair(std::string(name), body);
        }
        return std::nullopt;
    }

    // Check if a node is a require/import/use call.
    static bool is_require_call(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                aura::ast::NodeId id) {
        if (id >= flat.size())
            return false;
        auto v = flat.get(id);
        if (v.tag != aura::ast::NodeTag::Call)
            return false;
        auto callee = v.child(0);
        if (callee >= flat.size())
            return false;
        auto cv = flat.get(callee);
        if (cv.tag != aura::ast::NodeTag::Variable)
            return false;
        auto name = pool.resolve(cv.sym_id);
        return name == "require" || name == "import"; // use returns a value (module object)
    }

    // Pre-execute top-level require/import/use calls, removing them from
    // the expression so the remaining body can go through IR without fallback.
    // Returns the new root node (with requires removed), or original root.
    // Side effect: fills ir_cache_ + evaluator env via compile_module.
    aura::ast::NodeId pre_exec_requires(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                        aura::ast::NodeId root) {
        if (root >= flat.size())
            return root;
        auto v = flat.get(root);

        // Top-level standalone require: execute, no body left
        if (is_require_call(flat, pool, root)) {
            (void)evaluator_.eval_flat(flat, pool, root, evaluator_.top_env());
            return aura::ast::NULL_NODE;
        }

        // (begin ...) — scan children for require calls
        if (v.tag == aura::ast::NodeTag::Begin) {
            bool has_require = false;
            for (auto c : v.children) {
                if (is_require_call(flat, pool, c)) {
                    (void)evaluator_.eval_flat(flat, pool, c, evaluator_.top_env());
                    has_require = true;
                }
            }
            if (!has_require)
                return root; // no require → unchanged
        }

        return root;
    }

    // ── Compile-time AST validation ───────────────────────────
    // Validates macro-expanded AST for structural correctness.
    // Non-fatal: prints warnings; in strict mode becomes fatal.
    struct ValidationNote {
        aura::ast::NodeId node;
        std::string message;
    };

    void validate_ast(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                      aura::ast::NodeId root) const {
        std::vector<ValidationNote> notes;

        // Walk the AST checking structural rules
        auto walk = [&](this const auto& self, aura::ast::NodeId id) -> void {
            if (id >= flat.size())
                return;
            auto v = flat.get(id);

            switch (v.tag) {
                case aura::ast::NodeTag::IfExpr:
                    if (v.children.size() != 3)
                        notes.push_back(
                            {id,
                             "if requires 3 arguments (condition then-branch else-branch), got " +
                                 std::to_string(v.children.size())});
                    break;

                case aura::ast::NodeTag::Lambda:
                    if (v.children.empty())
                        notes.push_back({id, "lambda requires a body expression"});
                    if (v.params.empty() && !v.children.empty() &&
                        flat.get(v.child(0)).tag == aura::ast::NodeTag::Lambda) {
                        // (lambda () (lambda ...)) — ok
                    }
                    break;

                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec:
                    if (v.children.size() < 2)
                        notes.push_back(
                            {id, std::string(v.tag == aura::ast::NodeTag::Let ? "let" : "letrec") +
                                     " requires a value and body"});
                    break;

                case aura::ast::NodeTag::Define:
                    if (v.children.empty())
                        notes.push_back({id, "define requires a value expression"});
                    break;

                case aura::ast::NodeTag::Set:
                    if (v.children.empty())
                        notes.push_back({id, "set! requires a value expression"});
                    break;

                case aura::ast::NodeTag::Quote:
                    // Quote with no children is valid (quoting empty list)
                    break;

                default:
                    break;
            }

            // Recurse into children
            for (auto c : v.children)
                self(c);
        };

        if (root < flat.size())
            walk(root);

        // Print warnings (force flush so output is visible before potential crash)
        if (!notes.empty()) {
            for (auto& n : notes) {
                auto loc = flat.get(n.node);
                std::println("syntax: {}:{}: {}", loc.line, loc.col, n.message);
            }
        }
    }

    // ── Register ADT constructors in TypeRegistry (for match exhaustiveness) ──
    // ── Re-collect match clause metadata from expanded AST (stable node IDs) ──
    // The parser stores match_info on pre-expansion IDs. Macro expansion may shift
    // node IDs, so we re-derive match info from the expanded flat here.
    void collect_match_info(aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                            aura::ast::NodeId root) {
        auto is_ignore_name = [&](aura::ast::SymId sid) -> bool {
            if (sid == aura::ast::INVALID_SYM) return true;
            auto n = pool.resolve(sid);
            return n == "_" || (n.size() > 1 && n[0] == '_');
        };
        auto extract_ctor = [&](aura::ast::NodeId nid, auto& minfo) -> void {
            if (nid >= flat.size()) return;
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                auto callee_v = flat.get(nv.child(0));
                if (callee_v.tag == aura::ast::NodeTag::Variable &&
                    !is_ignore_name(callee_v.sym_id) && nv.children.size() >= 1) {
                    minfo.used_constructors.push_back(callee_v.sym_id);
                }
            }
        };
        auto walk = [&](this const auto& self, aura::ast::NodeId id) -> void {
            if (id >= flat.size()) return;
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::IfExpr && v.children.size() >= 3 &&
                v.child(0) < flat.size()) {
                auto test_v = flat.get(v.child(0));
                // Detect if: (if test body else-if-chain) — walk both branches
                // The then-branch (child 1) is a body, check its let for bindings
                auto then_id = v.child(1);
                if (then_id < flat.size()) {
                    auto then_v = flat.get(then_id);
                    // If then body is a let and we can resolve the arg to a ctor
                    if (then_v.tag == aura::ast::NodeTag::Let &&
                        !then_v.children.empty()) {
                        // Check if this let has match_info already
                        if (!flat.has_match_info(id)) {
                            aura::ast::MatchClauseInfo minfo;
                            extract_ctor(then_v.child(0), minfo);
                            flat.set_match_info(id, minfo);
                        }
                    }
                }
            }
            for (auto c : v.children) self(c);
        };
        if (root < flat.size()) walk(root);
    }

    void register_adt_from_define_types(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool,
                                        aura::ast::NodeId root) {
        auto walk = [&](this const auto& self, aura::ast::NodeId id) -> void {
            if (id >= flat.size())
                return;
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::DefineType) {
                auto type_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> ctors;
                for (auto cid : v.children) {
                    if (cid >= flat.size())
                        continue;
                    auto cv = flat.get(cid);
                    if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                        continue;
                    // First element of quoted list is constructor name
                    auto walk_quoted = cv.child(0);
                    if (walk_quoted >= flat.size())
                        continue;
                    auto wv = flat.get(walk_quoted);
                    if (wv.tag == aura::ast::NodeTag::Pair && !wv.children.empty()) {
                        auto car_id = wv.child(0);
                        if (car_id < flat.size()) {
                            auto car_v = flat.get(car_id);
                            if (car_v.tag == aura::ast::NodeTag::Variable) {
                                auto cname = std::string(pool.resolve(car_v.sym_id));
                                if (!cname.empty())
                                    ctors.push_back(cname);
                            }
                        }
                    }
                }
                if (!ctors.empty()) {
                    // Register the ADT type if not already registered, then add constructors
                    auto tid = type_registry_.lookup_type(type_name);
                    if (!tid.valid()) {
                        tid = type_registry_.register_type(aura::core::TypeTag::VARIANT,
                                                            type_name);
                    }
                    if (tid.valid())
                        type_registry_.register_adt_constructors(tid, ctors);

                }
            }
            for (auto c : v.children)
                self(c);
        };
        if (root < flat.size())
            walk(root);
    }

    // IR function cache: name → bundle of IR functions for cached defines.
    // The LAST function in the bundle is the user-defined lambda itself.
    // When inlined, all functions are added to the current module in order,
    // preserving func id references across cached calls.
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>> ir_cache_;

    // Bridge data cached alongside ir_cache_ (same keys, parallel indices).
    std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>> ir_cache_bridge_;
    // String pool cached alongside ir_cache_ (same keys).
    std::unordered_map<std::string, std::vector<std::string>> ir_cache_strings_;

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
    void track_define_dependencies(const std::string& def_name, aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool) {
        if (ir_cache_.empty())
            return;

        struct DepWalker {
            const std::string& def_name;
            aura::ast::FlatAST& flat;
            aura::ast::StringPool& pool;
            CompilerService* self;

            void walk(aura::ast::NodeId id) {
                if (id == aura::ast::NULL_NODE || id >= flat.size())
                    return;
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
                for (auto c : nv.children)
                    walk(c);
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
            if (it == dep_graph_.end())
                continue;

            for (auto& dependent : it->second.called_by) {
                if (visited.count(dependent))
                    continue;
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
        // Invalidate JIT cache for affected functions
        jit_cache_.erase(name);
        for (auto& dep_name : dependents)
            jit_cache_.erase(dep_name);

        // Clean up the original function's dep info
        auto it = dep_graph_.find(name);
        if (it != dep_graph_.end()) {
            for (auto& callee : it->second.calls) {
                auto& cb = dep_graph_[callee].called_by;
                cb.erase(std::remove(cb.begin(), cb.end(), name), cb.end());
            }
            dep_graph_.erase(name);
        }

        // Re-lower each dependent with current cache (nearest to redefined first = natural BFS
        // order)
        for (auto& dep_name : dependents) {
            auto src_it = function_sources_.find(dep_name);
            if (src_it == function_sources_.end())
                continue;

            // Re-parse the function source
            auto alloc = arena_.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE)
                continue;
            flat.root = pr.root;

            // Re-lower with current cache to detect new dependencies
            auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
            auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
            std::vector<std::string> cache_hits;
            auto ir_mod = aura::compiler::lower_to_ir_with_cache(
                flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
                cache_strings_ptr);

            // Phase 4: Run passes per-function on the re-lowered function bundle.
            {
                ComputeKindWrap ck_pass;
                ConstantFoldingWrap cf_pass;
                for (auto& func : ir_mod.functions) {
                    if (func.id == ir_mod.entry_function_id)
                        continue;
                    ck_pass.compute_function(func);
                    auto nf = cf_pass.fold_function(func);
                    if (nf > 0) {
                        std::println(std::cerr, "PM: folded {} instructions in function '{}'", nf,
                                     func.name);
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

        // Mark dependent modules dirty
        mark_module_dirty(name);
        for (auto& d : dependents)
            mark_module_dirty(d);
    }

    ast::ASTArena arena_;
    ast::ASTArena temp_arena_;
    ast::ArenaGroup arena_group_;
    Evaluator evaluator_;
    aura::compiler::EvalStrategy strategy_;
    aura::core::TypeRegistry type_registry_; // persistent type registry (L6)
    std::vector<aura::compiler::ClosureSnapshot> last_closures_;
    std::vector<aura::compiler::CellSnapshot> last_cells_;
    std::optional<aura::ir::IRModule> last_ir_mod_;

    // Set of loaded module names (for ArenaGroup tracking).
    std::unordered_set<std::string> loaded_modules_;

    // Reverse map: module_name → set of cached function names from that module.
    std::unordered_map<std::string, std::vector<std::string>> module_functions_;

    // Per-module state for incremental compilation (dirty tracking).
    std::unordered_map<std::string, ModuleState> module_states_;

    // Persistent AST for mutation workflows (set_code / typed_mutate).
    aura::ast::FlatAST* current_ast_ = nullptr;
    aura::ast::StringPool* current_pool_ = nullptr;

    // Track names defined via value define (tree-walker path) so subsequent
    // expressions referencing them fall back to tree-walker instead of IR.
    std::unordered_set<std::string> user_bindings_;

    // Strict mode: type errors → rejected instead of warnings only
    bool strict_mode_ = false;

    // Persistent JIT for --jit mode
    aura::jit::AuraJIT jit_;
    bool jit_initialized_ = false;

    // JIT function cache — maps function name → compiled function pointer + metadata
    struct JitCachedFn {
        aura::jit::ScalarFn fn_ptr = nullptr;
        std::uint32_t local_count = 0;
        std::uint32_t arg_count = 0;
        std::uint32_t env_count = 0;
    };
    std::unordered_map<std::string, JitCachedFn> jit_cache_;

    // Try to execute an IRModule via LLVM JIT
    // Returns EvalResult on success, nullopt on failure (falls back to IR interpreter)
    std::optional<types::EvalValue> try_jit_execute(const aura::ir::IRModule& ir_mod) {
        if (ir_mod.functions.empty())
            return std::nullopt;

        // Set string pool before compiling (for OpConstString)
        jit_.set_string_pool(&ir_mod.string_pool);

        // Compile ALL functions (with JIT cache) and register with runtime
        for (auto& ir_fn : ir_mod.functions) {
            std::uint32_t env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());

            // Check JIT cache
            aura::jit::ScalarFn fn_ptr = nullptr;
            auto cache_it = jit_cache_.find(ir_fn.name);
            if (cache_it != jit_cache_.end()) {
                fn_ptr = cache_it->second.fn_ptr;
            } else {
                // Build FlatFunction from IR function
                std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs(
                    ir_fn.blocks.size());
                std::vector<aura::jit::FlatBlock> flat_blocks(ir_fn.blocks.size());
                for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                    auto& block = ir_fn.blocks[bi];
                    for (auto& instr : block.instructions) {
                        flat_instrs[bi].push_back({static_cast<std::uint32_t>(instr.opcode),
                                                   {instr.operands[0], instr.operands[1],
                                                    instr.operands[2], instr.operands[3]}});
                    }
                    flat_blocks[bi] = {block.id, flat_instrs[bi].data(),
                                       static_cast<std::uint32_t>(flat_instrs[bi].size())};
                }
                aura::jit::FlatFunction flat_fn{ir_fn.name.c_str(),
                                                ir_fn.entry_block,
                                                ir_fn.local_count,
                                                ir_fn.arg_count,
                                                flat_blocks.data(),
                                                static_cast<std::uint32_t>(flat_blocks.size()),
                                                nullptr,
                                                0};

                fn_ptr = jit_.compile(flat_fn);
                if (!fn_ptr)
                    return std::nullopt;
                jit_cache_[ir_fn.name] = {fn_ptr, ir_fn.local_count, ir_fn.arg_count, env_count};
            }

            // Register with runtime for closure calls
            jit_.register_function(static_cast<int64_t>(ir_fn.id), fn_ptr, ir_fn.local_count,
                                   ir_fn.arg_count, env_count);
        }

        // Find and execute entry function
        auto entry_it = std::find_if(
            ir_mod.functions.begin(), ir_mod.functions.end(),
            [&](const aura::ir::IRFunction& f) { return f.id == ir_mod.entry_function_id; });
        if (entry_it == ir_mod.functions.end())
            return std::nullopt;

        auto& entry = *entry_it;
        std::vector<std::int64_t> locals(entry.local_count, 0);
        auto fn_ptr = jit_.get_function_ptr(entry.name.c_str());
        if (!fn_ptr)
            return std::nullopt;

        auto raw_result =
            reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)(locals.data(), entry.arg_count);

        // ── Convert JIT result to proper EvalValue type ──
        // After encoding unification: fixnums are val<<1, bools are 7/3, void is 11.
        // JIT runtime pairs (id<<2|1), closures (raw id), cells (raw id) use incompatible
        // encodings with EvalValue — we detect those by IR opcode scan.
        types::EvalValue result_type;
        std::uint32_t ret_slot = std::numeric_limits<std::uint32_t>::max();

        for (auto& block : entry.blocks)
            for (auto& instr : block.instructions)
                if (instr.opcode == aura::ir::IROpcode::Return)
                    ret_slot = instr.operands[0];

        if (ret_slot != std::numeric_limits<std::uint32_t>::max()) {
            for (auto& block : entry.blocks)
                for (auto& instr : block.instructions)
                    if (instr.operands[0] == ret_slot &&
                        instr.opcode != aura::ir::IROpcode::Return) {
                        switch (instr.opcode) {
                            case aura::ir::IROpcode::ConstBool:
                            case aura::ir::IROpcode::Eq:
                            case aura::ir::IROpcode::Lt:
                            case aura::ir::IROpcode::Gt:
                            case aura::ir::IROpcode::Le:
                            case aura::ir::IROpcode::Ge:
                            case aura::ir::IROpcode::And:
                            case aura::ir::IROpcode::Or:
                            case aura::ir::IROpcode::Not:
                                result_type = types::make_bool(raw_result == 7);
                                goto result_done;
                            case aura::ir::IROpcode::ConstI64:
                                // Tagged fixnum (val<<1), already EvalValue-compatible
                                result_type = types::EvalValue(raw_result);
                                goto result_done;
                            case aura::ir::IROpcode::ConstVoid:
                                result_type = types::make_void();
                                goto result_done;
                            case aura::ir::IROpcode::MakePair:
                                if (raw_result < 0)
                                    result_type = types::make_pair(
                                        static_cast<std::uint64_t>(-raw_result - 1));
                                else
                                    result_type = types::make_pair(
                                        static_cast<std::uint64_t>(raw_result >> 2));
                                goto result_done;
                            case aura::ir::IROpcode::NewCell:
                                result_type =
                                    types::make_cell(static_cast<std::uint64_t>(raw_result));
                                goto result_done;
                            case aura::ir::IROpcode::MakeClosure:
                                result_type =
                                    types::make_closure(static_cast<std::uint64_t>(raw_result));
                                goto result_done;
                            case aura::ir::IROpcode::PrimCall:
                                // PrimCall goes through evaluator which returns EvalValue-compatible int64
                                result_type = types::EvalValue(raw_result);
                                goto result_done;
                            default:
                                // Let fallthrough below handle it
                                break;
                        }
                        break;
                    }
        }

    result_done:
        // If the IR scan above didn't determine a type, fall back to decoding by value
        if (result_type.val == 0 && raw_result != 0) {
            if (raw_result == 11)
                result_type = types::make_void();
            else if (raw_result == 3 || raw_result == 7)
                result_type = types::make_bool(raw_result == 7);
            else if ((raw_result & 1) == 0 && raw_result > -10000000000000000LL)
                result_type = types::EvalValue(raw_result); // tagged fixnum, EvalValue-compatible
            else
                result_type = types::EvalValue(raw_result);
        }
        return result_type;
    }

    // Register evaluator primitives with JIT runtime
    void register_jit_primitives() {
        // Set the global primitives pointer for the JIT dispatcher
        g_jit_prim_ctx.store(&evaluator_.primitives(), std::memory_order_release);

// Register the dispatcher with JIT runtime
#ifdef AURA_HAVE_LLVM
        // aura_jit_prim_dispatch is defined at file scope (after imports)
        // and aura_set_prim_dispatcher is declared at file scope.
        aura_set_prim_dispatcher(aura_jit_prim_dispatch);
#endif
    }

    // ── Messaging (P14) ───────────────────────────────────────
    int wake_eventfd_ = -1;
    std::vector<std::pair<std::string, std::string>> mailbox_;  // (sender, msg)
    std::string last_sender_;
    std::string session_id_;
    std::unique_ptr<std::function<bool(const std::string&, const std::string&)>> msg_send_fn_;
    std::unique_ptr<std::function<std::optional<std::string>(int)>> msg_recv_fn_;
    std::unique_ptr<std::function<std::string()>> msg_id_fn_;

    // ── Static registry ──────────────────────────────────────
    // Using Scott Meyer's singletons to avoid ODR issues with module static members
    static std::unordered_map<std::string, CompilerService*>& registry() {
        static std::unordered_map<std::string, CompilerService*> reg;
        return reg;
    }
    static std::mutex& registry_mtx() {
        static std::mutex mtx;
        return mtx;
    }
};

} // namespace aura::compiler
