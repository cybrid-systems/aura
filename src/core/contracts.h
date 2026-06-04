// src/core/contracts.h
//
// Portable wrapper macros for C++26 Contracts.
//
// GCC 16.1 has __cpp_contracts in the library (so handle_contract_violation
// exists) but doesn't yet support the [[pre:expr]] / [[post:expr]] attribute
// syntax. Until GCC 17+ / clang 20+ lands, fall back to plain assert().
//
// When the real attribute syntax is available, the #else branch will use it
// automatically.
#pragma once

#include <cassert>

#ifdef __has_cpp_attribute
  #if __has_cpp_attribute(pre) >= 202311L
    // Real C++26 contracts attribute syntax available.
    #define AURA_CONTRACT_PRE(expr)  [[pre::audit expr]]
    #define AURA_CONTRACT_POST(expr) [[post::audit expr]]
  #else
    // Fallback: plain assert(). On contract violation, abort.
    #define AURA_CONTRACT_PRE(expr)  do { assert(expr); } while (0)
    #define AURA_CONTRACT_POST(expr) do { assert(expr); } while (0)
  #endif
#else
  // Pre-C++20 fallback.
  #define AURA_CONTRACT_PRE(expr)  do { assert(expr); } while (0)
  #define AURA_CONTRACT_POST(expr) do { assert(expr); } while (0)
#endif

// AURA_CONTRACT_ASSERT — runtime-only assertion (no contract semantics).
// Use for invariants that aren't a pre/post of any function.
#define AURA_CONTRACT_ASSERT(expr) do { assert(expr); } while (0)
