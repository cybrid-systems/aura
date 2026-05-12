cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(aura LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(aura)
set_property(TARGET aura PROPERTY CXX_MODULE_STD ON)

target_sources(aura PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/core/arena.ixx src/core/ast.ixx src/core/ast_flat.ixx src/core/ast_pool.ixx src/core/core.ixx
    src/core/type.ixx src/core/type_info.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/frontend.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_interpreter.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx
    src/binary/abf_deserializer.ixx)

target_sources(aura PRIVATE
    src/core/ast_impl.cpp
    src/core/ast_flat_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp src/parser/flat_parser_impl.cpp
    src/compiler/frontend_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/lowering_flat_impl.cpp
    src/compiler/ir_interpreter_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/arity_impl.cpp
    src/binary/abf_deserializer_impl.cpp src/main.cpp)

target_include_directories(aura PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(aura PRIVATE -Wall -Wextra -Wpedantic -fcontracts -fcontract-evaluation-semantic=observe)
target_link_libraries(aura PRIVATE stdc++ stdc++exp)

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
    src/core/arena.ixx src/core/ast.ixx src/core/ast_flat.ixx src/core/ast_pool.ixx src/core/core.ixx
    src/core/type.ixx src/core/type_info.ixx
    src/parser/lexer.ixx src/parser/parser.ixx
    src/compiler/frontend.ixx
    src/compiler/ir.ixx
    src/compiler/lowering.ixx
    src/compiler/ir_interpreter.ixx
    src/compiler/compute_kind.ixx
    src/compiler/arity.ixx
    src/compiler/diag.ixx
    src/compiler/service.ixx
    src/compiler/pass_manager.ixx
    src/compiler/query.ixx)
target_sources(test_ir PRIVATE
    src/core/ast_impl.cpp
    src/core/ast_flat_impl.cpp
    src/core/type_impl.cpp
    src/parser/lexer_impl.cpp src/parser/parser_impl.cpp src/parser/flat_parser_impl.cpp
    src/compiler/frontend_impl.cpp
    src/compiler/lowering_impl.cpp
    src/compiler/lowering_flat_impl.cpp
    src/compiler/ir_interpreter_impl.cpp
    src/compiler/compute_kind_impl.cpp
    src/compiler/query_impl.cpp
    src/compiler/arity_impl.cpp)
target_compile_options(test_ir PRIVATE -Wall -Wextra -Wpedantic -fcontracts -fcontract-evaluation-semantic=observe)
target_link_libraries(test_ir PRIVATE stdc++ stdc++exp)

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

# Diag: diagnostic shows node_id in format
add_test(NAME cli_serve_fix COMMAND sh -c "printf '(+ x 1)' | timeout 5 ./aura --serve | grep -q -F 'patches'" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── M3 Phase 2: Runtime reflection tests ──────────────────────
add_test(NAME inspect_closure_fact
    COMMAND sh -c "printf '(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))' | timeout 5 ./aura --inspect | grep -q 'closures (1)'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME inspect_no_closure
    COMMAND sh -c "printf '(+ 1 2)' | timeout 5 ./aura --inspect | grep -q 'closures (0)'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME inspect_env
    COMMAND sh -c "printf '(let ((x 10)) x)' | timeout 5 ./aura --env | grep -q 'result: 10'"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── validate_abf_nodes: P2996 ABF struct layout validation ────
add_executable(validate-abf-nodes tests/validate_abf_nodes.cpp)
target_include_directories(validate-abf-nodes PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(validate-abf-nodes PRIVATE -std=c++26 -freflection)
target_link_libraries(validate-abf-nodes PRIVATE stdc++)

add_test(NAME validate_abf_nodes
    COMMAND ./validate-abf-nodes
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── aura-reflect: standalone P2996 reflection tool ───────────────
# Built WITHOUT modules to avoid -freflection / import std; conflicts.
add_executable(aura-reflect tools/aura-reflect.cpp)
target_include_directories(aura-reflect PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(aura-reflect PRIVATE -std=c++26 -freflection)
target_link_libraries(aura-reflect PRIVATE stdc++)

add_test(NAME reflect_ir_instruction
    COMMAND ./aura-reflect --ir-instruction
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME reflect_schema
    COMMAND ./aura-reflect --schema
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ── aura-schema: standalone JSON Schema generator ────────────────
add_executable(aura-schema tools/aura-schema.cpp)
target_include_directories(aura-schema PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(aura-schema PRIVATE -std=c++26 -freflection)
target_link_libraries(aura-schema PRIVATE stdc++)

add_test(NAME schema_ir_instruction
    COMMAND ./aura-schema
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME schema_serve
    COMMAND sh -c "./aura-schema --serve | head -1 | grep -q schema"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
