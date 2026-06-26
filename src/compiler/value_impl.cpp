// value_impl.cpp — Implementation for aura.compiler.value exported functions
module;

module aura.compiler.value;
import std;

// `is_truthy` moved to aura::compiler::evaluator_pure
// (Issue #146 Phase 3 extract). Legacy `types::is_truthy`
// is a using-alias in evaluator_pure.ixx for backward compat.
