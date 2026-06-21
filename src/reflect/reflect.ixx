// aura.reflect — C++26 module wrapper for reflect.hh (Issue #268).
//
// Precompiled wrapper for future module consumers. test_issue_178 uses
// a split TU (test_issue_178_reflect.cpp) until GCC 16.2 fixes the
// std-module + <meta> ICE in a single TU.
module;

#include "reflect/reflect.hh"

export module aura.reflect;

export using ::aura::reflect::MemberKind;
export using ::aura::reflect::MemberInfo;