// tests/test_contracts.cpp
// C++ unit test for C++26 contract_assert + trailing `pre`/`post` enforcement.
//
// Usage:
//   test_contracts holds    — runs a function with pre-condition that holds
//   test_contracts fires    — runs a function with pre-condition that violates
//
// The custom handle_contract_violation() below mirrors the one in
// src/core/contract_handler.cpp (which prints "contract violation" and aborts).
// The test_contracts version is a custom one that calls _Exit(42) so we can
// verify the exit code in CI.
//
// This is the C++-level test for Issue #83's contract infrastructure.
// The Aura-level test is tests/contracts_test.aura.

#include <contracts>

// Custom violation handler — at global scope, exact name + signature so the
// compiler runtime finds it via symbol lookup.

#include <cstdio>
#include <iostream>
#include <print>
#include <string>
static int violation_count = 0;
static void handle_contract_violation(const std::contracts::contract_violation& v) {
    violation_count++;
    std::println(std::cerr,
                  "test_handler: contract violation (count={} kind={} semantic={})",
                  violation_count, (int)v.kind(), (int)v.semantic());
    _Exit(42);
}

// C++26 trailing pre-condition (the user-requested syntax)
static int positive_only(const int x)
    pre (x > 0)
{
    return x * 2;
}

// C++26 trailing pre + post
static int divide_ok(const int a, const int b)
    pre (b != 0)
    post (r: r == a / b)
{
    return a / b;
}

// Function with internal contract_assert
static int internal_check(const int x)
    pre (x > 0)
{
    contract_assert(x < 1000);  // additional runtime invariant
    return x;
}

static int run_holds() {
    std::setbuf(stdout, nullptr);
    int r1 = positive_only(5);
    std::printf("holds: positive_only(5)=%d\n", r1);
    if (r1 != 10) {
        std::println(std::cerr, "FAIL: expected 10");
        return 1;
    }
    int r2 = divide_ok(10, 2);
    std::printf("holds: divide_ok(10,2)=%d\n", r2);
    if (r2 != 5) {
        std::println(std::cerr, "FAIL: expected 5");
        return 1;
    }
    std::printf("PASS: pre + post held, no violations\n");
    return 0;
}

static int run_fires() {
    std::setbuf(stdout, nullptr);
    std::printf("fires: calling positive_only(-1) — pre should fire\n");
    int r2 = positive_only(-1);
    std::printf("fires FAIL: should not reach r2=%d\n", r2);
    return 1;  // should not reach (handler _Exits 42)
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(std::cerr, "usage: test_contracts {{holds|fires}}");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "holds") return run_holds();
    if (mode == "fires") return run_fires();
    std::println(std::cerr, "unknown mode: {}", argv[1]);
    return 2;
}
