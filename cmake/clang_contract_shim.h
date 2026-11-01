// Issue #4837: Clang has no C++26 contracts (-fcontracts), no <contracts>
// header, and no pre/post contract specifiers. This header is force-included
// ONLY in the Clang module build that feeds clang-tidy. It is macro-only so it
// can sit in front of a module declaration without breaking the rule that
// 'module;' starts the translation unit.
//
// The GCC build never includes this file.
#pragma once

#ifndef contract_assert
#define contract_assert(expr) ((void)0)
#endif

// Trailing contract specifiers: void f() pre(x > 0) post(r != nullptr) { ... }
// Expanding them to nothing leaves the plain function declarator.
#ifndef pre
#define pre(...)
#endif
#ifndef post
#define post(...)
#endif
