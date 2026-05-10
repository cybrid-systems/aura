cmake_minimum_required(VERSION 3.28)
project(aura LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API
    "2182bf5c-ef0d-489a-91da-49dbc3090d2a")

add_executable(aura)

# ─── Core layer ───
target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx
    src/core/ast.ixx
    src/core/core.ixx)

# ─── Parser layer ───
target_sources(aura PRIVATE
    src/parser/lexer_impl.cpp
    src/parser/parser_impl.cpp)
target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/parser/lexer.ixx
    src/parser/parser.ixx)

# ─── Compiler layer ───
target_sources(aura PRIVATE
    src/compiler/frontend_impl.cpp)
target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/compiler/frontend.ixx)

# ─── Main ───
target_sources(aura PRIVATE src/main.cpp)

target_compile_options(aura PRIVATE
    -fmodules-ts
    -Wall -Wextra -Wpedantic)
target_link_libraries(aura PRIVATE stdc++)

set_target_properties(aura PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# ─── Tests ───
enable_testing()

add_test(NAME step09_literal_42
    COMMAND sh -c "test \"\$(echo 42 | ./aura)\" = 42"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME step09_literal_neg5
    COMMAND sh -c "test \"\$(echo -5 | ./aura)\" = -5"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME step09_literal_large
    COMMAND sh -c "test \"\$(echo 999999 | ./aura)\" = 999999"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
