cmake_minimum_required(VERSION 4.0)
project(aura LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(aura)

target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx
    src/core/ast.ixx
    src/core/core.ixx
    src/parser/lexer.ixx
    src/parser/parser.ixx
    src/compiler/frontend.ixx)

target_sources(aura PRIVATE
    src/parser/lexer_impl.cpp
    src/parser/parser_impl.cpp
    src/compiler/frontend_impl.cpp
    src/main.cpp)

target_compile_options(aura PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(aura PRIVATE stdc++)

# Enable standard header module scanning for import <iostream> etc.
set(CMAKE_CXX_MODULE_STANDARD_HEADERS ON)

set_target_properties(aura PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

enable_testing()

add_test(NAME step09_int_42
    COMMAND sh -c "test \"\$(echo 42 | ./aura)\" = 42"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step09_int_neg5
    COMMAND sh -c "test \"\$(echo -5 | ./aura)\" = -5"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step10_var_error
    COMMAND sh -c "echo x | ./aura 2>&1 | grep -F 'unbound variable'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step10_let
    COMMAND sh -c "test \"\$(printf '(let ((x 10)) x)' | ./aura)\" = 10"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step10_let_nested
    COMMAND sh -c "test \"\$(printf '(let ((x 10)) (let ((x 20)) x))' | ./aura)\" = 20"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_add
    COMMAND sh -c "test \"\$(echo '(+ 1 2)' | ./aura)\" = 3"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_sub
    COMMAND sh -c "test \"\$(echo '(- 5 3)' | ./aura)\" = 2"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_mul
    COMMAND sh -c "test \"\$(echo '(* 2 3)' | ./aura)\" = 6"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_nested
    COMMAND sh -c "test \"\$(echo '(+ 1 (* 2 3))' | ./aura)\" = 7"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step11_eq
    COMMAND sh -c "test \"\$(echo '(= 3 3)' | ./aura)\" = 1"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step12_if_true
    COMMAND sh -c "test \"\$(printf '(if 1 42 0)' | ./aura)\" = 42"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step12_if_false
    COMMAND sh -c "test \"\$(printf '(if 0 0 99)' | ./aura)\" = 99"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step15_letrec_fact5
    COMMAND sh -c "test \"\$(printf '(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))' | ./aura)\" = 120"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME step15_letrec_fact10
    COMMAND sh -c "test \"\$(printf '(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))' | ./aura)\" = 3628800"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
