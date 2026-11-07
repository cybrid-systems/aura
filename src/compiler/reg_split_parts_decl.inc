// Issue #5033: workspace part state (defined in evaluator_primitives_workspace.cpp).
struct WorkspaceTopForm;
struct WorkspaceMergeReport;

// Issue #5032: ast:restore phase state (defined in evaluator_primitives_ast.cpp).
struct AstRestoreState;
struct AstRestoreCapture;

// Issue #5033: the messaging channel payload codec is defined in
// evaluator_primitives_messaging.cpp; only a forward declaration is needed here.
struct MessagingChannelCodec;

// Issue #5033: synthesize:optimize phase state (defined in
// evaluator_primitives_agent.cpp).
struct SynthesizeOptimizeState;
struct SpawnAgentArgs;
struct ScopeSpawnArgs;
struct ParallelIntendState;
struct ScopeJoinAllState;
struct SuperviseBatchState;
struct SuperviseTally;

// Issue #5033: mutate:query-and-replace predicate state (defined in
// evaluator_primitives_mutate.cpp).
struct QueryReplacePredicate;
struct QueryReplacePartialFail;

// Issue #5033: mutate:replace-pattern phase state (defined in
// evaluator_primitives_mutate.cpp).
struct ReplacePatternOptions;
struct ReplacePatternMatch;

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

    // Issue #5032: eval / typecheck / EDSL registration parts. The eval TU owns
    // one file, but its parts read Evaluator's private workspace, typechecker
    // and heap state, so they live here with the other #909 peeled register
    // parts. Each part is one responsibility and stays under the 300-line
    // ceiling; register_eval_primitives calls them in the original order.
    using MakeErrorVal = std::function<EvalValue(const std::string&, const std::string&)>;
    // Issue #4266: structured status shared by (typecheck-current) and
    // (typecheck-status); ok is what Agents branch on, report is text.
    struct WorkspaceTypecheckResult {
        bool ok = false;
        std::string report;
    };
    static std::optional<WorkspaceTypecheckResult> run_workspace_typecheck(Evaluator& ev);
    // Issue #5032: the set-code install phase, peeled out of the registration.
    static std::optional<EvalValue>
    set_code_install_workspace(Evaluator& ev, const MakeErrorVal& mev, const std::string& src,
                               bool& ok, std::string& set_code_src,
                               const std::function<void()>& destroy_defuse_index,
                               const std::function<void()>& republish_workspace_define_sources);
    // Issue #5032: the #4598 dirty-focus IR bind phase of (eval-current).
    static void eval_current_bind_dirty_lambdas(Evaluator& ev);
    // Issue #5032: (datum->string) reader-syntax printer (Issue #4746 limits).
    static std::optional<std::string> datum_print(Evaluator& ev, const EvalValue& v, int depth);
    static void register_set_code_primitive(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev,
                                            std::function<void()> destroy_defuse_index);
    static void register_ir_cache_primitives(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev);
    static void register_eval_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_load_primitive(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev,
                                        std::function<void()> destroy_defuse_index);
    static void register_source_inspection_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_api_reference_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_eval_current_primitive(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev);
    static void register_eval_current_output_primitive(PrimRegistrar add, Evaluator& ev,
                                                       MakeErrorVal mev);
    static void register_eval_async_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_offload_primitives(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev);
    static void register_typecheck_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_mutate_check_primitive(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev);
    static void register_typecheck_incremental_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_type_introspection_primitives(PrimRegistrar add, Evaluator& ev,
                                                       MakeErrorVal mev);
    static void register_annotate_functions_primitive(PrimRegistrar add, Evaluator& ev,
                                                      MakeErrorVal mev);
    static void register_datum_io_primitives(PrimRegistrar add, Evaluator& ev, MakeErrorVal mev);

    // Issue #5033: read / write / probe / entry / exec / listing registration
    // parts for the file+process primitive TU, plus the IO policy helpers they
    // share (defined in evaluator_primitives_file.cpp).
    static EvalValue file_deny_io(Evaluator& ev, std::string_view cap, std::string_view msg);
    static EvalValue file_deny_exec(Evaluator& ev, std::string_view msg);
    static bool file_is_regular(const std::string& path);
    static bool file_path_is_denied(std::string_view path);
    static EvalValue file_io_fail(Evaluator& ev, std::string_view op, std::string_view path,
                                  std::string_view why);
    static std::string file_errno_reason(int e);
    static EvalValue file_deny_write_scope(Evaluator& ev, std::string_view op,
                                           std::string_view path, std::string_view resolved);
    static void register_file_read_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_file_write_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_file_probe_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_file_entry_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_exec_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_directory_list_primitive(PrimRegistrar add, Evaluator& ev);

    // Issue #5033: core runtime / value / IO registration parts (defined in
    // evaluator_primitives_runtime.cpp). Each part is one primitive family and
    // register_runtime_primitives calls them in the original order.
    static void register_exit_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_equal_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_symbol_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_apply_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_print_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_format_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_error_primitives(PrimRegistrar add, Evaluator& ev);
    static void install_jit_error_hooks(Evaluator& ev);

    // Issue #5033: compile p27 parts (defined in evaluator_primitives_compile.cpp).
    // They are Evaluator friends like the #909 CompilePrims parts, but the
    // declarations live here so the compile TU splits its registration without
    // touching evaluator.ixx.
    static void register_compile_instruction_dirty_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_compile_incremental_stats_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_compile_safety_stats_primitives(PrimRegistrar add, Evaluator& ev);
    // Issue #5033: mutation / rollback / atomic-batch registration parts
    // (defined in evaluator_primitives_mutation.cpp). Each part is one
    // responsibility group and register_mutation_primitives calls them in the
    // original single-function order.
    static void register_mutation_counter_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_mutation_log_compact_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_mutation_log_pressure_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_mutation_history_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_mutation_rollback_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_mutation_log_summary_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_mutation_log_diff_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_atomic_batch_stats_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_atomic_batch_sv_stats_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_atomic_batch_snapshot_stats_primitive(PrimRegistrar add, Evaluator& ev);
    // Issue #5033: workspace primitive registration parts (defined in
    // evaluator_primitives_workspace.cpp) plus the helpers the parts share.
    // WorkspaceTopForm / WorkspaceMergeReport are defined in that TU; only
    // incomplete types are needed here.
    using ImplTable = std::unordered_map<std::string, PrimFn>;
    static EvalValue workspace_worldline_driver_error_4778(Evaluator& ev, std::string_view op,
                                                           std::uint64_t holder);
    static std::string workspace_ws_source(Evaluator& ev, std::uint32_t ws_id);
    static void workspace_detach_frames_from_pool(Evaluator& ev,
                                                  const aura::ast::StringPool* dying);
    static std::vector<WorkspaceTopForm> workspace_collect_top(const aura::ast::FlatAST& flat,
                                                               const aura::ast::StringPool& pool,
                                                               aura::ast::NodeId root);
    static void workspace_merge_splice(
        bool parent_parsed, const std::string& parent_source, const aura::ast::FlatAST& parent_flat,
        const aura::ast::StringPool& parent_pool, const std::vector<std::string>& child_order,
        std::unordered_map<std::string, std::string>& child_text,
        const std::shared_ptr<const std::unordered_map<std::string, std::string>>& merge_base_4776,
        std::vector<WorkspaceMergeReport>& reports, std::string& merged);
    static void register_workspace_snapshot_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_workspace_layering_primitives(PrimRegistrar add, Evaluator& ev,
                                                       std::shared_ptr<ImplTable> w_impls);
    static void register_workspace_inspection_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_workspace_lifecycle_primitives(PrimRegistrar add, Evaluator& ev,
                                                        std::shared_ptr<ImplTable> w_impls,
                                                        std::function<void()> destroy_defuse_index);
    static void register_workspace_memory_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_workspace_delete_primitives(PrimRegistrar add, Evaluator& ev,
                                                     std::function<void()> destroy_defuse_index);
    static void register_workspace_lock_primitives(PrimRegistrar add, Evaluator& ev,
                                                   std::shared_ptr<ImplTable> w_impls);
    static void register_workspace_sync_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_workspace_discard_primitives(PrimRegistrar add, Evaluator& ev,
                                                      std::function<void()> destroy_defuse_index);
    static void register_workspace_merge_primitives(PrimRegistrar add, Evaluator& ev,
                                                    std::shared_ptr<ImplTable> w_impls,
                                                    std::function<void()> destroy_defuse_index);
    static void register_workspace_conflict_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_workspace_dispatch_primitive(PrimRegistrar add, Evaluator& ev,
                                                      std::shared_ptr<ImplTable> w_impls);

    // Issue #5033: inter-agent messaging registration parts (defined in
    // evaluator_primitives_messaging.cpp). The channel payload codec is built
    // once per registration and passed to the channel part, so the #4763
    // encode/decode bodies keep their shape.
    static MessagingChannelCodec messaging_channel_codec(Evaluator& ev);
    static void register_messaging_core_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_messaging_mailbox_stats_primitive(Evaluator& ev);
    static void register_messaging_parallel_orch_stats_primitive(Evaluator& ev);
    static void register_messaging_fiber_spawn_primitive(PrimRegistrar add, Evaluator& ev);
    static void register_messaging_fiber_lifecycle_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_messaging_fiber_join_primitive(PrimRegistrar add, Evaluator& ev);
    static EvalValue fiber_join_publish_result(Evaluator& ev, std::int64_t fid);
    static std::optional<EvalValue> fiber_join_cached_result(Evaluator& ev, std::int64_t fid);
    static void register_messaging_scheduler_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_messaging_channel_primitives(PrimRegistrar add, Evaluator& ev,
                                                      const MessagingChannelCodec& codec);
    static void register_messaging_agent_list_primitive(PrimRegistrar add, Evaluator& ev);
    // Issue #5033: memory / arena registration parts (defined in
    // evaluator_primitives_memory.cpp). The #3269 compact-idle gate is a part
    // helper because it reads the mutation-boundary state.
    template <typename F> static EvalValue with_arena_compact_idle(Evaluator& ev, F&& body);
    static void register_memory_coverage_gc_primitives(PrimRegistrar add, Evaluator& ev,
                                                       std::function<void()> destroy_defuse_index);
    static void register_memory_heap_reset_primitives(PrimRegistrar add, Evaluator& ev,
                                                      std::function<void()> destroy_defuse_index);
    static void register_memory_gc_lifecycle_primitives(PrimRegistrar add, Evaluator& ev,
                                                        std::function<void()> destroy_defuse_index);
    static void
    register_memory_type_registry_primitives(PrimRegistrar add, Evaluator& ev,
                                             std::function<void()> destroy_defuse_index);
    static void
    register_memory_arena_compact_primitives(PrimRegistrar add, Evaluator& ev,
                                             std::function<void()> destroy_defuse_index);
    static void register_memory_densify_mutation_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_memory_dispatch_stats_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_memory_arena_policy_primitives(PrimRegistrar add, Evaluator& ev,
                                                        std::function<void()> destroy_defuse_index);
    static void register_memory_defrag_stats_primitives(PrimRegistrar add, Evaluator& ev,
                                                        std::function<void()> destroy_defuse_index);
    static void register_memory_string_pool_primitives(PrimRegistrar add, Evaluator& ev,
                                                       std::function<void()> destroy_defuse_index);
    static void register_memory_dirty_primitives(PrimRegistrar add, Evaluator& ev,
                                                 std::function<void()> destroy_defuse_index);
    static void
    register_memory_dirty_summary_primitives(PrimRegistrar add, Evaluator& ev,
                                             std::function<void()> destroy_defuse_index);
    static void register_memory_pressure_primitives(PrimRegistrar add, Evaluator& ev,
                                                    std::function<void()> destroy_defuse_index);
    static void register_memory_arena_threshold_primitives(PrimRegistrar add, Evaluator& ev);
    static void register_memory_closure_free_primitives(PrimRegistrar add, Evaluator& ev);
    // Issue #5033: stdlib production-review registration parts (defined in
    // evaluator_primitives_stdlib_review.cpp). Each part is one responsibility
    // group; the metrics accessor stays with the orchestrator and is passed in.
    template <typename MetricsFn>
    static void register_stdlib_review_dashboard_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_self_evo_pipeline_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_production_hardening_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_open_issue_sweep_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_lifetime_pin_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_production_sweep_a_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_production_sweep_b_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_dirty_provenance_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_production_sweep_c_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_stats_alias_primitives(Evaluator& ev, MetricsFn metrics);
    template <typename MetricsFn>
    static void register_production_sweep_d_primitives(Evaluator& ev, MetricsFn metrics);
    // Issue #5033: agent / synthesize / strategy registration parts (defined in
    // evaluator_primitives_agent.cpp). Each part is one responsibility group;
    // the capability-gated add wrapper stays with the orchestrator.
    template <typename AddFn> static void register_agent_loop_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_strategy_controller_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_strategy_evolution_stats_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_synthesize_template_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_synthesize_template_query_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_synthesize_define_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_synthesize_optimize_primitives(Evaluator& ev, AddFn& add,
                                                        std::function<void()> destroy_defuse_index);
    static std::string synth_optimize_classify_probe(Evaluator& ev, const EvalValue& v);
    static double synth_optimize_compute_fitness(Evaluator& ev, SynthesizeOptimizeState& st,
                                                 const std::string& src);
    static void synth_optimize_evolve(Evaluator& ev, SynthesizeOptimizeState& st);
    // Issue #5033: strategy / orch registration parts (defined in
    // evaluator_primitives_agent.cpp). Each part is one responsibility group;
    // the kCapStrategy add wrapper stays with the orchestrator.
    template <typename AddFn> static void register_intend_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_intend_history_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_intend_analytics_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_strategy_definition_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_evolve_strategy_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_parallel_intend_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn, typename AddRawFn>
    static void register_orch_spawn_agent_primitive(Evaluator& ev, AddFn& add, AddRawFn& add_raw);
    template <typename AddFn> static void register_agent_join_primitive(Evaluator& ev, AddFn& add);
    template <typename Handle, typename Scope, typename Policy, typename BuildOrchHash,
              typename AddDenyClass, typename AddIdentityPlane, typename AddReclaimed>
    static EvalValue agent_join_result(Evaluator& ev, std::span<const EvalValue> a, Handle* hp,
                                       Scope* join_scope, const Policy& policy, bool debug_report,
                                       BuildOrchHash& build_orch_hash, AddDenyClass& add_deny_class,
                                       AddIdentityPlane& add_identity_plane,
                                       AddReclaimed& add_reclaimed_pending_lifecycle);
    template <typename AddFn>
    static void register_agent_wait_reclaimed_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn> static void register_scope_spawn_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_scope_child_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn> static void register_scope_watch_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_scope_join_all_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_scope_lifecycle_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_agent_directory_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn> static void register_agent_reply_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn> static void register_agent_send_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn> static void register_agent_recv_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_orch_workflow_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_supervise_batch_primitive(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_run_workflow_primitive(Evaluator& ev, AddFn& add);
    template <typename Residual, typename BuildOrchHash, typename OrchKeywordKey>
    static EvalValue run_workflow_stages(Evaluator& ev, std::vector<EvalValue>& stage_hashes,
                                         Residual residual, BuildOrchHash& build_orch_hash,
                                         OrchKeywordKey& orch_keyword_key);
    template <typename AddFn> static void register_orch_stats_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_agent_token_primitives(Evaluator& ev, AddFn& add);
    template <typename AddFn>
    static void register_join_via_token_primitive(Evaluator& ev, AddFn& add);
    // Issue #5033: shared orch hash / keyword / handoff helpers.
    static EvalValue build_orch_hash(Evaluator& ev,
                                     std::span<const std::pair<std::string, EvalValue>> kv);
    static std::string orch_keyword_key(Evaluator& ev, const EvalValue& v);
    template <typename DenyClass>
    static void add_deny_class(Evaluator& ev, std::vector<std::pair<std::string, EvalValue>>& kv,
                               DenyClass cls, std::string_view detail, std::uint64_t retry_ms,
                               bool emit_retry);
    static void add_identity_plane(Evaluator& ev,
                                   std::vector<std::pair<std::string, EvalValue>>& kv,
                                   const char* plane);
    static void add_reclaimed_pending_lifecycle(Evaluator& ev,
                                                std::vector<std::pair<std::string, EvalValue>>& kv,
                                                bool pending);
    static bool parse_scope_addr_kw(Evaluator& ev, std::string_view k, const EvalValue& val,
                                    std::string& path, std::optional<std::int64_t>& child_index);
    template <typename Scope>
    static Scope* resolve_scope_addr(Scope& root, const std::string& path,
                                     std::optional<std::int64_t> child_index);
    static EvalValue make_scope_addr_fail(Evaluator& ev, int schema, const char* status,
                                          const char* error);
    template <typename Policy>
    static std::optional<std::string>
    resolve_join_fail_policy(std::string_view name, std::optional<std::uint32_t> max_restarts,
                             Policy& out);
    // Issue #5033: orch:spawn-agent / orch:scope-spawn phase state (defined in
    // evaluator_primitives_agent.cpp).
    template <typename BuildOrchHash, typename OrchKeywordKey, typename AddDenyClass,
              typename AddReclaimed>
    static std::optional<EvalValue>
    spawn_agent_guards(Evaluator& ev, std::span<const EvalValue> a, SpawnAgentArgs& st,
                       BuildOrchHash& build_orch_hash, OrchKeywordKey& orch_keyword_key,
                       AddDenyClass& add_deny_class, AddReclaimed& add_reclaimed_pending_lifecycle);
    template <typename BuildOrchHash, typename AddDenyClass, typename AddReclaimed>
    static EvalValue spawn_agent_body(Evaluator& ev, std::span<const EvalValue> a,
                                      SpawnAgentArgs& st, BuildOrchHash& build_orch_hash,
                                      AddDenyClass& add_deny_class,
                                      AddReclaimed& add_reclaimed_pending_lifecycle);
    template <typename Root, typename BuildOrchHash, typename OrchKeywordKey,
              typename ParseScopeAddr, typename ResolveScopeAddr, typename MakeScopeAddrFail,
              typename AddDenyClass, typename AddReclaimed>
    static std::optional<EvalValue>
    scope_spawn_target(Evaluator& ev, std::span<const EvalValue> a, Root& root, ScopeSpawnArgs& st,
                       BuildOrchHash& build_orch_hash, OrchKeywordKey& orch_keyword_key,
                       ParseScopeAddr& parse_scope_addr_kw, ResolveScopeAddr& resolve_scope_addr,
                       MakeScopeAddrFail& make_scope_addr_fail, AddDenyClass& add_deny_class,
                       AddReclaimed& add_reclaimed_pending_lifecycle);
    template <typename Scope, typename BuildOrchHash, typename AddDenyClass, typename AddReclaimed>
    static EvalValue scope_spawn_finish(Evaluator& ev, std::span<const EvalValue> a, Scope& scope,
                                        ScopeSpawnArgs& st, BuildOrchHash& build_orch_hash,
                                        AddDenyClass& add_deny_class,
                                        AddReclaimed& add_reclaimed_pending_lifecycle);
    // Issue #5033: parallel-intend phase state (defined in
    // evaluator_primitives_agent.cpp).
    static std::optional<EvalValue> parallel_intend_parse(Evaluator& ev, ParallelIntendState& st,
                                                          std::span<const EvalValue> a);
    static std::optional<EvalValue> parallel_intend_admit(Evaluator& ev, ParallelIntendState& st);
    static void parallel_intend_tasks(Evaluator& ev, ParallelIntendState& st);
    static std::optional<EvalValue> parallel_intend_join(Evaluator& ev, ParallelIntendState& st);
    static EvalValue parallel_intend_result(Evaluator& ev, ParallelIntendState& st);
    static EvalValue
    parallel_intend_build_hash(Evaluator& ev,
                               std::span<const std::pair<std::string, EvalValue>> kv);
    // Issue #5033: orch:scope-join-all phase state (defined in
    // evaluator_primitives_agent.cpp).
    template <typename Root, typename BuildOrchHash, typename OrchKeywordKey,
              typename ParseScopeAddr, typename ResolveScopeAddr, typename MakeScopeAddrFail,
              typename AddDenyClass, typename ResolveJoinFailPolicy>
    static std::optional<EvalValue>
    scope_join_all_exec(Evaluator& ev, ScopeJoinAllState& st, Root* root,
                        BuildOrchHash& build_orch_hash, OrchKeywordKey& orch_keyword_key,
                        ParseScopeAddr& parse_scope_addr_kw, ResolveScopeAddr& resolve_scope_addr,
                        MakeScopeAddrFail& make_scope_addr_fail, AddDenyClass& add_deny_class,
                        ResolveJoinFailPolicy& resolve_join_fail_policy);
    static EvalValue scope_join_all_result(Evaluator& ev, ScopeJoinAllState& st);
    // Issue #5033: orch:supervise-batch phase state (defined in
    // evaluator_primitives_agent.cpp).
    template <typename Root, typename Child>
    static void supervise_batch_arm(Evaluator& ev, std::span<const EvalValue> a, Root& root,
                                    Child& child, SuperviseBatchState& st);
    template <typename Root, typename Child, typename BuildOrchHash>
    static EvalValue supervise_batch_apply(Evaluator& ev, Root& root, Child& child,
                                           SuperviseBatchState& st, BuildOrchHash& build_orch_hash);
    // Issue #5033: query:orch-module-stats insert phases (defined in
    // evaluator_primitives_agent.cpp).
    template <typename Os, typename Kv>
    static void orch_module_stats_part1(Evaluator& ev, Os& os, Kv& insert_kv, std::uint64_t spawned,
                                        std::uint64_t joined, std::uint64_t sends,
                                        std::uint64_t recvs, std::uint64_t failures,
                                        std::uint64_t batches, std::uint64_t stable_ref_refresh,
                                        std::uint64_t steal_prov, std::uint64_t linear_prev,
                                        std::uint64_t cm_stable, std::uint64_t cm_steal,
                                        std::uint64_t cm_lin);
    template <typename Os, typename Kv>
    static void orch_module_stats_part2(Evaluator& ev, Os& os, Kv& insert_kv, std::uint64_t spawned,
                                        std::uint64_t joined, std::uint64_t sends,
                                        std::uint64_t recvs, std::uint64_t failures,
                                        std::uint64_t batches, std::uint64_t stable_ref_refresh,
                                        std::uint64_t steal_prov, std::uint64_t linear_prev,
                                        std::uint64_t cm_stable, std::uint64_t cm_steal,
                                        std::uint64_t cm_lin);
    template <typename Os, typename Kv>
    static void orch_module_stats_part3(Evaluator& ev, Os& os, Kv& insert_kv, std::uint64_t spawned,
                                        std::uint64_t joined, std::uint64_t sends,
                                        std::uint64_t recvs, std::uint64_t failures,
                                        std::uint64_t batches, std::uint64_t stable_ref_refresh,
                                        std::uint64_t steal_prov, std::uint64_t linear_prev,
                                        std::uint64_t cm_stable, std::uint64_t cm_steal,
                                        std::uint64_t cm_lin);
    template <typename Os, typename Kv>
    static void orch_module_stats_part4(Evaluator& ev, Os& os, Kv& insert_kv, std::uint64_t spawned,
                                        std::uint64_t joined, std::uint64_t sends,
                                        std::uint64_t recvs, std::uint64_t failures,
                                        std::uint64_t batches, std::uint64_t stable_ref_refresh,
                                        std::uint64_t steal_prov, std::uint64_t linear_prev,
                                        std::uint64_t cm_stable, std::uint64_t cm_steal,
                                        std::uint64_t cm_lin);
    // Issue #5033: mutate:* registration parts (defined in
    // evaluator_primitives_mutate.cpp). Each part is one responsibility group and
    // re-binds the shared registration helpers; order is the original order.
    template <typename Ctx> static void register_mutate_fingerprint_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_pin_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_replace_value_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_record_patch_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_query_replace_primitive(Ctx& cx);
    // Issue #5033: mutate:query-and-replace phases + shared node->source printer.
    static std::string mutate_node_source(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeId root_id);
    static std::optional<EvalValue> query_replace_collect_matches(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
        const std::vector<QueryReplacePredicate>& predicates,
        std::vector<aura::ast::FlatAST::StableNodeRef>& matches, bool& ok,
        const MakeErrorVal& mev);
    static std::optional<EvalValue> query_replace_apply_matches(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
        std::vector<aura::ast::FlatAST::StableNodeRef>& matches,
        const std::string& repl_template, std::size_t dot_count, const std::string& summary,
        bool allow_macro_qar, bool no_auto_restamp_qar, const MakeErrorVal& mev, bool& ok,
        int& replaced, std::vector<aura::ast::NodeId>& replaced_roots,
        std::uint64_t initial_mutation_id);
    template <typename Ctx> static void register_mutate_query_replace_batch_primitive(Ctx& cx);
    static std::optional<EvalValue> query_replace_batch_apply_matches(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
        std::vector<aura::ast::FlatAST::StableNodeRef>& matches,
        std::vector<QueryReplacePartialFail>& partial_fails, const std::string& repl_template,
        std::size_t dot_count, const std::string& summary, const std::string& hygiene_keep_mode,
        bool allow_macro_batch, bool no_auto_restamp_batch, const MakeErrorVal& mev, bool& ok,
        int& replaced, std::vector<aura::ast::NodeId>& replaced_roots, std::size_t& hygiene_kept,
        std::uint64_t initial_mutation_id, std::size_t size_before_batch);
    template <typename Ctx> static void register_mutate_stable_ref_policy_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_pattern_index_policy_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_as_stable_ref_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_rebind_primitive(Ctx& cx);
    // Issue #5033: mutate:rebind phases (preflight / target / parse / link / dirty /
    // invalidate / typecheck+gate / env refresh).
    template <typename Guard>
    static std::optional<EvalValue> rebind_preflight(Evaluator& ev, std::span<const EvalValue> a,
                                                     const MakeErrorVal& mev, Guard& guard,
                                                     bool& ok);
    static std::optional<EvalValue> rebind_find_target(
        Evaluator& ev, aura::ast::FlatAST& flat, const std::string& name, aura::ast::SymId sym,
        std::span<const EvalValue> a, const MakeErrorVal& mev, bool& ok,
        aura::ast::NodeId& old_define);
    static EvalValue rebind_add_new_define(Evaluator& ev, aura::ast::FlatAST& flat,
                                           const std::string& name, aura::ast::SymId sym,
                                           std::uint64_t code_idx, std::span<const EvalValue> a,
                                           const MakeErrorVal& mev, bool& ok);
    static std::optional<EvalValue> rebind_parse_value(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::SymId sym,
        aura::ast::NodeId& old_define, std::uint64_t code_idx, const MakeErrorVal& mev, bool& ok,
        aura::ast::NodeId& new_value, std::size_t& size_before_parse);
    static std::optional<EvalValue> rebind_shape_and_hygiene(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::NodeId old_define,
        aura::ast::NodeId new_value, std::span<const EvalValue> a, const std::string& name,
        std::size_t size_before_parse, const MakeErrorVal& mev, bool& ok);
    static std::optional<EvalValue> rebind_link_new_body(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::NodeId old_define,
        aura::ast::NodeId new_value, std::span<const EvalValue> a, const std::string& name,
        const MakeErrorVal& mev, bool& ok, bool& rejected_failed, std::uint64_t& cone_log_before,
        std::size_t size_before_parse);
    static void rebind_propagate_and_source(Evaluator& ev, aura::ast::FlatAST& flat,
                                            const std::string& name, aura::ast::SymId sym,
                                            const std::vector<aura::ast::NodeId>& dep_callers,
                                            aura::ast::NodeId new_value, std::uint64_t code_idx);
    static void rebind_invalidate_ir(Evaluator& ev, aura::ast::FlatAST& flat,
                                     const std::string& name, aura::ast::NodeId old_define,
                                     aura::ast::SymId sym, std::uint64_t cone_log_before);
    template <typename Guard>
    static std::optional<EvalValue> rebind_typecheck_and_gate(
        Evaluator& ev, aura::ast::FlatAST& flat, Guard& guard, aura::ast::NodeId old_define,
        const std::vector<aura::ast::NodeId>& dep_callers, const std::string& name,
        const MakeErrorVal& mev, bool& ok);
    static void rebind_refresh_env(Evaluator& ev, aura::ast::FlatAST& flat,
                                   const std::string& name, aura::ast::NodeId new_value,
                                   aura::ast::NodeId old_define);
    template <typename Ctx> static void register_mutate_set_body_primitive(Ctx& cx);
    // Issue #5033: mutate:set-body phases (rewrite Define / validate+gate).
    static std::optional<EvalValue> set_body_rewrite_define(
        Evaluator& ev, aura::ast::FlatAST& flat, aura::ast::NodeId& id, const std::string& name,
        aura::ast::SymId sym, std::uint64_t code_idx, std::span<const EvalValue> a,
        const std::vector<aura::ast::NodeId>& dep_callers, const MakeErrorVal& mev, bool& ok);
    template <typename Guard>
    static std::optional<EvalValue> set_body_typecheck_and_validate(
        Evaluator& ev, Guard& guard, aura::ast::FlatAST& flat, aura::ast::NodeId id,
        const std::string& name, const std::vector<aura::ast::NodeId>& dep_callers,
        const MakeErrorVal& mev, bool& ok);
    template <typename Ctx> static void register_mutate_node_surface_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_tweak_literal_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_replace_pattern_primitive(Ctx& cx);
    // Issue #5033: mutate:replace-pattern phases (kw parse / source printer / apply).
    static std::optional<EvalValue> replace_pattern_parse_options(
        Evaluator& ev, std::span<const EvalValue> a, const MakeErrorVal& mev, bool& ok,
        ReplacePatternOptions& opt);
    static std::string replace_pattern_node_source(Evaluator& ev, aura::ast::FlatAST& flat,
                                                   aura::ast::StringPool* pat_pool,
                                                   aura::ast::NodeId root_id);
    static std::optional<EvalValue> replace_pattern_apply_matches(
        Evaluator& ev, aura::ast::FlatAST& flat, std::vector<ReplacePatternMatch>& matches,
        const std::string& repl_template, int expected_captures, bool nested_arity,
        bool allow_macro_all, bool no_auto_restamp_rp, aura::ast::StringPool* pat_pool,
        std::uint32_t end_id, const MakeErrorVal& mev, bool& ok, int& replaced_count,
        std::vector<aura::ast::NodeId>& dirty_parents);
    template <typename Ctx> static void register_mutate_replace_subtree_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_atomic_batch_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_structural_insert_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_wrap_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_refactor_extract_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_rename_symbol_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_move_node_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_extract_function_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_inline_call_primitive(Ctx& cx);
    // Issue #5033: mutate:inline-call DFS clone + child reconnect pass.
    static void inline_call_clone_subtree(Evaluator& ev, aura::ast::FlatAST& flat,
                                          aura::ast::NodeId func_body_node,
                                          const std::vector<aura::ast::SymId>& formal_params,
                                          const std::vector<aura::ast::NodeId>& actual_args,
                                          std::vector<std::uint32_t>& old_to_new);
    template <typename Ctx> static void register_mutate_hygiene_checkpoint_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_dispatch_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_typed_audit_stats_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_type_propagation_stats_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_envframe_stats_primitives(Ctx& cx);
    template <typename Ctx> static void register_mutate_architecture_stats_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_hot_update_stats_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_reload_recovery_stats_primitive(Ctx& cx);
    template <typename Ctx> static void register_mutate_aot_audit_stats_primitive(Ctx& cx);
    // Issue #5033: query:typed-mutation-audit-trail insert phases.
    template <typename Kv>
    static void typed_audit_stats_snapshot_fields(
        Kv& insert_kv, std::uint64_t considered, std::uint64_t skipped, std::uint64_t contextual,
        std::uint64_t trail_sz, std::uint64_t rollbacks, std::uint64_t errors,
        std::uint32_t strategy, std::uint32_t ratio);
    template <typename Kv> static void typed_audit_stats_hard_gate_fields(Kv& insert_kv);
    template <typename Kv>
    static void typed_audit_stats_counter_fields(Kv& insert_kv, Evaluator& ev);
};
