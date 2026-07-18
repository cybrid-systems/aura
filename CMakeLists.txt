cmake_minimum_required(VERSION 4.3)
# CMake 4.4+ gate UUID (was 451f2fe2… on 4.3).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
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
    src/compiler/evaluator_primitives_persist.cpp
    src/compiler/evaluator_primitives_ast.cpp
    src/compiler/evaluator_primitives_compile.cpp
    src/compiler/evaluator_primitives_compile_00.cpp
    src/compiler/evaluator_primitives_compile_01.cpp
    src/compiler/evaluator_primitives_compile_02.cpp
    src/compiler/evaluator_primitives_compile_03.cpp
    src/compiler/evaluator_primitives_compile_04.cpp
    src/compiler/evaluator_primitives_compile_05.cpp
    src/compiler/evaluator_primitives_compile_06.cpp
    src/compiler/evaluator_primitives_compile_07.cpp
    src/compiler/evaluator_primitives_observability.cpp
    src/compiler/evaluator_primitives_stdlib_review.cpp
    src/compiler/evaluator_primitives_obs_eval_00.cpp
    src/compiler/evaluator_primitives_obs_eval_01.cpp
    src/compiler/evaluator_primitives_obs_eval_02.cpp
    src/compiler/evaluator_primitives_obs_eval_03.cpp
    src/compiler/evaluator_primitives_obs_eval_04.cpp
    src/compiler/evaluator_primitives_obs_eval_05.cpp
    src/compiler/evaluator_primitives_obs_eval_06.cpp
    src/compiler/evaluator_primitives_obs_eval_07.cpp
    src/compiler/evaluator_primitives_obs_eval_08.cpp
    src/compiler/evaluator_primitives_obs_eval_09.cpp
    src/compiler/evaluator_primitives_obs_eval_10.cpp
    src/compiler/evaluator_primitives_obs_eval_11.cpp
    src/compiler/evaluator_primitives_obs_eval_12.cpp
    src/compiler/evaluator_primitives_obs_eval_13.cpp
    src/compiler/evaluator_primitives_obs_jit_00.cpp
    src/compiler/evaluator_primitives_obs_jit_01.cpp
    src/compiler/evaluator_primitives_obs_jit_02.cpp
    src/compiler/evaluator_primitives_obs_jit_03.cpp
    src/compiler/evaluator_primitives_obs_jit_04.cpp
    src/compiler/evaluator_primitives_obs_jit_05.cpp
    src/compiler/evaluator_primitives_obs_jit_06.cpp
    src/compiler/evaluator_primitives_obs_jit_07.cpp
    src/compiler/evaluator_primitives_obs_jit_08.cpp
    src/compiler/evaluator_primitives_obs_jit_09.cpp
    src/compiler/evaluator_primitives_obs_jit_10.cpp
    src/compiler/evaluator_primitives_obs_jit_11.cpp
    src/compiler/evaluator_primitives_obs_jit_12.cpp
    src/compiler/evaluator_primitives_obs_jit_13.cpp
    src/compiler/evaluator_primitives_obs_jit_14.cpp
    src/compiler/verify_tool.cpp
    src/compiler/evaluator_primitives_eda.cpp
    src/compiler/evaluator_primitives_messaging.cpp
    src/compiler/evaluator_primitives_io.cpp
    src/compiler/evaluator_primitives_tui.cpp
    src/compiler/evaluator_primitives_agent.cpp
    src/compiler/evaluator_primitives_memory.cpp
    src/compiler/evaluator_primitives_policy.cpp
    src/compiler/evaluator_security.cpp
    src/compiler/evaluator_primitives_security.cpp
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

# Issue #873/#874: limit concurrent links. Each test binary links
# aura_test_objects + (often) aura_jit_test_objects + LLVM — parallel
# links thrash RAM on 2-core CI runners more than they save wall time.
# Compiles remain unbounded via ninja -j. Override: -DAURA_LINK_JOBS=N.
if(NOT DEFINED AURA_LINK_JOBS)
    set(AURA_LINK_JOBS 4)
endif()
if(AURA_LINK_JOBS GREATER 0)
    set_property(GLOBAL PROPERTY JOB_POOLS "link_pool=${AURA_LINK_JOBS}")
    set(CMAKE_JOB_POOL_LINK link_pool)
    message(STATUS "Link job pool: ${AURA_LINK_JOBS} (AURA_LINK_JOBS)")
endif()

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
    # Issue #1559: present_batch / draw_batch engine (header-based, non-module).
    src/renderer/render_primitives.cpp
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
    src/serve/http_health.cpp
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
    src/compiler/aura_jit_bridge.cpp
    # Issue #675 follow-up: http_health.cpp provides
    # aura::serve::http::port_from_env (used by
    # evaluator_primitives_security.cpp's
    # (query:security-stats) primitive). Without this
    # the test_ir link fails with "undefined reference
    # to aura::serve::http::port_from_env".
    src/serve/http_health.cpp)

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
# Issue #477: production-quality test file — enforce [[nodiscard]],
# unused-parameter, and sign-compare warnings on test_ir.cpp itself.
# The linked-in production sources (aura_test_objects, the per-target
# source list above) keep the lax `aura_test_compile_options` settings.
aura_strict_test_warnings(tests/test_ir.cpp)
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
# Issue #477: test_concurrent.cpp is the main concurrency specification;
# opt the test source itself into strict warnings.
aura_strict_test_warnings(tests/test_concurrent.cpp)
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
#   - aura_jit.cpp / aura_jit_runtime.cpp / aura_jit_bridge.cpp
#     (need -fno-rtti + LLVM; shared via aura_jit_test_objects below)
#   - Per-test observability headers (h-only; not compiled)
#   - LLVM link / AURA_HAVE_LLVM defines (via aura_jit_test_objects)
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
    src/serve/http_health.cpp
    # Issue #439 / #451: C-linkage shims (non-module)
    # for Fiber::check_gc_safepoint + the
    # (query:orchestration-metrics) primitive.
    src/compiler/fiber_bridge.cpp
    # Issue #1559: present_batch / draw_batch engine.
    src/renderer/render_primitives.cpp
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

# Shared LLVM JIT objects for issue tests (and similar binaries).
# Previously every aura_issue_test_link_llvm_jit() consumer recompiled
# aura_jit{,_runtime,_bridge}.cpp under its own CMakeFiles/<target>.dir/
# tree. With 100+ issue tests that means 100+ concurrent GCC module
# dyndep scans of the same TUs — CI has hit:
#   fatal error: when writing output to ...aura_jit_bridge.cpp.o.ddi.i:
#   Invalid argument
# Compiling once into aura_jit_test_objects eliminates that storm
# (same pattern as aura_test_objects for non-JIT sources).
# -fno-rtti is required by LLVM headers; applied once on this target.
if(LLVM_FOUND)
    # Issue #1527: self-contained JIT test archive —
    #   aura_jit.cpp needs aura_jit_prim_dispatch (weak stub) and
    #   handle_contract_violation (contract_handler) so minimal
    #   unit tests (e.g. test_issue_1477) link without service.ixx.
    # Do NOT add aura_jit_bridge_stub.cpp here — real bridge is already
    # in the archive (duplicate symbols).
    add_library(aura_jit_test_objects STATIC
        src/compiler/aura_jit.cpp
        src/compiler/aura_jit_runtime.cpp
        src/compiler/aura_jit_bridge.cpp
        src/compiler/aura_jit_prim_dispatch_stub.cpp
        src/core/contract_handler.cpp
        src/core/contract_stub.cpp
    )
    set_property(TARGET aura_jit_test_objects PROPERTY CXX_MODULE_STD ON)
    target_include_directories(aura_jit_test_objects PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/compiler
        ${LLVM_INCLUDE_DIRS}
    )
    target_compile_definitions(aura_jit_test_objects PUBLIC AURA_HAVE_LLVM=1)
    target_compile_features(aura_jit_test_objects PUBLIC cxx_std_26)
    aura_test_compile_options(aura_jit_test_objects)
    # Issue #1527: C++26 contracts for aura_jit.cpp pre/post + handlers.
    target_compile_options(aura_jit_test_objects PRIVATE -fcontracts)
    set_source_files_properties(
        src/core/contract_handler.cpp
        src/core/contract_stub.cpp
        PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
    # -fno-rtti only on LLVM-facing TUs. Do NOT put it on the whole
    # target: aura_jit_bridge.cpp does `import std`, and GCC 16 module
    # BMIs encode the dialect (C++26/contracts vs C++26/no-rtti/contracts).
    # A target-wide -fno-rtti makes bridge fail to load std.gcm.
    set_source_files_properties(
        src/compiler/aura_jit.cpp
        src/compiler/aura_jit_runtime.cpp
        PROPERTIES COMPILE_FLAGS "-fno-rtti -fcontracts")
    target_link_libraries(aura_jit_test_objects PUBLIC ${llvm_libs} ${CMAKE_DL_LIBS})
endif()

# Phase 2 dedup helper (definition placed early so it precedes all
# test_issue_* and pilot calls in this file). See also the pilots section
# comment for history.
#
# Fix #1 refactor: the helper used to inline `target_sources(PRIVATE ...)
# ast_impl/type_impl/value_impl` into every test_issue_* target. This
# caused each target to compile those sources independently. Now they
# come from the aura_test_objects static lib (see above), so the helper
# only wires modules + the per-test sources a few callers still need
# (observability headers; JIT is aura_jit_test_objects).
function(aura_add_issue_test NAME)
    # Source: tests/domain/<NAME>.cpp (preferred) or tests/<NAME>.cpp
    aura_resolve_test_cpp(${NAME} _aura_test_src)
    add_executable(${NAME} ${_aura_test_src})
    set_property(TARGET ${NAME} PROPERTY CXX_MODULE_STD ON)
    # tests/ for test_harness.hpp and domain/cases/* includes
    target_include_directories(${NAME} PRIVATE src tests)
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
    # Issue #871: 减法原则 — CTest LABELS for fast filtering.
    # Each issue test gets the "issue" label so that
    # `ctest -L issue` runs ALL issue_* tests; CTest LABELS
    # must be set on the test (via set_tests_properties) — not
    # on the target — to be picked up by `ctest --print-labels`
    # and `ctest -L <label>` filtering. Bundle-specific labels
    # (light / jit / fiber) can be added per-call as a third
    # arg: aura_add_issue_test(test_issue_N "" bundle_name).
    set_tests_properties(${NAME}_verification PROPERTIES LABELS "issue")
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
    # Issue #675 follow-up: http_health.cpp provides
    # aura::serve::http::port_from_env (used by
    # evaluator_primitives_security.cpp's
    # (query:security-stats) primitive). Without this
    # the test_gc_evaluator_integration link fails
    # with "undefined reference to
    # aura::serve::http::port_from_env".
    src/serve/http_health.cpp
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
# Issue #477: contract test is small + production-quality; opt into strict
# warnings for the test source itself.
aura_strict_test_warnings(tests/test_contracts.cpp)
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
    target_sources(test_spec_jit PRIVATE src/compiler/aura_jit.cpp src/compiler/aura_jit_runtime.cpp src/compiler/aura_jit_bridge_stub.cpp)
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
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge_stub.cpp)
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
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge_stub.cpp)
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

