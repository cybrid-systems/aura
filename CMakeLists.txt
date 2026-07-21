cmake_minimum_required(VERSION 4.3)
# CMake 4.4+ gate UUID (was 451f2fe2… on 4.3).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
project(aura LANGUAGES CXX C)
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
    src/compiler/arity_impl.cpp
    src/compiler/sv_ir_impl.cpp
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
 


# Issue #1967: tui:* domain is a commercial UI vertical (deferred from
# SlimSurface core budget per #1965). Default ON so existing tests +
# lib/std/tui keep working; slim/core-only builds can set
# -DAURA_ENABLE_TUI=OFF to skip registration (see docs/tui.md).
option(AURA_ENABLE_TUI "Register tui:* domain primitives (UI vertical, #1967)" ON)

# Issue #1968: eda:* commercial EDA vertical (deferred). Default ON for
# closed-loop SV / hardware-feedback tests; slim builds:
# -DAURA_ENABLE_EDA=OFF (see docs/eda.md).
option(AURA_ENABLE_EDA "Register eda:* domain primitives (EDA vertical, #1968)" ON)

# Issue #1969: auto-evolve-* self-evolution AI vertical (deferred). Default ON
# for agent closed-loop tests; slim builds: -DAURA_ENABLE_AUTO_EVOLVE=OFF
# (see docs/auto-evolve.md). Only the 7 auto-evolve-* names are gated;
# agent:/strategy: co-registered in the same TU stay available.
option(AURA_ENABLE_AUTO_EVOLVE
       "Register auto-evolve-* domain primitives (self-evo vertical, #1969)" ON)

# Issue #1970: git-* integration vertical (deferred). Default ON for
# production-sweep / git tooling tests; slim builds: -DAURA_ENABLE_GIT=OFF
# (see docs/git-integration.md). Independent of AURA_HAVE_LIBGIT2 (backend).
option(AURA_ENABLE_GIT "Register git-* domain primitives (integration, #1970)" ON)

# Issue #1971: terminal:* UI/integration vertical (deferred). Default ON so
# deprecation tests (#1351) keep working; slim builds: -DAURA_ENABLE_TERMINAL=OFF
# (see docs/terminal-domain.md). These 7 names are Phase-A deprecated no-ops.
option(AURA_ENABLE_TERMINAL
       "Register terminal:* domain primitives (UI vertical, #1971)" ON)

# Issue #1972: seva:* service-evaluation commercial vertical (deferred).
# Default ON for OpenClaw / demo metrics tests; slim:
# -DAURA_ENABLE_SEVA=OFF (see docs/seva.md).
option(AURA_ENABLE_SEVA "Register seva:* domain primitives (commercial, #1972)" ON)

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
if(AURA_ENABLE_TUI)
    set(_AURA_ENABLE_TUI_VAL 1)
else()
    set(_AURA_ENABLE_TUI_VAL 0)
endif()
if(AURA_ENABLE_EDA)
    set(_AURA_ENABLE_EDA_VAL 1)
else()
    set(_AURA_ENABLE_EDA_VAL 0)
endif()
if(AURA_ENABLE_AUTO_EVOLVE)
    set(_AURA_ENABLE_AUTO_EVOLVE_VAL 1)
else()
    set(_AURA_ENABLE_AUTO_EVOLVE_VAL 0)
endif()
if(AURA_ENABLE_GIT)
    set(_AURA_ENABLE_GIT_VAL 1)
else()
    set(_AURA_ENABLE_GIT_VAL 0)
endif()
if(AURA_ENABLE_TERMINAL)
    set(_AURA_ENABLE_TERMINAL_VAL 1)
else()
    set(_AURA_ENABLE_TERMINAL_VAL 0)
endif()
if(AURA_ENABLE_SEVA)
    set(_AURA_ENABLE_SEVA_VAL 1)
else()
    set(_AURA_ENABLE_SEVA_VAL 0)
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
set_source_files_properties(
    src/compiler/evaluator_primitives_tui.cpp
    PROPERTIES COMPILE_DEFINITIONS "AURA_ENABLE_TUI=${_AURA_ENABLE_TUI_VAL}")
# eda:* lives in evaluator_primitives_eda.cpp + 3 adds in compile.cpp (p34/p35).
set_source_files_properties(
    src/compiler/evaluator_primitives_eda.cpp
    PROPERTIES COMPILE_DEFINITIONS "AURA_ENABLE_EDA=${_AURA_ENABLE_EDA_VAL}")
# compile.cpp hosts both eda:* (p34/p35) and seva:* (p59–p62) — both flags.
set_source_files_properties(
    src/compiler/evaluator_primitives_compile.cpp
    PROPERTIES COMPILE_DEFINITIONS
               "AURA_ENABLE_EDA=${_AURA_ENABLE_EDA_VAL};AURA_ENABLE_SEVA=${_AURA_ENABLE_SEVA_VAL}")
