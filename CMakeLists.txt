cmake_minimum_required(VERSION 3.28)
project(aura LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API
    "2182bf5c-ef0d-489a-91da-49dbc3090d2a")

add_executable(aura)

target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx
    src/core/ast.ixx
    src/core/core.ixx)

target_sources(aura PRIVATE
    src/parser/lexer_impl.cpp
    src/parser/parser_impl.cpp)
target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/parser/lexer.ixx
    src/parser/parser.ixx)

target_sources(aura PRIVATE
    src/compiler/frontend_impl.cpp)
target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/compiler/frontend.ixx)

target_sources(aura PRIVATE src/main.cpp)

target_compile_options(aura PRIVATE -fmodules-ts -Wall -Wextra -Wpedantic)
target_link_libraries(aura PRIVATE stdc++)

enable_testing()

macro(aura_test name input expected)
    add_test(NAME ${name}
        COMMAND sh -c "test \"$(printf '${input}' | ./build/aura)\" = ${expected}")
endmacro()

add_test(NAME step09_int_42
    COMMAND sh -c "test \"$(echo 42 | ./aura)\" = 42"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step09_int_neg5
    COMMAND sh -c "test \"$(echo -5 | ./aura)\" = -5"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step10_var_error
    COMMAND sh -c "echo x | ./aura 2>&1 | grep -F 'unbound variable'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step10_let
    COMMAND sh -c "test \"$(printf '(let ((x 10)) x)' | ./aura)\" = 10"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step10_let_nested
    COMMAND sh -c "test \"$(printf '(let ((x 10)) (let ((x 20)) x))' | ./aura)\" = 20"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_add
    COMMAND sh -c "test \"$(echo '(+ 1 2)' | ./aura)\" = 3"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_sub
    COMMAND sh -c "test \"$(echo '(- 5 3)' | ./aura)\" = 2"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_mul
    COMMAND sh -c "test \"$(echo '(* 2 3)' | ./aura)\" = 6"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_nested
    COMMAND sh -c "test \"$(echo '(+ 1 (* 2 3))' | ./aura)\" = 7"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_eq
    COMMAND sh -c "test \"$(echo '(= 3 3)' | ./aura)\" = 1"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step12_if_true
    COMMAND sh -c "test \"$(printf '(if 1 42 0)' | ./aura)\" = 42"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step12_if_false
    COMMAND sh -c "test \"$(printf '(if 0 0 99)' | ./aura)\" = 99"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
