// aura.reflect — C++26 module wrapper for reflect.hh (Issue #268 / #2290).
//
// GCC 16.1.0 re-test (2026-07-29):
//   - Building this GMF module with -freflection succeeds.
//   - A consumer TU that both `import std` and `import aura.reflect`
//     (or `#include <meta>` / reflect.hh after `import std`) still
//     fails with std redefinition / header conflicts.
//   - Until that lifts, production uses split TUs + C ABI bridges
//     (e.g. test_issue_178) or the non-module aura-reflect library.
//
// Pure -freflection, non-module TUs should #include "reflect/reflect.hh"
// directly (see tests/reflect/test_opcode_reflect_2289.cpp).
module;

#include "reflect/reflect.hh"

export module aura.reflect;

export using ::aura::reflect::MemberKind;
export using ::aura::reflect::MemberInfo;
export using ::aura::reflect::ModuleExports;
export using ::aura::reflect::member_count;
export using ::aura::reflect::reflect_members;
export using ::aura::reflect::module_exports;
export using ::aura::reflect::to_json;
export using ::aura::reflect::auto_to_json;
