#pragma once

// Issue #178 / #268 — C ABI between the module TU (real NodeView) and the
// non-module reflect TU (P2996 + reflect.hh).
//
// GCC 16.1 ICEs when one TU both `import aura.core.ast` and
// `#include "reflect/reflect.hh"` (<meta>). Split:
//   - test_issue_178.cpp          (module side, drives NodeView)
//   - test_issue_178_reflect.cpp  (reflect side, implements these entry points)
//
// Lives next to the two TUs under tests/issues/ (not tests/ root).
//
// Include from the reflect TU only (after <cstdint>/<cstddef>).
// The module TU (test_issue_178.cpp) re-declares these by hand —
// #include under CXX_MODULE_STD + import std conflicts (GCC 16).

#include <cstddef>
#include <cstdint>

void issue178_reset_counters();
int issue178_failed_count();

void issue178_run_reflect_member_tests();
void issue178_run_ir_roundtrip_tests();

int issue178_roundtrip_populated(std::uint32_t id, std::uint32_t tag, std::int64_t int_value,
                                 double float_value, std::uint32_t sym_id, std::uint32_t line,
                                 std::uint32_t col, std::uint32_t type_id,
                                 const std::uint32_t* children, std::size_t children_count,
                                 const std::uint32_t* params, std::size_t params_count,
                                 const std::uint32_t* annot, std::size_t annot_count,
                                 std::uint8_t marker, std::size_t* out_bytes);

int issue178_roundtrip_empty(std::size_t* out_bytes);

int issue178_roundtrip_verify_marker(std::uint8_t marker_out);

// Issue #218: 1000+ serialize/deserialize iterations (TSan/ASan target).
int issue178_run_stress_iterations(int iterations);
