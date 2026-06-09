cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(aura LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(aura)
set_property(TARGET aura PROPERTY CXX_MODULE_STD ON)

target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
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
    src/repl/repl.cppm)

target_sources(aura PRIVATE
    src/core/contract_handler.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/thread_pool.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/serve/serve_async.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/compiler/git_ctx.cpp
    src/main.cpp)

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
 


# GCC 16 has an ICE in ealias pass at -O3 on x86_64 for cache_impl.cpp (memcpy-heavy)
# see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=nonnull_arg_p_ice
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16")
    set_source_files_properties(src/compiler/cache_impl.cpp PROPERTIES COMPILE_FLAGS "-O0")
endif()

target_include_directories(aura PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_options(aura PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
    -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter
    -Wno-misleading-indentation -Wno-parentheses
)

# C++26 Contracts — use the native trailing `pre`/`post` function specifiers
# and the `contract_assert()` statement. GCC 16.1 supports all three (and the
# <contracts> header + handle_contract_violation hook in
# src/core/contract_handler.cpp). The function-level attribute syntax
# `[[pre: expr]]` / `[[post: expr]]` is not yet parsed by GCC 16.1 — trailing
# syntax works on every compiler that supports -fcontracts.
target_compile_options(aura PRIVATE -fcontracts)

# LLVM JIT (optional, skipped if not found)
find_package(LLVM QUIET CONFIG)
if(LLVM_FOUND)
    message(STATUS "LLVM JIT: ${LLVM_PACKAGE_VERSION}")
    llvm_map_components_to_libnames(llvm_libs core orcjit native nativecodegen mc)
    target_sources(aura PRIVATE src/compiler/aura_jit.cpp src/compiler/aura_jit_runtime.cpp)
    target_include_directories(aura PRIVATE ${LLVM_INCLUDE_DIRS})
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    target_link_libraries(aura PRIVATE ${llvm_libs})
    target_compile_definitions(aura PRIVATE AURA_HAVE_LLVM=1)
    # -rdynamic: export all symbols (not just public ABI) so that the
    # crash handler in src/main.cpp can resolve internal function names
    # via backtrace_symbols(3) / dladdr(3). Without this, only the
    # handful of public dynamic symbols are visible and we get just
    # hex addresses, not source locations. Cost: ~5-10% binary size,
    # worth it for a Debug build with crash diagnostics.
    target_link_options(aura PRIVATE -rdynamic)
else()
    message(STATUS "LLVM JIT: disabled (not found)")
endif()

# libgit2 for in-process Git operations (faster + safer than popen)
find_package(libgit2 QUIET)
if(libgit2_FOUND)
    message(STATUS "libgit2: ${libgit2_VERSION} (in-process Git)")
    target_link_libraries(aura PRIVATE libgit2::libgit2package)
    target_compile_definitions(aura PRIVATE AURA_HAVE_LIBGIT2=1)
else()
    message(STATUS "libgit2: disabled (not found, using popen fallback)")
endif()

target_link_libraries(aura PRIVATE stdc++ stdc++exp)

# ── Separate compilation for std::meta reflection ───────────────
# cache_reflect.cpp needs -freflection, can't be in the module build.
# observability_json.cpp is in the same library for linker convenience
# (it doesn't use reflect directly; hand-rolled JSON to keep GCC 16
# compat).
add_library(aura-reflect STATIC
    src/reflect/cache_reflect.cpp
    src/compiler/ir_reflect_serialize.cpp
    src/compiler/observability_json.cpp
)
target_include_directories(aura-reflect PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(aura-reflect PRIVATE -freflection)
target_compile_features(aura-reflect PUBLIC cxx_std_26)
target_link_libraries(aura-reflect PRIVATE stdc++)

# Link reflect into aura
target_link_libraries(aura PRIVATE aura-reflect)




enable_testing()
macro(AT N I E)
    add_test(NAME ${N}
        COMMAND sh -c "test \"\$(printf '${I}' | ./aura)\" = ${E}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endmacro()
AT(step09_int_42 42 42)
AT(step09_int_neg5 "(- 5 10)" -5)
add_test(NAME step10_var_error
    COMMAND sh -c "echo x | ./aura 2>&1 | grep -F 'unbound variable'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
AT(step10_let "(let ((x 10)) x)" 10)
AT(step10_let_nested "(let ((x 10)) (let ((x 20)) x))" 20)
AT(step11_add "(+ 1 2)" 3)
AT(step11_sub "(- 5 3)" 2)
AT(step11_mul "(* 2 3)" 6)
AT(step11_nested "(+ 1 (* 2 3))" 7)
AT(step12_if_true "(if 1 42 0)" 42)
AT(step12_if_false "(if 0 0 99)" 99)
AT(step15_letrec_fact5
    "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))" 120)
AT(step15_letrec_fact10
    "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))" 3628800)

# ── IR tests ─────────────────────────────────────────────────────
add_executable(test_ir tests/test_ir.cpp)
set_property(TARGET test_ir PROPERTY CXX_MODULE_STD ON)
target_sources(test_ir PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_sources(test_ir PRIVATE
    src/core/contract_handler.cpp
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/thread_pool.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp)

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
target_compile_options(test_ir PRIVATE -Wall -Wextra -Wpedantic
    -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter
)
target_link_libraries(test_ir PRIVATE stdc++ stdc++exp aura-reflect)

# ── Concurrent model tests ────────────────────────────
# Tests fiber lifecycle, worker scheduling, and eventfd wakeup
add_executable(test_concurrent tests/test_concurrent.cpp)
set_property(TARGET test_concurrent PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_concurrent PRIVATE src)
target_sources(test_concurrent PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/compiler/messaging_bridge_impl.cpp
)
target_compile_options(test_concurrent PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_concurrent PRIVATE pthread)
add_test(NAME concurrent_basic COMMAND ./test_concurrent)
# Issue #115 verification tests (criterion #1 multi-core speedup
# + criterion #3 long-running stability). Standalone binary so it
# can be run independently of test_concurrent's other suites.
add_executable(test_issue_115 tests/test_issue_115.cpp)
set_property(TARGET test_issue_115 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_115 PRIVATE src)
target_sources(test_issue_115 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
)
target_compile_options(test_issue_115 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_115 PRIVATE pthread)
add_test(NAME issue_115_verification COMMAND ./test_issue_115)

# Issue #116 verification: deferred CoercionNode insertion
# (TypeChecker no longer mutates FlatAST in-place). Standalone
# binary so the snapshot / idempotency checks can run in
# isolation from the broader test_ir / test_concurrent harnesses.
add_executable(test_issue_116 tests/test_issue_116.cpp)
set_property(TARGET test_issue_116 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_116 PRIVATE src)
target_sources(test_issue_116 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_116 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_116 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_116 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_116_verification COMMAND ./test_issue_116)

# Issue #117 verification: full re-simulation mode for linear
# ownership + gradual+Linear boundary check. Standalone
# binary so the validation-path tests can run in isolation
# from the broader test_ir / test_concurrent harnesses.
add_executable(test_issue_117 tests/test_issue_117.cpp)
set_property(TARGET test_issue_117 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_117 PRIVATE src)
target_sources(test_issue_117 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_117 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_117 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_117 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_117_verification COMMAND ./test_issue_117)

# Issue #118 verification: solver timeout returns
# SolveResult (not bool) and unbound-variable / module-member
# diagnostic paths tag the AST node. Standalone binary so the
# diagnostics / solver tests can run in isolation.
add_executable(test_issue_118 tests/test_issue_118.cpp)
set_property(TARGET test_issue_118 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_118 PRIVATE src)
target_sources(test_issue_118 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_118 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_118 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_118 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_118_verification COMMAND ./test_issue_118)

# Issue #119 verification: proper-blocking fiber:join (eventfd
# wakeup in serve-async) + orch:parallel true parallel. Standalone
# binary so the join / scheduler-API tests can run in isolation.
add_executable(test_issue_119 tests/test_issue_119.cpp)
set_property(TARGET test_issue_119 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_119 PRIVATE src)
target_sources(test_issue_119 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_119 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_119 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_119 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_119_verification COMMAND ./test_issue_119)

# Issue #120 verification: define-hygienic-macro + clone_macro_body
# with name_map. Standalone binary so the macro-hygiene tests
# can run in isolation from the broader test_concurrent / test_ir
# harnesses.
add_executable(test_issue_120 tests/test_issue_120.cpp)
set_property(TARGET test_issue_120 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_120 PRIVATE src)
target_sources(test_issue_120 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_120 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_120 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_120 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_120_verification COMMAND ./test_issue_120)

# Issue #121 verification: gensym / symbol-append primitives +
# recursive macro expansion. Standalone binary so the macro /
# symbol tests can run in isolation.
add_executable(test_issue_121 tests/test_issue_121.cpp)
set_property(TARGET test_issue_121 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_121 PRIVATE src)
target_sources(test_issue_121 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_121 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_121 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_121 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_121_verification COMMAND ./test_issue_121)

# Issue #122 verification: compile-time reflection exposed as
# Aura runtime primitives. Standalone binary so the reflect
# tests can run in isolation.
add_executable(test_issue_122 tests/test_issue_122.cpp)
set_property(TARGET test_issue_122 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_122 PRIVATE src)
target_sources(test_issue_122 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_122 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_122 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_122 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_122_verification COMMAND ./test_issue_122)

# Issue #123 verification: IR-level pre-execution for require.
# Standalone binary so the require-stripping tests can run in
# isolation.
add_executable(test_issue_123 tests/test_issue_123.cpp)
set_property(TARGET test_issue_123 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_123 PRIVATE src)
target_sources(test_issue_123 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_123 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_123 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_123 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_123_verification COMMAND ./test_issue_123)

# Issue #124 verification: try/catch exception support in IR +
# interpreter. Standalone binary so the exception tests can
# run in isolation.
add_executable(test_issue_124 tests/test_issue_124.cpp)
set_property(TARGET test_issue_124 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_124 PRIVATE src)
target_sources(test_issue_124 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_124 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx
    src/compiler/ir.ixx)
target_compile_options(test_issue_124 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_124 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_124_verification COMMAND ./test_issue_124)

# Issue #125 verification: per-module dirty tracking + skip
# optimization counters. Standalone binary so the metrics /
# observability tests can run in isolation.
add_executable(test_issue_125 tests/test_issue_125.cpp)
set_property(TARGET test_issue_125 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_125 PRIVATE src src/compiler)
target_sources(test_issue_125 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/observability_metrics.h)
target_sources(test_issue_125 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/value.ixx src/compiler/diag.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_125 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_125 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_125_verification COMMAND ./test_issue_125)

# Issue #126 verification: pure functions extracted from
# CompilerService and Evaluator. Standalone binary that
# exercises the new aura.compiler.ir_cache_pure module
# in isolation from the full CompilerService.
add_executable(test_issue_126 tests/test_issue_126.cpp)
set_property(TARGET test_issue_126 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_126 PRIVATE src)
target_sources(test_issue_126 PRIVATE
    src/core/ast_impl.cpp
    src/core/type_impl.cpp)
target_sources(test_issue_126 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/diag.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx)
target_compile_options(test_issue_126 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_126 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_126_verification COMMAND ./test_issue_126)

# Issue #127 verification: Result<T> pipeline-stage
# aliases + lower_to_ir_result. Standalone binary that
# exercises the new diag.ixx aliases and lowering.ixx
# Result-returning function in isolation.
add_executable(test_issue_127 tests/test_issue_127.cpp)
set_property(TARGET test_issue_127 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_127 PRIVATE src src/compiler)
target_sources(test_issue_127 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/observability_metrics.h)
target_sources(test_issue_127 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/diag.ixx src/compiler/value.ixx
    src/compiler/ir.ixx src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/coercion_map.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_127 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_127 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_127_verification COMMAND ./test_issue_127)

# Issue #128 verification: std::span adoption in hot
# paths. Standalone binary that exercises the new
# cells() span view and compute_kind_instructions
# span helper.
add_executable(test_issue_128 tests/test_issue_128.cpp)
set_property(TARGET test_issue_128 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_128 PRIVATE src)
target_sources(test_issue_128 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/compute_kind_impl.cpp)
target_sources(test_issue_128 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/diag.ixx src/compiler/value.ixx
    src/compiler/ir.ixx src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/coercion_map.ixx
    src/compiler/compute_kind.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_128 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_128 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_128_verification COMMAND ./test_issue_128)

# Issue #130 verification: cache hit rate metric.
# Standalone binary that exercises the new
# TypeChecker::cache_hit_rate() getter.
add_executable(test_issue_130 tests/test_issue_130.cpp)
set_property(TARGET test_issue_130 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_130 PRIVATE src)
target_sources(test_issue_130 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_130 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/diag.ixx src/compiler/value.ixx
    src/compiler/ir.ixx src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/coercion_map.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_130 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_130 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_130_verification COMMAND ./test_issue_130)

# Issue #131 verification: FFI primitives extracted from
# monolithic evaluator_impl.cpp. Standalone binary that
# exercises the new FFIRuntime class in isolation.
add_executable(test_issue_131 tests/test_issue_131.cpp)
set_property(TARGET test_issue_131 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_131 PRIVATE src)
target_sources(test_issue_131 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp)
target_sources(test_issue_131 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/diag.ixx src/compiler/value.ixx
    src/compiler/ir.ixx src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_131 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_131 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_131_verification COMMAND ./test_issue_131)

# Issue #132 verification: AST walker helpers extracted
# from CompilerService. Standalone binary that exercises
# the new find_top_level_defines and collect_user_bindings
# pure functions.
add_executable(test_issue_132 tests/test_issue_132.cpp)
set_property(TARGET test_issue_132 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_132 PRIVATE src)
target_sources(test_issue_132 PRIVATE
    src/core/ast_impl.cpp
    src/core/type_impl.cpp)
target_sources(test_issue_132 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/compiler/ast_walkers.ixx)
target_compile_options(test_issue_132 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_132 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_132_verification COMMAND ./test_issue_132)

# Issue #134 verification: ADT (datatype ...) form.
# Standalone binary that exercises parse_datatype
# for various forms (with/without ctors, with/without
# type params, zero-arity ctors).
add_executable(test_issue_134 tests/test_issue_134.cpp)
set_property(TARGET test_issue_134 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_134 PRIVATE src src/compiler)
target_sources(test_issue_134 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/observability_metrics.h)
target_sources(test_issue_134 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/diag.ixx src/compiler/value.ixx
    src/compiler/ir.ixx src/compiler/ir_cache_pure.ixx
    src/compiler/coercion_map.ixx
    src/compiler/type_checker.ixx)
target_compile_options(test_issue_134 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_134 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_134_verification COMMAND ./test_issue_134)

# Issue #135 verification: True parallel multi-agent
# orchestration (fiber:join + orch:parallel + WorkspaceTree).
# The umbrella issue's work was completed across earlier
# sub-issues (#97, #98, #107, #109, #119). This test binary
# programmatically verifies all 5 acceptance criteria:
#   1. fiber:spawn + fiber:join returns spawned value
#   2. orch:parallel runs tasks concurrently (with results
#      and error isolation)
#   3. Workspace layering for isolated mutation experiments
#   4. No regression in fiber / orchestration tests
#   5. Memory / ASAN sanity (100 fibers + 50 workspaces)
add_executable(test_issue_135 tests/test_issue_135.cpp)
set_property(TARGET test_issue_135 PROPERTY CXX_MODULE_STD ON)
target_sources(test_issue_135 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_sources(test_issue_135 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_135 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_135 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_135 PRIVATE ${llvm_libs})
target_compile_options(test_issue_135 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_135 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_135_verification COMMAND ./test_issue_135)

# Issue #136 verification: memory hardening + hash performance
# + AOT polish + expanded benchmarks. The runtime.c tests for
# memory reset live in tests/runtime_test_harness.c (extended
# in this issue). The Aura-level benchmarks live in
# tests/benchmark.py (4 new cases). This C++ test binary
# exercises the parts that need direct C++ verification —
# primarily the AOT name-mangler (mangle_aot_name).
add_executable(test_issue_136 tests/test_issue_136.cpp)
set_property(TARGET test_issue_136 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_136 PRIVATE src src/compiler)
target_sources(test_issue_136 PRIVATE
    src/compiler/aot_mangle.h
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h)
target_compile_options(test_issue_136 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_136 PRIVATE pthread stdc++ stdc++exp)
add_test(NAME issue_136_verification COMMAND ./test_issue_136)

# Issue #137 verification: full hygienic macros (define-hygienic-macro
# + automatic gensym). The core implementation was shipped in
# Issue #120. This binary adds the verification coverage that
# #120 didn't have: full end-to-end runtime behavior (not just
# parse + typecheck), mutate:* compatibility, cross-layer
# hygiene, and a regression test for the known-pending items
# (rest parameters on hygienic macros).
add_executable(test_issue_137 tests/test_issue_137.cpp)
set_property(TARGET test_issue_137 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_137 PRIVATE src src/compiler)
target_sources(test_issue_137 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_sources(test_issue_137 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_137 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_137 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_137 PRIVATE ${llvm_libs})
target_compile_options(test_issue_137 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_137 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_137_verification COMMAND ./test_issue_137)

# Issue #138 verification: incremental dirty propagation and
# fine-grained type checking for EDSL mutations. The work was
# completed across earlier sub-issues (#97, #98, #107, #32b,
# #72). This binary verifies the infrastructure works end-to-end:
#   - FlatAST dirty-bit tracking (mark_dirty, has_dirty_subtree,
#     clear_all_dirty, is_dirty)
#   - mark_dirty_upward propagation from mutate primitives
#   - TypeChecker incremental cache (cache_hits, cache_misses,
#     stale_cache with TYPE_VAR validation)
#   - Stress test: 100 mutate+typecheck cycles
#   - Performance: incremental vs full typecheck cache behavior
add_executable(test_issue_138 tests/test_issue_138.cpp)
set_property(TARGET test_issue_138 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_138 PRIVATE src src/compiler)
target_sources(test_issue_138 PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_sources(test_issue_138 PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_138 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_138 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_138 PRIVATE ${llvm_libs})
target_compile_options(test_issue_138 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_138 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_138_verification COMMAND ./test_issue_138)

# ── GC ↔ Evaluator integration tests (Issue #113) ─────────
# Verifies the Evaluator root source registration API surface
# (flush_gc_roots + gc_root_count). The full GCRootSet + GCCollector
# interaction is tested in test_concurrent.cpp
# (`test_gc_safepoint_all_stop` and friends).
add_executable(test_gc_evaluator_integration tests/test_gc_evaluator_integration.cpp)
set_property(TARGET test_gc_evaluator_integration PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_gc_evaluator_integration PRIVATE src)
target_sources(test_gc_evaluator_integration PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/value.ixx
    src/compiler/cache.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/compiler/coercion_map.ixx
    src/compiler/ir_cache_pure.ixx
    src/compiler/lowering_linear_types.ixx
    src/compiler/ast_walkers.ixx
    src/compiler/ffi_primitives.ixx
    src/compiler/type_checker.ixx)
target_sources(test_gc_evaluator_integration PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/thread_pool.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
)
target_compile_options(test_gc_evaluator_integration PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_gc_evaluator_integration PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME gc_evaluator_integration COMMAND ./test_gc_evaluator_integration)

# Link JIT for test_gc_evaluator_integration when LLVM is available
if(LLVM_FOUND)
    target_sources(test_gc_evaluator_integration PRIVATE src/compiler/aura_jit.cpp src/compiler/aura_jit_runtime.cpp)
    target_include_directories(test_gc_evaluator_integration PRIVATE ${LLVM_INCLUDE_DIRS})
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    target_link_libraries(test_gc_evaluator_integration PRIVATE ${llvm_libs})
    target_compile_definitions(test_gc_evaluator_integration PRIVATE AURA_HAVE_LLVM=1)
endif()

# ── C++26 contracts test ─────────────────────────────────
# Verifies that the C++26 `pre`/`post`/`contract_assert` machinery is
# actually enforced at runtime. Two phases:
#   1. test_contracts holds — pre-condition holds, no violation (exit 0)
#   2. test_contracts fires — pre-condition violated, handler fires (exit 42)
add_executable(test_contracts tests/test_contracts.cpp)
set_source_files_properties(tests/test_contracts.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
target_compile_options(test_contracts PRIVATE -fcontracts)
add_test(NAME contracts_holds
         COMMAND test_contracts holds)
add_test(NAME contracts_fires
         COMMAND test_contracts fires)
# `fires` is expected to exit with non-zero (handler _Exits 42).
# Mark success when the violation handler message is printed.
set_tests_properties(contracts_fires PROPERTIES
    PASS_REGULAR_EXPRESSION "test_handler: contract violation"
    FAIL_REGULAR_EXPRESSION "FAIL:")


# ── EDSL IR cache V2 test (Phase 2) ───────────────────────
# Verifies the FNV-1a source-hash function used by ir_cache_v2_.
# Full integration is covered by tests/edsl_ir_cache_test.aura
# (see tests/run-tests.sh).
add_executable(test_ir_cache_v2 tests/test_ir_cache_v2.cpp)
add_test(NAME ir_cache_v2_fnv1a
         COMMAND test_ir_cache_v2)

# Link JIT for test_ir when LLVM is available (service.ixx references JIT types)
if(LLVM_FOUND)
    target_sources(test_ir PRIVATE src/compiler/aura_jit.cpp src/compiler/aura_jit_runtime.cpp)
    target_include_directories(test_ir PRIVATE ${LLVM_INCLUDE_DIRS})
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    target_link_libraries(test_ir PRIVATE ${llvm_libs})
    target_compile_definitions(test_ir PRIVATE AURA_HAVE_LLVM=1)
endif()

add_test(NAME ir_basic COMMAND ./test_ir
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# IR mode tests via --ir flag
macro(IRT N I E)
    add_test(NAME ir_${N}
        COMMAND sh -c "test \"\$(printf '${I}' | ./aura --ir)\" = ${E}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endmacro()
IRT(int_42 42 42)
IRT(add_1_2 "(+ 1 2)" 3)
IRT(mul_2_3 "(* 2 3)" 6)
IRT(if_true "(if 1 42 0)" 42)
IRT(if_false "(if 0 0 99)" 99)
IRT(let_x "(let ((x 10)) x)" 10)
IRT(lambda_call "((lambda (x) (* x 2)) 5)" 10)
IRT(lambda_closure "(let ((x 10)) ((lambda (y) (+ x y)) 5))" 15)

# ── CLI mode tests ─────────────────────────────────────────────
# Query: node-type Call on (+ 1 2) → 1 call node
add_test(NAME cli_query_call
    COMMAND sh -c "printf '(+ 1 2)' | ./aura --query '(node-type Call)' | grep -q '1 matches'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Query: (callee "+") on (+ 1 2) → 1 match
add_test(NAME cli_query_callee
    COMMAND sh -c "printf '(+ 1 2)' | ./aura --query '(callee \"+\")' | grep -q '1 matches'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Query-and-fix: replace LiteralInt with 99 → should succeed
add_test(NAME cli_query_and_fix
    COMMAND sh -c "printf '(+ 1 2)' | ./aura --query-and-fix '(node-type LiteralInt)' '(LiteralInt 99)' | grep -q 'applied=true'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Auto-fix: runs optimizations on (+ 1 2)
add_test(NAME cli_auto_fix
    COMMAND sh -c "printf '(+ 1 2)' | timeout 10 ./aura --auto-fix | grep -q 'auto-fix'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Serve mode: error detection on unbound variable
add_test(NAME cli_serve_error COMMAND sh -c "printf '(+ x 1)' | timeout 5 ./aura --serve | grep -q -F 'kind'" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Serve mode: ok result on valid code
add_test(NAME cli_serve_ok COMMAND sh -c "printf '(+ 1 2)' | timeout 5 ./aura --serve | grep -q -F 'ok'" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── TypeChecker tests (L6.x) ──────────────────────────────────
add_test(NAME tc_int
    COMMAND sh -c "printf '42' | timeout 5 ./aura --typecheck | grep -q 'type: Int'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME tc_lambda
    COMMAND sh -c "printf '(lambda (x) x)' | timeout 5 ./aura --typecheck | grep -q -e '->'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME tc_let
    COMMAND sh -c "printf '(let ((x 10)) x)' | timeout 5 ./aura --typecheck | grep -q 'type: Int'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})


# ── Shape profiler tests (Phase 1, #53) ─────────────────────
add_executable(test_shape tests/test_shape.cpp src/compiler/shape_profiler.cpp)
target_include_directories(test_shape PRIVATE src)
target_link_libraries(test_shape PRIVATE stdc++)
add_test(NAME test_shape COMMAND test_shape
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Speculative JIT tests (Phase 2, #53) ─────────────────────
add_executable(test_spec_jit tests/test_spec_jit.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/shape_profiler.cpp)
target_include_directories(test_spec_jit PRIVATE src)
target_link_libraries(test_spec_jit PRIVATE stdc++)
add_test(NAME test_spec_jit COMMAND test_spec_jit
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── JIT metrics tests (Issue #114) ─────────────────────
# Verifies the AuraJIT::Metrics struct (default state, atomic
# concurrent increments, format() output, edge cases). Doesn't
# need a full LLVM stack — exercises the counter paths directly.
add_executable(test_jit_metrics tests/test_jit_metrics.cpp
    tests/test_jit_metrics_stub.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp)
target_include_directories(test_jit_metrics PRIVATE src)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
target_sources(test_jit_metrics PRIVATE src/core/contract_handler.cpp)
target_link_libraries(test_jit_metrics PRIVATE stdc++ stdc++exp pthread aura-reflect)
if(LLVM_FOUND)
    target_include_directories(test_jit_metrics PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(test_jit_metrics PRIVATE ${llvm_libs})
    target_compile_definitions(test_jit_metrics PRIVATE AURA_HAVE_LLVM=1)
endif()
add_test(NAME jit_metrics COMMAND test_jit_metrics
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── JIT concurrent compile stress tests (Issue #114) ─────────
# Verifies the per-function compile cache and metrics counters
# under concurrent access. Doesn't need a full LLVM stack —
# exercises the counter paths directly.
add_executable(test_jit_concurrent_compile tests/test_jit_concurrent_compile.cpp
    tests/test_jit_metrics_stub.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp)
target_include_directories(test_jit_concurrent_compile PRIVATE src)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
target_sources(test_jit_concurrent_compile PRIVATE src/core/contract_handler.cpp)
target_link_libraries(test_jit_concurrent_compile PRIVATE stdc++ stdc++exp pthread aura-reflect)
if(LLVM_FOUND)
    target_include_directories(test_jit_concurrent_compile PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(test_jit_concurrent_compile PRIVATE ${llvm_libs})
    target_compile_definitions(test_jit_concurrent_compile PRIVATE AURA_HAVE_LLVM=1)
endif()
add_test(NAME jit_concurrent_compile COMMAND test_jit_concurrent_compile
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Speculative JIT runtime tests (Phase 3, #53) ────────────
add_executable(test_spec_runtime tests/test_spec_runtime.cpp)
target_include_directories(test_spec_runtime PRIVATE src)
target_link_libraries(test_spec_runtime PRIVATE stdc++)
add_test(NAME test_spec_runtime COMMAND test_spec_runtime
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# linenoise — single-file line editing library
add_library(linenoise STATIC
    src/linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC src/linenoise)
target_link_libraries(aura PRIVATE linenoise)