# agent.cpp: auto-evolve (#1969) + synthesize (#1974) + strategy: (#1973).
# agent:/intend/define-strategy co-registered stay always on.
set_source_files_properties(
    src/compiler/evaluator_primitives_agent.cpp
    PROPERTIES
        COMPILE_DEFINITIONS
        "AURA_ENABLE_AUTO_EVOLVE=${_AURA_ENABLE_AUTO_EVOLVE_VAL};AURA_ENABLE_SYNTHESIZE=${_AURA_ENABLE_SYNTHESIZE_VAL};AURA_ENABLE_STRATEGY=${_AURA_ENABLE_STRATEGY_VAL}")
# io.cpp: git-* (#1970) + tcp-* (#1975). getenv/http/terminal-buffer stay on.
set_source_files_properties(
    src/compiler/evaluator_primitives_io.cpp
    PROPERTIES COMPILE_DEFINITIONS
               "AURA_ENABLE_GIT=${_AURA_ENABLE_GIT_VAL};AURA_ENABLE_TCP=${_AURA_ENABLE_TCP_VAL}")
# terminal:* deprecated no-ops live in obs_jit register_jit_p59 + p113.
set_source_files_properties(
    src/compiler/evaluator_primitives_obs_jit.cpp
    PROPERTIES COMPILE_DEFINITIONS
               "AURA_ENABLE_TERMINAL=${_AURA_ENABLE_TERMINAL_VAL}")
# m4-* linear stubs in evaluator_primitives_math.cpp (#1976); math/regex stay on.
set_source_files_properties(
    src/compiler/evaluator_primitives_math.cpp
    PROPERTIES COMPILE_DEFINITIONS "AURA_ENABLE_M4=${_AURA_ENABLE_M4_VAL}")
message(STATUS "tui:* domain: AURA_ENABLE_TUI=${_AURA_ENABLE_TUI_VAL} (#1967)")
message(STATUS "eda:* domain: AURA_ENABLE_EDA=${_AURA_ENABLE_EDA_VAL} (#1968)")
message(STATUS
        "auto-evolve-* domain: AURA_ENABLE_AUTO_EVOLVE=${_AURA_ENABLE_AUTO_EVOLVE_VAL} (#1969)")
message(STATUS "git-* domain: AURA_ENABLE_GIT=${_AURA_ENABLE_GIT_VAL} (#1970)")
message(STATUS
        "terminal:* domain: AURA_ENABLE_TERMINAL=${_AURA_ENABLE_TERMINAL_VAL} (#1971)")
message(STATUS "seva:* domain: AURA_ENABLE_SEVA=${_AURA_ENABLE_SEVA_VAL} (#1972)")
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
add_executable(test_ir tests/arena/test_ir.cpp)
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
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
    src/compiler/hot_update_registry.cpp # Issue #1956
    # Issue #675 follow-up: http_health.cpp provides
    # aura::serve::http::port_from_env (used by
    # evaluator_primitives_security.cpp's
    # (query:security-stats) primitive). Without this
    # the test_ir link fails with "undefined reference
    # to aura::serve::http::port_from_env".
    src/serve/http_health.cpp
    # Issue #1559 + #1674: present_batch / draw_batch engine (header-based,
    # non-module). aura::renderer::draw_batch is referenced by the
    # (term-render-draw-batch) primitive in evaluator_primitives_io.cpp:1318.
    # Without this the test_ir link fails with
    # "undefined reference to aura::renderer::draw_batch".
    src/renderer/render_primitives.cpp)

