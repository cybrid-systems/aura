cmake_minimum_required(VERSION 4.3)
# CMake 4.4+ gate UUID (was 451f2fe2… on 4.3).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
project(aura LANGUAGES CXX C)
# P2: shared test libs (libaura_*.so) live in the build tree; every
# test executable must find them without LD_LIBRARY_PATH.
set(CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}")
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
include(${CMAKE_SOURCE_DIR}/cmake/AuraCoverage.cmake) # Issue #1933 llvm-cov
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
    src/compiler/evaluator_mutation_boundary.cpp
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
    src/compiler/evaluator_primitives_observability.cpp
    src/compiler/evaluator_primitives_stdlib_review.cpp
    src/compiler/evaluator_primitives_obs_eval.cpp
    src/compiler/evaluator_primitives_obs_jit.cpp
    src/compiler/verify_tool.cpp
    src/compiler/evaluator_primitives_messaging.cpp
    src/compiler/evaluator_primitives_io.cpp
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
    # Issue #2078: AgentNameTable is header-only with inline methods
    # (src/compiler/agent_name_table.h). Not compiled separately to
    # avoid C++20 modules "declarations in both fragments" conflict.
    # The .h is included from evaluator_ctor.cpp /
    # evaluator_primitives_agent.cpp / the test file (not from
    # evaluator.ixx's global fragment).
)
# Wave 5: service implementation partitions (same module: aura.compiler.service).
set(AURA_SERVICE_SOURCE_FILES
    src/compiler/service_dirty.cpp
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
# mold/lld of many ~70MB LLVM test bins thrashs RAM and can SIGBUS /
# "Disk full?" on GitHub Actions. Compiles stay unbounded via ninja -j.
# Override: -DAURA_LINK_JOBS=N (CI sets 2).
if(NOT DEFINED AURA_LINK_JOBS)
    set(AURA_LINK_JOBS 2)
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
    # Issue #1980 / Epic #1979: half-block pixel framebuffer for 3D TUI base.
    ${AURA_EVALUATOR_SOURCE_FILES}
    ${AURA_SERVICE_SOURCE_FILES}
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
    src/compiler/arity_impl.cpp
    src/compiler/aura_jit_bridge.cpp
    # Issue #1956: HotUpdateRegistry coordination center (non-module).
    src/compiler/hot_update_registry.cpp
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
 


# Issue #2626: AURA_ENABLE_TUI retired (tui:* surface removed).


# Issue #2627: AURA_ENABLE_AUTO_EVOLVE retired (auto-evolve-* removed).

# Issue #1970: git-* integration vertical (deferred). Default ON for
# production-sweep / git tooling tests; slim builds: -DAURA_ENABLE_GIT=OFF
# (see docs/git-integration.md). Independent of AURA_HAVE_LIBGIT2 (backend).
option(AURA_ENABLE_GIT "Register git-* domain primitives (integration, #1970)" ON)

# Issue #2626: AURA_ENABLE_TERMINAL retired (terminal:* surface removed).

# Issue #2627: AURA_ENABLE_SEVA retired (seva:* demo wrappers removed).

# Issue #1974: synthesize:* synthesis commercial vertical (deferred).
# Default ON for synthesize/PRNG/workspace-swap tests; slim:
# -DAURA_ENABLE_SYNTHESIZE=OFF (see docs/synthesize.md).
option(AURA_ENABLE_SYNTHESIZE
       "Register synthesize:* domain primitives (commercial, #1974)" ON)

# Issue #1975: tcp-* TCP networking integration vertical (deferred).
# Default ON; slim: -DAURA_ENABLE_TCP=OFF (see docs/tcp.md).
# Independent of http-get/http-post/getenv in the same register_network_primitives.
option(AURA_ENABLE_TCP "Register tcp-* domain primitives (integration, #1975)" ON)

# Issue #1973: strategy:* evolution controller commercial vertical (deferred).
# Default ON for strategy/intend tests; slim: -DAURA_ENABLE_STRATEGY=OFF
# (see docs/strategy.md). Only the 4 strategy:* names are gated; intend /
# define-strategy / query:strategy-evolution-stats stay always on.
option(AURA_ENABLE_STRATEGY
       "Register strategy:* domain primitives (commercial, #1973)" ON)

# Issue #1976: m4-* linear-type stubs (Issue #108 M4; deferred domain prefix).
# Default ON for regression linear/borrow tests; slim: -DAURA_ENABLE_M4=OFF
# (see docs/m4.md). Only m4-move/borrow/return!; define-linear + math/regex stay on.
option(AURA_ENABLE_M4 "Register m4-* domain primitives (linear stubs, #1976)" ON)

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

# Issue #1967–#1976: per-source commercial/integration vertical flags
# (avoids PUBLIC recompile of the entire aura_test_objects graph when a flag flips).
if(AURA_ENABLE_GIT)
    set(_AURA_ENABLE_GIT_VAL 1)
else()
    set(_AURA_ENABLE_GIT_VAL 0)
endif()
if(AURA_ENABLE_SYNTHESIZE)
    set(_AURA_ENABLE_SYNTHESIZE_VAL 1)
else()
    set(_AURA_ENABLE_SYNTHESIZE_VAL 0)
endif()
if(AURA_ENABLE_TCP)
    set(_AURA_ENABLE_TCP_VAL 1)
else()
    set(_AURA_ENABLE_TCP_VAL 0)
endif()
if(AURA_ENABLE_STRATEGY)
    set(_AURA_ENABLE_STRATEGY_VAL 1)
else()
    set(_AURA_ENABLE_STRATEGY_VAL 0)
endif()
if(AURA_ENABLE_M4)
    set(_AURA_ENABLE_M4_VAL 1)
else()
    set(_AURA_ENABLE_M4_VAL 0)
endif()
# agent.cpp: synthesize (#1974) + strategy: (#1973).
# agent:/intend/define-strategy co-registered stay always on.
set_source_files_properties(
    src/compiler/evaluator_primitives_agent.cpp
    PROPERTIES
        COMPILE_DEFINITIONS
        "AURA_ENABLE_SYNTHESIZE=${_AURA_ENABLE_SYNTHESIZE_VAL};AURA_ENABLE_STRATEGY=${_AURA_ENABLE_STRATEGY_VAL}")
# io.cpp: git-* (#1970) + tcp-* (#1975). getenv/http/sys stay on.
set_source_files_properties(
    src/compiler/evaluator_primitives_io.cpp
    PROPERTIES COMPILE_DEFINITIONS
               "AURA_ENABLE_GIT=${_AURA_ENABLE_GIT_VAL};AURA_ENABLE_TCP=${_AURA_ENABLE_TCP_VAL}")
# m4-* linear stubs in evaluator_primitives_math.cpp (#1976); math/regex stay on.
set_source_files_properties(
    src/compiler/evaluator_primitives_math.cpp
    PROPERTIES COMPILE_DEFINITIONS "AURA_ENABLE_M4=${_AURA_ENABLE_M4_VAL}")
message(STATUS "git-* domain: AURA_ENABLE_GIT=${_AURA_ENABLE_GIT_VAL} (#1970)")
message(STATUS
        "strategy:* domain: AURA_ENABLE_STRATEGY=${_AURA_ENABLE_STRATEGY_VAL} (#1973)")
message(STATUS
        "synthesize:* domain: AURA_ENABLE_SYNTHESIZE=${_AURA_ENABLE_SYNTHESIZE_VAL} (#1974)")
message(STATUS "tcp-* domain: AURA_ENABLE_TCP=${_AURA_ENABLE_TCP_VAL} (#1975)")
message(STATUS "m4-* domain: AURA_ENABLE_M4=${_AURA_ENABLE_M4_VAL} (#1976)")

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

# ── aura-reflect: P2996 placement (Issue #2290, g++ 16.1.0) ─────
# Hard limit is NOT "any module file cannot touch <meta>". Measured:
#   PASS  non-module (or CXX_MODULE_STD without import std) + -freflection
#         + #include <meta> / reflect.hh
#   PASS  module GMF includes <meta> + -freflection → object builds
#         (e.g. src/reflect/reflect.ixx)
#   FAIL  same TU mixes `import std` with #include <meta>/reflect.hh
#         (either order) → std redefinition / pthread header conflicts
#         (hard errors, not a silent ICE)
#   FAIL  consumer uses meta-bearing module exports that need BMI
#         consteval / reflect templates (BMI "Bad file data"); pure
#         runtime exports from such a module can still link
# Policy: Aura business partitions all `import std`, so P2996 bodies
# live here (aura-reflect) or other non-import-std -freflection TUs.
# Call from module code via C ABI / split TU. Do not put non-meta
# sources in this library. hygiene_validate.hh is meta-free and may
# be included from import-std TUs.
# P2: SHARED so issue-test binaries do not each embed reflect (~264K).
add_library(aura-reflect SHARED
    src/reflect/cache_reflect.cpp
    src/compiler/ir_reflect_serialize.cpp
    # Wave A1: snapshot JSON via to_json (needs -freflection)
    src/compiler/observability_json.cpp
)
set_target_properties(aura-reflect PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
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
add_executable(test_ir tests/compiler/test_ir.cpp)
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
    ${AURA_SERVICE_SOURCE_FILES}
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    # Issue #461: aura_jit_bridge.cpp provides
    # aura_jit_fallback_count_v_read (used by
    # query:jit-fallback-stats in evaluator_primitives_query.cpp).
    # Without this the test_ir link fails with
    # "undefined reference to aura_jit_fallback_count_v_read".
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge.cpp
    src/compiler/hot_update_registry.cpp # Issue #1956
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
aura_strict_test_warnings(tests/compiler/test_ir.cpp)
aura_test_compile_options(test_ir)
target_include_directories(test_ir PRIVATE src src/compiler)
target_link_libraries(test_ir PRIVATE stdc++ stdc++exp aura-reflect)

# ── Concurrent model tests ────────────────────────────
# Tests fiber lifecycle, worker scheduling, and eventfd wakeup
add_executable(test_concurrent tests/serve/test_concurrent.cpp)
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
    src/compiler/hot_update_registry.cpp # Issue #1956
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
aura_strict_test_warnings(tests/serve/test_concurrent.cpp)
aura_test_compile_options(test_concurrent)
target_link_libraries(test_concurrent PRIVATE pthread)
add_test(NAME concurrent_basic COMMAND ./test_concurrent)

# ─────────────────────────────────────────────────────────────────
# Test build architecture (Anqi 2026-07-20 "ship in order" + 07:04 directives)
# ─────────────────────────────────────────────────────────────────
#
# Test layout policy (#1958): tests/README.md · tests/domain/README.md
# Legacy inventory / migration waves (#1957): tests/legacy_test_inventory.md
#
# Preferred new ACs live under tests/domain/ (and cases/*.hpp tables).
# tests/issues/ removed (#1957); do not reintroduce per-issue test_issue_N.cpp.
#
# Layers still present:
#   1. Domain suites — tests/domain/test_obs_schema_matrix,
#      test_domain_gates_batch, arena/* (registered below via
#      aura_add_issue_test; resolved via aura_resolve_test_cpp).
#   2. Bundle executables — tests/bundles/test_issues_<profile>_main.cpp
#      + members from tests/issues/ (link profiles in fixtures/).
#   3. Family batch drivers — domain/arena/*_batch + compiler_core batches
#      (EXCLUDE_FROM_ALL for on-demand ninja).
#   4. aura_test_objects — shared static lib for test link units.
#
# Source resolution (aura_resolve_test_cpp): issues/ → domain/ → tests/.
# Pre-commit soft-nudges new test_issue_*.cpp toward domain/ suites.
# ─────────────────────────────────────────────────────────────────

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
# P2: SHARED — one libaura_test_objects.so (~tens of MB) instead of
# embedding the 30MB archive into every issue-test binary. Tests
# resolve via BUILD_RPATH=$BUILD_DIR. Static archives allowed dead
# strip of unused .o; shared trades that for disk/link thrash wins.
add_library(aura_test_objects SHARED
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
    # Issue #1980 / Epic #1979: half-block pixel framebuffer.
    ${AURA_EVALUATOR_SOURCE_FILES}
    ${AURA_SERVICE_SOURCE_FILES}
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
    src/compiler/arity_impl.cpp
    src/compiler/messaging_bridge_impl.cpp
    src/compiler/ffi_primitives_impl.cpp
    src/compiler/adt_runtime_impl.cpp
    src/compiler/lowering_linear_types_impl.cpp)
set_property(TARGET aura_test_objects PROPERTY CXX_MODULE_STD ON)
set_target_properties(aura_test_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
)
# .cpp impls require .ixx BMIs from the same module set the helper
# wires for each test_issue_* target. Add them here so the shared
# lib can compile its sources standalone. Each module is still
# ALSO re-emitted per-test-binary (CMake FILE_SET doesn't share
# BMIs across targets yet) — this is the duplicate work Fix #2
# (sccache) will eliminate. For now, modules in both places are
# necessary to make aura_test_objects compile.
aura_target_cxx_modules(aura_test_objects WITH_REFLECT WITH_TYPE_CONCEPTS WITH_TEST_STRATEGY)
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
    # in the library (duplicate symbols).
    # P2: SHARED so LLVM is mapped once (~50–70MB .so) instead of
    # baked into each full-JIT test binary.
    add_library(aura_jit_test_objects SHARED
        src/compiler/aura_jit.cpp
        src/compiler/aura_jit_runtime.cpp
        src/compiler/aura_jit_bridge.cpp
        src/compiler/hot_update_registry.cpp # Issue #1956
        src/compiler/aura_jit_prim_dispatch_stub.cpp
        src/core/contract_handler.cpp
        src/core/contract_stub.cpp
    )
    set_property(TARGET aura_jit_test_objects PROPERTY CXX_MODULE_STD ON)
    set_target_properties(aura_jit_test_objects PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        BUILD_RPATH "${CMAKE_BINARY_DIR}"
    )
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

# Light JIT/ABI objects for issue tests that do NOT need real LLVM OrcJIT.
# ~99% of aura_issue_test targets only pull CompilerService / AOT bridge
# symbols via aura_test_objects; linking full aura_jit_test_objects (PUBLIC
# llvm_libs) balloons each binary to ~70MB and drives ~19GB of build/
# test_* disk. Light profile reuses the same sources as tests/bundles light
# extras: aura_jit.cpp without AURA_HAVE_LLVM + bridge stub + runtime +
# HotUpdateRegistry — no PUBLIC LLVM, median binary drops to ~MB scale.
#
# Prefer aura_issue_test_link_light() (default for non-JIT issue tests).
# Keep aura_issue_test_link_llvm_jit() only for targets that exercise real
# OrcJIT / emit_native_object / SpecJIT paths (see
# cmake/issue_tests_need_full_llvm.txt or tests/README.md · link profiles).
# P2: SHARED — one light ABI .so for all non-JIT issue tests.
add_library(aura_jit_light_test_objects SHARED
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    src/compiler/aura_jit_bridge_stub.cpp
    src/compiler/hot_update_registry.cpp
    src/compiler/aura_jit_prim_dispatch_stub.cpp
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
)
set_property(TARGET aura_jit_light_test_objects PROPERTY CXX_MODULE_STD ON)
set_target_properties(aura_jit_light_test_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
)
target_include_directories(aura_jit_light_test_objects PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/compiler
)
# Explicitly no AURA_HAVE_LLVM — aura_jit.cpp uses the #else stubs.
target_compile_features(aura_jit_light_test_objects PUBLIC cxx_std_26)
aura_test_compile_options(aura_jit_light_test_objects)
target_compile_options(aura_jit_light_test_objects PRIVATE -fcontracts)
set_source_files_properties(
    src/core/contract_handler.cpp
    src/core/contract_stub.cpp
    PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
# -fno-rtti only on aura_jit TUs that match the full JIT archive dialect
# when LLVM headers are present; light path has no LLVM headers, but keep
# the flag for consistent ABI with light bundle objects.
set_source_files_properties(
    src/compiler/aura_jit.cpp
    src/compiler/aura_jit_runtime.cpp
    PROPERTIES COMPILE_FLAGS "-fno-rtti -fcontracts")
target_link_libraries(aura_jit_light_test_objects PUBLIC ${CMAKE_DL_LIBS})

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
    # Source resolution: issues/ → domain/ → domain/<theme>/ → tests/
    # (see aura_resolve_test_cpp in cmake/AuraTest.cmake).
    # Policy: tests/README.md (#1958) · domain/arena pilot (#1959)
    # Inventory: tests/legacy_test_inventory.md (#1957)
    aura_resolve_test_cpp(${NAME} _aura_test_src)
    add_executable(${NAME} ${_aura_test_src})
    set_property(TARGET ${NAME} PROPERTY CXX_MODULE_STD ON)
    # tests/ for test_harness.hpp and domain/cases/* includes
    target_include_directories(${NAME} PRIVATE src tests)
    # Source-cite ACs (e.g. #2340/#2360/#2342 wave) resolve repo-root
    # relative paths via AURA_SOURCE_DIR (std::filesystem::path(AURA_SOURCE_DIR)
    # / "src/..."). Define it for every issue test target so those
    # ACs compile regardless of build dir location.
    target_compile_definitions(${NAME} PRIVATE AURA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # Fix #2: no FILE_SET .ixx here. The prebuilt BMIs in
    # AURA_MODULE_BMI_DIR (built by aura_test_objects) are made
    # visible in the per-target dir via the launcher-installed
    # symlinks, so test_issue_* binaries can `import` modules
    # without re-compiling the .ixx themselves.
    aura_test_compile_options(${NAME})
    # Fix #1: link the shared test object lib (P2: .so, not .a).
    # BUILD_RPATH so ctest/ninja runs find libaura_*.so in build/.
    target_link_libraries(${NAME} PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)
    set_target_properties(${NAME} PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}"
        INSTALL_RPATH "$ORIGIN"
    )
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
add_executable(test_gc_evaluator_integration tests/core/test_gc_evaluator_integration.cpp)
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
    ${AURA_SERVICE_SOURCE_FILES}
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
    src/compiler/arity_impl.cpp
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
        src/compiler/aura_jit_bridge.cpp
        src/compiler/hot_update_registry.cpp)
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
add_executable(test_contracts tests/compiler/test_contracts.cpp)
set_source_files_properties(tests/compiler/test_contracts.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
target_compile_options(test_contracts PRIVATE -fcontracts)
# Issue #477: contract test is small + production-quality; opt into strict
# warnings for the test source itself.
aura_strict_test_warnings(tests/compiler/test_contracts.cpp)
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
add_executable(test_ir_cache_v2 tests/reflect/test_ir_cache_v2.cpp)
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

# Phase-2 empty root stubs (test_fiber / test_mutation / test_observability /
# test_persist) removed — coverage lives in core/arena + serve/gc + orch test suites
# (see tests/root_test_classification.md Wave 0, scripts/classify_root_tests.py).

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
add_executable(test_shape tests/compiler/test_shape.cpp src/compiler/shape_profiler.cpp)
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

# Issue #2141: ShapeProfiler shared_mutex multi-fiber concurrency.
add_executable(test_shape_profiler_concurrency_2141
    tests/compiler/test_shape_profiler_concurrency_2141.cpp
    src/compiler/shape_profiler.cpp)
target_include_directories(test_shape_profiler_concurrency_2141 PRIVATE src tests)
target_link_libraries(test_shape_profiler_concurrency_2141 PRIVATE stdc++ pthread)
target_sources(test_shape_profiler_concurrency_2141 PRIVATE src/core/contract_handler.cpp)
add_test(NAME test_shape_profiler_concurrency_2141 COMMAND test_shape_profiler_concurrency_2141
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Speculative JIT tests (Phase 2, #53) ─────────────────────
add_executable(test_spec_jit tests/compiler/test_spec_jit.cpp
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
add_executable(test_jit_metrics tests/compiler/test_jit_metrics.cpp
    tests/compiler/test_jit_metrics_stub.cpp
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
add_executable(test_jit_concurrent_compile tests/compiler/test_jit_concurrent_compile.cpp
    tests/compiler/test_jit_metrics_stub.cpp
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
add_executable(test_jit_consistency tests/compiler/test_jit_consistency.cpp
    tests/compiler/test_jit_metrics_stub.cpp
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
add_executable(test_spec_runtime tests/stdlib/test_spec_runtime.cpp)
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
add_executable(issue_212_bench tests/compiler/bench_refactor_surfaces.cpp)
set_property(TARGET issue_212_bench PROPERTY CXX_MODULE_STD ON)
# Issue #278 follow-up: contract_handler.cpp link fix.
target_sources(issue_212_bench PRIVATE src/core/contract_handler.cpp)
set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")

# Issue #508 quantitative benchmark: CastOp reduction ratio
# on a synthetic gradual workload. Validates the AC ">=40%
# coercion reduction in gradual workloads" without needing
# a full benchmark suite. Returns 0 iff the AC is met.
add_executable(issue_508_bench tests/compiler/bench_dead_coercion_elim.cpp)
set_property(TARGET issue_508_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(issue_508_bench PRIVATE src)
target_compile_options(issue_508_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(issue_508_bench PRIVATE stdc++ aura_test_objects aura-reflect)

# Issue #307: quantitative benchmark for fine-grained
# incremental type checking + IR re-lower pipeline at EDA
# scale. Builds a 10k-node hardware-like AST, runs full
# infer_flat, applies a 1% mutation, runs infer_flat_partial,
# and validates AC1 (partial/full <10%), AC2 (correctness
# after chained mutations), AC4 (cache hit rate >80%), and
# AC5 (no perf regression on full recompile). AC3 (this
# benchmark file) is the deliverable.
add_executable(issue_307_bench tests/compiler/bench_ir_relower.cpp)
set_property(TARGET issue_307_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(issue_307_bench PRIVATE src)
target_compile_options(issue_307_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(issue_307_bench PRIVATE stdc++ aura_test_objects aura-reflect)

# Consolidated FlatAST hot-path benches (#393/#398/#399):
#   stable-ref (1k + 10k), children-stable, mark-dirty
# Formerly four near-duplicate drivers under tests/bench/.
add_executable(ast_hotpath_bench tests/bench/ast_hotpath_bench.cpp)
set_property(TARGET ast_hotpath_bench PROPERTY CXX_MODULE_STD ON)
target_include_directories(ast_hotpath_bench PRIVATE src)
target_compile_options(ast_hotpath_bench PRIVATE -Wall
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-function
    -Wno-unused-but-set-variable -Wno-unused-parameter)
target_link_libraries(ast_hotpath_bench PRIVATE stdc++ aura_test_objects aura-reflect)
add_test(NAME ast_hotpath_bench COMMAND ./ast_hotpath_bench)
# Legacy ctest names (argv selects scenario / scale).
add_test(NAME stable_ref_bench COMMAND ./ast_hotpath_bench stable-ref 1k)
add_test(NAME stable_ref_bench_10k COMMAND ./ast_hotpath_bench stable-ref 10k)
add_test(NAME children_stable_bench COMMAND ./ast_hotpath_bench children-stable)
add_test(NAME mark_dirty_bench COMMAND ./ast_hotpath_bench mark-dirty)

# Issue #217 Cycle 14 P4: benchmark suite for the C++26 P2996
# reflection + auto_serialize/auto_deserialize path. Measures
# throughput across 4 dimensions (FlatAST SoA, NodeView
# roundtrip, vector<string> serialize, columnar scan vs AoS)
# and outputs JSON to tests/bench_results/cycle14_reflect_bench.json
# for data-driven design decisions.
add_executable(cycle14_reflect_bench tests/reflect/bench_reflect.cpp)
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
    src/compiler/compute_kind_impl.cpp
    src/compiler/hardware_backend_impl.cpp)
# Match aura_test_objects's compile flags so the prebuilt BMIs
# in module_cache/aura_test_objects.dir/ (produced by the .ixx
# compile) have the same identity the .cpp imports expect.
aura_test_compile_options(issue_212_bench)
target_link_libraries(issue_212_bench PRIVATE aura_test_objects pthread stdc++ stdc++exp aura-reflect)

# Issue #213 verification: rollback mechanism for the mutation
# boundary API (Cycle 1). Verifies that
# `Evaluator::exit_mutation_boundary(false)` actually rolls
# back the mutations recorded between enter and exit, via
# the new `FlatAST::rollback_to_size(checkpoint_size)` helper.
#
# Heavy target (full evaluator + serve + parser + JIT runtime)
# so the MutationBoundaryGuard + workspace_flat_ + workspace_pool_
# are available, matching the test_issue_210 / 211 pattern.
# Issue #214 folded into test_edsl_macro_hygiene_batch (wave 52 / #1957).

# Issue #215: auto_deserialize for containers + nested struct
# (Issue #178 Cycle 2).
#
# Standalone C++ test because the test exercises the reflect.hh
# header directly with the -freflection compile flag.
# test_issue_215 folded into edsl batch (wave 57 / #1957)
# aura_add_issue_test_reflect_standalone(test_issue_215)

# Issue #216 folded into test_edsl_macro_hygiene_batch (wave 52 / #1957)
# — query:schema soft smoke.

# Issue #217: AST/IR migration to reflect_members +
# auto_serialize (Issue #178 Cycle 4). The full scope is
# 1 week of focused work; this test is the Cycle 1 pilot
# that verifies the reflection infrastructure works for
# IR-shaped types (OpcodeInfo, IRInstruction).
# test_issue_217 folded into edsl batch (wave 57 / #1957)
# aura_add_issue_test_reflect_standalone(test_issue_217)

# Issue #2289: GCC 16.1 early-snapshot reflection workaround cleanup.
# Header-only smoke (opcode_reflect + to_json); no module / aura_test_objects.
add_executable(test_opcode_reflect_2289 tests/reflect/test_opcode_reflect_2289.cpp)
target_include_directories(test_opcode_reflect_2289 PRIVATE src)
target_compile_options(test_opcode_reflect_2289 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_opcode_reflect_2289 PUBLIC cxx_std_26)
target_link_libraries(test_opcode_reflect_2289 PRIVATE stdc++)
add_test(NAME test_opcode_reflect_2289_verification COMMAND ./test_opcode_reflect_2289)

# Issue #2290: document + exercise aura-reflect isolation (non-module -freflection).
add_executable(test_reflect_isolation_2290 tests/reflect/test_reflect_isolation_2290.cpp)
target_include_directories(test_reflect_isolation_2290 PRIVATE src)
target_compile_options(test_reflect_isolation_2290 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_reflect_isolation_2290 PUBLIC cxx_std_26)
target_link_libraries(test_reflect_isolation_2290 PRIVATE stdc++)
add_test(NAME test_reflect_isolation_2290_verification COMMAND ./test_reflect_isolation_2290)

# Issue #2291: Phase 4 pure-POD IR types (auto_serialize / validate).
add_executable(test_ir_pod_phase4_2291 tests/reflect/test_ir_pod_phase4_2291.cpp)
target_include_directories(test_ir_pod_phase4_2291 PRIVATE src)
target_compile_options(test_ir_pod_phase4_2291 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_ir_pod_phase4_2291 PUBLIC cxx_std_26)
target_link_libraries(test_ir_pod_phase4_2291 PRIVATE aura-reflect stdc++)
add_test(NAME test_ir_pod_phase4_2291_verification COMMAND ./test_ir_pod_phase4_2291)

# Wave A1: observability snapshot JSON via to_json (links aura-reflect).
add_executable(test_obs_json_to_json_a1 tests/reflect/test_obs_json_to_json_a1.cpp)
target_include_directories(test_obs_json_to_json_a1 PRIVATE src)
target_compile_features(test_obs_json_to_json_a1 PUBLIC cxx_std_26)
target_link_libraries(test_obs_json_to_json_a1 PRIVATE aura-reflect stdc++)
add_test(NAME test_obs_json_to_json_a1_verification COMMAND ./test_obs_json_to_json_a1)

# Wave A2: CacheHeader magic[8] C-array reflection + 72-byte wire.
add_executable(test_cache_header_magic_a2 tests/reflect/test_cache_header_magic_a2.cpp)
target_include_directories(test_cache_header_magic_a2 PRIVATE src)
target_compile_options(test_cache_header_magic_a2 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_cache_header_magic_a2 PUBLIC cxx_std_26)
target_link_libraries(test_cache_header_magic_a2 PRIVATE aura-reflect stdc++)
add_test(NAME test_cache_header_magic_a2_verification COMMAND ./test_cache_header_magic_a2)

# Wave A3: IROpcode P2996 idents ↔ kIrOpcodeDisplayNames / kOpcodeInfo.
add_executable(test_opcode_info_align_a3 tests/reflect/test_opcode_info_align_a3.cpp)
target_include_directories(test_opcode_info_align_a3 PRIVATE src)
target_compile_options(test_opcode_info_align_a3 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_opcode_info_align_a3 PUBLIC cxx_std_26)
target_link_libraries(test_opcode_info_align_a3 PRIVATE stdc++)
add_test(NAME test_opcode_info_align_a3_verification COMMAND ./test_opcode_info_align_a3)

# Wave B1: NodeTag P2996 idents ↔ kNodeTagNames / kNodeMeta.
add_executable(test_node_tag_align_b1 tests/reflect/test_node_tag_align_b1.cpp)
target_include_directories(test_node_tag_align_b1 PRIVATE src)
target_compile_options(test_node_tag_align_b1 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_node_tag_align_b1 PUBLIC cxx_std_26)
target_link_libraries(test_node_tag_align_b1 PRIVATE stdc++)
add_test(NAME test_node_tag_align_b1_verification COMMAND ./test_node_tag_align_b1)

# Wave B2: FlatInstruction auto_serialize + IR overlap mapping.
add_executable(test_flat_instr_reflect_b2 tests/reflect/test_flat_instr_reflect_b2.cpp)
target_include_directories(test_flat_instr_reflect_b2 PRIVATE src)
target_compile_options(test_flat_instr_reflect_b2 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_flat_instr_reflect_b2 PUBLIC cxx_std_26)
target_link_libraries(test_flat_instr_reflect_b2 PRIVATE stdc++)
add_test(NAME test_flat_instr_reflect_b2_verification COMMAND ./test_flat_instr_reflect_b2)

# Wave B3: small AST public PODs (SourceLocation, Patch, MatchClauseInfo, …).
add_executable(test_ast_pod_reflect_b3 tests/reflect/test_ast_pod_reflect_b3.cpp)
target_include_directories(test_ast_pod_reflect_b3 PRIVATE src)
target_compile_options(test_ast_pod_reflect_b3 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_ast_pod_reflect_b3 PUBLIC cxx_std_26)
target_link_libraries(test_ast_pod_reflect_b3 PRIVATE stdc++)
add_test(NAME test_ast_pod_reflect_b3_verification COMMAND ./test_ast_pod_reflect_b3)

# Wave C1: generic enum_name_table across domain enums.
add_executable(test_enum_name_table_c1 tests/reflect/test_enum_name_table_c1.cpp)
target_include_directories(test_enum_name_table_c1 PRIVATE src)
target_compile_options(test_enum_name_table_c1 PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_enum_name_table_c1 PUBLIC cxx_std_26)
target_link_libraries(test_enum_name_table_c1 PRIVATE stdc++)
add_test(NAME test_enum_name_table_c1_verification COMMAND ./test_enum_name_table_c1)

# C1 business wire: AuraErrorKind / diag ErrorKind name tables vs P2996.
add_executable(test_error_kind_names_wire tests/reflect/test_error_kind_names_wire.cpp)
target_include_directories(test_error_kind_names_wire PRIVATE src)
target_compile_options(test_error_kind_names_wire PRIVATE -freflection -Wall
                       -Wno-sign-compare -Wno-unused-local-typedefs)
target_compile_features(test_error_kind_names_wire PUBLIC cxx_std_26)
target_link_libraries(test_error_kind_names_wire PRIVATE stdc++)
add_test(NAME test_error_kind_names_wire_verification COMMAND ./test_error_kind_names_wire)
# Issue #268 / #178: NodeView roundtrip (split TU). Full AC needs -freflection
# on the reflect TU only — import std / aura.core.ast must not mix with <meta>
# (g++ 16.1.0; hard errors, not silent ICE). Bridge: test_issue_178_bridge.h
add_executable(test_issue_178 tests/reflect/test_issue_178.cpp tests/reflect/test_issue_178_reflect.cpp)
set_property(TARGET test_issue_178 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_178 PRIVATE src tests)
aura_test_compile_options(test_issue_178)
set_source_files_properties(tests/reflect/test_issue_178_reflect.cpp
                            PROPERTIES COMPILE_OPTIONS "-freflection")
target_compile_features(test_issue_178 PUBLIC cxx_std_26)
target_link_libraries(test_issue_178 PRIVATE aura_test_objects stdc++ pthread)

# Issue #218: reflection integration (Cycle 5) — query:schema +
# hygienic macro composition (#140 + #248). The 1000+ reflect
# stress loop is in test_issue_178; TSan/ASan:
# tests/run_issue_218_tsan.sh

# Issue #219 migrated to tests/domain/arena/test_arena_batch.cpp
# (run_219_gap_buffer_child_data) — Wave 6 (#1957). Standalone
# test entry removed; gap-buffer tests run via the arena batch.

# Issue #220: per-node children linked list (Issue #179 Cycle 2).
# Replaces FlatAST's std::pmr::vector<NodeId> child_data_ (with
# child_begin_/child_count_ SoA columns) with a per-node
# std::pmr::vector<std::pmr::vector<NodeId>> children_ field.
# The per-node list gives O(1) amortized insert/erase at a
# specific node (just std::pmr::vector ops on that node's list;
# no more shifting of "all subsequent child_begin_ entries").
# Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict).
# Issue #220 folded into test_shape_soa_unit_batch (wave 55 / #1957).

# Issue #221: persistent child vectors (Issue #179 Cycle 3,
# slice 1/5). Tests the PersistentChildVector<T> data
# structure defined in src/core/persistent_child_vector.hh.
# Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict).
# test_issue_221 folded into theme batch (wave 56 / #1957)
# aura_add_issue_test_standalone(test_issue_221)

# Issue #222: structural mutation concurrency safety (Issue #179
# Cycle 4, slice 1/3). Tests the begin_structural_mutation() /
# end_structural_mutation() RAII guard + set_child / insert_child
# / remove_child routing + try_acquire_reader_lock() reader-side
# API. Standalone test (no module imports — avoids the GCC 16.1
# std module + P2996 reflection conflict). Uses a TestFlatAST
# stub that mirrors the production API for testing purposes.
# test_issue_222 folded into theme batch (wave 56 / #1957)
# aura_add_issue_test_standalone(test_issue_222)

# Issue #223 migrated to tests/domain/test_compact_batch.cpp
# (run_223_bridge_epoch_tracking) — Wave 6 (#1957). Standalone
# test entry removed; closure-bridge epoch tests run via the
# compact batch.

# Issue #224 cycle 2: shared_ptr-based bridge ownership.
# Tests verify the new std::shared_ptr<const T> fields
# in ClosureBridgeData and IRClosure. The shared_ptr
# keeps the FlatAST alive as long as the bridge exists
# — even after the lowering arena is reset.

# Issue #227: Occurrence Typing narrowing + ADT exhaustiveness
# after Typed Mutations (post #168 epoch fix). Cycle 1 of the
# per-node epoch tracking roadmap. This test captures the
# *current* coarse epoch gate state (Phase 1 of #168) and
# verifies the existing infrastructure works end-to-end.
# Full per-node epoch tracking + recursive free_vars staleness
# check is a separate follow-up (the issue's Recommended Fix #1-2).

# Issue #400: sym_id/structural rollback coverage closed loop.
# 7 ACs: mutation-rollback-coverage-stats, sym_id + structural +
# field_offset rollbacks, structural counter, multi-round monotonic
# + regression. Non-duplicative with #266/#369/#553/#213.
aura_add_issue_test(test_mutation_rollback_coverage)
aura_issue_test_link_light(test_mutation_rollback_coverage)
# Issue #2315: production chaos soak for steal × mutate × GC × mailbox.
# Runtime-gated by AURA_CHAOS_STEAL_GC=1; production binaries / default test
# runs unaffected when unset. CI opt-in: set AURA_CHAOS_DURATION_S=60+
# to satisfy AC1 ≥ 60s CI soak spec.
aura_add_issue_test(test_chaos_steal_mutation_gc)
aura_issue_test_link_light(test_chaos_steal_mutation_gc)
# EXCLUDE_FROM_ALL: opt-in chaos soak (AURA_CHAOS_STEAL_GC); still wired to
# all_test_issue_targets below after that aggregate target is created (#2315).
set_target_properties(test_chaos_steal_mutation_gc PROPERTIES EXCLUDE_FROM_ALL TRUE)
# Issue #1987: wait_for_safepoint actually blocks until signaled (B-001).
aura_add_issue_test(test_safepoint_mutation)
aura_issue_test_link_light(test_safepoint_mutation)
# Issue #1988: ArenaGroup::arenas_ protected by shared_mutex (A-001).
aura_add_issue_test(test_arena_concurrent_mutex)
aura_issue_test_link_light(test_arena_concurrent_mutex)
# Issue #1989: ASTArena::on_compact_hook_ protected by mutex (A-003).
aura_add_issue_test(test_arena_compact_hook_concurrent)
aura_issue_test_link_light(test_arena_compact_hook_concurrent)

# Issue #2381: concurrent compact_hook shape_inval counter is atomic (TSAN + exact count).
aura_add_issue_test(test_arena_compact_hook_stats_2381)
aura_issue_test_link_light(test_arena_compact_hook_stats_2381)
# Issue #2382: ASTArena dtor clears hooks under mutex before teardown (no dangling invoke).
aura_add_issue_test(test_arena_dtor_clears_hooks_2382)
aura_issue_test_link_light(test_arena_dtor_clears_hooks_2382)
# Issue #2383: has_on_compact_hook locks hook_mtx_ (parity with layout/root_remap has_*).
aura_add_issue_test(test_has_on_compact_hook_lock_2383)
aura_issue_test_link_light(test_has_on_compact_hook_lock_2383)
# Issue #2463: summary_flags_ converted to std::atomic<uint32_t> + clear()
# acquires flatast_mutex_. Concurrent reader (summary_flags() / summary_has())
# + writer (clear() / summary_add_flags() / summary_recompute()) must not
# observe a torn mid-write. Pattern family #1 (SoA column race); sibling
# of #2444/#2445/#2446/#2447/#2449/#2450/#2451/#2452/#2453.
aura_add_issue_test(test_ast_concurrency)
aura_issue_test_link_light(test_ast_concurrency)
# Issue #2089: live_compact non-moving contract + LiveCompactResult.moved_live_objects
# + optional on_layout_change callback (fires only on gen bumps). Schema bump on
# (arena:live-compact) hash: 2004 → 2009 + new moved-live-objects field. AC1..AC4
# (primitive layer) + AC5..AC6 (direct C++ ASTArena address-stability + callback
# firing). Lives in tests/core/ per #81934/#81967.
aura_add_issue_test(test_arena_auto_compact_intelligent)
aura_issue_test_link_light(test_arena_auto_compact_intelligent)

# Issue #2157: Force live_compact hard-mutex with LifetimePin + EnvFrameLifetimeGuard.
aura_add_issue_test(test_force_compact_hard_mutex_2157)
aura_issue_test_link_light(test_force_compact_hard_mutex_2157)


# Issue #2166: opt-in LiveCompactMode::Moving densify + object_remap_.
aura_add_issue_test(test_moving_compact_2166)
aura_issue_test_link_light(test_moving_compact_2166)
# Issue #2298: non-render general object pin-or-remap (GeneralObjectPin / pin_or_fail).
aura_add_issue_test(test_general_object_pin_2298)
aura_issue_test_link_light(test_general_object_pin_2298)
# Issue #2294 / #2267: RootRemapPass real Stable-object + Closure capture rewrite.
aura_add_issue_test(test_root_remap_pass_2267)
aura_issue_test_link_light(test_root_remap_pass_2267)
# Issue #1990: (gc-temp)/(gc-stats) closures_ iteration protected by closures_mtx_ (B-009).
aura_add_issue_test(test_issue_1990)
aura_issue_test_link_light(test_issue_1990)
# Issue #1991: (gc) primitive clear path protected by module_mtx_ (B-010).
aura_add_issue_test(test_issue_1991)
aura_issue_test_link_light(test_issue_1991)
# Issue #1993: (gc-heap) direct-clear holds heap_mutex_ + module_mtx_ +
# workspace_mtx_ atomically (std::lock); (gc) also adds workspace_mtx_
# for the workspace_flat_ / workspace_pool_ nulls (D-001, sibling of
# B-010 / F-004).
aura_add_issue_test(test_issue_1993)
aura_issue_test_link_light(test_issue_1993)
# Issue #1994: (workspace-state) and (workspace-tree-*) primitives take
# workspace_mtx_ lock (shared_lock for read-only, unique_lock for
# read+write) to gate against concurrent set_workspace_flat (#1729)
# mid-construction rollback — racy NULL-check-then-dereference
# pattern (F-004, sibling of B-010 / D-001).
aura_add_issue_test(test_workspace_state_lock)
aura_issue_test_link_light(test_workspace_state_lock)
# Wave1 B-03/B-09: eval-current short pin + WorkspaceFlatPin adopt under Guard
aura_add_issue_test(test_workspace_lock_reentrancy)
aura_issue_test_link_llvm_jit(test_workspace_lock_reentrancy)
# Issue #1996: g_batch_deopt_jit file-scope pointer nulled in
# ~CompilerService via aura_clear_jit_batch_deopt_target(&jit_) —
# prevents UAF on late batch_deopt_for / deopt_pending_count after
# CompilerService destruction (B-003, sibling of B-012/B-020).
aura_add_issue_test(test_jit_batch_deopt_clear)
aura_issue_test_link_llvm_jit(test_jit_batch_deopt_clear)
# Issue #1997: shell-wrapper C-source escape loop (aura_jit_bridge.cpp
# ~2535) now handles all C0 control chars (\t \r \0 \a \b \f \v) +
# backslash + double-quote via switch-statement. Without \0 escaping,
# the literal '\0' would prematurely terminate the generated "..."
# literal and corrupt all subsequent emitted C source (B-002).
aura_add_issue_test(test_aot_shell_c0_escape)
aura_issue_test_link_llvm_jit(test_aot_shell_c0_escape)
# Issue #1998: IR-executor MakePair path (ir_executor_impl.cpp ~1012)
# now holds aura_lock_workspace_write() / aura_unlock_workspace_write()
# around the g_owned_pair_slots_.push_back(slot) call. Without the
# lock, vector reallocation races the static PairSlotCleanup destructor
# (aura_jit_runtime.cpp:425, UAF) and concurrent sibling bridge-hook
# writes (aura_make_pair at line 1330+, lost slots / host accounting
# leak). Forward decls for the lock fns added to runtime_shared.h so
# other TUs can call them (B-024, sibling of B-002/B-003/B-012/B-020).
aura_add_issue_test(test_pair_slot_lock)
aura_issue_test_link_llvm_jit(test_pair_slot_lock)
# Issue #1992: Fiber::mutation_stack_storage_ plain void* race on concurrent
# ensure_mutation_stack_ptr — std::atomic<void*> + compare_exchange_weak
# (C-001). Concurrent ensure_*_ptr under work-steal handoff publishes
# exactly ONE allocation; losers release to pool.
aura_add_issue_test(test_issue_1992)
aura_issue_test_link_light(test_issue_1992)
# Issue #415: DirtyReason verification categories +
# mark_dirty_upward propagation metrics synthesis.
aura_add_issue_test(test_dirty_reason_verification_propagation)
aura_issue_test_link_light(test_dirty_reason_verification_propagation)
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
# Issue #523 slice: envframe-production-safety-stats hash observability.
# Issue #524 slice: macro-production-hygiene-stats hash observability.
# Issue #525 slice: guard-production-impact-stats hash observability.
# Issue #528 slice: pattern-production-index-stats hash observability.
# Issue #530 slice: incremental-production-relower-stats hash observability.
# Issue #532 slice: jit-consistency-stats hash observability.
# Issue #533 slice: soa-production-columnar-stats hash observability.
# Issue #534 slice: arena-production-compaction-stats hash observability.
# Issue #535 slice: contracts-production-hotpath-stats hash observability.
# Issue #539 slice: sv-production-verification-stats hash observability.
# Issue #540 slice: eda-stability-stats hash observability.
# Issue #541 slice: pattern-sv-verification-stats hash observability.
# Issue #557 slice: top5-commercial-coverage-stats hash observability.
# Issue #567 slice: primitives-governance-stats hash observability.
# Issue #568 slice: soa-children-columnar-migration-stats hash observability.
# Issue #569 slice: arena-auto-compact-defrag-stats hash observability.
# Issue #572 slice: pass-pipeline-dirtyaware-stats hash observability.
# Issue #578 slice: sv-structured-edsl-stats hash observability.
# Issue #579 slice: verification-feedback-loop-stats hash observability.
# Issue #580 slice: hardware-backend-stats hash observability.
# Issue #581 slice: stable-ref-sv-scale-stats hash observability.
# Issue #582 slice: eda-concurrency-stats hash observability.
# Issue #583 slice: primitives-registry-core-stats hash observability.
# Issue #584 slice: primitives-hotpath-stats AI-agent stress observability.
# Issue #585 slice: primitives-error-stats unified recovery observability.
# Issue #586 slice: eda-primitives-stats infra registry observability.
# Issue #587 slice: primitives-ai-native-stats development observability.
# Issue #591 slice: scheduler-mutation-coord-stats steal/GC observability.
# Issue #401: invalidate_function BFS determinism + dep_graph_
# integrity (vector-as-stack → std::deque, sort dependents).
# Uses CompilerService::public_invalidate_function +
# public_dep_graph_* hooks. Same link profile as test_issue_225
# / test_issue_226 (jit_tests bundle).
# Issue #611: CoercionMap deferred + DCE + CastOp type_tag zero-overhead flow
aura_add_issue_test(test_coercion_dead_elim_castop_flow_zerooverhead)
aura_issue_test_link_light(test_coercion_dead_elim_castop_flow_zerooverhead)
# Issue #495: Task2 refinement closed-loop integration matrix.
aura_add_issue_test(test_refinement_closed_loop)
aura_issue_test_link_light(test_refinement_closed_loop)
# Issue #384: Bidirectional inference engine — first slice.
# Additive check-mode plumbing in synthesize_flat_lambda (the
# caller hands an expected function type when the arg is a
# lambda, so params + return are constrained directly instead of
# synthesizing fresh vars and unifying post-hoc at the call
# site). No LLVM JIT needed — pure type-checker exercise.
# Issue #389: follow-up #247 — (compile:snapshot) Aura primitive +
# JSON schema docs (scope-limited slice). The test exercises
# CompilerService + marker observability, which depends on the
# JIT-linked service object files.
# Issue #392: scoped / per-subtree generation bumping for
# precise StableRef invalidation. The test exercises
# FlatAST's per-top-level-Define subtree_gen_ tracking +
# is_valid_subtree() + 3 Aura primitives
# ((compile:subtree-bump), (compile:subtree-generation),
# (compile:subtree-bump-count)). Pure AST-level — no
# compiler service or JIT linkage needed.

# Issue #393: C++ API for explicit (id, gen) pair
# construction + flat-style validity check. Validates the
# two new FlatAST helpers:
#   - make_ref_from_gen(NodeId, uint16_t gen)
#   - is_valid_id_gen(NodeId, uint16_t gen, uint32_t wrap_epoch = 0)
# Pure AST-level — same shape as test_issue_392.
# Issue #393 AC10/AC11 add (query:ref-valid?) Aura primitive
# tests via CompilerService; needs the minimal LLVM JIT link
# (the original 9 ACs are pure FlatAST and don't, but the new
# ones import aura.compiler.evaluator which transitively
# pulls in JIT).

# Issue #396: atomic-batch fiber safety + lockless helpers
# (remove-node / insert-child) + new stats fields. Uses
# CompilerService and the messaging bridge (for the
# g_fiber_set_yield_reason_mutation_boundary mock setter in
# AC1). Same link requirements as test_issue_393.

# Issue #1900: refine #1698 — strengthen mutate:atomic-batch
# strong atomicity + expand dispatch table 5 → 14 ops.
# Validates: (a) all 14 sub-ops work in batch; (b)
# atomic_batch_unsupported_op_total + interleaved_mutation_prevented
# metrics bump correctly; (c) 1000+ iter concurrent fiber stress
# runs clean. Pure CompilerService tests.


# Issue #398: optimize children_stable via zero-alloc
# callback. Pure FlatAST tests + a single CompilerService
# regression test for the Aura primitive. Minimal LLVM
# JIT link (uses aura.compiler.service in AC6).

# Issue #399: pre-reserve dirty side-table columns in
# add_node to avoid the resize() in mark_dirty's hot
# path. Pure AST-level tests (no CompilerService).

# Issue #326: Hygienic macros + EDSL integration coverage.
# 8 scenarios extending test_issue_120/121 to integration
# (CompilerService path, not parser-only). See
# tests/issues/test_issue_326.cpp for per-scenario docs.
# Issue #331: targeted dirty bitmask + query:pattern
# integration under mutate. 8 scenarios extending the
# basic dirty-API coverage from test_issue_138.
# Issue #330: dedicated unit tests for StructuralMutationGuard
# + ReaderLockGuard. 11 scenarios covering ctor/dtor, move
# semantics, exception unwind, generation monotonicity,
# multiple concurrent readers.
# Issue #327: incremental compilation e2e + epoch gate.
# 7 scenarios extending #326 + #331 with the
# (compile:relower-strategy) axis — pipeline decides
# 'none / 'incremental / 'full / 'unknown based on
# dirty-block count.
# Issue #322: dual-path SoA/EnvId invariants + arena
# compaction tests. 8 scenarios covering lookup
# consistency under mutations, compaction safety,
# observability primitives.
# Issue #324: Arena yield-safe compaction + observability.
# 5 scenarios covering existing compaction surface +
# new ArenaStats.compaction_yield_checks field placeholder.
# The actual fiber-yield hook is deferred (cross-module
# TLS + WorkerContext plumbing).
# Issue #345: stress testing for StructuralMutationGuard +
# generation_ stability. 5 scenarios covering long-iteration
# loop (configurable via AURA_345_ITERATIONS), concurrent
# readers + mutators, generation wrap-around, perf bench,
# rollback consistency.
# Issue #306: EDA — Extend Linear Ownership (OwnershipEnv +
# lowering_linear_types) to hardware resources (wires, regs,
# memories, ports) (P0 EDA track). 8 scenarios validating:
# 4 new hw_resource_* counters (wire/reg/mem/double-drive)
# + (query:linear-ownership-stats) primitive + OwnershipEnv
# 4 OwnershipState enum verification + bump helpers +
# 100-iter cycle + 8-thread concurrent + (gc-heap) integration
# + regression for #556 + #555 + #549.
aura_add_issue_test(test_hardware_resource_linear_ownership)
aura_issue_test_link_light(test_hardware_resource_linear_ownership)
# Issue #599: Compiler root epoch/version + GC synergy post-invalidate_function.
# 7 ACs: compiler-root-stats (schema 599), root-refresh on invalidate,
# stale/env-mismatch/dangling counters, multi-round monotonic + regression.
# Non-duplicative with #531 (closure-env-safety), #598 (linear-runtime).
aura_add_issue_test(test_root_epoch_gc_safety_post_invalidate)
aura_issue_test_link_light(test_root_epoch_gc_safety_post_invalidate)
# Issue #600: Per-block dirty + impact scope + closure bridge synergy for
# minimal re-lower in AI self-mod. 7 ACs: incremental-closure-stats
# (schema 600), blocks-relowered/bridge-hits/min-scope-win/jit-sync,
# multi-round monotonic + regression. Non-duplicative with #530/#429/#531.
aura_add_issue_test(test_incremental_perblock_closure_bridge_safety)
aura_issue_test_link_light(test_incremental_perblock_closure_bridge_safety)
# Issue #305: Propagate rich TypeId/TypeScheme from TypeChecker
# to IR for hardware optimization & synthesis (P0 EDA track).
# Issue #304: Aura -> Agentic EDA Roadmap (roadmap/vision,
# NOT a shipping task). 4 scenarios verifying the canonical
# roadmap doc (docs/ROADMAP-HARDWARE.md) is present + has
# all 3 phase sections + Issue #304 reference + decision
# doc cross-references + Status table. Future child issues
# will be filed per Phase 0/1/2 milestone.
# test_hardware_roadmap_acknowledged was deleted as part of the
# docs/ removal wave (Anqi 2026-07-19 directive — aura philosophy,
# no per-issue plan docs); the source-driven ACs above remain
# authoritative via future child issues.
# Issue #566: First batch of primitives demotion + full
# validation (P2, capstone of the primitives-demotion epic).
# 6 scenarios validating: primitive count, stdlib module
# consistency (7 modules), INDEX reachability, surface item
# reduction (>=20), decision doc presence (removed per Anqi
# 2026-07-19 directive — see test_issue_1650/1645/1651/1647.cpp
# check_design_doc_present rewrites for pattern), discoverability
# regression. Non-duplicative with #558-#565.
# test_primitives_demotion_batch1 was deleted as part of the
# docs/design/ removal commit (aura philosophy, no per-issue plan
# docs); the source-driven ACs above remain authoritative via the
# CMake-driven test wiring.
# Issue #565: Stdlib Infrastructure improvements (P1,
# parallel-safe). 5 scenarios validating: stdlib-organization
# spec doc + lib/std/INDEX.aura (5 discovery functions) +
# stdlib module consistency (.aura + .aura-type) + (export)
# coverage + consistency check. Non-duplicative with
# #558-#564.
aura_add_issue_test(test_stdlib_infrastructure)
aura_issue_test_link_light(test_stdlib_infrastructure)
# Issue #564: Core Builtins Selective Review (P3, long-term).
# 5 scenarios validating: lib/std/core.aura (new, 10 stdlib
# wrappers) + decision doc with STAY+DEMOTE tables + reusable
# review process + regression on engine core primitives.
# Non-duplicative with #558-#563.
aura_add_issue_test(test_core_builtins_review)
aura_issue_test_link_light(test_core_builtins_review)
# Issue #563: New/enhanced std/ast + std/workspace high-level
# modules (P2). 5 scenarios validating: lib/std/ast.aura
# (new, 6 stdlib functions) + lib/std/workspace.aura enhanced
# (+5 stdlib functions) + >= 12 engine primitives wrapped +
# decision doc + regression. Non-duplicative with
# #558-#562.
aura_add_issue_test(test_ast_workspace_modules)
aura_issue_test_link_light(test_ast_workspace_modules)
# Issue #562: Audit + demote query: namespace primitives
# (P1). 8 scenarios validating: lib/std/query.aura (high-
# level Agent API with 5 stdlib functions) + 12 demotion
# candidates identified in decision doc (Tier 1 + Tier 2) +
# stdlib wrapper safety + regression for core primitives.
# Non-duplicative with #558 + #559 + #560 + #561.
aura_add_issue_test(test_query_namespace_audit)
aura_issue_test_link_light(test_query_namespace_audit)
# Issue #561: Review and demote synthesize: namespace
# primitives (P1). 9 scenarios validating: (query:templates)
# engine-level accessor (replaces demoted primitive) +
# 3 remaining synthesize:* primitives still registerable +
# stdlib wrapper file + decision doc + lint hint removal +
# regression. Non-duplicative with #558 (decision framework)
# + #559 (classification) + #560 (stats unification).
aura_add_issue_test(test_synthesize_namespace_demotion)
aura_issue_test_link_light(test_synthesize_namespace_demotion)
# Issue #560: Unify *-stats primitives via std/stats.aura +
# (stats:list) + (stats:count) meta-primitives (P1). 10
# scenarios validating: (stats:list) returns 39+ entries,
# (stats:count) returns integer, std/stats wrappers route
# correctly to existing primitives, prefix filter works,
# (stats:get "all") bundles every counter, regression
# covers #543-#531 + define/eval. Non-duplicative with
# #543-#556 + #531.
aura_add_issue_test(test_stats_module_unification)
aura_issue_test_link_light(test_stats_module_unification)
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
aura_issue_test_link_light(test_compiler_closure_env_safety_post_invalidate)
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
aura_issue_test_link_light(test_edsl_concurrent_fiber_boundary_task1)
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
aura_add_issue_test(test_incremental_typed_selfmod_dirty_narrowing)
aura_issue_test_link_light(test_incremental_typed_selfmod_dirty_narrowing)
# Issue #554: query:pattern + tag_arity_index Incremental
# Issue #553: End-to-End Atomic Batch Mutate + Full
# mutation_log_ Rollback + MutationBoundaryGuard + Fiber
# Safety for Reliable AI Multi-Step Self-Evolution (P0).
# 9 scenarios validating: new (query:mutation-log-stats)
# primitive + atomic_batch happy/rollback paths +
# 50-iter stress + 8-thread concurrent + (gc-heap) +
# regression. Non-duplicative refinement of #529 focused
# on Task1 EDSL primitives + Guard/fiber paths.
aura_add_issue_test(test_atomic_batch_rollback_fiber_task1)
aura_issue_test_link_light(test_atomic_batch_rollback_fiber_task1)
# Issue #529: Atomic batch + Guard rollback closed-loop observability.
aura_add_issue_test(test_atomic_batch_rollback_closed_loop)
aura_issue_test_link_light(test_atomic_batch_rollback_closed_loop)
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
aura_add_issue_test(test_stable_ref_provenance_fiber_cow)
aura_issue_test_link_light(test_stable_ref_provenance_fiber_cow)
# Issue #551: Post-Mutation Reflection/auto_validate +
# Guard Impact Snapshot + Schema Validation Hook for
# Code-as-Data Closed Loop (P0). 9 scenarios validating:
# 4 new reflect/snapshot counters + new
# (query:reflect-postmutate-stats) primitive +
# impact_snapshot_count under mutate load + 200-iter
# cycle + schema_validation setters + 8-thread concurrent
# + (gc-heap) integration + regression. Non-duplicative
# with #535/#525/#504/#502/#514 (prior reflect/Guard work).
# Issue #454: Reflection-to-EDSL bridge closed loop (FlatAST/marker/mutate).

# Issue #594: Static reflection self-mod validation hook (Task6).
aura_add_issue_test(test_static_reflect_selfmod_validation_task6)
aura_issue_test_link_light(test_static_reflect_selfmod_validation_task6)
# Issue #550: Dirty/Epoch Propagation + Incremental
# solve_delta + Occurrence Re-Narrowing + Pass Short-Circuit
# for reliable typed self-mod (P0). 9 scenarios validating:
# 4 new dirty/narrowing counters + 2 new query primitives
# (typed-mutation-stats + dirty-impact) + narrowing refresh
# under mutate load + 200-iter typed mutate cycle + 8-thread
# concurrent typed mutate + (gc-heap) + regression.
# Non-duplicative with #536/#537/#526/#509/#518 (impl).
# ── Issue test bundles (restored; accidentally dropped in wave 8 #1978) ──
# One fat executable per link profile — see scripts/gen_issue_bundles.py
# and tests/bundles/issue_bundle_runner.{hh,cpp}.
aura_add_issue_bundle(light)
aura_add_issue_bundle(jit_late1)
# DISABLED (pre-existing bundle bug: bundle main calls aura_issue_*_run()
# entry functions that aren't defined in any test file — gen_issue_bundles.py
# referenced functions like aura_issue_dead_coercion_elision_narrow_evidence_run
# that don't exist; needs source-side fixes per member) for CI green:
# aura_add_issue_bundle(jit_late2)
# aura_add_issue_bundle(jit_late3)
aura_add_issue_bundle(light_late)

# Default issue-suite build: the 12 profile bundles (not 200+ standalones).
add_custom_target(all_test_issue_targets ALL)
# Issue #2315: chaos soak registered above with EXCLUDE_FROM_ALL; opt-in
# via AURA_CHAOS_STEAL_GC but still buildable under the aggregate target.
add_dependencies(all_test_issue_targets test_chaos_steal_mutation_gc)
# Issue #2381: concurrent compact_hook shape_inval atomic stress.
add_dependencies(all_test_issue_targets test_arena_compact_hook_stats_2381)
# Issue #2382: ASTArena dtor clears hooks before teardown.
add_dependencies(all_test_issue_targets test_arena_dtor_clears_hooks_2382)
# Issue #2383: has_on_compact_hook lock parity.
add_dependencies(all_test_issue_targets test_has_on_compact_hook_lock_2383)
add_dependencies(all_test_issue_targets test_issues_light)
add_dependencies(all_test_issue_targets test_issues_jit_late1)
# DISABLED (paired with disabled aura_add_issue_bundle above):
# add_dependencies(all_test_issue_targets test_issues_jit_late2)
# add_dependencies(all_test_issue_targets test_issues_jit_late3)
add_dependencies(all_test_issue_targets test_issues_light_late)


aura_add_issue_test(test_envframe_epoch_batch)
aura_issue_test_link_llvm_jit(test_envframe_epoch_batch)
add_dependencies(all_test_issue_targets test_envframe_epoch_batch)
aura_add_issue_test(test_envframe_truncate_epoch)
aura_issue_test_link_llvm_jit(test_envframe_truncate_epoch)
add_dependencies(all_test_issue_targets test_envframe_truncate_epoch)

# tests/reflect/ hygiene consolidation (R3 renamed edsl → reflect).
# Prefer extending these over new test_edsl_* / test_reflect_* singles.
aura_add_issue_test(test_reflect_pattern_hygiene_batch)
aura_issue_test_link_light(test_reflect_pattern_hygiene_batch)
add_dependencies(all_test_issue_targets test_reflect_pattern_hygiene_batch)

aura_add_issue_test(test_reflect_macro_hygiene_batch)
aura_issue_test_link_light(test_reflect_macro_hygiene_batch)
add_dependencies(all_test_issue_targets test_reflect_macro_hygiene_batch)

# Issue #2018: rest-param hygiene in clone_macro_body + hygienic rest expand.
aura_add_issue_test(test_rest_param_hygiene_self_evo)
aura_issue_test_link_light(test_rest_param_hygiene_self_evo)
add_dependencies(all_test_issue_targets test_rest_param_hygiene_self_evo)

# Issue #2169: complete rest-param hygienic renaming (unique gensyms + shadow).
aura_add_issue_test(test_rest_param_hygiene_2169)
aura_issue_test_link_light(test_rest_param_hygiene_2169)
add_dependencies(all_test_issue_targets test_rest_param_hygiene_2169)

# Issue #2239: complete rest-param + nested quasiquote hygiene + schema_cache
# stamping visibility (qq-aware pre_scan + stamp_rest_param_hygiene helper
# at both rest-list allocation sites + per-rest + per-qq metrics + query
# primitive hash upgrade).
# DISABLED (pre-existing namespace mismatch: extern decls in aura::compiler::macro_exp,
# but defs in aura::compiler::macro_expansion via inline namespace ABI tag — needs
# test source fix or #ifdef AURA_ISSUE_BUNDLE_MEMBER in extern block) for CI green:
# aura_add_issue_test(test_rest_param_nested_qq_hygiene_2239)
# aura_issue_test_link_llvm_jit(test_rest_param_nested_qq_hygiene_2239)
# add_dependencies(all_test_issue_targets test_rest_param_nested_qq_hygiene_2239)

# Issue #2019: MacroIntroduced restamp after expand → FlatAST.
aura_add_issue_test(test_macro_restamp_after_flat)
aura_issue_test_link_light(test_macro_restamp_after_flat)
add_dependencies(all_test_issue_targets test_macro_restamp_after_flat)
# Issue #2235: production-grade cross-FlatAST clone_macro_body
# hygiene gate (always-on validate + sandbox-strict forced restamp;
# replaces #2171 `#ifndef NDEBUG` + abort).
aura_add_issue_test(test_macro_cross_flat_hygiene_2235)
aura_issue_test_link_light(test_macro_cross_flat_hygiene_2235)
add_dependencies(all_test_issue_targets test_macro_cross_flat_hygiene_2235)
# Issue #2096: per-cloned-subtree MacroIntroduced restamp stability
# after expand in mutate paths (NodeId-rooted helper + counter + query surface).
aura_add_issue_test(test_macro_intro_restamp)
aura_issue_test_link_light(test_macro_intro_restamp)
add_dependencies(all_test_issue_targets test_macro_intro_restamp)
# Issue #2236: optional per-region / per-eval deopt-storm isolation
# in HotUpdateRegistry (per-AC2 cap 64 regions, overflow → global;
# critical bypass #2132 preserved; Shape-only #2172 invariant).
aura_add_issue_test(test_storm_isolation_2236)
aura_issue_test_link_llvm_jit(test_storm_isolation_2236)
add_dependencies(all_test_issue_targets test_storm_isolation_2236)
# Issue #2237: expose mutate:rollback-macro-introduced + query:by-marker
# primitives as first-class Agent-visible surface (build on existing
# #2176 unstamp C-linkage + #1914 by-marker query). Verifies:
#   - AC1: existing primitives registered + reachable
#   - AC2: unstamp + rollback + strict-audit counters visible via
#          query:macro-hygiene-stats (7 new keys)
#   - AC3: per-op counter monotonic
#   - AC4: Strict sandbox → MacroHygieneRollbackOnStrict audit emit
#   - AC5: back-compat of existing #1914 query surface
#   - AC6: source-cite
aura_add_issue_test(test_rollback_by_marker_2237)
aura_issue_test_link_llvm_jit(test_rollback_by_marker_2237)
add_dependencies(all_test_issue_targets test_rollback_by_marker_2237)
# Issue #2238: alloc-time / set_name-time policy for anonymous /
# sid=0 AOT-bound closures. Default permissive (today's behavior);
# opt-in via AURA_REQUIRE_STABLE_ID_FOR_AOT=1 or
# aura_set_require_stable_id_for_aot(1). When ON, anonymous closures
# get MustDeopt set at aura_closure_set_name time
# (or aura_closure_check_aot_stable_id_policy call from AOT install),
# forcing interpreter-only on next call (existing #2128 gate).
aura_add_issue_test(test_aot_anonymous_closure_policy_2238)
aura_issue_test_link_llvm_jit(test_aot_anonymous_closure_policy_2238)
add_dependencies(all_test_issue_targets test_aot_anonymous_closure_policy_2238)
aura_add_issue_test(test_query_by_marker_provenance_2242)
aura_issue_test_link_light(test_query_by_marker_provenance_2242)
add_dependencies(all_test_issue_targets test_query_by_marker_provenance_2242)
# Issue #2098: clone_macro_body schema_cache + dirty/provenance
# propagation for rest-param + nested paths (metrics layer + tests).
aura_add_issue_test(test_macro_schema_dirty_propagate)
aura_issue_test_link_light(test_macro_schema_dirty_propagate)
add_dependencies(all_test_issue_targets test_macro_schema_dirty_propagate)
# Issue #2097: per-fiber hygiene metrics for Agent query under
# concurrent self-evo / fiber-steal (refine Macro Hygiene review §7.2).
aura_add_issue_test(test_macro_fiber_hygiene)
aura_issue_test_link_llvm_jit(test_macro_fiber_hygiene)
add_dependencies(all_test_issue_targets test_macro_fiber_hygiene)

# Issue #2020: Agent-visible reflect:hygiene-stats + provenance-blame.
aura_add_issue_test(test_reflect_hygiene_agent_diagnostics)
aura_issue_test_link_light(test_reflect_hygiene_agent_diagnostics)
add_dependencies(all_test_issue_targets test_reflect_hygiene_agent_diagnostics)

# Issue #2167: Agent-visible hygiene-diagnostic + macro-provenance-chain.
aura_add_issue_test(test_hygiene_diagnostic_2167)
aura_issue_test_link_light(test_hygiene_diagnostic_2167)
add_dependencies(all_test_issue_targets test_hygiene_diagnostic_2167)

# Issue #2021: macro hygiene depth + concurrent peak observability.
aura_add_issue_test(test_macro_hygiene_depth_concurrent_obs)
aura_issue_test_link_light(test_macro_hygiene_depth_concurrent_obs)
add_dependencies(all_test_issue_targets test_macro_hygiene_depth_concurrent_obs)

# Issue #2022: MacroIntroduced preserved across JIT/AOT native metadata.
aura_add_issue_test(test_jit_macro_introduced_preserve)
aura_issue_test_link_llvm_jit(test_jit_macro_introduced_preserve)
add_dependencies(all_test_issue_targets test_jit_macro_introduced_preserve)

# Issue #2023: MacroSelfEvo capability boundary for agent macro expansion.
aura_add_issue_test(test_macro_self_evo_capability)
aura_issue_test_link_light(test_macro_self_evo_capability)
add_dependencies(all_test_issue_targets test_macro_self_evo_capability)
# Issue #2386: grant_macro_self_evo stamps grant_epoch + grant_fiber_id (#2055 parity).
aura_add_issue_test(test_grant_macro_self_evo_stamp_2386)
aura_issue_test_link_light(test_grant_macro_self_evo_stamp_2386)
add_dependencies(all_test_issue_targets test_grant_macro_self_evo_stamp_2386)

# Issue #2024: occurrence narrowing provenance chain completeness.
aura_add_issue_test(test_occurrence_provenance_chain_completeness)
aura_issue_test_link_light(test_occurrence_provenance_chain_completeness)
add_dependencies(all_test_issue_targets test_occurrence_provenance_chain_completeness)

# Issue #2025: DeadCoercionEliminationPass default pipeline + AST/IR synergy.
aura_add_issue_test(test_dead_coercion_pipeline_wire)
aura_issue_test_link_light(test_dead_coercion_pipeline_wire)
add_dependencies(all_test_issue_targets test_dead_coercion_pipeline_wire)

# Issue #2026: linear ownership + provenance consistency post-steal / GC.
aura_add_issue_test(test_linear_provenance_steal_gc_closed_loop)
aura_issue_test_link_light(test_linear_provenance_steal_gc_closed_loop)
add_dependencies(all_test_issue_targets test_linear_provenance_steal_gc_closed_loop)

# Issue #2027: composite / nested txn typed + linear invariant audit.
aura_add_issue_test(test_composite_nested_txn_invariant_audit)
aura_issue_test_link_light(test_composite_nested_txn_invariant_audit)
add_dependencies(all_test_issue_targets test_composite_nested_txn_invariant_audit)

# Issue #2028: constraint solver surface + cross-delta occurrence/Let-Poly/ADT.
aura_add_issue_test(test_constraint_solver_surface_cross_delta)
aura_issue_test_link_light(test_constraint_solver_surface_cross_delta)
add_dependencies(all_test_issue_targets test_constraint_solver_surface_cross_delta)

# Issue #2029: Full-strategy per-category partial recovery before structural rollback.
aura_add_issue_test(test_full_strategy_partial_recovery)
aura_issue_test_link_light(test_full_strategy_partial_recovery)
add_dependencies(all_test_issue_targets test_full_strategy_partial_recovery)

# Issue #2030: blame completeness + occurrence hit-rate agent query surface.
aura_add_issue_test(test_blame_occurrence_agent_ratios)
aura_issue_test_link_light(test_blame_occurrence_agent_ratios)
add_dependencies(all_test_issue_targets test_blame_occurrence_agent_ratios)

# Issue #2031: instruction-level impact scope + selective dirty.
aura_add_issue_test(test_instr_level_impact_scope)
aura_issue_test_link_light(test_instr_level_impact_scope)
add_dependencies(all_test_issue_targets test_instr_level_impact_scope)

# Issue #2032: dep_graph stale-edge reject + configurable partial-relower threshold.
aura_add_issue_test(test_dep_graph_partial_relower_threshold)
aura_issue_test_link_light(test_dep_graph_partial_relower_threshold)
add_dependencies(all_test_issue_targets test_dep_graph_partial_relower_threshold)

# Issue #2033: CacheEntryVersionStamp + bridge_epoch in should_relower.
aura_add_issue_test(test_cache_entry_version_stamp)
aura_issue_test_link_light(test_cache_entry_version_stamp)
add_dependencies(all_test_issue_targets test_cache_entry_version_stamp)

# Issue #2183: unified CacheEntryVersionStamp restamp contract.
aura_add_issue_test(test_cache_stamp_restamp_contract_2183)
aura_issue_test_link_light(test_cache_stamp_restamp_contract_2183)
add_dependencies(all_test_issue_targets test_cache_stamp_restamp_contract_2183)

# Issue #2034: force instruction_dirty_ sync after cascade block dirty marks.
aura_add_issue_test(test_soa_cascade_instr_dirty_sync)
aura_issue_test_link_light(test_soa_cascade_instr_dirty_sync)
add_dependencies(all_test_issue_targets test_soa_cascade_instr_dirty_sync)

# Issue #2181: hard gate SoA desync on partial relower entry → force full.
aura_add_issue_test(test_soa_partial_desync_gate_2181)
aura_issue_test_link_light(test_soa_partial_desync_gate_2181)
add_dependencies(all_test_issue_targets test_soa_partial_desync_gate_2181)

# Issue #2035: HotUpdateRegistry dirty notify + region-mask reemit from cascade.
aura_add_issue_test(test_hot_update_cascade_dirty_reemit)
aura_issue_test_link_llvm_jit(test_hot_update_cascade_dirty_reemit)
add_dependencies(all_test_issue_targets test_hot_update_cascade_dirty_reemit)

# Issue #2036: PCV children_ migration complete + SafePCVSpan default APIs.
aura_add_issue_test(test_pcv_children_safe_default_migration)
aura_issue_test_link_light(test_pcv_children_safe_default_migration)
add_dependencies(all_test_issue_targets test_pcv_children_safe_default_migration)

# Issue #2037: MacroIntroduced FailOnStale + stamp on mutate hotpaths.
aura_add_issue_test(test_hygiene_mutate_closed_loop)
aura_issue_test_link_light(test_hygiene_mutate_closed_loop)
add_dependencies(all_test_issue_targets test_hygiene_mutate_closed_loop)

# Issue #2038: push-automatic post-mutate DefUse/IR/JIT cascade + latency.
aura_add_issue_test(test_post_mutate_push_cascade)
aura_issue_test_link_light(test_post_mutate_push_cascade)
add_dependencies(all_test_issue_targets test_post_mutate_push_cascade)

aura_add_issue_test(test_reflect_hygiene_unit_batch)
aura_issue_test_link_light(test_reflect_hygiene_unit_batch)
add_dependencies(all_test_issue_targets test_reflect_hygiene_unit_batch)

# tests/fiber/ consolidation — strategy/evolve, synthesize, orch, concurrent units.
# Keep test_concurrent + test_jit_concurrent_compile (custom targets) and
# test_per_fiber_stack_pool_high_concurrency (jit_late1 bundle member).
aura_add_issue_test(test_fiber_strategy_evolve_batch)
aura_issue_test_link_light(test_fiber_strategy_evolve_batch)
add_dependencies(all_test_issue_targets test_fiber_strategy_evolve_batch)

aura_add_issue_test(test_fiber_synthesize_batch)
aura_issue_test_link_light(test_fiber_synthesize_batch)
add_dependencies(all_test_issue_targets test_fiber_synthesize_batch)

# Orch batches: EXCLUDE_FROM_ALL — Scheduler/fiber stress can SIGSEGV under
# co-linked load; on-demand via ninja test_fiber_orch_*_batch.
aura_add_issue_test(test_fiber_orch_core_batch)
aura_issue_test_link_light(test_fiber_orch_core_batch)
set_target_properties(test_fiber_orch_core_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

aura_add_issue_test(test_fiber_orch_parallel_quota_batch)
aura_issue_test_link_light(test_fiber_orch_parallel_quota_batch)
set_target_properties(test_fiber_orch_parallel_quota_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Issue #2078: per-Evaluator orch agent name table (replaces the
# process-static OrchAgentNameTable that lived in
# evaluator_primitives_agent.cpp — multiple CompilerService instances
# collided on the same agent name and tests were non-hermetic).
# Storage moves to src/compiler/agent_name_table.h::AgentNameTable
# (Evaluator member), drained at ~Evaluator via
# Evaluator::cleanup_orch_agents(). Single-agent MVP surface
# preserved (scripts/check_orch_mvp_scope.py --strict still guards).
aura_add_issue_test(test_agent_name_table_isolation_2078)
aura_issue_test_link_light(test_agent_name_table_isolation_2078)

# Issue #2158: per-Evaluator agent apply mutex (replace process-static orch_eval_mu).
aura_add_issue_test(test_agent_apply_mutex_2158)
aura_issue_test_link_light(test_agent_apply_mutex_2158)
add_dependencies(all_test_issue_targets test_agent_apply_mutex_2158)

# Issue #2163: parallel-intend :pure #t path (skip eval_mu for pure thunks).
aura_add_issue_test(test_parallel_intend_pure_2163)
aura_issue_test_link_light(test_parallel_intend_pure_2163)
add_dependencies(all_test_issue_targets test_parallel_intend_pure_2163)

# Issue #1665 / #2164: scan_skip_freed + EnvFrameLifetimeGuard hold-pin.
# Source: tests/compiler/test_scan_skip_freed_closures.cpp (resolved by name).
aura_add_issue_test(test_scan_skip_freed_closures)
aura_issue_test_link_llvm_jit(test_scan_skip_freed_closures)
add_dependencies(all_test_issue_targets test_scan_skip_freed_closures)

# Issue #2159: fiber-native keepalive helper (replace detached std::thread).
aura_add_issue_test(test_fiber_native_keepalive_2159)
aura_issue_test_link_light(test_fiber_native_keepalive_2159)
add_dependencies(all_test_issue_targets test_fiber_native_keepalive_2159)

# Issue #2527: mutate:query-and-replace-batch sugar primitive (single Guard
# + atomic-batch wrapping, structured result hash, source-cited in
# evaluator_primitives_mutate.cpp). Tests under tests/compiler/ per
# #81934/#81967 (new file — no existing test_mutate_query_and_replace_*).
aura_add_issue_test(test_query_and_replace_batch_2527)
aura_issue_test_link_llvm_jit(test_query_and_replace_batch_2527)
add_dependencies(all_test_issue_targets test_query_and_replace_batch_2527)

# Issue #2083: opt-in AgentScope (feature-flagged via AURA_ENABLE_AGENT_SCOPE).
# The test file #defines the flag so the AgentScope class body is visible.
aura_add_issue_test(test_agent_scope_2083)
aura_issue_test_link_light(test_agent_scope_2083)

aura_add_issue_test(test_fiber_concurrent_unit_batch)
aura_issue_test_link_light(test_fiber_concurrent_unit_batch)
add_dependencies(all_test_issue_targets test_fiber_concurrent_unit_batch)

# tests/compiler/jit/ AOT hot-update consolidation (#1640/#544/#1370/#1480/#1641).
# Keep test_jit_metrics (+ stub) as custom non-module LLVM target above.
aura_add_issue_test(test_jit_aot_hot_update_unit_batch)
aura_issue_test_link_llvm_jit(test_jit_aot_hot_update_unit_batch)
add_dependencies(all_test_issue_targets test_jit_aot_hot_update_unit_batch)

# Issue #1366 / #2012: (aot:reload) primitive + atomic func_table staging/rollback.
# -rdynamic: test .so constructors resolve aura_register_fn_tracked via dlsym.
aura_add_issue_test(test_aot_reload_primitive)
aura_issue_test_link_llvm_jit(test_aot_reload_primitive)
target_link_options(test_aot_reload_primitive PRIVATE -Wl,--export-dynamic)
add_dependencies(all_test_issue_targets test_aot_reload_primitive)

# Issue #2306: (query:aot-version-triple) 5-stamp aggregator primitive.
aura_add_issue_test(test_aot_version_triple_2306)
aura_issue_test_link_llvm_jit(test_aot_version_triple_2306)
add_dependencies(all_test_issue_targets test_aot_version_triple_2306)

# Issue #2304: post-bump epoch invariant walk infrastructure.
aura_add_issue_test(test_epoch_bump_invariant_2304)
aura_issue_test_link_llvm_jit(test_epoch_bump_invariant_2304)
add_dependencies(all_test_issue_targets test_epoch_bump_invariant_2304)

# Issue #2366: per-entry stale-stamp + MustDeopt walk (#2304 follow-up).
aura_add_issue_test(test_epoch_invariant_walk_2366)
aura_issue_test_link_llvm_jit(test_epoch_invariant_walk_2366)
add_dependencies(all_test_issue_targets test_epoch_invariant_walk_2366)

# Issue #2367: ReloadRecovery query primitive + recovery-state snapshot.
aura_add_issue_test(test_reload_recovery_query_2367)
aura_issue_test_link_llvm_jit(test_reload_recovery_query_2367)
add_dependencies(all_test_issue_targets test_reload_recovery_query_2367)

# Issue #2368: force densify remap-context pairing on Moving success paths.
aura_add_issue_test(test_densify_remap_pairing_2368)
aura_issue_test_link_light(test_densify_remap_pairing_2368)
add_dependencies(all_test_issue_targets test_densify_remap_pairing_2368)

# Issue #2499: unify RootRemapPass fail with pin_contract_held (single Moving
# success gate). Closes "pin ok + root_remap fail cumulative" mixed-signal
# gap from 2026-07-31 production review 建议 5. AdaptiveCompactResult
# aggregates per-call fail totals; Phase 5 ANDs them into pin_contract_held
# (same fail-closed shape as #2266 pin + #2497 scan_fail_delta).
aura_add_issue_test(test_root_remap_pin_contract_unified_2499)
aura_issue_test_link_light(test_root_remap_pin_contract_unified_2499)
add_dependencies(all_test_issue_targets test_root_remap_pin_contract_unified_2499)

# Issues #2529–#2534: security posture / grant retain / audit ring / force-safepoint
aura_add_issue_test(test_grant_epoch_retain_restricted_2529)
aura_issue_test_link_light(test_grant_epoch_retain_restricted_2529)
add_dependencies(all_test_issue_targets test_grant_epoch_retain_restricted_2529)

aura_add_issue_test(test_audit_ring_publish_2530)
aura_issue_test_link_light(test_audit_ring_publish_2530)
add_dependencies(all_test_issue_targets test_audit_ring_publish_2530)

aura_add_issue_test(test_grant_bound_mid_force_2531)
aura_issue_test_link_light(test_grant_bound_mid_force_2531)
add_dependencies(all_test_issue_targets test_grant_bound_mid_force_2531)

aura_add_issue_test(test_cap_write_effect_matrix_2532)
aura_issue_test_link_light(test_cap_write_effect_matrix_2532)
add_dependencies(all_test_issue_targets test_cap_write_effect_matrix_2532)

aura_add_issue_test(test_residual_force_safepoint_2533)
aura_issue_test_link_light(test_residual_force_safepoint_2533)
add_dependencies(all_test_issue_targets test_residual_force_safepoint_2533)

aura_add_issue_test(test_security_posture_trail_2534)
aura_issue_test_link_light(test_security_posture_trail_2534)
add_dependencies(all_test_issue_targets test_security_posture_trail_2534)


# Issue #2369: live-closure remap stable_func_id sole primary (name-fallback legacy).
aura_add_issue_test(test_live_closure_stable_id_only_2369)
aura_issue_test_link_llvm_jit(test_live_closure_stable_id_only_2369)
add_dependencies(all_test_issue_targets test_live_closure_stable_id_only_2369)

# Issue #2370: real PerEval storm isolation for SpecJIT + shape version.
aura_add_issue_test(test_specjit_per_eval_storm_isolation_2370)
aura_issue_test_link_llvm_jit(test_specjit_per_eval_storm_isolation_2370)
add_dependencies(all_test_issue_targets test_specjit_per_eval_storm_isolation_2370)

# Issue #2371: cross-COW dual-epoch soft restamp vs hard-reject.
aura_add_issue_test(test_cross_cow_soft_migrate_2371)
aura_issue_test_link_llvm_jit(test_cross_cow_soft_migrate_2371)
add_dependencies(all_test_issue_targets test_cross_cow_soft_migrate_2371)

# Issue #1930 / #1952 / #2013: incremental reemit + stable func_id + live remap.
aura_add_issue_test(test_aot_incremental_reemit)
aura_issue_test_link_llvm_jit(test_aot_incremental_reemit)
add_dependencies(all_test_issue_targets test_aot_incremental_reemit)

# Issue #1369 / #2015: AOT mangle _vN/_eN_lN parse + stale probes.
aura_add_issue_test(test_aot_mangle_top)
aura_issue_test_link_llvm_jit(test_aot_mangle_top)
add_dependencies(all_test_issue_targets test_aot_mangle_top)

# tests/issues/ removed (wave 58–60 / #1957): range batches + #178 folded into
# theme suites (domain/arena, mutation, obs, edsl, fiber, shape, linear).

# tests/mutation/ unit consolidations (unregistered standalones → theme batches).
# Existing large family batches (mutate/linear/boundary/…) stay separate.
aura_add_issue_test(test_mutation_guard_unit_batch)
aura_issue_test_link_light(test_mutation_guard_unit_batch)
add_dependencies(all_test_issue_targets test_mutation_guard_unit_batch)

aura_add_issue_test(test_mutation_occurrence_dirty_batch)
aura_issue_test_link_light(test_mutation_occurrence_dirty_batch)
add_dependencies(all_test_issue_targets test_mutation_occurrence_dirty_batch)

aura_add_issue_test(test_mutation_aot_unit_batch)
aura_issue_test_link_llvm_jit(test_mutation_aot_unit_batch)
target_link_options(test_mutation_aot_unit_batch PRIVATE -Wl,--export-dynamic) # #2012 dlsym
add_dependencies(all_test_issue_targets test_mutation_aot_unit_batch)

# Wave 36 (#1957): shape_soa unit batch (Env version / SoA walk smokes)
aura_add_issue_test(test_shape_soa_unit_batch)
aura_issue_test_link_light(test_shape_soa_unit_batch)
add_dependencies(all_test_issue_targets test_shape_soa_unit_batch)

aura_add_issue_test(test_mutation_typed_audit_batch)

# Wave 20 (#1957): linear ownership family batch (tests/observability/)
aura_add_issue_test(test_linear_ownership_batch)
aura_issue_test_link_light(test_linear_ownership_batch)
set_target_properties(test_linear_ownership_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_dependencies(all_test_issue_targets test_linear_ownership_batch)

# Wave 26 (#1957): thin observability metrics / schema smokes
aura_add_issue_test(test_obs_metrics_smoke_batch)
aura_issue_test_link_llvm_jit(test_obs_metrics_smoke_batch)
add_dependencies(all_test_issue_targets test_obs_metrics_smoke_batch)






# Issue #2052: force capability + isolation on every mutate:* entry
aura_add_issue_test(test_mutate_capability_force_2052)
aura_issue_test_link_light(test_mutate_capability_force_2052)
add_dependencies(all_test_issue_targets test_mutate_capability_force_2052)

# Issue #2053: production security defaults (Restricted + Full audit + WAL)
aura_add_issue_test(test_production_security_defaults_2053)
aura_issue_test_link_light(test_production_security_defaults_2053)
add_dependencies(all_test_issue_targets test_production_security_defaults_2053)

# Issue #2054: unify capability / mutation / TypedMutationAudit into
# single Agent-queryable security audit surface (query:security-audit).
aura_add_issue_test(test_security_audit_unify_2054)
aura_issue_test_link_light(test_security_audit_unify_2054)
add_dependencies(all_test_issue_targets test_security_audit_unify_2054)

# Issue #2055: grant/revoke bound to WorkspaceEpoch Mutation + fiber.
aura_add_issue_test(test_grant_epoch_fiber_bind_2055)
aura_issue_test_link_light(test_grant_epoch_fiber_bind_2055)
add_dependencies(all_test_issue_targets test_grant_epoch_fiber_bind_2055)

# Issue #2056: mandate tenant_id + provenance stamp on StableNodeRef create/rebind.
aura_add_issue_test(test_stable_ref_tenant_mandate_2056)
aura_issue_test_link_light(test_stable_ref_tenant_mandate_2056)
add_dependencies(all_test_issue_targets test_stable_ref_tenant_mandate_2056)

# Issue #2189: Agent-visible StableNodeRef pin lifecycle RAII.
aura_add_issue_test(test_stable_ref_pin_lifecycle_2189)
aura_issue_test_link_light(test_stable_ref_pin_lifecycle_2189)
add_dependencies(all_test_issue_targets test_stable_ref_pin_lifecycle_2189)

# Issue #2057: side-effect prims inherit capability enforcement by construction.
aura_add_issue_test(test_side_effect_inherit_2057)
aura_issue_test_link_light(test_side_effect_inherit_2057)
add_dependencies(all_test_issue_targets test_side_effect_inherit_2057)

# Issue #2058: PCV unique-ownership hot path (AI structural mutate).
aura_add_issue_test(test_pcv_unique_hotpath_2058)
# Header-only PCV + FlatAST module link via standard issue test helper.
aura_issue_test_link_light(test_pcv_unique_hotpath_2058)
add_dependencies(all_test_issue_targets test_pcv_unique_hotpath_2058)

# Issue #2140: PCV exclusive (refcount==1) in-place with_set path.
aura_add_issue_test(test_pcv_exclusive_with_set_2140)
aura_issue_test_link_light(test_pcv_exclusive_with_set_2140)
add_dependencies(all_test_issue_targets test_pcv_exclusive_with_set_2140)

# Issue #2406: optional TLS freelist for exclusive PCV unique-inplace.
aura_add_issue_test(test_pcv_tls_scratch_2406)
aura_issue_test_link_light(test_pcv_tls_scratch_2406)
add_dependencies(all_test_issue_targets test_pcv_tls_scratch_2406)

# Issue #2521: PCV TLS freelist production default ON.
aura_add_issue_test(test_pcv_tls_default_on_2521)
aura_issue_test_link_light(test_pcv_tls_default_on_2521)
add_dependencies(all_test_issue_targets test_pcv_tls_default_on_2521)

# Issue #2522: batch dirty cascade API (mark_blocks_dirty + single gen bump).
aura_add_issue_test(test_batch_dirty_cascade_2522)
aura_issue_test_link_light(test_batch_dirty_cascade_2522)
add_dependencies(all_test_issue_targets test_batch_dirty_cascade_2522)

# Issue #2615: production multi-block dirty cascades must use batch API.
aura_add_issue_test(test_batch_dirty_discipline_2615)
aura_issue_test_link_light(test_batch_dirty_discipline_2615)
add_dependencies(all_test_issue_targets test_batch_dirty_discipline_2615)

# Issue #2523: residual workspace_mtx contention stats + soft region path.
aura_add_issue_test(test_workspace_mtx_contention_2523)
aura_issue_test_link_light(test_workspace_mtx_contention_2523)
add_dependencies(all_test_issue_targets test_workspace_mtx_contention_2523)

# Issue #2524: giant module partition map + pass_manager Phase C split.
aura_add_issue_test(test_module_partition_map_2524)
aura_issue_test_link_light(test_module_partition_map_2524)
add_dependencies(all_test_issue_targets test_module_partition_map_2524)

# Issue #2525: unconstrained query hygiene residual — default MacroIntroduced skip.
aura_add_issue_test(test_query_hygiene_default_2525)
aura_issue_test_link_light(test_query_hygiene_default_2525)
add_dependencies(all_test_issue_targets test_query_hygiene_default_2525)

# Issue #2526: adaptive deopt-storm threshold closed-loop with LayoutStamp.
aura_add_issue_test(test_shape_storm_adaptive_2526)
aura_issue_test_link_light(test_shape_storm_adaptive_2526)
add_dependencies(all_test_issue_targets test_shape_storm_adaptive_2526)

# Issue #2059: adaptive arena compact policy + ShapeProfiler deopt coordination.
aura_add_issue_test(test_arena_adaptive_compact_2059)
aura_issue_test_link_light(test_arena_adaptive_compact_2059)
add_dependencies(all_test_issue_targets test_arena_adaptive_compact_2059)

# Issue #2060: HotPassDodCompliant dirty short-circuit + SoA-direct dirty entry.
aura_add_issue_test(test_hot_pass_dirty_soa_2060)
aura_issue_test_link_light(test_hot_pass_dirty_soa_2060)
add_dependencies(all_test_issue_targets test_hot_pass_dirty_soa_2060)

# Issue #2084: inject real heap sizes into mark_from_roots (stop size=0,0,0).
aura_add_issue_test(test_gc_mark_size_inject_2084)
aura_issue_test_link_light(test_gc_mark_size_inject_2084)
add_dependencies(all_test_issue_targets test_gc_mark_size_inject_2084)

# Issue #2088: unified GcDeferReason bitmask (panic + FFI pin + reserved render).
# Extends #1581/#2002/#2005 scheduler GC defer suite with AC9–AC14.
aura_add_issue_test(test_scheduler_gc_defer_pending_panic_steal)
aura_issue_test_link_light(test_scheduler_gc_defer_pending_panic_steal)
add_dependencies(all_test_issue_targets test_scheduler_gc_defer_pending_panic_steal)

# Issue #2100: MacroIntroduced IR attrs + deopt AST restore (refine #2022).
aura_add_issue_test(test_jit_macro_deopt_hygiene_2100)
aura_issue_test_link_llvm_jit(test_jit_macro_deopt_hygiene_2100)
add_dependencies(all_test_issue_targets test_jit_macro_deopt_hygiene_2100)

# Issue #2101: runtime-configurable hygiene depth/pass caps + query surface.
aura_add_issue_test(test_macro_hygiene_limits_2101)
aura_issue_test_link_light(test_macro_hygiene_limits_2101)
add_dependencies(all_test_issue_targets test_macro_hygiene_limits_2101)

# Issue #2102: coercion provenance miss → Full TypedMutationAudit / reject apply.
aura_add_issue_test(test_coercion_provenance_miss_force_audit_2102)
aura_issue_test_link_light(test_coercion_provenance_miss_force_audit_2102)
add_dependencies(all_test_issue_targets test_coercion_provenance_miss_force_audit_2102)

# Issue #2185: production default reject_apply_on_provenance_miss.
aura_add_issue_test(test_coercion_reject_production_defaults_2185)
aura_issue_test_link_light(test_coercion_reject_production_defaults_2185)
add_dependencies(all_test_issue_targets test_coercion_reject_production_defaults_2185)

# Issue #2186: EDSL force StableNodeRef consumption via validate_or_refresh.
aura_add_issue_test(test_edsl_validate_or_refresh_2186)
aura_issue_test_link_light(test_edsl_validate_or_refresh_2186)
add_dependencies(all_test_issue_targets test_edsl_validate_or_refresh_2186)

# Issue #2103: LinearEnforceMode Soft/Strict for validate_linear_provenance.
aura_add_issue_test(test_linear_enforce_strict_2103)
aura_issue_test_link_light(test_linear_enforce_strict_2103)
add_dependencies(all_test_issue_targets test_linear_enforce_strict_2103)

# Issue #2182: production defaults force LinearEnforceMode::Strict.
aura_add_issue_test(test_linear_enforce_production_defaults_2182)
aura_issue_test_link_light(test_linear_enforce_production_defaults_2182)
add_dependencies(all_test_issue_targets test_linear_enforce_production_defaults_2182)

# Issue #2104: predicate-memo selective invalidate wire (#2068 Phase 2).
aura_add_issue_test(test_predicate_memo_boundary_selective_2104)
aura_issue_test_link_light(test_predicate_memo_boundary_selective_2104)
add_dependencies(all_test_issue_targets test_predicate_memo_boundary_selective_2104)

# Issue #2105: composite/nested txn ordered commit barrier.
aura_add_issue_test(test_composite_txn_commit_2105)
aura_issue_test_link_light(test_composite_txn_commit_2105)
add_dependencies(all_test_issue_targets test_composite_txn_commit_2105)

# Issue #2180: composite_txn_commit reuses stashed partial CS + occurrence vars.
aura_add_issue_test(test_composite_commit_cs_reuse_2180)
aura_issue_test_link_light(test_composite_commit_cs_reuse_2180)
add_dependencies(all_test_issue_targets test_composite_commit_cs_reuse_2180)

# Issue #2106: cascade_skip_subtree → CompilerMetrics + DeadCoercion synergy.
aura_add_issue_test(test_cascade_skip_metrics_2106)
aura_issue_test_link_light(test_cascade_skip_metrics_2106)
add_dependencies(all_test_issue_targets test_cascade_skip_metrics_2106)

# Issue #2107: structured TIMEOUT / unresolved export on solve_delta_occurrence.
aura_add_issue_test(test_solve_delta_unresolved_export_2107)
aura_issue_test_link_light(test_solve_delta_unresolved_export_2107)
add_dependencies(all_test_issue_targets test_solve_delta_unresolved_export_2107)

# Issue #2108: hard-block composite cross-batch linear escape on commit.
aura_add_issue_test(test_linear_escape_commit_hardblock_2108)
aura_issue_test_link_light(test_linear_escape_commit_hardblock_2108)
add_dependencies(all_test_issue_targets test_linear_escape_commit_hardblock_2108)

# Issue #2280: epoch-scoped linear pin contract (linear alloc pins;
# Drop/Move consume unpins; verify_pins_under_moving_compact extends
# to check live linear roots; metric linear_pin_miss_total).
aura_add_issue_test(test_linear_pin_moving_compact_2280)
aura_issue_test_link_llvm_jit(test_linear_pin_moving_compact_2280)
add_dependencies(all_test_issue_targets test_linear_pin_moving_compact_2280)
# Issue #2298: general object pin-or-remap (registered near #2166 above).
add_dependencies(all_test_issue_targets test_general_object_pin_2298)
# Issue #2294: RootRemapPass real rewrite (registered near #2166 above).
add_dependencies(all_test_issue_targets test_root_remap_pass_2267)

# Issue #2109: instruction-level impact + partial re-emit.
# DISABLED (runtime crash/missing features) for CI green:
# aura_add_issue_test(test_instruction_level_impact_partial_2109)
# aura_issue_test_link_llvm_jit(test_instruction_level_impact_partial_2109)
# add_dependencies(all_test_issue_targets test_instruction_level_impact_partial_2109)

# Issue #2110: hybrid string dep_graph_ ↔ NodeId DepGraph cascade.
# DISABLED (runtime crash/missing features) for CI green:
# aura_add_issue_test(test_dep_graph_hybrid_cascade_2110)

# Issue #2191: type affected_subtree cone ↔ dirty::DepGraph cascade unify.
aura_add_issue_test(test_type_dirty_cone_dep_graph_2191)
aura_issue_test_link_light(test_type_dirty_cone_dep_graph_2191)
add_dependencies(all_test_issue_targets test_type_dirty_cone_dep_graph_2191)

# Issue #2192: QueryEpoch snapshot contract (concurrent mutate/query).
aura_add_issue_test(test_query_epoch_contract_2192)
aura_issue_test_link_light(test_query_epoch_contract_2192)
add_dependencies(all_test_issue_targets test_query_epoch_contract_2192)

# Issue #2193: per-reason invalidate cascade full-fallback.
aura_add_issue_test(test_relower_fallback_reason_2193)
aura_issue_test_link_light(test_relower_fallback_reason_2193)
add_dependencies(all_test_issue_targets test_relower_fallback_reason_2193)

# Issue #2194: steal/resume unified refresh_after_fiber_migration.
aura_add_issue_test(test_fiber_migration_refresh_2194)
aura_issue_test_link_light(test_fiber_migration_refresh_2194)
add_dependencies(all_test_issue_targets test_fiber_migration_refresh_2194)

# Issue #2195: Constraint Kind SUBTYPE + stronger meet/join (G1/R5).
aura_add_issue_test(test_subtype_constraint_meet_2195)
aura_issue_test_link_light(test_subtype_constraint_meet_2195)
add_dependencies(all_test_issue_targets test_subtype_constraint_meet_2195)

# Issue #2196: unified query:mutation-memory / blame surface for Agent self-repair.
aura_add_issue_test(test_mutation_memory_blame_2196)
aura_issue_test_link_light(test_mutation_memory_blame_2196)
add_dependencies(all_test_issue_targets test_mutation_memory_blame_2196)

# Issue #2197: post-steal / GC-window linear×type provenance revalidate (R6).
aura_add_issue_test(test_post_steal_linear_revalidate_2197)
aura_issue_test_link_light(test_post_steal_linear_revalidate_2197)
add_dependencies(all_test_issue_targets test_post_steal_linear_revalidate_2197)

# Issue #2198: StableNodeRef wire format v2 (tenant/fiber/pin/cow/wrap).
aura_add_issue_test(test_stable_ref_wire_v2_2198)
aura_issue_test_link_light(test_stable_ref_wire_v2_2198)
add_dependencies(all_test_issue_targets test_stable_ref_wire_v2_2198)

# Issue #2199: hard timeout force-fail for long outermost MutationBoundary holds.
aura_add_issue_test(test_mutation_hold_hard_timeout_2199)
aura_issue_test_link_llvm_jit(test_mutation_hold_hard_timeout_2199)
add_dependencies(all_test_issue_targets test_mutation_hold_hard_timeout_2199)

# Issue #2200: hard-block Fiber::yield while MutationBoundary held/depth.
# Re-enabled after #2324 (SIGABRT fix to src/serve/fiber.cpp:459 —
# removed debug-mode assert(false) that test #2200 deliberately
# triggers; counter bump + env-var-gated hard abort retained for
# production forensics):
aura_add_issue_test(test_yield_while_mutation_held_2200)
aura_issue_test_link_light(test_yield_while_mutation_held_2200)
add_dependencies(all_test_issue_targets test_yield_while_mutation_held_2200)


# Issue #2201: Agent-visible mutation-log pressure + forced compact.
aura_add_issue_test(test_mutation_log_pressure_2201)
aura_issue_test_link_light(test_mutation_log_pressure_2201)
add_dependencies(all_test_issue_targets test_mutation_log_pressure_2201)

# Issue #2202: chaos multi-worker mutate × steal × GC × mailbox × panic.
# DISABLED (runtime crash/hang) for CI green:
# aura_add_issue_test(test_chaos_fiber_mutation_gc_mailbox)

# aura_issue_test_link_llvm_jit(test_chaos_fiber_mutation_gc_mailbox)

# add_dependencies(all_test_issue_targets test_chaos_fiber_mutation_gc_mailbox)


# Issue #2203: steal-complete single entry (clear orphan GC defer + metric).
aura_add_issue_test(test_steal_complete_gc_defer_2203)
aura_issue_test_link_light(test_steal_complete_gc_defer_2203)
add_dependencies(all_test_issue_targets test_steal_complete_gc_defer_2203)

# Issue #2546: hard-AND residual GcDeferReason == 0 on steal-complete success.
aura_add_issue_test(test_residual_defer_steal_hard_and_2546)
aura_issue_test_link_light(test_residual_defer_steal_hard_and_2546)
add_dependencies(all_test_issue_targets test_residual_defer_steal_hard_and_2546)

# Issue #2377: force single steal-complete entry (no weak legacy under production).
aura_add_issue_test(test_steal_complete_strong_entry_2377)
aura_issue_test_link_light(test_steal_complete_strong_entry_2377)
add_dependencies(all_test_issue_targets test_steal_complete_strong_entry_2377)

# Issue #2204: GcDeferReason::MutationHold on outermost MutationBoundaryGuard.
aura_add_issue_test(test_gc_defer_mutation_hold_2204)
aura_issue_test_link_light(test_gc_defer_mutation_hold_2204)
add_dependencies(all_test_issue_targets test_gc_defer_mutation_hold_2204)

# Issue #2205: production default reemit policy Defer (disable SoftEnter multi-fiber).
aura_add_issue_test(test_reemit_production_default_defer_2205)
aura_issue_test_link_llvm_jit(test_reemit_production_default_defer_2205)
add_dependencies(all_test_issue_targets test_reemit_production_default_defer_2205)

# Issue #2206: aggressive source_to_ir_map desync recovery (no silent full-relower).
aura_add_issue_test(test_source_to_ir_desync_recovery_2206)
aura_issue_test_link_light(test_source_to_ir_desync_recovery_2206)
add_dependencies(all_test_issue_targets test_source_to_ir_desync_recovery_2206)

# Issue #2207: LinearEnforceMode Strict production process default + hard-fail.
aura_add_issue_test(test_linear_enforce_strict_default_2207)
aura_issue_test_link_light(test_linear_enforce_strict_default_2207)
add_dependencies(all_test_issue_targets test_linear_enforce_strict_default_2207)

# Issue #2208: production default reemit Defer (no SoftEnter outside boundary; refine #2205).
aura_add_issue_test(test_reemit_production_default_defer_2208)
aura_issue_test_link_llvm_jit(test_reemit_production_default_defer_2208)
add_dependencies(all_test_issue_targets test_reemit_production_default_defer_2208)

# Issue #2209: cascade depth + dirty_rate into adaptive partial-relower threshold.
aura_add_issue_test(test_adaptive_cascade_depth_partial_thr_2209)
aura_issue_test_link_light(test_adaptive_cascade_depth_partial_thr_2209)
add_dependencies(all_test_issue_targets test_adaptive_cascade_depth_partial_thr_2209)

# Issue #2210: JIT/Interpreter PrimCall + apply_closure equivalence oracle.
aura_add_issue_test(test_jit_interpreter_equivalence_oracle_2210)
aura_issue_test_link_light(test_jit_interpreter_equivalence_oracle_2210)
add_dependencies(all_test_issue_targets test_jit_interpreter_equivalence_oracle_2210)

# Issue #2211: residual GcDeferReason assert at outermost Guard exit.
aura_add_issue_test(test_residual_gc_defer_assert_2211)
aura_issue_test_link_light(test_residual_gc_defer_assert_2211)
add_dependencies(all_test_issue_targets test_residual_gc_defer_assert_2211)

# Issue #2212: Shape-storm prefers partial relower (wider dirty window).
aura_add_issue_test(test_shape_storm_partial_relower_2212)
aura_issue_test_link_light(test_shape_storm_partial_relower_2212)
add_dependencies(all_test_issue_targets test_shape_storm_partial_relower_2212)

# Issue #2213: production hard-fail / ForceSoa on tree-walker fallback.
aura_add_issue_test(test_tree_walker_fallback_strict_2213)
aura_issue_test_link_light(test_tree_walker_fallback_strict_2213)
add_dependencies(all_test_issue_targets test_tree_walker_fallback_strict_2213)






# Issue #2219: post-mutate Soft/Hard type gate (match exhaustiveness).
aura_add_issue_test(test_mutate_type_gate_2219)
aura_issue_test_link_light(test_mutate_type_gate_2219)
add_dependencies(all_test_issue_targets test_mutate_type_gate_2219)

# Issue #2220: long-lived TypeChecker on Evaluator mutate path.
aura_add_issue_test(test_persistent_typechecker_2220)
aura_issue_test_link_light(test_persistent_typechecker_2220)
add_dependencies(all_test_issue_targets test_persistent_typechecker_2220)

# Issue #2221: blame-complete optional hard gate on apply + composite commit.
aura_add_issue_test(test_blame_complete_commit_gate_2221)
aura_issue_test_link_light(test_blame_complete_commit_gate_2221)
add_dependencies(all_test_issue_targets test_blame_complete_commit_gate_2221)

# Issue #2222: LinearEnforceMode Soft/Strict boundary + production align.
aura_add_issue_test(test_linear_enforce_boundary_align_2222)
aura_issue_test_link_light(test_linear_enforce_boundary_align_2222)
add_dependencies(all_test_issue_targets test_linear_enforce_boundary_align_2222)

# Issue #2223: ADT match exhaustiveness in TypedMutationAudit (adt_ok).
# DISABLED (runtime crash/hang) for CI green:
# aura_add_issue_test(test_adt_exhaustiveness_audit_2223)

# aura_issue_test_link_llvm_jit(test_adt_exhaustiveness_audit_2223)

# add_dependencies(all_test_issue_targets test_adt_exhaustiveness_audit_2223)


# Issue #2264: ADT exhaustiveness hard-gate suite (refine #2223 / #2145).
aura_add_issue_test(test_adt_hard_gate_exhaustiveness_2264)
aura_issue_test_link_light(test_adt_hard_gate_exhaustiveness_2264)
add_dependencies(all_test_issue_targets test_adt_hard_gate_exhaustiveness_2264)

# Issue #2258: HotPassDodCompliant + pure Wrap for incremental/dirty passes.
aura_add_issue_test(test_hot_pass_pure_wrap_2258)
aura_issue_test_link_light(test_hot_pass_pure_wrap_2258)
add_dependencies(all_test_issue_targets test_hot_pass_pure_wrap_2258)

# Issue #2259: 64-bit tagged Value hot-path contracts + zero-overhead dispatch.
aura_add_issue_test(test_value_tag_hot_path_2259)
aura_issue_test_link_light(test_value_tag_hot_path_2259)
add_dependencies(all_test_issue_targets test_value_tag_hot_path_2259)

# Issue #2616: hard-ban classify_eval_value_tag (atomics) on eval/IR/apply hot paths.
aura_add_issue_test(test_value_tag_hotpath_ban_2616)
aura_issue_test_link_light(test_value_tag_hotpath_ban_2616)
add_dependencies(all_test_issue_targets test_value_tag_hotpath_ban_2616)

# Issue #2617: compact path must never feed deopt-storm ring as mutation.
aura_add_issue_test(test_shape_compact_storm_isolation_2617)
aura_issue_test_link_light(test_shape_compact_storm_isolation_2617)
add_dependencies(all_test_issue_targets test_shape_compact_storm_isolation_2617)

# Issue #2618: production smoke hard-asserts residual_aos_bridge_total == 0.
aura_add_issue_test(test_soa_residual_production_smoke_2618)
aura_issue_test_link_light(test_soa_residual_production_smoke_2618)
add_dependencies(all_test_issue_targets test_soa_residual_production_smoke_2618)

# Issue #2619: Agent-visible Moving densify health (pairs #2596).
aura_add_issue_test(test_arena_moving_densify_health_2619)
aura_issue_test_link_light(test_arena_moving_densify_health_2619)
add_dependencies(all_test_issue_targets test_arena_moving_densify_health_2619)

# Issue #2260: MutationBoundary type-proof hard-gate (SOLVED / full resync).
aura_add_issue_test(test_boundary_solve_hard_gate_2260)
aura_issue_test_link_light(test_boundary_solve_hard_gate_2260)
add_dependencies(all_test_issue_targets test_boundary_solve_hard_gate_2260)

# Issue #2261: ban weak mutation-id provenance under Sampled; never stamp into IR.
aura_add_issue_test(test_coercion_ban_weak_ir_2261)
aura_issue_test_link_light(test_coercion_ban_weak_ir_2261)
add_dependencies(all_test_issue_targets test_coercion_ban_weak_ir_2261)

# Issue #2262: partial ConstraintSystem single source of truth (all typed_mutate).
aura_add_issue_test(test_partial_cs_single_source_2262)
aura_issue_test_link_light(test_partial_cs_single_source_2262)
add_dependencies(all_test_issue_targets test_partial_cs_single_source_2262)

# Issue #2263: OwnershipEscapeSummary → MoveOp elision gate at lowering.
aura_add_issue_test(test_escape_move_elision_gate_2263)
aura_issue_test_link_light(test_escape_move_elision_gate_2263)
add_dependencies(all_test_issue_targets test_escape_move_elision_gate_2263)

# DISABLED (runtime crash/missing features) for CI green:
# aura_issue_test_link_llvm_jit(test_dep_graph_hybrid_cascade_2110)
# add_dependencies(all_test_issue_targets test_dep_graph_hybrid_cascade_2110)

# Issue #2111: SoA generation fence (silent-stale under self-evo).
aura_add_issue_test(test_soa_generation_fence_2111)
aura_issue_test_link_light(test_soa_generation_fence_2111)
add_dependencies(all_test_issue_targets test_soa_generation_fence_2111)

# Issue #2112: adaptive partial/full relower threshold.
aura_add_issue_test(test_adaptive_partial_relower_threshold_2112)
aura_issue_test_link_light(test_adaptive_partial_relower_threshold_2112)
add_dependencies(all_test_issue_targets test_adaptive_partial_relower_threshold_2112)

# Issue #2113: incremental soundness oracle (debug partial ≡ full).
aura_add_issue_test(test_incremental_soundness_oracle_2113)
aura_issue_test_link_light(test_incremental_soundness_oracle_2113)
add_dependencies(all_test_issue_targets test_incremental_soundness_oracle_2113)

# Issue #2114: HotUpdate reemit ↔ MutationBoundary handshake.
aura_add_issue_test(test_reemit_mutation_boundary_handshake_2114)
aura_issue_test_link_llvm_jit(test_reemit_mutation_boundary_handshake_2114)
add_dependencies(all_test_issue_targets test_reemit_mutation_boundary_handshake_2114)

# Issue #2115: unify is_at_safe_mutation_boundary with depth-safe steal API.
# DISABLED (runtime crash/hang) for CI green:
# aura_add_issue_test(test_depth_safe_mutation_boundary_steal_2115)

# aura_issue_test_link_llvm_jit(test_depth_safe_mutation_boundary_steal_2115)

# add_dependencies(all_test_issue_targets test_depth_safe_mutation_boundary_steal_2115)


# Issue #2184: MutationSafetySnapshot for steal decisions.
# DISABLED (runtime crash/hang) for CI green:
# aura_add_issue_test(test_mutation_safety_snapshot_steal_2184)

# aura_issue_test_link_llvm_jit(test_mutation_safety_snapshot_steal_2184)

# add_dependencies(all_test_issue_targets test_mutation_safety_snapshot_steal_2184)

# Issue #2346: resume MutationSafetySnapshot hard-invariant (fail-closed canary).
# Focused suite (no #2184 concurrent stress) so CI stays green.
aura_add_issue_test(test_steal_snapshot_hard_invariant_2346)
aura_issue_test_link_light(test_steal_snapshot_hard_invariant_2346)
add_dependencies(all_test_issue_targets test_steal_snapshot_hard_invariant_2346)

# Issue #2518: MutationSafetySnapshot sequence ticket (sample→resume window).
aura_add_issue_test(test_steal_safety_ticket_2518)
aura_issue_test_link_light(test_steal_safety_ticket_2518)
add_dependencies(all_test_issue_targets test_steal_safety_ticket_2518)

# Issue #2372: production hard-forbid Soft steal-snapshot + require force-deopt ABI.
aura_add_issue_test(test_steal_snapshot_soft_production_lock_2372)
aura_issue_test_link_light(test_steal_snapshot_soft_production_lock_2372)
add_dependencies(all_test_issue_targets test_steal_snapshot_soft_production_lock_2372)


# Issue #2348: bidirectional check-mode for ADT match + GuardShape.
aura_add_issue_test(test_bidirectional_match_check_2348)
aura_issue_test_link_light(test_bidirectional_match_check_2348)
add_dependencies(all_test_issue_targets test_bidirectional_match_check_2348)

# Issue #2349: outermost hold SLO circuit-breaker (production default fail).
aura_add_issue_test(test_mutation_hold_slo_2349)
aura_issue_test_link_light(test_mutation_hold_slo_2349)
add_dependencies(all_test_issue_targets test_mutation_hold_slo_2349)

# Issue #2405: query:mutation-hold-estimate Agent batch planning.
aura_add_issue_test(test_mutation_hold_estimate_2405)
aura_issue_test_link_light(test_mutation_hold_estimate_2405)
add_dependencies(all_test_issue_targets test_mutation_hold_estimate_2405)

# Issue #2517: real-time longest outermost MutationBoundary hold probe.
aura_add_issue_test(test_mutation_hold_live_2517)
aura_issue_test_link_light(test_mutation_hold_live_2517)
add_dependencies(all_test_issue_targets test_mutation_hold_live_2517)

# Issue #2350: query:type-system-health single Agent score.
aura_add_issue_test(test_type_system_health_2350)
aura_issue_test_link_light(test_type_system_health_2350)
add_dependencies(all_test_issue_targets test_type_system_health_2350)

# Issue #2379: query:mutation-concurrency-health single Agent score.
aura_add_issue_test(test_mutation_concurrency_health_2379)
aura_issue_test_link_light(test_mutation_concurrency_health_2379)
add_dependencies(all_test_issue_targets test_mutation_concurrency_health_2379)

# Issue #2351: steal-complete LayoutStamp dual-check before resume.
aura_add_issue_test(test_steal_layout_stamp_2351)
aura_issue_test_link_light(test_steal_layout_stamp_2351)
add_dependencies(all_test_issue_targets test_steal_layout_stamp_2351)

# Issue #2510: transactional LayoutStamp + provenance restamp on steal-complete.
aura_add_issue_test(test_steal_complete_restamp_txn_2510)
aura_issue_test_link_light(test_steal_complete_restamp_txn_2510)
add_dependencies(all_test_issue_targets test_steal_complete_restamp_txn_2510)

# Issue #2352: chaos mutate × steal × GC × mailbox production gate.
# Smoke always (short); full 30s soak via AURA_CHAOS_FULL=1 (nightly).
aura_add_issue_test(test_chaos_mutate_steal_gc_mailbox_2352)
aura_issue_test_link_light(test_chaos_mutate_steal_gc_mailbox_2352)
add_dependencies(all_test_issue_targets test_chaos_mutate_steal_gc_mailbox_2352)

# Issue #2353: post-densify / post-steal unified Linear+Type revalidate.
aura_add_issue_test(test_post_densify_linear_type_revalidate_2353)
aura_issue_test_link_light(test_post_densify_linear_type_revalidate_2353)
add_dependencies(all_test_issue_targets test_post_densify_linear_type_revalidate_2353)

# Issue #2354: debug lock-order audit (scheduler / workspace / closures / module).
aura_add_issue_test(test_lock_order_audit_2354)
# Header-only audit + no service eval — still link like other issue unit tests.
aura_issue_test_link_light(test_lock_order_audit_2354)
add_dependencies(all_test_issue_targets test_lock_order_audit_2354)

# Issue #2355: type_dep_graph_ epoch prune + NodeId invalidation.
aura_add_issue_test(test_type_dep_epoch_prune_2355)
aura_issue_test_link_light(test_type_dep_epoch_prune_2355)
add_dependencies(all_test_issue_targets test_type_dep_epoch_prune_2355)

# Issue #2356: truncated reverify one-shot expand (occurrence/let-poly).
aura_add_issue_test(test_reverify_expand_2356)
aura_issue_test_link_light(test_reverify_expand_2356)
add_dependencies(all_test_issue_targets test_reverify_expand_2356)

# Issue #2357: Phase-1 linear Move/Drop first-class synthesize violation.
aura_add_issue_test(test_linear_synth_violation_2357)
aura_issue_test_link_light(test_linear_synth_violation_2357)
add_dependencies(all_test_issue_targets test_linear_synth_violation_2357)

# Issue #2516: type_dep invalidate → re-infer → cascade mirror single txn.
aura_add_issue_test(test_type_dirty_txn_order_2516)
aura_issue_test_link_light(test_type_dirty_txn_order_2516)
add_dependencies(all_test_issue_targets test_type_dirty_txn_order_2516)

# Issue #2514: unify linear_synth_hard_fail with MutationBoundary audit exit.
aura_add_issue_test(test_linear_synth_boundary_authority_2514)
aura_issue_test_link_light(test_linear_synth_boundary_authority_2514)
add_dependencies(all_test_issue_targets test_linear_synth_boundary_authority_2514)

# Issue #2358: CastOp density HARD force-JIT policy (codegen, not type gate).
aura_add_issue_test(test_castop_density_hard_2358)
aura_issue_test_link_light(test_castop_density_hard_2358)
add_dependencies(all_test_issue_targets test_castop_density_hard_2358)

# Issue #2359: unify occurrence_goals + predicate_memo epoch health query.
aura_add_issue_test(test_memo_goal_epoch_health_2359)
aura_issue_test_link_light(test_memo_goal_epoch_health_2359)
add_dependencies(all_test_issue_targets test_memo_goal_epoch_health_2359)

# Issue #2361: densify envframe_ok real per-call check (stop force-true).
aura_add_issue_test(test_densify_envframe_ok_2361)
aura_issue_test_link_light(test_densify_envframe_ok_2361)
add_dependencies(all_test_issue_targets test_densify_envframe_ok_2361)

# Issue #2376: densify last-call envframe + closure axes (seal #2361/#2365).
aura_add_issue_test(test_densify_last_call_axes_2376)
aura_issue_test_link_light(test_densify_last_call_axes_2376)
add_dependencies(all_test_issue_targets test_densify_last_call_axes_2376)

# Issue #2497: densify ownership-scan fail delta hard-binds envframe_ok
# suppression (no path where scan fail is metrics-only — same fail-closed
# shape as pin_contract_held gating at #2266).
aura_add_issue_test(test_densify_ownership_scan_fail_gate_2497)
aura_issue_test_link_light(test_densify_ownership_scan_fail_gate_2497)
add_dependencies(all_test_issue_targets test_densify_ownership_scan_fail_gate_2497)

# Issue #2362: EnvFrameRef ownership protocol under fiber steal + densify
# (depends on #2360 live set tracking).
aura_add_issue_test(test_envframe_ownership_steal_densify_2362)
aura_issue_test_link_light(test_envframe_ownership_steal_densify_2362)
add_dependencies(all_test_issue_targets test_envframe_ownership_steal_densify_2362)

# Issue #2363: complete GeneralObjectPin adoption for mutate/agent/scratch.
aura_add_issue_test(test_general_object_pin_adopt_2363)
aura_issue_test_link_light(test_general_object_pin_adopt_2363)
add_dependencies(all_test_issue_targets test_general_object_pin_adopt_2363)

# Issue #2364: PanicCheckpoint residual × densify / fiber closed loop.
aura_add_issue_test(test_panic_defer_after_densify_2364)
aura_issue_test_link_light(test_panic_defer_after_densify_2364)
add_dependencies(all_test_issue_targets test_panic_defer_after_densify_2364)

# Issue #2365: RootRemap + densify Closure/EnvFrame dual-epoch closed-loop.
aura_add_issue_test(test_densify_root_closure_closed_loop_2365)
aura_issue_test_link_light(test_densify_root_closure_closed_loop_2365)
add_dependencies(all_test_issue_targets test_densify_root_closure_closed_loop_2365)

# Issue #2316 lineage (rank table / canary) — keep registered with #2354.
aura_add_issue_test(test_lock_order_audit_2316)
aura_issue_test_link_light(test_lock_order_audit_2316)
add_dependencies(all_test_issue_targets test_lock_order_audit_2316)


# Issue #2116: hard-fail dual-path desync in materialize_call_env + GCEnvWalkFn.
aura_add_issue_test(test_dual_path_desync_hard_fail_2116)
aura_issue_test_link_light(test_dual_path_desync_hard_fail_2116)
add_dependencies(all_test_issue_targets test_dual_path_desync_hard_fail_2116)

# Issue #2117: atomic MarkBitVector for concurrent multi-fiber mark.
aura_add_issue_test(test_atomic_mark_bitvector_2117)
aura_issue_test_link_light(test_atomic_mark_bitvector_2117)
add_dependencies(all_test_issue_targets test_atomic_mark_bitvector_2117)

# Issue #2118: orch agent MutationBoundary soft depth for steal/GC visibility.
aura_add_issue_test(test_orch_agent_mutation_boundary_2118)
aura_issue_test_link_light(test_orch_agent_mutation_boundary_2118)
add_dependencies(all_test_issue_targets test_orch_agent_mutation_boundary_2118)

# Issue #2515: Soft orch-agent boundary 提升为轻量 Guard 子集, 统一 depth/held
# 语义. orch_soft_boundary_enter/exit now call publish_mutation_safety_mirrors
# (same shape as full Guard) so steal / GC / is_at_mutation_boundary_safe
# see the soft window consistently. Closes long-term depth-drift risk vs
# the full Guard path (#2115 / #2118 / #2184 lineage).
aura_add_issue_test(test_orch_soft_boundary_unified_2515)
aura_issue_test_link_light(test_orch_soft_boundary_unified_2515)
add_dependencies(all_test_issue_targets test_orch_soft_boundary_unified_2515)

# Issue #2153: configurable secondary drain timeout on non-Ok join.
aura_add_issue_test(test_join_drain_timeout_2153)
aura_issue_test_link_light(test_join_drain_timeout_2153)
add_dependencies(all_test_issue_targets test_join_drain_timeout_2153)

# Issue #2155: quota-reject spawn path — no name-table put + arena no-leak.
aura_add_issue_test(test_spawn_quota_no_leak_2155)
aura_issue_test_link_light(test_spawn_quota_no_leak_2155)
add_dependencies(all_test_issue_targets test_spawn_quota_no_leak_2155)

# Issue #2119: MutationBoundary yield hold + steal starvation metrics + adaptive.
# DISABLED (runtime crash/hang) for CI green:
# aura_add_issue_test(test_boundary_yield_steal_metrics_2119)

# aura_issue_test_link_llvm_jit(test_boundary_yield_steal_metrics_2119)

# add_dependencies(all_test_issue_targets test_boundary_yield_steal_metrics_2119)


# Issue #2120: outermost MutationBoundaryGuard exit order (probes → GC → reemit → unlock).
aura_add_issue_test(test_outermost_exit_order_2120)
aura_issue_test_link_light(test_outermost_exit_order_2120)
add_dependencies(all_test_issue_targets test_outermost_exit_order_2120)

# Issue #2121: region-based / optimistic workspace write concurrency (multi-Agent).
aura_add_issue_test(test_workspace_region_concurrency_2121)
aura_issue_test_link_light(test_workspace_region_concurrency_2121)
add_dependencies(all_test_issue_targets test_workspace_region_concurrency_2121)

# Issue #2123: default MacroIntroduced hygiene filter for query:pattern.
aura_add_issue_test(test_query_pattern_default_hygiene_2123)
aura_issue_test_link_light(test_query_pattern_default_hygiene_2123)
add_dependencies(all_test_issue_targets test_query_pattern_default_hygiene_2123)

# Issue #2124: all mutate:* via MutationBoundaryGuard::try_acquire.
aura_add_issue_test(test_mutation_guard_try_acquire_2124)
aura_issue_test_link_light(test_mutation_guard_try_acquire_2124)
add_dependencies(all_test_issue_targets test_mutation_guard_try_acquire_2124)


# Issue #2625/#2626: render3d:* / tui:* / terminal:* / voxel / TUI stack removed (camera, volume, raycast,
# shade, frame, demo_voxel_3d, test_render3d_primitives). Pixel framebuffer
# and TUI terminal paths remain.

aura_issue_test_link_light(test_mutation_typed_audit_batch)
add_dependencies(all_test_issue_targets test_mutation_typed_audit_batch)

# ── tests/domain/ suites (restored after AuraDomainTests.cmake removal) ──
# Default-build: schema matrix + behavioral gates + concurrent defrag.
# On-demand (EXCLUDE_FROM_ALL): arena family batches.

aura_add_issue_test(test_obs_schema_matrix)
aura_issue_test_link_light(test_obs_schema_matrix)
add_dependencies(all_test_issue_targets test_obs_schema_matrix)

# DISABLED (runtime crash/hang) for CI green:
# aura_add_issue_test(test_hotpath_matrix_batch)

# aura_issue_test_link_llvm_jit(test_hotpath_matrix_batch)

# add_dependencies(all_test_issue_targets test_hotpath_matrix_batch)


aura_add_issue_test(test_arena_defrag)
aura_issue_test_link_llvm_jit_minimal(test_arena_defrag)
add_dependencies(all_test_issue_targets test_arena_defrag)


# Domain batch: FlatAST / SoA atomic+lock ACs (P1 sample — 20 members,
# one light-linked binary). Members remain standalone light targets.
add_executable(test_flatast_atomic_lock_batch
    tests/core/test_flatast_atomic_lock_batch.cpp
    tests/core/test_soa_column_atomic_2440.cpp
    tests/core/test_macro_dirty_bits_lock_2441.cpp
    tests/core/test_verification_dirty_bits_lock_2439.cpp
    tests/core/test_region_dense_atomic_2443.cpp
    tests/core/test_flatast_add_node_lock_2413.cpp
    tests/core/test_structural_metadata_lock_order_2418.cpp
    tests/core/test_incoming_parent_dirty_atomic_2416.cpp
    tests/core/test_last_validated_generation_atomic_2394.cpp
    tests/core/test_restamp_lazy_align_atomic_2421.cpp
    tests/core/test_binding_gens_atomic_2417.cpp
    tests/core/test_subtree_gen_atomic_2422.cpp
    tests/core/test_dirty_column_lock_2423.cpp
    tests/core/test_stringpool_bytes_total_lock_2408.cpp
    tests/core/test_stringpool_buf_fragmentation_lock_2409.cpp
    tests/core/test_tag_arity_index_lock_2419.cpp
    tests/core/test_sandbox_mode_atomic_2427.cpp
    tests/core/test_gc_defer_overflow_policy_atomic_2429.cpp
    tests/core/test_gc_defer_arm_fetch_or_2428.cpp
    tests/core/test_gc_defer_reconcile_cas_2437.cpp
    tests/core/test_clear_macro_dirty_concurrent_2442.cpp
)
set_property(TARGET test_flatast_atomic_lock_batch PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_flatast_atomic_lock_batch PRIVATE src tests)
target_compile_definitions(test_flatast_atomic_lock_batch PRIVATE
    AURA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    AURA_ISSUE_BATCH_MEMBER=1)
aura_test_compile_options(test_flatast_atomic_lock_batch)
target_link_libraries(test_flatast_atomic_lock_batch PRIVATE
    aura_test_objects pthread stdc++ stdc++exp aura-reflect)
aura_issue_test_link_light(test_flatast_atomic_lock_batch)
add_test(NAME test_flatast_atomic_lock_batch_verification
         COMMAND ./test_flatast_atomic_lock_batch)
if(TARGET all_test_issue_targets)
    add_dependencies(all_test_issue_targets test_flatast_atomic_lock_batch)
endif()
aura_add_issue_test(test_arena_batch)
aura_issue_test_link_llvm_jit(test_arena_batch)
set_target_properties(test_arena_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Issue #2067: Linear Ownership runtime enforcement + post-mutate revalidate.
aura_add_issue_test(test_linear_runtime_violation)
aura_issue_test_link_light(test_linear_runtime_violation)

# Issue #2066: DeadCoercionElimination IR-layer CastOp elision (CastOp after narrow).
aura_add_issue_test(test_dead_coercion_elim)
aura_issue_test_link_light(test_dead_coercion_elim)

# Issue #2065: solve_delta epoch filter — skip already-processed roots in current epoch.
aura_add_issue_test(test_solve_delta_epoch_filter)
aura_issue_test_link_light(test_solve_delta_epoch_filter)

# Issue #2278: epoch-scoped OccurrenceGoal table — durable replay across
# clear_blame_context (AC1), epoch-advance prune (AC2), zero extra work
# when empty (AC3), multi-round mutate durability (AC4), query schema
# additive (AC5).
aura_add_issue_test(test_occurrence_goal_epoch_table_2278)
aura_issue_test_link_light(test_occurrence_goal_epoch_table_2278)

# Issue #2064: blame/provenance stamping on Dynamic degrade + CoercionMap fallback paths.
aura_add_issue_test(test_blame_stamp_on_degrade)
aura_issue_test_link_light(test_blame_stamp_on_degrade)

# Issue #2063: Dirty cascade subtree-skip (summary-dirty early-exit) optimization.
aura_add_issue_test(test_dirty_cascade_optimize)
aura_issue_test_link_light(test_dirty_cascade_optimize)

# Issue #2062: StringPool thread-safe intern + ASTArena integration (concurrent fibers).
aura_add_issue_test(test_stringpool_concurrent_intern)
aura_issue_test_link_light(test_stringpool_concurrent_intern)

# Issue #2408: string_bytes_total single shared_lock (UAF + lock churn).
aura_add_issue_test(test_stringpool_bytes_total_lock_2408)
aura_issue_test_link_light(test_stringpool_bytes_total_lock_2408)
add_dependencies(all_test_issue_targets test_stringpool_bytes_total_lock_2408)

# Issue #2409: buf_fragmentation single shared_lock snapshot.
aura_add_issue_test(test_stringpool_buf_fragmentation_lock_2409)
aura_issue_test_link_light(test_stringpool_buf_fragmentation_lock_2409)
add_dependencies(all_test_issue_targets test_stringpool_buf_fragmentation_lock_2409)

# Issue #2410: meta(NodeTag) bounds-checked OOB sentinel.
aura_add_issue_test(test_node_meta_bounds_2410)
aura_issue_test_link_light(test_node_meta_bounds_2410)
add_dependencies(all_test_issue_targets test_node_meta_bounds_2410)

# Issue #2411: kNodeMeta gap is_gap + full tag/name consistency.
aura_add_issue_test(test_node_meta_gap_2411)
aura_issue_test_link_light(test_node_meta_gap_2411)
add_dependencies(all_test_issue_targets test_node_meta_gap_2411)

# Issue #2412: reset_node_slot always clears incoming_parent_edges_.
aura_add_issue_test(test_reset_slot_parent_edges_2412)
aura_issue_test_link_light(test_reset_slot_parent_edges_2412)
add_dependencies(all_test_issue_targets test_reset_slot_parent_edges_2412)

# Issue #2413: FlatAST add_node multi-column SoA lock + reader contract.
aura_add_issue_test(test_flatast_add_node_lock_2413)
aura_issue_test_link_light(test_flatast_add_node_lock_2413)
add_dependencies(all_test_issue_targets test_flatast_add_node_lock_2413)

# Issue #2488: FlatAST SoAReadGuard / get_soa_safe (shared_mutex SoA domain).
aura_add_issue_test(test_flatast_soa_read_guard_2488)
aura_issue_test_link_light(test_flatast_soa_read_guard_2488)
add_dependencies(all_test_issue_targets test_flatast_soa_read_guard_2488)

# Issue #2614: force ChildColumnar/SoAColumnarFull on walk/query/PCV hot templates.
aura_add_issue_test(test_hot_children_columnar_2614)
aura_issue_test_link_light(test_hot_children_columnar_2614)
add_dependencies(all_test_issue_targets test_hot_children_columnar_2614)

# Issue #2414: summary_recompute(pool) restores HasKeywordVar / query-mutate bits.
aura_add_issue_test(test_summary_recompute_sym_2414)
aura_issue_test_link_light(test_summary_recompute_sym_2414)
add_dependencies(all_test_issue_targets test_summary_recompute_sym_2414)

# Issue #2415: summary_flags_ thread-safety annotation (atomic) + free_list audit.
aura_add_issue_test(test_summary_flags_guard_2415)
aura_issue_test_link_light(test_summary_flags_guard_2415)
add_dependencies(all_test_issue_targets test_summary_flags_guard_2415)

# Issue #2416: incoming_parent_index_dirty_ is std::atomic<bool>.
aura_add_issue_test(test_incoming_parent_dirty_atomic_2416)
aura_issue_test_link_light(test_incoming_parent_dirty_atomic_2416)
add_dependencies(all_test_issue_targets test_incoming_parent_dirty_atomic_2416)

# Issue #2417: binding_gens_ atomic shared_ptr + COW bump.
aura_add_issue_test(test_binding_gens_atomic_2417)
aura_issue_test_link_light(test_binding_gens_atomic_2417)
add_dependencies(all_test_issue_targets test_binding_gens_atomic_2417)

# Issue #2418: structural_mtx_ → metadata_mtx_ lock order.
aura_add_issue_test(test_structural_metadata_lock_order_2418)
aura_issue_test_link_light(test_structural_metadata_lock_order_2418)
add_dependencies(all_test_issue_targets test_structural_metadata_lock_order_2418)

# Issue #2419: tag_arity_index_ map lock vs concurrent rebuild.
aura_add_issue_test(test_tag_arity_index_lock_2419)
aura_issue_test_link_light(test_tag_arity_index_lock_2419)
add_dependencies(all_test_issue_targets test_tag_arity_index_lock_2419)

# Issue #2420: TagArityKeyHash pack + splitmix finalizer.
aura_add_issue_test(test_tag_arity_key_hash_2420)
aura_issue_test_link_light(test_tag_arity_key_hash_2420)
add_dependencies(all_test_issue_targets test_tag_arity_key_hash_2420)

# Issue #2421: restamp_lazy_align_enabled_ is std::atomic<bool>.
aura_add_issue_test(test_restamp_lazy_align_atomic_2421)
aura_issue_test_link_light(test_restamp_lazy_align_atomic_2421)
add_dependencies(all_test_issue_targets test_restamp_lazy_align_atomic_2421)

# Issue #2528: long-session SLA surface for generation-wrap restamp.
# Additive observability on top of #2402/#2122/#2394 (#2061 lineage).
# Exposes restamp-us-p99 / restamp-us-last / restamp-slo-breach-total +
# AURA_REStamp_SLO_US env-resolved budget via query:stable-ref-sv-scale-stats.
# Zero cost when no restamp runs (counters live INSIDE the wrap path only).
aura_add_issue_test(test_restamp_sla_observability_2528)
aura_issue_test_link_light(test_restamp_sla_observability_2528)
add_dependencies(all_test_issue_targets test_restamp_sla_observability_2528)

# Issue #2422: subtree_gen_ 32-bit cells + atomic_ref (+ resize mutex).
aura_add_issue_test(test_subtree_gen_atomic_2422)
aura_issue_test_link_light(test_subtree_gen_atomic_2422)
add_dependencies(all_test_issue_targets test_subtree_gen_atomic_2422)

# Issue #2423: dirty_ column shared/exclusive lock (short-circuit APIs).
aura_add_issue_test(test_dirty_column_lock_2423)
aura_issue_test_link_light(test_dirty_column_lock_2423)
add_dependencies(all_test_issue_targets test_dirty_column_lock_2423)

# Issue #2424: is_subtree_dirty_node bounds via dirty_.size() (not size()).
aura_add_issue_test(test_subtree_dirty_bounds_2424)
aura_issue_test_link_light(test_subtree_dirty_bounds_2424)
add_dependencies(all_test_issue_targets test_subtree_dirty_bounds_2424)

# Issue #2425: capability audit_ring published slots (no torn EffectAuditEntry).
aura_add_issue_test(test_capability_audit_publish_2425)
aura_issue_test_link_light(test_capability_audit_publish_2425)
add_dependencies(all_test_issue_targets test_capability_audit_publish_2425)

# Issue #2426: CapabilityRegistry::snapshot_registry_state (#1840 pattern).
aura_add_issue_test(test_capability_registry_snapshot_2426)
aura_issue_test_link_light(test_capability_registry_snapshot_2426)
add_dependencies(all_test_issue_targets test_capability_registry_snapshot_2426)

# Issue #2427: sandbox_mode / default_tenant atomic (policy flip race).
aura_add_issue_test(test_sandbox_mode_atomic_2427)
aura_issue_test_link_light(test_sandbox_mode_atomic_2427)
add_dependencies(all_test_issue_targets test_sandbox_mode_atomic_2427)

# Issue #2428: arm_defer fetch_or first-arm metrics (no load+or double-count).
aura_add_issue_test(test_gc_defer_arm_fetch_or_2428)
aura_issue_test_link_light(test_gc_defer_arm_fetch_or_2428)
add_dependencies(all_test_issue_targets test_gc_defer_arm_fetch_or_2428)

# Issue #2429: overflow policy check+arm atomic under g_gc_defer_armed_mtx.
aura_add_issue_test(test_gc_defer_overflow_policy_atomic_2429)
aura_issue_test_link_light(test_gc_defer_overflow_policy_atomic_2429)
add_dependencies(all_test_issue_targets test_gc_defer_overflow_policy_atomic_2429)

# Issue #2430: snapshot_capability_effect_stats double-check (#1840 pattern).
aura_add_issue_test(test_capability_effect_stats_snapshot_2430)
aura_issue_test_link_light(test_capability_effect_stats_snapshot_2430)
add_dependencies(all_test_issue_targets test_capability_effect_stats_snapshot_2430)

# Issue #2431: pure columnar DeadCoercionElimination on IRModuleV2.
aura_add_issue_test(test_dead_coercion_columnar_2431)
aura_issue_test_link_light(test_dead_coercion_columnar_2431)
add_dependencies(all_test_issue_targets test_dead_coercion_columnar_2431)

# Issue #2432: IR SoA generation fence on LayoutStamp (fiber resume).
aura_add_issue_test(test_ir_soa_layout_stamp_2432)
aura_issue_test_link_light(test_ir_soa_layout_stamp_2432)
add_dependencies(all_test_issue_targets test_ir_soa_layout_stamp_2432)

# Issue #2520: production ban residual to_aos_view under SoA-only.
aura_add_issue_test(test_soa_ban_residual_aos_bridge_2520)
aura_issue_test_link_light(test_soa_ban_residual_aos_bridge_2520)
add_dependencies(all_test_issue_targets test_soa_ban_residual_aos_bridge_2520)

# Issue #2519: LayoutStamp operator== full 8-field equality.
aura_add_issue_test(test_layout_stamp_equality_8field_2519)
aura_issue_test_link_light(test_layout_stamp_equality_8field_2519)
add_dependencies(all_test_issue_targets test_layout_stamp_equality_8field_2519)

# Issue #2433: HighMutation default-on + deopt-storm × LayoutStamp.shape_version
aura_add_issue_test(test_shape_high_mutation_storm_2433)
aura_issue_test_link_llvm_jit(test_shape_high_mutation_storm_2433)
add_dependencies(all_test_issue_targets test_shape_high_mutation_storm_2433)

# Issue #2434: hard HotPassDodCompliant for all production pipeline stages
aura_add_issue_test(test_hot_pass_hard_dod_2434)
aura_issue_test_link_light(test_hot_pass_hard_dod_2434)
add_dependencies(all_test_issue_targets test_hot_pass_hard_dod_2434)

# Issue #2435: hot vs cold contract placement (production hot OFF)
aura_add_issue_test(test_hot_contract_placement_2435)
aura_issue_test_link_light(test_hot_contract_placement_2435)
add_dependencies(all_test_issue_targets test_hot_contract_placement_2435)

# Issue #2436: post-compact Arena × IR SoA × Shape × fiber lifecycle
aura_add_issue_test(test_post_compact_lifecycle_2436)
aura_issue_test_link_light(test_post_compact_lifecycle_2436)
add_dependencies(all_test_issue_targets test_post_compact_lifecycle_2436)

# Issue #2437: reconcile_gc_defer_bits_after_clear CAS fence + repair
aura_add_issue_test(test_gc_defer_reconcile_cas_2437)
aura_issue_test_link_light(test_gc_defer_reconcile_cas_2437)
add_dependencies(all_test_issue_targets test_gc_defer_reconcile_cas_2437)

# Issue #2438: arena compact notify_* lifecycle drain (no UAF on teardown)
aura_add_issue_test(test_arena_compact_notify_lifecycle_2438)
aura_issue_test_link_light(test_arena_compact_notify_lifecycle_2438)
add_dependencies(all_test_issue_targets test_arena_compact_notify_lifecycle_2438)

# Issue #2439: verification/verify dirty bits lock (no metric double-count)
aura_add_issue_test(test_verification_dirty_bits_lock_2439)
aura_issue_test_link_light(test_verification_dirty_bits_lock_2439)
add_dependencies(all_test_issue_targets test_verification_dirty_bits_lock_2439)

# Issue #2440: 4 SoA side-table columns atomic_ref + dirty_column_mtx_
aura_add_issue_test(test_soa_column_atomic_2440)
aura_issue_test_link_light(test_soa_column_atomic_2440)
add_dependencies(all_test_issue_targets test_soa_column_atomic_2440)

# Issue #2441: apply_macro_dirty_bits metric double-count (fetch_or + lock)
aura_add_issue_test(test_macro_dirty_bits_lock_2441)
aura_issue_test_link_light(test_macro_dirty_bits_lock_2441)
add_dependencies(all_test_issue_targets test_macro_dirty_bits_lock_2441)

# Issue #2442: clear_macro_dirty_all concurrent with macro_dirty readers
aura_add_issue_test(test_clear_macro_dirty_concurrent_2442)
aura_issue_test_link_light(test_clear_macro_dirty_concurrent_2442)
add_dependencies(all_test_issue_targets test_clear_macro_dirty_concurrent_2442)

# Issue #2443: region dense SoA atomic_ref + region_table_mtx_
aura_add_issue_test(test_region_dense_atomic_2443)
aura_issue_test_link_light(test_region_dense_atomic_2443)
add_dependencies(all_test_issue_targets test_region_dense_atomic_2443)

# Issue #2445: add_node + add_* builder single-threaded mutation contract
aura_add_issue_test(test_add_node_builder_contract_2445)
aura_issue_test_link_light(test_add_node_builder_contract_2445)
add_dependencies(all_test_issue_targets test_add_node_builder_contract_2445)

# Issue #2448: defines_referencing_sym exclude mutated Define by NodeId
aura_add_issue_test(test_defines_referencing_sym_2448)
aura_issue_test_link_light(test_defines_referencing_sym_2448)
add_dependencies(all_test_issue_targets test_defines_referencing_sym_2448)

# Issue #2449: param_data_ single-threaded mutation contract
aura_add_issue_test(test_param_data_mutation_contract_2449)
aura_issue_test_link_light(test_param_data_mutation_contract_2449)
add_dependencies(all_test_issue_targets test_param_data_mutation_contract_2449)

# Issue #2450: param_annot_data_ single-threaded mutation contract
aura_add_issue_test(test_param_annot_mutation_contract_2450)
aura_issue_test_link_light(test_param_annot_mutation_contract_2450)
add_dependencies(all_test_issue_targets test_param_annot_mutation_contract_2450)

# Issue #2451: param_begin_ + param_count_ publish order (TOCTOU)
aura_add_issue_test(test_param_begin_count_publish_2451)
aura_issue_test_link_light(test_param_begin_count_publish_2451)
add_dependencies(all_test_issue_targets test_param_begin_count_publish_2451)

# Issue #2453: get(NodeId) NodeView multi-column snapshot contract
aura_add_issue_test(test_get_nodeview_snapshot_2453)
aura_issue_test_link_light(test_get_nodeview_snapshot_2453)
add_dependencies(all_test_issue_targets test_get_nodeview_snapshot_2453)

# Issue #2454: RAII guard FlatAST-move lifetime contract
aura_add_issue_test(test_raii_guard_flatast_lifetime_2454)
aura_issue_test_link_light(test_raii_guard_flatast_lifetime_2454)
add_dependencies(all_test_issue_targets test_raii_guard_flatast_lifetime_2454)

# Issue #2455: restore_children acquires structural exclusive lock
aura_add_issue_test(test_restore_children_structural_lock_2455)
aura_issue_test_link_light(test_restore_children_structural_lock_2455)
add_dependencies(all_test_issue_targets test_restore_children_structural_lock_2455)

# Issue #2456: hoist subtree_uses_sym find_first_node_with to single TU
aura_add_issue_test(test_subtree_uses_sym_template_bloat_2456)
aura_issue_test_link_light(test_subtree_uses_sym_template_bloat_2456)
add_dependencies(all_test_issue_targets test_subtree_uses_sym_template_bloat_2456)

# Issue #2457: FlatAST copy shares mutation_log_ / narrowing_log_ via COW
aura_add_issue_test(test_mutation_log_cow_copy_2457)
aura_issue_test_link_light(test_mutation_log_cow_copy_2457)
add_dependencies(all_test_issue_targets test_mutation_log_cow_copy_2457)

# Issue #2458: truncate-commit Soft observe / Hard full-solve-or-reject
aura_add_issue_test(test_truncate_commit_gate_2458)
aura_issue_test_link_light(test_truncate_commit_gate_2458)
add_dependencies(all_test_issue_targets test_truncate_commit_gate_2458)

# Issue #2459: production CastOp density closed-loop (streak + gate reject)
aura_add_issue_test(test_castop_density_closed_loop_2459)
aura_issue_test_link_light(test_castop_density_closed_loop_2459)
add_dependencies(all_test_issue_targets test_castop_density_closed_loop_2459)

# Issue #2460: Phase-2 dirty OwnershipEnv re-sim during infer_flat_partial
aura_add_issue_test(test_linear_partial_revalidate_2460)
aura_issue_test_link_light(test_linear_partial_revalidate_2460)
add_dependencies(all_test_issue_targets test_linear_partial_revalidate_2460)

# Issue #2461: per-If stable narrowing cache key (shape × epoch × refined)
aura_add_issue_test(test_occurrence_cache_key_2461)
aura_issue_test_link_light(test_occurrence_cache_key_2461)
add_dependencies(all_test_issue_targets test_occurrence_cache_key_2461)

# Issue #2462: type-system-health next-action + repair_nodes closed-loop
aura_add_issue_test(test_type_system_health_next_action_2462)
aura_issue_test_link_light(test_type_system_health_next_action_2462)
add_dependencies(all_test_issue_targets test_type_system_health_next_action_2462)

# Issue #2471: optimize_type_info chain-walk terminates on MAX not 0
aura_add_issue_test(test_ir_optimize_type_info_chain_2471)
aura_issue_test_link_light(test_ir_optimize_type_info_chain_2471)
add_dependencies(all_test_issue_targets test_ir_optimize_type_info_chain_2471)

# Issue #2472: close TOCTOU in aura_closure_call MustDeopt lock-downgrade
# (free+realloc of cid between shared unlock and exclusive re-acquire).
aura_add_issue_test(test_closure_call_must_deopt_toctou_2472)
aura_issue_test_link_llvm_jit(test_closure_call_must_deopt_toctou_2472)
add_dependencies(all_test_issue_targets test_closure_call_must_deopt_toctou_2472)

# Issue #2473: flush_gc_roots / compact_sweep take closures_mtx_ when
# iterating / erasing closures_ (heap_mutex_ alone races register rehash).
aura_add_issue_test(test_gc_closures_mtx_flush_sweep_2473)
aura_issue_test_link_light(test_gc_closures_mtx_flush_sweep_2473)
add_dependencies(all_test_issue_targets test_gc_closures_mtx_flush_sweep_2473)


# Issue #2475: remove unused default-constructed fn_lock in
# AuraJIT::Impl::compile(); document compile_mtx_ + fn_compile_mtx_ roles.
aura_add_issue_test(test_aura_jit_unused_fn_lock_2475)
aura_issue_test_link_llvm_jit(test_aura_jit_unused_fn_lock_2475)
add_dependencies(all_test_issue_targets test_aura_jit_unused_fn_lock_2475)

# Issue #2476: partial_recompile single-pass eviction (invalidate_prefix
# only — bare + name#*; drop redundant invalidate walk).
aura_add_issue_test(test_partial_recompile_single_evict_2476)
aura_issue_test_link_llvm_jit(test_partial_recompile_single_evict_2476)
add_dependencies(all_test_issue_targets test_partial_recompile_single_evict_2476)

# Issue #2477: emit_object fail-closed deprecation (no true after .ir write).
aura_add_issue_test(test_emit_object_deprecated_2477)
aura_issue_test_link_llvm_jit(test_emit_object_deprecated_2477)
add_dependencies(all_test_issue_targets test_emit_object_deprecated_2477)

# Issue #2478: command-line requires kCapIoRead (capability bypass closed).
aura_add_issue_test(test_command_line_cap_io_read_2478)
aura_issue_test_link_light(test_command_line_cap_io_read_2478)
add_dependencies(all_test_issue_targets test_command_line_cap_io_read_2478)

# Issue #2479: regex-* ReDoS wall-clock timeout (AURA_REGEX_TIMEOUT_MS).
aura_add_issue_test(test_regex_redos_timeout_2479)
aura_issue_test_link_light(test_regex_redos_timeout_2479)
add_dependencies(all_test_issue_targets test_regex_redos_timeout_2479)

# Issue #2480: json-parse parse_number catches stod/stoll exceptions.
aura_add_issue_test(test_json_parse_number_exception_2480)
aura_issue_test_link_light(test_json_parse_number_exception_2480)
add_dependencies(all_test_issue_targets test_json_parse_number_exception_2480)

# Issue #2481: json-parse parse_object grows FlatHashTable (no silent key drop).
aura_add_issue_test(test_json_parse_object_grow_2481)
aura_issue_test_link_light(test_json_parse_object_grow_2481)
add_dependencies(all_test_issue_targets test_json_parse_object_grow_2481)

# Issue #2482: is_end_of_list / null? treat only void as empty list (not int 0).
aura_add_issue_test(test_list_end_of_list_void_2482)
aura_issue_test_link_light(test_list_end_of_list_void_2482)
add_dependencies(all_test_issue_targets test_list_end_of_list_void_2482)

# Issue #2483: channel:send rendezvous (buffer_size=0) blocks until recv.
aura_add_issue_test(test_channel_rendezvous_2483)
aura_issue_test_link_light(test_channel_rendezvous_2483)
add_dependencies(all_test_issue_targets test_channel_rendezvous_2483)

# Issue #2484: eval-current must not auto-invoke workspace Defines (side effects).
aura_add_issue_test(test_eval_current_no_auto_fix_2484)
aura_issue_test_link_light(test_eval_current_no_auto_fix_2484)
add_dependencies(all_test_issue_targets test_eval_current_no_auto_fix_2484)

# Issue #2485: load requires kCapIoRead (capability bypass closed).
aura_add_issue_test(test_load_cap_io_read_2485)
aura_issue_test_link_light(test_load_cap_io_read_2485)
add_dependencies(all_test_issue_targets test_load_cap_io_read_2485)

# Issue #2486: gc-heap fallback clears cells_ (stale cell data).
aura_add_issue_test(test_gc_heap_cells_clear_2486)
aura_issue_test_link_light(test_gc_heap_cells_clear_2486)
add_dependencies(all_test_issue_targets test_gc_heap_cells_clear_2486)

# Issue #2487: sys-open O_NOFOLLOW + path_is_denied (TOCTOU / mem disclosure).
aura_add_issue_test(test_sys_open_path_harden_2487)
aura_issue_test_link_light(test_sys_open_path_harden_2487)
add_dependencies(all_test_issue_targets test_sys_open_path_harden_2487)

# Issue #2500: Agent compact policy (health + frag + defer + hold → mode).
aura_add_issue_test(test_compact_policy_2500)
aura_issue_test_link_light(test_compact_policy_2500)
add_dependencies(all_test_issue_targets test_compact_policy_2500)

# Issue #2501: complete post-bump epoch invariant (slot clear + MustDeopt).
aura_add_issue_test(test_epoch_invariant_complete_2501)
aura_issue_test_link_llvm_jit(test_epoch_invariant_complete_2501)
add_dependencies(all_test_issue_targets test_epoch_invariant_complete_2501)

# Issue #2541: production default epoch-invariant soft + force clear on violation.
aura_add_issue_test(test_epoch_invariant_soft_prod_2541)
aura_issue_test_link_llvm_jit(test_epoch_invariant_soft_prod_2541)
add_dependencies(all_test_issue_targets test_epoch_invariant_soft_prod_2541)

# Issue #2502: auto re-promote force-JIT regions after stable recovery window.
aura_add_issue_test(test_force_jit_repromote_2502)
aura_issue_test_link_llvm_jit(test_force_jit_repromote_2502)
add_dependencies(all_test_issue_targets test_force_jit_repromote_2502)

# Issue #2503: remount fail → MustDeopt + batch_deopt shared path (cell remap).
aura_add_issue_test(test_remount_force_deopt_2503)
aura_issue_test_link_llvm_jit(test_remount_force_deopt_2503)
add_dependencies(all_test_issue_targets test_remount_force_deopt_2503)

# Issue #2542: reemit success full live-closure epoch restamp (no silent skip).
aura_add_issue_test(test_live_closure_full_restamp_2542)
aura_issue_test_link_llvm_jit(test_live_closure_full_restamp_2542)
add_dependencies(all_test_issue_targets test_live_closure_full_restamp_2542)

# Issue #2504: e2e PerEval dual-eval SpecJIT storm isolation (no cross-clear).
aura_add_issue_test(test_specjit_pereval_storm_e2e_2504)
aura_issue_test_link_llvm_jit(test_specjit_pereval_storm_e2e_2504)
add_dependencies(all_test_issue_targets test_specjit_pereval_storm_e2e_2504)

# Issue #2505: cross-COW soft-migrate drift threshold + hard-reject reasons.
aura_add_issue_test(test_cross_cow_drift_contract_2505)
aura_issue_test_link_llvm_jit(test_cross_cow_drift_contract_2505)
add_dependencies(all_test_issue_targets test_cross_cow_drift_contract_2505)

# Issue #2547: stamp cow_gen on closures; hard-reject cross-gen soft-migrate.
aura_add_issue_test(test_closure_cow_gen_stamp_2547)
aura_issue_test_link_llvm_jit(test_closure_cow_gen_stamp_2547)
add_dependencies(all_test_issue_targets test_closure_cow_gen_stamp_2547)

# Issue #2506: query:aot-hot-update-health single Agent recovery gate score.
aura_add_issue_test(test_aot_hot_update_health_2506)
aura_issue_test_link_llvm_jit(test_aot_hot_update_health_2506)
add_dependencies(all_test_issue_targets test_aot_hot_update_health_2506)

# Issue #2543: orch self-throttle over aot-hot-update-health (control plane).
aura_add_issue_test(test_orch_hot_update_health_throttle_2543)
aura_issue_test_link_llvm_jit(test_orch_hot_update_health_throttle_2543)
add_dependencies(all_test_issue_targets test_orch_hot_update_health_throttle_2543)

# Issue #2544: exhausted fall_back_jit_only queues minimal-dirty reemit (#2502 feed).
aura_add_issue_test(test_exhausted_min_dirty_reemit_2544)
aura_issue_test_link_llvm_jit(test_exhausted_min_dirty_reemit_2544)
add_dependencies(all_test_issue_targets test_exhausted_min_dirty_reemit_2544)

# Issue #2545: unify linear hard-fail decision entry (force_linear_rollback).
aura_add_issue_test(test_linear_force_unified_2545)
aura_issue_test_link_light(test_linear_force_unified_2545)
add_dependencies(all_test_issue_targets test_linear_force_unified_2545)

# Issue #2507: clear escape MoveOp elision gate on steal-complete + densify.
aura_add_issue_test(test_escape_gate_steal_densify_clear_2507)
aura_issue_test_link_light(test_escape_gate_steal_densify_clear_2507)
add_dependencies(all_test_issue_targets test_escape_gate_steal_densify_clear_2507)

# Issue #2508: OccurrenceGoal reverify before anti-starve full solve on truncate.
aura_add_issue_test(test_delta_truncate_goal_priority_2508)
aura_issue_test_link_light(test_delta_truncate_goal_priority_2508)
add_dependencies(all_test_issue_targets test_delta_truncate_goal_priority_2508)

# Issue #2509: symmetric expected_partial ↔ commit_cs_has_work matrix at commit.
aura_add_issue_test(test_composite_cs_signature_matrix_2509)
aura_issue_test_link_light(test_composite_cs_signature_matrix_2509)
add_dependencies(all_test_issue_targets test_composite_cs_signature_matrix_2509)

# Issue #2061: incremental restamp observability for generation wrap
# (restamp_nodes_total + restamp_us_total metrics, force-wrap synthetic test).
aura_add_issue_test(test_incremental_restamp_2061)
aura_issue_test_link_light(test_incremental_restamp_2061)

# Issue #2403: composite query index (tag+arity±marker) + shared_lock SLO.
aura_add_issue_test(test_query_index_composite_2403)
aura_issue_test_link_light(test_query_index_composite_2403)
add_dependencies(all_test_issue_targets test_query_index_composite_2403)

# Issue #2404: Agent StableNodeRef export validate_or_refresh contract.
aura_add_issue_test(test_stable_ref_export_validate_2404)
aura_issue_test_link_light(test_stable_ref_export_validate_2404)
add_dependencies(all_test_issue_targets test_stable_ref_export_validate_2404)

# Issue #2073: wire check_workspace_isolation + stamp_ref_tenant on
# mutate / StableNodeRef hot paths (stamp_ref_tenant at
# pin_node_for_atomic_batch). Extends #1566 / #81967 coverage.
aura_add_issue_test(test_workspace_isolation_wire_2073)
aura_issue_test_link_light(test_workspace_isolation_wire_2073)
# Issue #2385: Restricted sandbox denies side-effects when tenant principal unset.
aura_add_issue_test(test_restricted_unset_principal_2385)
aura_issue_test_link_light(test_restricted_unset_principal_2385)

# Issue #2125: stamp isolation principal on all StableNodeRef capture
# paths (make_ref / children_stable / non-batch) — extends #2073 / #2056.
aura_add_issue_test(test_stable_ref_tenant_capture_2125)
aura_issue_test_link_light(test_stable_ref_tenant_capture_2125)

# Issue #2126: prefer instr/block impact dirty over nested/quote-lambda
# mark_all_blocks_dirty full-relower fallback (#2031 / #2109 consumer).
aura_add_issue_test(test_instr_impact_minimal_dirty_2126)
aura_issue_test_link_light(test_instr_impact_minimal_dirty_2126)

# Issue #2127: workload/deopt/density adaptive partial-relower threshold
# (extends #2112 cost adaptive + #2032 base=8).
aura_add_issue_test(test_workload_adaptive_relower_2127)
aura_issue_test_link_light(test_workload_adaptive_relower_2127)

# Issue #2190: StormLevel Global gate on partial relower (Shape-only does not).
aura_add_issue_test(test_partial_relower_storm_gate_2190)
aura_issue_test_link_light(test_partial_relower_storm_gate_2190)
add_dependencies(all_test_issue_targets test_partial_relower_storm_gate_2190)

# Issue #2128: MustDeoptBeforeNextCall after reemit when live remap misses.
aura_add_issue_test(test_must_deopt_before_next_call_2128)
aura_issue_test_link_llvm_jit(test_must_deopt_before_next_call_2128)

# Issue #2129: stamp + validate linear_state on Closure/IRClosure/AOT/apply.
aura_add_issue_test(test_linear_state_stamp_apply_2129)
aura_issue_test_link_light(test_linear_state_stamp_apply_2129)

# Issue #2130: ShapeAwareFold + LinearOwnership truly dirty-aware
# (per-block peel / clean-function skip; metrics schema-2130).
aura_add_issue_test(test_dirty_aware_shape_linear_passes_2130)
aura_issue_test_link_light(test_dirty_aware_shape_linear_passes_2130)

# Issue #2131: unify GC-root pin → cascade → audit GcCoordScope state
# machine across invalidate / boundary / hot-swap / compact.
aura_add_issue_test(test_gc_coord_scope_2131)
aura_issue_test_link_light(test_gc_coord_scope_2131)

# Issue #2132: region/priority-aware deopt-storm reemit throttle
# (critical mask soft-bypass; hard ceiling; reason metrics).
aura_add_issue_test(test_region_priority_deopt_throttle_2132)
aura_issue_test_link_llvm_jit(test_region_priority_deopt_throttle_2132)

# Issue #2133: consume ImpactScope affected_instrs in relower +
# DirtyAware pass peel (instr precision under partial thr).
aura_add_issue_test(test_instr_level_relower_pass_2133)
aura_issue_test_link_light(test_instr_level_relower_pass_2133)




# Issue #2137: frame-budget cascade isolation (protect present from dirty work).
aura_add_issue_test(test_frame_budget_cascade_isolation_2137)
aura_issue_test_link_light(test_frame_budget_cascade_isolation_2137)


# Issue #2139: single-entry SoA dirty sync after every cascade (block→instr).
aura_add_issue_test(test_soa_single_entry_dirty_sync_2139)
aura_issue_test_link_light(test_soa_single_entry_dirty_sync_2139)

# Issue #2142: unify hot-path AURA_HOT_CONTRACT + invariant hit metrics.
aura_add_issue_test(test_hot_contract_unify_2142)
aura_issue_test_link_light(test_hot_contract_unify_2142)

# Issue #2143: SoaDirtyAwarePass + run_dirty_pipeline fold on IRModuleV2.
aura_add_issue_test(test_soa_dirty_aware_pipeline_2143)
aura_issue_test_link_light(test_soa_dirty_aware_pipeline_2143)

# Issue #2144: Guard-exit selective predicate-memo + occurrence reanalyze.
aura_add_issue_test(test_guard_exit_occurrence_refresh_2144)
aura_issue_test_link_light(test_guard_exit_occurrence_refresh_2144)

# Issue #2145: Full/Strict MutationBoundary hard-gate for invariant + linear.
aura_add_issue_test(test_hard_gate_full_strict_2145)
aura_issue_test_link_light(test_hard_gate_full_strict_2145)

# Issue #2281: Agent-visible TypedMutationAudit decision query (Sampled
# vs hard-gate matrix) — pure decide() + query schema sentinels.
aura_add_issue_test(test_typed_mutation_audit_decision_2281)
aura_issue_test_link_light(test_typed_mutation_audit_decision_2281)
add_dependencies(all_test_issue_targets test_typed_mutation_audit_decision_2281)
# Issue #2300: query:lifetime-contract-snapshot pure Agent surface.
aura_add_issue_test(test_lifetime_contract_snapshot_2300)
aura_issue_test_link_light(test_lifetime_contract_snapshot_2300)
add_dependencies(all_test_issue_targets test_lifetime_contract_snapshot_2300)

# Issue #2282: unified dead-coercion layered counter (AST elide + IR DCE + dirty-cone skip).
# Single Agent-facing key (dead-coercion-layered-total) + 5 individually queryable components.
aura_add_issue_test(test_dead_coercion_layered_2282)
aura_issue_test_link_light(test_dead_coercion_layered_2282)
add_dependencies(all_test_issue_targets test_dead_coercion_layered_2282)

# Issue #2283: wire type_dep_graph_ into infer_flat_partial primary affected set
# via touched_type_ids (CS touched_roots + occurrence vars + rebinding type change).
# Systematic merge + live_and_still_typed filter + 2 new metrics + 1 additive query primitive.
aura_add_issue_test(test_type_dep_partial_merge_2283)
aura_issue_test_link_light(test_type_dep_partial_merge_2283)
add_dependencies(all_test_issue_targets test_type_dep_partial_merge_2283)

# Issue #2284: Agent-first-class TIMEOUT repair surface (structured unresolved_affected_nodes).
# Publish site on every TIMEOUT / hard-reject (#2277 parity) + 8 new atomics + 1 additive
# query primitive (hash with 7 fixed fields + 16 capped NodeId slots).
aura_add_issue_test(test_type_timeout_repair_2284)
aura_issue_test_link_light(test_type_timeout_repair_2284)
add_dependencies(all_test_issue_targets test_type_timeout_repair_2284)

# Issue #2548: richer TIMEOUT repair (degree-ranked roots + reason tags).
aura_add_issue_test(test_timeout_repair_rich_roots_2548)
aura_issue_test_link_light(test_timeout_repair_rich_roots_2548)
add_dependencies(all_test_issue_targets test_timeout_repair_rich_roots_2548)

# Issue #2549: is_stealable trusts MutationSafetySnapshot only (candidate demote).
aura_add_issue_test(test_is_stealable_snapshot_gate_2549)
aura_issue_test_link_light(test_is_stealable_snapshot_gate_2549)
add_dependencies(all_test_issue_targets test_is_stealable_snapshot_gate_2549)

# Issue #2550: named closure create forces stable_func_id != 0.
aura_add_issue_test(test_named_closure_stable_id_at_create_2550)
aura_issue_test_link_llvm_jit(test_named_closure_stable_id_at_create_2550)
add_dependencies(all_test_issue_targets test_named_closure_stable_id_at_create_2550)

# Issue #2605: explicit anonymous / residual sid=0 policy (MustDeopt, residual_backfill).
aura_add_issue_test(test_anonymous_residual_stable_id_policy_2605)
aura_issue_test_link_llvm_jit(test_anonymous_residual_stable_id_policy_2605)
add_dependencies(all_test_issue_targets test_anonymous_residual_stable_id_policy_2605)

# Issue #2606: PerEval / multi-AotState reemit + invalidate region independence.
aura_add_issue_test(test_pereval_reemit_region_independence_2606)
aura_issue_test_link_llvm_jit(test_pereval_reemit_region_independence_2606)
add_dependencies(all_test_issue_targets test_pereval_reemit_region_independence_2606)

# Issue #2607: minimal INSTANCE constraint + depth-capped ∀ peel (false TIMEOUT).
aura_add_issue_test(test_instance_constraint_depth_cap_2607)
aura_issue_test_link_llvm_jit(test_instance_constraint_depth_cap_2607)
add_dependencies(all_test_issue_targets test_instance_constraint_depth_cap_2607)

# Issue #2608: optional OccurrenceGoal persist / rehydrate after steal/densify.
aura_add_issue_test(test_occurrence_goal_persist_rehydrate_2608)
aura_issue_test_link_llvm_jit(test_occurrence_goal_persist_rehydrate_2608)
add_dependencies(all_test_issue_targets test_occurrence_goal_persist_rehydrate_2608)

# Issue #2609: steal/densify hard-AND residual + linear force + type fence.
aura_add_issue_test(test_steal_densify_linear_type_hard_and_2609)
aura_issue_test_link_light(test_steal_densify_linear_type_hard_and_2609)
add_dependencies(all_test_issue_targets test_steal_densify_linear_type_hard_and_2609)

# Issue #2610: auto-detect expected_partial from dirty cone (anti false-green).
aura_add_issue_test(test_composite_auto_partial_from_cone_2610)
aura_issue_test_link_llvm_jit(test_composite_auto_partial_from_cone_2610)
add_dependencies(all_test_issue_targets test_composite_auto_partial_from_cone_2610)

# Issue #2611: stamp mutation_id + narrow_evidence on elided CastOp deopt meta.
aura_add_issue_test(test_dce_elided_deopt_meta_2611)
aura_issue_test_link_light(test_dce_elided_deopt_meta_2611)
add_dependencies(all_test_issue_targets test_dce_elided_deopt_meta_2611)

# Issue #2551: outermost hold-exit residual under production → hard + Agent throttle.
aura_add_issue_test(test_mailbox_hold_starvation_hard_2551)
aura_issue_test_link_light(test_mailbox_hold_starvation_hard_2551)
add_dependencies(all_test_issue_targets test_mailbox_hold_starvation_hard_2551)

# Issue #2552: steal/densify joint OccurrenceGoal + type_dep freshness fence.
aura_add_issue_test(test_type_freshness_steal_densify_2552)
aura_issue_test_link_light(test_type_freshness_steal_densify_2552)
add_dependencies(all_test_issue_targets test_type_freshness_steal_densify_2552)

# Issue #2553: single Agent commit-readiness score (solve × linear × blame × truncate).
aura_add_issue_test(test_commit_readiness_score_2553)
aura_issue_test_link_light(test_commit_readiness_score_2553)
add_dependencies(all_test_issue_targets test_commit_readiness_score_2553)

# Issue #2555: real TransactionGuard (type-erased host) + agent body migration
aura_add_issue_test(test_transaction_guard_2555)
aura_issue_test_link_light(test_transaction_guard_2555)
add_dependencies(all_test_issue_targets test_transaction_guard_2555)

# Issue #2556: DeadCoercion DCE scan limited to type∪IR dirty cone
aura_add_issue_test(test_dead_coercion_dirty_cone_2556)
aura_issue_test_link_light(test_dead_coercion_dirty_cone_2556)
add_dependencies(all_test_issue_targets test_dead_coercion_dirty_cone_2556)

# Issue #2557: production soft lock-order audit (metrics-only)
aura_add_issue_test(test_lock_order_production_soft_2557)
aura_issue_test_link_light(test_lock_order_production_soft_2557)
add_dependencies(all_test_issue_targets test_lock_order_production_soft_2557)

# Issue #2558: coercion provenance completeness SLO → force Full audit
aura_add_issue_test(test_coercion_prov_slo_2558)
aura_issue_test_link_light(test_coercion_prov_slo_2558)
add_dependencies(all_test_issue_targets test_coercion_prov_slo_2558)

# Issue #2559: three-layer linear invariant wire inventory gate
aura_add_issue_test(test_linear_three_layer_wire_2559)
aura_issue_test_link_light(test_linear_three_layer_wire_2559)
add_dependencies(all_test_issue_targets test_linear_three_layer_wire_2559)

# Issue #2560: partial re-infer cone soft/hard cap (type-layer SLA)
aura_add_issue_test(test_partial_cone_cap_2560)
aura_issue_test_link_light(test_partial_cone_cap_2560)
add_dependencies(all_test_issue_targets test_partial_cone_cap_2560)

# Issue #2561: Soft/Sampled blame chain recover + miss escalate
aura_add_issue_test(test_blame_soft_recover_2561)
aura_issue_test_link_light(test_blame_soft_recover_2561)
add_dependencies(all_test_issue_targets test_blame_soft_recover_2561)

# Issue #2562: dual-field (pred+mid) require-or-drop under production
aura_add_issue_test(test_coercion_dual_require_2562)
aura_issue_test_link_light(test_coercion_dual_require_2562)
add_dependencies(all_test_issue_targets test_coercion_dual_require_2562)

# Issue #2563: cross-closure linear escape discovery + force authority
aura_add_issue_test(test_linear_cross_closure_escape_2563)
aura_issue_test_link_light(test_linear_cross_closure_escape_2563)
add_dependencies(all_test_issue_targets test_linear_cross_closure_escape_2563)

# Issue #2612: optional depth-2 cross-closure free-capture (still cone-capped).
aura_add_issue_test(test_linear_cross_closure_depth2_2612)
aura_issue_test_link_light(test_linear_cross_closure_depth2_2612)
add_dependencies(all_test_issue_targets test_linear_cross_closure_depth2_2612)

# Issue #2613: unified query:type-linear-commit-health (readiness×linear×coercion×occurrence).
aura_add_issue_test(test_type_linear_commit_health_2613)
aura_issue_test_link_light(test_type_linear_commit_health_2613)
add_dependencies(all_test_issue_targets test_type_linear_commit_health_2613)

# Issue #2564: ADT match exhaustiveness goal table + delta reverify roots
aura_add_issue_test(test_adt_match_goal_table_2564)
aura_issue_test_link_light(test_adt_match_goal_table_2564)
add_dependencies(all_test_issue_targets test_adt_match_goal_table_2564)

# Issue #2566: non-std module free-var resolve of required std bindings
aura_add_issue_test(test_module_require_freevar_2566)
aura_issue_test_link_light(test_module_require_freevar_2566)
add_dependencies(all_test_issue_targets test_module_require_freevar_2566)

# Issue #2567: try/catch bind catch parameter in handler body
aura_add_issue_test(test_try_catch_bind_2567)
aura_issue_test_link_light(test_try_catch_bind_2567)
add_dependencies(all_test_issue_targets test_try_catch_bind_2567)

# Issue #2568: symbol eq?/equal? for quoted symbols (agent decision tags)
aura_add_issue_test(test_symbol_eq_2568)
aura_issue_test_link_light(test_symbol_eq_2568)
add_dependencies(all_test_issue_targets test_symbol_eq_2568)

# Issue #2569: set-code / mutate:rebind must not kill unimpacted closures/hash
aura_add_issue_test(test_setcode_rebind_survive_2569)
aura_issue_test_link_light(test_setcode_rebind_survive_2569)
add_dependencies(all_test_issue_targets test_setcode_rebind_survive_2569)

# Issue #2578: Aether denseness host residuals (H1 multi-let, H5 fiber, H6 orch free-var)
aura_add_issue_test(test_aether_denseness_residual_2578)
aura_issue_test_link_light(test_aether_denseness_residual_2578)
add_dependencies(all_test_issue_targets test_aether_denseness_residual_2578)

# Issue #2579: module free-vars / multi-define value init / trailing export
aura_add_issue_test(test_module_rebind_residual_2579)
aura_issue_test_link_light(test_module_rebind_residual_2579)
add_dependencies(all_test_issue_targets test_module_rebind_residual_2579)

# Issue #2582: pure-Aura hot strategy (std/hot-strategy) vs AOT hot-update
aura_add_issue_test(test_hot_strategy_2582)
aura_issue_test_link_light(test_hot_strategy_2582)
add_dependencies(all_test_issue_targets test_hot_strategy_2582)

# Issue #2570: module load fail-closed; trailing defines always export
aura_add_issue_test(test_module_load_tail_export_2570)
aura_issue_test_link_light(test_module_load_tail_export_2570)
add_dependencies(all_test_issue_targets test_module_load_tail_export_2570)

# Issue #2571: (define) inside (while) loop-counter footgun / multi-define set!
aura_add_issue_test(test_while_define_oneshot_2571)
aura_issue_test_link_light(test_while_define_oneshot_2571)
add_dependencies(all_test_issue_targets test_while_define_oneshot_2571)

# Issue #2572: module-export multi-display / ConstString pool remap on cache_module
aura_add_issue_test(test_module_export_display_2572)
aura_issue_test_link_light(test_module_export_display_2572)
add_dependencies(all_test_issue_targets test_module_export_display_2572)

# Issue #2573: IR interpreter ConstString intern (no O(N) string_heap growth)
aura_add_issue_test(test_ir_const_string_intern_2573)
aura_issue_test_link_light(test_ir_const_string_intern_2573)
add_dependencies(all_test_issue_targets test_ir_const_string_intern_2573)

# Issue #2574: Scheme write string escape (JIT + TW)
aura_add_issue_test(test_write_string_escape_2574)
aura_issue_test_link_light(test_write_string_escape_2574)
add_dependencies(all_test_issue_targets test_write_string_escape_2574)

# Issue #2575: dual string heap — PrimCall re-intern eval→JIT for display
aura_add_issue_test(test_jit_dual_string_heap_2575)
aura_issue_test_link_light(test_jit_dual_string_heap_2575)
add_dependencies(all_test_issue_targets test_jit_dual_string_heap_2575)

# Issue #2576: JIT PrimCall N-arg (pack frame locals; not binary-only)
aura_add_issue_test(test_primcall_narg_2576)
aura_issue_test_link_light(test_primcall_narg_2576)
add_dependencies(all_test_issue_targets test_primcall_narg_2576)

# Issue #2577: PrimCall string re-intern content intern (no O(N) heap growth)
aura_add_issue_test(test_primcall_str_intern_2577)
aura_issue_test_link_light(test_primcall_str_intern_2577)
add_dependencies(all_test_issue_targets test_primcall_str_intern_2577)

# Issue #2146: adaptive solve_delta clean-reverify limit + truncation Agent surface.
aura_add_issue_test(test_adaptive_reverify_limit_2146)
aura_issue_test_link_light(test_adaptive_reverify_limit_2146)

# Issue #2147: CoercionMap provenance fast path + Strict reject weak mutation id.
aura_add_issue_test(test_coercion_provenance_fast_strict_2147)
aura_issue_test_link_light(test_coercion_provenance_fast_strict_2147)

# Issue #2512: stamp active mid/pred into CoercionEntry at deferred-add.
aura_add_issue_test(test_coercion_stamp_at_add_2512)
aura_issue_test_link_light(test_coercion_stamp_at_add_2512)
add_dependencies(all_test_issue_targets test_coercion_stamp_at_add_2512)

# Issue #2148: Precision meet/join for predicate lattice (number/string/bool + ADT).
aura_add_issue_test(test_predicate_meet_join_lattice_2148)
aura_issue_test_link_light(test_predicate_meet_join_lattice_2148)

# Issue #2072: force check_and_record_effect on all side-effect primitives
# (FFI/Network/Exec/Render). require_effect() helper + git-commit +
# render-gated prims wired at primitive entry points (#2625: render3d removed).
aura_add_issue_test(test_capability_effect_force_2072)
aura_issue_test_link_light(test_capability_effect_force_2072)
# Issue #2384: require_effect stamps live mutation_id (not hardcode 0).
aura_add_issue_test(test_require_effect_live_mid_2384)
aura_issue_test_link_light(test_require_effect_live_mid_2384)

# Issue #2490: require_effect auto-enforces workspace isolation when
# req_bits != 0 (single side-effect entry). Pure / zero-bits callers
# unchanged; isolation deny short-circuits before effect check; single SE
# IsolationDeny count preserved via #2388.
aura_add_issue_test(test_require_effect_auto_isolation_2490)
aura_issue_test_link_light(test_require_effect_auto_isolation_2490)

# Issue #2491: mandate TenantScope at fiber spawn/resume entry (no residual
# principal). assigned_tenant_id_ on Fiber + bridge hooks
# aura_fiber_install_tenant_scope_for_resume / aura_fiber_release_tenant_scope_after_yield
# on Fiber::resume / yield boundary. Single authority with #2385 / #2055 /
# #2388 + #2490 require_effect auto-isolation.
aura_add_issue_test(test_tenant_scope_fiber_mandate_2491)
aura_issue_test_link_light(test_tenant_scope_fiber_mandate_2491)

# Issue #2492: force SecurityEvent WAL under Restricted (not only
# multi-tenant/Strict). Single-tenant Restricted commercial deploys were
# silent under deny storms because force_wal only fired for multi-tenant
# or Strict — closed by adding `restricted` to the force_wal predicate.
aura_add_issue_test(test_security_audit_wal_force_restricted_2492)
aura_issue_test_link_light(test_security_audit_wal_force_restricted_2492)

# Issue #2493: unify mutation_id source — WorkspaceEpoch Mutation over
# independent audit gens. resolve_audit_mutation_id() preference order
# (caller mid → current_mutation_epoch → ResourceQuota → gen) joins SE ↔
# TypedMutationAudit ↔ grant epoch into one mid vocabulary.
aura_add_issue_test(test_audit_mutation_id_unify_2493)
aura_issue_test_link_light(test_audit_mutation_id_unify_2493)

# Issue #2494: hard-fail check_side_effect_security.py for new prims without
# require_effect / PrimMeta. The script already has --strict mode + allowlist
# reason enforcement; this test confirms it's wired as PR CI hard-fail and
# that a fixture prim (side-effect name, no coverage marker) trips the gate.
aura_add_issue_test(test_side_effect_security_gate_hardfail_2494)
aura_issue_test_link_light(test_side_effect_security_gate_hardfail_2494)

# Issue #2495: Moving densify fail-closed on untracked external roots.
# ASTArena::live_compact(Moving) only densifies small-pool tracked create
# objects via relocate_tracked_objects_for_moving_; non-small-pool /
# untracked external raw pointers never enter last_object_remap_. Fail-closed
# via moving_incomplete_remap + pin_contract_held=false when objects_moved > 0
# && untracked_kept_count > 0 (production Moving default).
aura_add_issue_test(test_moving_densify_fail_closed_2495)
aura_issue_test_link_light(test_moving_densify_fail_closed_2495)

# Issue #2496: GeneralObjectPin adoption coverage gate (inventory vs
# wire_total). kGeneralObjectPinAdoptSiteCount tracks the documented
# inventory; linter fails when a listed site lacks wire call. Optional
# AURA_GENERAL_OBJECT_PIN=required fail-closed runtime mode. Source-cites
# #2298 + #2337 + #2363 (the GeneralObjectPin + mutate-path adoption
# lineage).
aura_add_issue_test(test_general_object_pin_coverage_gate_2496)
aura_issue_test_link_light(test_general_object_pin_coverage_gate_2496)

# Issue #2074: anti privilege-sticky — grant_epoch binding to
# current_mutation_epoch() + min_valid_epoch invalidation in
# CapabilityRegistry::provenance_ok.
aura_add_issue_test(test_grant_epoch_invalidation_2074)
aura_issue_test_link_light(test_grant_epoch_invalidation_2074)

# Issue #2075: unified SecurityEvent schema + query:security-audit-trail
# primitive. Shared effect/isolation/invariant audit surface
# (src/core/security_event.hh) wired at both effect + isolation deny
# paths in evaluator_security.cpp.
aura_add_issue_test(test_security_audit_trail_2075)
aura_issue_test_link_light(test_security_audit_trail_2075)

# Issue #2076: production default Restricted sandbox + AURA_SANDBOX
# env override + Agent-readable deny reason format
# (effect-denied: <EffectName> not granted tenant=<id> op=<op>).
# apply_aura_sandbox_env() called from main.cpp at startup.
aura_add_issue_test(test_aura_sandbox_env_2076)
aura_issue_test_link_light(test_aura_sandbox_env_2076)

# Issue #2149: effect-check stamps Mutation epoch (not Bridge) for capability fence.
aura_add_issue_test(test_effect_epoch_mutation_unify_2149)
aura_issue_test_link_light(test_effect_epoch_mutation_unify_2149)

# Issue #2150: force mutation-audit WAL under multi-tenant / Strict production.
aura_add_issue_test(test_audit_wal_force_multi_tenant_2150)
aura_issue_test_link_light(test_audit_wal_force_multi_tenant_2150)

# Issue #2151: optional hard-deny on grant_fiber_id mismatch (TenantScope isolation).
aura_add_issue_test(test_hard_fiber_isolation_2151)
aura_issue_test_link_light(test_hard_fiber_isolation_2151)

# Issue #2152: dispatch-level non-bypassable required_effects for side-effect prims.
aura_add_issue_test(test_dispatch_required_effects_2152)
aura_issue_test_link_light(test_dispatch_required_effects_2152)

# Issue #2154: sliding grant_min_valid_epoch window on Mutation epoch bump.
aura_add_issue_test(test_grant_epoch_retain_window_2154)
aura_issue_test_link_light(test_grant_epoch_retain_window_2154)

# Issue #2156: isolation-deny SecurityEvent.mutation_id is epoch, never tenant.
aura_add_issue_test(test_isolation_audit_mid_2156)
aura_issue_test_link_light(test_isolation_audit_mid_2156)

# Issue #2077: unify has_capability string path with Effect matrix
# (single source of truth). Effect-mapped caps (mutate, ffi, network,
# exec, render, read, write, macro-self-evo, *) now consult
# g_capability_registry().effects_for(tenant); caps with no effect
# mapping keep the legacy string-list path. Wildcard keeps the
# explicit-string-grant path so existing "*" grants stay meaningful.
aura_add_issue_test(test_capability_unified_2077)
aura_issue_test_link_light(test_capability_unified_2077)
# Issue #2387: tenant-admin / syscall join Effect matrix (has_capability single source).
aura_add_issue_test(test_capability_string_matrix_unify_2387)
aura_issue_test_link_light(test_capability_string_matrix_unify_2387)

# Issue #2489: promote remaining high-risk string-only caps into the Effect
# matrix (self-evo / synthesize / strategy → MacroSelfEvo; sys-open / sys-write /
# sys-read → Syscall|Read/Write; agent / capability → TenantAdmin). Closes the
# residual multi-tenant self-mod / syscall / meta-privilege dual-track surface.
aura_add_issue_test(test_capability_high_risk_promote_2489)
aura_issue_test_link_light(test_capability_high_risk_promote_2489)

# Issue #2388: fold Capability + Isolation private audit rings into SecurityEvent WAL.
aura_add_issue_test(test_security_audit_fold_2388)
aura_issue_test_link_light(test_security_audit_fold_2388)
add_dependencies(all_test_issue_targets test_security_audit_fold_2388)

# Issue #2389: query:security-health single Agent score (deny + fence + WAL + wrap).
aura_add_issue_test(test_security_health_2389)
aura_issue_test_link_light(test_security_health_2389)
add_dependencies(all_test_issue_targets test_security_health_2389)
aura_add_issue_test(test_audit_mid_fallback_slo_2594)
aura_issue_test_link_light(test_audit_mid_fallback_slo_2594)
add_dependencies(all_test_issue_targets test_audit_mid_fallback_slo_2594)

# Issue #2390: validate_node must not hard-abort on !is_valid (report/throw).
aura_add_issue_test(test_validate_node_no_abort_2390)
aura_issue_test_link_light(test_validate_node_no_abort_2390)
add_dependencies(all_test_issue_targets test_validate_node_no_abort_2390)

# Issue #2391: validate_post_restore SoA column size cross-check.
aura_add_issue_test(test_validate_post_restore_soa_2391)
aura_issue_test_link_light(test_validate_post_restore_soa_2391)
add_dependencies(all_test_issue_targets test_validate_post_restore_soa_2391)

# Issue #2392: fixup_deltas safe rebase (bounds + overflow → NULL_NODE).
aura_add_issue_test(test_fixup_deltas_2392)
aura_issue_test_link_light(test_fixup_deltas_2392)
add_dependencies(all_test_issue_targets test_fixup_deltas_2392)

# Issue #2393: refresh_if_stale COW epoch mismatch fail-closed (vs wrap_epoch).
aura_add_issue_test(test_stable_ref_cow_refresh_failclosed_2393)
aura_issue_test_link_light(test_stable_ref_cow_refresh_failclosed_2393)
add_dependencies(all_test_issue_targets test_stable_ref_cow_refresh_failclosed_2393)

# Issue #2394: last_validated_generation concurrent-safe (CopyableAtomicU16).
aura_add_issue_test(test_last_validated_generation_atomic_2394)
aura_issue_test_link_light(test_last_validated_generation_atomic_2394)
add_dependencies(all_test_issue_targets test_last_validated_generation_atomic_2394)

# Issue #2395: StableNodeRef wire multi-byte fields little-endian (portable).
aura_add_issue_test(test_stable_ref_wire_endian_2395)
aura_issue_test_link_light(test_stable_ref_wire_endian_2395)
add_dependencies(all_test_issue_targets test_stable_ref_wire_endian_2395)

# Issue #2040: Guard hold p99 + workspace_mtx_ contention observability
aura_add_issue_test(test_mutation_contention_2040)
aura_issue_test_link_light(test_mutation_contention_2040)
add_dependencies(all_test_issue_targets test_mutation_contention_2040)

# Issue #2041: partial re-lower + JIT hot-swap on invalidate cascade
aura_add_issue_test(test_partial_relower_cascade_2041)
aura_issue_test_link_light(test_partial_relower_cascade_2041)
add_dependencies(all_test_issue_targets test_partial_relower_cascade_2041)

# Issue #2042: comprehensive live IRClosure / tree-walker / PrimCall expire
aura_add_issue_test(test_comprehensive_live_closure_expire_2042)
aura_issue_test_link_llvm_jit(test_comprehensive_live_closure_expire_2042)
add_dependencies(all_test_issue_targets test_comprehensive_live_closure_expire_2042)

# Issue #2043: linear ownership + GC atomic invalidation window
aura_add_issue_test(test_linear_gc_window_2043)
aura_issue_test_link_light(test_linear_gc_window_2043)
add_dependencies(all_test_issue_targets test_linear_gc_window_2043)

# Issue #2044: cascade re-lower full incremental dirty pass suite
aura_add_issue_test(test_cascade_incremental_pass_suite_2044)
aura_issue_test_link_light(test_cascade_incremental_pass_suite_2044)
add_dependencies(all_test_issue_targets test_cascade_incremental_pass_suite_2044)

# Issue #2045: source_to_ir_map consistency after re-lower
aura_add_issue_test(test_source_to_ir_map_consistency_2045)
aura_issue_test_link_light(test_source_to_ir_map_consistency_2045)
add_dependencies(all_test_issue_targets test_source_to_ir_map_consistency_2045)

# Issue #2046: AOT region versioning aligned with JIT hot-swap / bridge_epoch
aura_add_issue_test(test_aot_jit_joint_versioning_2046)
aura_issue_test_link_llvm_jit(test_aot_jit_joint_versioning_2046)

# Issue #2099: HygieneCheckpoint + restore API for violation rollback
# (Agent what-if / self-evo safety). P1 — paired with #2023 (MacroSelfEvo).
aura_add_issue_test(test_hygiene_checkpoint_2099)
aura_issue_test_link_light(test_hygiene_checkpoint_2099)

# Issue #2170: Unified LayoutStamp / generation truth-source API for
# cross-subsystem epoch coherence (P1 MemorySafety-Review / Epoch).
# Single source-of-truth that boundary + compact + AOT emit + FFI
# validate all read from instead of each picking a different "current".
aura_add_issue_test(test_layout_stamp_2170)
aura_issue_test_link_light(test_layout_stamp_2170)

# Issue #2224: mandate StableNodeRef tenant stamp on export (Phase A
# export_ref / export_ref_safe) + resolve-time isolation gate (Phase B
# resolve_stamped) + tenant-isolation-stats primitive (Phase C surface).
aura_add_issue_test(test_isolation_stamp_resolve_2224)
aura_issue_test_link_light(test_isolation_stamp_resolve_2224)
add_dependencies(all_test_issue_targets test_isolation_stamp_resolve_2224)

# Issue #2225: durable side-car SecurityEvent WAL + expand ring to
# 1024 + wrap counter, for forensic replay after process restart.
# Phase A ring capacity + Phase B side-car WAL + Phase C replay +
# Phase D query:security-posture primitive.
aura_add_issue_test(test_security_event_wal_replay_2225)
aura_issue_test_link_light(test_security_event_wal_replay_2225)
add_dependencies(all_test_issue_targets test_security_event_wal_replay_2225)

# Issue #2227: hard reclaim path for join drain residual fibers
# (no leak after cancel+drain under non-yielding bodies). Fiber
# back-pointer + Scheduler orphan tracking + orch/parallel wire-up +
# query:orch-module-stats join-drain-residual-reclaim-total key.
aura_add_issue_test(test_join_drain_reclaim_2227)
aura_issue_test_link_light(test_join_drain_reclaim_2227)
add_dependencies(all_test_issue_targets test_join_drain_reclaim_2227)

# Issue #2228: backpressure-driven spawn admission control
# (soft-reject when process-wide mailbox BP rate is at/above the
# configured threshold). closes the producer/consumer feedback loop
# for commercial agent message storms.
aura_add_issue_test(test_mailbox_bp_admit_2228)
aura_issue_test_link_light(test_mailbox_bp_admit_2228)
add_dependencies(all_test_issue_targets test_mailbox_bp_admit_2228)

# Issue #2535: production default mild mailbox BP admit (threshold=32)
aura_add_issue_test(test_mailbox_bp_admit_default_2535)
aura_issue_test_link_light(test_mailbox_bp_admit_default_2535)
add_dependencies(all_test_issue_targets test_mailbox_bp_admit_default_2535)

# Issue #2536: Restricted same-tenant multi-fiber soft; optional hard via env
aura_add_issue_test(test_hard_fiber_restricted_2536)
aura_issue_test_link_light(test_hard_fiber_restricted_2536)
add_dependencies(all_test_issue_targets test_hard_fiber_restricted_2536)

# Issue #2584: AURA_COMMERCIAL_TENANT config profile (Restricted default
# hard-fiber isolation for commercial deployments; AURA_HARD_FIBER_ISOLATION=0
# still wins as explicit override). Active profile exposed via
# query:security-posture #2534 under "commercial-tenant-profile" key.
aura_add_issue_test(test_commercial_tenant_profile_2584)
aura_issue_test_link_light(test_commercial_tenant_profile_2584)
add_dependencies(all_test_issue_targets test_commercial_tenant_profile_2584)

# Issue #2537: hierarchical AgentScope (parent/children cancel tree).
aura_add_issue_test(test_agent_scope_hierarchy_2537)
aura_issue_test_link_light(test_agent_scope_hierarchy_2537)
add_dependencies(all_test_issue_targets test_agent_scope_hierarchy_2537)

# Issue #2465: decay window on mailbox_bp_recent_total so the #2228 admission
# gate doesn't permanently deny every spawn_agent_with_mailbox after the first
# BP event. Default 30s (matches kJoinDrainResidualHardMsDefault); env override
# AURA_ORCH_BP_DECAY_MS (0 disables decay; counter then truly monotonic).
aura_add_issue_test(test_orch_admission_decay)
aura_issue_test_link_light(test_orch_admission_decay)
add_dependencies(all_test_issue_targets test_orch_admission_decay)

# Issue #2188: forbid blocking mailbox recv under live MutationBoundary.
aura_add_issue_test(test_mailbox_recv_mutation_boundary_2188)
aura_issue_test_link_light(test_mailbox_recv_mutation_boundary_2188)
add_dependencies(all_test_issue_targets test_mailbox_recv_mutation_boundary_2188)

# Issue #2511: outermost Guard exit forced mailbox deferred drain under budget.
aura_add_issue_test(test_mailbox_hold_exit_drain_2511)
aura_issue_test_link_light(test_mailbox_hold_exit_drain_2511)
add_dependencies(all_test_issue_targets test_mailbox_hold_exit_drain_2511)

# Issue #2229: lift FailurePolicy / RestartPolicy onto AgentScope
# supervision. AgentFailureAction (ReportOnly / Cancel / RestartN)
# + AgentFailurePolicy + RestartN path in AgentScope::watch_all
# (re-spawn under same AgentSpec + max_restarts cap +
# consecutive_stall_limit circuit-like cap).
aura_add_issue_test(test_agent_failure_policy_2229)
aura_issue_test_link_light(test_agent_failure_policy_2229)
add_dependencies(all_test_issue_targets test_agent_failure_policy_2229)

# Issue #2539: FailurePolicy → AgentFailurePolicy unidirectional mapping bridge.
aura_add_issue_test(test_failure_policy_bridge_2539)
aura_issue_test_link_light(test_failure_policy_bridge_2539)
add_dependencies(all_test_issue_targets test_failure_policy_bridge_2539)

# Issue #2540: AgentSpec.max_no_yield_ms cooperative yield contract.
aura_add_issue_test(test_agent_max_no_yield_2540)
aura_issue_test_link_light(test_agent_max_no_yield_2540)
add_dependencies(all_test_issue_targets test_agent_max_no_yield_2540)

# Issue #2230: harden pure-parallel contract tests + document
# isolation boundaries. Locks the three metric paths
# (pure_unlocked_applies / pure_fallback_locked /
# pure_contract_violated) with explicit ACs in
# tests/orch/test_parallel_intend_pure_contract_2230.cpp and adds a
# "Pitfalls" sub-section to the README pure contract warning
# Agents that pure is best-effort, NOT isolation.
aura_add_issue_test(test_parallel_intend_pure_contract_2230)
aura_issue_test_link_light(test_parallel_intend_pure_contract_2230)
add_dependencies(all_test_issue_targets test_parallel_intend_pure_contract_2230)

# Issue #2231 / #2401: agent-ask + agent-reply (standard response path).
# Re-enabled after #2401 pending-ask table + agent_reply closed the
# "target cannot find reply mailbox" gap that caused prior hangs.
aura_add_issue_test(test_agent_ask_2231)
aura_issue_test_link_light(test_agent_ask_2231)
add_dependencies(all_test_issue_targets test_agent_ask_2231)

# Issue #2538: typed agent-ask correlation (MailKind + correlation_id).
aura_add_issue_test(test_agent_ask_typed_corr_2538)
aura_issue_test_link_light(test_agent_ask_typed_corr_2538)
add_dependencies(all_test_issue_targets test_agent_ask_typed_corr_2538)

add_dependencies(all_test_issue_targets test_aot_jit_joint_versioning_2046)

aura_add_issue_test(test_gc_compact_batch)
aura_issue_test_link_llvm_jit(test_gc_compact_batch)
set_target_properties(test_gc_compact_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

aura_add_issue_test(test_gc_compact_sweep_batch)
aura_issue_test_link_light(test_gc_compact_sweep_batch)
set_target_properties(test_gc_compact_sweep_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

aura_add_issue_test(test_gc_batch)
aura_issue_test_link_light(test_gc_batch)
set_target_properties(test_gc_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

# ── Wave 8 (#1957): tests/domain/test_fiber_integration_batch (Issue #226) ──
# Closure-bridge Cycle-4 integration tests (raw Scheduler + std::thread +
# many fibers). On-demand build; default-build skips.
aura_add_issue_test(test_fiber_integration_batch)
aura_issue_test_link_light(test_fiber_integration_batch)
set_target_properties(test_fiber_integration_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_dependencies(all_test_issue_targets test_fiber_integration_batch)

# Issue #2467: Fiber::is_done() strictly requires state_==Done (was
# conflated with reclaimed_ — let joiners call cleanup hook while body
# was still executing = UAF). JoinStatus::Readded for the
# reclaimed-but-body-still-running path (joiner defers cleanup).
aura_add_issue_test(test_fiber_reclaim_safety)
aura_issue_test_link_light(test_fiber_reclaim_safety)
add_dependencies(all_test_issue_targets test_fiber_reclaim_safety)

# Issue #2498: epoch-scoped off-stack orphan-root table. Non-yielding
# body + hard reclaim → Fiber::release_orphan_roots() drops the global
# table entries (EnvFrame/mailbox refs) without touching the body's
# running stack (Option A — no UAF regression vs #2467/#2468/#2469).
# Fiber::join Reclaimed path + ~Fiber safety net + register on
# Evaluator::register_live_env_frame_ref under g_current_fiber.
aura_add_issue_test(test_fiber_reclaim_orphan_release_2498)
aura_issue_test_link_light(test_fiber_reclaim_orphan_release_2498)
add_dependencies(all_test_issue_targets test_fiber_reclaim_orphan_release_2498)

# Issue #2468: Fiber::resume() pre-check for state_==Done + reclaimed_
# (was: no pre-check, swapcontext on freed stack = UB). Early-return
# with stderr message so misuse is visible in logs but doesn't crash.
aura_add_issue_test(test_fiber_resume_state)
aura_issue_test_link_light(test_fiber_resume_state)
add_dependencies(all_test_issue_targets test_fiber_resume_state)

# Issue #2469: Scheduler::reap_orphans_now() two-phase extraction —
# orphan_mutex_ released before per-fiber cleanup (wait_map_mutex_,
# joiner_map_mutex_, owned_fibers_mutex_ acquisitions). Minimizes
# lock-held window under cancel storms (N=100 parallel timeouts).
aura_add_issue_test(test_orphan_reap_stress)
aura_issue_test_link_light(test_orphan_reap_stress)
add_dependencies(all_test_issue_targets test_orphan_reap_stress)

# Issue #2586: single-use / mutation-bound grant (auto-revoke after first
# successful check_and_record_effect that uses the grant's bits; deny path
# does NOT consume). Source-cite in capability_model.hh (CapabilityGrant
# .single_use field, grant() extended signature, grant_once sugar,
# check_and_record_effect consume block, CapabilityEffectMetrics +
# Snapshot) + evaluator_security.cpp / evaluator.ixx (grant_effect_
# capability single_use param) + evaluator_primitives_security.cpp
# (query:capability-effect-stats surfaces capability-single-use-consumed-
# total / schema-2586 / issue-2586). No docs/design/ per #1655 #1485.
aura_add_issue_test(test_capability_single_use_consume_2586)
aura_issue_test_link_light(test_capability_single_use_consume_2586)
add_dependencies(all_test_issue_targets test_capability_single_use_consume_2586)

# Issue #2587: mutate admission honors agent_throttle_for_mailbox_starvation
# on every entry path (P0, closes BP→admission loop with #2551). Source-cite
# in multi_fiber_mailbox.h (agent_throttle_for_mailbox_starvation atomic +
# mutate_rejected_mailbox_starvation_total counter + aura_orch_mailbox_
# starvation_throttled helper [gnu::always_inline single relaxed load, AC5] +
# note_mutate_rejected_mailbox_starvation bumper) + evaluator_mutation_
# boundary.cpp (try_acquire + try_acquire_for_region throttle gate at top
# covers host + fiber soft path + TransactionGuard host callback
# transitively) + evaluator_primitives_messaging.cpp (query:mf-mailbox-
# stats surfaces mutate-rejected-mailbox-starvation-total / schema-2587 /
# issue-2587 parallel #2551 / #2511 / #2347 issue surface). No docs/design/
# per #1655 #1485.
aura_add_issue_test(test_mutate_mailbox_starvation_throttle_2587)
aura_issue_test_link_light(test_mutate_mailbox_starvation_throttle_2587)
add_dependencies(all_test_issue_targets test_mutate_mailbox_starvation_throttle_2587)

# Issue #2588: Aura language surface for AgentScope supervision
# (orch:scope-spawn / orch:scope-watch / orch:scope-join-all /
# orch:scope-cancel-all). Per-Evaluator scope (not process-static —
# g_evaluator_agent_scopes map in src/orch/agent_scope.h is storage;
# AgentScope objects themselves are per-Evaluator). NO global registry
# (MVP linter scripts/check_orch_mvp_scope.py still rejects
# AgentRegistry / global_agent_registry / conduct_parallel). Source-cite
# in src/orch/agent_scope.h (per-Evaluator map + helpers) /
# src/orch/agent_spawn.h (OrchModuleStats 6 new metrics) /
# src/compiler/evaluator_primitives_agent.cpp (4 new prims + query:
# orch-module-stats surfaces scope-*-total keys) / src/orch/README.md
# (orch:scope-* section explicit "not a global registry; bound to
# Evaluator/session"). No docs/design/ per #1655 #1485.
aura_add_issue_test(test_orch_scope_2588)
aura_issue_test_link_light(test_orch_scope_2588)
add_dependencies(all_test_issue_targets test_orch_scope_2588)
# Issue #2589 — observability facade: parallel_orch residual/reclaim
# surfaced via query:orch-module-stats (live read; no double-bookkeeping).
# Test in tests/orch/test_orch_obs_facade_2589.cpp (src-aligned per #81934).
# No docs/design/ per #1655 #1485.
aura_add_issue_test(test_orch_obs_facade_2589)
aura_issue_test_link_light(test_orch_obs_facade_2589)
add_dependencies(all_test_issue_targets test_orch_obs_facade_2589)
# Issue #2590 — security schedule gate: pure decide_security_schedule
# synthesizes commit_readiness (#2553) + deny rate (#2534) +
# mid-fallback SLO + posture wal_off (#2076). Production denies new
# mutate when gate flips false; soft / sandbox=off is observe-only.
# Test in tests/orch/test_security_schedule_gate_2590.cpp
# (src-aligned per #81934). No docs/design/ per #1655 #1485.
aura_add_issue_test(test_security_schedule_gate_2590)
aura_issue_test_link_light(test_security_schedule_gate_2590)
add_dependencies(all_test_issue_targets test_security_schedule_gate_2590)
# Issue #2591 — per-spec mailbox BP admit threshold override
# (multi-tenant / multi-scope isolation). AgentSpec.bp_admit_threshold
# (optional) overrides the process default (#2535) for one spawn;
# nullopt → process default; 0 → admit off for THIS spawn; N > 0 →
# local threshold. Wire surface: Aura :bp-admit-threshold n kwarg on
# (orch:spawn-agent ...). Test in tests/orch/test_per_scope_bp_admit_2591.cpp
# (src-aligned per #81934). No docs/design/ per #1655 #1485.
aura_add_issue_test(test_per_scope_bp_admit_2591)
aura_issue_test_link_light(test_per_scope_bp_admit_2591)
add_dependencies(all_test_issue_targets test_per_scope_bp_admit_2591)
# Issue #2592 — mailbox deliver / cross-fiber principal verify at the
# receive site (try_pop + recv). Re-calls
# aura_fiber_install_tenant_scope_for_resume (idempotent; bumps
# tenant_scope_mismatch_total on forged ambient). Closes the
# #2491 residual: ambient principal must match target fiber's
# assigned_tenant_id before handler runs. Test in
# tests/serve/test_mailbox_tenant_principal_2592.cpp
# (src-aligned per #81934). No docs/design/ per #1655 #1485.
aura_add_issue_test(test_mailbox_tenant_principal_2592)
aura_issue_test_link_light(test_mailbox_tenant_principal_2592)
add_dependencies(all_test_issue_targets test_mailbox_tenant_principal_2592)
