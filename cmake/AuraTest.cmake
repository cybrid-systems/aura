# Shared compile options for Aura C++ test targets.
#
# Usage:
#   aura_test_compile_options(my_test_target)
#
# Wired into aura_add_issue_test() and the main test binaries
# (test_ir, test_concurrent, aura_test_objects, aura).

# Resolve test source: prefer tests/domain/<NAME>.cpp (domain suites),
# then tests/<NAME>.cpp (legacy / unit / issue standalones).
function(aura_resolve_test_cpp NAME OUT_VAR)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/domain/${NAME}.cpp")
        set(${OUT_VAR} "tests/domain/${NAME}.cpp" PARENT_SCOPE)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/tests/${NAME}.cpp")
        set(${OUT_VAR} "tests/${NAME}.cpp" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "aura_resolve_test_cpp(${NAME}): no tests/domain/${NAME}.cpp or tests/${NAME}.cpp")
    endif()
endfunction()

function(aura_test_compile_options TARGET)
    target_compile_options(${TARGET} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-missing-field-initializers
        -Wno-unused-variable -Wno-unused-function
        -Wno-unused-but-set-variable -Wno-unused-parameter
        -Wno-misleading-indentation -Wno-parentheses
        -Wno-sign-compare -Wno-unused-result -Wno-type-limits
        # Issue #0 (CI fix 2026-06-30): libstdc++ 16's <regex>
        # headers (regex_automaton.h, regex_automaton.tcc) trigger
        # `-Werror=maybe-uninitialized` false positives inside
        # std::function's move ctor when imported via `import std`
        # from aura::parser. Suppressing here is safe — our own
        # code is small enough that maybe-uninitialized is
        # genuinely an error we'd want to catch, and the warning
        # only fires from inside the libstdc++ regex headers.
        -Wno-error=maybe-uninitialized -Wno-maybe-uninitialized
        # Issue #1556: MutationBoundaryGuard legacy ctor is [[deprecated]]
        # in favor of try_acquire; ~30 residual call-sites migrate
        # incrementally. Attribute remains for IDEs / new code.
        -Wno-deprecated-declarations
    )
endfunction()

# Issue #477: stricter compile options for production-quality test files.
#
# These are test files that should be held to production-quality
# standards because they are the canonical specification of behavior
# (e.g., test_ir.cpp) or because they are the main entry point for a
# test category (test_concurrent.cpp, test_contracts.cpp).
#
# Usage:
#   aura_strict_test_warnings(tests/test_ir.cpp)
#
# This uses set_source_files_properties(COMPILE_OPTIONS ...) so the
# stricter warnings apply ONLY to the named source file. The rest of
# the target (linked-in libraries like aura_test_objects) keep the
# lax `aura_test_compile_options` settings — the strictness is scoped
# to the test code itself.
#
# The 3 categories removed (per #477's analysis):
#   -Wno-unused-result  →  [[nodiscard]] violations hide real bugs
#                          (mutation ops + error checks not verified)
#   -Wno-unused-parameter  →  unused params indicate mismatched
#                              signatures / incomplete test logic
#   -Wno-sign-compare  →  signed/unsigned comparison bugs (silent
#                          wrap on negative values)
function(aura_strict_test_warnings SOURCE_FILE)
    set_source_files_properties(${SOURCE_FILE} PROPERTIES
        COMPILE_OPTIONS "-Werror=unused-result;-Werror=unused-parameter;-Werror=sign-compare"
    )
endfunction()

function(aura_test_compile_options_reflect TARGET)
    target_compile_options(${TARGET} PRIVATE -freflection)
    aura_test_compile_options(${TARGET})
endfunction()

function(aura_test_compile_options_reflect_minimal TARGET)
    target_compile_options(${TARGET} PRIVATE
        -freflection -Wall
        -Wno-sign-compare -Wno-unused-local-typedefs -Wno-switch
    )
endfunction()

# Extra observability header used by several issue tests.
function(aura_issue_test_observability TARGET)
    target_include_directories(${TARGET} PRIVATE src/compiler)
    target_sources(${TARGET} PRIVATE src/compiler/observability_metrics.h)
