cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(aura LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fix #2: shared BMI cache. The launcher wraps every C++ compile,
# runs g++ normally, then post-processes the cwd to copy newly
# produced .gcm files into $BUILD_DIR/module_cache/ and symlink
# them into every per-target dir. Test targets that DON'T include
# the .ixx in their FILE_SET (and thus don't re-compile modules)
# can still `import` them because their own CMakeFiles/<t>.dir/
# has a symlink to the shared .gcm.
set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_SOURCE_DIR}/cmake/aura_module_launcher.sh")

add_executable(aura)
set_property(TARGET aura PROPERTY CXX_MODULE_STD ON)

target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx
    src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/evaluator_pure.ixx
    src/compiler/adt_runtime.ixx  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
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
    src/repl/repl.cppm)

target_sources(aura PRIVATE
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/adt_runtime_impl.cpp  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
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

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
 


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
    src/compiler/evaluator_pure.ixx
    src/compiler/adt_runtime.ixx  # ADT extraction (FFI pattern, hygiene 3.2) (for test_ir isolation)
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
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
    src/compiler/type_checker.ixx)
target_sources(test_ir PRIVATE
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
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
    src/compiler/adt_runtime_impl.cpp  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp)

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
target_compile_options(test_ir PRIVATE -Wall -Wextra -Wpedantic
    -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter
)
target_include_directories(test_ir PRIVATE src src/compiler)
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
    # Issue #195: fiber.cpp has a static initializer that
    # calls aura_set_current_fiber_id_fn (defined in
    # aura_jit_runtime.cpp) to install the per-fiber id
    # hook. Without this dependency, the linker fails with
    # "undefined reference to aura_set_current_fiber_id_fn".
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    # aura_jit.cpp defines emit_native_object (called from
    # aura_jit_bridge.cpp's aot_flat_functions_to_binary).
    # contract_handler.cpp provides the global
    # __tu_has_violation symbol for C++26 contract checks
    # (aura_jit.cpp is built with -fcontracts).
    src/compiler/aura_jit.cpp
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    # Stub for aura_jit_prim_dispatch (real one in service.ixx).
    # test_concurrent has no CompilerService context; the stub
    # returns 0 (no primitive found). See
    # src/compiler/aura_jit_prim_dispatch_stub.cpp for details.
    src/compiler/aura_jit_prim_dispatch_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_concurrent PRIVATE src src/compiler)
target_compile_definitions(test_concurrent PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_concurrent PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_concurrent PRIVATE ${llvm_libs})
target_compile_options(test_concurrent PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_concurrent PRIVATE pthread)
add_test(NAME concurrent_basic COMMAND ./test_concurrent)

# Shared object library for test_issue_* binaries.
#
# Why: 40-44 of 55 test_issue_* binaries list the same ~25 .cpp
# source files (ast_impl/type_impl/value_impl + serve/* + parser/*
# + evaluator_impl + cache/lowering/ir_executor + type_checker/arity/
# messaging_bridge + ffi_primitives/adt_runtime/lowering_linear_types).
# Pre-Fix #1, each test_issue_* binary compiled these sources
# independently, generating ~1950 MB of duplicated .o files (the
# largest — evaluator_impl.cpp.o 12.7 MB — alone duplicated 45
# times, wasting 544 MB of compile work and ~5 min of CI build time
# on a 2-vCPU runner).
#
# This static lib compiles each source ONCE, links the resulting
# libaura_test_objects.a into every test_issue_* binary. Net effect:
#   - Cold build time: ~75 min → ~25 min on CI 2-core
#   - Object file footprint: 1950 MB → ~80 MB
#   - Incremental build (one .cpp changed): rebuilds only that .o,
#     re-links all binaries that use it
#
# What's NOT in here:
#   - .ixx modules (C++ modules don't share BMIs cleanly across
#     targets in CMake's FILE_SET model; module dep chain is
#     compiled per-binary. This is a known limitation; see
#     https://gitlab.kitware.com/cmake/cmake/-/issues/25845)
#   - aura_jit.cpp / aura_jit_runtime.cpp (need per-binary
#     -fno-rtti compile flag; kept as per-test deltas)
#   - Per-test observability headers (h-only; not compiled)
#   - LLVM link / AURA_HAVE_LLVM defines (per-binary)
# Fix #2: shared BMI cache. aura_test_objects emits each .ixx once
# into ${CMAKE_BINARY_DIR}/module_cache/<name>.gcm. Test targets
# (and `aura`) consume the prebuilt BMIs via the symlinks the
# launcher installs in their per-target dirs, eliminating the
# per-binary re-emission that previously made every test_issue_*
# re-compile the full set of .ixx modules.
set(AURA_MODULE_BMI_DIR ${CMAKE_BINARY_DIR}/module_cache)
file(MAKE_DIRECTORY ${AURA_MODULE_BMI_DIR})
add_library(aura_test_objects STATIC
    src/core/contract_stub.cpp
    src/core/ast_impl.cpp
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/parser/lexer_impl.cpp
    src/parser/parser_impl.cpp
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/compiler/evaluator_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/adt_runtime_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp)
set_property(TARGET aura_test_objects PROPERTY CXX_MODULE_STD ON)
# .cpp impls require .ixx BMIs from the same module set the helper
# wires for each test_issue_* target. Add them here so the static
# lib can compile its sources standalone. Each module is still
# ALSO re-emitted per-test-binary (CMake FILE_SET doesn't share
# BMIs across targets yet) — this is the duplicate work Fix #2
# (sccache) will eliminate. For now, modules in both places are
# necessary to make aura_test_objects compile.
target_sources(aura_test_objects PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/core.ixx
    src/core/type_arena.ixx src/core/type.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/evaluator.ixx
    src/compiler/evaluator_pure.ixx
    src/compiler/adt_runtime.ixx
    src/compiler/cache.ixx
    src/compiler/value.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_executor.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
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
    src/compiler/ir_soa.ixx
    src/compiler/type_concepts.ixx)
