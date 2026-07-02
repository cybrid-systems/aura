cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(aura LANGUAGES CXX C)
include(${CMAKE_SOURCE_DIR}/cmake/AuraTest.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/AuraModules.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/AuraIssueBundles.cmake)


# P0: evaluator implementation partitions (same module: aura.compiler.evaluator).
set(AURA_EVALUATOR_SOURCE_FILES
    src/compiler/evaluator_ctor.cpp
    src/compiler/evaluator_adt.cpp
    src/compiler/evaluator_defuse_index.cpp
    src/compiler/evaluator_eval_flat.cpp
    src/compiler/macro_expansion.cpp
    src/compiler/evaluator_primitives_registry.cpp
    src/compiler/evaluator_env.cpp
    src/compiler/evaluator_module_loader.cpp
    src/compiler/evaluator_workspace_tree.cpp
    src/compiler/evaluator_gc.cpp
    src/compiler/evaluator_typecheck.cpp
    src/compiler/evaluator_primitives_builtins.cpp
    src/compiler/evaluator_fiber_mutation.cpp
    src/compiler/evaluator_query_index.cpp
    src/compiler/evaluator_primitives_core.cpp
    src/compiler/evaluator_primitives_pair.cpp
    src/compiler/evaluator_primitives_list.cpp
    src/compiler/evaluator_primitives_json.cpp
    src/compiler/evaluator_primitives_vector.cpp
    src/compiler/evaluator_primitives_math.cpp
    src/compiler/evaluator_primitives_reflect.cpp
    src/compiler/evaluator_primitives_query.cpp
    src/compiler/evaluator_primitives_query_workspace.cpp
    src/compiler/evaluator_primitives_query_defuse.cpp
    src/compiler/evaluator_primitives_mutate.cpp
    src/compiler/query_matcher.cpp
    src/compiler/evaluator_primitives_workspace.cpp
    src/compiler/evaluator_primitives_ast.cpp
    src/compiler/evaluator_primitives_compile.cpp
    src/compiler/evaluator_primitives_observability.cpp
    src/compiler/verify_tool.cpp
    src/compiler/evaluator_primitives_messaging.cpp
    src/compiler/evaluator_primitives_io.cpp
    src/compiler/evaluator_primitives_agent.cpp
    src/compiler/evaluator_primitives_memory.cpp
    src/compiler/evaluator_primitives_policy.cpp
    src/compiler/evaluator_primitives_eval.cpp
    src/compiler/evaluator_primitives_types.cpp
    src/compiler/evaluator_primitives_diagnostic.cpp
    src/compiler/evaluator_primitives_module.cpp
    src/compiler/evaluator_primitives_file.cpp
    src/compiler/evaluator_primitives_runtime.cpp
    src/compiler/evaluator_primitives_test.cpp
    src/compiler/evaluator_primitives_misc.cpp
    src/compiler/evaluator_primitives_control.cpp
    src/compiler/evaluator_primitives_char.cpp
    src/compiler/evaluator_primitives_mutation.cpp
)
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

# macOS: ucontext.h functions (getcontext/makecontext/swapcontext) are
# deprecated but still functional. Suppress the deprecation warnings
# globally so the fiber scheduler compiles cleanly.
#
# Two macros are needed together:
#  - _XOPEN_SOURCE: <ucontext.h> #errors out without it ("The deprecated
#    ucontext routines require _XOPEN_SOURCE to be defined").
#  - _DARWIN_C_SOURCE: keeps all Darwin/POSIX extensions visible that
#    _XOPEN_SOURCE alone would hide (at_quick_exit in <cstdlib>,
#    strcasecmp in <strings.h>, etc.).
# Using _XOPEN_SOURCE=700 alone breaks cstdlib/linenoise; using
# _DARWIN_C_SOURCE alone doesn't satisfy ucontext.h's #ifdef check.
if(APPLE)
    add_compile_options(-Wno-deprecated-declarations)
    add_compile_definitions(_XOPEN_SOURCE _DARWIN_C_SOURCE)
endif()

add_executable(aura)
set_property(TARGET aura PROPERTY CXX_MODULE_STD ON)

aura_target_cxx_modules(aura WITH_REPL)