set_source_files_properties(src/core/contract_handler.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
set_source_files_properties(src/core/contract_stub.cpp PROPERTIES COMPILE_FLAGS "-include contracts -fcontracts")
# Issue #477: production-quality test file — enforce [[nodiscard]],
# unused-parameter, and sign-compare warnings on test_ir.cpp itself.
# The linked-in production sources (aura_test_objects, the per-target
# source list above) keep the lax `aura_test_compile_options` settings.
aura_strict_test_warnings(tests/arena/test_ir.cpp)
aura_test_compile_options(test_ir)
target_include_directories(test_ir PRIVATE src src/compiler)
target_link_libraries(test_ir PRIVATE stdc++ stdc++exp aura-reflect)

# ── Concurrent model tests ────────────────────────────
# Tests fiber lifecycle, worker scheduling, and eventfd wakeup
add_executable(test_concurrent tests/fiber/test_concurrent.cpp)
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
aura_strict_test_warnings(tests/fiber/test_concurrent.cpp)
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
# Do not grow tests/issues/test_issue_*.cpp for routine work.
#
# Layers still present:
#   1. Domain suites — tests/domain/test_domain_*.cpp, test_obs_schema_matrix
#      (registered via aura_add_issue_test in CMakeLists.txt; resolved via
#      aura_resolve_test_cpp). AuraDomainTests.cmake removed in Anqi 2026-07-21
#      wave (was dead code — never included in CMakeLists.txt).
#   2. Bundle executables — tests/bundles/test_issues_<profile>_main.cpp
#      + members from tests/issues/ (link profiles in fixtures/).
#   3. Family batch drivers — tests/test_*_batch.cpp (EXCLUDE_FROM_ALL
#      intermediate consolidation; same CMake file).
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
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
    # in the archive (duplicate symbols).
    add_library(aura_jit_test_objects STATIC
        src/compiler/aura_jit.cpp
        src/compiler/aura_jit_runtime.cpp
        src/compiler/aura_jit_bridge.cpp
        src/compiler/hot_update_registry.cpp # Issue #1956
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
    # Source resolution: issues/ → domain/ → domain/<theme>/ → tests/
    # (see aura_resolve_test_cpp in cmake/AuraTest.cmake).
    # Policy: tests/README.md (#1958) · domain/arena pilot (#1959)
    # Inventory: tests/legacy_test_inventory.md (#1957)
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
add_executable(test_gc_evaluator_integration tests/compiler_core/test_gc_evaluator_integration.cpp)
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
    # Issue #1884: C hooks for TypedMutationAudit correlation (non-module)
    src/compiler/typed_mutation_audit_hooks.cpp
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
add_executable(test_contracts tests/compiler_core/test_contracts.cpp)
set_source_files_properties(tests/compiler_core/test_contracts.cpp PROPERTIES COMPILE_FLAGS "-include contracts")
target_compile_options(test_contracts PRIVATE -fcontracts)
# Issue #477: contract test is small + production-quality; opt into strict
# warnings for the test source itself.
aura_strict_test_warnings(tests/compiler_core/test_contracts.cpp)
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
add_executable(test_ir_cache_v2 tests/edsl/test_ir_cache_v2.cpp)
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
# test_persist) removed — coverage lives in tests/domain/* suites
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
add_executable(test_shape tests/shape/test_shape.cpp src/compiler/shape_profiler.cpp)
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
add_executable(test_spec_jit tests/mutation/test_spec_jit.cpp
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
add_executable(test_jit_metrics tests/jit/test_jit_metrics.cpp
    tests/jit/test_jit_metrics_stub.cpp
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
add_executable(test_jit_concurrent_compile tests/fiber/test_jit_concurrent_compile.cpp
    tests/jit/test_jit_metrics_stub.cpp
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
add_executable(test_jit_consistency tests/observability/test_jit_consistency.cpp
    tests/jit/test_jit_metrics_stub.cpp
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
# Bridge header: tests/issues/test_issue_178_bridge.h
# Only the reflect TU needs -freflection; applying it to the whole
# target forces aura_test_objects BMI synth into a reflection partition
# that conflicts with import std (GCC 16 modules).
add_executable(test_issue_178 tests/issues/test_issue_178.cpp tests/issues/test_issue_178_reflect.cpp)
set_property(TARGET test_issue_178 PROPERTY CXX_MODULE_STD ON)
target_include_directories(test_issue_178 PRIVATE src tests)
aura_test_compile_options(test_issue_178)
set_source_files_properties(tests/issues/test_issue_178_reflect.cpp
                            PROPERTIES COMPILE_OPTIONS "-freflection")
target_compile_features(test_issue_178 PUBLIC cxx_std_26)
target_link_libraries(test_issue_178 PRIVATE aura_test_objects stdc++ pthread)

# Issue #218: reflection integration (Cycle 5) — query:schema +
# hygienic macro composition (#140 + #248). The 1000+ reflect
# stress loop is in test_issue_178; TSan/ASan:
# tests/run_issue_218_tsan.sh

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
aura_issue_test_link_llvm_jit(test_mutation_rollback_coverage)
# Issue #415: DirtyReason verification categories +
# mark_dirty_upward propagation metrics synthesis.
aura_add_issue_test(test_dirty_reason_verification_propagation)
aura_issue_test_link_llvm_jit(test_dirty_reason_verification_propagation)
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
aura_issue_test_link_llvm_jit(test_coercion_dead_elim_castop_flow_zerooverhead)
# Issue #495: Task2 refinement closed-loop integration matrix.
aura_add_issue_test(test_task2_refinement_closed_loop)
aura_issue_test_link_llvm_jit(test_task2_refinement_closed_loop)
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
aura_issue_test_link_llvm_jit(test_hardware_resource_linear_ownership)
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
aura_add_issue_test(test_incremental_typed_selfmod_dirty_narrowing)
aura_issue_test_link_llvm_jit(test_incremental_typed_selfmod_dirty_narrowing)
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
aura_add_issue_test(test_atomic_batch_rollback_closed_loop)
aura_issue_test_link_llvm_jit(test_atomic_batch_rollback_closed_loop)
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
aura_issue_test_link_llvm_jit(test_stable_ref_provenance_fiber_cow)
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
aura_issue_test_link_llvm_jit(test_static_reflect_selfmod_validation_task6)
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

# Default issue-suite build: the 12 profile bundles (not 200+ standalones).
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

# Domain / theme suite registrations (AuraDomainTests.cmake) temporarily
# not re-included here: wave #1978 left orphan target names that fail
# aura_resolve_test_cpp. Re-enable after domain cmake is refreshed.
# include(${CMAKE_SOURCE_DIR}/cmake/AuraDomainTests.cmake)
