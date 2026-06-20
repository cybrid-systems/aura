# Shared compile options for Aura C++ test targets.
#
# Usage:
#   aura_test_compile_options(my_test_target)
#
# Wired into aura_add_issue_test() and the main test binaries
# (test_ir, test_concurrent, aura_test_objects, aura).

function(aura_test_compile_options TARGET)
    target_compile_options(${TARGET} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-missing-field-initializers
        -Wno-unused-variable -Wno-unused-function
        -Wno-unused-but-set-variable -Wno-unused-parameter
        -Wno-misleading-indentation -Wno-parentheses
    )
endfunction()

function(aura_test_compile_options_reflect TARGET)
    target_compile_options(${TARGET} PRIVATE -freflection)
    aura_test_compile_options(${TARGET})
endfunction()

function(aura_test_compile_options_reflect_minimal TARGET)
    target_compile_options(${TARGET} PRIVATE -freflection -Wall)
endfunction()

# Extra observability header used by several issue tests.
function(aura_issue_test_observability TARGET)
    target_include_directories(${TARGET} PRIVATE src/compiler)
    target_sources(${TARGET} PRIVATE src/compiler/observability_metrics.h)
endfunction()

# LLVM JIT sources shared by orchestration / mutation issue tests.
function(aura_issue_test_link_llvm_jit TARGET)
    target_include_directories(${TARGET} PRIVATE src/compiler)
    target_sources(${TARGET} PRIVATE
        src/compiler/aura_jit.cpp
        src/compiler/aura_jit_runtime.cpp
        src/compiler/aura_jit_bridge.cpp
        src/compiler/observability_metrics.h
        src/compiler/observability_snapshot.h
    )
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    target_include_directories(${TARGET} PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(${TARGET} PRIVATE AURA_HAVE_LLVM=1)
    target_link_libraries(${TARGET} PRIVATE ${llvm_libs})
endfunction()

# LLVM JIT + tests/ include (closure-bridge issue tests).
function(aura_issue_test_link_llvm_jit_tests TARGET)
    target_include_directories(${TARGET} PRIVATE tests)
    aura_issue_test_link_llvm_jit(${TARGET})
endfunction()

# Reflection-only standalone tests (no full aura-reflect link set).
function(aura_add_issue_test_reflect_standalone NAME)
    add_executable(${NAME} tests/${NAME}.cpp)
    set_property(TARGET ${NAME} PROPERTY CXX_MODULE_STD ON)
    target_include_directories(${NAME} PRIVATE src)
    aura_test_compile_options_reflect_minimal(${NAME})
    target_compile_features(${NAME} PUBLIC cxx_std_26)
    target_link_libraries(${NAME} PRIVATE aura_test_objects stdc++ pthread)
    add_test(NAME ${NAME}_verification COMMAND ./${NAME})
endfunction()

# Header-only / gap-buffer style tests (no CXX modules).
function(aura_add_issue_test_standalone NAME)
    add_executable(${NAME} tests/${NAME}.cpp)
    target_include_directories(${NAME} PRIVATE src)
    target_compile_options(${NAME} PRIVATE -Wall -Wextra)
    target_compile_features(${NAME} PUBLIC cxx_std_26)
    target_link_libraries(${NAME} PRIVATE aura_test_objects stdc++ pthread)
    add_test(NAME ${NAME}_verification COMMAND ./${NAME})
endfunction()

# Fiber + JIT stub sources (GC / env_frames issue tests).
function(aura_issue_test_link_llvm_fiber_stubs TARGET)
    target_include_directories(${TARGET} PRIVATE src/serve src/compiler)
    target_sources(${TARGET} PRIVATE
        src/compiler/aura_jit_runtime.cpp
        src/compiler/aura_jit_bridge.cpp
        src/compiler/aura_jit.cpp
        src/core/contract_stub.cpp
        src/compiler/aura_jit_prim_dispatch_stub.cpp
    )
    set_source_files_properties(src/compiler/aura_jit.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    set_source_files_properties(src/compiler/aura_jit_runtime.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
    target_compile_definitions(${TARGET} PRIVATE AURA_HAVE_LLVM=1)
    target_include_directories(${TARGET} PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${llvm_libs})
endfunction()