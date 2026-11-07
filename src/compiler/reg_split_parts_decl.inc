// Issue #5032: ast:restore phase state (defined in evaluator_primitives_ast.cpp).
struct AstRestoreState;
struct AstRestoreCapture;

// Peeled register parts. The struct is a friend of Evaluator, same as
// CompilePrims. Each function is one responsibility group.
struct RegSplitParts {
    using PrimRegistrar = std::function_ref<void(std::string, PrimFn)>;
    static void register_declare_type_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_generate_type_sigs_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_check_module_signature_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_persist_serialize(PrimRegistrar add, Evaluator& ev);
    static void register_persist_deserialize(PrimRegistrar add, Evaluator& ev);
    static void register_persist_info(PrimRegistrar add, Evaluator& ev);
    static void register_diagnose_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_apply_fix_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_check_preconditions_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_agent_recover_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_fiber_overlay_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_fiber_atomic_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_git_inspect_primitives(Evaluator& ev);
    static void register_git_index_primitives(Evaluator& ev);
    static void register_git_rev_parse_primitive(Evaluator& ev);
    static void register_math_regex_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                               ::aura::compiler::LockedStringHeap& string_heap,
                                               std::vector<EvalValue>& error_values,
                                               std::atomic<std::uint64_t>* primitive_error_counter,
                                               Evaluator& ev);
    static void register_math_scalar_primitives(PrimRegistrar add,
                                                ::aura::compiler::LockedStringHeap& string_heap,
                                                std::vector<EvalValue>& error_values,
                                                std::atomic<std::uint64_t>* primitive_error_counter,
                                                Evaluator& ev);
    static void register_math_integer_primitives(
        PrimRegistrar add, ::aura::compiler::LockedStringHeap& string_heap,
        std::vector<EvalValue>& error_values, std::atomic<std::uint64_t>* primitive_error_counter,
        Evaluator& ev);
    static void register_math_minmax_primitives(PrimRegistrar add,
                                                ::aura::compiler::LockedStringHeap& string_heap,
                                                std::vector<EvalValue>& error_values,
                                                std::atomic<std::uint64_t>* primitive_error_counter,
                                                Evaluator& ev);
    static void register_json_evo_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                             ::aura::compiler::LockedStringHeap& string_heap,
                                             std::vector<EvalValue>& error_values,
                                             std::atomic<std::uint64_t>* primitive_error_counter,
                                             Evaluator& ev);
    static void register_json_codec_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                               ::aura::compiler::LockedStringHeap& string_heap,
                                               std::vector<EvalValue>& error_values,
                                               std::atomic<std::uint64_t>* primitive_error_counter,
                                               Evaluator& ev);
    static void register_json_parse_primitive(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                              ::aura::compiler::LockedStringHeap& string_heap,
                                              std::vector<EvalValue>& error_values,
                                              std::atomic<std::uint64_t>* primitive_error_counter,
                                              Evaluator& ev);
    static void register_char_core_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_char_encoding_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_f64vector_core_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_f64vector_bytes_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_native_view_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_view_consistent_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_f64vector_bulk_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_handle_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_string_basics(PrimRegistrar add, Evaluator& ev,
                                       std::pmr::vector<Pair>& pairs,
                                       ::aura::compiler::LockedStringHeap& string_heap,
                                       std::vector<EvalValue>& error_values,
                                       std::atomic<std::uint64_t>* primitive_error_counter);
    static void register_pair_constructor(PrimRegistrar add, Evaluator& ev,
                                          std::pmr::vector<Pair>& pairs,
                                          ::aura::compiler::LockedStringHeap& string_heap,
                                          std::vector<EvalValue>& error_values,
                                          std::atomic<std::uint64_t>* primitive_error_counter);
    static void register_pair_shorthands_first(PrimRegistrar add, Evaluator& ev,
                                               std::pmr::vector<Pair>& pairs,
                                               ::aura::compiler::LockedStringHeap& string_heap,
                                               std::vector<EvalValue>& error_values,
                                               std::atomic<std::uint64_t>* primitive_error_counter);
    static void register_pair_shorthands_rest(PrimRegistrar add, Evaluator& ev,
                                              std::pmr::vector<Pair>& pairs,
                                              ::aura::compiler::LockedStringHeap& string_heap,
                                              std::vector<EvalValue>& error_values,
                                              std::atomic<std::uint64_t>* primitive_error_counter);
    static void register_mutable_pair_ops(PrimRegistrar add, Evaluator& ev,
                                          std::pmr::vector<Pair>& pairs,
                                          ::aura::compiler::LockedStringHeap& string_heap,
                                          std::vector<EvalValue>& error_values,
                                          std::atomic<std::uint64_t>* primitive_error_counter);
    static void register_string_ops(PrimRegistrar add, Evaluator& ev, std::pmr::vector<Pair>& pairs,
                                    ::aura::compiler::LockedStringHeap& string_heap,
                                    std::vector<EvalValue>& error_values,
                                    std::atomic<std::uint64_t>* primitive_error_counter);
    static void register_string_number_ops(PrimRegistrar add, Evaluator& ev,
                                           std::pmr::vector<Pair>& pairs,
                                           ::aura::compiler::LockedStringHeap& string_heap,
                                           std::vector<EvalValue>& error_values,
                                           std::atomic<std::uint64_t>* primitive_error_counter);
    static EvalValue apply_callback(Evaluator& ev, std::uint64_t cid,
                                    std::span<const EvalValue> args);
    static EvalValue apply_unary(Evaluator& ev, const EvalValue& fn, const EvalValue& arg,
                                 bool list_hotpath = false);
    static EvalValue apply_pred(Evaluator& ev, const EvalValue& fn, const EvalValue& arg,
                                bool list_hotpath = false);
    static EvalValue apply_binary(Evaluator& ev, const EvalValue& fn, const EvalValue& acc,
                                  const EvalValue& arg, bool list_hotpath = false);
    static EvalValue apply_n(Evaluator& ev, const EvalValue& fn, std::span<const EvalValue> args,
                             bool list_hotpath = false);
    static EvalValue walk_lists(Evaluator& ev, std::pmr::vector<Pair>& pairs,
                                ::aura::compiler::LockedStringHeap& string_heap,
                                std::vector<EvalValue>& error_values, std::span<const EvalValue> a,
                                const char* op, bool collect);
    static void register_list_basics(PrimRegistrar add, Evaluator& ev,
                                     std::pmr::vector<Pair>& pairs,
                                     ::aura::compiler::LockedStringHeap& string_heap,
                                     std::vector<EvalValue>& error_values);
    static void register_list_higher_order(PrimRegistrar add, Evaluator& ev,
                                           std::pmr::vector<Pair>& pairs,
                                           ::aura::compiler::LockedStringHeap& string_heap,
                                           std::vector<EvalValue>& error_values);
    static void register_list_extra(PrimRegistrar add, Evaluator& ev, std::pmr::vector<Pair>& pairs,
                                    ::aura::compiler::LockedStringHeap& string_heap,
                                    std::vector<EvalValue>& error_values);
    static void register_list_walk(PrimRegistrar add, Evaluator& ev, std::pmr::vector<Pair>& pairs,
                                   ::aura::compiler::LockedStringHeap& string_heap,
                                   std::vector<EvalValue>& error_values);
    static void register_list_sort(PrimRegistrar add, Evaluator& ev, std::pmr::vector<Pair>& pairs,
                                   ::aura::compiler::LockedStringHeap& string_heap,
                                   std::vector<EvalValue>& error_values);
    static void register_vector_core_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                                ::aura::compiler::LockedStringHeap& string_heap,
                                                std::vector<EvalValue>& error_values,
                                                std::vector<std::vector<EvalValue>>& vector_heap,
                                                std::atomic<std::uint64_t>* primitive_error_counter,
                                                Evaluator& ev);
    static void register_vector_seq_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                               ::aura::compiler::LockedStringHeap& string_heap,
                                               std::vector<EvalValue>& error_values,
                                               std::vector<std::vector<EvalValue>>& vector_heap,
                                               std::atomic<std::uint64_t>* primitive_error_counter,
                                               Evaluator& ev);
    static void register_vector_mutate_primitives(
        PrimRegistrar add, std::pmr::vector<Pair>& pairs,
        ::aura::compiler::LockedStringHeap& string_heap, std::vector<EvalValue>& error_values,
        std::vector<std::vector<EvalValue>>& vector_heap,
        std::atomic<std::uint64_t>* primitive_error_counter, Evaluator& ev);
    static void register_hash_basic_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                               ::aura::compiler::LockedStringHeap& string_heap,
                                               std::vector<EvalValue>& error_values,
                                               std::vector<std::vector<EvalValue>>& vector_heap,
                                               std::atomic<std::uint64_t>* primitive_error_counter,
                                               Evaluator& ev);
    static void register_hash_mutate_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                                ::aura::compiler::LockedStringHeap& string_heap,
                                                std::vector<EvalValue>& error_values,
                                                std::vector<std::vector<EvalValue>>& vector_heap,
                                                std::atomic<std::uint64_t>* primitive_error_counter,
                                                Evaluator& ev);

    // Issue #5032: ast:* registration parts. The ast TU owns one file but the
    // parts need Evaluator's private workspace/snapshot state, so they live
    // here with the other #909 peeled register parts. Each part is one
    // responsibility and stays under the 300-line ceiling.
    static void register_ast_snapshot_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_ast_snapshot_fail_reason_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_ast_introspection_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_ast_restore_primitive(PrimRegistrar add, Evaluator& ev,
                                               std::function<void()> destroy_defuse_index);
    static void register_ast_diff_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_ast_summary_primitive(
        PrimRegistrar add, Evaluator& ev,
        std::function<std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>()>
            defuse_summary_stats);
    static void register_ast_validate_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_ast_ref_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_ast_node_maintenance_primitives(PrimRegistrar add, Evaluator& ev);
    // Issue #5032: ast:snapshot / ast:diff workspace-source + line-diff helpers.
    static std::optional<std::string> workspace_source_string(Evaluator& ev);
    static EvalValue line_diff(Evaluator& ev, const std::string& old_text,
                               const std::string& new_text);
    // Issue #5032: ast:restore phases. The phase state structs are defined in
    // evaluator_primitives_ast.cpp (they only need to be incomplete here).
    static void ast_restore_capture(Evaluator& ev, AstRestoreState& state, AstRestoreCapture& cap);
    static void ast_restore_publish(Evaluator& ev, AstRestoreState& state, AstRestoreCapture& cap,
                                    const std::function<void()>& destroy_defuse_index);
    static void ast_restore_finalize(Evaluator& ev, AstRestoreState& state);
    static EvalValue ast_restore_source_fallback(Evaluator& ev, std::size_t id);
};
