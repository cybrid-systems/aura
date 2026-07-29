// aura.reflect — GMF wrapper for reflect.hh (Issue #268 / #2290).
//
// g++ 16.1.0 conclusions (re-probed):
//   - This interface unit builds with -freflection (meta only in GMF).
//   - Hard limit: do not mix `import std` with #include <meta> in one TU.
//   - Consumers that `import std` + `import aura.reflect` and then use
//     exported reflect templates / meta consteval still fail (BMI /
//     header conflicts). Pure runtime-only exports are less constrained
//     but not how this module is used today.
//   - Production path: #include "reflect/reflect.hh" in non-import-std
//     -freflection TUs (aura-reflect, tests/*_reflect*.cpp), or C ABI.
//
// Business partitions (`import std`) must not include this header or
// <meta>; use hygiene_validate.hh when only hygiene gates are needed.
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