endfunction()

# LLVM JIT via shared aura_jit_test_objects (see CMakeLists.txt).
# Compiling aura_jit*.cpp once avoids 100+ concurrent GCC module
# dyndep scans that flaked CI with:
#   when writing output to ...aura_jit_bridge.cpp.o.ddi.i: Invalid argument
function(aura_issue_test_link_llvm_jit TARGET)
    if(NOT TARGET aura_jit_test_objects)
        message(FATAL_ERROR
            "aura_issue_test_link_llvm_jit(${TARGET}): aura_jit_test_objects missing "
            "(LLVM not found or library not defined yet)")
    endif()
    target_include_directories(${TARGET} PRIVATE src/compiler)
    target_sources(${TARGET} PRIVATE
        src/compiler/observability_metrics.h
        src/compiler/observability_snapshot.h
    )
    # AURA_HAVE_LLVM + LLVM includes + llvm_libs come PUBLIC from the lib.
    target_link_libraries(${TARGET} PRIVATE aura_jit_test_objects)
endfunction()

# LLVM JIT without observability headers (light JIT API tests).
function(aura_issue_test_link_llvm_jit_minimal TARGET)
    if(NOT TARGET aura_jit_test_objects)
        message(FATAL_ERROR
            "aura_issue_test_link_llvm_jit_minimal(${TARGET}): aura_jit_test_objects missing")
    endif()
    target_include_directories(${TARGET} PRIVATE src/compiler)
    target_link_libraries(${TARGET} PRIVATE aura_jit_test_objects)
endfunction()

# LLVM JIT + observability + contract stub.
function(aura_issue_test_link_llvm_jit_contract TARGET)
    aura_issue_test_link_llvm_jit(${TARGET})
    target_sources(${TARGET} PRIVATE src/core/contract_stub.cpp)
endfunction()

# LLVM JIT + tests/ include (closure-bridge issue tests).
function(aura_issue_test_link_llvm_jit_tests TARGET)
    target_include_directories(${TARGET} PRIVATE tests)
    aura_issue_test_link_llvm_jit(${TARGET})
endfunction()

# Reflection-only standalone tests (no full aura-reflect link set).
function(aura_add_issue_test_reflect_standalone NAME)
    aura_resolve_test_cpp(${NAME} _src)
    add_executable(${NAME} ${_src})
    set_property(TARGET ${NAME} PROPERTY CXX_MODULE_STD ON)
    target_include_directories(${NAME} PRIVATE src)
    aura_test_compile_options_reflect_minimal(${NAME})
    target_compile_features(${NAME} PUBLIC cxx_std_26)
    target_link_libraries(${NAME} PRIVATE aura_test_objects stdc++ pthread)
    add_test(NAME ${NAME}_verification COMMAND ./${NAME})
endfunction()

# Header-only / gap-buffer style tests (no CXX modules).
function(aura_add_issue_test_standalone NAME)
    aura_resolve_test_cpp(${NAME} _src)
    add_executable(${NAME} ${_src})
    target_include_directories(${NAME} PRIVATE src)
    target_compile_options(${NAME} PRIVATE -Wall -Wextra)
    target_compile_features(${NAME} PUBLIC cxx_std_26)
    target_link_libraries(${NAME} PRIVATE aura_test_objects stdc++ pthread)
    add_test(NAME ${NAME}_verification COMMAND ./${NAME})
endfunction()

# Fiber + JIT stub sources (GC / env_frames issue tests).
function(aura_issue_test_link_llvm_fiber_stubs TARGET)
    if(NOT TARGET aura_jit_test_objects)
        message(FATAL_ERROR
            "aura_issue_test_link_llvm_fiber_stubs(${TARGET}): aura_jit_test_objects missing")
    endif()
    target_include_directories(${TARGET} PRIVATE src/serve src/compiler)
    target_sources(${TARGET} PRIVATE
        src/core/contract_stub.cpp
        src/compiler/aura_jit_prim_dispatch_stub.cpp
    )
    target_link_libraries(${TARGET} PRIVATE aura_jit_test_objects)
endfunction()