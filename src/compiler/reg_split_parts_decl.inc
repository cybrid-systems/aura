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
};