# ── JIT consistency / observability tests (Issue #427) ─────────
# Verifies the (query:jit-stats) surface (format() includes
# all 12 documented fields), unhandled_opcode_count atomicity
# under concurrent bumps, observability-counter independence,
# and fresh-instance zero baseline. Doesn't need a full LLVM
# pipeline — exercises the public metrics + per-function
# accessors directly. Same build pattern as test_jit_metrics.
add_executable(test_jit_consistency tests/test_jit_consistency.cpp
    tests/test_jit_metrics_stub.cpp
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge_stub.cpp)
target_include_directories(test_jit_consistency PRIVATE src)
set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
target_sources(test_jit_consistency PRIVATE src/core/contract_handler.cpp)
target_link_libraries(test_jit_consistency PRIVATE stdc++ stdc++exp pthread aura-reflect)
if(LLVM_FOUND)
    target_include_directories(test_jit_consistency PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(test_jit_consistency PRIVATE ${llvm_libs})
    target_compile_definitions(test_jit_consistency PRIVATE AURA_HAVE_LLVM=1)
endif()
add_test(NAME jit_consistency COMMAND test_jit_consistency
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
aura_issue_test_link_llvm_jit(test_issue_265)
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
# Issue #508 slice: dead-coercion-zerooverhead-stats hash observability.
aura_add_issue_test(test_issue_508_observability)
aura_issue_test_link_llvm_jit(test_issue_508_observability)
# Issue #510 slice: eda-verification-stats hash observability.
aura_add_issue_test(test_issue_510_observability)
aura_issue_test_link_llvm_jit(test_issue_510_observability)
# Issue #511 slice: workspace-snapshot-stats hash observability.
aura_add_issue_test(test_issue_511_observability)
aura_issue_test_link_llvm_jit(test_issue_511_observability)
# Issue #512 slice: runtime-orchestration-stats hash observability.
aura_add_issue_test(test_issue_512_observability)
aura_issue_test_link_llvm_jit(test_issue_512_observability)
# Issue #513 slice: aot-hot-reload-stats hash observability.
aura_add_issue_test(test_issue_513_observability)
aura_issue_test_link_llvm_jit(test_issue_513_observability)
# Issue #515 slice: consolidated-p0-production-stats hash observability.
aura_add_issue_test(test_issue_515_observability)
aura_issue_test_link_llvm_jit(test_issue_515_observability)
# Issue #516 slice: prompt6-memory-safety-stats hash observability.
aura_add_issue_test(test_issue_516_observability)
aura_issue_test_link_llvm_jit(test_issue_516_observability)
# Issue #519 slice: edsl-eda-sv-closedloop-stats hash observability.
aura_add_issue_test(test_issue_519_observability)
aura_issue_test_link_llvm_jit(test_issue_519_observability)
# Issue #521 slice: multi-fiber-orchestration-stats hash observability.
aura_add_issue_test(test_issue_521_observability)
aura_issue_test_link_llvm_jit(test_issue_521_observability)
# Issue #522 slice: aot-production-reload-stats hash observability.
aura_add_issue_test(test_issue_522_observability)
aura_issue_test_link_llvm_jit(test_issue_522_observability)
# Issue #523 slice: envframe-production-safety-stats hash observability.
aura_add_issue_test(test_issue_523_observability)
aura_issue_test_link_llvm_jit(test_issue_523_observability)
# Issue #524 slice: macro-production-hygiene-stats hash observability.
aura_add_issue_test(test_issue_524_observability)
aura_issue_test_link_llvm_jit(test_issue_524_observability)
# Issue #525 slice: guard-production-impact-stats hash observability.
aura_add_issue_test(test_issue_525_observability)
aura_issue_test_link_llvm_jit(test_issue_525_observability)
# Issue #528 slice: pattern-production-index-stats hash observability.
aura_add_issue_test(test_issue_528_observability)
aura_issue_test_link_llvm_jit(test_issue_528_observability)
# Issue #530 slice: incremental-production-relower-stats hash observability.
aura_add_issue_test(test_issue_530_observability)
aura_issue_test_link_llvm_jit(test_issue_530_observability)
# Issue #532 slice: jit-consistency-stats hash observability.
aura_add_issue_test(test_issue_532_observability)
aura_issue_test_link_llvm_jit(test_issue_532_observability)
# Issue #533 slice: soa-production-columnar-stats hash observability.
aura_add_issue_test(test_issue_533_observability)
aura_issue_test_link_llvm_jit(test_issue_533_observability)
# Issue #534 slice: arena-production-compaction-stats hash observability.
aura_add_issue_test(test_issue_534_observability)
aura_issue_test_link_llvm_jit(test_issue_534_observability)
# Issue #535 slice: contracts-production-hotpath-stats hash observability.
aura_add_issue_test(test_issue_535_observability)
aura_issue_test_link_llvm_jit(test_issue_535_observability)
# Issue #539 slice: sv-production-verification-stats hash observability.
aura_add_issue_test(test_issue_539_observability)
aura_issue_test_link_llvm_jit(test_issue_539_observability)
# Issue #540 slice: eda-stability-stats hash observability.
aura_add_issue_test(test_issue_540_observability)
aura_issue_test_link_llvm_jit(test_issue_540_observability)
# Issue #541 slice: pattern-sv-verification-stats hash observability.
aura_add_issue_test(test_issue_541_observability)
aura_issue_test_link_llvm_jit(test_issue_541_observability)
# Issue #557 slice: top5-commercial-coverage-stats hash observability.
aura_add_issue_test(test_issue_557_observability)
aura_issue_test_link_llvm_jit(test_issue_557_observability)
# Issue #567 slice: primitives-governance-stats hash observability.
aura_add_issue_test(test_issue_567_observability)
aura_issue_test_link_llvm_jit(test_issue_567_observability)
# Issue #568 slice: soa-children-columnar-migration-stats hash observability.
aura_add_issue_test(test_issue_568_observability)
aura_issue_test_link_llvm_jit(test_issue_568_observability)
# Issue #569 slice: arena-auto-compact-defrag-stats hash observability.
aura_add_issue_test(test_issue_569_observability)
aura_issue_test_link_llvm_jit(test_issue_569_observability)
# Issue #572 slice: pass-pipeline-dirtyaware-stats hash observability.
aura_add_issue_test(test_issue_572_observability)
aura_issue_test_link_llvm_jit(test_issue_572_observability)
# Issue #578 slice: sv-structured-edsl-stats hash observability.
aura_add_issue_test(test_issue_578_observability)
aura_issue_test_link_llvm_jit(test_issue_578_observability)
# Issue #579 slice: verification-feedback-loop-stats hash observability.
aura_add_issue_test(test_issue_579_observability)
aura_issue_test_link_llvm_jit(test_issue_579_observability)
# Issue #580 slice: hardware-backend-stats hash observability.
aura_add_issue_test(test_issue_580_observability)
aura_issue_test_link_llvm_jit(test_issue_580_observability)
# Issue #581 slice: stable-ref-sv-scale-stats hash observability.
aura_add_issue_test(test_issue_581_observability)
aura_issue_test_link_llvm_jit(test_issue_581_observability)
# Issue #582 slice: eda-concurrency-stats hash observability.
aura_add_issue_test(test_issue_582_observability)
aura_issue_test_link_llvm_jit(test_issue_582_observability)
# Issue #583 slice: primitives-registry-core-stats hash observability.
aura_add_issue_test(test_issue_583_observability)
aura_issue_test_link_llvm_jit(test_issue_583_observability)
# Issue #584 slice: primitives-hotpath-stats AI-agent stress observability.
aura_add_issue_test(test_issue_584_observability)
aura_issue_test_link_llvm_jit(test_issue_584_observability)
# Issue #585 slice: primitives-error-stats unified recovery observability.
aura_add_issue_test(test_issue_585_observability)
aura_issue_test_link_llvm_jit(test_issue_585_observability)
# Issue #586 slice: eda-primitives-stats infra registry observability.
aura_add_issue_test(test_issue_586_observability)
aura_issue_test_link_llvm_jit(test_issue_586_observability)
# Issue #587 slice: primitives-ai-native-stats development observability.
aura_add_issue_test(test_issue_587_observability)
aura_issue_test_link_llvm_jit(test_issue_587_observability)
# Issue #591 slice: scheduler-mutation-coord-stats steal/GC observability.
aura_add_issue_test(test_issue_591_observability)
aura_issue_test_link_llvm_jit(test_issue_591_observability)
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

# Issue #1900: refine #1698 — strengthen mutate:atomic-batch
# strong atomicity + expand dispatch table 5 → 14 ops.
# Validates: (a) all 14 sub-ops work in batch; (b)
# atomic_batch_unsupported_op_total + interleaved_mutation_prevented
# metrics bump correctly; (c) 1000+ iter concurrent fiber stress
# runs clean. Pure CompilerService tests.
aura_add_issue_test(test_issue_1900)
aura_issue_test_link_llvm_jit_minimal(test_issue_1900)

# Issue #1901 (refine #1822): EDA self-evolution
# StableNodeRef refresh-after-mutate contract. Validates:
# (a) eda:demo-sv-self-evolution runs clean for 1000+ iter
# without stale-ref crash; (b) stable_ref_auto_refresh_in_eda_total
# + eda_self_evolution_stale_ref_prevented metrics bump correctly;
# (c) explicit stale-ref refresh path works; (d) primitive
# call surface intact. Pure CompilerService tests.
aura_add_issue_test(test_eda_self_evolution_stable_ref)
aura_issue_test_link_llvm_jit_minimal(test_eda_self_evolution_stable_ref)

# Issue #1902 (refine #1818 / #1821): EDA primitive
# MutationBoundaryGuard exception safety. Validates:
# (a) eda:run-verification-feedback wraps throwable helpers
# in try/catch + guard_ok=false on exception; (b)
# eda:run-commercial-simulator-stub (#1821 sibling) wraps
# the throwable body in Guard + try/catch; (c) 3 metrics
# (eda_feedback_exception_total + eda_feedback_rolled_back_total
# + guard_exception_rollback_success) addressable in stats hash;
# (d) happy-path call surface intact. Pure CompilerService
# tests.
aura_add_issue_test(test_eda_guard_exception_safety)
aura_issue_test_link_llvm_jit_minimal(test_eda_guard_exception_safety)

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
# Issue #599: Compiler root epoch/version + GC synergy post-invalidate_function.
# 7 ACs: compiler-root-stats (schema 599), root-refresh on invalidate,
# stale/env-mismatch/dangling counters, multi-round monotonic + regression.
# Non-duplicative with #531 (closure-env-safety), #598 (linear-runtime).
aura_add_issue_test(test_compiler_root_epoch_gc_safety_post_invalidate)
aura_issue_test_link_llvm_jit(test_compiler_root_epoch_gc_safety_post_invalidate)
# Issue #600: Per-block dirty + impact scope + closure bridge synergy for
# minimal re-lower in AI self-mod. 7 ACs: incremental-closure-stats
# (schema 600), blocks-relowered/bridge-hits/min-scope-win/jit-sync,
# multi-round monotonic + regression. Non-duplicative with #530/#429/#531.
aura_add_issue_test(test_incremental_perblock_closure_bridge_safety)
aura_issue_test_link_llvm_jit(test_incremental_perblock_closure_bridge_safety)
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
# Issue #1455: occurrence stale propagation after mark_dirty / mutate
aura_add_issue_test(test_issue_1455_occurrence_stale_propagation)
aura_issue_test_link_llvm_jit(test_issue_1455_occurrence_stale_propagation)
# Issue #1456: affected_subtree locality for incremental typecheck
aura_add_issue_test(test_issue_1456_affected_subtree_locality)
aura_issue_test_link_llvm_jit(test_issue_1456_affected_subtree_locality)
# Issue #1457: IR type propagation + CastOp zero-overhead
aura_add_issue_test(test_issue_1457_type_propagation_castop_zerooverhead)
aura_issue_test_link_llvm_jit(test_issue_1457_type_propagation_castop_zerooverhead)
# Issue #1458: linear ownership post-mutate hardening
aura_add_issue_test(test_issue_1458_linear_ownership_post_mutate)
aura_issue_test_link_llvm_jit(test_issue_1458_linear_ownership_post_mutate)
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
# Issue #592: Panic checkpoint + arena auto-rollback lifecycle
# with nested MutationBoundaryGuard + fiber yield/steal/resume
# safety (P0 Task6 production review). 10 scenarios:
# nested Guard checkpoint ownership + success/panic restore
# paths + post-restore arena size invariant + fiber yield/
# resume depth preservation + steal matrix + bridge transfer
# + 5000-iter fuzz + 8-fiber concurrent panic+yield stress
# + regression. Extends #353. Non-duplicative with #548
# (lifecycle counters) + #546 (arena sizes) + #588 (stack
# sync) + #453 (bridge hooks).
aura_add_issue_test(test_panic_checkpoint_fiber_resume_safety)
aura_issue_test_link_llvm_jit(test_panic_checkpoint_fiber_resume_safety)
# Issue #593: Hygiene + MacroIntroduced propagation in query:pattern
# + IR InlinePass — AST→query→IR closed-loop (P0 Task6 review).
# 10 scenarios: macro expand + default hygiene filter + :respect-
# hygiene alias + mutate safe + IR InlinePass policy + tag_arity
# dirty hook + query:pattern-ir-hygiene-closed-loop-stats (schema
# 593) + combined metrics monotonic + 5000-iter fuzz stress +
# regression. Non-duplicative with #547/#486/#501/#524/#420.
aura_add_issue_test(test_query_pattern_hygiene_macrointroduced)
aura_issue_test_link_llvm_jit(test_query_pattern_hygiene_macrointroduced)
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
# Issue #596: Guard + panic checkpoint + reflect/schema validation +
# fiber resume safety closed loop (P0 Task6 production review).
# 7 ACs: guard-panic-reflect-stats (schema 596), Guard validate+commit,
# panic restore observability, fiber yield/resume depth, inner-guard steal
# defer, multi-round monotonic + regression. Non-duplicative with #592
# (panic checkpoint matrix), #594 (reflection-selfmod), #548 (lifecycle).
aura_add_issue_test(test_guard_panic_reflect_fiber_resume_task6)
aura_issue_test_link_llvm_jit(test_guard_panic_reflect_fiber_resume_task6)
# Issue #673: Unified Runtime Observability Layer (P1) cross-module
# correlation primitive (production review / monitoring foundation).
# 7 ACs: runtime-observability-correlated-stats (schema 673),
# steal-attempts-correlated baseline, steal-deferred-correlated,
# steal-ownership-violation-correlated, correlated-events-total sum,
# multi-round monotonic, 4 module-local query regressions. Non-duplicative
# with #591 (scheduler-mutation-coord), #438 (fiber-migration),
# #448 (mutation-coordination), #599 (compiler-root), #592 (panic-fiber),
# #596 (guard-panic-reflect).
aura_add_issue_test(test_runtime_observability_correlated_stats_673)
aura_issue_test_link_llvm_jit(test_runtime_observability_correlated_stats_673)
# Issue #674: Closed-loop self-evolution stability stress testing
# (P0) companion observability primitive + chaos outcome classifier.
# 7 ACs: self-evolution-chaos-stats (schema 674), chaos-cycles,
# chaos-failures, chaos-corruptions, chaos-events-total sum,
# multi-round chaos simulation (100 cycles) monotonic,
# 4 module-local regression (panic-checkpoint-lifecycle, atomic-batch-
# rollback, stable-ref-cow-fiber, runtime-observability-correlated).
# Non-duplicative with #548/#529/#527/#400/#679 (production recovery
# counter set); #674 covers the chaos outcome classifier layer.
aura_add_issue_test(test_self_evolution_chaos_stable_674)
aura_issue_test_link_llvm_jit(test_self_evolution_chaos_stable_674)
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
# Issue #1444: MutationBoundary full coverage audit + new coverage primitive.
aura_add_issue_test(test_mutation_boundary_full_coverage)
aura_issue_test_link_llvm_jit(test_mutation_boundary_full_coverage)
# Issue #1445: orchestration steal boost + new steal-stats primitive.
aura_add_issue_test(test_orchestration_steal_boost)
aura_issue_test_link_llvm_jit(test_orchestration_steal_boost)
# Issue #1446: nested boundary + steal + GC compact re-pin smoke test.
aura_add_issue_test(test_fiber_steal_panic_checkpoint_nested_gc)
aura_issue_test_link_llvm_jit(test_fiber_steal_panic_checkpoint_nested_gc)
# Follow-up smoke: #1446 AC3 primitive expose + #1445 priority-degrade counters.
aura_add_issue_test(test_followup_smoke)
aura_issue_test_link_llvm_jit(test_followup_smoke)
# Issue #1465: AST-level dirty short-circuit API surface (#1465 AC2).
aura_add_issue_test(test_dirty_short_circuit_api)
aura_issue_test_link_llvm_jit(test_dirty_short_circuit_api)
# Issue #418: EnvFrame dual-path + stale policy closed loop.
aura_add_issue_test(test_envframe_dualpath_stale_closed_loop_418)
aura_issue_test_link_llvm_jit(test_envframe_dualpath_stale_closed_loop_418)
# Issue #419: defuse_version modular API + AOT dispatch closed loop.
aura_add_issue_test(test_defuse_version_closed_loop_419)
aura_issue_test_link_llvm_jit(test_defuse_version_closed_loop_419)
# Issue #420: MacroIntroduced end-to-end hygiene contract closed loop.
aura_add_issue_test(test_macro_hygiene_contract_closed_loop_420)
aura_issue_test_link_llvm_jit(test_macro_hygiene_contract_closed_loop_420)
# Issue #421: query:pattern recursive MacroIntroduced filter closed loop.
aura_add_issue_test(test_pattern_macro_filter_closed_loop_421)
aura_issue_test_link_llvm_jit(test_pattern_macro_filter_closed_loop_421)
# Issue #422: Hygiene violation detection in mutate/Guard paths.
aura_add_issue_test(test_hygiene_violation_closed_loop_422)
aura_issue_test_link_llvm_jit(test_hygiene_violation_closed_loop_422)
# Issue #423: query:pattern structural pre-index closed loop.
aura_add_issue_test(test_pattern_structural_index_closed_loop_423)
aura_issue_test_link_llvm_jit(test_pattern_structural_index_closed_loop_423)
# Issue #424: StableNodeRef WorkspaceTree COW closed loop.
aura_add_issue_test(test_stable_ref_workspace_tree_closed_loop_424)
aura_issue_test_link_llvm_jit(test_stable_ref_workspace_tree_closed_loop_424)
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
# Issue #1903: dual-path consistency enforcement on every bind/bind_symid
# + post-steal refresh + post-materialize + GCEnvWalkFn INVALID_VERSION
# skip + prefer bindings_symid_ + (query:envframe-dual-consistency-stats)
# primitive. Non-duplicative with #355/#418/#485/#488/#1269/#1550:
# #1903 is the P0 ship that turns the existing observability counter
# family into an enforced invariant (ensure_dual_path_consistent on
# every mutate path) plus INVALID_VERSION GC walk filter. Scenarios:
#   - bind_with_linear_state / bind_symid_with_linear_state maintain
#     length + content parity after every call (pool_ set + null)
#   - post-copy materialize_call_env keeps dual-path consistent
#   - walk_env_frame_roots skips INVALID_VERSION + prefers bindings_symid_
#     (legacy bindings_ fallback bumps envframe_gc_walk_legacy_fallback_uses_)
#   - (gc-heap) cycle does not collect INVALID_VERSION / inconsistent frames
#   - concurrent mutate + steal + GC + (query:envframe-dual-consistency-stats)
#     10k iter: counters grow, no desync leak
aura_add_issue_test(test_issue_1903)
aura_issue_test_link_llvm_jit(test_issue_1903)
# Issue #1904: mutate:* primitive MutationBoundaryGuard full coverage.
# Closes #213 Cycle 1-3 + the remaining ~13 legacy mutate:* primitives
# that still use manual std::unique_lock(ev.workspace_mtx_) + explicit
# ev.defuse_version_.fetch_add(...) patterns. Adds:
#   - mutation_legacy_manual_lock_total counter (#1904)
#   - (engine:metrics "query:mutation-guard-coverage") primitive
#     (returns -1 regression sentinel / 10000 vacuous / basis points)
#   - scripts/check_legacy_mutate_lock.py CI linter (regex + self-test
#     + synthetic fixture detection) wired through tests/test_issue_1904.cpp
#     AC6 + AC7
#   - 4 Evaluator accessors: get_mutation_boundary_primitives_wrapped /
#     get_mutation_legacy_manual_lock_total /
#     get_mutation_guard_try_acquire_total /
#     get_mutation_guard_try_acquire_reject_total
# Non-duplicative with #213 Cycles 1-3 (rollback + initial migration);
# #1904 is the final close-out: zero remaining manual lock + bump sites
# in evaluator_primitives_*.cpp, every primitive uses Guard RAII,
# defuse_version_ bumps exactly once per successful boundary.
aura_add_issue_test(test_issue_1904)
aura_issue_test_link_llvm_jit(test_issue_1904)
# Issue #1905: close AOT incremental hot-update / invalidation loop
# (build on #1046). Adds:
#   - 6 new CompilerMetrics counters backing the AOT refresh +
#     bridge_epoch bump + post-steal revalidation surface:
#       aot_live_closure_refresh_on_mutation_total /
#       aot_live_closure_refresh_on_steal_total /
#       aot_bridge_epoch_bump_on_mutation_total /
#       aot_bridge_epoch_bump_on_steal_total /
#       aot_region_mismatch_on_resume_total /
#       aot_stale_deopt_on_steal_total
#   - 6 new Evaluator bump + 6 new getter helpers
#   - 2 bridge hooks in aura_jit_bridge.cpp:
#       aura_refresh_live_closures_for_mutated_define (Step 2 - flush
#         outermost exit bumps g_aot_table_epoch + invalidates captured
#         closure's bridge_epoch snapshots)
#       aura_post_steal_aot_revalidate (Step 3 - post-steal AOT drift
#         detection returns 1 on bridge_epoch mismatch)
#   - (engine:metrics "query:aot-hot-update-stats") primitive returns
#     -1 sentinel when aot_stale_deopt_on_steal_total > 0; otherwise
#     sum of 6 counters (sum-path, like #1903/#1904 P0 shape)
#   - scripts/check_aot_hot_update_coverage.py CI linter (regex +
#     self-test + comment stripping + production file scan)
#   - Wire-up of flush_mutation_boundary Step 2 + complete_post_resume_steal_refresh
#     Step 3 marker in evaluator_fiber_mutation.cpp
# Non-duplicative with #1046 (foundation), #1508/#1491/#1632 (dual-check),
# #1592/#1631 (post-steal refresh), #1626 (apply_closure dual-check).
aura_add_issue_test(test_issue_1905)
aura_issue_test_link_llvm_jit(test_issue_1905)
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
aura_add_issue_bundle(jit_late1)
aura_add_issue_bundle(jit_late2)
aura_add_issue_bundle(jit_late3)
aura_add_issue_bundle(jit_late4)
aura_add_issue_bundle(jit_late5)
aura_add_issue_bundle(light_late)

# Performance (#871/#873): standalones that are already linked into a
# profile bundle are EXCLUDE_FROM_ALL — build on demand only. Default
# `ninja all_test_issue_targets` builds ~12 fat bundles instead of
# 200+ × ~200MB individual executables.
set(_AURA_BUNDLE_MEMBER_STANDALONES
  test_aot_bridge_checkpoint_version_steal
  test_arena_auto_compact_fiber_defag_shape_dirty_closedloop
  test_atomic_batch_snapshot_stable_ref_ai_loops
  test_aura_result_error_policy
  test_compiler_core_incremental_selfmod_gaps
  test_cpp26_contracts_hotpath_arena_soa_value_shape_pass
  test_dead_coercion_elision_narrow_evidence
  test_eda_production_infra
  test_eda_sv_verification_closedloop_stress
  test_edsl_core_stability_cow_atomic_query_mutate
  test_error_merr
  test_harness_pilot
  test_highperf_cpp26_gaps_arena_soa_value_shape_pass
  test_issue_115
  test_issue_116
  test_issue_117
  test_issue_118
  test_issue_119
  test_issue_120
  test_issue_121
  test_issue_122
  test_issue_123
  test_issue_124
  test_issue_125
  test_issue_126
  test_issue_127
  test_issue_128
  test_issue_130
  test_issue_131
  test_issue_132
  test_issue_134
  test_issue_135
  test_issue_136
  test_issue_137
  test_issue_138
  test_issue_139
  test_issue_140
  test_issue_141
  test_issue_142
  test_issue_143
  test_issue_144
  test_issue_145
  test_issue_146
  test_issue_147
  test_issue_148
  test_issue_149
  test_issue_158
  test_issue_159
  test_issue_159_bench
  test_issue_161
  test_issue_162
  test_issue_163
  test_issue_165
  test_issue_166
  test_issue_167
  test_issue_168
  test_issue_169
  test_issue_171
  test_issue_173
  test_issue_174
  test_issue_177
  test_issue_178
  test_issue_181
  test_issue_182
  test_issue_184
  test_issue_187
  test_issue_188
  test_issue_189
  test_issue_190
  test_issue_191
  test_issue_192
  test_issue_193
  test_issue_194
  test_issue_196
  test_issue_197
  test_issue_197_observability
  test_issue_204
  test_issue_205
  test_issue_206
  test_issue_208
  test_issue_210
  test_issue_211
  test_issue_212
  test_issue_213
  test_issue_213_panic_fiber
  test_issue_218
  test_issue_224
  test_issue_224_closure_bridge
  test_issue_225_bridge_invalidation
  test_issue_226
  test_issue_227
  test_issue_228
  test_issue_237
  test_issue_240
  test_issue_242
  test_issue_243
  test_issue_244
  test_issue_246
  test_issue_247
  test_issue_248
  test_issue_249
  test_issue_250
  test_issue_252
  test_issue_253
  test_issue_254
  test_issue_255
  test_issue_256
  test_issue_258
  test_issue_259
  test_issue_260
  test_issue_261
  test_issue_262
  test_issue_263
  test_issue_264
  test_issue_265
  test_issue_267
  test_issue_270
  test_issue_271
  test_issue_272
  test_issue_273
  test_issue_274
  test_issue_275
  test_issue_276
  test_issue_277
  test_issue_289
  test_issue_290
  test_issue_291
  test_issue_292
  test_issue_293
  test_issue_294
  test_issue_295
  test_issue_296
  test_issue_297
  test_issue_298
  test_issue_300
  test_issue_401
  test_issue_428_closure
  test_issue_429_soa
  test_issue_430_arena_compaction
  test_issue_431_cxx26
  test_issue_440_edsl_readiness
  test_issue_442_seva_demo
  test_issue_444_strategy_evolution
  test_issue_445_openclaw_integration
  test_issue_446_seva_docs_metrics
  test_issue_452_aot_hot_update
  test_issue_462_shape_aware_folding
  test_issue_463_soa_phase2_wiring
  test_issue_464_arena_auto_compaction
  test_issue_465_cxx26_hotpath
  test_issue_470_stable_ref_sv_scale
  test_issue_471_dirty_sv_scale
  test_issue_473
  test_issue_479
  test_issue_482
  test_issue_484_minimal
  test_issue_485
  test_issue_486
  test_issue_488
  test_issue_489
  test_issue_490
  test_issue_491
  test_issue_492
  test_issue_493
  test_issue_494
  test_issue_496
  test_issue_497
  test_issue_498
  test_issue_499
  test_issue_500
  test_issue_501_hygiene
  test_issue_502
  test_issue_503
  test_issue_504
  test_issue_505
  test_issue_507
  test_issue_508_observability
  test_issue_510_observability
  test_issue_511_observability
  test_issue_512_observability
  test_issue_513_observability
  test_issue_515_observability
  test_issue_516_observability
  test_issue_519_observability
  test_issue_521_observability
  test_issue_522_observability
  test_issue_523_observability
  test_issue_524_observability
  test_issue_525_observability
  test_issue_528_observability
  test_issue_530_observability
  test_issue_532_observability
  test_issue_533_observability
  test_issue_534_observability
  test_issue_535_observability
  test_issue_539_observability
  test_issue_540_observability
  test_issue_541_observability
  test_issue_557_observability
  test_issue_567_observability
  test_issue_589
  test_issue_590
  test_issue_601
  test_issue_603
  test_issue_604
  test_issue_606
  test_issue_614
  test_issue_615
  test_issue_616
  test_issue_617
  test_issue_618
  test_issue_620
  test_issue_621
  test_issue_622
  test_issue_623
  test_issue_624
  test_issue_625
  test_issue_626
  test_issue_630
  test_issue_631
  test_issue_632
  test_issue_633
  test_issue_637
  test_issue_640
  test_issue_641
  test_issue_642
  test_issue_643
  test_issue_644
  test_issue_645
  test_issue_646
  test_issue_647
  test_issue_648
  test_issue_649
  test_issue_650
  test_issue_651
  test_issue_660_cache_define_bundle
  test_issue_661_sv_interface_structure_stats
  test_issue_662_sva_structured_mutate
  test_issue_663_hardware_backend_sv_stats
  test_issue_664_sv_defuse_stats
  test_issue_665_sv_stability_stats
  test_issue_667_primitives_apply_stats
  test_issue_668
  test_issue_669
  test_issue_670
  test_issue_671
  test_issue_672
  test_issue_675
  test_issue_676
  test_issue_677
  test_issue_678
  test_issue_679
  test_issue_680
  test_issue_681
  test_issue_682
  test_issue_683
  test_issue_684
  test_issue_685
  test_issue_686
  test_issue_687
  test_issue_688
  test_issue_689
  test_issue_690
  test_issue_691
  test_issue_692
  test_issue_693
  test_issue_694
  test_issue_695
  test_issue_697
  test_issue_698
  test_issue_706
  test_issue_707
  test_issue_708
  test_issue_709
  test_issue_710
  test_issue_711
  test_issue_712
  test_issue_713
  test_issue_714
  test_issue_715
  test_issue_716
  test_issue_717
  test_issue_718
  test_issue_719
  test_issue_720
  test_issue_721
  test_issue_722
  test_issue_723
  test_issue_726
  test_issue_728
  test_issue_731
  test_issue_732
  test_issue_733
  test_issue_735
  test_issue_756
  test_issue_757
  test_issue_758
  test_issue_759
  test_issue_760
  test_issue_761
  test_issue_762
  test_issue_763
  test_issue_764
  test_issue_765
  test_issue_766
  test_issue_767
  test_issue_768
  test_issue_769
  test_issue_770
  test_issue_771
  test_issue_772
  test_issue_773
  test_issue_774
  test_issue_775
  test_issue_776
  test_issue_777
  test_issue_778
  test_issue_779
  test_issue_780
  test_issue_781
  test_issue_782
  test_issue_783
  test_issue_784
  test_issue_785
  test_issue_786
  test_issue_787
  test_issue_788
  test_issue_789
  test_issue_790
  test_issue_791
  test_issue_792
  test_issue_793
  test_issue_794
  test_issue_795
  test_issue_796
  test_issue_797
  test_issue_803
  test_issue_804
  test_issue_806
  test_issue_edsl_hygiene_atomic
  test_linear_ownership_occurrence_predicate_mutate
  test_linear_ownership_postmutate_guard_steal_envframe
  test_list_vector_soa_hotpath_ai_loops
  test_longrunning_infra_primitives
  test_macro_hygiene_fiber_panic_aot_soa_self_evo
  test_open_issues_phase1_batch
  test_per_fiber_stack_pool_high_concurrency
  test_primitives_capture_contract
  test_primitives_hotpath_registry_slo
  test_primitives_init
  test_prompt2_6_impact_scope_quote_lambda_bridge_env
  test_prompt6_epoch_atomic_visibility_fiber_steal
  test_prompt6_linear_jit_l2_post_invalidate_arena_gc
  test_reflection_runtime_validate_macro_edsl_mutate
  test_runtime_concurrent_full_cycle_chaos
  test_scheduler_llm_bottleneck_adaptive_steal_gc
  test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate
  test_stable_ref_cow_subworkspace_concurrent_ai
  test_stable_ref_cross_cow_provenance_enforcement
  test_sv_verification_commercial_emit_fidelity
  test_sv_verification_edsl_full_structured_closedloop
  test_sv_verification_self_evolution_closed_loop_reliability
  test_typechecker_incremental_guard_steal_fidelity
  test_typesystem_solve_delta_occurrence_priority_heavy_mutate
  test_typesystem_type_propagation_jit_l2_typed_mutate
  test_typesystem_typed_mutate_incremental_gaps
)
foreach(_m IN LISTS _AURA_BUNDLE_MEMBER_STANDALONES)
  if(TARGET ${_m})
    set_target_properties(${_m} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endforeach()

add_custom_target(all_test_issue_targets ALL)
add_dependencies(all_test_issue_targets test_issues_light)
add_dependencies(all_test_issue_targets test_issues_jit)
add_dependencies(all_test_issue_targets test_issues_jit_minimal)
add_dependencies(all_test_issue_targets test_issues_jit_contract)
add_dependencies(all_test_issue_targets test_issues_jit_tests)
add_dependencies(all_test_issue_targets test_issues_fiber)
add_dependencies(all_test_issue_targets test_issues_jit_late1)
add_dependencies(all_test_issue_targets test_issues_jit_late2)
add_dependencies(all_test_issue_targets test_issues_jit_late3)
add_dependencies(all_test_issue_targets test_issues_jit_late4)
add_dependencies(all_test_issue_targets test_issues_jit_late5)
add_dependencies(all_test_issue_targets test_issues_light_late)
add_dependencies(all_test_issue_targets test_issue_170)

add_dependencies(all_test_issue_targets test_issue_195)

add_dependencies(all_test_issue_targets test_issue_214)
add_dependencies(all_test_issue_targets test_issue_215)
add_dependencies(all_test_issue_targets test_issue_216)
add_dependencies(all_test_issue_targets test_issue_217)

# Issue #425: EDSL hygiene integration into query primitives
# (query:filter :hygiene / :skip-macro-introduced top-level
# keywords) + post-truncate size verification on
# restore_panic_checkpoint. Follows the test_issue_244 +
# test_issue_190 + test_issue_250 pattern (query primitives +
# marker / atomic batch integration).
aura_add_issue_test(test_issue_edsl_hygiene_atomic)
aura_issue_test_link_llvm_jit(test_issue_edsl_hygiene_atomic)

# Issue #428: closure bridge + bridge_epoch drift observability.
# Verifies (query:closure-stats) (new unified surface), the
# 7-field consistency with legacy (closure:stats), and the
# multi-round mutate:set-body bridge refresh path. Follows
# the test_issue_252 + test_issue_531 pattern (closure
# dispatch + bridge_epoch observability).
aura_add_issue_test(test_issue_428_closure)
aura_issue_test_link_llvm_jit(test_issue_428_closure)

# Issue #429: IRFunctionSoA + FlatAST PersistentChildVector
# production cache locality. Verifies the new
# (query:soa-dirty-stats) primitive (8-field hash with the
# live per-block / per-instruction dirty state) and the
# CompilerService::get_soa_dirty_stats() / func_instruction_count
# accessors. The full SoA consumer migration (lowering,
# ir_executor, JIT, Pass pipeline) is multi-week and deferred.
aura_add_issue_test(test_issue_429_soa)
aura_issue_test_link_llvm_jit(test_issue_429_soa)

# Issue #430: Production Arena compaction: live-object moving /
# handle indirection / auto-trigger policy. The full scope is
# multi-week; the scope-limited close ships POLICY +
# OBSERVABILITY + TEST:
#   - ArenaGroup::compact_with_policy(name, policy) (3 policies)
#   - (arena:compact-with-policy ...) Aura primitive
#   - (query:arena-compaction-stats-hash) 10-field hash variant
#   - (stats:count) 40 → 41
aura_add_issue_test(test_issue_430_arena_compaction)
aura_issue_test_link_llvm_jit(test_issue_430_arena_compaction)

# Issue #431: deepen C++26 Contracts + Concepts + consteval
# invariants. Full scope is multi-week (20+ new Contracts
# across Arena + Value + Shape + SoA + dirty + Pass);
# scope-limited close ships:
#   - 3 new Concepts (SoAColumnar / DirtyPropagator /
#     ShapeDispatchable) in aura.core.concepts
#   - src/core/cxx26_invariants.ixx (22 consteval invariants
#     for SmallObjectPool tier sizes + Value tag layouts +
#     concept self-checks)
#   - (query:cxx26-invariants) 5-field hash primitive
#   - (stats:count) 41 → 42
aura_add_issue_test(test_issue_431_cxx26)
aura_issue_test_link_llvm_jit(test_issue_431_cxx26)

# Issue #440: Consolidated Task 1 Review — EDSL production
# readiness aggregator. The issue body enumerates gaps
# already tracked by separate issues (#420, #421, #423,
# #424, #425, #438, #439). The scope-limited close ships
# the OBSERVABILITY + TEST layer: (query:edsl-readiness)
# 6-field hash that aggregates the top EDSL readiness
# signals in one query (replaces the conservative "ask
# 30+ separate (query:*-stats) primitives" pattern).
aura_add_issue_test(test_issue_440_edsl_readiness)
aura_issue_test_link_llvm_jit(test_issue_440_edsl_readiness)

# Issue #442: SEVA Demo end-to-end scaffolding. The full
# scope is multi-day (real SystemVerilog FIFO + iverilog
# integration + testbench generation + autonomous bug-fix
# + docs + screenshots); scope-limited close ships the
# AURA-DEMO foundation: fifo_dut.aura + seva_demo.aura +
# tests/test_issue_442_seva_demo.cpp + demos/seva/README.md.
# No new primitives required — uses existing verify:* (#437,
# #469), mutate:*, query:* primitives + the #440 aggregator.
aura_add_issue_test(test_issue_442_seva_demo)
aura_issue_test_link_llvm_jit(test_issue_442_seva_demo)

# Issue #444: SEVA Demo coverage-driven mutation strategy
# evolution controller. Full scope is multi-week (pheromone
# tracking + PID + pluggable strategy registry + agent
# loop); scope-limited close ships:
#   - 3 built-in strategies (coverage-greedy, bug-fix-
#     priority, minimal-mutation)
#   - Per-strategy pheromone counters in CompilerMetrics
#   - (query:strategy-evolution-stats) 8-field hash
#   - 4 new Aura primitives (set-strategy, active,
#     report-success, escalate)
#   - SEVA demo extension showing the strategy loop
#   - (stats:count) 43 → 44
aura_add_issue_test(test_issue_444_strategy_evolution)
aura_issue_test_link_llvm_jit(test_issue_444_strategy_evolution)

# Issue #445: OpenClaw / LLM Agent integration layer for
# SEVA natural-language goals. Full scope: NL goal →
# sequence of query/mutate/eval calls + safety guardrails
# + audit log + OpenClaw skill/plugin + integration
# contract docs. Scope-limited close ships the AURA-SIDE
# foundation:
#   - 4 SEVA goal primitives (seva:achieve-coverage,
#     seva:fix-reset-bugs, seva:generate-regression,
#     seva:approve-mutation)
#   - (query:seva-audit-log) audit trail primitive
#   - demos/seva/openclaw-skill/seva_skill.py — example
#     OpenClaw skill showing the integration shape
#   - (stats:count) 44 → 45
aura_add_issue_test(test_issue_445_openclaw_integration)
aura_issue_test_link_llvm_jit(test_issue_445_openclaw_integration)

# Issue #446: SEVA docs + tutorial + metrics. Full scope:
# docs/demos/seva.md + tutorial + metrics collection +
# README update. Scope-limited close ships the DOCS +
# METRICS layer:
#   - docs/demos/seva.md (architecture + how-to + primitives)
#   - demos/seva/TUTORIAL.md (step-by-step walkthrough)
#   - README.md (Demos section pointing to SEVA)
#   - (seva:run-demo-with-metrics) 6-field hash primitive
#   - (stats:count) 45 → 46
aura_add_issue_test(test_issue_446_seva_docs_metrics)
aura_issue_test_link_llvm_jit(test_issue_446_seva_docs_metrics)
aura_add_issue_test(test_issue_452_aot_hot_update)
aura_issue_test_link_llvm_jit(test_issue_452_aot_hot_update)
aura_add_issue_test(test_issue_462_shape_aware_folding)
aura_issue_test_link_llvm_jit(test_issue_462_shape_aware_folding)
aura_add_issue_test(test_issue_463_soa_phase2_wiring)
aura_issue_test_link_llvm_jit(test_issue_463_soa_phase2_wiring)
# Issue #675: Build/CI reproducibility + SBOM + security scan gates.
# 6 ACs: query:ci-reproducibility-stats hash + 5 fields +
# sanitizer-mode none + SOURCE_DATE_EPOCH flag + stats:list/count.
aura_add_issue_test(test_issue_675)
aura_issue_test_link_llvm_jit(test_issue_675)
# Issue #676: sandbox capability enforcement + mutation audit log.
aura_add_issue_test(test_issue_676)
aura_issue_test_link_llvm_jit(test_issue_676)
# Issue #677: HTTP health endpoints + deployment stats + install layout.
aura_add_issue_test(test_issue_677)
aura_issue_test_link_llvm_jit(test_issue_677)
# Issue #678: PCV span lifetime safety in concurrent query paths.
aura_add_issue_test(test_issue_678)
aura_issue_test_link_llvm_jit(test_issue_678)
# Issue #679: nested Guard + atomic-batch rollback alignment.
aura_add_issue_test(test_issue_679)
aura_issue_test_link_llvm_jit(test_issue_679)
# Issue #680: precise Define mutate IR/JIT/bridge invalidation.
aura_add_issue_test(test_issue_680)
aura_issue_test_link_llvm_jit(test_issue_680)
# Issue #681: IRClosure/EnvFrame epoch enforcement post-mutate.
aura_add_issue_test(test_issue_681)
aura_issue_test_link_llvm_jit(test_issue_681)
# Issue #682: GC root registration for compiler IRClosure/EnvId.
aura_add_issue_test(test_issue_682)
aura_issue_test_link_llvm_jit(test_issue_682)

aura_add_issue_test(test_issue_683)
aura_issue_test_link_llvm_jit(test_issue_683)

aura_add_issue_test(test_issue_684)
aura_issue_test_link_llvm_jit(test_issue_684)

aura_add_issue_test(test_issue_685)
aura_issue_test_link_llvm_jit(test_issue_685)

aura_add_issue_test(test_issue_686)
aura_issue_test_link_llvm_jit(test_issue_686)

aura_add_issue_test(test_issue_688)
aura_issue_test_link_llvm_jit(test_issue_688)

aura_add_issue_test(test_issue_689)
aura_issue_test_link_llvm_jit(test_issue_689)
# Issue #690: ConstraintSystem typed-mutation reverify + blame completeness.
aura_add_issue_test(test_issue_690)
aura_issue_test_link_llvm_jit(test_issue_690)
# Issue #691: CoercionMap + NarrowingRecord provenance linkage.
aura_add_issue_test(test_issue_691)
aura_issue_test_link_llvm_jit(test_issue_691)
# Issue #692: ADT exhaustiveness + pattern provenance typed-mutation.
aura_add_issue_test(test_issue_692)
aura_issue_test_link_llvm_jit(test_issue_692)
# Issue #693: Hardware backend SV commercial feedback closed-loop.
aura_add_issue_test(test_issue_693)
aura_issue_test_link_llvm_jit(test_issue_693)
# Issue #694: SVA structured NodeTags + builders + mutate primitives.
aura_add_issue_test(test_issue_694)
aura_issue_test_link_llvm_jit(test_issue_694)
# Issue #695: EDA-SV verification closed-loop stress harness.
aura_add_issue_test(test_issue_695)
aura_issue_test_link_llvm_jit(test_issue_695)
# Issue #696: EDA-SV verification closed-loop production stress (named harness).
aura_add_issue_test(test_eda_sv_verification_closedloop_stress)
aura_issue_test_link_llvm_jit(test_eda_sv_verification_closedloop_stress)
# Issue #697: Declarative Primitives Extension Kit + AI Agent EDA integration.
aura_add_issue_test(test_issue_697)
aura_issue_test_link_llvm_jit(test_issue_697)
# Issue #698: Hardware backend commercial interop closed-loop.
aura_add_issue_test(test_issue_698)
aura_issue_test_link_llvm_jit(test_issue_698)
# Issue #706: Adaptive StealBudget + work-stealing bias for LLM bottleneck.
aura_add_issue_test(test_issue_706)
aura_issue_test_link_llvm_jit(test_issue_706)
# Issue #707: Bounded per-fiber stack pool + panic/steal re-stamp.
aura_add_issue_test(test_issue_707)
aura_issue_test_link_llvm_jit(test_issue_707)
# Issue #652: Per-fiber MutationStack / YieldCheckpoint bounded pool for
# high-concurrency orchestration. 7 ACs: per-fiber-stack-pool-stats
# (schema 652), 200+ fiber pool reuse >= 80%, lazy alloc bound,
# steal/yield/panic max-depth cap, monotonic + regression.
# Non-duplicative with #707 (ship), #588, #542.
aura_add_issue_test(test_per_fiber_stack_pool_high_concurrency)
aura_issue_test_link_llvm_jit(test_per_fiber_stack_pool_high_concurrency)
# Issue #708: AOT hot-reload refcount swap + region/panic multi-fiber safety.
aura_add_issue_test(test_issue_708)
aura_issue_test_link_llvm_jit(test_issue_708)
# Issue #653: AOT bridge_epoch / emit_version in restore_post_yield_or_rollback.
# 7 ACs: aot-checkpoint-version-stats (schema 653), defuse/bridge drift,
# deopt-on-steal, multi-fiber steal+reload stress, monotonic + regression.
# Non-duplicative with #708 (ship), #652, #592.
aura_add_issue_test(test_aot_bridge_checkpoint_version_steal)
aura_issue_test_link_llvm_jit(test_aot_bridge_checkpoint_version_steal)
# Issue #654: Macro+reflect+self-evo hygiene vs fiber/panic/AOT/SoA gaps.
# 7 ACs: macro-hygiene-fiber-panic-stats (schema 654), 5 gap counters,
# macro expand+Guard+query matrix, fiber yield + regression.
# Non-duplicative with #593/#596/#597/#600/#653.
aura_add_issue_test(test_macro_hygiene_fiber_panic_aot_soa_self_evo)
aura_issue_test_link_llvm_jit(test_macro_hygiene_fiber_panic_aot_soa_self_evo)
# Issue #655: EDSL core stability — StableNodeRef COW + tag_arity delta +
# nested atomic rollback + children safe view + mutate invalidation. 7 ACs:
# edsl-core-stability-stats (schema 655), query/mutate matrix + regression.
# Non-duplicative with #527/#552/#622/#654.
aura_add_issue_test(test_edsl_core_stability_cow_atomic_query_mutate)
aura_issue_test_link_llvm_jit(test_edsl_core_stability_cow_atomic_query_mutate)
# Issue #657: Compiler core incremental self-mod — cache bridge epoch, impact-scope
# partial re-lower, JIT unhandled deopt, linear metadata, quote fallback. 7 ACs:
# compiler-core-incremental-stats (schema 657), recursive/quote/mutate matrix + regression.
# Non-duplicative with #600/#680/#530/#655.
aura_add_issue_test(test_compiler_core_incremental_selfmod_gaps)
aura_issue_test_link_llvm_jit(test_compiler_core_incremental_selfmod_gaps)
# Issue #658: High-perf C++26 gaps — Arena tier fallback, IRSoA dirty cascade,
# Value v2 classify, Shape history jitter, Pass DirtyAware. 7 ACs:
# highperf-cpp26-stats (schema 658), heavy mutate matrix + regression.
# Non-duplicative with #657/#642/#571/#570/#494.
aura_add_issue_test(test_highperf_cpp26_gaps_arena_soa_value_shape_pass)
aura_issue_test_link_llvm_jit(test_highperf_cpp26_gaps_arena_soa_value_shape_pass)
# Issue #659: Type system typed-mutate gaps — solve_delta reverify, dead coercion
# elim, linear ownership, occurrence provenance, coercion map incremental. 7 ACs:
# typesystem-typed-mutate-stats (schema 659), predicate/linear/mutate matrix + regression.
# Non-duplicative with #656/#657/#690/#508.
aura_add_issue_test(test_typesystem_typed_mutate_incremental_gaps)
aura_issue_test_link_llvm_jit(test_typesystem_typed_mutate_incremental_gaps)
# Issue #709: Primitives registry fast dispatch + capture discipline + EDA.
aura_add_issue_test(test_issue_709)
aura_issue_test_link_llvm_jit(test_issue_709)
# Issue #710: verify_tool/diagnostic Guard + StableRef closed-loop.
aura_add_issue_test(test_issue_710)
aura_issue_test_link_llvm_jit(test_issue_710)
# Issue #711: AI-native primitives meta introspection + generator +
# EDA category backfill + style spec (elevates #669 #617 #643,
# non-duplicative). Closed-loop integration test wires together
# the foundations shipped from #617 (catalog), #669 (per-name
# enrichment), #671 (style spec), #697 (generator + extension
# stats). 7 ACs: drift check, catalog by-category-eda,
# generator category detection (sva/verification/eda), closed-
# loop Agent sim, regression of foundations. No new primitives
# added (all five surfaces already exist).
aura_add_issue_test(test_issue_711)
aura_issue_test_link_llvm_jit(test_issue_711)
# Issue #712: macro subtree reflect validation — subtree-level
# diagnostics (validation-calls, schema-mismatches-caught,
# post-mutate-hygiene-drift, schema-pass, schema=712) via the
# new (query:macro-reflect-validation-stats) primitive. Pairs
# with #488/#654 foundations. 6 ACs / 14 CHECKs: hash shape,
# fresh-service zero state, schema sentinel, subtree walk after
# workspace mutation, regression of #654 + #488 surfaces.
aura_add_issue_test(test_issue_712)
aura_issue_test_link_llvm_jit(test_issue_712)
# Issue #713: macro hygiene in JIT/AOT/Interpreter — 3 atomics
# backing (query:macro-jit-hygiene-stats, schema 713) primitive.
# Pairs with #488/#654/#712 (#713 is the JIT/AOT/Interpreter
# extension of those subtree/workspace hygiene surfaces).
# 5 ACs / 17 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #697 + #712. Real JIT deopt / AOT reload marker-mismatch /
# interpreter fallback hooks are follow-up (each requires
# dedicated LLVM/dlopen/IR-dispatch session).
aura_add_issue_test(test_issue_713)
aura_issue_test_link_llvm_jit(test_issue_713)
# Issue #714: self-evolution closed-loop observability primitive —
# 3 atomics + 3 bump helpers + (query:self-evolution-closedloop-stats,
# schema 714) primitive (10-entry hash: 9 fields + schema sentinel).
# Pairs with #488/#654/#712/#713 (#714 is the FIRST primitive that
# correlates hygiene + dirty + epoch + reflect + strategy into a
# single Agent-facing report).
# 5 ACs / 16 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility + recommended-strategy flip,
# regression of #712 + #713. Real Guard dtor + mark_dirty_upward +
# reflect auto_validate hook wiring + mutation strategy primitives +
# correlation primitive + AI self-evo demo loop are all follow-up
# (each is a dedicated session).
aura_add_issue_test(test_issue_714)
aura_issue_test_link_llvm_jit(test_issue_714)
# Issue #715: cross-workspace StableNodeRef validation + provenance —
# is_valid_in_layer pure helper + 3 atomics + 3 bump helpers +
# (query:stable-ref-layer-stats, schema 715) primitive (4-entry hash:
# 3 fields + schema sentinel). Pairs with #191/#255/#368/#655/#736
# (#715 is the FIRST observability surface that splits out
# cross-layer + COW-boundary signals for the Agent).
# 6 ACs / 22 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, is_valid_in_layer helper
# behavior (NULL-id / workspace_id mismatch / non-existent node /
# pin_for_cow), regression of #712 + #713 + #714. Real
# MutationBoundaryGuard + workspace-merge auto-remap + lib/std/ AI
# multi-layer orchestration are all follow-up (each is a dedicated
# session in evaluator_workspace_tree.cpp).
aura_add_issue_test(test_issue_715)
aura_issue_test_link_llvm_jit(test_issue_715)
# Issue #716: pattern matcher observability primitive —
# 3 atomics + 3 bump helpers + (query:pattern-stats, schema 716)
# primitive (4-entry hash: 3 fields + schema sentinel). Pairs with
# #547/#490/#621/#654 tag_arity_index_* which track the index
# itself — #716 tracks the matcher call path + hygiene filter +
# fast-path promotion as separate signals.
# 5 ACs / 24 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility (incl. bulk filter bump
# with N>1 argument + default-arg single bump), regression of
# #712 + #713 + #714 + #715. Real is_macro_introduced() skip wiring
# in query_matcher.cpp + real pattern_index_ lookup + fast-path
# promotion + benchmark on 10k+ node AST are all follow-up (each
# is a dedicated session).
aura_add_issue_test(test_issue_716)
aura_issue_test_link_llvm_jit(test_issue_716)
# Issue #717: fiber-boundary violation stats primitive —
# 3 atomics + 3 bump helpers + (query:fiber-boundary-violation-stats,
# schema 717) primitive (4-entry hash: 3 fields + schema sentinel).
# Pairs with #438 query:fiber-migration-stats which tracks the
# SCHEDULER side (steal-attempts / boundary-violations / defers);
# #717 tracks the GUARD side (rollback / yield-resume / recovery-
# failure) as a separate signal.
# 5 ACs / 22 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712 + #713 +
# #714 + #715 + #716. Real fiber-context check on guard dtor +
# panic_checkpoint per-fiber mutation_stack_ snapshot + multi-fiber
# yield+resume tests + AI orchestration rollback docs are all
# follow-up (each is a dedicated session).
aura_add_issue_test(test_issue_717)
aura_issue_test_link_llvm_jit(test_issue_717)
# Issue #718: incremental fine-grained per-block re-lower — pure
# helper + atomics + primitive. Phase 1 ships:
#   - ir_cache_pure::should_partial_relower (0 / 1..7 / 8+ decision)
#   - 4 atomics (impact-blocks-hit / partial-relowers / full-fallbacks
#     / time-saved-us)
#   - 4 bump helpers
#   - (query:incremental-relower-stats, schema 718) primitive
#   - test_issue_718.cpp (6 ACs)
# Real invalidate_function wiring + lower_to_ir_with_cache partial
# re-lower + pass_manager short-circuit + large-define mutate test
# are follow-up (each is a dedicated session in service.ixx +
# lowering_impl.cpp + pass_manager.ixx).
# 6 ACs / 30 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility (incl. bulk time-saved bump),
# should_partial_relower pure helper behavior across 4 buckets
# (0/1-7/8+), regression of #712 + #713 + #714 + #715 + #716 + #717.
aura_add_issue_test(test_issue_718)
aura_issue_test_link_llvm_jit(test_issue_718)
# Issue #719: Prompt 6 closure/EnvFrame epoch + linear ownership +
# GC root sync runtime safety counters — 4 atomics + 4 bump helpers
# + (query:closure-env-epoch-safety-stats, schema 719) primitive
# (5-entry hash: 4 fields + schema sentinel). Pairs with #672
# linear_stats (compile-time) and #681 epoch enforcement (IR-level
# metadata); #719 tracks runtime outcomes in apply_closure + JIT +
# GC as separate signals.
# 5 ACs / 25 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712 + #713
# + #714 + #715 + #716 + #717 + #718. Real apply_closure epoch
# check + IRClosure/closure_bridge_ invalidate management +
# linear_ownership_state runtime guard in GuardShape/JIT +
# ScopedCompilerRoot GC hook + multi-round mutate:rebind +
# closure apply + fiber steal/GC/panic test are all follow-up
# (each is a dedicated session in evaluator_eval_flat.cpp +
# service.ixx + evaluator_gc.cpp + ir_executor_impl.cpp +
# aura_jit*.cpp).
aura_add_issue_test(test_issue_719)
aura_issue_test_link_llvm_jit(test_issue_719)
# Issue #720: JIT/Interpreter parity stats primitive — 4 atomics + 4
# bump helpers + (query:jit-interpreter-parity-stats, schema 720)
# primitive (5-entry hash: 4 fields + schema sentinel). Pairs with
# the aggregate unhandled_opcode_count / fallback_count metrics in
# aura_jit.cpp; #720 splits by *cause* (unhandled spike vs metadata
# mismatch vs deopt-on-mutate vs interpreter fallback) and adds
# post-mutation drift signals.
# 5 ACs / 29 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712 + #713 +
# #714 + #715 + #716 + #717 + #718 + #719. Real FlatInstruction
# metadata extension + unhandled hook + GuardShape/linear full
# consume + deopt->service wiring + PrimCall/closure/linear fast-path
# coverage + opcode + metadata parity test matrix are all follow-up
# (each is a dedicated session in aura_jit.cpp + aura_jit.h +
# aura_jit_bridge.cpp + service.ixx + ir_executor_impl.cpp).
aura_add_issue_test(test_issue_720)
aura_issue_test_link_llvm_jit(test_issue_720)
# Issue #721: IRFunctionSoA column migration + dirty cascade
# observability primitive — 3 atomics + 3 bump helpers +
# (query:ir-soa-completeness-stats, schema 721) primitive (4-entry
# hash: 3 fields + schema sentinel). Pairs with the existing
# IRFunctionSoA scaffold (10 columns + mark_block_dirty cascade);
# #721 tracks SoA column migration progress + dirty cascade shape
# propagation + PCV byte savings as separate signals.
# 5 ACs / 28 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility (incl. bulk byte-savings bump
# with N>0 byte count), regression of #712..#720. Real PCV-style
# column extension + add_instruction atomic growth + IRInstructionView
# dirty bit query + port of hot emit/view paths to SoA iterators +
# ShapeProfiler invalidate hook + Arena defrag hint + large define
# mutate:rebind test are all follow-up (each is a dedicated session
# in ir_soa.ixx + lowering_impl.cpp + evaluator + aura_jit.cpp +
# ShapeProfiler + Arena).
aura_add_issue_test(test_issue_721)
aura_issue_test_link_llvm_jit(test_issue_721)
# Issue #722: Arena tier/dtor/compact integration observability
# primitive — 4 atomics + 4 bump helpers + (query:arena-integration-stats,
# schema 722) primitive (5-entry hash: 4 fields + schema sentinel).
# Pairs with the existing ArenaStats in arena.ixx (internal aggregate
# metrics); #722 exposes Arena <-> dirty/shape integration signals
# as separate counters the Agent can consume.
# 5 ACs / 30 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility (incl. fragmentation setter
# with scaled-ratio value + setter overwrite edge case), regression
# of #712..#721. Real fallback dirty-mark hook + dtor-to-shape
# integration + auto-compact policy + 10k+ small node stress test
# are all follow-up (each is a dedicated session in arena.ixx +
# ShapeProfiler + ir_cache_pure + service.ixx).
aura_add_issue_test(test_issue_722)
aura_issue_test_link_llvm_jit(test_issue_722)
# Issue #723: value-dispatch stats primitive (Pass pipeline +
# Value v2 + Shape history integration) — 4 atomics + 4 bump
# helpers + (query:value-dispatch-stats, schema 723) primitive
# (5-entry hash: 4 fields + schema sentinel). Pairs with the existing
# pass_manager.ixx Wraps / value.ixx v2 / shape_profiler.cpp history
# infrastructure; #723 tracks the integration outcomes as separate
# counters the Agent can consume to decide Pass short-circuit / deopt.
# 5 ACs / 33 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#722.
# Real DirtyAware impl for Wraps + static_asserts + Contracts
# expansion + shape history ring_buffer replacement + deopt_hook
# wiring + partial dirty IRSoA + high shape churn mutate test are
# all follow-up (each is a dedicated session in pass_manager.ixx +
# value.ixx + value_tags.h + shape_profiler.cpp).
aura_add_issue_test(test_issue_723)
aura_issue_test_link_llvm_jit(test_issue_723)
# Issue #726: closed-loop self-evolution reliability observability
# primitive — 3 atomics + 3 bump helpers + (query:closed-loop-
# reliability-stats, schema 726) primitive (4-entry hash: 3 fields
# + schema sentinel). Pairs with #748 SV verification structure
# stats (structural mutate + emit + dirty re-emit); #726 covers
# the closed-loop reliability side (ref drift prevented +
# rollback success + feedback mutate rounds) as separate counters
# the Agent can consume to monitor SEVA self-evolution stability.
# 5 ACs / 28 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#723.
# Real verify:parse-coverage-feedback / parse-assert-failure /
# parse-formal-cex / mutate:from-verification-feedback primitives
# + closed-loop controller (seva:run-closed-loop) + enhanced
# MutationBoundaryGuard subtree StableNodeRef validation + backend
# re-emit tie-in (#725) + multi-round closed-loop stress test +
# SEVA demo extension + docs are all follow-up (each is a dedicated
# session in evaluator_primitives_verify*.cpp or new verify_
# primitives module + MutationBoundaryGuard + ast dirty + new
# test harness + SEVA demo + docs).
aura_add_issue_test(test_issue_726)
aura_issue_test_link_llvm_jit(test_issue_726)
# Issue #728: unified structured error + provenance + recovery
# observability primitive — 3 atomics + 3 bump helpers +
# (query:unified-error-stats, schema 728) primitive (4-entry hash:
# 3 fields + schema sentinel). Pairs with the existing #478
# primitive-error-stats pair and #585 primitives-error-stats hash;
# #728 covers the *unified model* specifically — structured
# ErrorValue hits + provenance StableNodeRef capture + recovery
# success — as separate counters the Agent consumes to monitor
# migration to the unified error model.
# 5 ACs / 36 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#726.
# Real unified ErrorValue / EvalValue tagged-error extension +
# refactor of evaluator_primitives_list.cpp / math.cpp / regex /
# verify error sites to make_structured_primitive_error(guard,
# kind, msg, context) + new (primitive:error) / (with-error) /
# (primitive:try) primitives + Guard.capture auto-provenance +
# CI lint for legacy make_primitive_error usage +
# tests/test_unified_primitive_error_model harness + SEVA
# error-resilient closed-loop + primitives_style.md mandate are
# all follow-up (each is a dedicated session in evaluator.ixx +
# primitives_detail.h + evaluator_primitives_*.cpp + Guard +
# diagnostic + ast.ixx StableNodeRef + new test + SEVA + docs).
aura_add_issue_test(test_issue_728)
aura_issue_test_link_llvm_jit(test_issue_728)
# Issue #731: Arena + SoA + EnvFrame concurrent compaction safety
# observability primitive — 4 atomics + 4 bump helpers +
# (query:arena-concurrent-compact-stats, schema 731) primitive
# (5-entry hash: 4 fields + schema sentinel). Pairs with the
# existing #722 query:arena-integration-stats tier hash and #743
# Arena auto-compact policy + fiber safepoint primitive; #731
# covers the *concurrent* safety specifically — scheduler-safepoint
# coordination + EnvFrame GCEnvWalkFn revalidation + panic-rollback-
# compact integration + race prevention — as separate per-decision-
# point counters the Agent consumes to monitor production memory
# stability under multi-fiber steal/resume + concurrent compact.
# 5 ACs / 41 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#728.
# Real concurrent compact / defrag safepoint coordination in
# arena.ixx + GCEnvWalkFn EnvFrame revalidation in evaluator_gc.cpp
# + fiber.cpp resume() / transfer hook integration + panic
# checkpoint snapshot integration + new
# tests/test_arena_concurrent_compact_envframe_fiber_steal.cpp
# harness (heavy alloc / mutate under 10+ fibers + steal + periodic
# compact + panic injection) + #674 chaos stress integration + docs
# are all follow-up (each is a dedicated session in arena.ixx +
# gc_coordinator + evaluator_gc.cpp + fiber.cpp + panic_checkpoint
# + new test + chaos stress + docs).
aura_add_issue_test(test_issue_731)
aura_issue_test_link_llvm_jit(test_issue_731)
# Issue #732: AOT hot-reload safe-swap at MutationBoundary
# observability primitive — 1 atomic + 1 bump helper +
# (query:aot-safe-swap-boundary-stats, schema 732) primitive
# (6-entry hash: 5 fields + schema sentinel). Pairs with the
# existing #708 query:aot-reload-stats 5-7 field high-level
# summary + #644 query:aot-reload-func-table-stats enforcement
# primitive + #590 query:aot-hotupdate-stats 3 atomics; #732
# covers the *safe-swap at MutationBoundary* specifically —
# reloads that fired at outermost safe-swap point (NOT mid-
# mutation) — as the per-decision-point signal the Agent
# consumes to monitor safe-swap adoption rate + zero-downtime
# orchestration quality.
# 5 ACs / 44 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#731.
# Real atomic func_table refcount swap protocol in
# aura_jit_bridge.cpp aura_reload_aot_module + per-region
# isolation enforcement + aura_aot_request_safe_reload() API +
# MutationBoundaryGuard outermost exit hook + GraceEpoch defer-old-
# decrement + tests/test_aot_hot_swap_refcount_region_guard_safe.cpp
# harness (multi-agent different regions + AOT emit + mutate +
# concurrent apply + reload at boundary) + #674 chaos stress
# integration + docs are all follow-up (each is a dedicated
# session in aura_jit_bridge.cpp + MutationBoundaryGuard + fiber.cpp
# + new test + chaos stress + docs).
aura_add_issue_test(test_issue_732)
aura_issue_test_link_llvm_jit(test_issue_732)
# Issue #733: Macro SyntaxMarker propagation + IR/JIT hygiene
# enforcement observability primitive — 5 atomics + 5 bump
# helpers + (query:ir-marker-hygiene-stats, schema 733)
# primitive (6-entry hash: 5 fields + schema sentinel). Pairs
# with #714 (query:self-evolution-closedloop-stats ref-drift +
# rollback + feedback-mutate-rounds) + #455 (internal ir marker
# snapshot) + #373 (mutate hygiene guard flat.is_macro_
# introduced internal check); #733 covers the *marker
# propagation + IR/JIT enforcement* specifically across the
# entire compile/execution pipeline (macro expand -> AST ->
# lowering -> IR -> JIT hot-path -> Interpreter) as separate
# per-decision-point counters the Agent consumes to monitor
# hygiene fidelity.
# 5 ACs / 53 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#732.
# Real propagate_marker_from_ast helper in lowering_impl.cpp +
# ir_soa.ixx marker_ column + aura_jit.cpp + aura_jit_runtime.cpp
# + ir_executor.ixx conservative policy on source_marker==
# MacroIntroduced + IRFunction creation marker-from-root-AST-
# marker in service/lowering + tests/test_macro_marker_
# propagation_ir_jit_post_mutate.cpp harness (define macro that
# introduces lambda + mutate inside it under fiber + JIT hot
# path) + #674 chaos stress integration + SEVA macro-heavy
# cases are all follow-up (each is a dedicated session in
# lowering_impl.cpp + ir_soa.ixx + aura_jit.cpp + aura_jit_
# runtime.cpp + ir_executor.ixx + new test + chaos stress +
# SEVA demo + docs).
aura_add_issue_test(test_issue_733)
aura_issue_test_link_llvm_jit(test_issue_733)
# Issue #735: MacroIntroduced provenance in StableNodeRef +
# targeted dirty/rollback for macro subtrees observability
# primitive — 4 atomics + 4 bump helpers +
# (query:macro-provenance-stats, schema 735) primitive
# (5-entry hash: 4 fields + schema sentinel). Pairs with the
# existing #714 query:self-evolution-closedloop-stats ref-drift
# + #717 query:fiber-boundary-violation-stats + #392 subtree
# gen + #373 mutate hygiene guard + #733 query:ir-marker-
# hygiene-stats IR-level marker propagation + #750 query:reflection-
# schema-stats runtime reflection validate; #735 covers the
# *MacroIntroduced provenance + targeted macro-subtree handling*
# specifically — capture-time provenance in StableNodeRef,
# hot-path consult, targeted dirty propagation, rollback success —
# as separate per-decision-point counters the Agent consumes to
# monitor precise macro handling in long-running self-evo loops.
# 5 ACs / 54 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#733.
# Real ast.ixx StableNodeRef + macro_introduced_at_capture +
# original_macro_expansion_id fields + is_valid_subtree
# macro_provenance_check + MutationBoundaryGuard + rollback_macro_
# subtree_provenance + mark_dirty_upward targeted macro-subtree +
# dirty/epoch interaction strengthening (verify/macro dirty cascade
# respect MacroIntroduced provenance for incremental re-lower) +
# StableRef / hygiene stats correlation enhancement +
# tests/test_macro_provenance_stable_ref_rollback_self_evo.cpp
# harness (nested macro expand + multi-round mutate:rebind inside
# macro body under fiber steal / panic / Guard fail) + SEVA macro
# cases + #674 chaos stress integration + docs are all follow-up
# (each is a dedicated session in ast.ixx + mutate.cpp +
# evaluator_primitives_mutate.cpp + new test + SEVA demo + chaos
# stress + docs).
aura_add_issue_test(test_issue_735)
aura_issue_test_link_llvm_jit(test_issue_735)
# Issue #756: EnvFrame dual-path consistency enforcement +
# desync panic policy + GCEnvWalkFn stale handling under
# concurrent mutation/steal observability primitive — 2
# atomics + 2 bump helpers + (query:envframe-dualpath-
# policy-stats, schema 756) primitive (5-entry hash: 4
# fields + schema sentinel). Pairs with the existing
# #647 query:envframe-dualpath-stale-stats-hash 3-field
# hash + #418 query:envframe-dualpath-stale-stats legacy
# int + existing envframe_desync_detected_ +
# envframe_gc_walk_safe_skips_ internal atomics; #756
# covers the *desync panic policy + GCEnvWalkFn stale
# handling* specifically — strict-panic vs log-and-sync
# mode firings + GC walk detected stale under concurrency
# — as separate per-decision-point counters the Agent
# consumes to monitor SoA EnvFrame dual-path production
# safety under concurrency.
# 5 ACs / 50 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #712..#735. Real mandatory ensure_envframe_dual_path_
# consistency call in walk_env_frames / GCEnvWalkFn /
# materialize_call_env / post-rollback paths + strict-panic
# vs log-and-sync policy flag + GCEnvWalkFn stale + concurrent
# steal/resume re-ensure + tests/test_envframe_dualpath_
# consistency_concurrent_steal_gc.cpp harness (heavy mutate +
# steal + GC under dual-path load) + #674 + #731 chaos stress
# integration + docs are all follow-up (each is a dedicated
# session in evaluator.ixx + evaluator_env.cpp + gc_coordinator
# + new test + chaos stress + docs).
aura_add_issue_test(test_issue_756)
aura_issue_test_link_llvm_jit(test_issue_756)
# Issue #757: fine-grained MacroIntroduced provenance tracking
# + dynamic inliner policy + AI-queryable hygiene violation
# correlation observability primitive — 2 atomics + 2 bump
# helpers + (query:macro-hygiene-provenance-stats, schema 757)
# primitive (5-entry hash: 4 fields + schema sentinel). Pairs
# with the existing #654 query:macro-hygiene-fiber-panic-stats
# 5-field hash + #458 query:pattern-hygiene-stats basic count
# + #373 mutate hygiene guard flat.is_macro_introduced + #750
# query:reflection-schema-stats runtime reflection validate;
# #757 covers the *fine-grained provenance + dynamic inliner
# policy + per-macro correlation* specifically — provenance
# captured at clone_macro_body, inliner policy violation
# firings, per-macro hygiene violation correlation, query-
# filter hits — as separate per-decision-point counters the
# Agent consumes to monitor and tune macro hygiene in self-evo
# loops.
# 5 ACs / 54 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #712..#756.
# Real ast.ixx FlatAST + provenance_ column or extended marker
# (macro_def_node_id or sym + gensym history) populated in
# clone_macro_body success path + QueryExpr :marker MacroIntroduced
# :provenance macro-name filter support + (query:macro-hygiene-
# provenance node-id) function primitive + (hygiene:set-inliner-
# respect-macro! #t/#f [subtree]) primitive + InlinePass respect_
# macro_hygiene_ dynamic check from EDSL/primitive + Guard
# integration with hygiene_violation_by_macro correlation +
# enhance query:mutation-impact-snapshot and hygiene-stats with
# macro_provenance_map + inliner_policy_violations + hygiene_
# violation_by_macro + tests/test_macro_hygiene_provenance_
# inliner_policy_ai.cpp harness (define macro with nested,
# mutate under different policies) + SEVA demo extension +
# docs are all follow-up (each is a dedicated session in ast.ixx
# + query_matcher + evaluator_primitives_query.cpp + InlinePass
# + aura_jit.cpp + MutationBoundaryGuard + new test + SEVA demo
# + docs).
aura_add_issue_test(test_issue_757)
aura_issue_test_link_llvm_jit(test_issue_757)
# Issue #758: runtime auto_validate bridge for user-defined EDSL
# structs (DEFINE_STRUCT / custom nodes) under MutationBoundaryGuard
# with macro hygiene invariant correlation observability
# primitive — 4 atomics + 4 bump helpers +
# (query:edsl-reflection-stats, schema 758) primitive (5-entry
# hash: 4 fields + schema sentinel). Pairs with the existing
# #750 query:reflection-schema-stats 4-field hash + (reflect:validate-
# macro-body) + (reflect:validate-edsl) primitives; #758 covers the
# *user-defined EDSL struct + macro hygiene invariant correlation*
# specifically — per-type EDSL struct pass, MacroIntroduced
# descendants verified for valid provenance, per-type EDSL struct
# fail, macro_def_id-correlated violations — as separate per-decision-
# point counters the Agent consumes to monitor extensible EDSL
# struct production safety in self-evo loops.
# 5 ACs / 24 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of #735/#756/#757.
# Real reflect.hh + new runtime_reflect_edsl_bridge.cpp +
# runtime_validate_edsl_struct + MutationBoundaryGuard integration
# + (reflect:validate-edsl node-id [type]) with optional type arg +
# tests/test_reflection_edsl_struct_validate_guard_mutate.cpp harness +
# SEVA custom EDSL demo + dirty/epoch cascade on violation + docs
# are all follow-up (each is a dedicated session in reflect.hh +
# runtime_reflect_edsl_bridge.cpp + evaluator_primitives_mutate.cpp +
# new test + SEVA demo + docs).
aura_add_issue_test(test_issue_758)
aura_issue_test_link_llvm_jit(test_issue_758)
# Issue #759: unified 'code-as-data' closed-loop maturity
# observability composite (production readiness dashboard for
# Task6: marker propagation fidelity, Guard rollback hygiene
# safety, reflection schema coverage on macro/EDSL subtrees,
# concurrent fiber stress success) primitive — 4 atomics + 4
# bump helpers + (query:code-as-data-maturity-stats, schema 759)
# primitive (5-entry hash: 4 fields + schema sentinel). Pairs
# with the existing #757 query:macro-hygiene-provenance-stats +
# #758 query:edsl-reflection-stats 4-field hashes; #759 covers
# the *code-as-data closed-loop maturity composite* specifically
# — marker propagation fidelity (drift / samples), Guard
# rollback hygiene safety (safe / attempts), reflection schema
# coverage on macro/EDSL subtrees (covered / total) — as
# separate per-decision-point counters the Agent consumes to
# monitor extensible code-as-data production safety.
# 5 ACs / 23 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758.
# Real tests/test_task6_code_as_data_closedloop_harness.cpp
# multi-fiber stress (random macro expansion deep nesting +
# EDSL struct mutate under Guard + simulated reflect validate
# + panic/rollback injection + steal during boundary → assert
# fidelity metrics stay high, no hygiene drift post-rollback,
# schema coverage tracks, TSan/ASan clean) + wire marker
# provenance (from #757) + runtime reflect validate (from #758)
# + Guard rollback path to feed the maturity stats (auto-
# update on every successful self-mod boundary) + SLO /
# (query:code-as-data-slo) with thresholds + Prometheus text
# or OTLP deployment exporter + Task6 health score composite
# + SEVA extension with macro-generated + user-EDSL verification
# + CI gate on harness passing with fidelity thresholds + docs
# are all follow-up (each is a dedicated session in
# observability_metrics.h + evaluator_primitives_observability
# .cpp + new test harness + SEVA demo + docs).
aura_add_issue_test(test_issue_759)
aura_issue_test_link_llvm_jit(test_issue_759)
# Issue #760: query:pattern performance + hygiene fidelity
# observability primitive — 4 atomics + 4 bump helpers +
# (query:pattern-performance-stats, schema 760) primitive
# (5-entry hash: 4 fields + schema sentinel). Pairs with the
# existing #757 query:macro-hygiene-provenance-stats +
# #758 query:edsl-reflection-stats + #759 query:code-as-data-
# maturity-stats 4-field hashes; #760 covers the *query:pattern
# performance + hygiene fidelity* specifically — linear scans
# vs index hits (perf cliff detection on tag_arity_index_ fast-
# path), wildcard cost (early exit / DFA benefit on ... rest-
# param handling), hygiene filtered (deep hygiene predicate
# :marker MacroIntroduced :from-macro sym activity) — as
# separate per-decision-point counters the Agent consumes to
# monitor query:pattern production-readiness on large macro-
# heavy concurrent workspaces.
# 5 ACs / 24 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758/#759.
# Real query_matcher.cpp tag_arity_index_ populated on add_node
# / structural mutate + specialized ... rest-param / wildcard
# handling with early exit or DFA + QueryExpr / pattern parser
# :marker MacroIntroduced :from-macro sym extension + matcher
# auto-apply hygiene filter or provenance stamp when matching
# under macro context + wire to clone_macro_body name_map +
# mandate children_safe_view / StableNodeRef pinning in all
# pattern iterator paths + integrate with MutationBoundaryGuard
# reader snapshot + (query:pattern-explain node pattern)
# primitive + tests/test_query_pattern_indexing_hygiene_
# concurrent.cpp harness (large macro-expanded AST + concurrent
# fibers + pattern mutate under Guard → assert index used,
# hygiene respected, no drift, perf win, TSan clean) + SEVA
# pattern-heavy verification self-edit + CI perf gate + docs
# are all follow-up (each is a dedicated session in
# query_matcher.cpp + evaluator_primitives_query.cpp + new
# test + SEVA demo + docs).
aura_add_issue_test(test_issue_760)
aura_issue_test_link_llvm_jit(test_issue_760)
# Issue #761: end-to-end atomic batch mutate + suppressed
# generation bump + cross-fiber safety observability primitive
# — 4 atomics + 4 bump helpers + (query:mutate-batch-stats,
# schema 761) primitive (5-entry hash: 4 fields + schema
# sentinel). Pairs with the existing #757 query:macro-hygiene-
# provenance-stats + #758 query:edsl-reflection-stats +
# #759 query:code-as-data-maturity-stats + #760 query:pattern-
# performance-stats 4-field hashes; #761 covers the *end-to-end
# atomic batch mutate + suppressed generation bump + cross-fiber
# safety composite* specifically — batch lifecycle (started /
# committed / rolled-back), suppressed bump count (churn saved),
# cross-fiber steals during suppressed batch (re-stamp events),
# hygiene violations caught within batch boundary — as separate
# per-decision-point counters the Agent consumes to monitor
# atomic compound EDSL edit production-readiness.
# 5 ACs / 26 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758/#759/#760.
# Real (mutate:batch [body]) or begin/commit primitives in
# evaluator_primitives_mutate.cpp + per-boundary atomic_batch_
# bumps_saved_ via active_mutation_stack or depth + cross-fiber
# steal during suppressed batch re-stamp + checkpoint_yield_
# boundary integration + unified mark_dirty_upward for all
# touched + defuse_version_ bump once + feed mutation-impact-
# snapshot with batch_impact flag + tests/test_mutate_batch_
# atomic_cross_fiber_safety.cpp harness (multi-fiber AI edit
# script with compound rebind+replace under batch + steal/panic
# → assert single bump, all-or-nothing, hygiene preserved,
# metrics accurate, TSan clean) + SEVA compound edit demo +
# metrics correlation link to existing hygiene-stats + stable-ref
# invalidations + defuse_version_ + CI gate + docs are all
# follow-up (each is a dedicated session in evaluator_primitives_
# mutate.cpp + ast.ixx + new test + SEVA demo + docs).
aura_add_issue_test(test_issue_761)
aura_issue_test_link_llvm_jit(test_issue_761)
# Issue #762: Workspace '锁定-导航-修改-执行' closed-loop
# orchestration observability primitive — 4 atomics + 4 bump
# helpers + (query:workspace-closedloop-orchestration-stats,
# schema 762) primitive (5-entry hash: 4 fields + schema
# sentinel). Pairs with the existing #757 query:macro-hygiene-
# provenance-stats + #758 query:edsl-reflection-stats +
# #759 query:code-as-data-maturity-stats +
# #760 query:pattern-performance-stats +
# #761 query:mutate-batch-stats 4-field hashes; #762 covers
# the *Workspace closed-loop orchestration* specifically —
# concurrent query/mutate success under fiber steal, cross-COW
# StableRef validity (auto-propagation win rate), yield point
# hit count (exhaustive yield coverage), shared_mutex contention
# events (orchestration bottleneck detection) — as separate
# per-decision-point counters the Agent consumes to monitor
# Workspace closed-loop production-readiness in orchestrated
# multi-Agent deployments.
# 5 ACs / 28 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758/#759/#760/#761.
# Real evaluator_primitives_query.cpp + mutate.cpp + workspace
# paths instrumentation with explicit fiber yield points or
# safepoint checks in pattern matcher + children_safe iteration
# + mark_dirty_upward + auto-propagate active StableRef pins or
# dirty bits via epoch or weak registry on workspace COW/clone
# in primitives + extend make_ref / get_safe in query/mutate to
# auto-refresh on workspace boundary cross + wire mark_dirty_
# upward to notify pinned refs or sub-workspace listeners +
# integration with mutation-impact + stable-ref-stats + force
# StableRef validation + dirty re-propagation for active
# Workspace edits in restore_post_yield + steal paths +
# tests/test_workspace_closedloop_fiber_multiagent_
# orchestration.cpp harness (10+ fibers/agents with parallel
# query/mutate on shared+COW workspaces + steal/yield → assert
# auto refresh, dirty consistent, no contention deadlock,
# metrics accurate, TSan clean) + SEVA multi-Agent demo +
# Prometheus / SLO (closedloop_fidelity >99.5%) + CI gate + docs
# are all follow-up (each is a dedicated session in
# evaluator_primitives_query.cpp + mutate.cpp + ast.ixx + new
# test + SEVA demo + docs).
aura_add_issue_test(test_issue_762)
aura_issue_test_link_llvm_jit(test_issue_762)
# Issue #763: runtime linear_ownership_state enforcement + GC
# root registration observability primitive — 4 atomics + 4
# bump helpers + (query:linear-ownership-gc-compiler-stats,
# schema 763) primitive (5-entry hash: 4 fields + schema
# sentinel). Pairs with the existing #757 query:macro-hygiene-
# provenance-stats + #758 query:edsl-reflection-stats +
# #759 query:code-as-data-maturity-stats +
# #760 query:pattern-performance-stats +
# #761 query:mutate-batch-stats +
# #762 query:workspace-closedloop-orchestration-stats 4-field
# hashes AND the existing query:linear-ownership-gc-stats GC
# layer observability; #763 covers the *compiler IRClosure +
# EnvFrame + invalidate runtime linear enforcement composite*
# specifically — IRClosure/EnvFrame root registrations, stale
# GC root hits on invalidate, runtime linear violations caught,
# Env version re-syncs on invalidate — as separate per-
# decision-point counters the Agent consumes to monitor linear
# ownership + GC + compiler maturation production-readiness.
# 5 ACs / 30 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758/#759/#760/#761/#762.
# Real service.ixx invalidate_function + LoweringState walk of
# live IRClosure (via closure_bridge_ or closures_ map) for
# linear_ownership_state nodes + force re-emit or mark for
# runtime check + register affected EnvId/IRClosure as GC root
# with version_ stamp synced to mutation_epoch_ + evaluator_gc
# .cpp + gc_coordinator compiler IRClosure/EnvFrame root
# registration hook (called from invalidate + fiber mutation
# boundary) + on GC walk enforce linear state via runtime
# check (debug) or DropOp simulation for owned values in
# bridged closures + ir_executor.ixx + aura_jit.cpp Apply/
# MakeClosure paths and linear ops runtime assert/check for
# linear_ownership_state consistency + on invalidate impact
# trigger root re-registration + integration with
# EscapeAnalysisWrap + DirtyAware for targeted linear dirty +
# sync with bridge_epoch bump + tests/test_prompt6_linear_
# ownership_gc_root_invalidate_closure.cpp harness (linear
# define with move/borrow + quote/lambda capture + mutate:rebind
# on body → invalidate + live closure apply under GC pressure
# → assert no violation/UAF, roots registered, metrics,
# TSan/ASan clean) + SEVA linear-ownership demo + CI gate +
# docs are all follow-up (each is a dedicated session in
# service.ixx + evaluator_gc.cpp + ir_executor.ixx + aura_jit
# .cpp + new test + SEVA demo + docs).
aura_add_issue_test(test_issue_763)
aura_issue_test_link_llvm_jit(test_issue_763)
# Issue #764: compiler Arena AST / shared_ptr<FlatAST> lifetime
# safety vs GC-managed Env/Closure in closure_bridge_
# observability primitive — 4 atomics + 4 bump helpers +
# (query:compiler-arena-closure-lifetime-stats, schema 764)
# primitive (5-entry hash: 4 fields + schema sentinel). Pairs
# with the existing #757 query:macro-hygiene-provenance-stats +
# #758 query:edsl-reflection-stats +
# #759 query:code-as-data-maturity-stats +
# #760 query:pattern-performance-stats +
# #761 query:mutate-batch-stats +
# #762 query:workspace-closedloop-orchestration-stats +
# #763 query:linear-ownership-gc-compiler-stats 4-field hashes;
# #764 covers the *compiler Arena AST / shared_ptr<FlatAST>
# lifetime vs GC-managed Env/Closure in closure_bridge_*
# composite specifically — arena AST root hits, bridge
# shared_ptr pinned, cross-lifetime violations prevented,
# invalidate AST refresh count — as separate per-decision-point
# counters the Agent consumes to monitor cross-lifetime
# production safety in incremental AI mutation flows.
# 5 ACs / 33 CHECKs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758/#759/#760/#761/#762/#763.
# Real service.ixx invalidate_function + LoweringState on
# re-lower impact for affected closure_bridge entries retain/
# refresh shared_ptr<FlatAST> snapshot before Arena reset + bump
# bridge_epoch + notify GC to root the old AST temporarily if
# live closures reference it + evaluator_gc.cpp + gc_coordinator
# explicit root registration for active IRClosure
# shared_ptr<FlatAST> + on GC safepoint/compact validate Arena
# liveness or pin AST nodes + lowering_impl.cpp
# set_closure_bridge_ptr + apply_closure capture Arena epoch
# or generation + on apply verify AST nodes still valid (via
# marker or size check) or fallback safely + wire to
# MutationBoundaryGuard for cross-request safety +
# tests/test_prompt6_arena_ast_sharedptr_closure_bridge_gc_
# lifetime.cpp harness (quote/lambda define + heavy mutate:rebind
# + Arena reset + GC compact/steal + live closure apply → assert
# AST valid or safe fallback, no UAF/leak, roots correct,
# TSan/ASan clean) + SEVA arena/closure bridge demo + sync with
# bridge_epoch + mutation_epoch_ + Env version_ + extend
# EscapeAnalysis for AST node escape in bridge + CI gate + docs
# are all follow-up (each is a dedicated session in
# service.ixx + evaluator_gc.cpp + lowering_impl.cpp + new test
# + SEVA demo + docs).
aura_add_issue_test(test_issue_764)
aura_issue_test_link_llvm_jit(test_issue_764)
# Issue #799: DeadCoercionElimination + narrow_evidence CastOp elision.
# 4 CompilerMetrics atomics (dead_coercion_elision_*_total) +
# (query:dead-coercion-elision-stats, schema 799) + bump wiring in
# accumulate_coercion_pass_metrics / ir_executor / type_checker.
# 8 ACs: schema, 4 bump paths, DCE Rule 6 exercise, regression #687/#629.
# Non-duplicative with #796/#795/#794/#687.
aura_add_issue_test(test_dead_coercion_elision_narrow_evidence)
aura_issue_test_link_llvm_jit(test_dead_coercion_elision_narrow_evidence)
# Issue #765: Full DepEntry quote/lambda tracking + impact_scope
# propagation to bridge_epoch bump, EnvFrame version re-stamp
# and linear state refresh observability primitive — 4 atomics
# + 4 bump helpers + (query:incremental-quote-lambda-linear-
# stats, schema 765) primitive (5-entry hash: 4 fields + schema
# sentinel). Pairs with the existing #757 query:macro-hygiene-
# provenance-stats + #758 query:edsl-reflection-stats +
# #759 query:code-as-data-maturity-stats +
# #760 query:pattern-performance-stats +
# #761 query:mutate-batch-stats +
# #762 query:workspace-closedloop-orchestration-stats +
# #763 query:linear-ownership-gc-compiler-stats +
# #764 query:compiler-arena-closure-lifetime-stats 4-field
# hashes; #765 covers the *incremental compilation safety for
# quote/lambda/closure-heavy defines composite* specifically —
# DepEntry quote/lambda hit, bridge_epoch bump on impact, EnvFrame
# version refresh, linear state refreshed — as separate per-
# decision-point counters the Agent consumes to monitor fine-
# grained incremental compilation + ownership safety production-
# readiness.
# 5 ACs / 35 CHECKs: hash shape, fresh-service zero state, schema
# sentinel, bump-helper accessibility, regression of
# #735/#756/#757/#758/#759/#760/#761/#762/#763/#764.
# Real ir_cache_pure.ixx compute_dependencies + compute_impact_
# scope + service dep_graph_ DepEntry quote/lambda flag + impact_
# scope priority for closure_bridge/linear blocks + service.ixx
# invalidate_function + LoweringState bridge_epoch bump + EnvFrame
# version_ re-stamp + linear_ownership_state re-emit + DirtyAware
# Pass integration + lowering_impl.cpp Variable cache-hit + set_
# closure_bridge_ptr + emit paths linear_state propagation + bridge
# shared_ptr refresh + tests/test_prompt2_6_dep_quote_lambda_
# impact_linear_bridge_env.cpp harness (define with quote + lambda
# capturing linear + mutate inside body → impact_scope + partial
# re-lower + live closure apply → assert bridge/Env/linear fresh,
# no stale ownership/hygiene, metrics, TSan clean) + SEVA quote/
# lambda linear demo + sync epochs with mutation_epoch_ + wire to
# pass_manager DirtyAware + EscapeAnalysis for linear in quote
# contexts + CI gate + docs are all follow-up (each is a
# dedicated session in ir_cache_pure.ixx + service.ixx +
# lowering_impl.cpp + new test + SEVA demo + docs).
aura_add_issue_test(test_issue_765)
aura_issue_test_link_llvm_jit(test_issue_765)
# Issue #800: linear ownership post-mutate fidelity (Guard/steal/EnvFrame).
# 4 CompilerMetrics atomics (linear_postmutate_*_total) +
# (query:linear-postmutate-fidelity-stats, schema 800) + bump wiring in
# record_linear_gc_probe / restore_panic_checkpoint / resume_fiber_migration /
# materialize_call_env. 8 ACs. Non-duplicative with #793/#792/#784/#763.
aura_add_issue_test(test_linear_ownership_postmutate_guard_steal_envframe)
aura_issue_test_link_llvm_jit(test_linear_ownership_postmutate_guard_steal_envframe)
# Issue #798: ConstraintSystem incremental fidelity under Guard/steal.
# 4 CompilerMetrics atomics (type_incremental_*_total) +
# (query:type-incremental-fidelity-stats, schema 798) + bump wiring in
# mark_touched_on_delta / record_cross_delta_blame_hit /
# reverify_clean_constraints_for_touched. 8 ACs. Non-duplicative with
# #792/#793/#608/#509.
aura_add_issue_test(test_typechecker_incremental_guard_steal_fidelity)
aura_issue_test_link_llvm_jit(test_typechecker_incremental_guard_steal_fidelity)
# Issue #841: EDA production infrastructure observability + co-sim bridge.
# 4 CompilerMetrics atomics (eda_infra_*_total) +
# (query:eda-infra-stats, schema 841) + bump wiring in evaluator_primitives_eda
# + eda:ingest-result / eda:invoke-simulator primitives. 8 ACs.
# Non-duplicative with #499/#616/#837.
aura_add_issue_test(test_eda_production_infra)
aura_issue_test_link_llvm_jit(test_eda_production_infra)
# Issue #801: SV commercial emit fidelity + roundtrip stub + dirty re-emit.
# 4 CompilerMetrics atomics (sv_commercial_emit_*_total) +
# (query:sv-commercial-emit-fidelity-stats, schema 801) + bump wiring in
# maybe_hardware_feedback / mutate feedback / eda:validate-sv-emit-roundtrip.
# 8 ACs. Non-duplicative with #772/#748/#725.
aura_add_issue_test(test_sv_verification_commercial_emit_fidelity)
aura_issue_test_link_llvm_jit(test_sv_verification_commercial_emit_fidelity)
# Issue #802: SV verification feedback-driven self-evolution closed-loop.
# 4 CompilerMetrics atomics (sv_self_evo_*_total) +
# (query:sv-verification-self-evolution-stats, schema 802) + bump wiring in
# verify:parse-* / mutate:from-verification-feedback / eda:run-verification-feedback.
# 8 ACs. Non-duplicative with #774/#726/#748.
aura_add_issue_test(test_sv_verification_self_evolution_closed_loop_reliability)
aura_issue_test_link_llvm_jit(test_sv_verification_self_evolution_closed_loop_reliability)
# Issue #766: IR-SoA migration observability + DirtyAware incremental
# pipeline dashboard. 5 CompilerMetrics atomics (ir_soa_*_total) +
# (query:ir-soa-migration-stats, schema 766) + 5 bump helpers on
# Evaluator (bump_ir_soa_instructions_emitted /
# bump_ir_soa_dirty_block_skips / set_ir_soa_clean_block_hit_rate_pct /
# set_ir_soa_pmr_column_utilization_pct /
# bump_ir_soa_jit_codegen_time_ns). 5 ACs: hash shape, fresh-service
# zero state, schema sentinel, bump-helper accessibility, regression
# of #758/#763/#764. Non-duplicative with #729/#752/#765.
aura_add_issue_test(test_issue_766)
aura_issue_test_link_llvm_jit(test_issue_766)
# Issue #767: Arena Auto-Compact Policy + Live Defrag + Fiber/GC
# Safepoint Yield observability. 2 CompilerMetrics atomics
# (arena_auto_compact_fiber_yield_during_compact_total +
# arena_auto_compact_defrag_blocked_fibers_total) +
# (query:arena-auto-compact-defrag-fiber-stats, schema 767) +
# 2 bump helpers on Evaluator
# (bump_arena_auto_compact_fiber_yield_during_compact /
# bump_arena_auto_compact_defrag_blocked_fibers). 5 ACs: hash
# shape, fresh-service zero state, schema sentinel, bump-helper
# accessibility, regression of #685/#642/#766. Non-duplicative
# with #685/#642/#731 (parallel companion primitive with 2
# truly new counters + 4 reused arena stats).
aura_add_issue_test(test_issue_767)
aura_issue_test_link_llvm_jit(test_issue_767)
# Issue #768: Shape + Pass + Contracts hot-path observability.
# 5 CompilerMetrics atomics (shape_pass_*_total +
# shape_stability_transitions_total + jit_epoch_sync_hits_total +
# deopt_targeted_skips_total + concept_violations_caught_total)
# + (query:shape-pass-hotpath-stats, schema 768) + 5 bump helpers
# on Evaluator. 5 ACs: hash shape, fresh-service zero state,
# schema sentinel, bump-helper accessibility, regression of
# #570/#767/#766. Non-duplicative with #507/#570/#492/#494/#571
# /#744 (parallel companion primitive with 5 truly new counters).
aura_add_issue_test(test_issue_768)
aura_issue_test_link_llvm_jit(test_issue_768)
# Issue #769: DeadCoercionEliminationPass + evidence-based CastOp
# elision for zero-overhead narrowed paths — already-shipped
# verification (scope-limited close). The 3 body items ship on
# latest main via prior commits:
# 1. DCE Pass class + Rule 6 narrow_evidence path → #280/#538/#629
#    (pass_manager.ixx:1184-1473).
# 2. JIT hot path narrow_evidence CastOp shortcut → #538/#746
#    (aura_jit.cpp:1463).
# 3. Metrics primitive + schema sentinel → #629 (4 atomics) + #799
#    ((query:dead-coercion-elision-stats, schema 799)).
# 5 ACs: DCE Pass instantiable + JITFriendly concept + metrics
# primitive reachable + zerooverhead primitive regression +
# sibling observability (#766/#767/#768) regression. No new
# production code (all items ship) — verification test only.
aura_add_issue_test(test_issue_769)
aura_issue_test_link_llvm_jit(test_issue_769)
# Issue #770: solve_delta + reverify with subtree-aware dirty spans
# to eliminate truncation — already-shipped verification (scope-
# limited close). The 4 body items all ship on latest main via
# prior commits:
# 1. FlatAST subtree dirty descriptors (#411/#487) — ast.ixx:4363/5611
# 2. Dynamic reverify limit + Occurrence priority (#745 commit 3b61c5da
#    2026-07-08) — type_checker_impl.cpp
# 3. affected_subtree_from_mutation wired into infer_flat_partial
#    (#411/#487) — type_checker_impl.cpp:4951
# 4. Heavy nested mutate regression tests (#411/#745) — multiple tests
#    including test_typesystem_solve_delta_occurrence_priority_heavy_mutate
# Metrics surface for "100% conflict detection coverage" ships via
# 5 primitives: (#509/#573/#608/#628/#745/#798) all in
# kObservabilityStatsPrimitives.
# 5 ACs: FlatAST subtree API reachable + type-incremental-fidelity-stats
# schema 798 + constraint-reverify-occurrence-stats schema 745 + heavy
# nested mutate no truncation + sibling observability regression.
aura_add_issue_test(test_issue_770)
aura_issue_test_link_llvm_jit(test_issue_770)
# Issue #771: Strengthen OwnershipEnv + escape analysis integration
# with typed mutation + fiber scheduler — already-shipped
# verification (scope-limited close, mirror #770). The 4 body items
# all ship on latest main via prior commits:
# 1. OwnershipEnv mutation-aware flow tracking — #224/#283/#411/#487/#598
# 2. EscapeAnalysisPass (pass_manager.ixx:1803) +
#    EscapeAnalysisWrap (service.ixx:232, DirtyAwarePass static_assert at
#    line 299) — closure captures post-mutate
# 3. Ownership-specific tests — 8+ existing test binaries:
#    test_hardware_resource_linear_ownership (29/29),
#    test_linear_ownership_post_mutate_validation_runtime (16/16),
#    test_linear_ownership_postmutate_guard_steal_envframe (16/16),
#    test_linear_ownership_runtime_enforcement_post_mutate (14/14),
#    test_linear_ownership_occurrence_predicate_mutate (16/16),
#    test_linear_ownership_guardshape_post_mutate (15/15),
#    test_linear_ownership_incremental_post_mutate_task2 (19/19),
#    test_prompt6_linear_jit_l2_post_invalidate_arena_gc (14/14)
# 4. Runtime linear metadata checks (body says "Optional") — #638
#    (linear-ownership-safety-stats) + #800 (linear-postmutate-
#    fidelity-stats) + #763 (linear-ownership-gc-compiler-stats)
# Metrics surface for items 1-4 ships via 4 primitives (already in
# kObservabilityStatsPrimitives):
# - (query:linear-occurrence-mutate-stats, #747, schema 747)
# - (query:linear-postmutate-fidelity-stats, #800, schema 800)
# - (query:linear-ownership-runtime-stats, #598)
# - (query:linear-ownership-gc-compiler-stats, #763, schema 763)
# 5 ACs: EscapeAnalysis API + linear-occurrence/postmutate primitives +
# linear-ownership-runtime/gc-compiler primitives + OwnershipEnv
# regression + sibling observability (#770/#768/#767/#766).
aura_add_issue_test(test_issue_771)
aura_issue_test_link_llvm_jit(test_issue_771)
# Issue #772: Consolidated SV Verification EDSL + Backend Emit
# Fidelity + Roundtrip Validation + Dirty-Driven Incremental Re-emit
# Harness for Commercial Tool Interop (consolidates/refines
# #693/#724/#725/#726/#748). 5 CompilerMetrics atomics
# (sv_slo_*_total + sv_slo_reemit_latency_max_us) +
# (query:sv-closedloop-slo, schema 772) + 5 bump helpers on
# Evaluator (bump_sv_slo_emit_parse_success /
# bump_sv_slo_emit_parse_failure /
# bump_sv_slo_reemit_latency_max_us [compare-exchange high-water
# mark] / bump_sv_slo_reemit_hits / bump_sv_slo_breach). 5 ACs:
# hash shape, fresh-service zero state, schema sentinel, bump-
# helper accessibility + SLO status transitions (ok/warn/breach),
# regression of #748/#801/#802/#766. Non-duplicative with #748/
# #801/#802.
aura_add_issue_test(test_issue_772)
aura_issue_test_link_llvm_jit(test_issue_772)
# Issue #773: Workspace closed-loop fiber/multi-agent EDA
# verification observability. 3 NEW CompilerMetrics atomics
# (workspace_closedloop_shared_mutex_contention_ns_total +
# workspace_closedloop_multi_agent_edit_fidelity_pct +
# workspace_closedloop_stale_ref_prevented_eda_loops_total) +
# (query:workspace-closedloop-fiber-eda-stats, schema 773) +
# 3 bump helpers on Evaluator
# (bump_workspace_closedloop_shared_mutex_contention_ns /
# set_workspace_closedloop_multi_agent_edit_fidelity_pct /
# bump_workspace_closedloop_stale_ref_prevented_eda_loops).
# 5 ACs: hash shape, fresh-service zero state, schema sentinel,
# bump-helper accessibility, sibling observability regression
# of #762/#772/#798/#747. Non-duplicative with #762 (parallel
# companion primitive with 3 NEW dimensions + 3 reused).
aura_add_issue_test(test_issue_773)
aura_issue_test_link_llvm_jit(test_issue_773)
# Issue #774: Verification feedback-driven closed-loop
# self-evolution convergence rate observability. 0 NEW
# CompilerMetrics atomics (convergence_rate is derived at
# primitive-call time from existing #802 atomics) + 0 NEW
# bump helpers + (query:closed-loop-convergence-stats,
# schema 774) primitive returning 4 body-specified fields
# + schema sentinel (5-entry hash: convergence-rate +
# closed-loop-rounds + convergence-hits + feedback-mutate-
# rounds + schema). Reuses 3 atomics from #802 (closed-
# loop-rounds + convergence-hits) and 1 atomic from #726
# (feedback-mutate-rounds). 5 ACs: hash shape, fresh-
# service baseline 100.00%, schema sentinel, convergence
# rate derivation correctness, sibling observability
# regression of #726/#802/#772/#773. Non-duplicative with
# #726 (closed-loop-reliability-stats) and #802 (sv-
# verification-self-evolution-stats) — parallel companion
# primitive that adds the deployment-grade convergence_rate
# pct the body asks for.
aura_add_issue_test(test_issue_774)
aura_issue_test_link_llvm_jit(test_issue_774)
# Issue #775: Formal Primitives Extension Kit for AI Agent safe
# generation, registration, contract enforcement + auto-meta +
# test template observability. 0 NEW CompilerMetrics atomics
# (all 4 body-specified fields are derived from existing #633
# + #751 + #697 + #669 atomics) + 0 NEW bump helpers (AC4
# uses production-path (primitive:generate-skeleton) to bump
# the test_skeletons_generated counter) + (query:extension-
# kit-stats, schema 775) primitive returning 4 body-specified
# fields + schema sentinel (5-entry hash: extensions_registered
# + contract_violations_caught + meta_completeness_pct +
# test_skeletons_generated + schema). 5 ACs: hash shape,
# fresh-service zero state (meta_completeness_pct = 10000
# baseline when slot_count == 0), schema sentinel, production-
# path bump accessibility (call primitive:generate-skeleton
# → test_skeletons_generated grows), sibling observability
# regression of #697/#751/#669/#774. Non-duplicative with
# #697 (primitives-extension-stats, 8 registry-level counters)
# + #751 (primitives-contract-stats, 4 contract counters) +
# #669 (primitives-meta-stats, 4 meta-axis counters) —
# parallel companion primitive that aggregates the Agent-
# facing extension kit SLO composite the body asks for.
aura_add_issue_test(test_issue_775)
aura_issue_test_link_llvm_jit(test_issue_775)
# Issue #776: Integrated Primitives Hot-Path Benchmark Suite
# + Mutation/Fiber-Load Regression Gate with Quantitative SLOs
# observability. 0 NEW CompilerMetrics atomics (all 4 body-
# specified fields are derived from existing #614 + #751
# atomics; reuse the #614 stability_score formula) + 0 NEW
# bump helpers (AC4 uses existing bump_primitive_call_count +
# bump_pair_alloc_count from #441/#491 surface) +
# (query:primitives-hotpath-slo-stats, schema 776) primitive
# returning 4 body-specified fields + schema sentinel
# (5-entry hash: current-vs-baseline-pct + contract-
# violations + fastpath-hit-rate-pct + regression-flag +
# schema). 5 ACs: hash shape, fresh-service 100.00% baseline
# (no load → no regression), schema sentinel, derived
# SLO composite correctness (bump pair_alloc to trigger
# regression-flag; bump call_count to verify fastpath
# derivation), sibling observability regression of #614/
# #584 + #751 + #774 + #775. Non-duplicative with #614
# (primitives-hotpath-stats, 11 raw hot-path counters) and
# #751 (primitives-contract-stats, 4 contract counters) —
# parallel companion primitive that aggregates the
# primitives hot-path SLO composite the body asks for.
aura_add_issue_test(test_issue_776)
aura_issue_test_link_llvm_jit(test_issue_776)
# Issue #777: Consolidated EDA Infrastructure Primitives
# Production Readiness Roadmap + Milestone Tracker. 0 NEW
# CompilerMetrics atomics + 0 NEW bump helpers + 1 NEW
# primitive (query:eda-production-readiness, schema 777)
# returning 6 body-specified fields + schema sentinel
# (7-entry hash: m1/m2/m3/m4-completeness-pct derived from
# primitive lookup + blocking-issues-count + recommendation
# + schema). Uses ev.primitives_.lookup(name) to check
# existence of expected primitives for each milestone;
# completeness = (found / total) × 10000 via integer
# division. 5 ACs: hash shape, fresh-state completeness
# (all milestones should be 100% on production build with
# all related primitives shipped), schema sentinel,
# derived completeness correctness (verify each milestone
# computes the right percentage by checking individual
# primitive existence), sibling observability regression
# of #748/#772/#774/#775/#776. Non-duplicative with the
# individual EDA primitives — #777 is the FIRST
# observability surface that aggregates the EDA production
# readiness composite as a single deployment-grade
# production-readiness dashboard.
aura_add_issue_test(test_issue_777)
aura_issue_test_link_llvm_jit(test_issue_777)
# Issue #778: FFI call overhead observability for batch
# terminal output + rendering engine hot-path (P1 perf
# surface). 0 NEW CompilerMetrics atomics (uses existing
# coverage_counters_[8] which all FFI primitives already
# increment) + 0 NEW bump helpers (production-path
# bumps already wired up in ffi_primitives_impl.cpp) +
# 1 NEW public accessor on Evaluator (get_ffi_call_count)
# + (query:ffi-call-overhead-stats, schema 778) primitive
# returning 4 body-specified fields + schema sentinel
# (5-entry hash: ffi-call-count + batch-ffi-supported +
# terminal-batch-write-supported + recommendation +
# schema). 5 ACs: hash shape, fresh-zero (ffi-call-count
# == 0 because fresh service hasn't called any FFI
# primitive yet; both batch-ffi-supported and terminal-
# batch-write-supported == 0; recommendation == 3
# early-stage), schema sentinel, production-path bump
# accessibility (call c-func + c-alloc via EDSL → ffi-
# call-count grows), FFI call overhead benchmark (c-func
# lookup + c-alloc + c-opaque measured for ns/op under
# 1000 iterations), sibling observability regression
# of #777. Non-duplicative with #131 (FFI extraction)
# and #699 (ffi-calls-stats) — parallel companion
# primitive that exposes FFI call volume + batch
# primitive readiness signals.
aura_add_issue_test(test_issue_778)
aura_issue_test_link_llvm_jit(test_issue_778)
# Issue #779: Dirty region / delta rendering observability
# for terminal rendering engine (P2 perf surface). 0 NEW
# CompilerMetrics atomics + 0 NEW bump helpers (no existing
# dirty region counter on main; would be added with the
# Phase 2+ (terminal-dirty-region) primitive) + 1 NEW
# primitive (query:dirty-region-rendering-stats, schema
# 779) returning 4 body-specified fields + schema sentinel
# (5-entry hash: dirty-region-count hardcoded 0 +
# present-delta-supported hardcoded 0 + terminal-dirty-
# region-supported hardcoded 0 + recommendation + schema).
# 5 ACs: hash shape, fresh-zero (all 3 fields == 0,
# recommendation == 3 early-stage), schema sentinel,
# vector primitive ns/op benchmark (the existing vector
# primitives are the rendering hot path the body mentions;
# benchmark (make-vector 64) + 100x (vector-set! ...) to
# measure baseline ns/op for the full-redraw path the
# delta-rendering optimization would replace), sibling
# observability regression of #777/#778. Non-duplicative
# with the vector primitives in evaluator_primitives
# _vector.cpp — parallel companion observability surface
# for the deferred dirty region + present-delta work.
aura_add_issue_test(test_issue_779)
aura_issue_test_link_llvm_jit(test_issue_779)
# Issue #780: JIT / hot-update coverage observability
# for rendering hot paths (present/draw) (P2 perf
# surface). 0 NEW CompilerMetrics atomics (reuses
# existing #441 hotpath_eval_flat_calls +
# hotpath_lowering_calls) + 0 NEW bump helpers +
# (query:jit-rendering-coverage-stats, schema 780)
# primitive returning 4 body-specified fields + schema
# sentinel (6-entry hash: hotpath-eval-flat-calls +
# hotpath-lowering-calls + 2 hardcoded \"not yet\"
# flags + recommendation + schema). 5 ACs: hash shape,
# fresh-service zero state (both hotpath counters ==
# 0, both optimization flags == 0, recommendation ==
# 3 early-stage), schema sentinel, JIT path bump
# accessibility (call eval-flat / lowering to verify
# the hotpath counters grow; rendering-like hot path
# benchmark via vector primitives in a loop to give
# the Agent a baseline ns/op), sibling observability
# regression of #778 + #779. Non-duplicative with the
# existing JIT primitives (#427 + #720 + #746) —
# parallel companion observability surface for the
# deferred rendering-path JIT + hot-update work.
aura_add_issue_test(test_issue_780)
aura_issue_test_link_llvm_jit(test_issue_780)
# Issue #781: High-performance byte buffer + zero-copy
# primitives observability for framebuffer management
# (P2 perf surface). 0 NEW CompilerMetrics atomics
# (reuses existing #491 pair_alloc_total) + 0 NEW
# bump helpers (production paths already wire the
# counter) + (query:zero-copy-framebuffer-stats,
# schema 781) primitive returning 4 body-specified
# fields + schema sentinel (6-entry hash:
# pair-alloc-total reused + 3 hardcoded \"not yet\"
# flags + recommendation + schema). 5 ACs: hash shape,
# fresh-service zero state (pair-alloc-total == 0
# because no vector construction in setup; all 3
# support flags == 0; recommendation == 3 early-stage
# OR == 2 if pair-alloc-total > 0), schema sentinel,
# production-path bump accessibility (call
# (make-vector 64) in a loop to bump pair-alloc-total
# — the body says vector construction is wasteful
# for framebuffer; the zero-copy primitive would
# replace this), pair allocation ns/op benchmark
# ((make-vector 64) × 100 iters measured for baseline),
# sibling observability regression of #779 + #780.
# Non-duplicative with the existing memory + vector
# primitives — parallel companion observability
# surface for the deferred zero-copy byte-buffer +
# ANSI helper + memory profiling work.
aura_add_issue_test(test_issue_781)
aura_issue_test_link_llvm_jit(test_issue_781)
# Issue #782: Dedicated terminal rendering primitives
# module + profiling integration observability (P2
# infrastructure surface). 0 NEW CompilerMetrics
# atomics + 0 NEW bump helpers + 1 NEW primitive
# (query:terminal-rendering-module-stats, schema
# 782) returning 4 body-specified fields + schema
# sentinel (6-entry hash: core-primitive-count derived
# from live primitive lookup of [clear, draw-batch,
# present, dirty-tracking] + 3 hardcoded \"not yet\"
# flags + recommendation + schema). 5 ACs: hash shape,
# fresh-service zero state (core-primitive-count == 0
# because no evaluator_primitives_terminal.cpp exists
# on main; all 3 module flags == 0; recommendation == 3
# early-stage), schema sentinel, live lookup
# correctness (verify each expected core primitive is
# not yet registered; cross-check the live count
# matches independent primitive existence check),
# primitive existence sanity check (clear +
# draw-batch + present + dirty-tracking all NOT
# registered on fresh main), sibling observability
# regression of #780 + #781. Non-duplicative with
# the existing vector + memory + I/O primitives —
# parallel companion observability surface for the
# deferred terminal rendering module + profiling
# integration + example renderer.
aura_add_issue_test(test_issue_782)
aura_issue_test_link_llvm_jit(test_issue_782)
# Issue #783: P0 strict outermost MutationBoundary depth check +
# defer logic observability (Non-duplicative refinement of #755 /
# #730 / #451). Splits coarse steal_deferred_mutation_boundary_
# count_ into outermost-steal-total + inner-deferred-total +
# cross-fiber-safe-steal-total via 3 NEW Fiber atomics + 3 NEW
# C-linkage shims + 1 NEW (query:orchestration-steal-outermost-
# stats, schema 783) primitive. 5 ACs: hash shape, fresh-zero,
# schema sentinel, production-path bump helpers callable +
# activity reflects in primitive, sibling #781/#782/#779
# regression. WorkerThread::steal() wires the refined bump
# helpers into the existing is_at_mutation_boundary_safe()
# branch (outermost success → 2 bumps; inner defer → 1 bump).
aura_add_issue_test(test_issue_783)
aura_issue_test_link_llvm_jit(test_issue_783)
# Issue #784: P0 mandatory dual-path (bindings_ vs
# bindings_symid_) consistency enforcement + desync
# panic policy + GCEnvWalkFn stale integration under
# concurrent mutation/steal (Non-duplicative refinement
# of #756/#731/#647). 3 NEW atomics + 3 NEW bump
# helpers + 1 NEW (query:envframe-dualpath-mandatory-
# enforce-stats, schema 784) primitive. 5 ACs: hash
# shape, fresh-zero, schema sentinel, production-path
# bump helpers callable + activity reflects in
# primitive, sibling #756/#783 regression.
aura_add_issue_test(test_issue_784)
aura_issue_test_link_llvm_jit(test_issue_784)
# Issue #785: P0 AOT hot-update maturity (concurrent
# steal + grace period + EnvFrame version sync) for
# multi-agent / multi-fiber concurrent orchestration
# (Non-duplicative refinement of #732/#708/#590). 3 NEW
# atomics + 3 NEW bump helpers + 1 NEW
# (query:aot-concurrent-hotupdate-stats, schema 785)
# primitive. 5 ACs: hash shape, fresh-zero, schema
# sentinel, production-path bump helpers callable +
# activity reflects in primitive, sibling #708/#784
# regression.
aura_add_issue_test(test_issue_785)
aura_issue_test_link_llvm_jit(test_issue_785)
# Issue #1396: Scope-limited close for the 4 unwired AOT
# hot-reload bump helpers (#732 safe_boundary + #785
# concurrent_steal / grace_period / env_version_sync). Audit
# found 0 production call sites; the helpers are wrapped
# behind AOT_RELOAD_PHASE_2_PLUS so default builds stay honest
# (helpers absent) without losing the Phase 2+ design intent.
# Metric fields (observability_metrics.h:5232-5234, 5298) +
# query primitives stay UNCONDITIONAL. 4 ACs: #732 primitive
# stays queryable, #785 primitive stays queryable (schema=785),
# all 4 newly added metric fields read 0 in default build,
# AOT_RELOAD_PHASE_2_PLUS documented as the chosen gating macro.
aura_add_issue_test(test_issue_1396)
aura_issue_test_link_llvm_jit(test_issue_1396)

# Issue #1397: cells_/pairs/string_heap_ uniform alloc_storage_lock stress test
# (2 threads (map f lst) no torn + set-car! index stable + 4 threads x 100K cons ops no UB)
aura_add_issue_test(test_stress_alloc_storage_lock)
aura_issue_test_link_llvm_jit(test_stress_alloc_storage_lock)
# Issue #1398: hash-set! resize + hash-keys/hash-values iter invalidation contract test
# (hash-keys/hash-values/hash->alist return by value - snapshot stable after hash-set! triggers resize)
aura_add_issue_test(test_hash_iter_invalidation)
aura_issue_test_link_llvm_jit(test_hash_iter_invalidation)
# Issue #1397: extended coverage for ASTArena::request_defrag
# atomic CAS newly_set semantics - 50-cycle + multi-service
# independence + (arena:defrag-requested?) tracking. Pairs with
# test_issue_300.cpp AC5 (one-cycle basic) + closes the gap surfaced
# by #1428 (GitHub-numbered from #1397 request).
aura_add_issue_test(test_issue_1397)
aura_issue_test_link_llvm_jit(test_issue_1397)
# Issue #1399: set-car!/set-cdr! pair mutation race — idx resolution
# moved INSIDE alloc_storage_lock_ critical section (extends #1397's
# field-write guard to cover idx capture). 4 ACs: single-thread
# correctness + 2-thread set-vs-cons race + same-thread
# set-push-set sequence + 4-thread stress.
aura_add_issue_test(test_issue_1399)
aura_issue_test_link_llvm_jit(test_issue_1399)
# Issue #1400: bridge_epoch ↔ mutation_epoch sync invariant —
# verify public bridge_epoch() + bump_bridge_epoch() reflect the
# shared atomic (aliasing). Lock-in: invalidate_function now calls
# bump_bridge_epoch() instead of mutation_epoch_.fetch_add(1, ...)
# so a future decoupling fails to compile at the call site.
aura_add_issue_test(test_issue_1400)
aura_issue_test_link_llvm_jit(test_issue_1400)
# Issue #1401: load_module_file ↔ compact_env_frames() mutex interlock.
# Evaluator::compact_env_frames_lock_ serializes both call sites
# (canonical order: interlock → env_frames_mtx_ | workspace_mtx_).
# Test exercises direct Evaluator method calls (bypasses pre-existing
# cs.eval concurrent crash from #1399 honest gap).
aura_add_issue_test(test_issue_1401)
aura_issue_test_link_llvm_jit(test_issue_1401)
# Issue #1402: kPrimSecPrivileged capability gate contract test.
# Verifies that Privileged primitives (security:*, fiber:spawn) return
# merr in sandboxed mode without the required capability, and succeed
# after granting kCapWildcard. Locks the enforcement invariant.
aura_add_issue_test(test_issue_1402)
aura_issue_test_link_llvm_jit(test_issue_1402)
# Issue #1404: restamp_yield_checkpoint_top fail-loud contract.
# Source change: function now returns bool (true = mismatch detected,
# no restamp; false = OK, restamped). 3 callers updated to capture
# the return. Contract test verifies basic structure + that the
# diagnostic counters remain accessible. Full integration test
# (trigger real mismatch via fiber:spawn + concurrent arena:defrag)
# is documented as follow-up — blocked by the pre-existing cs.eval
# concurrent crash documented in #1399's honest gap.
aura_add_issue_test(test_issue_1404)
aura_issue_test_link_llvm_jit(test_issue_1404)
# Issue #1405: workspace_flat_ generation counter (Option 1 — infra only).
# Source change: update_shared_tree_root now bumps workspace_flat_->generation.
# Fiber-side check + merr (full Option 2 per body) is documented follow-up.
aura_add_issue_test(test_issue_1405)
aura_issue_test_link_llvm_jit(test_issue_1405)
# Issue #1433: (engine:metrics) facade — nested CompilerMetrics groups +
# :prefix / :group + golden hash shape (≥200 fields).
aura_add_issue_test(test_engine_metrics_facade)
aura_issue_test_link_llvm_jit(test_engine_metrics_facade)
# Issue #1434 stretch: 20× direct stats vs 1× facade wall time (soft bar).
aura_add_issue_test(test_stats_facade_bench)
aura_issue_test_link_llvm_jit(test_stats_facade_bench)
# Issue #1435: (query :op) unified dispatcher for core structural ops.
aura_add_issue_test(test_query_dispatch)
aura_issue_test_link_llvm_jit(test_query_dispatch)
# Issue #1436: (mutate :op) unified dispatcher for core mutate ops.
aura_add_issue_test(test_mutate_dispatch)
aura_issue_test_link_llvm_jit(test_mutate_dispatch)
# Issue #1437: (workspace :op) unified dispatcher.
aura_add_issue_test(test_workspace_dispatch)
aura_issue_test_link_llvm_jit(test_workspace_dispatch)
# Issue #1406: propagate_cow_pins_after_clone Option 2 — bounded retention
# cap (kMaxPinnedBoundaryRefs=4096, drop oldest) + version-check retry
# using cow_boundary_pins_total_ as the version counter. Unpin-on-merge
# deferred (no explicit layer-merge/finalize path; separate issue).
aura_add_issue_test(test_issue_1406)
aura_issue_test_link_llvm_jit(test_issue_1406)
# Issue #1407 R1: ConstraintSolver engine-level cache +
# typed_mutation epoch bump. Ships cs_cache_ (NodeId → {epoch,
# node-shape-hash, SolveResult, TypeId}) on TypeChecker +
# hash_node_shape fingerprint + bump_bridge_epoch() in the
# typed_mutate success path + 2 metrics
# (cs_cache_lookups/hits + typed_mutate_epoch_bumps). 5 ACs:
# cache hit/miss/hash/epoch-bump. 2 follow-ups: per-node
# re-inference integration, full SolveResult in TypeCheckResult.
aura_add_issue_test(test_issue_1407_constraint_solver_cache)
# Issue #1408: R2 Composite typed_mutation transaction + rollback.
# TypedTransactionGuard RAII + typed_mutate_atomic(span<string_view>)
# wraps N typed_mutate calls in one transaction — all-or-nothing via
# workspace_flat_->rollback_since(snapshot_id) on guard destruction.
# 4 ACs: happy/abort/empty/snapshot. EDSL primitive (typed-mutate-atomic)
# is a follow-up (typed-mutate itself is tree-walker-dispatched).
aura_add_issue_test(test_composite_typed_mutate)
# Issue #1408: TypedTransactionGuard test exercises the full
# CompilerService::eval path, which references the JIT runtime
# (AuraJIT::invalidate / set_string_pool / compile / register_function /
# get_function_ptr + the AOT emit_native_object / run_escape_analysis
# shims). These live in aura_jit_test_objects with AURA_HAVE_LLVM=1
# properly defined. Use the existing helper that wires LLVM includes +
# libs (same pattern as test_issue_1406 / test_ir / test_spec_jit).
aura_issue_test_link_llvm_jit(test_composite_typed_mutate)
# Issue #1441: try_rollback_rebind_op — variable-state rollback for
# mutate:rebind (completes #1408 atomic-batch rebind contract).
aura_add_issue_test(test_issue_1408_followup_rebind_rollback)
aura_issue_test_link_llvm_jit(test_issue_1408_followup_rebind_rollback)
# Issue #1442: EDSL (typed-mutate-atomic) — Aura surface for
# CompilerService::typed_mutate_atomic (#1408 follow-up).
aura_add_issue_test(test_issue_1408_followup_edsl)
aura_issue_test_link_llvm_jit(test_issue_1408_followup_edsl)
# Issue #1410: Linear ∩ Refinement type-driven discovery in
# validate_ownership_full (src/compiler/type_checker_impl.cpp:5387+).
# Production code already has the type-driven path (uses reg.linear_of
# on flat.type_id of the let-value child); this test exercises both
# paths (syntactic Linear wrapper + type-driven linear-registered
# type) and defense-in-depth. 3 ACs: type-driven only, syntactic
# regression, both-paths fire independently.
aura_add_issue_test(test_issue_1410)
# Issue #1417: Linear ∩ Refinement type-driven discovery (full AC
# surface). Extends #1387/#1410 with: move→no-leak, TypeAnnotation
# +drop→no-leak, Let-node type_id discovery, TypeAnnotation name
# lookup, defense-in-depth, and #117 syntactic baseline. Production
# changes in validate_ownership_full discovery (type_checker_impl).
aura_add_issue_test(test_issue_1417)
# Issue #1418: DeadCoercionEliminationPass wire-up to run_pipeline
# packs that previously omitted it (eval_ir / exec_jit / hot-swap
# in service.ixx). Default eval() + run_coercion_elim_on_function
# already ran DCE; this closes the gap so dead CastOps cannot
# accumulate on IR-direct / JIT / evolve! paths. Test: synthetic
# IR with 5 CastOps (2 identity → elided, 3 necessary → kept) +
# run_pipeline Pass-concept invocation.
aura_add_issue_test(test_issue_1418)
# Issue #1419: Compound provenance wire-up on MutationRecord
# (follow-up to #1412 field addition). FlatAST stamps
# author_fingerprint / parent_mutation_id / composite_transaction_id
# into every add_mutation; TypedTransactionGuard opens composite
# tx + parent chain; Evaluator agent fingerprint; query:mutation-
# provenance + MutationLogEntry surface. 6 ACs: stamp, defaults,
# shared composite, parent chain, create_record params, accessors.
aura_add_issue_test(test_issue_1419)
# Issue #1425: DeadCoercion AST-level identity elision at
# apply_coercion_map (skip CoercionNode when child type_id already
# matches target) + IR DCE pipeline contract (complements #1418).
# Metrics: dead_coercion_eliminated_total bumped from TypeCheckWrap
# and post-mutate typecheck paths. 4 ACs: 2-of-5 AST elision,
# no false elision, IR 2-of-5, run_pipeline DCE.
aura_add_issue_test(test_issue_1425)
# Issue #1412: Compound provenance on MutationRecord (author_fingerprint
# + parent_mutation_id + composite_transaction_id). Production struct
# change in src/core/mutation.ixx adds 3 fields (default 0 = system for
# backward compat); this test exercises brace-init, defaults, post-construction
# mutation, and that existing add_mutation call sites still work without
# setting provenance. 5 ACs: struct has fields, defaults=0, brace-init
# preserves, add_mutation backward-compat, set after construction.
# Wiring the fields at typed_mutate sites is a follow-up issue.
aura_add_issue_test(test_mutation_provenance)
# Issue #1413: True bidirectional type checking (Synth + Check) —
# annotation mismatch detection at compile time. Production
# bidirectional check-mode plumbing is already wired in
# type_checker_impl.cpp:3087+ (Issue #384 first slice) +
# check_flat_call/lambda at 4606/4641. This test exercises the
# full CompilerService pipeline (cs.eval) to verify annotation
# mismatch produces TypeError. 4 ACs: matches pass, mismatches
# reject, bidirectional body check, backward compat.
aura_add_issue_test(test_bidirectional_annotation)
# Reusable invariant (per #1408/#1410/#1411): any test that exercises
# full CompilerService::eval path needs this helper. Wires
# aura_jit_test_objects + LLVM libs + AURA_HAVE_LLVM=1 PUBLIC.
# Without it: JIT-runtime + AOT-shim + AuraJIT-method triangle of
# undefined references (g_tl_arena, tl_arena_push/pop,
# AuraJIT::unhandled_opcode_count_for_function, aura_set_aot_metrics, etc).
aura_issue_test_link_llvm_jit(test_bidirectional_annotation)
# Issue #1420: (compile:bidirectional-stats) EDSL primitive +
# engine:metrics facade wiring (4 new compile_bidirectional_*
# atomic fields land in :compile group via prefix rule).
# Production code: src/compiler/observability_metrics.h
# (4 atomic fields), src/compiler/type_checker_impl.cpp:4590
# (counter increments at check_flat_call pass/coerce/fail
# paths), src/compiler/evaluator.ixx (get_bidirectional_stats_fn_
# field), src/compiler/service.ixx (hook +
# bidirectional_mode() accessor), src/compiler/evaluator_primitives_compile_07.cpp
# (primitive in p63). Tests direct CompilerService::eval,
# requires aura_issue_test_link_llvm_jit per #1408/#1410/#1411
# invariant.
aura_add_issue_test(test_bidirectional_stats)
aura_issue_test_link_llvm_jit(test_bidirectional_stats)
# Issue #1416: Runtime GuardShape enforcement — kPrimSec* capability
# gate at primitive dispatch site. Production code lives in
# src/compiler/security_capabilities.h (new kCapSandbox constant),
# src/compiler/primitives_detail.h (DEFINE_PRIMITIVE_META_SECURE macro
# taking 7th SECURITY param), src/compiler/evaluator.ixx
# (invoke_prim_with_telemetry capability gate at the dispatch site
# using name → meta → sec_tier lookup), and src/compiler/evaluator_ctor.cpp
# (backfill_capability_tiers() tier-assigning the 7 Part 4 #1396 EDSL
# escape hatches to kPrimSecPrivileged). This test exercises direct
# CompilerService::eval calls, so aura_issue_test_link_llvm_jit is
# required (same reusable invariant from #1408/#1410/#1411).
aura_add_issue_test(test_capability_gating)
aura_issue_test_link_llvm_jit(test_capability_gating)
# Issue #1448: SlimSurface infrastructure — PrimMeta.deprecated dispatch
# counter, (stats:count)/(stats:list)/(engine:metrics) catalog ACs.
# Script-side freeze + --strict live in check_primitive_surface.py +
# tests/test_primitive_surface_gate.py (./build.py gate).
aura_add_issue_test(test_primitives_surface_convergence)
aura_issue_test_link_llvm_jit(test_primitives_surface_convergence)
# Issue #1450 / Epic #1449 Phase 1: Observability Facade —
# stats:get / stats:prefix / engine:surface + residual public
# *-stats deprecation marks.
aura_add_issue_test(test_issue_1450)
aura_issue_test_link_llvm_jit(test_issue_1450)
# Issue #1451: Primitives Governance Policy + (primitive:validate-new).
aura_add_issue_test(test_issue_1451)
aura_issue_test_link_llvm_jit(test_issue_1451)
# Issue #1466: hot-path Contracts + consteval invariant coverage.
# Pure C++ (no CompilerService / LLVM JIT); tests consteval count + counter
# flags + cross-source consistency between cpp26_contract_stats.h and
# cxx26_invariants.ixx.
aura_add_issue_test(test_issue_1466)
# Issue #1467 Phase 1: live-object-moving defrag foundation (mark-only).
# Pure C++ (no CompilerService / LLVM JIT); tests live_defrag() method
# skeleton + counter bumps + shape invalidation hook integration.
# Full copy + pointer remapping is Phase 2/3 follow-up.
aura_add_issue_test(test_issue_1467)
# Issue #1468: ShapeProfiler history/dominant/stability tuning +
# deopt-storm protection + AI workload metrics.
# Pure C++ (no CompilerService / LLVM JIT); tests Preset application,
# deopt-storm detection, 4 metric accessors, invalidate_all counter,
# history_hit counter, SpecJITController coordination hook.
aura_add_issue_test(test_issue_1468)
# Issue #1462: Agent Migration Guide + compat shims for demoted
# query:* primitives. Integration test (uses CompilerService to load
# lib/std/compat.aura and verify shim wrappers route to the new
# stdlib/primitive equivalents).
aura_add_issue_test(test_issue_1462)
aura_issue_test_link_llvm_jit(test_issue_1462)
# Issue #1469: FlatAST generation wrap-around handling (P0 EDSL Review).
# Pure C++ (no CompilerService / LLVM JIT); tests bump_generation_count
# observability + 100k mutate cycle + StableNodeRef capture +
# code-presence checks for wrap_epoch_ + generation_wrap_count_ +
# maybe_auto_restamp_on_wrap wire points.
aura_add_issue_test(test_issue_1469)
# Issue #1460: std/agent EDSL closed-loop + decision metrics
aura_add_issue_test(test_issue_1460)
aura_issue_test_link_llvm_jit(test_issue_1460)
# Issue #1471: Deepen SyntaxMarker::MacroIntroduced hygiene in
# query:pattern and matcher. Pure C++ (no CompilerService / LLVM JIT);
# tests matcher hygiene counters + new detailed-list accessor +
# code-presence checks for hygiene landmarks.
aura_add_issue_test(test_issue_1471)
# Issue #1461: Agent Decision Metrics contract liveness + inject/react
aura_add_issue_test(test_issue_1461)
aura_issue_test_link_llvm_jit(test_issue_1461)
# Issue #1500: Full StableNodeRef provenance (cow_epoch/wrap_epoch/
# fiber_id/workspace_id) auto-refresh + is_valid enforcement +
# Guard dtor / steal batch restamp of pinned refs.
aura_add_issue_test(test_issue_1500)
aura_issue_test_link_llvm_jit(test_issue_1500)
# Issue #1508: JIT aura_closure_call dual check (bridge_epoch +
# defuse/env_frame) + interpreter-deopt fallback (no UAF on stale).
aura_add_issue_test(test_issue_1508)
aura_issue_test_link_llvm_jit(test_issue_1508)
# Issue #1509: multi-fiber concurrent mutate→call-stale closure
# stress + ASan/TSan-oriented safe-fallback validation.
aura_add_issue_test(test_issue_1509)
aura_issue_test_link_llvm_jit(test_issue_1509)
# Issue #1510: materialize_call_env + compact_env_frames cooperation —
# atomic env_id rewrite + defuse/bridge epoch bump under interlock.
aura_add_issue_test(test_issue_1510)
aura_issue_test_link_llvm_jit(test_issue_1510)
# Issue #1511: dual check on closure_bridge_ callback entry
# (bridge_epoch + EnvFrame version_) + EnvFrame re-stamp + metrics.
aura_add_issue_test(test_issue_1511)
aura_issue_test_link_llvm_jit(test_issue_1511)
# Issue #1512: JIT opcode coverage tracking + IRInterpreter
# consistency compare API (strict mode + fuzz harness).
aura_add_issue_test(test_issue_1512)
aura_issue_test_link_llvm_jit(test_issue_1512)
# Issue #1658: full opcode coverage + strict consistency default mandate.
aura_add_issue_test(test_jit_full_opcode_coverage_1658)
aura_issue_test_link_llvm_jit(test_jit_full_opcode_coverage_1658)
# Issue #1513: invalidate → live IRClosure/EnvFrame safety closed
# loop (bridge_epoch + env_version dual-check + expire on invalidate).
aura_add_issue_test(test_issue_1513)
aura_issue_test_link_llvm_jit(test_issue_1513)
# Issue #1514: re-lower granularity (nested-lambda targeted cascade)
# + JIT partial_recompile sync.
aura_add_issue_test(test_issue_1514)
aura_issue_test_link_llvm_jit(test_issue_1514)
# Issue #1515: linear ownership runtime checks + GC root coordination
# (linear_ownership_state state machine + sync_linear_roots_and_bridge_epoch).
aura_add_issue_test(test_issue_1515)
aura_issue_test_link_llvm_jit(test_issue_1515)
# Issue #1516: exception EH coverage + per-function AOT productionization.
aura_add_issue_test(test_issue_1516)
aura_issue_test_link_llvm_jit(test_issue_1516)
# Issue #1517: SoAView concept enforcement + EDSL hot-path migration.
aura_add_issue_test(test_issue_1517)
aura_issue_test_link_llvm_jit(test_issue_1517)
# Issue #1518: arena live compact + freelist relocate + Shape/JIT deopt coord.
aura_add_issue_test(test_issue_1518)
# Pure arena unit test — no LLVM JIT required.
# Issue #1519: deepen C++26 Contracts + consteval on Arena/Value/Shape/dirty.
aura_add_issue_test(test_issue_1519)
aura_issue_test_link_llvm_jit(test_issue_1519)
# Issue #1520: children_ columnar SoA + region dense lookup (EDSL hot path).
aura_add_issue_test(test_issue_1520)
aura_issue_test_link_llvm_jit(test_issue_1520)
# Issue #1521: ShapeProfiler versioning + Arena compact synergy + deopt hooks.
aura_add_issue_test(test_issue_1521)
aura_issue_test_link_llvm_jit(test_issue_1521)
# Issue #1522: JIT fn_trackers_ batch_deopt notify on bridge epoch bump.
aura_add_issue_test(test_issue_1522)
aura_issue_test_link_llvm_jit(test_issue_1522)
# Issue #1523: lock-order audit for invalidate paths + metrics.
aura_add_issue_test(test_issue_1523)
aura_issue_test_link_llvm_jit(test_issue_1523)
# Issue #1524: typed_mutate dual-epoch via atomic_bump_epochs_and_stamp_bridge.
aura_add_issue_test(test_issue_1524)
aura_issue_test_link_llvm_jit(test_issue_1524)
# Issue #1525: multi-fiber concurrent mutate + stale eval + JIT dual-check stress.
aura_add_issue_test(test_issue_1525)
aura_issue_test_link_llvm_jit(test_issue_1525)
# Issue #1526: compact_env_frames dual-epoch + bridge restamp cooperation.
aura_add_issue_test(test_issue_1526)
aura_issue_test_link_llvm_jit(test_issue_1526)
# Issue #1527: fix aura_jit_test_objects link; re-enable #1477 dual-epoch fence test.
aura_add_issue_test(test_issue_1477)
aura_issue_test_link_llvm_jit(test_issue_1477)
# Issue #1528: O(delta) re-inference — solve_delta local worklist prune +
# type_dep_graph expand + incremental_reinfer metrics.
aura_add_issue_test(test_issue_1528)
aura_issue_test_link_llvm_jit(test_issue_1528)
# Issue #1529: complete blame/provenance chain for occurrence narrowing
# after delta conflicts (mutation_id + predicate + affected NodeId).
aura_add_issue_test(test_issue_1529)
# Pure ConstraintSystem unit test — no JIT link required.
# Issue #1530: expand TypePropagationPass opcode coverage + CastOp
# elision win-rate metrics (pure IR unit + CompilerService metrics).
aura_add_issue_test(test_issue_1530)
aura_issue_test_link_llvm_jit(test_issue_1530)
# Issue #1531: OwnershipEnv + escape re-analysis for linear safety
# under typed_mutate (AST escape + IR EscapeAnalysisPass metrics).
aura_add_issue_test(test_issue_1531)
aura_issue_test_link_llvm_jit(test_issue_1531)
# Issue #1532: ADT match exhaustiveness structured API + progressive
# warn/error (strict_) + checked/diagnostics metrics.
aura_add_issue_test(test_issue_1532)
aura_issue_test_link_llvm_jit(test_issue_1532)
# Issue #1534: wire is_fn_epoch_stale into OpGuardShape dual-epoch fence
# (capture_fn_epoch at compile + runtime aura_jit_guard_shape_epoch_check).
aura_add_issue_test(test_issue_1534)
aura_issue_test_link_llvm_jit(test_issue_1534)
# Issue #1535: Linear* dual-epoch fence (is_fn_epoch_stale + is_env_frame_stale
# on Move/Borrow/Drop + safety_probe; deopt on mid-op mutate).
aura_add_issue_test(test_issue_1535)
aura_issue_test_link_llvm_jit(test_issue_1535)
# Issue #1536: walk_active_closures bulk epoch refresh + deopt-on-next-apply
# after mark_define_dirty / invalidate_function.
aura_add_issue_test(test_issue_1536)
aura_issue_test_link_llvm_jit(test_issue_1536)
# Issue #1537: LLVM IR-level dual-epoch check at every JIT Apply prologue.
aura_add_issue_test(test_issue_1537)
aura_issue_test_link_llvm_jit(test_issue_1537)
# Issue #1538: combined post_mutation_invariant_check + linear_post_mutate_enforce.
aura_add_issue_test(test_issue_1538)
aura_issue_test_link_llvm_jit(test_issue_1538)
# Issue #1539: EnvFrame bindings_linear_ownership_state_ SoA + real enforce.
aura_add_issue_test(test_issue_1539)
aura_issue_test_link_llvm_jit(test_issue_1539)
# Issue #1540: wire linear_post_mutate_enforce into JIT linear_safety_probe / Apply.
aura_add_issue_test(test_issue_1540)
aura_issue_test_link_llvm_jit(test_issue_1540)
# Issue #1542: wire linear_post_mutate_enforce into materialize_call_env entry.
aura_add_issue_test(test_issue_1542)
aura_issue_test_link_llvm_jit(test_issue_1542)
# Issue #1543: GC root registration consistency audit + docs + query log.
aura_add_issue_test(test_issue_1543)
aura_issue_test_link_llvm_jit(test_issue_1543)
# Issue #1544: linear post-mutate stress (AI mutate + GC + fiber steal).
aura_add_issue_test(test_issue_1544)
aura_issue_test_link_llvm_jit(test_issue_1544)
set_tests_properties(test_issue_1544_verification PROPERTIES TIMEOUT 60)
# Issue #1545: live-closure linear capture scan on invalidate/compact/JIT.
aura_add_issue_test(test_issue_1545)
aura_issue_test_link_llvm_jit(test_issue_1545)
# Issue #1486: linear ownership closed-loop parent (entry + boundary + GC + metrics).
aura_add_issue_test(test_issue_1486)
aura_issue_test_link_llvm_jit(test_issue_1486)
# Issue #1487: ResourceQuota Arena + MutationBoundary closed-loop parent.
aura_add_issue_test(test_issue_1487)
aura_issue_test_link_llvm_jit(test_issue_1487)
# Issue #1488: remove dead string_heap_ push pollution (adaptive-stats + scan).
aura_add_issue_test(test_issue_1488)
aura_issue_test_link_llvm_jit(test_issue_1488)
# Issue #1489: PanicCheckpoint ↔ scheduler GC defer closed-loop.
aura_add_issue_test(test_issue_1489)
aura_issue_test_link_llvm_jit(test_issue_1489)
# Issue #1490: post-steal EnvFrame/bridge_epoch refresh + linear re-pin.
aura_add_issue_test(test_issue_1490)
aura_issue_test_link_llvm_jit(test_issue_1490)
# Issue #1491: apply_closure + JIT dual-epoch force-check closed-loop.
aura_add_issue_test(test_issue_1491)
aura_issue_test_link_llvm_jit(test_issue_1491)
# Issue #1492: nested MutationBoundary steal defer + starvation mitigation.
aura_add_issue_test(test_issue_1492)
# Issue #1633: nested MutationBoundary steal defer + starvation mitigation mandate
aura_add_issue_test(test_inner_steal_starvation_1633)
aura_issue_test_link_llvm_jit(test_inner_steal_starvation_1633)
aura_issue_test_link_llvm_jit(test_issue_1492)
# Issue #1493: per-fiber mutation depth / adaptive safepoint observability.
aura_add_issue_test(test_issue_1493)
aura_issue_test_link_llvm_jit(test_issue_1493)
# Issue #1494: linear ownership + GC root on invalidate/mutate closed-loop.
aura_add_issue_test(test_issue_1494)
aura_issue_test_link_llvm_jit(test_issue_1494)
# Issue #1495: wire per-block dirty bitmask into partial re-lower (eval/mutate).
aura_add_issue_test(test_issue_1495)
aura_issue_test_link_llvm_jit(test_issue_1495)
# Issue #1574: wire IRCacheEntry dirty bitmask into optimization pass pipeline.
aura_add_issue_test(test_issue_1574)
aura_issue_test_link_llvm_jit(test_issue_1574)
# Issue #1575: DirtyPropagation BFS cascade + IR bridges + DirtyAware integration.
aura_add_issue_test(test_dirty_propagation_cascade)
# Issue #1576: concrete optimization passes + C++26 contracts registry.
aura_add_issue_test(test_optimization_passes_contracts)
aura_issue_test_link_llvm_jit(test_optimization_passes_contracts)
# Issue #1577: centralized Pass concepts (concept_constraints.ixx).
aura_add_issue_test(test_concept_constraints)
aura_issue_test_link_llvm_jit(test_concept_constraints)
# Issue #1578: RenderPass dirty-aware + shape_stable incremental render.
aura_add_issue_test(test_render_pass_incremental)
aura_issue_test_link_llvm_jit(test_render_pass_incremental)
# Issue #1579: dedicated ResourceQuota module + process fiber enforcement.
aura_add_issue_test(test_resource_quota_module)
aura_issue_test_link_llvm_jit(test_resource_quota_module)
# Issue #1580: fiber resume post-steal EnvFrame/StableNodeRef/panic refresh.
aura_add_issue_test(test_fiber_resume_post_steal_refresh)
aura_issue_test_link_llvm_jit(test_fiber_resume_post_steal_refresh)
# Issue #1581: scheduler GC defer + pending PanicCheckpoint steal/GC stress.
aura_add_issue_test(test_scheduler_gc_defer_pending_panic_steal)
aura_issue_test_link_llvm_jit(test_scheduler_gc_defer_pending_panic_steal)
# Issue #1582: policy-driven SelfHealing engine + graceful drain + quota/panic.
aura_add_issue_test(test_self_heal_policy_engine)
aura_issue_test_link_llvm_jit(test_self_heal_policy_engine)
# Issue #1583: recovery latency stall budget + longrunning-recovery-stats.
aura_add_issue_test(test_longrunning_recovery_latency)
aura_issue_test_link_llvm_jit(test_longrunning_recovery_latency)
# Issue #1584: Fiber::join structured multi-fiber coordination.
aura_add_issue_test(test_fiber_join)
aura_issue_test_link_llvm_jit(test_fiber_join)
# Issue #1585: MultiFiberMailbox multi-attach / broadcast / backpressure.
aura_add_issue_test(test_multi_fiber_mailbox)
aura_issue_test_link_llvm_jit(test_multi_fiber_mailbox)
# Issue #1586: parallel_orch / parallel_intend spawn+join+aggregate.
aura_add_issue_test(test_parallel_orch)
aura_issue_test_link_llvm_jit(test_parallel_orch)
# Issue #1587: Aura (parallel-intend) agent primitive surface.
aura_add_issue_test(test_parallel_intend_primitive)
aura_issue_test_link_llvm_jit(test_parallel_intend_primitive)
# Issue #1588: unified src/orch module + agent_spawn abstraction.
aura_add_issue_test(test_orch_agent_spawn)
aura_issue_test_link_llvm_jit(test_orch_agent_spawn)
# Issue #1589: TypedMutationAudit production trail + strategy gate.
aura_add_issue_test(test_typed_mutation_audit)
aura_issue_test_link_llvm_jit(test_typed_mutation_audit)
# Issue #1590: ResourceQuota hot-path (arena + Guard + query schema 1590).
aura_add_issue_test(test_resource_quota_hotpath)
aura_issue_test_link_llvm_jit(test_resource_quota_hotpath)
# Issue #1591: mutation-boundary safe-yield + fairness stats (schema 1591).
aura_add_issue_test(test_mutation_boundary_fairness_1591)
aura_issue_test_link_llvm_jit(test_mutation_boundary_fairness_1591)
# Issue #1635: first-class safe yield + per-fiber depth orchestration mandate
aura_add_issue_test(test_safe_yield_orchestration_1635)
aura_issue_test_link_llvm_jit(test_safe_yield_orchestration_1635)
# Issue #1660: unify apply_closure dual-path + EnvFrame SoA staleness mandate.
aura_add_issue_test(test_apply_closure_envframe_soa_1660)
aura_issue_test_link_llvm_jit(test_apply_closure_envframe_soa_1660)
# Issue #1661: shape + linear collaborative pass mandate.
aura_add_issue_test(test_shape_linear_collaborative_pass_1661)
aura_issue_test_link_llvm_jit(test_shape_linear_collaborative_pass_1661)
# Issue #1662: ~Evaluator clears arena_owner (UAF fix).
aura_add_issue_test(test_arena_owner_dtor_clear_1662)
aura_issue_test_link_llvm_jit(test_arena_owner_dtor_clear_1662)
# Issue #1663: set_arena owner atomic vs concurrent allocate_raw.
aura_add_issue_test(test_set_arena_atomic_owner_1663)
aura_issue_test_link_llvm_jit(test_set_arena_atomic_owner_1663)
# Issue #1664: closures→env dual-lock order (probe vs scan).
aura_add_issue_test(test_lock_order_closures_env_1664)
aura_issue_test_link_llvm_jit(test_lock_order_closures_env_1664)
# Issue #1665: scan_live_closures skips tombstoned / erased TW closures.
aura_add_issue_test(test_scan_skip_freed_closures_1665)
aura_issue_test_link_llvm_jit(test_scan_skip_freed_closures_1665)
# Issue #1666: compact hook replace/chain (no silent overwrite).
aura_add_issue_test(test_compact_hook_chain_1666)
aura_issue_test_link_llvm_jit(test_compact_hook_chain_1666)
# Issue #1667: ~Evaluator releases PanicCheckpoint GC defer (no depth leak).
aura_add_issue_test(test_gc_defer_dtor_release_1667)
aura_issue_test_link_llvm_jit(test_gc_defer_dtor_release_1667)
# Issue #1670: observability peel tier table (data-driven register_*_all).
aura_add_issue_test(test_observability_tier_table_1670)
aura_issue_test_link_llvm_jit(test_observability_tier_table_1670)
# Issue #1671: lookup_stats_impl heterogeneous find (no per-call string alloc).
aura_add_issue_test(test_lookup_stats_impl_heterogeneous_1671)
aura_issue_test_link_llvm_jit(test_lookup_stats_impl_heterogeneous_1671)
# Issue #1672: stats catalog ↔ register_stats_impl drift diagnostic.
aura_add_issue_test(test_stats_catalog_drift_1672)
aura_issue_test_link_llvm_jit(test_stats_catalog_drift_1672)
# Issue #1673: production terminal render closed loop (diff→dirty + render-stats).
aura_add_issue_test(test_terminal_render_production_1673)
aura_issue_test_link_llvm_jit(test_terminal_render_production_1673)
# Issue #1674: wire render bump_* + snapshot FnMetrics + query:render-stats.
aura_add_issue_test(test_render_hotpath_observability_1674)
aura_issue_test_link_llvm_jit(test_render_hotpath_observability_1674)
# Issue #1675: render memory predictability (frame arena, GC hotpath defer).
aura_add_issue_test(test_render_memory_predictability_1675)
aura_issue_test_link_llvm_jit(test_render_memory_predictability_1675)
# Issue #1676: render-tier fast dispatch + linear/epoch TUI/render fences.
aura_add_issue_test(test_render_dispatch_linear_epoch_1676)
aura_issue_test_link_llvm_jit(test_render_dispatch_linear_epoch_1676)
# Issue #1677: render prim template + AI Native query/mutate evolution.
aura_add_issue_test(test_render_ai_native_template_1677)
aura_issue_test_link_llvm_jit(test_render_ai_native_template_1677)
# Issue #1678: workspace_marker_macro_introduced max(walk, snapshot).
aura_add_issue_test(test_workspace_marker_macro_max_1678)
aura_issue_test_link_llvm_jit(test_workspace_marker_macro_max_1678)
# Issue #1679: reflect validate terminates on cyclic FlatAST children.
aura_add_issue_test(test_reflect_validate_cycle_guard_1679)
aura_issue_test_link_llvm_jit(test_reflect_validate_cycle_guard_1679)
# Issue #1680: query:module-exports mtime cache.
aura_add_issue_test(test_module_export_cache_1680)
aura_issue_test_link_llvm_jit(test_module_export_cache_1680)
# Issue #1681: stale-ref errors must not pollute string_heap_.
aura_add_issue_test(test_stale_ref_string_heap_1681)
aura_issue_test_link_llvm_jit(test_stale_ref_string_heap_1681)
# Issue #1682: occurrence-dirty subtree walk cycle guard.
aura_add_issue_test(test_occurrence_dirty_cycle_guard_1682)
aura_issue_test_link_llvm_jit(test_occurrence_dirty_cycle_guard_1682)
# Issue #1683: SV closed-loop pins workspace under shared/outer unique lock.
aura_add_issue_test(test_sv_closedloop_workspace_lock_1683)
aura_issue_test_link_llvm_jit(test_sv_closedloop_workspace_lock_1683)
# Issue #1684: MutationBoundaryGuard::run_or_rollback exception safety.
aura_add_issue_test(test_mutate_guard_exception_safety_1684)
aura_issue_test_link_llvm_jit(test_mutate_guard_exception_safety_1684)
# Issue #1685: re-resolve Define after parse_to_flat (rebind/set-body).
aura_add_issue_test(test_mutate_rebind_stale_define_1685)
aura_issue_test_link_llvm_jit(test_mutate_rebind_stale_define_1685)
# Issue #1686: set-body / sibling run_or_rollback exception safety.
aura_add_issue_test(test_mutate_set_body_exception_safety_1686)
aura_issue_test_link_llvm_jit(test_mutate_set_body_exception_safety_1686)
# Issue #1687: set-body double-stale Define+Lambda after parse_to_flat.
aura_add_issue_test(test_mutate_set_body_stale_ids_1687)
aura_issue_test_link_llvm_jit(test_mutate_set_body_stale_ids_1687)
# Issue #1688: remove-node detaches from ALL parents (DAG).
aura_add_issue_test(test_mutate_remove_node_all_parents_1688)
aura_issue_test_link_llvm_jit(test_mutate_remove_node_all_parents_1688)
# Issue #1689: inverted parent-edge index for O(deg) remove-node.
aura_add_issue_test(test_mutate_remove_node_parent_index_1689)
aura_issue_test_link_llvm_jit(test_mutate_remove_node_parent_index_1689)
# Issue #1690: insert-child re-validate parent after parse_to_flat.
aura_add_issue_test(test_mutate_insert_child_stale_parent_1690)
aura_issue_test_link_llvm_jit(test_mutate_insert_child_stale_parent_1690)
# Issue #1691: no dead define_str string_heap_ push in refactor/extract.
aura_add_issue_test(test_mutate_dead_heap_define_str_1691)
aura_issue_test_link_llvm_jit(test_mutate_dead_heap_define_str_1691)
# Issue #1692: no dead circular-dep string_heap_ push in module loader.
aura_add_issue_test(test_module_loader_dead_heap_circular_1692)
aura_issue_test_link_llvm_jit(test_module_loader_dead_heap_circular_1692)
# Issue #1693: no dead prefix inject string_heap_ push in import.
aura_add_issue_test(test_module_prefix_dead_heap_1693)
aura_issue_test_link_llvm_jit(test_module_prefix_dead_heap_1693)
# Issue #1694: replace-pattern re-validate parent after each parse_to_flat.
aura_add_issue_test(test_mutate_replace_pattern_stale_parent_1694)
aura_issue_test_link_llvm_jit(test_mutate_replace_pattern_stale_parent_1694)
# Issue #1695: QueryMatcher captures are StableNodeRef (not raw NodeId).
aura_add_issue_test(test_matcher_stable_captures_1695)
aura_issue_test_link_llvm_jit(test_matcher_stable_captures_1695)
# Issue #1696: multi-node mutators log under NULL_NODE, not NodeId 0.
aura_add_issue_test(test_mutate_multi_node_log_null_1696)
aura_issue_test_link_llvm_jit(test_mutate_multi_node_log_null_1696)
# Issue #1697: replace-subtree re-validate/re-derive parent after parse_to_flat.
aura_add_issue_test(test_mutate_replace_subtree_stale_parent_1697)
aura_issue_test_link_llvm_jit(test_mutate_replace_subtree_stale_parent_1697)
# Issue #1699: splice re-validate parent after each multi-arg parse_to_flat.
aura_add_issue_test(test_mutate_splice_stale_parent_1699)
aura_issue_test_link_llvm_jit(test_mutate_splice_stale_parent_1699)
# Issue #1700: wrap re-validate parent_of_target after parse_to_flat.
aura_add_issue_test(test_mutate_wrap_stale_parent_1700)
aura_issue_test_link_llvm_jit(test_mutate_wrap_stale_parent_1700)
# Issue #1701: extract-function re-validate parent after multi add_*.
aura_add_issue_test(test_mutate_extract_stale_parent_1701)
aura_issue_test_link_llvm_jit(test_mutate_extract_stale_parent_1701)
# Issue #1702: inline-call re-validate call_parent after DFS clone add_*.
aura_add_issue_test(test_mutate_inline_stale_parent_1702)
aura_issue_test_link_llvm_jit(test_mutate_inline_stale_parent_1702)
# Issue #1703: refactor/extract re-validate parent after parse_to_flat.
aura_add_issue_test(test_mutate_refactor_extract_stale_parent_1703)
aura_issue_test_link_llvm_jit(test_mutate_refactor_extract_stale_parent_1703)
# Issue #1704: sv-add-coverpoint / sv-weaken-property MutationBoundaryGuard.
aura_add_issue_test(test_mutate_sv_guard_1704)
aura_issue_test_link_llvm_jit(test_mutate_sv_guard_1704)
# Issue #1706: aura_closure_exists disambiguates epoch OOR vs 0.
aura_add_issue_test(test_closure_exists_epoch_1706)
aura_issue_test_link_llvm_jit(test_closure_exists_epoch_1706)
# Issue #1707: closure cache generation seqlock (torn-read safety).
aura_add_issue_test(test_jit_closure_cache_race_1707)
aura_issue_test_link_llvm_jit(test_jit_closure_cache_race_1707)
# Issue #1708: free_list push before freed=1 (slot leak on throw).
aura_add_issue_test(test_closure_free_list_order_1708)
aura_issue_test_link_llvm_jit(test_closure_free_list_order_1708)
# Issue #1709: capture bounds on func_ids+envs (not envs alone).
aura_add_issue_test(test_closure_capture_bounds_1709)
aura_issue_test_link_llvm_jit(test_closure_capture_bounds_1709)
# Issue #1592: post-steal EnvFrame/StableNodeRef/linear closed loop.
aura_add_issue_test(test_post_steal_closed_loop_1592)
aura_issue_test_link_llvm_jit(test_post_steal_closed_loop_1592)
# Issue #1593: AI closed-loop readiness health-score + SLO + adaptive.
aura_add_issue_test(test_ai_closedloop_readiness_1593)
aura_issue_test_link_llvm_jit(test_ai_closedloop_readiness_1593)
# Issue #1594: ResourceQuota Arena + Guard wired refine (inventory + closed-loop AC).
aura_add_issue_test(test_resource_quota_wired)
aura_issue_test_link_llvm_jit(test_resource_quota_wired)
# Issue #1595: join/mailbox/parallel linear ownership + StableNodeRef enforcement.
aura_add_issue_test(test_fiber_join_linear)
aura_issue_test_link_llvm_jit(test_fiber_join_linear)
# Issue #1596: runtime linear ownership + live-closure scan refine.
aura_add_issue_test(test_linear_ownership_post_mutate)
aura_issue_test_link_llvm_jit(test_linear_ownership_post_mutate)
# Issue #1597: orch metrics folded into ai-closedloop-readiness-stats.
aura_add_issue_test(test_ai_closedloop_orch_readiness_1597)
aura_issue_test_link_llvm_jit(test_ai_closedloop_orch_readiness_1597)
# Issue #1598: apply_closure / JIT / post-steal dual-epoch hotpath refine.
aura_add_issue_test(test_epoch_apply_hotpath_1598)
aura_issue_test_link_llvm_jit(test_epoch_apply_hotpath_1598)
# Issue #1604: stale closure fallback on apply_closure + JIT aura_closure_call.
aura_add_issue_test(test_stale_closure_fallback)

# Issue #1626: forced dual-check apply_closure + JIT aura_closure_call
aura_add_issue_test(test_closure_dual_check_1626)
aura_issue_test_link_llvm_jit(test_closure_dual_check_1626)
aura_issue_test_link_llvm_jit(test_stale_closure_fallback)
# Issue #1632: mandate bridge_epoch + defuse check on apply + JIT + IR
aura_add_issue_test(test_epoch_apply_mandate_1632)
aura_issue_test_link_llvm_jit(test_epoch_apply_mandate_1632)
# Issue #1599: linear GC audit + closedloop readiness + adaptive safepoint refine.
aura_add_issue_test(test_linear_gc_closedloop_readiness_1599)
aura_issue_test_link_llvm_jit(test_linear_gc_closedloop_readiness_1599)
# Issue #1600: ResourceQuota on agent_spawn / parallel_intend / join orch paths.
aura_add_issue_test(test_orch_resource_quota_1600)
aura_issue_test_link_llvm_jit(test_orch_resource_quota_1600)
# Issue #1601: eval/eval_ir prefer partial re-lower consumer wiring.
aura_add_issue_test(test_incremental_relower_consumer_1601)
aura_issue_test_link_llvm_jit(test_incremental_relower_consumer_1601)
# Issue #1605: per-block dirty → partial re-lower (named test_incremental_relower).
aura_add_issue_test(test_incremental_relower)
aura_issue_test_link_llvm_jit(test_incremental_relower)
# Issue #1606: walk_active_closures + linear live scan on invalidate/compact/JIT.
aura_add_issue_test(test_walk_active_closures_1606)
aura_issue_test_link_llvm_jit(test_walk_active_closures_1606)
# Issue #1659: linear ownership + GC/Arena mutation safety mandate.
aura_add_issue_test(test_linear_ownership_mutation_safety_1659)
aura_issue_test_link_llvm_jit(test_linear_ownership_mutation_safety_1659)
# Issue #1607: unified mark_define_dirty / invalidate dual-epoch protocol.
aura_add_issue_test(test_unified_invalidation_1607)
aura_issue_test_link_llvm_jit(test_unified_invalidation_1607)

# Issue #1627: soft/hard invalidate consistency + pre-cascade unify
aura_add_issue_test(test_invalidate_consistency_1627)
aura_issue_test_link_llvm_jit(test_invalidate_consistency_1627)
# Issue #1608: fiber resume post-steal EnvFrame/linear refresh.
aura_add_issue_test(test_fiber_resume_post_steal_1608)
aura_issue_test_link_llvm_jit(test_fiber_resume_post_steal_1608)
# Issue #1631: mandate EnvFrame/bridge refresh + linear probe on Fiber::resume
aura_add_issue_test(test_fiber_resume_lifecycle_1631)
aura_issue_test_link_llvm_jit(test_fiber_resume_lifecycle_1631)
# Issue #1609: query:pattern MacroIntroduced hygiene force-filter.
aura_add_issue_test(test_query_pattern_hygiene_1609)
aura_issue_test_link_llvm_jit(test_query_pattern_hygiene_1609)
# Issue #1636: mandate MacroIntroduced hygiene + marker dimension via tag_arity.
aura_add_issue_test(test_query_pattern_hygiene_mandate_1636)
aura_issue_test_link_llvm_jit(test_query_pattern_hygiene_mandate_1636)
# Issue #1610: IR/JIT/AOT MacroIntroduced + provenance propagation.
aura_add_issue_test(test_ir_hygiene_propagation_1610)
aura_issue_test_link_llvm_jit(test_ir_hygiene_propagation_1610)
# Issue #1611: reflect.hh MacroIntroduced hygiene + post_mutation gate.
aura_add_issue_test(test_reflect_macro_hygiene_1611)
aura_issue_test_link_llvm_jit(test_reflect_macro_hygiene_1611)
# Issue #1612: fiber resume/steal/GC MacroIntroduced marker + provenance.
aura_add_issue_test(test_fiber_macro_hygiene_refresh_1612)
aura_issue_test_link_llvm_jit(test_fiber_macro_hygiene_refresh_1612)
# Issue #1613: macro hygiene closed-loop health + audit trail.
aura_add_issue_test(test_macro_hygiene_closedloop_health_1613)
aura_issue_test_link_llvm_jit(test_macro_hygiene_closedloop_health_1613)
# Issue #1614: TypedMutationAudit real type/linear/provenance checks.
aura_add_issue_test(test_typed_mutation_invariant_audit_1614)
aura_issue_test_link_llvm_jit(test_typed_mutation_invariant_audit_1614)
# Issue #1615: linear ownership + coercion synergy / post-coercion reval.
aura_add_issue_test(test_linear_coercion_synergy_1615)
aura_issue_test_link_llvm_jit(test_linear_coercion_synergy_1615)
# Issue #1616: IRClosure/ClosureBridge MacroIntroduced provenance (refine #1047).
aura_add_issue_test(test_ir_closure_provenance_1616)
aura_issue_test_link_llvm_jit(test_ir_closure_provenance_1616)

aura_add_issue_test(test_let_poly_solve_delta_1617)
aura_issue_test_link_llvm_jit(test_let_poly_solve_delta_1617)

aura_add_issue_test(test_resource_quota_manager_1618)
aura_issue_test_link_llvm_jit(test_resource_quota_manager_1618)

aura_add_issue_test(test_soa_view_enforcement_1619)
aura_issue_test_link_llvm_jit(test_soa_view_enforcement_1619)

# Issue #1629: IR SoA dual-emit flag default-off AC closed-loop
aura_add_issue_test(test_ir_soa_dual_emit_flag_1629)
aura_issue_test_link_llvm_jit(test_ir_soa_dual_emit_flag_1629)

# Issue #1624: PCV / pmr SoAColumnarFull DOD + Contracts
aura_add_issue_test(test_soa_dod_pcv_pmr_1624)
aura_issue_test_link_llvm_jit(test_soa_dod_pcv_pmr_1624)

aura_add_issue_test(test_cpp26_contracts_hotpath_1620)
aura_issue_test_link_llvm_jit(test_cpp26_contracts_hotpath_1620)

aura_add_issue_test(test_arena_auto_compact_policy_1621)
aura_issue_test_link_llvm_jit(test_arena_auto_compact_policy_1621)

aura_add_issue_test(test_value_encoding_v2_dispatch_contracts_1622)
aura_issue_test_link_llvm_jit(test_value_encoding_v2_dispatch_contracts_1622)

aura_add_issue_test(test_eval_relower_hotpath_1623)
aura_issue_test_link_llvm_jit(test_eval_relower_hotpath_1623)
# Issue #1602: parallel orchestration stress (mutate + parallel agent + GC).
aura_add_issue_test(test_parallel_orchestration_stress_1602)
aura_issue_test_link_llvm_jit(test_parallel_orchestration_stress_1602)

# Issue #1496: unify mark_define_dirty / invalidate_function dual-epoch protocol.
aura_add_issue_test(test_issue_1496)
aura_issue_test_link_llvm_jit(test_issue_1496)
# Issue #1571: concurrent epoch safety stress for #1496 (mutate + steal + old closure).
aura_add_issue_test(test_issue_1496_concurrent_epoch_safety)
aura_issue_test_link_llvm_jit(test_issue_1496_concurrent_epoch_safety)
# Issue #1497: StableNodeRef auto-restamp across GC/steal/safepoint.
aura_add_issue_test(test_issue_1497)
aura_issue_test_link_llvm_jit(test_issue_1497)
# Issue #1498: ResourceQuota production closed-loop (Arena + Guard + stats).
aura_add_issue_test(test_issue_1498)
aura_issue_test_link_llvm_jit(test_issue_1498)
# Issue #1499: AI closed-loop readiness consolidated observability.
aura_add_issue_test(test_issue_1499)
aura_issue_test_link_llvm_jit(test_issue_1499)
# Issue #1501: query:pattern MacroIntroduced hygiene + marker-aware index.
aura_add_issue_test(test_issue_1501)
aura_issue_test_link_llvm_jit(test_issue_1501)
# Issue #1502: strong atomic structural topology rollback (children_/parent_).
aura_add_issue_test(test_issue_1502)
aura_issue_test_link_llvm_jit(test_issue_1502)
# Issue #1503: incremental tag_arity_index + dirty linkage + rebuild policy.
aura_add_issue_test(test_issue_1503)
aura_issue_test_link_llvm_jit(test_issue_1503)
# Issue #1504: first-class safe yield at MutationBoundaryGuard + depth stats.
aura_add_issue_test(test_issue_1504)
aura_issue_test_link_llvm_jit(test_issue_1504)
# Issue #1505: mark_define_dirty cascade nested-lambda free-var scan + targeted dirty.
aura_add_issue_test(test_issue_1505)
aura_issue_test_link_llvm_jit(test_issue_1505)

# Issue #1625: nested lambda per-block targeted dirty cascade
aura_add_issue_test(test_issue_1625_nested_lambda_targeted)
aura_issue_test_link_llvm_jit(test_issue_1625_nested_lambda_targeted)
# Issue #1506: eval/eval_ir prefer relower_define_blocks (partial) over cache_define.
aura_add_issue_test(test_issue_1506)
aura_issue_test_link_llvm_jit(test_issue_1506)
# Issue #1555: eval/eval_ir clean-hit reuse + 1000-round set-body relower stress.
aura_add_issue_test(test_issue_1555)
aura_issue_test_link_llvm_jit(test_issue_1555)
# Issue #1546: Arena::allocate_raw ↔ check_arena_quota / allocate_checked wiring.
aura_add_issue_test(test_arena_quota_wired)
aura_issue_test_link_llvm_jit(test_arena_quota_wired)
# Issue #1547: MutationBoundaryGuard::try_acquire typed ResourceQuotaExceeded.
aura_add_issue_test(test_mutation_guard_typed_error)
aura_issue_test_link_llvm_jit(test_mutation_guard_typed_error)

# Issue #1628: try_acquire factory + typed ResourceQuotaExceeded closed-loop
aura_add_issue_test(test_mutation_guard_try_acquire_1628)
aura_issue_test_link_llvm_jit(test_mutation_guard_try_acquire_1628)
# Issue #1634: unify invalidate + try_acquire typed quota mandate
aura_add_issue_test(test_unify_invalidate_try_acquire_1634)
aura_issue_test_link_llvm_jit(test_unify_invalidate_try_acquire_1634)
# Issue #1630: mandate full StableNodeRef provenance (fiber/cow/wrap/pin)
aura_add_issue_test(test_stable_ref_provenance_mandate_1630)
aura_issue_test_link_llvm_jit(test_stable_ref_provenance_mandate_1630)
# Issue #1556: try_acquire refine — deprecated legacy ctor + rebind/set-body quota.
aura_add_issue_test(test_issue_1556)
aura_issue_test_link_llvm_jit(test_issue_1556)
# Issue #1557: walk_active_closures + linear scan on steal/invalidate/compact/JIT.
aura_add_issue_test(test_issue_1557)
aura_issue_test_link_llvm_jit(test_issue_1557)
# Issue #1558: dual-epoch apply_closure + JIT aura_closure_call + post-steal refresh.
aura_add_issue_test(test_issue_1558)
aura_issue_test_link_llvm_jit(test_issue_1558)
# Issue #1472: End-to-end atomic batch for complex mutate ops + index
# sync. Pure C++ (no CompilerService / LLVM JIT); tests atomic-batch
# observability + MutationBoundaryGuard + lockless helper suppression.
aura_add_issue_test(test_issue_1472)
# Epic #1449 demotion batch: facade dashboards + Tier-1 siblings removal
aura_add_issue_test(test_issue_1449_demotion_batch)
aura_issue_test_link_llvm_jit(test_issue_1449_demotion_batch)
# Issue #1414: ConstraintSolver engine-level cache + typed_mutation
# epoch bump. Production code lives in
# src/compiler/type_checker.ixx (ConstraintSystem public hash
# helpers compute_dirty_set_hash / compute_vars_hash) +
# src/compiler/type_checker_impl.cpp (hash implementations) +
# src/compiler/service.ixx (solved_delta_cache_ + solve_delta_cached
# + on_typed_mutation_epoch_bump, wired into invalidate_function
# after bump_bridge_epoch). This test exercises direct TypeChecker
# + ConstraintSystem calls (no full CompilerService::eval path), so
# aura_issue_test_link_llvm_jit is NOT required. 6 ACs: cache hit
# on identical dirty_set+vars, cache invalidates on epoch bump,
# cached result matches direct solve_delta, 100 mutations × 5 calls
# hit rate ≥ 80% (the issue's main perf AC), cache stability
# across calls, clean state early-returns without cache touch.
aura_add_issue_test(test_issue_1414)
# CompilerService::solve_delta_cached + TypeChecker path needs
# full JIT/runtime link set (same triangle as other CS tests).
aura_issue_test_link_llvm_jit(test_issue_1414)
# Issue #1411: DeadCoercionEliminationPass wire-up to pass_manager
# pipeline. Production code is already wired in
# service.ixx:run_coercion_elim_on_function (line 7390+); this test
# is a contract test that exercises the DCE pass directly on
# synthetic IR with 5 CastOp instructions and verifies the pass
# counts. 5 ACs: counts sum to total, elapsed_us nonzero,
# keep_for_debug mode, run_function per-function path, idempotent.
# NOTE: aura_add_issue_test(test_dead_coercion_elim) is already
# registered at line 1405 (with aura_issue_test_link_llvm_jit on
# line 1406) — do NOT add a second registration here (CMP0002
# "target already exists" error).
# Issue #786: P0 unified 'code-as-data' closed-loop
# production health composite dashboard (Consolidate
# #759/#758/#757/#750/#755/#773/#774 etc.). 0 NEW
# atomics + 0 NEW bump helpers + 1 NEW
# (query:code-as-data-production-health, schema 786)
# primitive that uses live primitive lookup to
# aggregate the 8 expected sub-primitives + derive
# composite SLO status (parallel companion pattern,
# mirror #777 live lookup + #782 module
# readiness). 5 ACs: hash shape, fresh-zero state,
# schema sentinel, production-path coverage
# correctness (live count matches independent EDSL
# reachability check), sibling #785/#759 regression.
aura_add_issue_test(test_issue_786)
aura_issue_test_link_llvm_jit(test_issue_786)
# Issue #787: P0 end-to-end hygiene + schema +
# linear ownership fidelity under fiber steal + AOT
# hot-reload + Guard rollback chaos in macro/EDSL
# self-mod loops (Consolidate #757/#758/#750/#755/
# #783/#785 non-duplicative). 0 NEW atomics + 0 NEW
# bump helpers + 1 NEW (query:task6-concurrent-
# fidelity, schema 787) primitive — consolidation
# composite (mirror #786) that uses live primitive
# lookup to aggregate 6 expected sub-primitives +
# derives composite fidelity status. 5 ACs: hash
# shape, fresh-zero state, schema sentinel,
# production-path coverage correctness, sibling
# #786/#785 regression.
aura_add_issue_test(test_issue_787)
aura_issue_test_link_llvm_jit(test_issue_787)
# Issue #788: P0 first-class AI Agent primitives for
# macro policy tuning + runtime EDSL struct
# definition/extension with built-in schema/hygiene/
# linear validation + observability (Consolidate
# #757/#758/#750/#775/#751 non-duplicative). 0 NEW
# atomics + 0 NEW bump helpers + 1 NEW
# (query:ai-native-extension-stats, schema 788)
# primitive — consolidation composite (mirror
# #786/#787) that uses live primitive lookup to
# aggregate 5 expected sub-primitives + derive
# composite AI extension status. 5 ACs: hash shape,
# fresh-zero state, schema sentinel, production-path
# coverage correctness, sibling #786/#787 regression.
aura_add_issue_test(test_issue_788)
aura_issue_test_link_llvm_jit(test_issue_788)
# Issue #789: P0 mandate SafePCVSpan / children_safe
# in all query:pattern / matcher walks + enforce
# tag_arity_index_ hot-path + deep :marker provenance
# predicate (Refine/Consolidate #760 non-duplicative).
# 2 NEW atomics + 2 NEW bump helpers + 1 NEW
# (query:pattern-index-safe-span-stats, schema 789)
# primitive. 5 ACs: hash shape, fresh-zero, schema
# sentinel, production-path bump helpers callable +
# activity reflects in primitive, sibling #760/#788
# regression.
aura_add_issue_test(test_issue_789)
aura_issue_test_link_llvm_jit(test_issue_789)
# Issue #790: P0 first-class (mutate:atomic-batch
# body-expr :snapshot? #t) primitive with pinned
# StableNodeRef snapshot + per-boundary observability
# + cross-fiber safety (Refine/Consolidate #737/#761
# non-duplicative). 2 NEW Evaluator atomics + 2 NEW
# bump helpers + 2 NEW accessors + 1 NEW
# (query:mutate-batch-atomic-stats, schema 790)
# primitive. 5 ACs: hash shape, fresh-zero, schema
# sentinel, production-path bump helpers callable +
# activity reflects in primitive, sibling #761/#789
# regression.
aura_add_issue_test(test_issue_790)
aura_issue_test_link_llvm_jit(test_issue_790)
# Issue #791: P0 exhaustive fiber yield-point
# instrumentation + automatic StableRef/dirty
# cross-boundary propagation in all Workspace EDSL
# primitives (Refine/Consolidate #773/#762
# non-duplicative). 3 NEW CompilerMetrics atomics +
# 3 NEW bump helpers on Evaluator + 1 NEW
# (query:workspace-closedloop-fiber-multi-agent-yield-
# stats, schema 791) primitive. 5 ACs: hash shape,
# fresh-zero, schema sentinel, production-path bump
# helpers callable + activity reflects in primitive,
# sibling #773/#790 regression.
aura_add_issue_test(test_issue_791)
aura_issue_test_link_llvm_jit(test_issue_791)
# Issue #792: P0 compiler invalidate_function +
# mutation_epoch_ synchronization with outermost
# MutationBoundaryGuard depth + live IRClosure /
# EnvFrame / GuardShape version refresh under
# concurrent fiber steal (Non-duplicative
# refinement of #783/#755/#784/#787). 4 NEW
# CompilerMetrics atomics + 4 NEW bump helpers +
# 1 NEW (query:compiler-invalidate-guard-steal-
# stats, schema 792) primitive. 5 ACs: hash shape,
# fresh-zero, schema sentinel, production-path
# bump helpers callable + activity reflects in
# primitive, sibling #783/#791 regression.
aura_add_issue_test(test_issue_792)
aura_issue_test_link_llvm_jit(test_issue_792)
# Issue #793: P0 JIT/AOT hot-swap + GuardShape +
# linear + EnvFrame version_ consistency (Non-
# duplicative consolidation/refinement of #785/
# #787/#755). 4 NEW CompilerMetrics atomics + 4
# NEW bump helpers + 1 NEW (query:jit-aot-hotswap-
# fidelity-stats, schema 793) primitive. 5 ACs:
# hash shape, fresh-zero, schema sentinel,
# production-path bump helpers callable + activity
# reflects in primitive, sibling #785/#792
# regression.
aura_add_issue_test(test_issue_793)
aura_issue_test_link_llvm_jit(test_issue_793)
# Issue #794: P0 unified end-to-end closed-loop
# fidelity harness (full compiler pipeline + EDSL
# mutate + fiber steal + AOT hot-reload + StableRef/
# EnvFrame pinning). Non-duplicative to #786/#787/
# #755/#792/#793. 4 NEW CompilerMetrics atomics +
# 4 NEW bump helpers + 1 NEW (query:full-
# closedloop-compiler-edsl-fidelity-stats, schema
# 794) primitive. 5 ACs: hash shape, fresh-zero,
# schema sentinel, production-path bump helpers
# callable + activity reflects in primitive,
# sibling #793/#787 regression.
aura_add_issue_test(test_issue_794)
aura_issue_test_link_llvm_jit(test_issue_794)
# Issue #795: P0 deep hot-path Contracts + stronger
# SoAView/ShapeStablePass Concepts + ShapeProfiler
# JIT Epoch Sync + Dirty Propagation (Non-
# duplicative refinement of #768/#507/#766/#767/
# #741). 4 NEW CompilerMetrics atomics + 4 NEW
# bump helpers + 1 NEW (query:shape-pass-hotpath-
# contracts-stats, schema 795) primitive. 5 ACs:
# hash shape, fresh-zero, schema sentinel,
# production-path bump helpers callable + activity
# reflects in primitive, sibling #768/#794
# regression.
aura_add_issue_test(test_issue_795)
aura_issue_test_link_llvm_jit(test_issue_795)
# Issue #796: P0 end-to-end IRModuleV2 SoA full
# migration + DirtyAware short-circuit + DepGraph
# integration (Non-duplicative extension of #766/
# #741). 4 NEW CompilerMetrics atomics + 4 NEW
# bump helpers + 1 NEW (query:ir-soa-full-
# migration-stats, schema 796) primitive. 5 ACs:
# hash shape, fresh-zero, schema sentinel,
# production-path bump helpers callable + activity
# reflects in primitive, sibling #766/#795
# regression.
aura_add_issue_test(test_issue_796)
aura_issue_test_link_llvm_jit(test_issue_796)
# Issue #797: P0 Arena live-object defrag + auto-compact
# policy + fiber/GC safepoint yield + Shape/Dirty
# integration (complete #300 P1 + #767 + #685). 5 ACs:
# hash shape (existing #767 primitive now exposes
# production-readiness derived field), fresh-zero,
# schema sentinel == 767, production-path #767 bump
# helpers callable + observability only bumps do NOT
# flip production-readiness (conservative semantics —
# derive from arena stats + yield together), sibling
# observability regression #685 + #642 + #569 + #796.
aura_add_issue_test(test_issue_797)
aura_issue_test_link_llvm_jit(test_issue_797)
# Issue #806: P0 stdlib AI-native registry +
# extension first-class AI Agent primitives for
# safe stdlib extension via registry (refine/
# consolidate #775 non-duplicative focus on
# registry integration). 5 ACs: hash shape, fresh-
# zero split-rule, schema sentinel == 806, NEW
# bump helper callable + slo-validation-pct /
# recommendation transition, sibling #775 + #633
# + #797 regression.
aura_add_issue_test(test_issue_806)
aura_issue_test_link_llvm_jit(test_issue_806)
# Issue #803: P0 EDA-SV-verification-production
# long-running concurrent multi-agent verification
# evolution harness (consolidation of #794/#755/#773/
# #774/#802; non-duplicative with each component
# primitive). 5 ACs: hash shape (create(16) with 8
# fields + schema), fresh-zero split-rule, schema
# sentinel == 803, 3 NEW bump helpers callable +
# slo-derived convergence-rate + hygiene-safe-rollback-
# pct in valid range, sibling #802 + #759 + #806 + #794
# + #783 regression.
aura_add_issue_test(test_issue_803)
aura_issue_test_link_llvm_jit(test_issue_803)
# Issue #818: StableNodeRef full provenance + cross-COW/sub-workspace
# auto-resolve enforcement observability (Task1-review follow-up).
# 4 CompilerMetrics atomics (stable_ref_*_total) +
# (query:stable-ref-cross-cow-provenance-stats, schema 818) + bumps in
# mutate StableRef resolve, workspace:resolve-stable-ref, fiber resume.
# 8 ACs. Non-duplicative with #641/#715/#738/#749/#791.
aura_add_issue_test(test_stable_ref_cross_cow_provenance_enforcement)
aura_issue_test_link_llvm_jit(test_stable_ref_cross_cow_provenance_enforcement)
# Issue #805: query:primitives-hotpath-registry-stats (schema 805)
# — sample-based registry + list-apply load SLO (ns/apply,
# linear-cost, extension-reg-ns, bench-runs). Non-duplicative
# with #776 hotpath-slo stability composite. 8 ACs.
aura_add_issue_test(test_primitives_hotpath_registry_slo)
aura_issue_test_link_llvm_jit(test_primitives_hotpath_registry_slo)
# Issues #807/#808: exception policy + AuraResult Phase 1 bridge
# (map_kind_name / eval_as_aura_result / eval_result_to_aura).
# 8 ACs: kind mappers, policy kinds, success/failure AuraResult,
# monadic and_then, classic eval regression.
aura_add_issue_test(test_aura_result_error_policy)
aura_issue_test_link_llvm_jit(test_aura_result_error_policy)
# Domain suites + legacy phase-1 batches (see cmake/AuraDomainTests.cmake).
include(${CMAKE_SOURCE_DIR}/cmake/AuraDomainTests.cmake)
# Issue #804: P0 stdlib-Registry unified primitive
# error semantics + provenance tracking + recovery
# hooks (refine/consolidate #585 + #751 + #775 + #478).
# 5 ACs: hash shape (create(16) with 8 fields +
# schema), fresh-zero split-rule, schema sentinel
# == 804, 3 NEW bump helpers callable + unified-path-
# pct + recommendation transition, sibling #478 +
# #585 + #751 regression.
aura_add_issue_test(test_issue_804)
aura_issue_test_link_llvm_jit(test_issue_804)

# Issue #660: cache_define bundle ordering (multi-define pre-existing bug).
# Regression net for the bundle[0] fix shipped across fe30663b + 476ce10d
# + 9d632679 + c00a9c1a + fef8898d. 7 ACs: 2-define direct call → 6,
# 2-define (+ x 1) arg → 42, 2-define inline lambda → 6, 1-define baseline
# → 6, 3-define chain (fn-b+fn-c both call fn-a) → 6/10, redefine path
# → 115. Non-duplicative with #402 bundle infra.
aura_add_issue_test(test_issue_660_cache_define_bundle)
aura_issue_test_link_llvm_jit(test_issue_660_cache_define_bundle)
# Issue #661: SV InterfaceIR/ModportIR structure observability (P1).
# 7 ACs: sv-interface-structure-stats (schema 661), ports-count /
# modport-views / direction-changes bump paths, interface-events-total
# sum invariant, eda:parse-netlist modport path (1 modport + 3 ports
# bumps modport-views by 1 + ports by 3), 6 module-local regression
# (sv-node, sv-structured-edsl, sv-sva-structure, sv-verification-
# closedloop, self-evolution-chaos, runtime-observability-correlated).
# Non-duplicative with #640/#630/#539/#497/#498/#496 (SVA / verification
# / pattern scope); #661 covers the interface IR/ModportIR BUILDER
# shape specifically.
aura_add_issue_test(test_issue_661_sv_interface_structure_stats)
aura_issue_test_link_llvm_jit(test_issue_661_sv_interface_structure_stats)
# Issue #662: SVA structured mutate feedback closed loop (P1).
# Regression net for the AI self-evo "coverage hole -> add coverpoint"
# / "assert fail -> weaken property" closed loop. The underlying work
# shipped across #310 (Interface/Modport NodeTag foundation) + #694
# (SVA structured NodeTags + map_ + dirty bits + eda:weaken-property
# + eda:add-coverpoint-bin + query:sv-sva-structure-stats) + #640
# (verification feedback closed loop). 7 ACs: sv-sva-structure-stats
# baseline (schema 694), eda:weaken-property path, eda:add-coverpoint-bin
# path, combined hits +2, sva-dirty-total >= 1, SV primitives
# regression, eda:parse-netlist coexistence (cross-feature
# integration from #661).
aura_add_issue_test(test_issue_662_sva_structured_mutate)
aura_issue_test_link_llvm_jit(test_issue_662_sva_structured_mutate)
# Issue #663: hardware backend SV-specific observability (P1).
# Verbatim-name view of the issue body's Action #4
# (hook_calls + ppa_reemits + verification_triggers). The 3
# underlying atomics already exist + are bumped from
# #580/#277/#640/#698 sites. Ships the (query:hardware-backend-
# sv-stats, schema 663) primitive + 7-AC regression net:
# schema, fields present, sum invariant, SV structural mutate
# → hardware hook path, verification feedback path, regression on
# #580/#698 hardware-backend primitives, cross-feature regression
# on #661/#694 SV primitives.
aura_add_issue_test(test_issue_663_hardware_backend_sv_stats)
aura_issue_test_link_llvm_jit(test_issue_663_hardware_backend_sv_stats)
# Issue #664: SV DefUse incremental observability (P1).
# Observability surface for the structured DefUse build +
# incremental update path. 3 CompilerMetrics atomics
# (sv_defuse_nested_modports_total /
#  sv_defuse_cross_refs_total /
#  sv_defuse_incremental_updates_total) + query:sv-defuse-stats
# primitive (schema 664). The production wiring for Actions #1
# (DefUse build for generate + cross-ref) + #2 (post-mutate
# incremental rebuild hook) is follow-up scope; this commit
# ships the observability surface + regression net.
# 7 ACs: schema, 3 bump paths, sum invariant, multi-round
# simulate (20+15+10 = 45 events), regression on adjacent
# SV primitives (sv-sva 694, sv-interface 661, hardware-
# backend-sv 663). Non-duplicative with #317/#337/#640/#663/
# #691/#661/#662.
aura_add_issue_test(test_issue_664_sv_defuse_stats)
aura_issue_test_link_llvm_jit(test_issue_664_sv_defuse_stats)
# Issue #665: SV stability observability (P1 EDA-SV scalability).
# Observability surface for the SV-tagged stability + scale
# activity. 3 CompilerMetrics atomics
# (sv_dirty_traversal_depth_total / sv_generation_wrap_total /
#  sv_stable_ref_invalidation_total) + query:sv-stability-stats
# primitive (schema 665). The production wiring for Actions #1
# (early-exit / special-case SV tags in mark_dirty_upward) +
# #2 (compact_nodes / restore subtree-gen scope for SV) is
# follow-up scope; this commit ships the observability surface
# + regression net. 7 ACs: schema, 3 bump paths (with depth
# values for the avg-derivation path), sum invariant,
# multi-round simulation (50×5 depth + 30 wrap + 20 inval =
# 300 total events), regression on adjacent SV primitives
# (sv-interface 661, sv-sva 694, sv-defuse 664,
# hardware-backend-sv 663). Non-duplicative with #641/#368/
# #392/#336/#642/#664.
aura_add_issue_test(test_issue_665_sv_stability_stats)
aura_issue_test_link_llvm_jit(test_issue_665_sv_stability_stats)
# Issue #667: list/map/filter apply hot-path observability (P1).
# Wiring the existing list.cpp apply_unary / apply_pred /
# apply_binary hot path with 3 new CompilerMetrics atomics
# (primitives_apply_lookup_hits_total +
#  primitives_apply_closure_calls_total +
#  primitives_apply_fastpath_wins_total) + the
# (query:primitives-apply-stats, schema 667) primitive. 7 ACs:
# schema, 3 bump paths, sum invariant, real hot-path
# exercise (map/filter/fold over 100-element lists),
# regression on adjacent primitives (runtime-observability-
# correlated 673, sv-defuse 664, sv-interface 661).
# Non-duplicative with #479/#480/#614/#643/#633.
aura_add_issue_test(test_issue_667_primitives_apply_stats)
aura_issue_test_link_llvm_jit(test_issue_667_primitives_apply_stats)
# Issue #668: math regex primitive error consistency (P1 stdlib-impl).
aura_add_issue_test(test_issue_668)
aura_issue_test_link_llvm_jit(test_issue_668)
# Issue #669: primitives meta introspection gap-fill (P1 stdlib-impl).
aura_add_issue_test(test_issue_669)
aura_issue_test_link_llvm_jit(test_issue_669)
# Issue #670: verify_tool/diagnostic Guard + StableRef + dirty wiring (P1).
aura_add_issue_test(test_issue_670)
aura_issue_test_link_llvm_jit(test_issue_670)
# Issue #671: primitives capture consistency + style compliance (P1 stdlib-impl).
aura_add_issue_test(test_issue_671)
aura_issue_test_link_llvm_jit(test_issue_671)
# Issue #737: atomic batch + snapshot/rollback + StableNodeRef pinning
# for multi-round AI Agent edit loops (P0 EDSL-Core).
aura_add_issue_test(test_atomic_batch_snapshot_stable_ref_ai_loops)
aura_issue_test_link_llvm_jit(test_atomic_batch_snapshot_stable_ref_ai_loops)
# Issue #738: StableNodeRef COW/sub-workspace pinning + cross-boundary
# validity tracking for concurrent AI orchestration (P0 EDSL-Core).
aura_add_issue_test(test_stable_ref_cow_subworkspace_concurrent_ai)
aura_issue_test_link_llvm_jit(test_stable_ref_cow_subworkspace_concurrent_ai)
# Issue #672: Linear ownership + GuardShape runtime invariant enforcement (P0).
aura_add_issue_test(test_issue_672)
aura_issue_test_link_llvm_jit(test_issue_672)
# Issue #687: DeadCoercionEliminationPass + IR-interpreter identity fast-path (P0).
aura_add_issue_test(test_issue_687)
aura_issue_test_link_llvm_jit(test_issue_687)
# Issue #739: atomic epoch visibility + memory_order under fiber steal.
aura_add_issue_test(test_prompt6_epoch_atomic_visibility_fiber_steal)
aura_issue_test_link_llvm_jit(test_prompt6_epoch_atomic_visibility_fiber_steal)
# Issue #740: linear JIT L2 post-invalidate Arena/DropOp/GC root safety.
aura_add_issue_test(test_prompt6_linear_jit_l2_post_invalidate_arena_gc)
aura_issue_test_link_llvm_jit(test_prompt6_linear_jit_l2_post_invalidate_arena_gc)
# Issue #741: impact_scope → closure_bridge + EnvFrame version re-stamp.
aura_add_issue_test(test_prompt2_6_impact_scope_quote_lambda_bridge_env)
aura_issue_test_link_llvm_jit(test_prompt2_6_impact_scope_quote_lambda_bridge_env)
# Issue #742: C++26 Contracts + consteval hot-path invariants.
aura_add_issue_test(test_cpp26_contracts_hotpath_arena_soa_value_shape_pass)
aura_issue_test_link_llvm_jit(test_cpp26_contracts_hotpath_arena_soa_value_shape_pass)
# Issue #743: Arena auto-compact + defrag + fiber safepoint + dirty/Shape closed loop.
aura_add_issue_test(test_arena_auto_compact_fiber_defag_shape_dirty_closedloop)
aura_issue_test_link_llvm_jit(test_arena_auto_compact_fiber_defag_shape_dirty_closedloop)
# Issue #744: Shape → dirty → Pass → JIT deopt/recompile closed loop.
aura_add_issue_test(test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate)
aura_issue_test_link_llvm_jit(test_shape_jit_pass_deopt_incremental_closedloop_ai_mutate)
# Issue #745: Dynamic reverify limit + Occurrence priority in solve_delta.
aura_add_issue_test(test_typesystem_solve_delta_occurrence_priority_heavy_mutate)
aura_issue_test_link_llvm_jit(test_typesystem_solve_delta_occurrence_priority_heavy_mutate)
# Issue #746: narrow_evidence / TypeId / linear propagation → JIT L2 typed mutate.
aura_add_issue_test(test_typesystem_type_propagation_jit_l2_typed_mutate)
aura_issue_test_link_llvm_jit(test_typesystem_type_propagation_jit_l2_typed_mutate)
# Issue #747: Linear + Occurrence predicate-branch safety under typed mutation.
aura_add_issue_test(test_linear_ownership_occurrence_predicate_mutate)
aura_issue_test_link_llvm_jit(test_linear_ownership_occurrence_predicate_mutate)
# Issue #748: SV verification EDSL structured mutate + dirty re-emit closed-loop.
aura_add_issue_test(test_sv_verification_edsl_full_structured_closedloop)
aura_issue_test_link_llvm_jit(test_sv_verification_edsl_full_structured_closedloop)
# Issue #750: Runtime reflection schema validation for macro/EDSL Guard mutate.
aura_add_issue_test(test_reflection_runtime_validate_macro_edsl_mutate)
aura_issue_test_link_llvm_jit(test_reflection_runtime_validate_macro_edsl_mutate)
# Issue #751: PRIM_ERROR / capture contract enforcement + contract-stats.
aura_add_issue_test(test_primitives_capture_contract)
aura_issue_test_link_llvm_jit(test_primitives_capture_contract)
# Issue #752: list/vector map/filter SoA + intrinsic hot-path observability.
# 4 CompilerMetrics atomics (list_chain_traversals_total /
# list_soa_hits_total / list_intrinsic_dispatches_total /
# list_estimated_cache_misses_total) + (query:list-soa-hotpath-stats,
# schema 752) + map/filter/foldl bump wiring. 8 ACs: schema, 4 bump
# paths, sum invariant, real AI-loop exercise (100-element list +
# 10x map/filter + foldl +), regression on adjacent primitives
# (#667 apply-stats, #506 soa-hotpath-adoption, #614 perf-stats).
# Non-duplicative with #667/#506/#614/#727.
aura_add_issue_test(test_list_vector_soa_hotpath_ai_loops)
aura_issue_test_link_llvm_jit(test_list_vector_soa_hotpath_ai_loops)
# Issue #753: long-running deployment infra (quota/checkpoint/heal/SLO).
# 5 CompilerMetrics atomics (longrunning_quota_violations_total /
# longrunning_checkpoint_success_total / longrunning_heal_triggers_total /
# longrunning_resource_trend_total / longrunning_deployment_slo_hits_total)
# + resource:quota-set/get/check + (query:longrunning-infra-stats,
# schema 753) + panic-checkpoint/heal bump wiring. 8 ACs.
# Non-duplicative with #548/#677/#674/#729.
aura_add_issue_test(test_longrunning_infra_primitives)
aura_issue_test_link_llvm_jit(test_longrunning_infra_primitives)
# Issue #754: LLM-bottleneck adaptive scheduling + yield-classification metrics
# + work-stealing bias + GC safepoint self-tuning.
# 1 CompilerMetrics atomic (orchestration_llm_gc_safepoint_adapted_total)
# + (query:orchestration-llm-bottleneck-stats, schema 754) aggregating
# AdaptiveStealStats (outermost-preferred / backoff-triggers /
# llm-tail-reduction) + GC safepoint defer bump wiring in
# request_gc_safepoint. 8 ACs: schema, gc-safepoint-adapted bump/defer
# path, sum invariant, real guard exercise, regression on #706/#646.
# Non-duplicative with #730/#706/#650/#646.
aura_add_issue_test(test_scheduler_llm_bottleneck_adaptive_steal_gc)
aura_issue_test_link_llvm_jit(test_scheduler_llm_bottleneck_adaptive_steal_gc)
# Issue #755: end-to-end concurrent safety full-cycle integration
# (MutationBoundary + steal + AOT + GC + recovery).
# 4 CompilerMetrics atomics (concurrent_safety_steal_boundary_success_total /
# concurrent_safety_aot_reload_at_guard_total /
# concurrent_safety_gc_safepoint_during_steal_total /
# concurrent_safety_recovery_success_total) +
# (query:concurrent-safety-full-cycle-stats, schema 755) +
# bump wiring in probe_linear_on_steal / transfer_panic_checkpoint /
# resume_fiber_migration / restore_panic_checkpoint. 8 ACs.
# Non-duplicative with #732/#731/#730/#674/#739.
aura_add_issue_test(test_runtime_concurrent_full_cycle_chaos)
aura_issue_test_link_llvm_jit(test_runtime_concurrent_full_cycle_chaos)
# Issue #485: multi-fiber + MutationBoundaryGuard + SoA EnvFrame close-out.
aura_add_issue_test(test_issue_485)
aura_issue_test_link_llvm_jit(test_issue_485)
# Issue #486: query:pattern MacroIntroduced filter + macro-hygiene-stats.
aura_add_issue_test(test_issue_486)
aura_issue_test_link_llvm_jit(test_issue_486)
# Issue #488: post-mutate reflect validation + Guard impact snapshot.
aura_add_issue_test(test_issue_488)
aura_issue_test_link_llvm_jit(test_issue_488)
# Issue #489: StableNodeRef + get_safe enforcement in mutate/query hot paths.
aura_add_issue_test(test_issue_489)
aura_issue_test_link_llvm_jit(test_issue_489)
# Issue #490: proactive tag_arity_index rebuild on COW/compact + policy tuning.
aura_add_issue_test(test_issue_490)
aura_issue_test_link_llvm_jit(test_issue_490)
# Issue #491: JIT opcode coverage + hot-swap safety + jit-stats-hash.
aura_add_issue_test(test_issue_491)
aura_issue_test_link_llvm_jit(test_issue_491)
# Issue #492: ShapeProfiler deopt hook + query:shape-profiler-stats.
aura_add_issue_test(test_issue_492)
aura_issue_test_link_llvm_jit(test_issue_492)
# Issue #493: EDSL hot-path bottleneck stats + eval/lowering instrumentation.
aura_add_issue_test(test_issue_493)
aura_issue_test_link_llvm_jit(test_issue_493)
# Issue #494: Pass pipeline JIT/incremental concepts + yield + stats hash.
aura_add_issue_test(test_issue_494)
aura_issue_test_link_llvm_jit(test_issue_494)
# Issue #496: Native SV Constraint/Class NodeTags + query:sv-node-stats.
aura_add_issue_test(test_issue_496)
aura_issue_test_link_llvm_jit(test_issue_496)
# Issue #497: StableRef lifecycle soft compact + refresh + stats hash.
aura_add_issue_test(test_issue_497)
aura_issue_test_link_llvm_jit(test_issue_497)
# Issue #498: AI-native primitive metadata + skeleton ergonomics.
aura_add_issue_test(test_issue_498)
aura_issue_test_link_llvm_jit(test_issue_498)
# Issue #499: EDA foundation primitives module + query:eda-foundation-stats.
aura_add_issue_test(test_issue_499)
aura_issue_test_link_llvm_jit(test_issue_499)
# Issue #500: Work-stealing + MutationBoundary outermost depth safety.
aura_add_issue_test(test_issue_500)
aura_issue_test_link_llvm_jit(test_issue_500)
# Issue #501: IR MacroIntroduced hygiene (InlinePass + lowering).
aura_add_issue_test(test_issue_501_hygiene)
aura_issue_test_link_llvm_jit(test_issue_501_hygiene)
# Issue #502: Post-mutate reflect validation + Guard impact snapshot.
aura_add_issue_test(test_issue_502)
aura_issue_test_link_llvm_jit(test_issue_502)
# Issue #503: query:pattern hygiene flags + pattern-marker-stats.
aura_add_issue_test(test_issue_503)
aura_issue_test_link_llvm_jit(test_issue_503)
# Issue #504: MutationBoundaryGuard impact log + query primitive.
aura_add_issue_test(test_issue_504)
aura_issue_test_link_llvm_jit(test_issue_504)
# Issue #505: closure/EnvFrame/bridge_epoch post-invalidate safety stats hash.
aura_add_issue_test(test_issue_505)
aura_issue_test_link_llvm_jit(test_issue_505)
# Issue #507: Task4 hot-path Contracts + consteval invariants.
aura_add_issue_test(test_issue_507)
aura_issue_test_link_llvm_jit(test_issue_507)
# Issue #508 slice: dead-coercion-zerooverhead-stats hash observability.
# Issue #510 slice: eda-verification-stats hash observability.
# Issue #511 slice: workspace-snapshot-stats hash observability.
# Issue #512 slice: runtime-orchestration-stats hash observability.
# Issue #513 slice: aot-hot-reload-stats hash observability.
# Issue #515 slice: consolidated-p0-production-stats hash observability.
# Issue #516 slice: prompt6-memory-safety-stats hash observability.
# Issue #519 slice: edsl-eda-sv-closedloop-stats hash observability.
# Issue #521 slice: multi-fiber-orchestration-stats hash observability.
# Issue #522 slice: aot-production-reload-stats hash observability.
# Issue #601: JIT hot-swap / invalidate safety for live closures —
# bridge_epoch refresh + (query:jit-hotswap-closure-stats) observability.
aura_add_issue_test(test_issue_601)
aura_issue_test_link_llvm_jit(test_issue_601)
# Issue #603: IR SoA full consumer adoption + per-block dirty_ driven
# minimal re-lower — enhanced (compile:ir-soa-stats) primitive + 3 new
# counters (view-cache-hits / block-dirty-hits / relower-blocks-saved).
aura_add_issue_test(test_issue_603)
aura_issue_test_link_llvm_jit(test_issue_603)
# Issue #604: arena auto-compact + defrag + fiber/GC safepoint coordination.
# compact()/defrag() now consult gc_hooks::fiber_active() and hit the GC
# safepoint from a fiber context; (query:arena-auto-stats) is a live snapshot.
aura_add_issue_test(test_issue_604)
aura_issue_test_link_llvm_jit(test_issue_604)
# Issue #606: Pass/AnalysisPass concept + fold pipeline + Pure Wrap adoption.
# ComputeKindWrap/ArityWrap now const-correct (PureAnalysisPass);
# ConstantFoldingWrap exposes run_function/run_block aliases (IncrementalPass);
# ShapeWrap + LinearOwnershipWrap are new pure-delegating wraps.
aura_add_issue_test(test_issue_606)
aura_issue_test_link_llvm_jit(test_issue_606)
# Issue #614: primitives hot-path memory stability + pair/cdr observability.
# 3 new atomics (pair_alloc_total / linear_traverse_total / cdr_depth_max)
# + (query:primitives-hotpath-stats) 4-field hash.
aura_add_issue_test(test_issue_614)
aura_issue_test_link_llvm_jit(test_issue_614)
# Issue #615: PRIM_ERROR macro + math.cpp regex unification + silent-swallow
# audit. PRIM_ERROR in primitives_detail.h wraps make_primitive_error with
# default capture names; 4 math.cpp call sites (regex-match?/find/replace/
# split) consolidated to use it; 11 defensive silent catches in agent/ast/
# compile/query_workspace/types/pair/workspace carry [SILENCE-PRIM-#615]
# comments documenting why each is intentionally silent. (query:primitive-
# error-stats) shape unchanged from #478; by_kind / recovery_attempts
# enhancement is a follow-up.
aura_add_issue_test(test_issue_615)
aura_issue_test_link_llvm_jit(test_issue_615)
# Issue #616: EDA hardware-co-design primitives — file-boundary layer
# (eda:load-sv, eda:parse-verification-result) + dedicated observability
# primitive (query:eda-hw-stats). 4 new atomic counters (eda_load_sv_total/
# _failure_total + eda_parse_verification_result_total/_failure_total).
# Fixtures: tests/fixtures/issue_616/sample.sv + cov.json.
aura_add_issue_test(test_issue_616)
aura_issue_test_link_llvm_jit(test_issue_616)
# Issue #617: AI-Native primitive introspection — 3 new query primitives
# (query:primitives-by-category, query:schema-of-primitive,
# query:primitives-meta-catalog) + 3 new atomic counters. Builds on the
# DEFINE_PRIMITIVE_META macro / PrimMeta struct / (query:primitive-
# metadata) / (query:primitive-list-with-meta) / (primitive:generate-
# skeleton) foundation from #480/#697/#709.
aura_add_issue_test(test_issue_617)
aura_issue_test_link_llvm_jit(test_issue_617)
# Issue #618: LLM-aware orchestration metrics foundation — 2 new
# primitives (query:scheduler-mutation-coord-stats, orchestration:tune-
# gc-frequency) + 1 new atomic (gc_frequency_tune_ratio in fiber.h).
# Builds on #451/#588/#591/#607 counter + yield-classification infra.
aura_add_issue_test(test_issue_618)
aura_issue_test_link_llvm_jit(test_issue_618)
# Issue #620: StableNodeRef provenance query primitive — 1 new Aura
# primitive (query:stable-ref-provenance) + 1 new atomic counter.
# Builds on #291/#303/#368/#392 StableNodeRef struct + make_safe_ref
# + capture_for_fiber + is_valid_in foundation.
aura_add_issue_test(test_issue_620)
aura_issue_test_link_llvm_jit(test_issue_620)
# Issue #621: query:pattern-index-stats-hash — Agent-discoverable
# structured observability for the tag_arity_index lifecycle.
# 10-field hash with derived metrics (arity-accuracy, delta-hit-rate,
# linear-fallbacks, recommendation). No new C++ atomics — all
# derived inline. Builds on #547/#554/#490/#503 index counters.
aura_add_issue_test(test_issue_621)
aura_issue_test_link_llvm_jit(test_issue_621)
# Issue #622: high-level Aura atomic-batch primitives — wraps
# FlatAST's #250/#255 begin/commit/rollback_atomic_batch C++ API.
# 4 new Aura primitives (mutate:atomic-begin/commit/rollback +
# query:atomic-batch-stats). Builds on FlatAST atomic_batch_commits_/
# atomic_batch_bumps_saved_/bump_generation_suppressed_ counters.
aura_add_issue_test(test_issue_622)
aura_issue_test_link_llvm_jit(test_issue_622)
# Issue #623: arena auto-compact threshold tunables — wraps the
# existing ArenaGroup::set_compact_threshold() / compact_threshold()
# C++ accessors (added by #187, never Aura-exposed). 2 new
# Arena primitives + 1 atomic counter for production-harden Agent
# memory-pressure watchdog tuning.
aura_add_issue_test(test_issue_623)
aura_issue_test_link_llvm_jit(test_issue_623)
# Issue #624: ShapeProfiler + GuardShape + JIT maturity — 1 new Aura
# primitive (query:shape-stability-jit-stats-hash) that the issue
# body AC4 specifies by name. 3 fields use existing shape::*
# counters verbatim; 2 fields are derived inline (stability-ratio-
# post-mutate / deopt-on-instability). Builds on the existing
# shape:: namespace counters from #570 / #605 / #492 / #686.
aura_add_issue_test(test_issue_624)
aura_issue_test_link_llvm_jit(test_issue_624)
# Issue #625: Pass concept + Contracts + zero-overhead pipeline —
# 1 new Aura primitive (query:pass-pipeline-incremental-stats-hash)
# that the issue body AC4 specifies by name. 1 new atomic counter
# (pass_pipeline_runs_total, bumped per-full-pipeline-run in
# pass_manager.ixx). 4 of the 5 AC4 fields are derived from
# existing counters (shortcircuit-savings = dirty_skipped +
# module_dirty_skips; dirty-blocks-skipped = dirty_pipeline_skip;
# pure-delegation-hits = ShapeWrap + LinearOwnershipWrap sums;
# contracts-checked synthetic from zerooverhead_wins). Only the
# 'passes-run' field is the new atomic. Builds on #494 / #606 /
# #406 / #686 counter surface.
aura_add_issue_test(test_issue_625)
aura_issue_test_link_llvm_jit(test_issue_625)
# Issue #626: C++26 Contracts + consteval + hot-path observability —
# 1 new Aura primitive (query:contracts-hotpath-stats-hash) that
# the issue body AC5 specifies by name. 8-field hash with 4 verbatim
# fields (violations-in-debug / consteval-hits / pass-pipeline-runs /
# arena-auto-triggers) + 3 derived fields (contracts-checked /
# zero-overhead-savings / dirty-blocks-skipped) + schema sentinel.
# Builds on #406 / #508 / #605 / #686 / #494 / #625 existing counters.
aura_add_issue_test(test_issue_626)
aura_issue_test_link_llvm_jit(test_issue_626)
# Issue #630: SV verification feedback closed-loop observability —
# 1 new Aura primitive (query:sv-verification-closedloop-stats-hash)
# that the issue body AC4 specifies by name. 6-field hash with all
# 6 fields read from existing C++ counters (no new atoms):
#   - feedback-to-mutate-cycles        (feedback_mutate_hits_total)
#   - stable-ref-captures-in-sv        (Evaluator::get_verify_tool_stable_ref_hits_total)
#   - verification-dirty-propagations  (Evaluator::get_verify_tool_dirty_propagations_total)
#   - reverify-success                  (verification_loop_success_total)
#   - rollback-on-partial                (sv_emit_parse_fail_total)
#   - ppa-savings-total                  (ppa_savings_total)
# Builds on #499 / #579 / #616 existing primitives + counters.
aura_add_issue_test(test_issue_630)
aura_issue_test_link_llvm_jit(test_issue_630)
# Issue #631: StableNodeRef SV provenance cross-fiber/multi-agent
# observability — 1 new Aura primitive
# (query:stable-ref-provenance-sv-stats-hash) + 2 new atomics
# (cross_fiber_violations_total + safe_resolves_total). Primitive
# has 5 fields: 2 verbatim (provenance-mismatches-on-sv /
# total-stable-ref-invalidations, both from existing
# stable_ref_invalidations counter) + 2 from the new atomics
# (cross-fiber-violations + safe-resolves, currently 0 until
# AC1+AC2 enforcement work bumps them) + schema sentinel.
# Builds on #313/#368/#457/#620/#497 existing infrastructure.
aura_add_issue_test(test_issue_631)
aura_issue_test_link_llvm_jit(test_issue_631)
# Issue #632: atomic-batch SV observability foundation — 1 new Aura
# primitive (query:atomic-batch-sv-stats-hash) + 2 new atomics
# (atomic_batch_sv_rollback_total + atomic_batch_sv_impact_nodes_total).
# Primitive has 5 fields: 2 verbatim (active-sv-batches +
# suppressed-bumps-on-sv, both syntactically from existing #622
# batch surface) + 2 from the new atomics (rollback-success-sv +
# batch-impact-sv-nodes, foundation for AC2+AC3 enforcement work,
# currently 0) + schema sentinel. Builds on #192 / #437 / #529 /
# #622 existing primitives + counters.
aura_add_issue_test(test_issue_632)
aura_issue_test_link_llvm_jit(test_issue_632)
# Issue #1470: query:ai-closedloop-readiness-stats — consolidated
# 5-counter AI closed-loop readiness hash (wraps + invalidations +
# batch_commits + hygiene_skips + dirty_prunes + recommendation).
aura_add_issue_test(test_issue_1470)
aura_issue_test_link_llvm_jit(test_issue_1470)
# Issue #1473: Robust StableNodeRef auto-restamp across COW / fiber-steal /
# GC — 1000+ iter stress test driving the 3 hook sites wired in
# evaluator_fiber_mutation.cpp + evaluator_gc.cpp.
aura_add_issue_test(test_issue_1473)
aura_issue_test_link_llvm_jit(test_issue_1473)
# Issue #1474: Wire per-block dirty bitmask into actual re-lower
# path. Per-block selective copy inside relower_define_function +
# 1 new counter (incremental_relower_blocks_total) + 1 derived
# snapshot field (dirty_block_ratio_bp). 1000-round stress test
# verifies the per-block path fires 1 block per round (not the
# full function).
aura_add_issue_test(test_issue_1474)
aura_issue_test_link_llvm_jit(test_issue_1474)
# Issue #1475: Force bridge_epoch + defuse_version_ dual checks
# in apply_closure / IR closure call paths. This test ships the
# pure helper `is_env_frame_stale(env_id, frame_version, current)`
# (parallel to is_bridge_stale) and verifies its 7 invariant cases
# + integration with defuse_version_for_test /
# bump_defuse_version_for_test. Hot-path wiring of the dual check
# into the 3 IR executor sites + JIT + closure_bridge_ callback +
# multi-fiber stress test is deferred to follow-up issues.
aura_add_issue_test(test_issue_1475)
aura_issue_test_link_llvm_jit(test_issue_1475)
# Issue #1476: Unify mark_define_dirty / invalidate_function + atomic
# bridge_epoch + mutation_epoch bump protocol. This test verifies
# the MVP: bridge_epoch_bumps_total wired to bump_bridge_epoch +
# mark_define_dirty acquires mutate_mtx_ + bumps BOTH epochs
# (bridge_epoch via mutation_epoch_ release + defuse_version acq_rel)
# atomically via atomic_bump_epochs_and_stamp_bridge helper +
# invalidate_cascade_depth_max tracked via CAS loop. JIT fn_trackers_
# notify + lock-ordering audit + multi-fiber concurrent stress +
# typed_mutate migration deferred to follow-up issues.
aura_add_issue_test(test_issue_1476)
aura_issue_test_link_llvm_jit(test_issue_1476)
# Issue #1478 / #1541: linear_post_mutate_enforce MVP + 2 counters
# (9 ACs restored after scope-limited close deferred build).
aura_add_issue_test(test_issue_1478)
aura_issue_test_link_llvm_jit(test_issue_1478)
# Issue #633: stdlib commercial-evolution reverse-ask observability
# foundation — 1 new Aura primitive
# (query:stdlib-compiler-demands-stats-hash) + 2 new atomics
# (stdlib_extension_count_total + ai_native_primitive_hits_total).
# Primitive has 5 fields: 2 derived sums from existing hot-path
# counters (hotpath-calls + soa-jit-win) + 1 verbatim existing
# counter (error-consistency) + 2 from the new atomics
# (extension-count + ai-native-hits, foundation for AC3+AC4 wire-up,
# currently 0) + schema sentinel. Builds on #479 / #617 / #618 /
# #625 / #709 existing primitives + counters.
aura_add_issue_test(test_issue_633)
aura_issue_test_link_llvm_jit(test_issue_633)
# Issue #1480: incremental re-AOT pipeline (Phase 2) — verifies
# aura_reemit_aot_for_dirty C-linkage surface (Phase 1 skeleton
# fallback, Phase 2 region mask filter, last-call stats accessors,
# 100-iter stress + atomic commit_func_table_swap cadence, closure
# bridge refresh pair metric). 6 ACs, pure C-linkage (no CompilerService,
# no LLVM JIT) so the test links fast even with the system 5-min
# build timeout (per #1478 allowlist precedent).
aura_add_issue_test(test_incremental_aot_closure_deps)
# Issue #1481: typed-error ResourceQuota enforcement helpers
# (check_arena_quota / check_mutation_quota / check_fiber_quota /
# check_time_quota / allocate_checked + AuraErrorKind::ResourceQuotaExceeded
# variant). 7 ACs verify the typed-error surface + counter wiring that
# exists at HEAD (0bfeec38). Imports aura.compiler.evaluator (matching
# test_issue_1476 pattern) so links against the evaluator module.
# Per #1478 / #1480 precedent, this test is also added to test-binding-
# allowlist.txt in case the link hits the system 5-min build timeout
# (per invariant #29). Verification deferred to #1538 batch.
aura_add_issue_test(test_resource_quota)
aura_issue_test_link_llvm_jit(test_resource_quota)
# Issue #1548: Aura-side query:resource-quota-stats + quota edge cases.
aura_add_issue_test(test_primitive_resource_quota_stats)
aura_issue_test_link_llvm_jit(test_primitive_resource_quota_stats)
aura_add_issue_test(test_quota_edge_cases)
aura_issue_test_link_llvm_jit(test_quota_edge_cases)
# Issue #1482: Env::bind_symid PRIMARY storage + lazy bindings_with_names()
# derive + alloc_env_frame_from_env drops legacy bindings_ mirror (SymId-
# mirror only). 8 ACs covering the post-Commit 1/2/3 invariants: bind_symid
# leaves bindings_ empty (Commit 1 eager-mirror drop), binding_index_ also
# untouched (Commit 1 string-keyed shadow drop), bindings_symid_iter() ==
# bindings_symid() canonical accessor, bindings_with_names() resolves via
# pool_ when set + falls back to "@<symid:N>" when null, alloc_env_frame
# _from_env frame.bindings_ stays empty (Commit 2 mirror drop),
# envframe_desync_detected fires for (0, N) shape (Commit 3 doc), and
# perf microbench at N=1000 × 10k reps (iter vs with_names ratio).
# Imports aura.compiler.evaluator (matching test_issue_1476 pattern) so
# links against evaluator module. Per #1478 / #1480 / #1481 precedent,
# added to test-binding-allowlist.txt in case the link hits the system
# 5-min build timeout (per invariant #29). Verification deferred to
# #1538 batch.
aura_add_issue_test(test_env_symid_primary_path)
# Issue #1483: per-fiber mutation_stack_depth metrics (C2 atomics +
# bump helpers + wire at evaluator_fiber_mutation.cpp:316+:454) +
# query:per-fiber-mutation-stack-stats primitive (C3) + adaptive
# GC safepoint threshold with exponential backoff (C4 default
# heuristic (a)) + query:gc-safepoint-adaptive-stats primitive (C4).
# 7 ACs covering: bump_per_fiber_mutation_stack_depth_max + _current_max
# monotonic-CAS, accessor correctness, request_gc_safepoint() immediate
# path resets threshold, bump_safepoint_adaptive_threshold() doubles
# (CAS-capped at 1024), should_adapt_safepoint_threshold() pressure
# signal, request_gc_safepoint() force-defer when pressure > threshold.
# Imports aura.compiler.evaluator (matching test_issue_1476 +
# test_resource_quota + test_env_symid_primary_path pattern) so
# links against evaluator module. Per #1478 / #1480 / #1481 / #1482
# precedent, added to test-binding-allowlist.txt in case the link
# hits the system 5-min build timeout (per invariant #29).
# Verification deferred to #1538 batch.
aura_add_issue_test(test_per_fiber_mutation_safepoint)
# Issue #637: query:closure-bridge-safety-stats-hash — P0
# memory-safety foundation — 1 new Aura primitive
# (query:closure-bridge-safety-stats-hash) + 3 new atomics
# (closure_invalidation_post_mutate_total +
# closure_version_mismatch_caught_total +
# closure_safe_rebuild_total). Primitive has 4 fields: 3 from
# the new atomics (all foundation scaffolding for AC1+AC2+AC3
# enforcement, currently 0) + schema sentinel. Builds on
# #401 / #498 / #531 / #601 / #604 existing counters +
# invalidate_function / bridge_epoch infrastructure.
# Non-duplicative to #620/#623/#624 (see issue body).
aura_add_issue_test(test_issue_637)
aura_issue_test_link_llvm_jit(test_issue_637)
# Issue #640: query:sv-verification-closedloop-stats — P0
# EDA-SV-Review closed-loop foundation — 1 new Aura
# primitive (query:sv-verification-closedloop-stats, NO
# `-hash` suffix — distinct from #630 historical hash
# primitive) + 3 new atomics
# (sv_verify_feedback_apply_total +
# sv_guard_reemit_hook_total +
# sv_stable_ref_provenance_strict_total). Primitive has 4
# fields: 3 from the new atomics (all foundation scaffolding
# for AC1+AC2+AC3 enforcement, currently 0) + schema
# sentinel. Builds on #579 / #630 / #693 / #695 / #697
# existing counters + verification-feedback-loop-stats +
# sv-verification-closedloop-stats-hash primitives.
aura_add_issue_test(test_issue_640)
aura_issue_test_link_llvm_jit(test_issue_640)
# Issue #641: query:stable-ref-provenance-sv-stats — P0
# EDSL-Review cross-fiber provenance enforcement foundation
# — 1 new Aura primitive (query:stable-ref-provenance-sv-stats,
# NO `-hash` suffix — distinct from #631 historical hash
# primitive) + 3 new atomics
# (stable_ref_fiber_provenance_check_total +
# stable_ref_provenance_auto_refresh_total +
# stable_ref_sv_feedback_wired_total). Primitive has 4 fields:
# 3 from the new atomics (all foundation scaffolding for
# AC1+AC2+AC4 enforcement, currently 0) + schema sentinel.
# Builds on #604 / #631 / #640 existing primitives +
# StableNodeRef provenance infrastructure.
aura_add_issue_test(test_issue_641)
aura_issue_test_link_llvm_jit(test_issue_641)
# Issue #642: query:arena-auto-compaction-stats — P0
# Prompt6-MemorySafety Arena Auto-Compaction + Fiber/GC
# Safepoint Coordination foundation — 1 new Aura primitive
# (query:arena-auto-compaction-stats, NOTE `-compaction` with
# `-ion` suffix — distinct from existing `-compact-stats` /
# `-compact-defrag-stats`) + 3 new atomics
# (arena_auto_compact_trigger_total +
# arena_live_move_yield_total +
# arena_guard_request_defrag_total). Primitive has 4 fields:
# 3 from the new atomics (all foundation scaffolding for
# AC1+AC2+AC3 enforcement, currently 0) + schema sentinel.
# Builds on #569 arena-auto-compact-defrag-stats +
# earlier arena-auto-compact-stats + arena-auto-stats +
# arena-compaction-stats existing primitives.
aura_add_issue_test(test_issue_642)
aura_issue_test_link_llvm_jit(test_issue_642)
# Issue #643: query:primitives-meta [name] — P0 Stdlib-Impl-P1
# AI-native primitives-meta introspection (implements #633
# AC3+AC4) — 1 new Aura primitive (query:primitives-meta
# [name], distinct from #498 query:primitive-metadata via
# the `s` suffix + optional [name] arg dispatch) + 3 new
# atomics (define_primitive_macro_used_total +
# prim_error_unified_total + primitives_meta_query_total).
# Primitive has 2 forms: no-arg (aggregate foundation
# counters + schema sentinel) + per-name-lookup (name +
# arity + has-fn + category + schema sentinel). Builds on
# #498 / #559 / #617 / #618 / #625 existing primitives +
# primitives_meta_catalog_query_total hit-rate counter.
aura_add_issue_test(test_issue_643)
aura_issue_test_link_llvm_jit(test_issue_643)
# Issue #644: query:aot-reload-func-table-stats — P0
# Runtime-Gap + AOT hot-reload func_table Refcount +
# Per-Region Isolation foundation — 1 new Aura primitive
# (query:aot-reload-func-table-stats, distinct from #708
# query:aot-reload-stats via the `-func-table` prefix
# enforcing-layer focus) + 3 new atomics
# (aot_func_table_ref_bump_total +
# aot_func_table_ref_decrement_total +
# aot_region_filter_reapply_total). Primitive has 4 fields:
# 3 from the new atomics (all foundation scaffolding for
# AC1+AC2+AC4 enforcement, currently 0) + schema sentinel.
# Builds on #358 / #452 / #708 existing primitives +
# AOT hot-reload infrastructure. Non-duplicative to
# #624/#601/#358 (see issue body).
aura_add_issue_test(test_issue_644)
aura_issue_test_link_llvm_jit(test_issue_644)
# Issue #645: query:scheduler-steal-bias-stats — P0
# Runtime-Gap + Scheduler Work-Stealing LIFO/FIFO Adaptive
# Bias + YieldReason / outermost Mutation Depth foundation
# — 1 new Aura primitive (query:scheduler-steal-bias-stats,
# distinct from #706 query:scheduler-stealbudget-adaptive-
# stats via the `-bias-` midfix) + 3 new atomics
# (scheduler_lifo_hits_total +
# scheduler_fifo_steals_total +
# scheduler_mutation_deferred_bias_total). Primitive has 4
# fields: 3 from the new atomics (all foundation scaffolding
# for AC1+AC2+AC4 enforcement, currently 0) + schema
# sentinel. Builds on #451 / #588 / #618 / #706 existing
# scheduler / fiber / yield_reason infrastructure.
# Non-duplicative to #618/#588/#451 (see issue body).
aura_add_issue_test(test_issue_645)
aura_issue_test_link_llvm_jit(test_issue_645)
# Issue #646: query:gc-safepoint-deferral-stats — P0
# Runtime-Gap + GC Safepoint Deferral + Backoff Only for
# Outermost MutationBoundary foundation — 1 new Aura
# primitive (query:gc-safepoint-deferral-stats, distinct
# from existing query:gc-safepoint-stats via the
# `-deferral-` midfix) + 3 new atomics
# (gc_outermost_deferral_total +
# gc_inner_proceeded_total +
# gc_backoff_trigger_total). Primitive has 4 fields: 3
# from the new atomics (all foundation scaffolding for
# AC1+AC2+AC4 enforcement, currently 0) + schema sentinel.
# Builds on existing query:gc-safepoint-stats + #591 gc
# pause attribution + #588 per-fiber stack + #623 arena +
# GC safepoint + #642 arena auto-compaction.
# Non-duplicative to #642/#623/#591 (see issue body).
aura_add_issue_test(test_issue_646)
aura_issue_test_link_llvm_jit(test_issue_646)
# Issue #647: query:envframe-dualpath-stale-stats-hash — P0
# Runtime-Gap + SoA Dual-Path EnvFrame/Env Cross-Fiber Stale
# Detection + materialize_call_env After Steal foundation —
# 1 new Aura primitive (query:envframe-dualpath-stale-stats-
# hash, distinct from existing #418 query:envframe-dualpath-
# stale-stats flat-int primitive via the `-hash` suffix —
# matches the #630/#641 hash-vs-int naming convention)
# + 3 new atomics (envframe_cross_fiber_stale_total +
# envframe_version_mismatch_post_steal_total +
# envframe_dualpath_repair_total). Primitive has 4 fields:
# 3 from the new atomics (all foundation scaffolding for
# AC1+AC2+AC4 enforcement, currently 0) + schema sentinel.
# Builds on #418 / #589 / #355 / #637 existing primitives
# + EnvFrame SoA infrastructure. Non-duplicative to
# #637/#589/#355 (see issue body).
aura_add_issue_test(test_issue_647)
aura_issue_test_link_llvm_jit(test_issue_647)
# Issue #648: query:panic-checkpoint-fiber-stats — P0
# Runtime-Gap + Panic Checkpoint + Yield Storage Lifecycle +
# INVALID_VERSION Frame Handling in Fiber Resume +
# Concurrent GC foundation — 1 new Aura primitive
# (query:panic-checkpoint-fiber-stats, distinct from
# existing query:panic-checkpoint-lifecycle-stats via
# the `-fiber-` midfix) + 3 new atomics
# (panic_transfer_on_resume_total +
# panic_invalid_frames_skipped_total +
# panic_concurrent_gc_conflict_total). Primitive has 4
# fields: 3 from the new atomics (all foundation scaffolding
# for AC1+AC2+AC3 enforcement, currently 0) + schema
# sentinel. Builds on existing
# query:panic-checkpoint-lifecycle-stats + #264 yield
# checkpoint + #356 INVALID_VERSION + #637 IRClosure/EnvFrame
# versioning + #588 per-fiber stack + #591 GC pause
# attribution. Non-duplicative to #637/#356/#264 (see issue
# body).
aura_add_issue_test(test_issue_648)
aura_issue_test_link_llvm_jit(test_issue_648)
# Issue #649: query:yield-checkpoint-panic-stats — P0
# Runtime-Gap + Panic Full Per-Fiber YieldCheckpointStorage
# Re-Stamp + Size Validation on Panic Transfer + Cross-Steal
# foundation — 1 new Aura primitive
# (query:yield-checkpoint-panic-stats, distinct from #648
# query:panic-checkpoint-fiber-stats via the `yield-checkpoint-`
# prefix — focus on yield checkpoint STORAGE LIFECYCLE
# rather than panic CHECKPOINT TRANSPORT) + 3 new atomics
# (yield_transfer_with_restamp_total +
# yield_size_mismatch_caught_total +
# yield_cross_steal_invalidation_total). Primitive has 4
# fields: 3 from the new atomics (all foundation scaffolding
# for AC1+AC2+AC3 enforcement, currently 0) + schema
# sentinel. Builds on #648 panic-checkpoint-fiber-stats +
# #264 yield checkpoint + #588 per-fiber stack +
# transfer_panic_checkpoint_trampoline +
# restore_post_yield_or_rollback. Non-duplicative to
# #648/#264 (see issue body).
aura_add_issue_test(test_issue_649)
aura_issue_test_link_llvm_jit(test_issue_649)
# Issue #650: query:scheduler-stealbudget-yield-class-stats —
# P0 Runtime-Gap + Scheduler StealBudget to Use fiber
# yield_classification() + Outermost Mutation Depth for
# Adaptive Bias foundation — 1 new Aura primitive
# (query:scheduler-stealbudget-yield-class-stats, distinct
# from #706 query:scheduler-stealbudget-adaptive-stats via
# the `-yield-class-` midfix — focus on yield-class bias
# enforcement layer rather than 5-field adaptive bias
# summary) + 3 new atomics (stealbudget_outermost_bias_total
# + stealbudget_explicit_bias_total +
# stealbudget_max_before_sleep_raised_total). Primitive has
# 4 fields: 3 from the new atomics (all foundation
# scaffolding for AC1+AC2+AC4 enforcement, currently 0) +
# schema sentinel. Builds on #706 + #645 existing
# scheduler / StealBudget infrastructure.
# Non-duplicative to #645 (see issue body).
aura_add_issue_test(test_issue_650)
aura_issue_test_link_llvm_jit(test_issue_650)
# Issue #651: query:gc-panic-deferral-stats — P0 Runtime-
# Gap + GC Actual Deferral/Block Logic in
# block_gc_for_pending_checkpoint_trampoline + Request
# Shim (fills TODO in evaluator_fiber_mutation.cpp) —
# 1 new Aura primitive (query:gc-panic-deferral-stats,
# distinct from #646 query:gc-safepoint-deferral-stats
# via the `-panic-` midfix — focus on GC+panic coordination
# layer that fills the TODO) + 3 new atomics
# (gc_panic_pending_deferral_total +
# gc_blocked_by_panic_total +
# gc_panic_conflict_resolved_total). Primitive has 4 fields:
# 3 from the new atomics (all foundation scaffolding for
# AC1+AC2+AC3 enforcement, currently 0) + schema sentinel.
# Builds on #646 + #648 existing GC + panic observability
# primitives. Non-duplicative to #646/#648 (see issue body).
aura_add_issue_test(test_issue_651)
aura_issue_test_link_llvm_jit(test_issue_651)
# Issue #589: query:envframe-dualpath-enforce-stats — P0
# Runtime-Review + SoA EnvFrame/EnvId dual-path bindings_
# vs bindings_symid_ consistency + version stamping +
# stale refresh in materialize_call_env & GCEnvWalkFn
# foundation — 1 new Aura primitive
# (query:envframe-dualpath-enforce-stats, distinct from
# existing flat-int query:envframe-dualpath-stats +
# query:envframe-dualpath-stale-stats and from #647
# query:envframe-dualpath-stale-stats-hash via the
# `-enforce-` midfix — focus on SoA dual-path
# CONSISTENCY enforcement layer that the existing
# stale-focused primitives don't cover) + 3 new atomics
# (envframe_dualpath_mirror_write_total +
# envframe_dualpath_refresh_total +
# envframe_dualpath_consistency_violations_total).
# Primitive has 4 fields: 3 from the new atomics (all
# foundation scaffolding for AC1+AC2+AC3 enforcement,
# currently 0) + schema sentinel (matches issue number
# 589 even though #589 is older than #651). Builds on
# existing query:envframe-dualpath-stats +
# query:envframe-dualpath-stale-stats + #647
# envframe-dualpath-stale-stats-hash + #543 SoA EnvFrame +
# #568 children SoA + #205 GCEnvWalkFn + the existing
# envframe_desync_detected_ / envframe_stale_refresh_count_
# / envframe_post_rollback_invalidations_ +
# envframe_version_mismatch_in_walk_ +
# envframe_gc_walk_safe_skips_ + gc_envframe_stale_skipped_
# counters. Non-duplicative to existing #543/#568/#205
# (see issue body).
aura_add_issue_test(test_issue_589)
aura_issue_test_link_llvm_jit(test_issue_589)
# Issue #590: query:aot-hotupdate-stats — P0 Runtime-Review +
# AOT mangle versioning + region filtering + multi-agent
# hot-update isolation + closure dispatch stale detection
# foundation — 1 new Aura primitive
# (query:aot-hotupdate-stats, distinct from existing
# query:aot-reload-stats #708 +
# query:aot-reload-func-table-stats #644 +
# query:aot-hot-reload-stats #358/#452 via the `-hotupdate-`
# midfix — focus on multi-agent hot-update isolation
# enforcement layer) + 3 new atomics
# (aot_hotupdate_region_isolation_total +
# aot_hotupdate_dispatch_stale_prevented_total +
# aot_hotupdate_multi_agent_reload_total). Primitive has 4
# fields: 3 from the new atomics (all foundation scaffolding
# for AC1+AC2+AC3 enforcement, currently 0) + schema sentinel
# (590 matches issue number). Builds on #544 / #323 / #287
# existing AOT versioning + hot-update infrastructure.
# Non-duplicative to existing #544/#323/#287 (see issue body).
aura_add_issue_test(test_issue_590)
aura_issue_test_link_llvm_jit(test_issue_590)
# Issue #479: per-prim fast-path hit tracking + (query:primitive-fastpath-per-prim).
aura_add_issue_test(test_issue_479)
# test uses (list ...) + (map ...) + (filter ...) + (apply ...) primitives which
# reference FlatHashTable::create (defined in aura_jit_runtime.cpp); that
# symbol isn't in libaura_test_objects.a, so we link LLVM JIT sources in.
aura_issue_test_link_llvm_jit(test_issue_479)
# Issue #473 Tier 1: serve-async security hardening (JSON escape + stdin cap + auth/curl/platform).
aura_add_issue_test(test_issue_473)
target_compile_definitions(test_issue_473 PRIVATE AURA_TEST_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
# Issue #464: Arena auto-compaction lifecycle + counter + primitive.
aura_add_issue_test(test_issue_464_arena_auto_compaction)
aura_issue_test_link_llvm_jit(test_issue_464_arena_auto_compaction)
aura_add_issue_test(test_issue_465_cxx26_hotpath)
aura_issue_test_link_llvm_jit(test_issue_465_cxx26_hotpath)
aura_add_issue_test(test_issue_470_stable_ref_sv_scale)
aura_issue_test_link_llvm_jit(test_issue_470_stable_ref_sv_scale)
aura_add_issue_test(test_issue_471_dirty_sv_scale)
aura_issue_test_link_llvm_jit(test_issue_471_dirty_sv_scale)
# Issue #1485: stale-closure defense in depth — 2 new atomics on
# CompilerMetrics (stale_closure_prevented, closure_epoch_mismatch_fallback)
# + Evaluator accessor / bump helpers + per-closure provenance accessors
# (aura_get_closure_bridge_epoch, aura_get_closure_defuse_version) +
# C-side bridge wire (service.ixx::bump_bridge_epoch now also calls
# aura_set_current_bridge_epoch). Pure C-linkage + Evaluator helpers,
# no LLVM JIT. Test file linked via allowlist per #1453/#1478/#1480
# precedent (build verify defers to #1538 per invariant #29).
aura_add_issue_test(test_issue_1485)
add_dependencies(all_test_issue_targets test_issue_219)
add_dependencies(all_test_issue_targets test_issue_220)
add_dependencies(all_test_issue_targets test_issue_221)
add_dependencies(all_test_issue_targets test_issue_222)
add_dependencies(all_test_issue_targets test_issue_223)


# Issue #677: production install layout (extends #374).
include(GNUInstallDirs)
install(TARGETS aura RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/lib/runtime.c
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aura)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/demos
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aura
        OPTIONAL)

# File size lint (.ixx + large .cpp/.h caps). See scripts/file_size_policy.md.
# On demand: cmake --build build --target lint_file_size
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(lint_file_size
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_file_size.py
                --src ${CMAKE_SOURCE_DIR}/src
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Linting source file sizes (.ixx + cpp caps)"
        VERBATIM)
endif()