target_include_directories(aura_test_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(aura_test_objects PUBLIC cxx_std_26)
target_compile_options(aura_test_objects PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(aura_test_objects PUBLIC stdc++ stdc++exp aura-reflect)

# Phase 2 dedup helper (definition placed early so it precedes all
# test_issue_* and pilot calls in this file). See also the pilots section
# comment for history.
#
# Fix #1 refactor: the helper used to inline `target_sources(PRIVATE ...)
# ast_impl/type_impl/value_impl` into every test_issue_* target. This
# caused each target to compile those sources independently. Now they
# come from the aura_test_objects static lib (see above), so the helper
# only wires modules + the per-test sources a few callers still need
# (aura_jit.cpp, observability headers, LLVM, etc.).
function(aura_add_issue_test NAME)
    add_executable(${NAME} tests/${NAME}.cpp)
    set_property(TARGET ${NAME} PROPERTY CXX_MODULE_STD ON)
    target_include_directories(${NAME} PRIVATE src)
    # Fix #2: no FILE_SET .ixx here. The prebuilt BMIs in
    # AURA_MODULE_BMI_DIR (built by aura_test_objects) are made
    # visible in the per-target dir via the launcher-installed
    # symlinks, so test_issue_* binaries can `import` modules
    # without re-compiling the .ixx themselves.
    target_compile_options(${NAME} PRIVATE -Wall -Wextra -Wpedantic
        -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
        -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
        -Wno-parentheses)
    # Fix #1: link the shared test object lib. Compiled once, used
    # by all 55 test_issue_* binaries.
    target_link_libraries(${NAME} PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
    add_test(NAME ${NAME}_verification COMMAND ./${NAME})
endfunction()

# Issue #115 verification tests (criterion #1 multi-core speedup
# + criterion #3 long-running stability). Standalone binary so it
# can be run independently of test_concurrent's other suites.
# Phase 2 pilot-43: converted using early helper. Light serve-only (4 files; helper base + small delta for include and serve sources).
aura_add_issue_test(test_issue_115)
target_include_directories(test_issue_115 PRIVATE src)
target_sources(test_issue_115 PRIVATE
    # Same dep as test_issue_205: aura_set_current_fiber_id_fn
    # (defined in aura_jit_runtime.cpp) is called from
    # fiber.cpp's static init in libaura_test_objects.a.
    # Plus aura_jit.cpp (defines emit_native_object, called
    # from aura_jit_bridge.cpp) and the contract / dispatch
    # stubs (test binaries don't have CompilerService or
    # aura.compiler.constant_folding's libaura_test_objects
    # symbol export).
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit.cpp
    src/core/contract_stub.cpp
    src/compiler/aura_jit_prim_dispatch_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_115 PRIVATE src src/compiler)
target_compile_definitions(test_issue_115 PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_issue_115 PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_issue_115 PRIVATE ${llvm_libs})

# Issue #116 verification: deferred CoercionNode insertion
# (TypeChecker no longer mutates FlatAST in-place). Standalone
# binary so the snapshot / idempotency checks can run in
# isolation from the broader test_ir / test_concurrent harnesses.
# Phase 2 pilot-44: converted using early helper. Light (serve + parser + core + value + type_checker; helper base + deltas).
aura_add_issue_test(test_issue_116)
target_include_directories(test_issue_116 PRIVATE src)

# Issue #117 verification: full re-simulation mode for linear
# ownership + gradual+Linear boundary check. Standalone
# binary so the validation-path tests can run in isolation
# from the broader test_ir / test_concurrent harnesses.
# Phase 2 pilot-45: converted using early helper. Light (serve + parser + core + value + type_checker; base + deltas).
aura_add_issue_test(test_issue_117)
target_include_directories(test_issue_117 PRIVATE src)

# Issue #118 verification: solver timeout returns
# SolveResult (not bool) and unbound-variable / module-member
# diagnostic paths tag the AST node. Standalone binary so the
# diagnostics / solver tests can run in isolation.
add_executable(test_issue_118 tests/test_issue_118.cpp)
set_property(TARGET test_issue_118 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_118 PRIVATE src)
target_compile_options(test_issue_118 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_118 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_118_verification COMMAND ./test_issue_118)

# Issue #119 verification: proper-blocking fiber:join (eventfd
# wakeup in serve-async) + orch:parallel true parallel. Standalone
# binary so the join / scheduler-API tests can run in isolation.
add_executable(test_issue_119 tests/test_issue_119.cpp)
set_property(TARGET test_issue_119 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_119 PRIVATE src)
target_compile_options(test_issue_119 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_119 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_119_verification COMMAND ./test_issue_119)

# Issue #120 verification: define-hygienic-macro + clone_macro_body
# with name_map. Standalone binary so the macro-hygiene tests
# can run in isolation from the broader test_concurrent / test_ir
# harnesses.
add_executable(test_issue_120 tests/test_issue_120.cpp)
set_property(TARGET test_issue_120 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_120 PRIVATE src)
target_compile_options(test_issue_120 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_120 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_120_verification COMMAND ./test_issue_120)

# Issue #121 verification: gensym / symbol-append primitives +
# recursive macro expansion. Standalone binary so the macro /
# symbol tests can run in isolation.
add_executable(test_issue_121 tests/test_issue_121.cpp)
set_property(TARGET test_issue_121 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_121 PRIVATE src)
target_compile_options(test_issue_121 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_121 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_121_verification COMMAND ./test_issue_121)

# Issue #122 verification: compile-time reflection exposed as
# Aura runtime primitives. Standalone binary so the reflect
# tests can run in isolation.
add_executable(test_issue_122 tests/test_issue_122.cpp)
set_property(TARGET test_issue_122 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_122 PRIVATE src)
target_compile_options(test_issue_122 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_122 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_122_verification COMMAND ./test_issue_122)

# Issue #123 verification: IR-level pre-execution for require.
# Standalone binary so the require-stripping tests can run in
# isolation.
add_executable(test_issue_123 tests/test_issue_123.cpp)
set_property(TARGET test_issue_123 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_123 PRIVATE src)
target_compile_options(test_issue_123 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_123 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_123_verification COMMAND ./test_issue_123)

# Issue #124 verification: try/catch exception support in IR +
# interpreter. Standalone binary so the exception tests can
# run in isolation.
add_executable(test_issue_124 tests/test_issue_124.cpp)
set_property(TARGET test_issue_124 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_124 PRIVATE src)
target_compile_options(test_issue_124 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_124 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_124_verification COMMAND ./test_issue_124)

# Issue #125 verification: per-module dirty tracking + skip
# optimization counters. Standalone binary so the metrics /
# observability tests can run in isolation.
add_executable(test_issue_125 tests/test_issue_125.cpp)
set_property(TARGET test_issue_125 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_125 PRIVATE src src/compiler)
target_sources(test_issue_125 PRIVATE
src/compiler/observability_metrics.h
)
target_compile_options(test_issue_125 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_125 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_125_verification COMMAND ./test_issue_125)

# Issue #126 verification: pure functions extracted from
# CompilerService and Evaluator. Standalone binary that
# exercises the new aura.compiler.ir_cache_pure module
# in isolation from the full CompilerService.
add_executable(test_issue_126 tests/test_issue_126.cpp)
set_property(TARGET test_issue_126 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_126 PRIVATE src)
target_compile_options(test_issue_126 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_126 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_126_verification COMMAND ./test_issue_126)

# Issue #127 verification: Result<T> pipeline-stage
# aliases + lower_to_ir_result. Standalone binary that
# exercises the new diag.ixx aliases and lowering.ixx
# Result-returning function in isolation.
add_executable(test_issue_127 tests/test_issue_127.cpp)
set_property(TARGET test_issue_127 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_127 PRIVATE src src/compiler)
target_sources(test_issue_127 PRIVATE
src/compiler/observability_metrics.h
)
target_compile_options(test_issue_127 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_127 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_127_verification COMMAND ./test_issue_127)

# Issue #128 verification: std::span adoption in hot
# paths. Standalone binary that exercises the new
# cells() span view and compute_kind_instructions
# span helper.
add_executable(test_issue_128 tests/test_issue_128.cpp)
set_property(TARGET test_issue_128 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_128 PRIVATE src)
target_compile_options(test_issue_128 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_128 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_128_verification COMMAND ./test_issue_128)

# Issue #130 verification: cache hit rate metric.
# Standalone binary that exercises the new
# TypeChecker::cache_hit_rate() getter.
add_executable(test_issue_130 tests/test_issue_130.cpp)
set_property(TARGET test_issue_130 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_130 PRIVATE src)
# Fix #2: consume prebuilt BMIs from AURA_MODULE_BMI_DIR (produced by
# aura_test_objects). No more per-binary re-emission of the 17 .ixx
# modules previously listed here.
target_compile_options(test_issue_130 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_130 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_130_verification COMMAND ./test_issue_130)

# Issue #131 verification: FFI primitives extracted from
# monolithic evaluator_impl.cpp. Standalone binary that
# exercises the new FFIRuntime class in isolation.
add_executable(test_issue_131 tests/test_issue_131.cpp)
set_property(TARGET test_issue_131 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_131 PRIVATE src)
target_compile_options(test_issue_131 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_131 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_131_verification COMMAND ./test_issue_131)

# Phase 2 dedup (post-5.2 per roadmap): aura_add_issue_test helper.
# Reduces the 30-60 line boilerplate per test_issue_* (add_executable,
# set_property, includes, massive repeated target_sources for core modules,
# compile_options, link with aura-reflect, add_test) down to 1 call + small
# unique tail for test-specific modules/impls.
#
# Base set here covers the common core + light compiler modules used by
# (Definition of aura_add_issue_test is now early in the file — before
# the first test_issue_* — for correct CMake call ordering.
# The body above (near line ~260) is the active one. This late text is a
# historical marker only.)

# ── CMake pilots (Step 1.1/1.2/1.3 / 3.2) now via aura_add_issue_test (Phase 2 dedup) ──
# test_error_merr.cpp — exercises make_merr (centralized in 0.1+) via
# Evaluator construction + error paths. Now deduped via helper.
# (Note: adt_runtime extraction (FFI pattern) complete; sources included in relevant targets per 3.2 hygiene)
aura_add_issue_test(test_error_merr)
# Minimal pilot (harness demo); make_merr also covered in test_ir.

# Second pilot (Step 1.2 / 3.2) — Primitives / init_pair sanity (deduped).
aura_add_issue_test(test_primitives_init)

# Third pilot (Step 1.3 / 3.2) — harness demo (deduped).
aura_add_issue_test(test_harness_pilot)

# Issue #132 verification: AST walker helpers extracted
# from CompilerService. Standalone binary that exercises
# the new find_top_level_defines and collect_user_bindings
# pure functions.
# Phase 2 pilot-2: first real (non-pilot) test converted to aura_add_issue_test.
# Only the test-specific extra (ast_walkers) is appended; base covers core.
aura_add_issue_test(test_issue_132)
# Specific to this test (ast_walkers pure extraction from #132)

# Issue #134 verification: ADT (datatype ...) form.
# Standalone binary that exercises parse_datatype
# for various forms (with/without ctors, with/without
# type params, zero-arity ctors).
# Phase 2 pilot-4: converted using early helper (definition before all calls).
# Base covers core + value/diag/value_impl; append the rest (parser, serve,
# type_checker, ir/coercion, observability .h, extra include dir).
aura_add_issue_test(test_issue_134)
target_include_directories(test_issue_134 PRIVATE src/compiler)
target_sources(test_issue_134 PRIVATE
src/compiler/observability_metrics.h
)

# Issue #135 verification: True parallel multi-agent
# orchestration (fiber:join + orch:parallel + WorkspaceTree).
# The umbrella issue's work was completed across earlier
# sub-issues (#97, #98, #107, #109, #119). This test binary
# programmatically verifies all 5 acceptance criteria.
# Phase 2 pilot-5: converted using early helper. Base covers core
# + value/diag/value_impl; append the heavy orchestration/eval/JIT
# specifics (serve, parser, evaluator*, lowering*, query*, jit*,
# LLVM bits, rtti flags, extra includes).
add_executable(test_issue_135 tests/test_issue_135.cpp)
set_property(TARGET test_issue_135 PROPERTY CXX_MODULE_STD ON)
target_sources(test_issue_135 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_135 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_135 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_135 PRIVATE aura_test_objects ${llvm_libs})
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
# Phase 2 pilot-6: converted using early helper (small/light test).
# Base covers core + value etc.; extra include + the AOT/observability headers.
aura_add_issue_test(test_issue_136)
target_include_directories(test_issue_136 PRIVATE src/compiler)
target_sources(test_issue_136 PRIVATE
    src/compiler/aot_mangle.h
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)

# Issue #137 verification: full hygienic macros (define-hygienic-macro
# + automatic gensym). The core implementation was shipped in
# Issue #120. This binary adds the verification coverage that
# #120 didn't have: full end-to-end runtime behavior (not just
# parse + typecheck), mutate:* compatibility, cross-layer
# hygiene, and a regression test for the known-pending items
# (rest parameters on hygienic macros).
# Phase 2 pilot-7: converted using early helper. Base covers core + value/diag/value_impl; append the heavy parser/serve/evaluator/JIT/LLVM specifics + rtti flags + LLVM bits.
aura_add_issue_test(test_issue_137)
target_sources(test_issue_137 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_137 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_137 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_137 PRIVATE ${llvm_libs})

# Issue #158 verification: quasiquote should expand inner macro
# calls in code positions. Standalone binary so the qq-in-macro
# tests run in isolation.
# Phase 2 pilot-8: converted using early helper. Base covers core + value/diag/value_impl; append the parser/serve/evaluator/JIT/LLVM specifics + rtti flags + LLVM bits.
aura_add_issue_test(test_issue_158)
target_sources(test_issue_158 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_158 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_158 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_158 PRIVATE ${llvm_libs})

# Issue #159 Phase 1 verification: (typecheck-incremental)
# primitive wires up the partial re-inference path. Standalone
# binary so the new primitive dispatch is tested in isolation.
# Phase 2 pilot-9: converted using early helper. Base covers core + value/diag/value_impl; append the parser/serve/evaluator/JIT/LLVM specifics + rtti flags + LLVM bits.
aura_add_issue_test(test_issue_159)
target_sources(test_issue_159 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_159 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_159 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_159 PRIVATE ${llvm_libs})

# Issue #161 Phase 2 verification: parser is a pure function. The
# test only needs the parser (no evaluator/IR/JIT) — the
# deteminist test is parser-local. We link a minimal subset.
# Phase 2 pilot-10: converted using early helper (light parser-only test).
# Base covers core + value/diag/value_impl; append the parser modules + parser impls.
aura_add_issue_test(test_issue_161)

# Issue #162 Phase 1 verification: Type Concepts for the type
# system. Verifies concept TypeConstraint + 2 concrete constraint
# types (EqualConstraint, ConsistentConstraint) + the
# solve_constraints<C> free function template. The concept is
# compile-time-checked via static_assert inside type_concepts.ixx.
# Phase 2 pilot-11: converted using early helper. Base covers core + value/diag/value_impl; append the parser + type concepts specifics.
aura_add_issue_test(test_issue_162)
target_sources(test_issue_162 PRIVATE
src/compiler/type_concepts_impl.cpp
)

# Issue #163 verification: expanded Pass concept usage. Verifies
# concept AnalysisPass, run_pipeline in 3+ locations (test #4),
# run_analysis_pipeline, and the mark_coercions free function
# (stateful class reduction).
# Phase 2 pilot-12: converted using early helper. Base covers core + value/diag/value_impl; append the parser + type/pass_manager specifics.
aura_add_issue_test(test_issue_163)

# Issue #164 verification: fiber:join spin-fallback elimination.
# Verifies (1) basic fiber:join still works, (2) no spin even
# on already-done target, (3) concurrent mutate:rebind during
# fiber:join completes correctly (no version mismatch, no
# timeout), (4) 20 parallel fibers via orch:parallel work,
# (5) 50 sequential spawn+join complete within reasonable time
# (proves no CPU burn from the removed spin fallback).
# Phase 2 pilot-13: converted using early helper. Base covers core + value/diag/value_impl; append the heavy fiber/serve + evaluator/JIT/LLVM specifics.
# Issue #164 verification: fiber:join spin-fallback elimination.
# Verifies (1) basic fiber:join still works, (2) no spin even
# on already-done target, (3) concurrent mutate:rebind during
# fiber:join completes correctly (no version mismatch, no
# timeout), (4) 20 parallel fibers via orch:parallel work,
# (5) 50 sequential spawn+join complete within reasonable time
# (proves no CPU burn from the removed spin fallback).
add_executable(test_issue_164 tests/test_issue_164.cpp)
set_property(TARGET test_issue_164 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_164 PRIVATE src src/compiler)
target_sources(test_issue_164 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_164 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_164 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_164 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_164 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_164 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_164_verification COMMAND ./test_issue_164)

# Issue #184 verification: MutationBoundaryGuard RAII + atomic
# defuse_version_ + Fiber + Mutation Concurrency Safety. Cycle 1
# scope: single-threaded verification that the guard wraps the
# mutation effect correctly, RAII releases the lock on every exit
# path (success, parse error, type error), and defuse_version_ is
# monotonic under typed_mutate. Cycles 2-5 will extend with
# multi-threaded smoke, fiber yield integration, and stress tests.
add_executable(test_issue_184 tests/test_issue_184.cpp)
set_property(TARGET test_issue_184 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_184 PRIVATE src src/compiler)
target_sources(test_issue_184 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_184 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_184 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_184 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_184 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_184 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_184_verification COMMAND ./test_issue_184)

# Issue #165 verification: macro re-expansion + SyntaxMarker
# handling after EDSL mutations. The test is TDD — it
# reproduces the hygiene violation (AC #1+#2+#4 of the issue)
# and is expected to FAIL on the current main (proving the
# bug exists) and PASS after the post-mutation macro
# re-expansion pass is added.
# Phase 2 pilot-14: converted using early helper. Base covers core + value/diag/value_impl; append the heavy parser/serve/evaluator/JIT/LLVM specifics.
# Issue #165 verification: macro re-expansion + SyntaxMarker
# handling after EDSL mutations. The test is TDD — it
# reproduces the hygiene violation (AC #1+#2+#4 of the issue)
# and is expected to FAIL on the current main (proving the
# bug exists) and PASS after the post-mutation macro
# re-expansion pass is added.
add_executable(test_issue_165 tests/test_issue_165.cpp)
set_property(TARGET test_issue_165 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_165 PRIVATE src src/compiler)
target_sources(test_issue_165 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_165 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_165 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_165 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_165 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_165 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_165_verification COMMAND ./test_issue_165)

# Issue #166 verification: multi-layer cache invalidation
# (Phase 1: epoch counter). Verifies the basic flow still
# works after adding the global mutation_epoch_ + per-entry
# last_seen_epoch_ on IRCacheEntry and JitCachedFn.
add_executable(test_issue_166 tests/test_issue_166.cpp)
set_property(TARGET test_issue_166 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_166 PRIVATE src src/compiler)
target_sources(test_issue_166 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_166 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_166 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_166 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_166 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_166 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_166_verification COMMAND ./test_issue_166)

# Issue #167 verification: IR layer SoA/DOD migration
# (Phase 1: scaffold). Verifies the basic IRModuleV2 +
# IRInstructionView API. The new module is parallel to
# existing ir.ixx — no consumer changes.
# Phase 2 pilot-16: converted using early helper (light IR API test).
# Base covers core + value/diag/value_impl; append the ir + ir_soa specifics.
aura_add_issue_test(test_issue_167)

# Issue #168 verification: incremental type cache safety
# (Phase 1: epoch gate). Verifies the set_cache_epoch API +
# basic flow with mutation.
# Phase 2 pilot-17: converted using early helper (light type/cache test).
# Base covers core + value/diag/value_impl; append the parser + type_checker specifics.
aura_add_issue_test(test_issue_168)

# Issue #169 verification: Fine-grained Incremental Compilation
# v3 (Phase 1: config flag). Verifies the IncrementalStrictness
# enum + CompilerService set/get methods + no-behavior-change.
add_executable(test_issue_169 tests/test_issue_169.cpp)
set_property(TARGET test_issue_169 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_169 PRIVATE src src/compiler)
target_sources(test_issue_169 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_169 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_169 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_169 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_169 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_169 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_169_verification COMMAND ./test_issue_169)

# Issue #170 verification: LLVM JIT AOT entry points
# (Phase 1). Verifies the new public API on the empty
# state (no compile yet). The actual module-emission
# flow needs a real FlatFunction, which is exercised by
# the test_ir / test_issue_158 / test_issue_159 tests
# (which already compile via the full IR pipeline).
# Phase 2 pilot-18: converted using early helper (light JIT AOT test).
# Base covers core + value/diag/value_impl; append the jit + messaging + spec_jit specifics + LLVM bits.
aura_add_issue_test(test_issue_170)
target_include_directories(test_issue_170 PRIVATE src/compiler)
target_sources(test_issue_170 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_170 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_170 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_170 PRIVATE ${llvm_libs})

# Issue #159 Phase 4: incremental compilation benchmark
# harness. Measures eval_current latency across 4 scenarios
# × N workspace sizes, outputs JSON to tests/bench_results/.
# Standalone binary so the benchmark runs in isolation.
add_executable(test_issue_159_bench tests/test_issue_159_bench.cpp)
set_property(TARGET test_issue_159_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_159_bench PRIVATE src src/compiler)
target_sources(test_issue_159_bench PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_159_bench PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_159_bench PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_159_bench PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_159_bench PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_159_bench PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_159_bench COMMAND ./test_issue_159_bench)

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
target_sources(test_issue_138 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_138 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_138 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_138 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_138 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_138 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_138_verification COMMAND ./test_issue_138)

# Issue #139 verification: structural refactor primitives
# (mutate:extract-function, mutate:inline-call, mutate:rename-symbol,
# mutate:refactor/extract, mutate:move-node, mutate:splice, mutate:wrap).
# The work was already shipped across earlier sub-issues. This
# binary verifies the operators work end-to-end on real Aura code.
add_executable(test_issue_139 tests/test_issue_139.cpp)
set_property(TARGET test_issue_139 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_139 PRIVATE src src/compiler)
target_sources(test_issue_139 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_139 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_139 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_139 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_139 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_139 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_139_verification COMMAND ./test_issue_139)

# Issue #140 verification: query:pattern EDSL primitive with
# Ellipsis and basic hygiene. The work was already shipped
# (the primitive exists at evaluator_impl.cpp:6326). This PR
# adds a hygiene fix (skip SyntaxMarker::MacroIntroduced nodes)
# and a verification binary.
add_executable(test_issue_140 tests/test_issue_140.cpp)
set_property(TARGET test_issue_140 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_140 PRIVATE src src/compiler)
target_sources(test_issue_140 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_140 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_140 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_140 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_140 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_140 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_140_verification COMMAND ./test_issue_140)

# Issue #141 verification: full WorkspaceTree with COW, read-only
# permissions, and merge primitives. All 18 workspace:* primitives
# were already shipped in earlier sub-issues (#97, #98, #107).
# This binary verifies they work end-to-end.
add_executable(test_issue_141 tests/test_issue_141.cpp)
set_property(TARGET test_issue_141 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_141 PRIVATE src src/compiler)
target_sources(test_issue_141 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_141 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_141 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_141 PRIVATE aura_test_objects ${llvm_libs})
target_compile_options(test_issue_141 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_141 PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_141_verification COMMAND ./test_issue_141)

# Issue #142 verification: composite query primitives (query:where /
# query:filter) plus mutate:replace-subtree with capture detection
# and hygiene. query:where and query:filter shipped in commit 8bc00b6;
# mutate:replace-subtree, capture detection, and the extended
# MutationRecord are added in this PR.
# Phase 2 pilot-23: converted using early helper. Base covers core + value/diag/value_impl; append the heavy parser/serve/evaluator/JIT/LLVM + query/mutate specifics + observability headers + rtti/LLVM bits (heavy like 141/140).
aura_add_issue_test(test_issue_142)
target_include_directories(test_issue_142 PRIVATE src src/compiler)
target_sources(test_issue_142 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_142 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_142 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_142 PRIVATE ${llvm_libs})

# ── GC ↔ Evaluator integration tests (Issue #113) ─────────
# Verifies the Evaluator root source registration API surface
# (flush_gc_roots + gc_root_count). The full GCRootSet + GCCollector
# interaction is tested in test_concurrent.cpp
# (`test_gc_safepoint_all_stop` and friends).
add_executable(test_gc_evaluator_integration tests/test_gc_evaluator_integration.cpp)
set_property(TARGET test_gc_evaluator_integration PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_gc_evaluator_integration PRIVATE src)
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
    src/compiler/adt_runtime_impl.cpp  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
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
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
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
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
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



# Issue #143 verification (partial close): the only piece of the
# 3-workstream roadmap that fits in a verify+close cycle is
# escape analysis. escape_analysis.h has been in the tree for
# a while but was never integrated into pass_manager. This test
# wires it up via a new EscapeAnalysisWrap class and verifies
# the basic analysis correctness on hand-crafted IRFunctions.
# Phase 2 pilot-24: converted using early helper. Base covers core + value/diag/value_impl; append heavy parser/serve/evaluator/JIT/LLVM + observability headers + rtti/LLVM bits.
aura_add_issue_test(test_issue_143)
target_include_directories(test_issue_143 PRIVATE src src/compiler)
target_sources(test_issue_143 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_143 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_143 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_143 PRIVATE ${llvm_libs})

# Issue #144 verification: C++26 contracts integration. Tests
# the 13 contract_assert sites added in this PR, plus the
# hook-based violation integration (AC #2).
# Phase 2 pilot-25: converted using early helper. Base + append for heavy (parser/serve/eval + jit + llvm + contract_handler.cpp + shape_profiler + observability). Note traditional headers in .cpp (shape.h before modules).
aura_add_issue_test(test_issue_144)
target_include_directories(test_issue_144 PRIVATE src src/compiler)
target_sources(test_issue_144 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_144 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_144 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_144 PRIVATE ${llvm_libs})

# Issue #145 verification (partial): DOD/SoA Phase 1. Tests
# Closure::params SoA migration + EnvView/ClosureView new
# types + bind_symid/lookup_by_symid fast path. Full issue is
# 2-3 weeks; this PR ships the highest-impact slice.
# Phase 2 pilot-26: converted using early helper. Consolidated split config (bare early decl + late full block) to aura_add + deltas. Heavy (jit, serve, shape, lowering etc.; note rtti comment shared with 146).
aura_add_issue_test(test_issue_145)
target_include_directories(test_issue_145 PRIVATE src src/compiler)
target_sources(test_issue_145 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h
)
# Issue #145: aura_jit.cpp uses LLVM and pimpl over an Impl
# forward declaration. Needs -fno-rtti to match the aura binary
# configuration (shared note with 146).
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_145 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_145 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_145 PRIVATE ${llvm_libs})

# Issue #146 verification: pure-function + monadic Result
# extraction for Evaluator core. Tests the first extract
# (coerce_to_int_pure) and its compatibility with the legacy
# coerce_to_int wrapper. Full issue is 2 weeks of work; this
# PR ships the first slice of the 6-8 target functions.
# Phase 2 pilot-27: converted using early helper. Leverages new evaluator_pure module (modernization). Heavy (jit, serve etc.).
aura_add_issue_test(test_issue_146)
target_include_directories(test_issue_146 PRIVATE src src/compiler)
target_sources(test_issue_146 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
)
# Issue #146: aura_jit.cpp uses LLVM and pimpl over an Impl
# forward declaration. Needs -fno-rtti (shared with 145).
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_146 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_146 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_146 PRIVATE ${llvm_libs})
# Phase 2 pilot-28: converted using early helper (heavy like 146/145).
aura_add_issue_test(test_issue_147)
target_include_directories(test_issue_147 PRIVATE src src/compiler)
target_sources(test_issue_147 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_147 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_147 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_147 PRIVATE ${llvm_libs})
# Issue #148 verification: mutation-aware incremental type checking with constraint
# delta solving. 
# Phase 2 pilot-29: converted using early helper. Heavy (full parser/serve/eval/jit/LLVM + type_checker).
aura_add_issue_test(test_issue_148)
target_include_directories(test_issue_148 PRIVATE src src/compiler)
target_sources(test_issue_148 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_148 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_148 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_148 PRIVATE ${llvm_libs})
# Issue #149 verification: propagate rich inferred types for specialization,
# monomorphization and improved deopt guards.
# Phase 2 pilot-30: converted using early helper. Heavy (jit, serve, type etc.).
aura_add_issue_test(test_issue_149)
target_include_directories(test_issue_149 PRIVATE src src/compiler)
target_sources(test_issue_149 PRIVATE
 # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_149 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_149 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_149 PRIVATE ${llvm_libs})

# Issue #171 verification: Function Inliner (Priority 2) +
# TCO (Priority 3). Verifies the InlinePass actually inlines
# trivial callees at known-static call sites (the #160 scaffold
# only counted call sites). TCO tests follow in a follow-up
# commit when the TCO pass ships.
# Issue #171 verification: Function Inliner (Priority 2) +
# TCO (Priority 3). Verifies the InlinePass actually inlines
# trivial callees at known-static call sites (the #160 scaffold
# only counted call sites). TCO tests follow in a follow-up
# commit when the TCO pass ships.
# Phase 2 pilot-31 prep: short form attempted (added adt/pure/ffi for evaluator imports) but link requires more impls (Primitives, mutation_stack TLS etc from full evaluator support). Restored full original block for health (pattern of 141/145 preps). Binary ran 30/30 previously. Docs updated. 
add_executable(test_issue_171 tests/test_issue_171.cpp)
set_property(TARGET test_issue_171 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_171 PRIVATE src src/compiler)
target_compile_options(test_issue_171 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_171 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_171_verification COMMAND ./test_issue_171)

# Issue #204 verification: GC env_frames_ walk integration
# (Issue #172 Phase 4). Verifies GCCollector::mark_env_frame_roots()
# sets the correct bits for indices reachable through env
# parent chains. The caller-side integration (evaluator walking
# env_frames_ and producing the index lists) is a separate
# follow-up — this test verifies the GC-side method only.
add_executable(test_issue_204 tests/test_issue_204.cpp)
set_property(TARGET test_issue_204 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_204 PRIVATE src src/serve)
target_sources(test_issue_204 PRIVATE
    # Same dep as test_issue_205: aura_set_current_fiber_id_fn
    # (defined in aura_jit_runtime.cpp) is called from
    # fiber.cpp's static init in libaura_test_objects.a.
    # Plus aura_jit.cpp (defines emit_native_object, called
    # from aura_jit_bridge.cpp) and the contract / dispatch
    # stubs (test binaries don't have CompilerService or
    # aura.compiler.constant_folding's libaura_test_objects
    # symbol export).
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit.cpp
    src/core/contract_stub.cpp
    src/compiler/aura_jit_prim_dispatch_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_204 PRIVATE src src/compiler)
target_compile_definitions(test_issue_204 PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_issue_204 PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_issue_204 PRIVATE ${llvm_libs})
target_compile_options(test_issue_204 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_204 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_204_verification COMMAND ./test_issue_204)

# Issue #205 verification: caller-side env_frames_ walk
# wires evaluator to GCCollector::mark_env_frame_roots.
# Verifies the registration mechanism + EnvFrameRoots
# struct + that mark_env_frame_roots is invoked with the
# env walk's output.
add_executable(test_issue_205 tests/test_issue_205.cpp)
set_property(TARGET test_issue_205 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_205 PRIVATE src src/serve)
target_sources(test_issue_205 PRIVATE
    # Same dep as test_concurrent: aura_set_current_fiber_id_fn
    # (defined in aura_jit_runtime.cpp) is called from
    # fiber.cpp's static init. Plus aura_jit.cpp
    # (defines emit_native_object, called from
    # aura_jit_bridge.cpp) and the contract / dispatch
    # stubs.
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit.cpp
    src/core/contract_stub.cpp
    src/compiler/aura_jit_prim_dispatch_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_205 PRIVATE src src/compiler)
target_compile_definitions(test_issue_205 PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_issue_205 PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_issue_205 PRIVATE ${llvm_libs})
target_compile_options(test_issue_205 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_link_libraries(test_issue_205 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_205_verification COMMAND ./test_issue_205)

# Issue #173 verification: stable-id type aliases (Phase 2 of
# #145 workstream 2). Verifies PairId/CellId/StringId +
# NULL_X_ID sentinels. Full migration (remap table,
# resolve_X, GC sweep compact) is the follow-up.
# Phase 2 pilot-33: converted using early helper. Very light (no sources; just include + options + link; helper provides base).
aura_add_issue_test(test_issue_173)
target_include_directories(test_issue_173 PRIVATE src src/compiler)


# Issue #181 verification: EvalValue 64-bit tagged encoding
# redesign (Cycle 1: Option A prototype). Verifies the v2
# string encoding has no tag collisions (no misclassification
# at the historical idx 19/31 collisions), roundtrip works,
# and is no slower than the old encoding. Full migration to
# the v2 encoding is Cycle 2 — this test exercises the
# prototype helpers only.
aura_add_issue_test(test_issue_181)
target_include_directories(test_issue_181 PRIVATE src src/compiler)

# Issue #182 verification: Hardware IR + Verilog Backend
# (Cycle 1: IR + accessors + display + query helpers, C++
# verification only).
#
# Issue #182 is by design 100% stdlib work (lib/std/eda.aura).
# The Aura binary's top-level `define` has upstream bugs
# (see lib/std/eda.aura DESIGN-NOTES) that block the stdlib
# port. To make Cycle 1 progress shippable, this test
# verifies the same IR concepts in C++. When Aura's macro
# /hygiene bugs are fixed, the stdlib version can be ported
# from this C++ prototype.
aura_add_issue_test(test_issue_182)
target_include_directories(test_issue_182 PRIVATE src src/compiler)

# Issue #187 verification: Arena compaction + double-arena strategy
# (P0 performance). Tests both the C++ API (ArenaStats, ASTArena::compact,
# StringPool::compact, ArenaGroup::auto_compact) and the Aura-level
# primitives (arena:compact, arena:estimate, arena:stats-json,
# string-pool:compact, string-pool:stats).
# Phase 2 pilot-N: full evaluator+JIT build (matches test_issue_137 pattern;
# the primitives being tested are registered in evaluator_impl.cpp).
aura_add_issue_test(test_issue_187)
target_include_directories(test_issue_187 PRIVATE src src/compiler)
target_sources(test_issue_187 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_187 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_187 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_187 PRIVATE ${llvm_libs})

# Issue #188 verification: Fine-grained incremental type checking +
# dirty propagation + Occurrence Typing re-validation. P5 issue.
# The shipped bits are the per-node dirty bitmask (kGeneralDirty /
# kConstraintDirty / kOccurrenceDirty / kOwnershipDirty /
# kCoercionDirty), the targeted re-analysis hooks
# (is_dirty_for / clear_dirty_for / dirty_column), and the Aura
# observability primitives ((dirty:reasons), (dirty:counts)).
# Phase 2 pilot-N: matches test_issue_140 pattern (light eval, no JIT).
aura_add_issue_test(test_issue_188)
target_include_directories(test_issue_188 PRIVATE src src/compiler)
target_sources(test_issue_188 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_188 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_188 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_188 PRIVATE ${llvm_libs})

# Issue #189 verification: race condition fix + observability
# (P0 concurrency). Tests the memory-ordering fix (26 callsites
# changed from relaxed to acq_rel), the reader-side snapshot API,
# and the 3 new observability primitives.
# Phase 2 pilot-N: matches test_issue_140 pattern (light eval).
aura_add_issue_test(test_issue_189)
target_include_directories(test_issue_189 PRIVATE src src/compiler)
target_sources(test_issue_189 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_189 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_189 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_189 PRIVATE ${llvm_libs})

# Issue #190 verification: SyntaxMarker::MacroIntroduced full
# pipeline integration (P0 hygiene). Tests the cloned_marker
# parameter on clone_macro_body, the 3 callsite updates
# (closure=User, macro paths=MacroIntroduced), and the new
# observability primitives.
# Phase 2 pilot-N: matches test_issue_140 pattern (light eval).
aura_add_issue_test(test_issue_190)
target_include_directories(test_issue_190 PRIVATE src src/compiler)
target_sources(test_issue_190 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_190 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_190 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_190 PRIVATE ${llvm_libs})

# Issue #191 verification: StableNodeRef + generation bump on
# structural mutate (P0 EDSL). Tests the StableNodeRef struct,
# the structural-mutation generation bump, and 4 new
# observability primitives.
# Phase 2 pilot-N: matches test_issue_190 pattern (light eval +
# LLVM/JIT for the structural primitives).
aura_add_issue_test(test_issue_191)
target_include_directories(test_issue_191 PRIVATE src src/compiler)
target_sources(test_issue_191 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_191 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_191 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_191 PRIVATE ${llvm_libs})

# Issue #192 verification: atomic batch mutate (P0 EDSL).
# Tests the mutate:atomic-batch primitive and
# atomic-batch:stats observability. Phase 2 pilot-N:
# matches test_issue_191 pattern.
aura_add_issue_test(test_issue_192)
target_include_directories(test_issue_192 PRIVATE src src/compiler)
target_sources(test_issue_192 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_192 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_192 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_192 PRIVATE ${llvm_libs})

# Issue #193 verification: per-function unhandled-opcode
# threshold for SpecJITController (jit). The threshold
# parameter was already in place; this verifies the
# backward-compat + production-safe threshold behaviors.
# Phase 2 pilot-N: matches test_issue_140 pattern (light eval).
aura_add_issue_test(test_issue_193)
target_include_directories(test_issue_193 PRIVATE src src/compiler)
target_sources(test_issue_193 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_193 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_193 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_193 PRIVATE ${llvm_libs})

# Issue #194 verification: per-lowering runtime→intrinsic
# migration observability (jit, #170 follow-up). The
# intrinsic_count counter was already in place; this verifies
# the Aura-level primitives (jit:intrinsic-count) and
# (jit:deopt-fn?) that surface the counter + per-function
# unhandled-opcode count for observability.
# Phase 2 pilot-N: matches test_issue_193 pattern.
aura_add_issue_test(test_issue_194)
target_include_directories(test_issue_194 PRIVATE src src/compiler)
target_sources(test_issue_194 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_194 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_194 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_194 PRIVATE ${llvm_libs})

# Issue #195 verification: structured EH (#170 follow-up).
# The full LLVM-native replacement (invoke + landingpad +
# personality function + per-fiber exception state) is a
# 1-2 week effort. This test verifies the existing
# thread_local-based structured EH (shipped in ae11053) still
# works for the single-fiber case and documents the
# limitation.
# Phase 2 pilot-N: matches test_issue_193 pattern.
aura_add_issue_test(test_issue_195)
target_include_directories(test_issue_195 PRIVATE src src/compiler)
target_sources(test_issue_195 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_195 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_195 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_195 PRIVATE ${llvm_libs})

# Issue #196 verification: incremental recompilation
# observability primitives (compile:cache-size, dirty-count,
# epoch, dep-edges). The full per-block / per-expression
# dirty tracking is a larger follow-up.
# Phase 2 pilot-N: matches test_issue_194 pattern.
aura_add_issue_test(test_issue_196)
target_include_directories(test_issue_196 PRIVATE src src/compiler)
target_sources(test_issue_196 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_196 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_196 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_196 PRIVATE ${llvm_libs})

# Issue #224: per-block re-lower consumer (cycle 2 of the
# smarter-re-lower follow-up to #196). The helper
# relower_define_blocks(name) consults the per-block
# bitmask and either skips the re-lower entirely (no
# dirty blocks → reuse cached IR) or does a full
# re-lower (at least one dirty block → lower_to_ir +
# store_define_v2). Cycle 2 is observability + safe
# early-exit; the true per-block re-lower is cycle 3.
# Reuses the test_issue_196 module dep chain (same
# CXX_MODULES list, same source files).
aura_add_issue_test(test_issue_224)
target_include_directories(test_issue_224 PRIVATE src src/compiler tests)
target_sources(test_issue_224 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_224 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_224 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_224 PRIVATE ${llvm_libs})

# Issue #197: branch-aware inliner + parameter renaming
# (extends InlinePass from #171). Predicate + single-block
# transformation shipped; multi-block CFG splicing is a
# follow-up. Pattern matches test_issue_171 (direct C++ tests
# on IRModule, no Aura layer needed).
# Issue #197: branch-aware inliner + parameter renaming
# (extends InlinePass from #171). Predicate + single-block
# transformation shipped; multi-block CFG splicing is a
# follow-up. Pattern matches test_issue_171 (direct C++ tests
# on IRModule, no Aura layer needed).
add_executable(test_issue_197 tests/test_issue_197.cpp)
set_property(TARGET test_issue_197 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_197 PRIVATE src src/compiler)
target_compile_options(test_issue_197 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_197 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_197_verification COMMAND ./test_issue_197)

# Issue #246 verification: Inliner MacroIntroduced-awareness.
# Scope-limited close: IRFunction.marker + InlinePass consults
# it via respect_macro_hygiene_ static flag (default true).
# Pure C++ test on IRModule + IRFunction + InlinePass, no Aura.
add_executable(test_issue_246 tests/test_issue_246.cpp)
set_property(TARGET test_issue_246 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_246 PRIVATE src src/compiler)
target_compile_options(test_issue_246 PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_246 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_246_inliner_macro_hygiene_verification COMMAND ./test_issue_246)

# Issue #247 verification: SyntaxMarker observability integration
# (CompilerSnapshot marker fields + query:marker-stats primitive).
# Integration test (uses CompilerService).
aura_add_issue_test(test_issue_247)
target_include_directories(test_issue_247 PRIVATE src src/compiler)
target_sources(test_issue_247 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_247 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_247 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_247 PRIVATE ${llvm_libs})
add_test(NAME issue_247_marker_observability_verification COMMAND ./test_issue_247)

# Issue #248 verification: query:schema-of-marker primitive.
# Observability side of macro-type-tracking. Integration test
# (uses CompilerService).
aura_add_issue_test(test_issue_248)
target_include_directories(test_issue_248 PRIVATE src src/compiler)
target_sources(test_issue_248 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_248 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_248 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_248 PRIVATE ${llvm_libs})
add_test(NAME issue_248_schema_of_marker_verification COMMAND ./test_issue_248)

# Issue #249 verification: StableNodeRef ergonomics.
# Children/parent stable accessors + Aura primitives.
# Integration test (uses CompilerService).
aura_add_issue_test(test_issue_249)
target_include_directories(test_issue_249 PRIVATE src src/compiler)
target_sources(test_issue_249 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_249 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_249 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_249 PRIVATE ${llvm_libs})
add_test(NAME issue_249_stable_node_ref_ergonomics_verification COMMAND ./test_issue_249)

# Issue #252 verification: closure dual-path observability.
# Scope-limited close: 5 counters + closure:stats primitive.
# Follow-up #396 will add the actual fast paths.
# Phase 2 pilot-46: converted using early helper. Mirrors
# test_issue_250's per-test source setup (uses CompilerService
# which pulls in JIT + observability + the TLarena globals).
aura_add_issue_test(test_issue_252)
target_include_directories(test_issue_252 PRIVATE src src/compiler)
target_sources(test_issue_252 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h
    src/core/contract_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_252 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_252 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_252 PRIVATE ${llvm_libs})
add_test(NAME issue_252_closure_observability_verification COMMAND ./test_issue_252)

# Issue #253 verification: linear-move elision in TypeSpecializationWrap
# (Phase 3 slice of #149 roadmap). Scope-limited close.
# Pass runs TypeSpecializationWrap → counts MoveOp elisions where
# source has linear_ownership_state == Owned. The counter is exposed
# via CompilerMetrics / CompilerSnapshot + the
# (compile:linear-elide-count) Aura primitive.
aura_add_issue_test(test_issue_253)
target_include_directories(test_issue_253 PRIVATE src src/compiler)
target_sources(test_issue_253 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h
    src/core/contract_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_253 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_253 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_253 PRIVATE ${llvm_libs})
add_test(NAME issue_253_linear_move_elision_verification COMMAND ./test_issue_253)

# Issue #254 verification: IR SoA dual-emit foundation
# (Phase 2 slice of the #254 SoA migration roadmap). Scope-limited
# close. LoweringState gets an opt-in dual-emit path: emit() +
# alloc_block() + set_cur_function() write to both AoS and SoA
# when dual_emit_soa is enabled. The (compile:ir-soa-stats) Aura
# primitive exposes the dual-emit counters. The real lowering
# path (lower_to_ir / lower_to_ir_with_cache) does NOT opt into
# dual-emit yet — that's a follow-up.
aura_add_issue_test(test_issue_254)
target_include_directories(test_issue_254 PRIVATE src src/compiler)
target_sources(test_issue_254 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
    src/compiler/observability_snapshot.h
    src/core/contract_stub.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_254 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_254 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_254 PRIVATE ${llvm_libs})
add_test(NAME issue_254_ir_soa_dual_emit_foundation_verification COMMAND ./test_issue_254)

# Issue #250 verification: atomic-batch truly atomic.
# Integration test (uses CompilerService).
aura_add_issue_test(test_issue_250)
target_include_directories(test_issue_250 PRIVATE src src/compiler)
target_sources(test_issue_250 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_250 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_250 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_250 PRIVATE ${llvm_libs})
add_test(NAME issue_250_atomic_batch_truly_atomic_verification COMMAND ./test_issue_250)

# Issue #197 observability: (compile:inline-pass-stats)
# Aura primitive. Tests the Aura-level hook + hash return
# shape. Pattern matches test_issue_194 (small Aura-level
# observability test).
aura_add_issue_test(test_issue_197_observability)
target_include_directories(test_issue_197_observability PRIVATE src src/compiler)
target_sources(test_issue_197_observability PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_197_observability PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_197_observability PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_197_observability PRIVATE ${llvm_libs})

# Issue #206 verification: GC sweep compact + remap +
# resolve_X helpers. Tests the compact_pairs() and
# resolve_pair() API on the Evaluator.
aura_add_issue_test(test_issue_206)
target_include_directories(test_issue_206 PRIVATE src src/compiler)
target_sources(test_issue_206 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_206 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_206 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_206 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")

# Issue #174 Cycle 1 verification: Env::bindings_ migration
# scaffold. Tests bindings_symid_iter(), bindings_with_names(),
# and the bindings_legacy_uses metric.
aura_add_issue_test(test_issue_174)
target_include_directories(test_issue_174 PRIVATE src src/compiler)
target_sources(test_issue_174 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_174 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_174 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_174 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")

# Issue #208 Cycle 2 verification: bindings_legacy_uses
# metric in the integration path. Tests the metric +
# accessors; the full 25-site migration is a separate
# multi-day effort.
aura_add_issue_test(test_issue_208)
target_include_directories(test_issue_208 PRIVATE src src/compiler)
target_sources(test_issue_208 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_208 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_208 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_208 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")

# Issue #210 Cycle 4 verification: env cleanup-readiness
# tests. The actual `drop bindings_` commit is deferred
# to after the Cycle 2.5+ migration completes (per the
# plan in #208). This test exercises the post-migration
# invariants.
aura_add_issue_test(test_issue_210)
target_include_directories(test_issue_210 PRIVATE src src/compiler)
target_sources(test_issue_210 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_210 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_210 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_210 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")

# Issue #211 verification: (tag, arity) index for
# query:pattern primitive. Verifies the index is built
# on demand, used for fast lookup, and invalidated on
# workspace changes.
aura_add_issue_test(test_issue_211)
target_include_directories(test_issue_211 PRIVATE src src/compiler)
target_sources(test_issue_211 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_211 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_211 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_211 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")

# Issue #212 verification: pure-function extraction of
# constant_folding (Cycle 1). Verifies the new
# `aura.compiler.constant_folding` module is the
# canonical home for the fold logic, and that
# `ConstantFoldingWrap` (in pass_manager.ixx) is now a
# thin wrapper. Light target — no evaluator/serve deps;
# this is a pure module test, exercised against
# hand-built IRFunctions.
aura_add_issue_test(test_issue_212)
# C++26 contract enforcement: test_212 calls
# arithmetic_div_pure which has a `pre(...)` contract. The
# contract violation handler is __tu_has_violation, a
# weak runtime symbol provided by GCC's -fcontracts
# enforcement. The aura target gets -fcontracts; the
# per-test helper does not. Enable here to match.
# Note: -fno-contracts is also tried first (see below).
# The aura.compiler.constant_folding module is built with
# -fcontracts and embeds a contract check on
# arithmetic_div_pure. The check generates a call to
# __tu_has_violation in the test's .o file. The
# libaura_test_objects.a archive has __tu_has_violation as
# a LOCAL symbol (compiler-generated, "t" in nm), which
# static libraries do not export. We work around this by
# providing a default-visibility stub at the bottom of
# test_issue_212.cpp.
target_sources(test_issue_212 PRIVATE
    # aura_alloc_float / aura_float_ref are defined in
    # aura_jit.cpp (LLVM-emitted extern "C" helpers used
    # by the runtime float pool). test_212 calls
    # types::make_float / types::as_float, which reference
    # these symbols. The libaura_test_objects.a archive
    # does not include aura_jit.cpp (per-binary compile
    # flag constraint), so the test must include it as a
    # per-target source. We also need aura_jit_runtime.cpp
    # (Impl definition for AuraJIT's pImpl) and LLVM libs
    # since aura_jit.cpp pulls in LLVM headers.
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_212 PRIVATE src src/compiler)
target_compile_definitions(test_issue_212 PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_issue_212 PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_issue_212 PRIVATE ${llvm_libs})

# Issue #212 Phase 4: pure-function path benchmark.
# Measures the perf cost of the pure-function extraction
# (constant_folding + arithmetic_sum) vs the legacy Wrap
# path. The benchmark runs in-process, <2s total, and
# writes JSON to tests/bench_results/issue_212_bench.json.
add_executable(issue_212_bench tests/bench/issue_212_bench.cpp)
set_property(TARGET issue_212_bench PROPERTY CXX_MODULE_STD ON)

# Issue #217 Cycle 14 P4: benchmark suite for the C++26 P2996
# reflection + auto_serialize/auto_deserialize path. Measures
# throughput across 4 dimensions (FlatAST SoA, NodeView
# roundtrip, vector<string> serialize, columnar scan vs AoS)
# and outputs JSON to tests/bench_results/cycle14_reflect_bench.json
# for data-driven design decisions.
add_executable(cycle14_reflect_bench tests/bench/cycle14_reflect_bench.cpp)
set_property(TARGET cycle14_reflect_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(cycle14_reflect_bench PRIVATE src)
target_compile_options(cycle14_reflect_bench PRIVATE -freflection -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(cycle14_reflect_bench PRIVATE stdc++ aura-reflect)
add_test(NAME cycle14_reflect_bench COMMAND ./cycle14_reflect_bench)

# Issue #221 Cycle 14 P5: benchmark suite for PersistentChildVector
# (slice 5/5 — the ship gate for #221). Measures throughput across
# 4 dimensions (PCV ops perf vs std::vector, COW back-reference
# refcount growth, simulated #177 rollback flow, persistence
# stress with the AC of < 2µs/op) and outputs JSON to
# tests/bench_results/cycle221_pcv_bench.json for data-driven
# design decisions. Standalone TU (no module imports — avoids the
# GCC 16.1 std module + P2996 reflection conflict). The PCV header
# is header-only.
add_executable(cycle221_pcv_bench tests/bench/cycle221_pcv_bench.cpp)
target_include_directories(cycle221_pcv_bench PRIVATE src)
target_compile_options(cycle221_pcv_bench PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_compile_features(cycle221_pcv_bench PUBLIC cxx_std_26)
target_link_libraries(cycle221_pcv_bench PRIVATE stdc++ pthread)
add_test(NAME cycle221_pcv_bench COMMAND ./cycle221_pcv_bench)
target_include_directories(issue_212_bench PRIVATE src)
target_sources(issue_212_bench PRIVATE
    src/core/ast_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/compute_kind_impl.cpp
    tests/_stubs/issue_212_runtime_stubs.cpp)
# Match aura_test_objects's compile flags so the prebuilt BMIs
# in module_cache/aura_test_objects.dir/ (produced by the .ixx
# compile) have the same identity the .cpp imports expect.
target_compile_options(issue_212_bench PRIVATE -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(issue_212_bench PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_212_bench COMMAND ./issue_212_bench)

# Issue #213 verification: rollback mechanism for the mutation
# boundary API (Cycle 1). Verifies that
# `Evaluator::exit_mutation_boundary(false)` actually rolls
# back the mutations recorded between enter and exit, via
# the new `FlatAST::rollback_to_size(checkpoint_size)` helper.
#
# Heavy target (full evaluator + serve + parser + JIT runtime)
# so the MutationBoundaryGuard + workspace_flat_ + workspace_pool_
# are available, matching the test_issue_210 / 211 pattern.
aura_add_issue_test(test_issue_213)
target_include_directories(test_issue_213 PRIVATE src src/compiler)
target_sources(test_issue_213 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_213 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_213 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_213 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
add_test(NAME issue_213_verification COMMAND ./test_issue_213)

# Issue #213 verification: integration-level smoke test for
# the migrated mutate:* primitives + the per-fiber state.
# Companion to tests/test_issue_213.cpp (which is the unit-level
# coverage). This is the canonical test for #177 (the parent
# issue that #213 completes).
#
# Uses the test_issue_211 heavy target pattern (full evaluator
# + serve + parser + JIT runtime) so the MutationBoundaryGuard
# + workspace_flat_ + workspace_pool_ APIs are all available.
aura_add_issue_test(test_issue_177)
target_include_directories(test_issue_177 PRIVATE src src/compiler)
target_sources(test_issue_177 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_177 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_177 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_177 PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
add_test(NAME issue_177_verification COMMAND ./test_issue_177)

# Companion to tests/test_issue_213.cpp (unit-level) and
# tests/test_issue_177.cpp (integration smoke). This test
# covers the 3 'Test scenarios' follow-up from the issue
# body:
#   5. Concurrent fibers don't see each other's checkpoints
#   6. Panic + abort mid-mutation triggers clean rollback
#   7. 1000+ iteration stress test
#
# Same heavy target pattern as test_issue_177 — needs the
# scheduler + fiber APIs.
aura_add_issue_test(test_issue_213_panic_fiber)
target_include_directories(test_issue_213_panic_fiber PRIVATE src src/compiler)
target_sources(test_issue_213_panic_fiber PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
src/compiler/aura_jit_bridge.cpp
)
target_include_directories(test_issue_213_panic_fiber PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_213_panic_fiber PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_213_panic_fiber PRIVATE ${llvm_libs})
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
add_test(NAME issue_213_panic_fiber_verification COMMAND ./test_issue_213_panic_fiber)

# Issue #214 Cycle 1: reflect_module_exports + stdlib source
# scanner + query:module-exports primitive.
#
# Standalone C++ test (not linked into the Aura evaluator)
# because the C++ side tests the compile-time reflection in
# reflect.hh (requires -freflection flag, which the aura
# target doesn't use). The Aura-side test (Test 4) shells
# out to the ./build/aura binary.
add_executable(test_issue_214 tests/test_issue_214.cpp)
set_property(TARGET test_issue_214 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_214 PRIVATE src)
target_compile_options(test_issue_214 PRIVATE -freflection -Wall)
target_compile_features(test_issue_214 PUBLIC cxx_std_26)
target_link_libraries(test_issue_214 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_214_verification COMMAND ./test_issue_214)

# Issue #215: auto_deserialize for containers + nested struct
# (Issue #178 Cycle 2).
#
# Standalone C++ test (like test_issue_214) because the
# test exercises the reflect.hh header directly with the
# -freflection compile flag.
add_executable(test_issue_215 tests/test_issue_215.cpp)
set_property(TARGET test_issue_215 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_215 PRIVATE src)
target_compile_options(test_issue_215 PRIVATE -freflection -Wall)
target_compile_features(test_issue_215 PUBLIC cxx_std_26)
target_link_libraries(test_issue_215 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_215_verification COMMAND ./test_issue_215)

# Issue #216: query:schema + mutate integration
# (Issue #178 Cycle 3).
#
# Standalone C++ test (like test_issue_214/215) because
# the test exercises the reflect_schema.hh header directly
# with the -freflection compile flag. Tests 3-6 shell out
# to the ./build/aura binary to test the Aura-side
# query:schema primitive.
add_executable(test_issue_216 tests/test_issue_216.cpp)
set_property(TARGET test_issue_216 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_216 PRIVATE src)
target_compile_options(test_issue_216 PRIVATE -freflection -Wall)
target_compile_features(test_issue_216 PUBLIC cxx_std_26)
target_link_libraries(test_issue_216 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_216_verification COMMAND ./test_issue_216)

# Issue #217: AST/IR migration to reflect_members +
# auto_serialize (Issue #178 Cycle 4). The full scope is
# 1 week of focused work; this test is the Cycle 1 pilot
# that verifies the reflection infrastructure works for
# IR-shaped types (OpcodeInfo, IRInstruction).
add_executable(test_issue_217 tests/test_issue_217.cpp)
set_property(TARGET test_issue_217 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_217 PRIVATE src)
target_compile_options(test_issue_217 PRIVATE -freflection -Wall)
target_compile_features(test_issue_217 PUBLIC cxx_std_26)
target_link_libraries(test_issue_217 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_217_verification COMMAND ./test_issue_217)

# Issue #219: gap-buffer child_data_ for O(1) insert/remove
# in FlatAST. Tests the GapBuffer<T> template defined in
# src/core/gap_buffer.hh. Standalone test (no production
# module imports), so it doesn't trigger the GCC 16.1
# std module + P2996 reflection conflict.
add_executable(test_issue_219 tests/test_issue_219.cpp)
target_include_directories(test_issue_219 PRIVATE src)
target_compile_options(test_issue_219 PRIVATE -Wall -Wextra)
target_compile_features(test_issue_219 PUBLIC cxx_std_26)
target_link_libraries(test_issue_219 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_219_verification COMMAND ./test_issue_219)

# Issue #220: per-node children linked list (Issue #179 Cycle 2).
# Replaces FlatAST's std::pmr::vector<NodeId> child_data_ (with
# child_begin_/child_count_ SoA columns) with a per-node
# std::pmr::vector<std::pmr::vector<NodeId>> children_ field.
# The per-node list gives O(1) amortized insert/erase at a
# specific node (just std::pmr::vector ops on that node's list;
# no more shifting of "all subsequent child_begin_ entries").
# Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict).
add_executable(test_issue_220 tests/test_issue_220.cpp)
target_include_directories(test_issue_220 PRIVATE src)
target_compile_options(test_issue_220 PRIVATE -Wall -Wextra)
target_compile_features(test_issue_220 PUBLIC cxx_std_26)
target_link_libraries(test_issue_220 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_220_verification COMMAND ./test_issue_220)

# Issue #221: persistent child vectors (Issue #179 Cycle 3,
# slice 1/5). Tests the PersistentChildVector<T> data
# structure defined in src/core/persistent_child_vector.hh.
# Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict).
add_executable(test_issue_221 tests/test_issue_221.cpp)
target_include_directories(test_issue_221 PRIVATE src)
target_compile_options(test_issue_221 PRIVATE -Wall -Wextra)
target_compile_features(test_issue_221 PUBLIC cxx_std_26)
target_link_libraries(test_issue_221 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_221_verification COMMAND ./test_issue_221)

# Issue #222: structural mutation concurrency safety (Issue #179
# Cycle 4, slice 1/3). Tests the begin_structural_mutation() /
# end_structural_mutation() RAII guard + set_child / insert_child
# / remove_child routing + try_acquire_reader_lock() reader-side
# API. Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict). Uses a TestFlatAST
# stub that mirrors the production API for testing purposes.
add_executable(test_issue_222 tests/test_issue_222.cpp)
target_include_directories(test_issue_222 PRIVATE src)
target_compile_options(test_issue_222 PRIVATE -Wall -Wextra)
target_compile_features(test_issue_222 PUBLIC cxx_std_26)
target_link_libraries(test_issue_222 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_222_verification COMMAND ./test_issue_222)

# Issue #223: closure-bridge epoch counter for lifetime tracking
# (Issue #180 Cycle 1). Tests bridge_epoch_ on ClosureBridgeData +
# IRClosure + the bridge_epoch() / bump_bridge_epoch() / reset()
# epoch tracking on CompilerService. Standalone test (no module
# imports — avoids the GCC 16.1 std module + P2996 reflection
# conflict). Uses a MockEpochTracker that mirrors the production
# mutation_epoch_ / bridge_epoch() semantics.
add_executable(test_issue_223 tests/test_issue_223.cpp)
target_include_directories(test_issue_223 PRIVATE src)
target_compile_options(test_issue_223 PRIVATE -Wall -Wextra)
target_compile_features(test_issue_223 PUBLIC cxx_std_26)
target_link_libraries(test_issue_223 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME issue_223_verification COMMAND ./test_issue_223)

# Issue #224 cycle 2: shared_ptr-based bridge ownership.
# Tests verify the new std::shared_ptr<const T> fields
# in ClosureBridgeData and IRClosure. The shared_ptr
# keeps the FlatAST alive as long as the bridge exists
# — even after the lowering arena is reset.
aura_add_issue_test(test_issue_224_closure_bridge)
target_include_directories(test_issue_224_closure_bridge PRIVATE src src/compiler tests)
target_sources(test_issue_224_closure_bridge PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_224_closure_bridge PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_224_closure_bridge PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_224_closure_bridge PRIVATE ${llvm_libs})
add_test(NAME issue_224_closure_bridge_verification COMMAND ./test_issue_224_closure_bridge)

# Issue #225 cycle 3: bridge invalidation. Tests verify
# that mark_define_dirty / hot_swap_function_impl / reset()
# all bump the bridge_epoch_ field (Issue #223) on the
# affected entry's bridge data, so any closure holding a
# reference will detect staleness and re-parse.
aura_add_issue_test(test_issue_225_bridge_invalidation)
target_include_directories(test_issue_225_bridge_invalidation PRIVATE src src/compiler tests)
target_sources(test_issue_225_bridge_invalidation PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_225_bridge_invalidation PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_225_bridge_invalidation PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_225_bridge_invalidation PRIVATE ${llvm_libs})
add_test(NAME issue_225_bridge_invalidation_verification COMMAND ./test_issue_225_bridge_invalidation)

# Issue #226 Cycle 4: closure-bridge tests + invariants + TSan
# (Issue #180 Cycle 4). 4 test sections — one per cycle:
#   1. concurrent mutate + closure invoke (100 fibers)
#   2. arena reset + bridge fallback
#   3. post-mutation invariant checks
#   4. doomsday stress (200 fibers, random ops)
aura_add_issue_test(test_issue_226)
target_include_directories(test_issue_226 PRIVATE src src/compiler tests)

# Issue #237: AOT end-to-end test (spawns aura --emit-binary as subprocess,
# verifies the resulting ELF runs and produces expected output).
# This is a pure-POSIX integration test; no Aura runtime imports needed.
add_executable(test_issue_237 tests/test_issue_237.cpp)
target_include_directories(test_issue_237 PRIVATE tests)
target_link_libraries(test_issue_237 PRIVATE aura_test_objects)
add_test(NAME issue_237_verification COMMAND ./test_issue_237)
target_sources(test_issue_226 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_226 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_226 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_226 PRIVATE ${llvm_libs})
add_test(NAME issue_226_closure_bridge_stress_verification COMMAND ./test_issue_226)

# Issue #227: Occurrence Typing narrowing + ADT exhaustiveness
# after Typed Mutations (post #168 epoch fix). Cycle 1 of the
# per-node epoch tracking roadmap. This test captures the
# *current* coarse epoch gate state (Phase 1 of #168) and
# verifies the existing infrastructure works end-to-end.
# Full per-node epoch tracking + recursive free_vars staleness
# check is a separate follow-up (the issue's Recommended Fix #1-2).
aura_add_issue_test(test_issue_227)
target_include_directories(test_issue_227 PRIVATE src src/compiler tests)
target_sources(test_issue_227 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_227 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_227 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_227 PRIVATE ${llvm_libs})
add_test(NAME issue_227_occurrence_narrowing_verification COMMAND ./test_issue_227)

# Issue #240: per-node occurrence-dirty bit (fine-grained scoping
# for find_occurrence_contexts). Tests the new Aura primitives
# (compile:mark-narrowing-dirty!, compile:narrowing-dirty?) and
# verifies that post_mutation_invariant_check scopes its
# diagnostic to kOccurrenceDirty-tagged nodes vs the conservative
# general-dirty fallback. Uses CompilerService + FlatAST dirty
# bitmask (from #188) — no JIT needed.
aura_add_issue_test(test_issue_240)
target_include_directories(test_issue_240 PRIVATE src src/compiler tests)
target_sources(test_issue_240 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_240 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_240 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_240 PRIVATE ${llvm_libs})
add_test(NAME issue_240_per_node_occurrence_dirty_verification COMMAND ./test_issue_240)

# Issue #242: EnvFrame/EnvId SoA dual-path consistency +
# version stamping + arena rollback hardening. Tests Phase 1
# (version stamping in alloc_env_frame), Phase 2 (is_env_frame_stale
# + materialize_call_env detection), Phase 3 (save/restore panic
# checkpoint snapshots 4 pmr arena sizes). Uses CompilerService
# for layer 2 (closure capture + mutate + consistency); direct
# C++ tests for layers 1+3.
aura_add_issue_test(test_issue_242)
target_include_directories(test_issue_242 PRIVATE src src/compiler tests)
target_sources(test_issue_242 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_242 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_242 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_242 PRIVATE ${llvm_libs})
add_test(NAME issue_242_envframe_version_stamping_verification COMMAND ./test_issue_242)

# Issue #243: AOT bridge enhancement (mangle versioning + AOT
# observability). Phase 1 (mangle with defuse_version suffix)
# + Phase 3 (AOT bridge observability: start banner, per-
# function progress, collision warning version, aot_emit_version
# symbol). Incremental re-AOT for dirty functions is deferred
# to a separate follow-up issue. Tests are pure C++ (no
# CompilerService dependency) — the mangle helper is in
# aot_mangle.h and the C-linkage setter/getter are
# extern "C" in aura_jit_bridge.cpp.
aura_add_issue_test(test_issue_243)
target_include_directories(test_issue_243 PRIVATE src src/compiler tests)
# test_issue_243 doesn't link the full aura_jit.cpp (which would
# pull in ir_executor_impl.cpp and its TL arena dependencies).
# The C-linkage setter/getter for g_aot_defuse_version are
# trivial — we provide a local stub here so the test only
# exercises the header-only mangle_aot_name and the
# setter/getter round-trip. The real bridge TU
# (aura_jit_bridge.cpp) is exercised by test_issue_237's
# end-to-end emit-binary path.
add_test(NAME issue_243_aot_bridge_enhancement_verification COMMAND ./test_issue_243)

# Issue #244: SyntaxMarker query primitives + :marker field
# in query:where. Phase 1 (scope-limited close) ships:
#   - :marker field for (where :marker "MacroIntroduced"|"User"|"BoolLiteral")
#   - (query:by-marker marker-name) primitive
#   - (query:macro-introduced) shortcut
# Deferred: mutate:* guards for MacroIntroduced nodes.
aura_add_issue_test(test_issue_244)
target_include_directories(test_issue_244 PRIVATE src src/compiler)
target_sources(test_issue_244 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_244 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_244 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_244 PRIVATE ${llvm_libs})
add_test(NAME issue_244_syntax_marker_query_verification COMMAND ./test_issue_244)

# Issue #228: Hardware IR Dependent Type System for Agentic EDA.
# Phase 1 minimum viable — tests the Aura-level dependent type
# library extension (lib/std/eda.aura Phase 1 addition: BitVec,
# ClockDomain, ResetType, TypedSignal + compatibility checks).
# Phases 2-5 are deferred to follow-up issues.
aura_add_issue_test(test_issue_228)
target_include_directories(test_issue_228 PRIVATE src src/compiler tests)
target_sources(test_issue_228 PRIVATE
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/observability_metrics.h
src/compiler/observability_snapshot.h
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_issue_228 PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test_issue_228 PRIVATE AURA_HAVE_LLVM=1)
target_link_libraries(test_issue_228 PRIVATE ${llvm_libs})
add_test(NAME issue_228_dependent_type_system_verification COMMAND ./test_issue_228)

# Issue #178 Cycle 5: production NodeView migration to
# reflect_members + auto_serialize.
#
# Imports aura.core.ast (for the real NodeView) and uses
# the reflect path to roundtrip a populated NodeView.
# Verifies the generic reflect path works on the actual
# production struct (12 fields: 8 POD scalars + 3 spans
# + 1 enum-byte).
add_executable(test_issue_178 tests/test_issue_178.cpp)
set_property(TARGET test_issue_178 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_178 PRIVATE src)
target_compile_options(test_issue_178 PRIVATE -freflection -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-misleading-indentation
    -Wno-parentheses)
target_link_libraries(test_issue_178 PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
add_test(NAME issue_178_verification COMMAND ./test_issue_178)

# ── Aggregate target: build all test_issue_* binaries in one ninja invocation ──
#
# Issue: the CI's `build.py test issues` step ran `ninja -k 0` for each
# test_issue_* target individually via run_issue_tests.py, and the
# per-target CMakeLists.txt fragments added for test_issue_225/226 (LLVM
# link, full module dep chain) made each target take minutes to compile.
# On a CI runner with limited cores (2-4), 62 targets × ~30s each = ~30min,
# easily exceeding the 1200s subprocess timeout.
#
# This aggregate target lets cmd_build (in build.py) compile them all
# ONCE during the build phase. The test_issues suite then just runs the
# binaries — no rebuild needed in the test step.
#
# We can't use add_custom_target(... DEPENDS ...) because the targets
# already exist; instead, we list them as dependencies of a custom
# phony target that ninja can schedule in parallel.
add_custom_target(all_test_issue_targets ALL)
add_dependencies(all_test_issue_targets test_issue_115)
add_dependencies(all_test_issue_targets test_issue_116)
add_dependencies(all_test_issue_targets test_issue_117)
add_dependencies(all_test_issue_targets test_issue_118)
add_dependencies(all_test_issue_targets test_issue_119)
add_dependencies(all_test_issue_targets test_issue_120)
add_dependencies(all_test_issue_targets test_issue_121)
add_dependencies(all_test_issue_targets test_issue_122)
add_dependencies(all_test_issue_targets test_issue_123)
add_dependencies(all_test_issue_targets test_issue_124)
add_dependencies(all_test_issue_targets test_issue_125)
add_dependencies(all_test_issue_targets test_issue_126)
add_dependencies(all_test_issue_targets test_issue_127)
add_dependencies(all_test_issue_targets test_issue_128)
add_dependencies(all_test_issue_targets test_issue_130)
add_dependencies(all_test_issue_targets test_issue_131)
add_dependencies(all_test_issue_targets test_issue_132)
add_dependencies(all_test_issue_targets test_issue_134)
add_dependencies(all_test_issue_targets test_issue_135)
add_dependencies(all_test_issue_targets test_issue_136)
add_dependencies(all_test_issue_targets test_issue_137)
add_dependencies(all_test_issue_targets test_issue_138)
add_dependencies(all_test_issue_targets test_issue_139)
add_dependencies(all_test_issue_targets test_issue_140)
add_dependencies(all_test_issue_targets test_issue_141)
add_dependencies(all_test_issue_targets test_issue_142)
add_dependencies(all_test_issue_targets test_issue_143)
add_dependencies(all_test_issue_targets test_issue_144)
add_dependencies(all_test_issue_targets test_issue_145)
add_dependencies(all_test_issue_targets test_issue_146)
add_dependencies(all_test_issue_targets test_issue_147)
add_dependencies(all_test_issue_targets test_issue_148)
add_dependencies(all_test_issue_targets test_issue_149)
add_dependencies(all_test_issue_targets test_issue_158)
add_dependencies(all_test_issue_targets test_issue_159)
add_dependencies(all_test_issue_targets test_issue_159_bench)
add_dependencies(all_test_issue_targets test_issue_161)
add_dependencies(all_test_issue_targets test_issue_162)
add_dependencies(all_test_issue_targets test_issue_163)
add_dependencies(all_test_issue_targets test_issue_164)
add_dependencies(all_test_issue_targets test_issue_165)
add_dependencies(all_test_issue_targets test_issue_166)
add_dependencies(all_test_issue_targets test_issue_167)
add_dependencies(all_test_issue_targets test_issue_168)
add_dependencies(all_test_issue_targets test_issue_169)
add_dependencies(all_test_issue_targets test_issue_170)
add_dependencies(all_test_issue_targets test_issue_171)
add_dependencies(all_test_issue_targets test_issue_173)
add_dependencies(all_test_issue_targets test_issue_174)
add_dependencies(all_test_issue_targets test_issue_177)
add_dependencies(all_test_issue_targets test_issue_178)
add_dependencies(all_test_issue_targets test_issue_181)
add_dependencies(all_test_issue_targets test_issue_182)
add_dependencies(all_test_issue_targets test_issue_184)
add_dependencies(all_test_issue_targets test_issue_187)
add_dependencies(all_test_issue_targets test_issue_188)
add_dependencies(all_test_issue_targets test_issue_189)
add_dependencies(all_test_issue_targets test_issue_190)
add_dependencies(all_test_issue_targets test_issue_191)
add_dependencies(all_test_issue_targets test_issue_192)
add_dependencies(all_test_issue_targets test_issue_193)
add_dependencies(all_test_issue_targets test_issue_194)
add_dependencies(all_test_issue_targets test_issue_195)
add_dependencies(all_test_issue_targets test_issue_196)
add_dependencies(all_test_issue_targets test_issue_197)
add_dependencies(all_test_issue_targets test_issue_197_observability)
add_dependencies(all_test_issue_targets test_issue_204)
add_dependencies(all_test_issue_targets test_issue_205)
add_dependencies(all_test_issue_targets test_issue_206)
add_dependencies(all_test_issue_targets test_issue_208)
add_dependencies(all_test_issue_targets test_issue_210)
add_dependencies(all_test_issue_targets test_issue_211)
add_dependencies(all_test_issue_targets test_issue_212)
add_dependencies(all_test_issue_targets test_issue_213)
add_dependencies(all_test_issue_targets test_issue_213_panic_fiber)
add_dependencies(all_test_issue_targets test_issue_214)
add_dependencies(all_test_issue_targets test_issue_215)
add_dependencies(all_test_issue_targets test_issue_216)
add_dependencies(all_test_issue_targets test_issue_217)
add_dependencies(all_test_issue_targets test_issue_219)
add_dependencies(all_test_issue_targets test_issue_220)
add_dependencies(all_test_issue_targets test_issue_221)
add_dependencies(all_test_issue_targets test_issue_222)
add_dependencies(all_test_issue_targets test_issue_223)
add_dependencies(all_test_issue_targets test_issue_224)
add_dependencies(all_test_issue_targets test_issue_224_closure_bridge)
add_dependencies(all_test_issue_targets test_issue_225_bridge_invalidation)
add_dependencies(all_test_issue_targets test_issue_226)
add_dependencies(all_test_issue_targets test_issue_227)
add_dependencies(all_test_issue_targets test_issue_228)
add_dependencies(all_test_issue_targets test_issue_237)
add_dependencies(all_test_issue_targets test_issue_240)
add_dependencies(all_test_issue_targets test_issue_242)