target_sources(aura PRIVATE
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    src/core/ast_impl.cpp
    src/core/ast_stability.cpp
    src/core/mutation_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    ${AURA_EVALUATOR_SOURCE_FILES}
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/adt_runtime_impl.cpp  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/hardware_backend_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/sv_ir_impl.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/messaging_bridge_impl.cpp
    # Issue #439 / #451: fiber_bridge.cpp provides the
    # C-linkage shims (aura_evaluator_*,
    # aura_fiber_static_gc_pause_attributed_to_mutation)
    # used by Fiber::check_gc_safepoint + the
    # (query:orchestration-metrics) primitive. The
    # module-side counterparts live in
    # evaluator_fiber_mutation.cpp (compiled into
    # the module's binary); the standalone .cpp
    # version is needed because the module side
    # can't be linked from a module TU directly
    # (the link order prefers the module symbol,
    # so both binaries get the module version).
    src/compiler/fiber_bridge.cpp
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
 


# GCC 16 ICE workaround (Issue #475): ealias pass ICE on memcpy-heavy TUs at
# -O2/-O3 (tree-ssa-structalias / nonnull_arg_p). Track:
#   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=122051
# Remove when GCC >= fixed-version builds cleanly without -O0 overrides.
option(AURA_ENABLE_GCC_ICE_WORKAROUND "Per-file -O0 for cache_impl.cpp + ast.ixx on GCC 16+" ON)
if(AURA_ENABLE_GCC_ICE_WORKAROUND
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16")
    set_source_files_properties(src/compiler/cache_impl.cpp PROPERTIES COMPILE_FLAGS "-O0")
    set_source_files_properties(src/core/ast.ixx PROPERTIES COMPILE_FLAGS "-O0")
endif()

target_include_directories(aura PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
aura_test_compile_options(aura)

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
    # macOS note: the crash handler is already #if'd to Linux-only
    # (execinfo.h + dladdr), and macOS ld uses -export_dynamic instead
    # of -rdynamic. Skip on Apple to avoid an "unknown option" link error.
    if(NOT APPLE)
        target_link_options(aura PRIVATE -rdynamic)
    endif()
else()
    message(STATUS "LLVM JIT: disabled (not found)")
    target_sources(aura PRIVATE src/compiler/aura_jit.cpp src/compiler/aura_jit_runtime.cpp)
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
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
aura_target_cxx_modules(test_ir)
target_sources(test_ir PRIVATE
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    src/core/mutation_impl.cpp
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/thread_pool.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/core/ast_impl.cpp
    src/core/ast_stability.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    ${AURA_EVALUATOR_SOURCE_FILES}
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/adt_runtime_impl.cpp  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/hardware_backend_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/sv_ir_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    # Issue #461: aura_jit_bridge.cpp provides
    # aura_jit_fallback_count_v_read (used by
    # query:jit-fallback-stats in evaluator_primitives_query.cpp).
    # Without this the test_ir link fails with
    # "undefined reference to aura_jit_fallback_count_v_read".
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp)

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
aura_test_compile_options(test_ir)
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
    # Issue #439: fiber_bridge.cpp provides the
    # C-linkage shims (aura_evaluator_request_gc_safepoint,
    # aura_evaluator_wait_for_safepoint,
    # aura_evaluator_mutation_boundary_depth) called
    # by Fiber::check_gc_safepoint (from fiber.cpp).
    # Without this, test_concurrent fails to link with
    # "undefined reference to aura_evaluator_request_gc_safepoint".
    # Note: this is a non-module .cpp (not a module
    # partition) so non-module binaries can include it.
    src/compiler/fiber_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_concurrent PRIVATE src src/compiler)
target_compile_definitions(test_concurrent PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_concurrent PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_concurrent PRIVATE ${llvm_libs})
aura_test_compile_options(test_concurrent)
target_link_libraries(test_concurrent PRIVATE pthread)
add_test(NAME concurrent_basic COMMAND ./test_concurrent)

# Shared object library for test_issue_* binaries.
#
# Why: 40-44 of 55 test_issue_* binaries list the same ~25 .cpp
# source files (ast_impl/type_impl/value_impl + serve/* + parser/*
# + evaluator partition TUs + cache/lowering/ir_executor + type_checker/arity/
# messaging_bridge + ffi_primitives/adt_runtime/lowering_linear_types).
# Pre-Fix #1, each test_issue_* binary compiled these sources
# independently, generating ~1950 MB of duplicated .o files (the
# largest — evaluator partition .o (~12.7 MB) — alone duplicated 45
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
    src/core/ast_stability.cpp
    src/core/contract_handler.cpp
    src/core/mutation_impl.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/parser/lexer_impl.cpp
    src/parser/parser_impl.cpp
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    # Issue #439 / #451: C-linkage shims (non-module)
    # for Fiber::check_gc_safepoint + the
    # (query:orchestration-metrics) primitive.
    src/compiler/fiber_bridge.cpp
    ${AURA_EVALUATOR_SOURCE_FILES}
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/hardware_backend_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/sv_ir_impl.cpp
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
aura_target_cxx_modules(aura_test_objects WITH_REFLECT WITH_TYPE_CONCEPTS)
set_source_files_properties(src/reflect/reflect.ixx PROPERTIES COMPILE_FLAGS "-freflection")
target_include_directories(aura_test_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(aura_test_objects PUBLIC cxx_std_26)
aura_test_compile_options(aura_test_objects)
# Issue #273: enforce FlatAST hot-path contracts in issue tests.
target_compile_options(aura_test_objects PRIVATE -fcontracts)
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
    aura_test_compile_options(${NAME})
    # Fix #1: link the shared test object lib. Compiled once, used
    # by all 55 test_issue_* binaries.
    target_link_libraries(${NAME} PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
    add_test(NAME ${NAME}_verification COMMAND ./${NAME})
endfunction()

# Issue #115 verification tests (criterion #1 multi-core speedup
# + criterion #3 long-running stability). Standalone binary so it
# can be run independently of test_concurrent's other suites.
# Phase 2 pilot-43: converted using early helper. Light serve-only (4 files; helper base + small delta for include and serve sources).


# Issue #184 verification: MutationBoundaryGuard RAII + atomic
# defuse_version_ + Fiber + Mutation Concurrency Safety. Cycle 1
# scope: single-threaded verification that the guard wraps the
# mutation effect correctly, RAII releases the lock on every exit
# path (success, parse error, type error), and defuse_version_ is
# monotonic under typed_mutate. Cycles 2-5 will extend with
# multi-threaded smoke, fiber yield integration, and stress tests.
aura_add_issue_test(test_issue_159_bench)
aura_issue_test_link_llvm_jit(test_issue_159_bench)

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
add_executable(test_gc_evaluator_integration tests/test_gc_evaluator_integration.cpp)
set_property(TARGET test_gc_evaluator_integration PROPERTY CXX_MODULE_STD ON)
# Issue #278 follow-up: pre-existing build fix. Without this
# FILE_SET, the C++ module scanner produces empty modmap files
# (e.g. `ast_impl.cpp.o.modmap` has only `$root .`), causing
# "unknown compiled module interface" errors for every source
# that uses `module aura.core.ast;` etc. Same set as test_ir
# (line ~249). The launcher then symlinks the aura_test_objects
# BMIs into this per-target dir, so the actual compilation
# succeeds.
aura_target_cxx_modules(test_gc_evaluator_integration WITH_REFLECT WITH_TYPE_CONCEPTS)
set_source_files_properties(src/reflect/reflect.ixx PROPERTIES COMPILE_FLAGS "-freflection")
target_include_directories(test_gc_evaluator_integration PRIVATE src)
target_sources(test_gc_evaluator_integration PRIVATE
    src/core/mutation_impl.cpp
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/thread_pool.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/core/ast_impl.cpp
    src/core/ast_stability.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp
    src/compiler/value_impl.cpp
    ${AURA_EVALUATOR_SOURCE_FILES}
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/adt_runtime_impl.cpp  # ADT extraction (FFI pattern, hygiene 3.2)
    src/compiler/lowering_linear_types_impl.cpp
    src/compiler/cache_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/ir_executor_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/hardware_backend_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/shape_profiler.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/sv_ir_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
)
aura_test_compile_options(test_gc_evaluator_integration)
# Issue #278 follow-up: pre-existing build fix. The C++26 contract
# violation handler (`__tu_has_violation`) lives in contract_handler.cpp
# and was missing from the test_gc_evaluator_integration sources. Same
# pattern as test_shape / test_spec_jit / test_jit_metrics.
target_sources(test_gc_evaluator_integration PRIVATE src/core/contract_handler.cpp src/core/contract_stub.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
target_link_libraries(test_gc_evaluator_integration PRIVATE pthread stdc++ stdc++exp aura-reflect)
add_test(NAME gc_evaluator_integration COMMAND ./test_gc_evaluator_integration)

# Link JIT for test_gc_evaluator_integration when LLVM is available
if(LLVM_FOUND)
    target_sources(test_gc_evaluator_integration PRIVATE
        src/compiler/aura_jit.cpp
        src/compiler/aura_jit_runtime.cpp
        src/compiler/aura_jit_bridge.cpp)
    # Issue #278 follow-up: aura_jit_prim_dispatch lives in
    # service.ixx (which IS in our FILE_SET above) — no stub
    # needed, unlike test_concurrent / test_spec_jit which don't
    # compile service.ixx.
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
# Issue #278 follow-up: pre-existing build fix. contract_handler.cpp
# provides `handle_contract_violation` for C++26 contract checks.
target_sources(test_contracts PRIVATE src/core/contract_handler.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
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
# Issue #278 follow-up: pre-existing build fix. shape_profiler.cpp
# uses C++26 contracts whose violation handler is in
# contract_handler.cpp. Without this, the link fails with
# `undefined reference to handle_contract_violation`. Same pattern
# as test_jit_metrics (line ~695).
target_sources(test_shape PRIVATE src/core/contract_handler.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
add_test(NAME test_shape COMMAND test_shape
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Speculative JIT tests (Phase 2, #53) ─────────────────────
add_executable(test_spec_jit tests/test_spec_jit.cpp
    src/compiler/spec_jit_controller.cpp
    src/compiler/shape_profiler.cpp)
target_include_directories(test_spec_jit PRIVATE src)
target_link_libraries(test_spec_jit PRIVATE stdc++)
# Issue #278 follow-up: same contract_handler link fix as test_shape.
target_sources(test_spec_jit PRIVATE src/core/contract_handler.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
# Issue #278 follow-up: spec_jit_controller.cpp calls
# AuraJIT::unhandled_opcode_count_for_function, which lives in
# aura_jit.cpp. Link the JIT sources when LLVM is available
# (same pattern as test_gc_evaluator_integration / test_ir).
if(LLVM_FOUND)
    target_sources(test_spec_jit PRIVATE src/compiler/aura_jit.cpp src/compiler/aura_jit_runtime.cpp)
    # Issue #278 follow-up: aura_jit.cpp references
    # aura_jit_prim_dispatch which lives in service.ixx. The
    # per-test binary can't import service.ixx cleanly (the
    # module is tied to CompilerService), so use the stub.
    # See test_concurrent's identical pattern (~line 350).
    target_sources(test_spec_jit PRIVATE src/compiler/aura_jit_prim_dispatch_stub.cpp)
    target_include_directories(test_spec_jit PRIVATE ${LLVM_INCLUDE_DIRS})
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    target_link_libraries(test_spec_jit PRIVATE ${llvm_libs})
    target_compile_definitions(test_spec_jit PRIVATE AURA_HAVE_LLVM=1)
endif()
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
add_executable(issue_212_bench tests/bench/issue_212_bench.cpp)
set_property(TARGET issue_212_bench PROPERTY CXX_MODULE_STD ON)
# Issue #278 follow-up: contract_handler.cpp link fix.
target_sources(issue_212_bench PRIVATE src/core/contract_handler.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")

# Issue #508 quantitative benchmark: CastOp reduction ratio
# on a synthetic gradual workload. Validates the AC ">=40%
# coercion reduction in gradual workloads" without needing
# a full benchmark suite. Returns 0 iff the AC is met.
add_executable(issue_508_bench tests/bench/issue_508_bench.cpp)
set_property(TARGET issue_508_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(issue_508_bench PRIVATE src)
target_compile_options(issue_508_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(issue_508_bench PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME issue_508_bench COMMAND ./issue_508_bench)

# Issue #307: quantitative benchmark for fine-grained
# incremental type checking + IR re-lower pipeline at EDA
# scale. Builds a 10k-node hardware-like AST, runs full
# infer_flat, applies a 1% mutation, runs infer_flat_partial,
# and validates AC1 (partial/full <10%), AC2 (correctness
# after chained mutations), AC4 (cache hit rate >80%), and
# AC5 (no perf regression on full recompile). AC3 (this
# benchmark file) is the deliverable.
add_executable(issue_307_bench tests/bench/issue_307_bench.cpp)
set_property(TARGET issue_307_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(issue_307_bench PRIVATE src)
target_compile_options(issue_307_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(issue_307_bench PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME issue_307_bench COMMAND ./issue_307_bench)

# Issue #393 follow-up #249: StableNodeRef vs raw NodeId
# 1000-round re-query benchmark. Compares the cost of
# always re-querying (raw path) vs keeping StableNodeRef
# handles and only re-querying on is_valid() == false
# (stable path). AC: stable re-queries < 50% of raw OR
# speedup >= 30%. Standalone TU that imports aura.core.ast
# (no reflection) — same shape as issue_307_bench.
add_executable(stable_ref_bench tests/bench/stable_ref_bench.cpp)
set_property(TARGET stable_ref_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(stable_ref_bench PRIVATE src)
target_compile_options(stable_ref_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(stable_ref_bench PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME stable_ref_bench COMMAND ./stable_ref_bench)

# Issue #398: bench for the new zero-alloc
# for_each_stable_child callback vs the old allocating
# children_stable. Same shape as stable_ref_bench
# (no JIT, just C++ module imports).
add_executable(children_stable_bench tests/bench/children_stable_bench.cpp)
set_property(TARGET children_stable_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(children_stable_bench PRIVATE src)
target_compile_options(children_stable_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(children_stable_bench PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME children_stable_bench COMMAND ./children_stable_bench)

# Issue #399: bench for the mark_dirty no-resize strategy.
# Measures wall-time stability of the mark_dirty hot path
# after add_node's pre-reserve of the dirty side-table
# columns. Pure AST bench, no JIT.
add_executable(mark_dirty_bench tests/bench/mark_dirty_bench.cpp)
set_property(TARGET mark_dirty_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(mark_dirty_bench PRIVATE src)
target_compile_options(mark_dirty_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(mark_dirty_bench PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME mark_dirty_bench COMMAND ./mark_dirty_bench)

# Issue #393 follow-up #3: production-scale variant of
# stable_ref_bench. Same shape as the 1k bench, but
# 10x the defines (1k → 10k nodes total) to verify the
# scoped invalidation win holds at production-shaped
# AST scale (typical EDA / AI agent workspaces).
# 100 rounds × 10k refs should run in ~30s wall time.
add_executable(stable_ref_bench_10k tests/bench/stable_ref_bench_10k.cpp)
set_property(TARGET stable_ref_bench_10k PROPERTY CXX_MODULE_STD ON)
target_include_directories(stable_ref_bench_10k PRIVATE src)
target_compile_options(stable_ref_bench_10k PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(stable_ref_bench_10k PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME stable_ref_bench_10k COMMAND ./stable_ref_bench_10k)

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
aura_test_compile_options(cycle221_pcv_bench)
target_compile_features(cycle221_pcv_bench PUBLIC cxx_std_26)
# Issue #278 follow-up: contract_handler.cpp link fix.
target_sources(cycle221_pcv_bench PRIVATE src/core/contract_handler.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
target_link_libraries(cycle221_pcv_bench PRIVATE stdc++ pthread)
add_test(NAME cycle221_pcv_bench COMMAND ./cycle221_pcv_bench)
target_include_directories(issue_212_bench PRIVATE src)
target_sources(issue_212_bench PRIVATE
    src/core/ast_impl.cpp
    src/core/ast_stability.cpp
    src/core/type_impl.cpp
    src/compiler/value_impl.cpp
    src/compiler/constant_folding_impl.cpp
    src/compiler/type_checker_impl.cpp
    src/compiler/arity_impl.cpp
    src/compiler/sv_ir_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/hardware_backend_impl.cpp
    tests/_stubs/issue_212_runtime_stubs.cpp)
# Match aura_test_objects's compile flags so the prebuilt BMIs
# in module_cache/aura_test_objects.dir/ (produced by the .ixx
# compile) have the same identity the .cpp imports expect.
aura_test_compile_options(issue_212_bench)
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
aura_add_issue_test_reflect_standalone(test_issue_214)

# Issue #215: auto_deserialize for containers + nested struct
# (Issue #178 Cycle 2).
#
# Standalone C++ test (like test_issue_214) because the
# test exercises the reflect.hh header directly with the
# -freflection compile flag.
aura_add_issue_test_reflect_standalone(test_issue_215)

# Issue #216: query:schema + mutate integration
# (Issue #178 Cycle 3).
#
# Standalone C++ test (like test_issue_214/215) because
# the test exercises the reflect_schema.hh header directly
# with the -freflection compile flag. Tests 3-6 shell out
# to the ./build/aura binary to test the Aura-side
# query:schema primitive.
aura_add_issue_test_reflect_standalone(test_issue_216)

# Issue #217: AST/IR migration to reflect_members +
# auto_serialize (Issue #178 Cycle 4). The full scope is
# 1 week of focused work; this test is the Cycle 1 pilot
# that verifies the reflection infrastructure works for
# IR-shaped types (OpcodeInfo, IRInstruction).
aura_add_issue_test_reflect_standalone(test_issue_217)

# Issue #268: production aura::ast::NodeView roundtrip (split TU:
# module side uses real NodeView; reflect side uses nodeview_wire.hh).
add_executable(test_issue_178 tests/test_issue_178.cpp tests/test_issue_178_reflect.cpp)
set_property(TARGET test_issue_178 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_178 PRIVATE src tests)
aura_test_compile_options_reflect_minimal(test_issue_178)
target_compile_features(test_issue_178 PUBLIC cxx_std_26)
target_link_libraries(test_issue_178 PRIVATE aura_test_objects stdc++ pthread)
add_test(NAME test_issue_178_verification COMMAND ./test_issue_178)

# Issue #218: reflection integration (Cycle 5) — query:schema +
# hygienic macro composition (#140 + #248). The 1000+ reflect
# stress loop is in test_issue_178; TSan/ASan:
# tests/run_issue_218_tsan.sh
aura_add_issue_test(test_issue_218)
aura_issue_test_link_llvm_jit(test_issue_218)

# Issue #219: gap-buffer child_data_ for O(1) insert/remove
# in FlatAST. Tests the GapBuffer<T> template defined in
# src/core/gap_buffer.hh. Standalone test (no production
# module imports), so it doesn't trigger the GCC 16.1
# std module + P2996 reflection conflict.
aura_add_issue_test_standalone(test_issue_219)

# Issue #220: per-node children linked list (Issue #179 Cycle 2).
# Replaces FlatAST's std::pmr::vector<NodeId> child_data_ (with
# child_begin_/child_count_ SoA columns) with a per-node
# std::pmr::vector<std::pmr::vector<NodeId>> children_ field.
# The per-node list gives O(1) amortized insert/erase at a
# specific node (just std::pmr::vector ops on that node's list;
# no more shifting of "all subsequent child_begin_ entries").
# Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict).
aura_add_issue_test_standalone(test_issue_220)

# Issue #221: persistent child vectors (Issue #179 Cycle 3,
# slice 1/5). Tests the PersistentChildVector<T> data
# structure defined in src/core/persistent_child_vector.hh.
# Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict).
aura_add_issue_test_standalone(test_issue_221)

# Issue #222: structural mutation concurrency safety (Issue #179
# Cycle 4, slice 1/3). Tests the begin_structural_mutation() /
# end_structural_mutation() RAII guard + set_child / insert_child
# / remove_child routing + try_acquire_reader_lock() reader-side
# API. Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict). Uses a TestFlatAST
# stub that mirrors the production API for testing purposes.
aura_add_issue_test_standalone(test_issue_222)

# Issue #223: closure-bridge epoch counter for lifetime tracking
# (Issue #180 Cycle 1). Tests bridge_epoch_ on ClosureBridgeData +
# IRClosure + the bridge_epoch() / bump_bridge_epoch() / reset()
# epoch tracking on CompilerService. Standalone test (no module
# imports — avoids the GCC 16.1 std module + P2996 reflection
# conflict). Uses a MockEpochTracker that mirrors the production
# mutation_epoch_ / bridge_epoch() semantics.
aura_add_issue_test_standalone(test_issue_223)

# Issue #224 cycle 2: shared_ptr-based bridge ownership.
# Tests verify the new std::shared_ptr<const T> fields
# in ClosureBridgeData and IRClosure. The shared_ptr
# keeps the FlatAST alive as long as the bridge exists
# — even after the lowering arena is reset.
add_executable(test_issue_237 tests/test_issue_237.cpp)
target_include_directories(test_issue_237 PRIVATE tests)
target_link_libraries(test_issue_237 PRIVATE aura_test_objects)
add_test(NAME issue_237_verification COMMAND ./test_issue_237)

# Issue #227: Occurrence Typing narrowing + ADT exhaustiveness
# after Typed Mutations (post #168 epoch fix). Cycle 1 of the
# per-node epoch tracking roadmap. This test captures the
# *current* coarse epoch gate state (Phase 1 of #168) and
# verifies the existing infrastructure works end-to-end.
# Full per-node epoch tracking + recursive free_vars staleness
# check is a separate follow-up (the issue's Recommended Fix #1-2).

# Standalone issue tests that cannot merge into bundles (reflection / extern C).
aura_add_issue_test(test_issue_170)
aura_issue_test_link_llvm_jit_minimal(test_issue_170)
aura_add_issue_test(test_issue_195)
aura_issue_test_link_llvm_jit(test_issue_195)
aura_add_issue_test(test_issue_261)
aura_issue_test_link_llvm_jit(test_issue_261)
aura_add_issue_test(test_issue_262)
aura_issue_test_link_llvm_jit(test_issue_262)
aura_add_issue_test(test_issue_263)
aura_issue_test_link_llvm_jit(test_issue_263)
aura_add_issue_test(test_issue_264)
aura_issue_test_link_llvm_jit_minimal(test_issue_264)
aura_add_issue_test(test_issue_265)
aura_add_issue_test(test_issue_266)
aura_issue_test_link_llvm_jit(test_issue_266)
# Issue #400: sym_id/structural rollback coverage closed loop.
# 7 ACs: mutation-rollback-coverage-stats, sym_id + structural +
# field_offset rollbacks, structural counter, multi-round monotonic
# + regression. Non-duplicative with #266/#369/#553/#213.
aura_add_issue_test(test_mutation_rollback_coverage_400)
aura_issue_test_link_llvm_jit(test_mutation_rollback_coverage_400)
# Issue #415: DirtyReason verification categories +
# mark_dirty_upward propagation metrics synthesis.
aura_add_issue_test(test_dirty_reason_verification_propagation_415)
aura_issue_test_link_llvm_jit(test_dirty_reason_verification_propagation_415)
aura_add_issue_test(test_issue_267)
aura_add_issue_test(test_issue_484_minimal)
aura_add_issue_test(test_issue_290)
aura_add_issue_test(test_issue_291)
aura_add_issue_test(test_issue_292)
aura_add_issue_test(test_issue_293)
aura_add_issue_test(test_issue_296)
aura_add_issue_test(test_issue_294)
aura_add_issue_test(test_issue_295)
aura_add_issue_test(test_issue_297)
aura_add_issue_test(test_issue_298)
aura_issue_test_link_llvm_jit(test_issue_298)
aura_add_issue_test(test_issue_300)
aura_issue_test_link_llvm_jit(test_issue_300)
aura_add_issue_test(test_issue_410)
aura_issue_test_link_llvm_jit(test_issue_410)
aura_add_issue_test(test_issue_411)
aura_issue_test_link_llvm_jit(test_issue_411)
aura_add_issue_test(test_issue_412)
aura_issue_test_link_llvm_jit(test_issue_412)
aura_add_issue_test(test_issue_411_followup_1)
aura_issue_test_link_llvm_jit(test_issue_411_followup_1)
aura_add_issue_test(test_issue_411_followup_2)
aura_issue_test_link_llvm_jit(test_issue_411_followup_2)
aura_add_issue_test(test_issue_411_followup_3)
aura_issue_test_link_llvm_jit(test_issue_411_followup_3)
aura_add_issue_test(test_issue_411_followup_4)
aura_issue_test_link_llvm_jit(test_issue_411_followup_4)
aura_add_issue_test(test_issue_412_followup_1)
aura_issue_test_link_llvm_jit(test_issue_412_followup_1)
aura_add_issue_test(test_issue_413)
aura_issue_test_link_llvm_jit(test_issue_413)
aura_add_issue_test(test_issue_386)
aura_issue_test_link_llvm_jit(test_issue_386)
aura_add_issue_test(test_issue_338)
aura_issue_test_link_llvm_jit(test_issue_338)
aura_add_issue_test(test_issue_433)
aura_issue_test_link_llvm_jit(test_issue_433)
aura_add_issue_test(test_issue_508)
aura_issue_test_link_llvm_jit(test_issue_508)
# Issue #401: invalidate_function BFS determinism + dep_graph_
# integrity (vector-as-stack → std::deque, sort dependents).
# Uses CompilerService::public_invalidate_function +
# public_dep_graph_* hooks. Same link profile as test_issue_225
# / test_issue_226 (jit_tests bundle).
aura_add_issue_test(test_issue_401)
aura_issue_test_link_llvm_jit_tests(test_issue_401)
# Issue #629: zero-overhead coercion path — narrow_evidence DCE
# Rule 6, TypeSpec skip, observability primitive, JIT pipeline.
aura_add_issue_test(test_dead_coercion_elim_narrow_evidence_ir_jit_zerooverhead)
aura_issue_test_link_llvm_jit(test_dead_coercion_elim_narrow_evidence_ir_jit_zerooverhead)
# Issue #538: DCE + IR type info downstream + post-mutate pipeline
aura_add_issue_test(test_dead_coercion_elim)
aura_issue_test_link_llvm_jit(test_dead_coercion_elim)
# Issue #611: CoercionMap deferred + DCE + CastOp type_tag zero-overhead flow
aura_add_issue_test(test_coercion_dead_elim_castop_flow_zerooverhead)
aura_issue_test_link_llvm_jit(test_coercion_dead_elim_castop_flow_zerooverhead)
# Issue #574: Task2 dead-coercion elimination + coercion-elim-stats.
aura_add_issue_test(test_dead_coercion_elimination_task2)
aura_issue_test_link_llvm_jit(test_dead_coercion_elimination_task2)
# Issue #468: DeadCoercionEliminationPass zero-overhead closed loop.
# 8 ACs: DCE >60% gradual reduction, identity elision, narrowing
# preserved, typed mutate + stats monotonic, query primitive +
# regression. Non-duplicative with #433/#538/#574/#629.
aura_add_issue_test(test_dead_coercion_zerooverhead_closed_loop_468)
aura_issue_test_link_llvm_jit(test_dead_coercion_zerooverhead_closed_loop_468)
# Issue #495: Task2 refinement closed-loop integration matrix.
aura_add_issue_test(test_task2_refinement_closed_loop_495)
aura_issue_test_link_llvm_jit(test_task2_refinement_closed_loop_495)
# Issue #384: Bidirectional inference engine — first slice.
# Additive check-mode plumbing in synthesize_flat_lambda (the
# caller hands an expected function type when the arg is a
# lambda, so params + return are constrained directly instead of
# synthesizing fresh vars and unifying post-hoc at the call
# site). No LLVM JIT needed — pure type-checker exercise.
aura_add_issue_test(test_issue_384)
# Issue #389: follow-up #247 — (compile:snapshot) Aura primitive +
# JSON schema docs (scope-limited slice). The test exercises
# CompilerService + marker observability, which depends on the
# JIT-linked service object files.
aura_add_issue_test(test_issue_389)
aura_issue_test_link_llvm_jit(test_issue_389)
# Issue #392: scoped / per-subtree generation bumping for
# precise StableRef invalidation. The test exercises
# FlatAST's per-top-level-Define subtree_gen_ tracking +
# is_valid_subtree() + 3 Aura primitives
# ((compile:subtree-bump), (compile:subtree-generation),
# (compile:subtree-bump-count)). Pure AST-level — no
# compiler service or JIT linkage needed.
aura_add_issue_test(test_issue_392)

# Issue #393: C++ API for explicit (id, gen) pair
# construction + flat-style validity check. Validates the
# two new FlatAST helpers:
#   - make_ref_from_gen(NodeId, uint16_t gen)
#   - is_valid_id_gen(NodeId, uint16_t gen, uint32_t wrap_epoch = 0)
# Pure AST-level — same shape as test_issue_392.
aura_add_issue_test(test_issue_393)
# Issue #393 AC10/AC11 add (query:ref-valid?) Aura primitive
# tests via CompilerService; needs the minimal LLVM JIT link
# (the original 9 ACs are pure FlatAST and don't, but the new
# ones import aura.compiler.evaluator which transitively
# pulls in JIT).
aura_issue_test_link_llvm_jit_minimal(test_issue_393)

# Issue #396: atomic-batch fiber safety + lockless helpers
# (remove-node / insert-child) + new stats fields. Uses
# CompilerService and the messaging bridge (for the
# g_fiber_set_yield_reason_mutation_boundary mock setter in
# AC1). Same link requirements as test_issue_393.
aura_add_issue_test(test_issue_396)
aura_issue_test_link_llvm_jit_minimal(test_issue_396)

# Issue #397: centralize SyntaxMarker::MacroIntroduced
# checks behind FlatAST::is_macro_introduced helper. 8
# inline call sites refactored. Pure CompilerService tests
# (no JIT-specific paths). Same link requirements as
# test_issue_393.
aura_add_issue_test(test_issue_397)
aura_issue_test_link_llvm_jit_minimal(test_issue_397)

# Issue #398: optimize children_stable via zero-alloc
# callback. Pure FlatAST tests + a single CompilerService
# regression test for the Aura primitive. Minimal LLVM
# JIT link (uses aura.compiler.service in AC6).
aura_add_issue_test(test_issue_398)
aura_issue_test_link_llvm_jit_minimal(test_issue_398)

# Issue #399: pre-reserve dirty side-table columns in
# add_node to avoid the resize() in mark_dirty's hot
# path. Pure AST-level tests (no CompilerService).
aura_add_issue_test(test_issue_399)

# Issue #326: Hygienic macros + EDSL integration coverage.
# 8 scenarios extending test_issue_120/121 to integration
# (CompilerService path, not parser-only). See
# tests/test_issue_326.cpp for per-scenario docs.
aura_add_issue_test(test_issue_326)
aura_issue_test_link_llvm_jit(test_issue_326)
# Issue #331: targeted dirty bitmask + query:pattern
# integration under mutate. 8 scenarios extending the
# basic dirty-API coverage from test_issue_138.
aura_add_issue_test(test_issue_331)
aura_issue_test_link_llvm_jit(test_issue_331)
# Issue #330: dedicated unit tests for StructuralMutationGuard
# + ReaderLockGuard. 11 scenarios covering ctor/dtor, move
# semantics, exception unwind, generation monotonicity,
# multiple concurrent readers.
aura_add_issue_test(test_issue_330)
aura_issue_test_link_llvm_jit(test_issue_330)
# Issue #327: incremental compilation e2e + epoch gate.
# 7 scenarios extending #326 + #331 with the
# (compile:relower-strategy) axis — pipeline decides
# 'none / 'incremental / 'full / 'unknown based on
# dirty-block count.
aura_add_issue_test(test_issue_327)
aura_issue_test_link_llvm_jit(test_issue_327)
# Issue #322: dual-path SoA/EnvId invariants + arena
# compaction tests. 8 scenarios covering lookup
# consistency under mutations, compaction safety,
# observability primitives.
aura_add_issue_test(test_issue_322)
aura_issue_test_link_llvm_jit(test_issue_322)
# Issue #324: Arena yield-safe compaction + observability.
# 5 scenarios covering existing compaction surface +
# new ArenaStats.compaction_yield_checks field placeholder.
# The actual fiber-yield hook is deferred (cross-module
# TLS + WorkerContext plumbing).
aura_add_issue_test(test_issue_324)
aura_issue_test_link_llvm_jit(test_issue_324)
# Issue #345: stress testing for StructuralMutationGuard +
# generation_ stability. 5 scenarios covering long-iteration
# loop (configurable via AURA_345_ITERATIONS), concurrent
# readers + mutators, generation wrap-around, perf bench,
# rollback consistency.
aura_add_issue_test(test_issue_345)
aura_issue_test_link_llvm_jit(test_issue_345)
# Issue #306: EDA — Extend Linear Ownership (OwnershipEnv +
# lowering_linear_types) to hardware resources (wires, regs,
# memories, ports) (P0 EDA track). 8 scenarios validating:
# 4 new hw_resource_* counters (wire/reg/mem/double-drive)
# + (query:linear-ownership-stats) primitive + OwnershipEnv
# 4 OwnershipState enum verification + bump helpers +
# 100-iter cycle + 8-thread concurrent + (gc-heap) integration
# + regression for #556 + #555 + #549.
aura_add_issue_test(test_hardware_resource_linear_ownership)
aura_issue_test_link_llvm_jit(test_hardware_resource_linear_ownership)
# Issue #610: linear ownership post-mutate validation + runtime
aura_add_issue_test(test_linear_ownership_post_mutate_validation_runtime)
aura_issue_test_link_llvm_jit(test_linear_ownership_post_mutate_validation_runtime)
# Issue #638: Linear ownership + GuardShape runtime safety post-mutate.
# 7 ACs: linear-ownership-safety-stats, leak violations, moved-linear
# pass, mutate monotonicity, invalidate_shape + GuardShape refresh,
# multi-round matrix + regression. Non-duplicative with #610/#575/#531.
aura_add_issue_test(test_linear_ownership_guardshape_post_mutate)
aura_issue_test_link_llvm_jit(test_linear_ownership_guardshape_post_mutate)
# Issue #598: Runtime linear ownership enforcement + invalidate hook.
# 7 ACs: linear-ownership-runtime-stats, moved-linear pass, invalidate
# deopt/enforcement bumps, leak violations, gc-heap, multi-round monotonic
# + regression. Non-duplicative with #638/#610/#575.
aura_add_issue_test(test_linear_ownership_runtime_enforcement_post_mutate)
aura_issue_test_link_llvm_jit(test_linear_ownership_runtime_enforcement_post_mutate)
# Issue #575: Task2 PerDefUse incremental linear ownership post-mutate.
aura_add_issue_test(test_linear_ownership_incremental_post_mutate_task2)
aura_issue_test_link_llvm_jit(test_linear_ownership_incremental_post_mutate_task2)
# Issue #305: Propagate rich TypeId/TypeScheme from TypeChecker
# to IR for hardware optimization & synthesis (P0 EDA track).
# 9 scenarios validating: 4 new type_propagation counters +
# (query:type-propagation-stats) primitive + TypePropagationPass
# existence + bump helpers + 100-iter cycle + 8-thread
# concurrent + (gc-heap) integration + design doc bit-width
# mention + regression. Non-duplicative with #550
# (query:typed-mutation-stats).
aura_add_issue_test(test_type_propagation_ir_eda)
aura_issue_test_link_llvm_jit(test_type_propagation_ir_eda)
# Issue #304: Aura -> Agentic EDA Roadmap (roadmap/vision,
# NOT a shipping task). 4 scenarios verifying the canonical
# roadmap doc (docs/ROADMAP-HARDWARE.md) is present + has
# all 3 phase sections + Issue #304 reference + decision
# doc cross-references + Status table. Future child issues
# will be filed per Phase 0/1/2 milestone.
aura_add_issue_test(test_hardware_roadmap_acknowledged)
aura_issue_test_link_llvm_jit(test_hardware_roadmap_acknowledged)
# Issue #566: First batch of primitives demotion + full
# validation (P2, capstone of the primitives-demotion epic).
# 6 scenarios validating: primitive count, stdlib module
# consistency (7 modules), INDEX reachability, surface item
# reduction (>=20), decision doc presence, discoverability
# regression. Non-duplicative with #558-#565.
aura_add_issue_test(test_primitives_demotion_batch1)
aura_issue_test_link_llvm_jit(test_primitives_demotion_batch1)
# Issue #565: Stdlib Infrastructure improvements (P1,
# parallel-safe). 5 scenarios validating: stdlib-organization
# spec doc + lib/std/INDEX.aura (5 discovery functions) +
# stdlib module consistency (.aura + .aura-type) + (export)
# coverage + consistency check. Non-duplicative with
# #558-#564.
aura_add_issue_test(test_stdlib_infrastructure)
aura_issue_test_link_llvm_jit(test_stdlib_infrastructure)
# Issue #564: Core Builtins Selective Review (P3, long-term).
# 5 scenarios validating: lib/std/core.aura (new, 10 stdlib
# wrappers) + decision doc with STAY+DEMOTE tables + reusable
# review process + regression on engine core primitives.
# Non-duplicative with #558-#563.
aura_add_issue_test(test_core_builtins_review)
aura_issue_test_link_llvm_jit(test_core_builtins_review)
# Issue #563: New/enhanced std/ast + std/workspace high-level
# modules (P2). 5 scenarios validating: lib/std/ast.aura
# (new, 6 stdlib functions) + lib/std/workspace.aura enhanced
# (+5 stdlib functions) + >= 12 engine primitives wrapped +
# decision doc + regression. Non-duplicative with
# #558-#562.
aura_add_issue_test(test_ast_workspace_modules)
aura_issue_test_link_llvm_jit(test_ast_workspace_modules)
# Issue #562: Audit + demote query: namespace primitives
# (P1). 8 scenarios validating: lib/std/query.aura (high-
# level Agent API with 5 stdlib functions) + 12 demotion
# candidates identified in decision doc (Tier 1 + Tier 2) +
# stdlib wrapper safety + regression for core primitives.
# Non-duplicative with #558 + #559 + #560 + #561.
aura_add_issue_test(test_query_namespace_audit)
aura_issue_test_link_llvm_jit(test_query_namespace_audit)
# Issue #561: Review and demote synthesize: namespace
# primitives (P1). 9 scenarios validating: (query:templates)
# engine-level accessor (replaces demoted primitive) +
# 3 remaining synthesize:* primitives still registerable +
# stdlib wrapper file + decision doc + lint hint removal +
# regression. Non-duplicative with #558 (decision framework)
# + #559 (classification) + #560 (stats unification).
aura_add_issue_test(test_synthesize_namespace_demotion)
aura_issue_test_link_llvm_jit(test_synthesize_namespace_demotion)
# Issue #560: Unify *-stats primitives via std/stats.aura +
# (stats:list) + (stats:count) meta-primitives (P1). 10
# scenarios validating: (stats:list) returns 39+ entries,
# (stats:count) returns integer, std/stats wrappers route
# correctly to existing primitives, prefix filter works,
# (stats:get "all") bundles every counter, regression
# covers #543-#531 + define/eval. Non-duplicative with
# #543-#556 + #531.
aura_add_issue_test(test_stats_module_unification)
aura_issue_test_link_llvm_jit(test_stats_module_unification)
# Issue #531: Live IRClosure/EnvFrame/bridge_epoch +
# linear_ownership_state safety post-invalidate_function +
# GC synergy (Memory-Safety Core, Prompt6 review). 10
# scenarios validating: 4 new closure-env-safety counters
# on CompilerMetrics + 4 CompilerService accessors + bump
# helpers + new (query:closure-env-safety-stats) primitive
# + recursive closure + invalidate cycle + 100-iter cycle +
# (gc-heap) integration + regression. Non-duplicative
# production memory-safety surface.
aura_add_issue_test(test_compiler_closure_env_safety_post_invalidate)
aura_issue_test_link_llvm_jit(test_compiler_closure_env_safety_post_invalidate)
# Issue #556: Concurrency Safety of workspace_mtx_ +
# Fiber Yield + Per-Fiber MutationBoundaryGuard Stack
# in Multi-Agent EDSL Orchestration (P0). 9 scenarios
# validating: 2 new Task1 EDSL concurrency counters
# (unsafe_boundary_attempts + lock_contention_us) + new
# (query:edsl-concurrency-stats) primitive + 8-thread ×
# 100 concurrent mutate+query + Scheduler + 8 fibers ×
# 50 yields + 1000-iter stress + (gc-heap) + bump helper
# test + regression. Non-duplicative refinement of
# #546/#332 focused on Task1 EDSL locking + Guard/fiber
# paths.
aura_add_issue_test(test_edsl_concurrent_fiber_boundary_task1)
aura_issue_test_link_llvm_jit(test_edsl_concurrent_fiber_boundary_task1)
# Issue #555: MutationBoundaryGuard + mark_dirty_upward +
# defuse_version_ + Incremental Type/Occurrence Narrowing
# Post-Mutate for Reliable Typed AI Self-Mod (P0). 9
# scenarios validating: 4 new Task1 typed-mod counters +
# new (query:typed-mutation-stats-task1) primitive +
# guard_dirty_epoch under mutate + 200-iter cycle +
# dirty_propagation under mark_dirty_upward + 8-thread
# concurrent + (gc-heap) + regression. Non-duplicative
# refinement of #550 (Task6 review) focused on Task1
# EDSL mutate + Guard + dirty propagation paths.
aura_add_issue_test(test_incremental_typed_selfmod_dirty_narrowing_task1)
aura_issue_test_link_llvm_jit(test_incremental_typed_selfmod_dirty_narrowing_task1)
# Issue #554: query:pattern + tag_arity_index Incremental
# Maintenance + Full Hygiene (MacroIntroduced) Integration
# for Large-AST AI Multi-Round Self-Evolution (P0). 9
# scenarios validating: 2 new rebuild-timing + delta-hits
# counters + extended (query:pattern-index-stats) primitive
# (4 -> 6 counters) + rebuild records time + dirty_marks
# under mutate + :respect-hygiene regression + large AST +
# 200-iter cycle + 8-thread concurrent + regression.
# Non-duplicative refinement of #528 + #547 focused on
# Task 1 EDSL primitives.
aura_add_issue_test(test_query_pattern_hygiene_index_task1)
aura_issue_test_link_llvm_jit(test_query_pattern_hygiene_index_task1)
# Issue #553: End-to-End Atomic Batch Mutate + Full
# mutation_log_ Rollback + MutationBoundaryGuard + Fiber
# Safety for Reliable AI Multi-Step Self-Evolution (P0).
# 9 scenarios validating: new (query:mutation-log-stats)
# primitive + atomic_batch happy/rollback paths +
# 50-iter stress + 8-thread concurrent + (gc-heap) +
# regression. Non-duplicative refinement of #529 focused
# on Task1 EDSL primitives + Guard/fiber paths.
aura_add_issue_test(test_atomic_batch_rollback_fiber_task1)
aura_issue_test_link_llvm_jit(test_atomic_batch_rollback_fiber_task1)
# Issue #529: Atomic batch + Guard rollback closed-loop observability.
aura_add_issue_test(test_atomic_batch_rollback_closed_loop_529)
aura_issue_test_link_llvm_jit(test_atomic_batch_rollback_closed_loop_529)
# Issue #552: StableNodeRef + generation_ + COW/Fiber/
# Provenance Safety for Reliable Long-Running AI
# Multi-Round Query → Mutate → Eval Loops (P0). 9
# scenarios validating: 5 EDSL stability counters + new
# (query:edsl-stability-stats) primitive + cross_cow
# under mutate:* + validate loop + 500-iter heavy EDSL
# cycle + query:*-stable primitive integration + 8-thread
# concurrent + (gc-heap) + regression. Non-duplicative
# refinement of #549 (Task6 review) focused on Task1 EDSL
# primitives (mutate:* + query:*-stable).
aura_add_issue_test(test_stable_ref_provenance_fiber_cow_task1)
aura_issue_test_link_llvm_jit(test_stable_ref_provenance_fiber_cow_task1)
# Issue #551: Post-Mutation Reflection/auto_validate +
# Guard Impact Snapshot + Schema Validation Hook for
# Code-as-Data Closed Loop (P0). 9 scenarios validating:
# 4 new reflect/snapshot counters + new
# (query:reflect-postmutate-stats) primitive +
# impact_snapshot_count under mutate load + 200-iter
# cycle + schema_validation setters + 8-thread concurrent
# + (gc-heap) integration + regression. Non-duplicative
# with #535/#525/#504/#502/#514 (prior reflect/Guard work).
aura_add_issue_test(test_reflect_postmutate_guard_snapshot)
aura_issue_test_link_llvm_jit(test_reflect_postmutate_guard_snapshot)
# Issue #454: Reflection-to-EDSL bridge closed loop (FlatAST/marker/mutate).
# 8 ACs: reflect-node-members, schema-of-marker, marker-stats, reflect-members,
# mutate under Guard + bridge stats monotonic, mutation-log + regression.
# Non-duplicative with #218/#248/#551 (per-theme reflect tests).
aura_add_issue_test(test_reflect_edsl_bridge_closed_loop_454)
aura_issue_test_link_llvm_jit(test_reflect_edsl_bridge_closed_loop_454)
# Issue #594: Static reflection self-mod validation hook (Task6).
aura_add_issue_test(test_static_reflect_selfmod_validation_task6_594)
aura_issue_test_link_llvm_jit(test_static_reflect_selfmod_validation_task6_594)
# Issue #550: Dirty/Epoch Propagation + Incremental
# solve_delta + Occurrence Re-Narrowing + Pass Short-Circuit
# for reliable typed self-mod (P0). 9 scenarios validating:
# 4 new dirty/narrowing counters + 2 new query primitives
# (typed-mutation-stats + dirty-impact) + narrowing refresh
# under mutate load + 200-iter typed mutate cycle + 8-thread
# concurrent typed mutate + (gc-heap) + regression.
# Non-duplicative with #536/#537/#526/#509/#518 (impl).
aura_add_issue_test(test_incremental_typed_selfmod_dirty_narrowing)
aura_issue_test_link_llvm_jit(test_incremental_typed_selfmod_dirty_narrowing)
# Issue #526: dirty/epoch → type_checker selective recheck +
# Pass short-circuit for typed self-mod soundness.
aura_add_issue_test(test_incremental_type_dirty)
aura_issue_test_link_llvm_jit(test_incremental_type_dirty)
# Issue #536: touched_roots + selective re-narrow + typed-mutation
# stats matrix (dirty/epoch propagation for AI self-mod).
aura_add_issue_test(test_incremental_type_dirty_narrowing)
aura_issue_test_link_llvm_jit(test_incremental_type_dirty_narrowing)
# Issue #608: dependency-tracked solve_delta + occurrence-dirty
# + per-symbol post-mutate incremental type reliability matrix.
aura_add_issue_test(test_typechecker_incremental_dependency_occurrence_dirty_post_mutate)
aura_issue_test_link_llvm_jit(test_typechecker_incremental_dependency_occurrence_dirty_post_mutate)
# Issue #518 P0 Phase 1: Post-Mutation Occurrence Typing
# Re-Narrowing (reanalyze_occurrence_contexts +
# propagate_narrowing_to_uses in infer_flat_partial).
# Per #537 test matrix: 6 ACs for re-narrow path,
# dirty recovery counters, stale-bit clearing, multi-
# round mutate, occurrence-stale-count query, regression.
aura_add_issue_test(test_occurrence_mutate_narrowing)
aura_issue_test_link_llvm_jit(test_occurrence_mutate_narrowing)
# Issue #467: per-node occurrence-dirty + blame chain post-mutate.
aura_add_issue_test(test_occurrence_dirty_blame_post_mutate_467)
aura_issue_test_link_llvm_jit(test_occurrence_dirty_blame_post_mutate_467)
# Issue #639: narrow blame + stale invalidation post-mutate
aura_add_issue_test(test_occurrence_narrow_blame_stale_invalidation_post_mutate)
aura_issue_test_link_llvm_jit(test_occurrence_narrow_blame_stale_invalidation_post_mutate)
aura_add_issue_test(test_bidirectional_check_occurrence_narrow_post_mutate)
aura_issue_test_link_llvm_jit(test_bidirectional_check_occurrence_narrow_post_mutate)
# Issue #609: occurrence typing blame + post-mutate recovery
aura_add_issue_test(test_occurrence_typing_blame_post_mutate_recovery)
aura_issue_test_link_llvm_jit(test_occurrence_typing_blame_post_mutate_recovery)
# Issue #576: Task2 occurrence blame + provenance post-mutate.
aura_add_issue_test(test_occurrence_typing_blame_post_mutate_task2)
aura_issue_test_link_llvm_jit(test_occurrence_typing_blame_post_mutate_task2)
# Issue #549: StableNodeRef + generation_ + mutation_log
# provenance + COW/Fiber safety for long-running
# self-evolution loops (P0). 9 scenarios validating: 4
# new self-evolution-stability counters + new
# (query:self-evolution-stability-stats) primitive +
# validate_stable_ref classification + 200-iter long-
# running mutate + COW loop + 8-thread concurrent +
# (gc-heap) integration + regression. Non-duplicative
# with #540/#527/#497/#457 (prior stable-ref work).
aura_add_issue_test(test_stable_ref_provenance_fiber_cow)
aura_issue_test_link_llvm_jit(test_stable_ref_provenance_fiber_cow)
# Issue #527: StableNodeRef cross-COW/fiber closed-loop observability.
aura_add_issue_test(test_stable_ref_cow_fiber_closed_loop_527)
aura_issue_test_link_llvm_jit(test_stable_ref_cow_fiber_closed_loop_527)
# Issue #548: MutationBoundaryGuard Panic Checkpoint +
# Nested Mutation + Fiber Resume + Auto-Rollback Lifecycle
# stress tests (P0). 11 scenarios complementing #546 with:
# 4 panic-checkpoint lifecycle counters + new
# (query:panic-checkpoint-lifecycle-stats) primitive +
# per-fiber depth probe + 500-iter nested mutate + random
# panic fuzz + (gc-heap) under panic + 8-thread concurrent
# nested mutate + regression. Non-duplicative with #546
# (single-threaded Guard) + #542 (multi-fiber yield).
aura_add_issue_test(test_mutation_boundary_panic_rollback_fiber)
aura_issue_test_link_llvm_jit(test_mutation_boundary_panic_rollback_fiber)
# Issue #547: query:pattern + MacroIntroduced Hygiene Filter
# + Incremental tag_arity_index + DefUseIndex integration
# for large-scale self-evolving SV/RTL verification loops
# (P0). 9 scenarios validating: tag_arity_index dirty hook
# + mark_dirty_upward integration + rebuild clears flag +
# 2 new query:pattern-index-stats / hygiene-stats primitives
# + :respect-hygiene keyword + 200-iter mutate stress +
# regression. Non-duplicative with #541 (query hygiene)
# + #528/#524 (marker propagation) + #514 (meta).
aura_add_issue_test(test_query_pattern_hygiene_index_sv)
aura_issue_test_link_llvm_jit(test_query_pattern_hygiene_index_sv)
# Issue #597: Comprehensive test coverage matrix +
# observability for macro hygiene + static reflection +
# EDSL self-evolution paths (Task6 production review).
# 11 scenarios validating: full macro → query:pattern →
# Guard mutate → reflect validate → dirty/epoch → eval
# → fiber yield + new (query:macro-reflect-self-evo-stats)
# primitive + fuzz/panic injection + 8-thread concurrent
# + 200-iter stress + regression. Non-duplicative with
# #547/#548/#549/#551/#326/#327 (per-theme tests).
aura_add_issue_test(test_macro_reflect_self_evo_full_matrix_task6)
aura_issue_test_link_llvm_jit(test_macro_reflect_self_evo_full_matrix_task6)
# Issue #619: Task6 macro+reflect+self-evo follow-up closed loop.
aura_add_issue_test(test_macro_reflect_self_evo_closed_loop_task6_followup)
aura_issue_test_link_llvm_jit(test_macro_reflect_self_evo_closed_loop_task6_followup)
# Issue #595: EDSL self-evolution marker/dirty/epoch/Guard closed loop.
# 7 ACs: self-evolution-loop-stats synthesis, hygiene filter, Guard dirty/
# epoch propagation, validation_pass, marker+epoch regression, multi-round
# monotonic + regression. Non-duplicative with #597/#619/#547/#525.
aura_add_issue_test(test_edsl_self_evolution_marker_dirty_guard_task6)
aura_issue_test_link_llvm_jit(test_edsl_self_evolution_marker_dirty_guard_task6)
# Issue #514: Task6 Top 3 production-readiness closed loop (meta).
# 8 ACs: ir-hygiene + pattern-marker + production-readiness stats,
# macro→query hygiene → Guard mutate → dirty/type incremental,
# multi-round monotonic + regression. Non-duplicative with #547/
# #550/#551/#597/#619 (per-theme tests).
aura_add_issue_test(test_task6_production_readiness_closed_loop_514)
aura_issue_test_link_llvm_jit(test_task6_production_readiness_closed_loop_514)
# Issue #520: Consolidated Top 5 production roadmap closed loop (meta).
aura_add_issue_test(test_production_roadmap_closed_loop_520)
aura_issue_test_link_llvm_jit(test_production_roadmap_closed_loop_520)
# Issue #517: Consolidated 3-pillar production priority meta tracker.
aura_add_issue_test(test_consolidated_production_priority_517)
aura_issue_test_link_llvm_jit(test_consolidated_production_priority_517)
# Issue #441: Consolidated compiler/runtime production-readiness closed loop
# (meta). 7 ACs: compiler-runtime-production-readiness-stats synthesis,
# P0 #438 fiber + #439 gc-safepoint + #440 EDSL + #437 verify-dirty pillars,
# multi-round monotonic + regression. Non-duplicative with #438/#439/#454/
# #437/#514 (per-theme tests).
aura_add_issue_test(test_compiler_runtime_production_readiness_closed_loop_441)
aura_issue_test_link_llvm_jit(test_compiler_runtime_production_readiness_closed_loop_441)
# Issue #634: Commercial production readiness closed loop (meta).
# 8 ACs: commercial-production-readiness-stats synthesis, stable-ref/gc/
# shape/coercion/verify pillars, multi-round monotonic + regression.
# Non-duplicative with #441/#595/#602/#607 and #620-#633 per-theme tests.
aura_add_issue_test(test_commercial_production_readiness_closed_loop_634)
aura_issue_test_link_llvm_jit(test_commercial_production_readiness_closed_loop_634)
# Issue #635: Macro+reflect+self-evo commercial closed-loop (July 2026).
aura_add_issue_test(test_macro_reflect_self_evo_commercial_closed_loop_635)
aura_issue_test_link_llvm_jit(test_macro_reflect_self_evo_commercial_closed_loop_635)
# Issue #636: EDSL workspace query/mutate commercial closed-loop (July 2026).
aura_add_issue_test(test_edsl_query_mutate_commercial_closed_loop_636)
aura_issue_test_link_llvm_jit(test_edsl_query_mutate_commercial_closed_loop_636)
# Issue #602: Comprehensive fuzz + stress matrix for Prompt6
# memory safety invariants (IRClosure/EnvFrame/bridge_epoch/
# linear/GuardShape) under concurrent fiber + GC + multi-round
# AI mutate. 12 scenarios validating: closure+invalidate path,
# EnvFrame dualpath, mutate type matrix, GC integration, fuzz/
# panic injection, 8-thread concurrent, fiber yield, long stress
# + new (query:prompt6-violation-count) + (query:prompt6-safety-
# score) primitives + regression. Non-duplicative with #531/
# #543/#545/#542/#544/#597 (per-theme tests).
aura_add_issue_test(test_prompt6_full_memory_safety_fuzz_stress)
aura_issue_test_link_llvm_jit(test_prompt6_full_memory_safety_fuzz_stress)
# Issue #607: End-to-end hot-path fuzz + stress matrix for Task4
# high-perf mechanisms (Arena/SoA/Value/Shape/Pass) under AI
# multi-round self-modify + fiber/GC. 13 scenarios validating:
# SoA dirty hook, shape profiler, mutate/eval hot path, arena/GC
# integration, fuzz + 8-thread concurrent + fiber yield + long
# stress + new (query:task4-hotpath-safety-score) +
# (query:task4-cache-locality-win) + (query:task4-mutation-
# stability) primitives + regression. Non-duplicative with
# #602/#597/#547/#531/#254 (per-theme tests).
aura_add_issue_test(test_task4_highperf_full_hotpath_matrix)
aura_issue_test_link_llvm_jit(test_task4_highperf_full_hotpath_matrix)
# Issue #506: IR SoA + dirty-aware Pass hotpath adoption observability.
aura_add_issue_test(test_soa_hotpath_adoption_506)
aura_issue_test_link_llvm_jit(test_soa_hotpath_adoption_506)
# Issue #403: IRInstruction rich metadata interpreter/JIT closed loop.
aura_add_issue_test(test_ir_metadata_interpreter_jit_closed_loop_403)
aura_issue_test_link_llvm_jit(test_ir_metadata_interpreter_jit_closed_loop_403)
# Issue #404: IR SoA Phase 3 block_dirty incremental lowering closed loop.
aura_add_issue_test(test_ir_soa_incremental_closed_loop_404)
aura_issue_test_link_llvm_jit(test_ir_soa_incremental_closed_loop_404)
# Issue #405: Arena auto-compaction orchestration closed loop.
aura_add_issue_test(test_arena_compaction_orchestration_closed_loop_405)
aura_issue_test_link_llvm_jit(test_arena_compaction_orchestration_closed_loop_405)
# Issue #406: Pass Pipeline + Contracts hot-path closed loop.
aura_add_issue_test(test_pass_contracts_hotpath_closed_loop_406)
aura_issue_test_link_llvm_jit(test_pass_contracts_hotpath_closed_loop_406)
# Issue #407: ShapeProfiler burst/deopt storm closed loop.
aura_add_issue_test(test_shape_profiler_burst_closed_loop_407)
aura_issue_test_link_llvm_jit(test_shape_profiler_burst_closed_loop_407)
# Issue #408: EDSL dirty propagation cost closed loop.
aura_add_issue_test(test_dirty_propagation_cost_closed_loop_408)
aura_issue_test_link_llvm_jit(test_dirty_propagation_cost_closed_loop_408)
# Issue #414: generation_/epoch long-running session closed loop.
aura_add_issue_test(test_generation_epoch_closed_loop_414)
aura_issue_test_link_llvm_jit(test_generation_epoch_closed_loop_414)
# Issue #416: AST column compaction closed loop.
aura_add_issue_test(test_ast_column_compaction_closed_loop_416)
aura_issue_test_link_llvm_jit(test_ast_column_compaction_closed_loop_416)
# Issue #417: MutationBoundary cross-TU invariant closed loop.
aura_add_issue_test(test_mutation_boundary_invariant_closed_loop_417)
aura_issue_test_link_llvm_jit(test_mutation_boundary_invariant_closed_loop_417)
# Issue #571: EvalValue v2 encoding dispatch table + hot-path
# Contracts matrix under AI mutate → eval → shape-classify.
# 12 scenarios validating: consteval table, classify roundtrip,
# collision idx 19/31, shape_profiler v2 path, fuzz + 8-thread
# concurrent + fiber yield + long stress + new
# (query:value-dispatch-stats) + regression. Non-duplicative
# with #181 (collision prototype) / #607 (Task4 hot-path).
aura_add_issue_test(test_value_encoding_v2_dispatch_contracts_task4)
aura_issue_test_link_llvm_jit(test_value_encoding_v2_dispatch_contracts_task4)
# Issue #570: ShapeProfiler stability + versioning + deopt/fiber
# hook matrix under AI mutate → eval → shape-classify. 12 scenarios
# validating: stability ratio, version++ on invalidate, invalidate_all
# mutation path, fuzz + 8-thread concurrent + fiber refresh + long
# stress + new (query:shape-stability-stats) + regression.
# Non-duplicative with #571 (value dispatch) / test_shape.cpp.
aura_add_issue_test(test_shape_profiler_stability_deopt_fiber_task4)
aura_issue_test_link_llvm_jit(test_shape_profiler_stability_deopt_fiber_task4)
# Issue #605: ShapeProfiler versioning + JIT cache synergy under
# multi-round AI mutation. Validates jit_shape_miss_count,
# shape_version staleness re-compile, (eval-current :jit)
# semantics post-mutate, concurrent/fiber stress + stats.
# Non-duplicative with #570 (fiber/deopt) / #601 (JIT hotswap).
aura_add_issue_test(test_shapeprofiler_stability_deopt_jit_mutate)
aura_issue_test_link_llvm_jit(test_shapeprofiler_stability_deopt_jit_mutate)
# Issue #546: Arena/pmr::vector + MutationBoundaryGuard
# Panic Checkpoint Lifecycle + Auto-Rollback in Nested
# Mutation + Fiber Resume scenarios (P0). 10 scenarios
# validating the panic-checkpoint + nested Guard matrix
# + auto-rollback + (gc-heap) integration + 1000+ nested
# mutate fuzz + 8-thread concurrent nested mutate +
# regression. Non-duplicative with #534 (impl) and #529
# (atomic batch + rollback).
aura_add_issue_test(test_arena_panic_checkpoint_nested_fiber)
aura_issue_test_link_llvm_jit(test_arena_panic_checkpoint_nested_fiber)
# Issue #545: Scheduler/Worker Work-Stealing + GC Safepoint
# + MutationBoundary Coordination + Eventfd Wakeup stress
# tests (P0). 8 scenarios complementing #542 with: 16+
# workers + 50+ fibers, GC safepoint request/wait/resume
# round-trip, eventfd wakeup validation, affinity +
# load-aware distribution, 10k+ iter combined stress,
# steal_deferred_mutation_boundary_count validation.
# Non-duplicative with #534 (impl) and #542 (8-fiber).
aura_add_issue_test(test_scheduler_gc_safepoint_mutation_coordination)
aura_issue_test_link_llvm_jit(test_scheduler_gc_safepoint_mutation_coordination)
# Issue #544: AOT mangle versioning + reload_aot_module
# stale detection + atomic func_table swap + multi-agent
# hot-update end-to-end tests (P0). 9 scenarios validating
# the C-linkage versioning surface + mangle matrix +
# hot-swap counter + multi-agent isolation + 8-thread
# stress + fallback counter + regression. Non-duplicative
# with #532 (impl) and #323 (4-thread hot-swap).
aura_add_issue_test(test_aot_hotupdate_versioning)
aura_issue_test_link_llvm_jit(test_aot_hotupdate_versioning)
# Issue #543: SoA EnvFrame/EnvId dual-path consistency
# + version stamping + stale detection + GCEnvWalkFn
# comprehensive tests (P0). 11 scenarios validating the
# 4 observability counters + (query:envframe-dualpath-
# stats) primitive + dual-path length consistency +
# multi-thread rebind + GC walk under (gc-heap) +
# regression. Non-duplicative with #242/#286/#322.
aura_add_issue_test(test_soa_envframe_dualpath_consistency)
aura_issue_test_link_llvm_jit(test_soa_envframe_dualpath_consistency)
# Issue #542: Multi-Fiber MutationBoundary + Work-Stealing
# Safety + Starvation Prevention Stress Tests (P0). New
# stress binary complementing #321/#345 with: 8-50
# std::threads concurrent mutate + version-stamp
# monotonicity, GC safepoint coordination under mutation
# pressure, scheduler+fiber yield-reason observability,
# long-running fuzz, regression check. Deliberately
# avoids eval-in-fiber (workspace-lock deadlock pattern
# observed in first iteration).
aura_add_issue_test(test_fiber_mutation_steal_safety)
aura_issue_test_link_llvm_jit(test_fiber_mutation_steal_safety)
# Issue #588: MutationBoundary per-fiber stack sync +
# precise is_at_mutation_boundary_safe + scheduler steal
# defer under inner guard. 8 scenarios: depth probe,
# defer vs allow, resume sync, mixed matrix, stress,
# fiber-migration-stats + regression. Non-duplicative
# with #542 (yield metrics) / #545 (GC safepoint).
aura_add_issue_test(test_runtime_mutation_boundary_steal_safety)
aura_issue_test_link_llvm_jit(test_runtime_mutation_boundary_steal_safety)
# Issue #329: white-box StableNodeRef / generation_ stress
# + dangling-rate gate. Extends #345 with NodeId sampling
# via query:children + mutation invalidation tracking.
# 5 scenarios covering capture, mutate, validate, threshold.
aura_add_issue_test(test_issue_329)
aura_issue_test_link_llvm_jit(test_issue_329)
# Issue #353: follow-up to #241 — panic-checkpoint test gap.
# 6 ACs covering the 3-case matrix + nested + multi-fiber.
aura_add_issue_test(test_issue_353)
aura_issue_test_link_llvm_jit(test_issue_353)
# Issue #364: nested hygienic macros + mutation interactions.
# Extends #326 (basic hygiene + EDSL integration) with
# nested + mutate:rebind + snapshot/rollback + 100-cycle
# stress + macro-defining-macro scenarios.
aura_add_issue_test(test_issue_364)
aura_issue_test_link_llvm_jit(test_issue_364)
# Issue #321: P0 multi-fiber mutation boundary + yield stress.
# Extends #332 (4 threads) with 8 fibers, defuse_version_
# monotonicity, starvation detection, MutationBoundaryGuard
# depth observability.
aura_add_issue_test(test_issue_321)
aura_issue_test_link_llvm_jit(test_issue_321)
# Issue #333: FlatAST serialize/deserialize roundtrip stability.
# 4 scenarios covering generation_ / node_gen_ / dirty_ /
# mutation_log_ preservation across checkpoint/restore.
aura_add_issue_test(test_issue_333)
aura_issue_test_link_llvm_jit(test_issue_333)
# Issue #323: AOT/Bridge hot-update + stale detection.
# 6 scenarios: mangle uniqueness, version suffix,
# __top__ preservation, special-char escaping,
# hot_swap_count observability, multi-threaded stress.
aura_add_issue_test(test_issue_323)
aura_issue_test_link_llvm_jit(test_issue_323)
# Issue #328: self-evolution mutation loop + fuzz enhancement.
# 5 scenarios covering fuzz-driven query:pattern + mutate
# cycles, marker state differential, MBG depth, macro
# mixing, result identity.
aura_add_issue_test(test_issue_328)
aura_issue_test_link_llvm_jit(test_issue_328)
# Issue #377: differential testing IR vs tree-walker.
# 5 scenarios covering arithmetic, comparison, define+rebind,
# eval stability, quoted dispatch. Consistency-based oracle.
aura_add_issue_test(test_issue_377)
aura_issue_test_link_llvm_jit(test_issue_377)
# Issue #357: end-to-end panic-checkpoint smoke (follow-up #242).
# 3 scenarios covering e2e restore on success + failure +
# no-rollback-when-disabled. Complements test_issue_353's
# unit-level matrix.
aura_add_issue_test(test_issue_357)
aura_issue_test_link_llvm_jit(test_issue_357)
# Issue #388: caller-side marker check + Aura opt-in +
# end-to-end test (follow-up #246). 5 scenarios + 3 source
# changes (pass_manager, evaluator.ixx, evaluator_primitives_compile).
aura_add_issue_test(test_issue_388)
aura_issue_test_link_llvm_jit(test_issue_388)
# Issue #371: tag/arity index atomic invalidation under
# fiber concurrent mutate + query:pattern. 6 scenarios:
# single-thread regression + index round-trip + 4-thread
# serialized eval race + 4-thread direct-lock race (the
# real concurrency probe, bypasses eval() serializer) +
# 200-iter mixed-op stress + (query:pattern-index-stats)
# observability. Pairs with TSan for race detection on
# the std::shared_mutex discipline.
aura_add_issue_test(test_issue_371)
aura_issue_test_link_llvm_jit(test_issue_371)
# Issue #372: cross-pool name-based Define lookup. 5 scenarios:
# StringPool::find_by_name round-trip + grow, cross-pool SymId
# mismatch (problem confirmation), cross-pool find_define_by_name
# in parsed Flats, Aura primitive (workspace:find-define) round-trip,
# COW clone + name re-resolve after (workspace:switch). Pairs with
# the existing (workspace:resolve-stable-ref)/StableNodeRef/
# NodeIdRemapTable foundation — name-based is the user-level API.
aura_add_issue_test(test_issue_372)
aura_issue_test_link_llvm_jit(test_issue_372)
# Issue #373: Mutate guards for MacroIntroduced nodes +
# workspace_flat_ marker plumbing. 5 scenarios: query:macro-
# introduced returns cloned-body nodes (eval-current path),
# query:by-marker "MacroIntroduced" returns the same set,
# mutate:rebind on a MacroIntroduced Define returns
# ("hygiene-protected" "...") by default, succeeds after
# (hygiene:set-allow-macro-mutate! #t) global flag, and
# succeeds with per-call :allow-macro? #t kwarg. Pairs with
# the #244 SyntaxMarker query primitives + #190 macro
# marker column.
aura_add_issue_test(test_issue_373)
aura_issue_test_link_llvm_jit(test_issue_373)
# Issue #374: AURA_RUNTIME_DIR CI lookup + install layout
# support. 3 scenarios — env-var override works from a CWD
# with no lib/ (AC1, the CI fix), walk-up from aura binary
# finds runtime.c when env var unset (AC2, regression check
# for the post-#237 v4 walk), and bad AURA_RUNTIME_DIR falls
# through to walk-up / CWD-rel / install-path without
# surfacing "AOT: cannot find runtime.c" (AC3, guards
# against future short-circuits). Pairs with #237 v4 + #360.
aura_add_issue_test(test_issue_374)
aura_issue_test_link_llvm_jit(test_issue_374)
# Issue #375: IRInstruction compact encoding + SoA execution
# view observability. Scope-limited close ships the foundation
# (aura::ir::IRStatsSnapshot + compute_ir_stats() in ir.ixx +
# CompilerService::last_ir_stats() + (compile:ir-stats) Aura
# primitive) so the B/C follow-ups (dual representation +
# interpreter switch → compact view) have baseline numbers to
# design against. 5 ACs cover: zero-on-fresh-service, snapshot
# populates after eval, ratio in valid range, heavier workload
# has more instructions, no regression on existing eval.
aura_add_issue_test(test_issue_375)
aura_issue_test_link_llvm_jit(test_issue_375)
# Issue #376: low-overhead unified apply_closure fast path
# + integrated epoch/stale checks. Scope-limited close
# ships the BASELINE capture on closure-heavy workloads
# (map/filter/foldl) using the existing (closure:stats)
# primitive from #252 — the actual fast-path refactor
# (bypass callback for pure IR closures) is too
# hot-path + epoch-sensitive for this session and
# remains a follow-up. 7 ACs cover: zero counters on
# fresh service, map/filter/foldl baseline, IR path
# counter, bridge-fraction-pct computation, mutation
# stress sanity, Aura primitive consistency, no
# regression on tree-walker dispatch.
aura_add_issue_test(test_issue_376)
aura_issue_test_link_llvm_jit(test_issue_376)
# Issue #301: C++26 std::meta migration audit + foundation.
# 4 scenarios: call-site audit, auto_serialize validation
# on StableNodeRef-like POD, serialize perf baseline,
# <meta> header availability check.
aura_add_issue_test(test_issue_301)
aura_issue_test_link_llvm_jit(test_issue_301)
# Issue #302: Contracts + runtime asserts for memory-safety.
# 4 scenarios covering normal-usage smoke, contract-coverage
# audit, stale-ref detection, arena dtor tracking.
aura_add_issue_test(test_issue_302)
aura_issue_test_link_llvm_jit(test_issue_302)
# Issue #303: SafeStableNodeRef wrappers + provenance tracking.
# 5 scenarios: backward-compat, Provenance accessor, validate
# side-effect, cross-fiber detection, factory equivalence.
aura_add_issue_test(test_issue_303)
aura_issue_test_link_llvm_jit(test_issue_303)
# Issue #332: Concurrent EDSL query-during-mutate stress.
# 4 std::thread workers (2 query + 2 mutate) sharing one
# CompilerService + a Serializing mutex at the eval()
# boundary (CompilerService internal state isn't yet
# lock-free). Each worker calls Fiber::yield(YieldReason
# ::MutationBoundary) after every op to exercise the
# yield-reason API even though we don't drive Fibers
# from a Scheduler. StableNodeRef + defuse_version_
# observability per AC #2.
# Pairs with tests/run_concurrent_edsl_tsan.sh for TSan.
add_executable(test_edsl_concurrent_query_mutate
               tests/test_edsl_concurrent_query_mutate.cpp)
set_property(TARGET test_edsl_concurrent_query_mutate
             PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_edsl_concurrent_query_mutate PRIVATE src)
# Mirror test_concurrent's source list: Fiber + JIT runtime
# symbols are needed for the Fiber API and the CompilerService
# link. Without these we get "undefined reference to
# aura_set_current_fiber_id_fn" and similar at link time.
target_sources(test_edsl_concurrent_query_mutate PRIVATE
    src/serve/fiber.cpp
    src/serve/worker.cpp
    src/serve/scheduler.cpp
    src/serve/gc_coordinator.cpp
    src/exec/execution_adapter.cpp
    src/exec/combinators.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/aura_jit.cpp
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    src/compiler/fiber_bridge.cpp
)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
target_include_directories(test_edsl_concurrent_query_mutate PRIVATE src src/compiler)
target_compile_definitions(test_edsl_concurrent_query_mutate PRIVATE AURA_HAVE_LLVM=1)
target_include_directories(test_edsl_concurrent_query_mutate PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(test_edsl_concurrent_query_mutate PRIVATE ${llvm_libs} aura_test_objects)
aura_test_compile_options(test_edsl_concurrent_query_mutate)
target_link_libraries(test_edsl_concurrent_query_mutate PRIVATE pthread)
add_test(NAME test_edsl_concurrent_query_mutate_verification
         COMMAND ./test_edsl_concurrent_query_mutate)
aura_add_issue_test(test_issue_434)
aura_issue_test_link_llvm_jit(test_issue_434)
aura_add_issue_test(test_issue_390)
aura_issue_test_link_llvm_jit(test_issue_390)
# Issue #394: hash-ref fix for (atomic-batch:stats) + concurrent
# fiber safety during atomic batch + mutate_api.md docs.
aura_add_issue_test(test_issue_394)
aura_issue_test_link_llvm_jit(test_issue_394)
aura_add_issue_test(test_issue_409)
aura_issue_test_link_llvm_jit(test_issue_409)
# Issue #432 / #466: ConstraintSystem solve_delta cross-delta
# soundness (touched-root reverify + conflict metrics).
aura_add_issue_test(test_incremental_type_soundness)
aura_issue_test_link_llvm_jit(test_incremental_type_soundness)
# Issue #509: touched_roots cross-delta + constraint-delta-stats.
aura_add_issue_test(test_constraintsystem_solve_delta_touched_roots_509)
aura_issue_test_link_llvm_jit(test_constraintsystem_solve_delta_touched_roots_509)
# Issue #628: solve_delta clean-constraint conflict detection +
# query:solve-delta-safety-stats matrix.
aura_add_issue_test(test_constraintsystem_solve_delta_clean_conflict_detection)
aura_issue_test_link_llvm_jit(test_constraintsystem_solve_delta_clean_conflict_detection)
# Issue #573: Task2 solve_delta cross-delta + typed-incremental-stats.
aura_add_issue_test(test_constraint_system_solve_delta_cross_delta_task2)
aura_issue_test_link_llvm_jit(test_constraint_system_solve_delta_cross_delta_task2)
aura_add_issue_test(test_issue_341)
aura_issue_test_link_llvm_jit(test_issue_341)
aura_add_issue_test(test_issue_342)
aura_issue_test_link_llvm_jit(test_issue_342)
aura_add_issue_test(test_issue_343)
aura_issue_test_link_llvm_jit(test_issue_343)
aura_add_issue_test(test_issue_383)
aura_issue_test_link_llvm_jit(test_issue_383)
aura_add_issue_test(test_issue_385)
aura_issue_test_link_llvm_jit(test_issue_385)
aura_add_issue_test(test_issue_487)
aura_issue_test_link_llvm_jit(test_issue_487)
# Issue #387: Type Dependency Graph observability
# foundation. Pure unit test (TypeChecker + flat, no
# LLVM/JIT). Tests record + query round-trip directly on
# TypeChecker; the Aura primitive surface is exercised by
# tests/run-tests.sh.
aura_add_issue_test(test_issue_387)
aura_issue_test_link_llvm_jit(test_issue_387)
# Issue #474: Aura unified error type foundation.
# Pure-header test (no LLVM/JIT needed). Tests
# AuraError + AuraResult + monadic ops + mutation
# conversion helpers.
aura_add_issue_test(test_issue_474)
# Issue #501: Aura core Concepts foundation.
# Pure compile-time test (no runtime work needed — all
# assertions are static_assert). Verifies the 5 concepts
# (NodeHandle, ASTContainer, Mutator, ArenaAllocator,
# Queryable) satisfy for real types and reject
# non-matching types.
aura_add_issue_test(test_issue_501)
# Issue #501 Phase 2: apply Mutator concept to mutation
# strategy classes (scope-limited close). Tests the 4
# concrete strategies in aura.core.mutators (Replace /
# Insert / Remove / NoOp) plus the generic apply_mutation
# dispatch template + monadic AuraResult chaining.
aura_add_issue_test(test_issue_501_phase2)
# Issue #501 Phase 3: ASTContainer relaxation + apply
# to query.ixx (scope-limited close). Verifies FlatAST
# now satisfies the relaxed ASTContainer (std::span
# qualifies as std::ranges::range) and the new
# walk_children<> helper in aura.compiler.query.
aura_add_issue_test(test_issue_501_phase3)
# Issue #501 Phase 4: apply Mutator strategy classes to
# mutate:* primitives (mutate:insert-child + mutate:remove-node
# migrated) + (compile:mutator-dispatch-stats) observability
# (follow-up #4). Tests cover C++-level dispatch counters;
# Aura-level end-to-end behavior is verified by test_issue_213
# .. test_issue_223.
aura_add_issue_test(test_issue_501_phase4)
# Issue #501 Concepts extension (Phase C1-C6): validation
# test for the 9 concepts in aura.core.concepts. All checks
# are static_assert — pure compile-time. If the build
# succeeds, all 9 concepts correctly accept the documented
# good shapes and reject the documented bad shapes.
aura_add_issue_test(test_issue_501_concepts)
# Issue #411 fu1 follow-up #1: per-DefUseIndex caller tracking.
# Pure-header test (no LLVM/JIT needed) — registers without
# aura_issue_test_link_llvm_jit.
aura_add_issue_test(test_per_defuse_index)
aura_add_issue_test(test_pcv_heap_use_after_free)
aura_issue_test_link_llvm_jit(test_pcv_heap_use_after_free)
aura_issue_test_link_llvm_jit(test_issue_296)
aura_issue_test_link_llvm_jit(test_issue_297)
aura_issue_test_link_llvm_jit(test_issue_293)
aura_issue_test_link_llvm_jit(test_issue_292)
aura_issue_test_link_llvm_jit(test_issue_291)
aura_issue_test_link_llvm_jit(test_issue_290)
aura_issue_test_link_llvm_jit(test_issue_484_minimal)
aura_issue_test_link_llvm_jit(test_issue_267)
aura_add_issue_test(test_issue_269)
aura_add_issue_test(test_issue_270)
aura_issue_test_link_llvm_jit(test_issue_270)
aura_add_issue_test(test_issue_271)
aura_issue_test_link_llvm_jit(test_issue_271)
aura_add_issue_test(test_issue_272)
aura_issue_test_link_llvm_jit(test_issue_272)
aura_add_issue_test(test_issue_289)
aura_issue_test_link_llvm_jit(test_issue_289)
aura_add_issue_test(test_issue_482)
aura_issue_test_link_llvm_jit(test_issue_482)
aura_add_issue_test_standalone(test_issue_273)
aura_add_issue_test_standalone(test_issue_274)
aura_add_issue_test_standalone(test_issue_275)
aura_add_issue_test(test_issue_276)
aura_issue_test_link_llvm_jit(test_issue_276)
aura_add_issue_test(test_issue_277)
aura_issue_test_link_llvm_jit(test_issue_277)
aura_add_issue_test(test_issue_278)
aura_issue_test_link_llvm_jit(test_issue_278)
aura_add_issue_test(test_issue_279)
aura_issue_test_link_llvm_jit(test_issue_279)
aura_add_issue_test(test_issue_280)
aura_issue_test_link_llvm_jit(test_issue_280)
aura_add_issue_test(test_issue_281)
aura_issue_test_link_llvm_jit(test_issue_281)
aura_add_issue_test(test_issue_282)
aura_issue_test_link_llvm_jit(test_issue_282)
aura_add_issue_test(test_issue_283)
aura_issue_test_link_llvm_jit(test_issue_283)
aura_add_issue_test(test_issue_284)
aura_issue_test_link_llvm_jit(test_issue_284)
aura_add_issue_test(test_issue_436)
aura_issue_test_link_llvm_jit(test_issue_436)
aura_add_issue_test(test_issue_436_phase2)
aura_issue_test_link_llvm_jit(test_issue_436_phase2)
aura_add_issue_test(test_issue_436_phase3)
aura_issue_test_link_llvm_jit(test_issue_436_phase3)
aura_add_issue_test(test_issue_436_phase4)
aura_issue_test_link_llvm_jit(test_issue_436_phase4)
aura_add_issue_test(test_issue_436_phase5)
aura_issue_test_link_llvm_jit(test_issue_436_phase5)
aura_add_issue_test(test_issue_436_phase6)
aura_issue_test_link_llvm_jit(test_issue_436_phase6)
aura_add_issue_test(test_issue_436_phase7)
aura_issue_test_link_llvm_jit(test_issue_436_phase7)
aura_add_issue_test(test_issue_435_phase1)
aura_issue_test_link_llvm_jit(test_issue_435_phase1)
aura_add_issue_test(test_issue_435_phase3)
aura_issue_test_link_llvm_jit(test_issue_435_phase3)
aura_add_issue_test(test_issue_435_phase4)
aura_issue_test_link_llvm_jit(test_issue_435_phase4)
aura_add_issue_test(test_issue_435_phase5)
aura_issue_test_link_llvm_jit(test_issue_435_phase5)
aura_add_issue_test(test_issue_435_phase6)
aura_issue_test_link_llvm_jit(test_issue_435_phase6)
aura_add_issue_test(test_issue_285)
aura_issue_test_link_llvm_jit(test_issue_285)
aura_add_issue_test(test_issue_286)
aura_issue_test_link_llvm_jit(test_issue_286)
aura_add_issue_test(test_issue_287)
aura_issue_test_link_llvm_jit(test_issue_287)
aura_add_issue_test(test_issue_178_cycle3)
aura_issue_test_link_llvm_jit(test_issue_178_cycle3)
aura_add_issue_test(test_issue_453)
aura_issue_test_link_llvm_jit(test_issue_453)
aura_add_issue_test(test_issue_461)
aura_issue_test_link_llvm_jit(test_issue_461)
aura_add_issue_test(test_issue_455)
aura_issue_test_link_llvm_jit(test_issue_455)
aura_add_issue_test(test_issue_458)
aura_issue_test_link_llvm_jit(test_issue_458)
aura_add_issue_test(test_issue_460)
aura_issue_test_link_llvm_jit(test_issue_460)
aura_add_issue_test(test_issue_459)
aura_issue_test_link_llvm_jit(test_issue_459)
aura_add_issue_test(test_issue_437)
aura_issue_test_link_llvm_jit(test_issue_437)
aura_add_issue_test(test_issue_456)
aura_issue_test_link_llvm_jit(test_issue_456)
aura_add_issue_test(test_issue_457)
aura_issue_test_link_llvm_jit(test_issue_457)
aura_add_issue_test(test_issue_469)
aura_issue_test_link_llvm_jit(test_issue_469)
aura_add_issue_test(test_issue_448)
aura_issue_test_link_llvm_jit(test_issue_448)
aura_add_issue_test(test_issue_426)
aura_issue_test_link_llvm_jit(test_issue_426)
aura_add_issue_test(test_issue_391)
aura_issue_test_link_llvm_jit(test_issue_391)
aura_add_issue_test(test_issue_447)
aura_issue_test_link_llvm_jit(test_issue_447)
aura_add_issue_test(test_issue_438)
aura_issue_test_link_llvm_jit(test_issue_438)
aura_add_issue_test(test_issue_439)
aura_issue_test_link_llvm_jit(test_issue_439)
aura_add_issue_test(test_issue_443)
aura_issue_test_link_llvm_jit(test_issue_443)
aura_add_issue_test(test_issue_451)
aura_issue_test_link_llvm_jit(test_issue_451)
aura_add_issue_test(test_issue_478)
aura_issue_test_link_llvm_jit(test_issue_478)
# Issue #583: Primitives registry + core list/math consistency (stdlib).
aura_add_issue_test(test_primitives_registry_core_consistency_583)
# Issue #480: Self-describing primitive metadata + Agent extension kit.
aura_add_issue_test(test_primitive_meta_self_describing_closed_loop_480)
aura_issue_test_link_llvm_jit(test_primitive_meta_self_describing_closed_loop_480)
aura_issue_test_link_llvm_jit(test_primitives_registry_core_consistency_583)
# Issue #308: hardware BitVector type + 4 Aura primitives
# (compile:hw-bitvec-register / -width / -signed? /
# -compatible?) + C++ TypeRegistry::register_hw_bitvec +
# hw_bitvec_of side-table. 5-AC test validates hw enum
# exhaustiveness (AC1, re-validates #260), BitVector
# width-mismatch diagnostic (AC2, the new primitive path),
# nested match exhaustiveness (AC3, re-validates #260), the
# hw module pattern (AC4 — covers AC1+AC2+AC3 together).
aura_add_issue_test(test_issue_308)
aura_issue_test_link_llvm_jit(test_issue_308)
# Issue #612: ADT/match exhaustiveness post-mutation reliability.
aura_add_issue_test(test_adt_match_exhaust_post_mutate_reliability)
aura_issue_test_link_llvm_jit(test_adt_match_exhaust_post_mutate_reliability)
# Issue #577: Task2 ADT exhaustiveness + incremental match matrix.
aura_add_issue_test(test_adt_match_exhaustiveness_incremental_task2)
aura_issue_test_link_llvm_jit(test_adt_match_exhaustiveness_incremental_task2)
# Issue #309: hardware lossy-coercion diagnostics + 2 new
# Aura primitives (compile:hw-coercion-lossy? +
# -warning). Extends the #308 BitVector foundation with
# narrowing-detection (from_w > to_w) + human-readable
# warning string. 5-AC test covers BlameInfo intact (AC1,
# re-validates #342), lossy? primitive (AC2), Gradual
# Guarantee for hw mixed (AC3, re-validates existing),
# diagnostic format (AC4 — Wn + signedness + bit count).
aura_add_issue_test(test_issue_309)
aura_issue_test_link_llvm_jit(test_issue_309)
# Issue #310: minimal SV structural NodeTags (Interface +
# Modport) added to NodeTag enum + kNodeMeta + tag_dispatch
# mirror. Pure AST-level foundation; no builder/query/emit
# added in this PR (follow-up issues will wire the
# constructors + side-table population + lowering hooks).
aura_add_issue_test(test_issue_310)
aura_issue_test_link_llvm_jit(test_issue_310)
# Issue #311: add_interface + add_modport builders shipped
# on top of #310's NodeTag foundation. Uses
# PersistentChildVector (mirrors add_export shape) for
# Interface body + param_data_ side-table (mirrors
# add_define_module shape) for Modport ports. 7 tests
# covering 5 ACs: NodeId validity, PCV storage, parent_
# links, COW semantics, end-to-end SV interface pattern.
aura_add_issue_test(test_issue_311)
aura_issue_test_link_llvm_jit(test_issue_311)
# Issue #312: query:node-type + query:where flow through
# the new Interface / Modport kNodeMeta entries added in
# #310 — no code change in evaluator_impl is needed
# (both primitives iterate kNodeMeta). Tests cover both
# the Aura query path + the C++-side filter equivalent
# (Direct FlatAST scan via the new builders from #311).
aura_add_issue_test(test_issue_312)
aura_issue_test_link_llvm_jit(test_issue_312)
# Issue #313: kVerificationDirty constant +
# mark_dirty_verification(_upward) helpers +
# is_verification_dirty(_for) /
# clear_verification_dirty(_for) accessors. The constant
# is a *named identifier* (not an actual dirty-byte bit)
# because the existing uint8_t dirty bitmask is already
# full (all 8 bits used by other DirtyReason values).
# The actual per-reason state lives in the orthogonal
# verification_dirty_ side-table (#469), which the
# helpers populate. 6 tests + 3 ACs covered.
aura_add_issue_test(test_issue_313)
aura_issue_test_link_llvm_jit(test_issue_313)
# Issue #314: structural mutations on the new SV tags
# (Interface / Modport) automatically route through the
# existing safety path: set_child/insert_child/remove_child
# → add_mutation_child_op → add_mutation_with_rollback
# → mutation_log_ + mark_dirty_upward + StructuralMutationGuard
# bumps generation_. No code change in evaluator_impl is
# needed — the existing path is tag-agnostic.
aura_add_issue_test(test_issue_314)
aura_issue_test_link_llvm_jit(test_issue_314)
# Issue #315: SVInterfaceIR + SVModportIR (SymId-based
# structured IR) shipped in src/compiler/sv_ir.ixx +
# sv_ir_impl.cpp. Parallel to the std::string-based #435
# InterfaceIR / ModportIR (which were the list-emit-
# friendly variant). Includes constructors + AST →
# IR mapping helper (map_interface_node_to_ir). 3 ACs:
# constructible, AST mapping, compiles.
aura_add_issue_test(test_issue_315)
aura_issue_test_link_llvm_jit(test_issue_315)
# Issue #316: emit_sv_interface + emit_sv_modport (SymId-
# based SV emit) shipped in src/compiler/sv_ir.ixx +
# sv_ir_impl.cpp. Parallel to the std::string-based #435
# emit_interface + emit_modport (the list-emit-friendly
# variant). 3 ACs: emits valid SV syntax with modport
# list, supports balanced-paren lint, output passes basic
# grammar checks.
aura_add_issue_test(test_issue_316)
aura_issue_test_link_llvm_jit(test_issue_316)
# Issue #317: DefUseIndex extended with SV Interface +
# Modport as scope-creators. Each Interface/Modport now
# creates its own scope with defs (interface name for
# the Interface; modport name + port-list SymIds for the
# Modport). Cross-module Variable references to the
# interface name appear as uses in the parent's scope.
# 3 ACs + a perf-bound check.
aura_add_issue_test(test_issue_317)
aura_issue_test_link_llvm_jit(test_issue_317)
# Issue #318: (verify:coverage-holes) +
# (verify:suggest-constraint-refine). Two Aura primitives
# that close the verification → AST loop by returning
# NodeIds of nodes marked kCoverageFeedbackDirty. The
# AI agent / editor tool can pipe the list into
# (mutate:query-and-replace ...) + (ast:snapshot /
# ast:rollback) for safe refinement. Composes with
# (verify:parse-coverage-feedback) from #469 and
# (mark_dirty_verification) from #313.
# 3 ACs + perf-bound (<50ms on 200-node workspace).
aura_add_issue_test(test_issue_318)
aura_issue_test_link_llvm_jit(test_issue_318)
# Issue #319: pilot test for AI-driven SV constraint
# refinement closed loop. Composes the SV EDSL work
# (#310/#311/#315/#316/#317) with the verification
# feedback primitives (#318/#469) and the mutation
# pipeline (#110/#270) to demonstrate the full
# "verification feedback drives self-evolution" loop
# end-to-end. The "constraint" is a regular Aura
# (define constraint-i body) form; "refinement" is
# mutating the body in response to coverage feedback.
# 4 ACs: closed loop runs, multi-round refinement,
# rollback works, light CI compat (Aura primitives
# only).
aura_add_issue_test(test_issue_319)
aura_issue_test_link_llvm_jit(test_issue_319)
# Issue #320: per-node epoch tracking for infer_flat
# (P1 follow-up to #227). Ships the foundation: the
# last_seen_epoch_ SoA column on FlatAST + the
# mark_dirty_for_reinfer helper + the mark_dirty
# bump path + the reset_node_slot / push_back /
# copy / move wiring. The synthesize_flat cache
# check wiring (the actual per-node invalidation)
# is deferred as a follow-up — the column is plumbed
# but the cache still uses the coarse whole-cache
# gate from #168. 6 ACs covered: column plumbed,
# mark_dirty bumps, mark_dirty_for_reinfer stamps
# supplied epoch, mark_dirty_upward propagates,
# typed_mutate bumps end-to-end, #227 regression
# (coarse gate still works).
aura_add_issue_test(test_issue_320)
aura_issue_test_link_llvm_jit(test_issue_320)
# Issue #325: ci/test — improve sanitizer coverage +
# known-failures management + test reporting. Ships:
#   - tests/run_sanitizer_matrix.sh (single entry
#     point for TSan/ASan/UBSan + coverage on 4
#     representative test binaries)
#   - docs/sanitizers.md (usage doc)
#   - tests/test_issue_325.cpp (smoke test that the
#     script + doc exist + are valid)
# 4 ACs: 3-sanitizer matrix + coverage toolchain +
# known-failures handling + docs.
aura_add_issue_test(test_issue_325)
aura_issue_test_link_llvm_jit(test_issue_325)
# Issue #335: [Perf] enhance ArenaGroup auto_compact
# heuristics + tight integration with evaluator
# memory_pressure. Ships the ArenaGroup API:
# should_auto_compact(name) probe + adaptive_compact
# (single module) + adaptive_compact_all() + 3 new
# Aura primitives (arena:adaptive-compact,
# arena:should-auto-compact?, arena:adaptive-stats).
# The EMA-based threshold adjustment (alpha=0.3,
# gain=0.0001 per byte) lowers the effective trigger
# threshold when recent compactions were productive
# (shift next trigger sooner) and raises it toward
# base when they were wasteful (shift later).
# 5 ACs: probe works, adaptive_compact reclaims +
# updates EMA, EMA lowers threshold, observability
# counters, perf-bound (<100ms for 100 calls).
aura_add_issue_test(test_issue_335)
aura_issue_test_link_llvm_jit(test_issue_335)
# Issue #336: [Perf] fine-grained dirty propagation
# using multi-reason bitmask. Ships:
#   - mark_dirty_upward_fast() with early-exit
#     (fixed-point check on parent for the target
#     reason bits; BFS terminates at a parent
#     that's already dirty for those reasons)
#   - clear_dirty_for_subtree() range variant
#   - (compile:mark-dirty-upward-fast) Aura
#     primitive exposing the fast path
#   - (compile:ast-ops-stats) extended with
#     dirty-upward-fast-fixed-point-hits counter
# 5 ACs: early-exit fires, clear_dirty_for_subtree
# walks children, the Aura primitive works,
# ast-ops-stats returns the new field, end-to-end
# fast path via Aura.
aura_add_issue_test(test_issue_336)
aura_issue_test_link_llvm_jit(test_issue_336)
# Issue #337: [Perf/C++23+] adopt std::flat_map /
# std::span views in ShapeProfiler, caches, SoA
# query paths. Ships:
#   - ShapeProfiler.profiles_ is now
#     std::flat_map<FnKey, FnProfile> (C++23)
#     (replaces std::unordered_map for better
#     cache locality in the lookup-heavy
#     shape-stability check)
#   - FlatAST gets 5 new std::span view accessors
#     (dirty_view, ppa_dirty_view,
#     last_seen_epoch_view, verify_dirty_view,
#     verification_dirty_view) — the foundation
#     for ranges::views::zip pass-level adoption
# 5 ACs: flat_map used, dirty_view non-empty,
# last_seen_epoch_view size matches, tracked_fns
# no API regression, end-to-end views via
# CompilerService.
aura_add_issue_test(test_issue_337)
aura_issue_test_link_llvm_jit(test_issue_337)
# Issue #339: mutation-aware validation + smart
# invalidation for occurrence-narrowing contexts.
# Ships the foundation: per-node occ_stale_ SoA
# column on FlatAST (uint8_t per node, mirrors
# the verify_dirty_ / verification_dirty_ /
# macro_dirty_ side-table pattern from #437 +
# #469 + #290) + 3 new Aura primitives
# (query:occurrence-stale?,
# query:occurrence-stale-count,
# query:mark-occurrence-stale) + C++ accessors
# (is_occurrence_stale, mark_occurrence_stale,
# clear_occurrence_stale, occurrence_stale_count).
# The validate_occurrence_narrowing() function
# + post_mutation_invariant_check integration
# are filed as follow-ups (deeper type system
# refactor: BlameInfo + subtyping + env_type
# lookup).
# 5 ACs: column plumbed, mark + read, clear,
# count, end-to-end via Aura.
aura_add_issue_test(test_issue_339)
aura_issue_test_link_llvm_jit(test_issue_339)
# Issue #340: cache OccurrenceInfoFlat results for
# faster incremental re-inference in if-branches.
# Ships the (compile:occ-cache-stats) Aura
# primitive that closes the observability gap for
# the pre-existing predicate_memo_ cache (#281).
# The memo infrastructure (per-cond-NodeId cache
# with epoch-based invalidation) is already in
# place from #281; this PR adds the Aura-level
# stats surface. 3 ACs: 3-tuple shape, memo
# infrastructure exists, end-to-end callable.
aura_add_issue_test(test_issue_340)
aura_issue_test_link_llvm_jit(test_issue_340)
# Issue #344: refine targeted dirty bitmask for
# precise incremental control + EDSL integration.
# Ships:
#   - (compile:dirty-reason-counts) Aura primitive
#     returning the 8-tuple of per-DirtyReason counts
#     (general / constraint / occurrence / ownership
#     / coercion / struct / defuse / ppa-hint)
#   - (query:dirty-nodes :reason "X") Aura
#     primitive returning a pair-list of NodeIds
#     dirty for the given reason
# 5 ACs: 8-tuple shape, dirty-nodes primitive,
# mark shows up, clear removes, end-to-end mark+query.
aura_add_issue_test(test_issue_344)
aura_issue_test_link_llvm_jit(test_issue_344)
# Issue #346: strengthen mutation audit log +
# provenance + debugging. Ships:
#   - (query:mutation-log [n]) Aura primitive
#     returning a pair-list of the most recent n
#     mutations (chronological, oldest first)
#   - (query:mutations-since <id>) Aura primitive
#     returning mutations with id > the given id
#   - mutation_log_view() span accessor on
#     FlatAST (the std::span pattern from #337
#     + #344)
#   - push_pair() public helper on Evaluator
#     (needed because the pairs_ vector is
#     private; pairs the existing push_string_heap)
# 4 ACs: primitive returns pair-list, N-arg
# works, mutations-since filters, end-to-end in
# mutate context.
aura_add_issue_test(test_issue_346)
aura_issue_test_link_llvm_jit(test_issue_346)
# Issue #347: [Docs] StableNodeRef, generation_ and
# mutation safety best practices guide. Ships:
#   - docs/design/core/stable_ref_best_practices.md
#     (comprehensive best-practices doc covering
#     when to use StableNodeRef vs raw NodeId,
#     safe patterns for multi-round query-mutate-
#     eval loops, stale-ref handling, workspace
#     layering + COW, fiber concurrency, 3 code
#     examples, 6 anti-patterns)
#   - docs/architecture.md link to the new doc
#     (in the "Agent 编排" section)
#   - tests/test_issue_347.cpp verifying the doc
#     exists + has the required sections + cross-
#     references resolve to real files
# 4 ACs: doc exists with sections, examples +
# anti-patterns, linked from main docs, cross-
# references resolve.
aura_add_issue_test(test_issue_347)
aura_issue_test_link_llvm_jit(test_issue_347)
# Issue #348: auto-wire mark_narrowing_dirty into
# mutate:rebind when target is if-predicate. Ships:
#   - auto_wire_k_occurrence_dirty_for_subtree
#     helper in evaluator_primitives_mutate.cpp
#     (iterative DFS walker that marks every
#     IfExpr in the new value's subtree via the
#     set_occurrence_dirty_fn_ hook)
#   - mutate:rebind calls the helper after
#     set_child(old_define, 0, new_value), so the
#     if-nodes in the new function body are
#     auto-marked kOccurrenceDirty
#   - mutate:replace-value + mutate:replace-type
#     auto-wiring is filed as a follow-up
# 4 ACs: helper walks subtree, rebind auto-marks
# new if-nodes, walker only marks IfExpr (not
# other node types), end-to-end via CompilerService.
aura_add_issue_test(test_issue_348)
aura_issue_test_link_llvm_jit(test_issue_348)
# Issue #349: [Follow-up #240-2] blame tracking
# in structural mutation primitives. Ships:
#   - (query:last-mutation-blame) Aura primitive
#     returning a 2-tuple (operator_name . summary)
#     for the most-recent mutation
#   - The C++ side blame plumbing (from #260) is
#     already in place: post_mutation_invariant_check
#     populates source_mutation_id + blame on
#     every emitted note from the MutationRecord.
#     The new primitive surfaces this to Aura.
# 4 ACs: primitive returns value, pair car is
# operator_name, post-mutation invariant populates
# blame, end-to-end blame tracks the latest.
aura_add_issue_test(test_issue_349)
aura_issue_test_link_llvm_jit(test_issue_349)
# Issue #350: [Follow-up #240-3 / #227-3] match exhaustiveness re-eval post-mutation.
# Ships (query:match-exhaustiveness-notes) Aura primitive. C++ side already in place from #260.
aura_add_issue_test(test_issue_350)
aura_issue_test_link_llvm_jit(test_issue_350)
# Issue #351: [Follow-up #240-4] match-pattern support
# in find_occurrence_contexts. Ships the per-node
# occurrence-dirty scoping on the match pattern
# walker's conservative fallback (the "Subject type
# unknown" branch in recheck_match_exhaustiveness_in_dirty_scope):
#   - has kOccurrenceDirty: emit the precise note
#     ("invalidated-match-narrowing")
#   - has kGeneralDirty only: emit the conservative
#     note ("invalidated-match-pattern")
#   - clean: skip the node entirely
# Mirrors the #240 fix on find_occurrence_contexts
# (IfExpr path). 4 ACs: C++ side wired, kOccurrenceDirty
# emits precise, kGeneralDirty emits conservative,
# end-to-end via CompilerService.
aura_add_issue_test(test_issue_351)
aura_issue_test_link_llvm_jit(test_issue_351)
# Issue #354: [Follow-up #241-3] fiber yield
# assertion — no held mutation lock. Ships:
#   - std::atomic<bool> mutation_boundary_held_
#     on Evaluator (set/cleared by outermost
#     MutationBoundaryGuard ctor/dtor)
#   - mutation_boundary_held() public accessor
#   - g_mutation_boundary_held bridge function
#     in messaging_bridge.h (the C-linkage shim
#     for the fiber code)
#   - mutation_boundary_held_trampoline
#     registered at static init
#   - Fiber::yield() check: asserts in debug
#     builds, logs warning + continues in release
# 4 ACs: flag false outside Guard, flag true
# inside Guard, flag cleared after Guard, end-
# to-end via mutate:rebind primitive.
aura_add_issue_test(test_issue_354)
aura_issue_test_link_llvm_jit(test_issue_354)
aura_add_issue_test(test_followups)
aura_issue_test_link_llvm_jit(test_followups)
target_include_directories(test_issue_264 PRIVATE src/serve tests)
target_sources(test_issue_264 PRIVATE
    src/serve/fiber.cpp
    src/serve/scheduler.cpp
    src/serve/worker.cpp
    src/serve/thread_pool.cpp
)

# Issue test bundles — one link per profile (see scripts/gen_issue_bundles.py)
aura_add_issue_bundle(light)
aura_add_issue_bundle(jit)
aura_add_issue_bundle(jit_minimal)
aura_add_issue_bundle(jit_contract)
aura_add_issue_bundle(jit_tests)
aura_add_issue_bundle(fiber)

add_custom_target(all_test_issue_targets ALL)
add_dependencies(all_test_issue_targets test_issues_light)
add_dependencies(all_test_issue_targets test_issues_jit)
add_dependencies(all_test_issue_targets test_issues_jit_minimal)
add_dependencies(all_test_issue_targets test_issues_jit_contract)
add_dependencies(all_test_issue_targets test_issues_jit_tests)
add_dependencies(all_test_issue_targets test_issues_fiber)
add_dependencies(all_test_issue_targets test_issue_170)

add_dependencies(all_test_issue_targets test_issue_195)
add_dependencies(all_test_issue_targets test_issue_159_bench)

add_dependencies(all_test_issue_targets test_issue_214)
add_dependencies(all_test_issue_targets test_issue_215)
add_dependencies(all_test_issue_targets test_issue_216)
add_dependencies(all_test_issue_targets test_issue_217)
add_dependencies(all_test_issue_targets test_issue_178)
add_dependencies(all_test_issue_targets test_issue_218)
add_dependencies(all_test_issue_targets test_issue_219)
add_dependencies(all_test_issue_targets test_issue_220)
add_dependencies(all_test_issue_targets test_issue_221)
add_dependencies(all_test_issue_targets test_issue_222)
add_dependencies(all_test_issue_targets test_issue_223)
add_dependencies(all_test_issue_targets test_issue_237)


# ── Issue #382: file size lint ──────────────────────────────────
#
# Custom target that runs scripts/check_file_size.py to enforce
# the .ixx file size policy (see scripts/file_size_policy.md).
#
# Run on demand:
#   cmake --build build --target lint_file_size
#
# Exits 1 if any .ixx file is over the blocker threshold (2000
# lines by default). Exits 0 if everything is under the warning
# threshold (800 lines by default).
#
# Not added to the default `all` target — it's a manual lint,
# not an auto-build step. CI can invoke it via:
#   cmake --build build --target lint_file_size
# and check the exit code.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(lint_file_size
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_file_size.py
                --src ${CMAKE_SOURCE_DIR}/src
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Linting .ixx file sizes (Issue #382 policy)"
        VERBATIM)
endif()
