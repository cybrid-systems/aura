# Shared C++ module (FILE_SET CXX_MODULES) lists for Aura targets.
# Issue #475: single source of truth — adding a module file edits one place.

set(AURA_CXX_MODULE_CORE
    src/core/arena.ixx
    src/core/concepts.ixx
    src/core/cxx26_invariants.ixx
    src/core/error.ixx
    src/core/mutation.ixx
    src/core/mutators.ixx
    src/core/ast.ixx
    src/core/panic_checkpoint_raii.ixx
    src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx
    src/parser/parser.ixx
)

set(AURA_CXX_MODULE_COMPILER
    src/compiler/evaluator.ixx
    src/compiler/evaluator_pure.ixx
    src/compiler/macro_expansion.ixx
    src/compiler/adt_runtime.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_soa.ixx
    # Issue #1241 / #1517: SoAView helpers (before pass_manager which imports it).
    src/compiler/soa_view.ixx
    # Issue #1206 / #1575: dirty cascade engine (before pass_manager which imports it).
    src/compiler/dirty_propagation.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/hardware_backend.ixx
    src/compiler/arity.ixx
    src/compiler/sv_ir.ixx
    src/compiler/constant_folding.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx
    src/compiler/query_matcher.ixx
)

# Wire the common CXX module FILE_SET onto TARGET.
# Optional flags:
#   WITH_REPL          — include src/repl/repl.cppm (main aura binary)
#   WITH_REFLECT       — include src/reflect/reflect.ixx (issue tests)
#   WITH_TYPE_CONCEPTS — include src/compiler/type_concepts.ixx (issue tests)
function(aura_target_cxx_modules TARGET)
    set(options WITH_REPL WITH_REFLECT WITH_TYPE_CONCEPTS)
    cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

    set(_module_files ${AURA_CXX_MODULE_CORE} ${AURA_CXX_MODULE_COMPILER})
    if(ARG_WITH_REPL)
        list(APPEND _module_files src/repl/repl.cppm)
    endif()
    if(ARG_WITH_REFLECT)
        list(APPEND _module_files src/reflect/reflect.ixx)
    endif()
    if(ARG_WITH_TYPE_CONCEPTS)
        list(APPEND _module_files src/compiler/type_concepts.ixx)
    endif()

    target_sources(${TARGET} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${_module_files})
endfunction()

# Shared -fcontracts flags for contract_handler / contract_stub.
function(aura_apply_contract_flags TARGET)
    target_sources(${TARGET} PRIVATE
        src/core/contract_handler.cpp
        src/core/contract_stub.cpp
    )
    set_source_files_properties(src/core/contract_handler.cpp PROPERTIES
        COMPILE_FLAGS "-include contracts -fcontracts")
    set_source_files_properties(src/core/contract_stub.cpp PROPERTIES
        COMPILE_FLAGS "-include contracts -fcontracts")
endfunction()