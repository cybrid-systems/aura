# 0 "/home/dev/code/aura/src/compiler/type_checker.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/type_checker.ixx"
module;
# 1 "/usr/local/include/c++/16.1.0/cstdint" 1 3
# 40 "/usr/local/include/c++/16.1.0/cstdint" 3
# 1 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 1 3
# 37 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"

#pragma GCC diagnostic ignored "-Wc++11-extensions"
#pragma GCC diagnostic ignored "-Wc++23-extensions"
# 342 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
namespace std
{
  typedef long unsigned int size_t;
  typedef long int ptrdiff_t;


  typedef decltype(nullptr) nullptr_t;


#pragma GCC visibility push(default)


  extern "C++" __attribute__ ((__noreturn__, __always_inline__))
  inline void __terminate() noexcept
  {
    void terminate() noexcept __attribute__ ((__noreturn__,__cold__));
    terminate();
  }
#pragma GCC visibility pop
}
# 375 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
namespace std
{
  inline namespace __cxx11 __attribute__((__abi_tag__ ("cxx11"))) { }
}
namespace __gnu_cxx
{
  inline namespace __cxx11 __attribute__((__abi_tag__ ("cxx11"))) { }
}
# 579 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
namespace std
{
#pragma GCC visibility push(default)




  __attribute__((__always_inline__))
  constexpr inline bool
  __is_constant_evaluated() noexcept
  {


    if consteval { return true; } else { return false; }






  }
#pragma GCC visibility pop
}
# 623 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
namespace std
{
#pragma GCC visibility push(default)

  extern "C++" __attribute__ ((__noreturn__)) __attribute__((__cold__))
  void
  __glibcxx_assert_fail
    (const char* __file, int __line, const char* __function,
     const char* __condition)
  noexcept;
#pragma GCC visibility pop
}
# 654 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
namespace std
{
  __attribute__((__always_inline__,__visibility__("default")))
  inline void
  __glibcxx_assert_fail()
  { }
}
# 733 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
# 1 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/os_defines.h" 1 3
# 39 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/os_defines.h" 3
# 1 "/usr/include/features.h" 1 3
# 431 "/usr/include/features.h" 3
# 1 "/usr/include/features-time64.h" 1 3
# 20 "/usr/include/features-time64.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 21 "/usr/include/features-time64.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/timesize.h" 1 3
# 22 "/usr/include/features-time64.h" 2 3
# 432 "/usr/include/features.h" 2 3
# 539 "/usr/include/features.h" 3
# 1 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 1 3
# 730 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 731 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/long-double.h" 1 3
# 732 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 2 3
# 540 "/usr/include/features.h" 2 3
# 563 "/usr/include/features.h" 3
# 1 "/usr/include/aarch64-linux-gnu/gnu/stubs.h" 1 3




# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 6 "/usr/include/aarch64-linux-gnu/gnu/stubs.h" 2 3


# 1 "/usr/include/aarch64-linux-gnu/gnu/stubs-lp64.h" 1 3
# 9 "/usr/include/aarch64-linux-gnu/gnu/stubs.h" 2 3
# 564 "/usr/include/features.h" 2 3
# 40 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/os_defines.h" 2 3
# 734 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 2 3


# 1 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/cpu_defines.h" 1 3
# 737 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 2 3
# 893 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
namespace __gnu_cxx
{
  typedef __decltype(0.0bf16) __bfloat16_t;
}
# 962 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 3
# 1 "/usr/local/include/c++/16.1.0/pstl/pstl_config.h" 1 3
# 963 "/usr/local/include/c++/16.1.0/aarch64-unknown-linux-gnu/bits/c++config.h" 2 3



#pragma GCC diagnostic pop
# 41 "/usr/local/include/c++/16.1.0/cstdint" 2 3






# 1 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stdint.h" 1 3
# 9 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stdint.h" 3
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
# 1 "/usr/include/stdint.h" 1 3
# 26 "/usr/include/stdint.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/libc-header-start.h" 1 3
# 27 "/usr/include/stdint.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/types.h" 1 3
# 27 "/usr/include/aarch64-linux-gnu/bits/types.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 28 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/timesize.h" 1 3
# 29 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3


typedef unsigned char __u_char;
typedef unsigned short int __u_short;
typedef unsigned int __u_int;
typedef unsigned long int __u_long;


typedef signed char __int8_t;
typedef unsigned char __uint8_t;
typedef signed short int __int16_t;
typedef unsigned short int __uint16_t;
typedef signed int __int32_t;
typedef unsigned int __uint32_t;

typedef signed long int __int64_t;
typedef unsigned long int __uint64_t;






typedef __int8_t __int_least8_t;
typedef __uint8_t __uint_least8_t;
typedef __int16_t __int_least16_t;
typedef __uint16_t __uint_least16_t;
typedef __int32_t __int_least32_t;
typedef __uint32_t __uint_least32_t;
typedef __int64_t __int_least64_t;
typedef __uint64_t __uint_least64_t;



typedef long int __quad_t;
typedef unsigned long int __u_quad_t;







typedef long int __intmax_t;
typedef unsigned long int __uintmax_t;
# 141 "/usr/include/aarch64-linux-gnu/bits/types.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/typesizes.h" 1 3
# 142 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/time64.h" 1 3
# 143 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3


typedef unsigned long int __dev_t;
typedef unsigned int __uid_t;
typedef unsigned int __gid_t;
typedef unsigned long int __ino_t;
typedef unsigned long int __ino64_t;
typedef unsigned int __mode_t;
typedef unsigned int __nlink_t;
typedef long int __off_t;
typedef long int __off64_t;
typedef int __pid_t;
typedef struct { int __val[2]; } __fsid_t;
typedef long int __clock_t;
typedef unsigned long int __rlim_t;
typedef unsigned long int __rlim64_t;
typedef unsigned int __id_t;
typedef long int __time_t;
typedef unsigned int __useconds_t;
typedef long int __suseconds_t;
typedef long int __suseconds64_t;

typedef int __daddr_t;
typedef int __key_t;


typedef int __clockid_t;


typedef void * __timer_t;


typedef int __blksize_t;




typedef long int __blkcnt_t;
typedef long int __blkcnt64_t;


typedef unsigned long int __fsblkcnt_t;
typedef unsigned long int __fsblkcnt64_t;


typedef unsigned long int __fsfilcnt_t;
typedef unsigned long int __fsfilcnt64_t;


typedef long int __fsword_t;

typedef long int __ssize_t;


typedef long int __syscall_slong_t;

typedef unsigned long int __syscall_ulong_t;



typedef __off64_t __loff_t;
typedef char *__caddr_t;


typedef long int __intptr_t;


typedef unsigned int __socklen_t;




typedef int __sig_atomic_t;
# 28 "/usr/include/stdint.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wchar.h" 1 3
# 29 "/usr/include/stdint.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 30 "/usr/include/stdint.h" 2 3
# 38 "/usr/include/stdint.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/stdint-intn.h" 1 3
# 24 "/usr/include/aarch64-linux-gnu/bits/stdint-intn.h" 3
typedef __int8_t int8_t;
typedef __int16_t int16_t;
typedef __int32_t int32_t;
typedef __int64_t int64_t;
# 39 "/usr/include/stdint.h" 2 3


# 1 "/usr/include/aarch64-linux-gnu/bits/stdint-uintn.h" 1 3
# 24 "/usr/include/aarch64-linux-gnu/bits/stdint-uintn.h" 3
typedef __uint8_t uint8_t;
typedef __uint16_t uint16_t;
typedef __uint32_t uint32_t;
typedef __uint64_t uint64_t;
# 42 "/usr/include/stdint.h" 2 3



# 1 "/usr/include/aarch64-linux-gnu/bits/stdint-least.h" 1 3
# 25 "/usr/include/aarch64-linux-gnu/bits/stdint-least.h" 3
typedef __int_least8_t int_least8_t;
typedef __int_least16_t int_least16_t;
typedef __int_least32_t int_least32_t;
typedef __int_least64_t int_least64_t;


typedef __uint_least8_t uint_least8_t;
typedef __uint_least16_t uint_least16_t;
typedef __uint_least32_t uint_least32_t;
typedef __uint_least64_t uint_least64_t;
# 46 "/usr/include/stdint.h" 2 3





typedef signed char int_fast8_t;

typedef long int int_fast16_t;
typedef long int int_fast32_t;
typedef long int int_fast64_t;
# 64 "/usr/include/stdint.h" 3
typedef unsigned char uint_fast8_t;

typedef unsigned long int uint_fast16_t;
typedef unsigned long int uint_fast32_t;
typedef unsigned long int uint_fast64_t;
# 80 "/usr/include/stdint.h" 3
typedef long int intptr_t;


typedef unsigned long int uintptr_t;
# 94 "/usr/include/stdint.h" 3
typedef __intmax_t intmax_t;
typedef __uintmax_t uintmax_t;
# 12 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stdint.h" 2 3
#pragma GCC diagnostic pop
# 48 "/usr/local/include/c++/16.1.0/cstdint" 2 3


namespace std
{

  using ::int8_t;
  using ::int16_t;
  using ::int32_t;
  using ::int64_t;

  using ::int_fast8_t;
  using ::int_fast16_t;
  using ::int_fast32_t;
  using ::int_fast64_t;

  using ::int_least8_t;
  using ::int_least16_t;
  using ::int_least32_t;
  using ::int_least64_t;

  using ::intmax_t;
  using ::intptr_t;

  using ::uint8_t;
  using ::uint16_t;
  using ::uint32_t;
  using ::uint64_t;

  using ::uint_fast8_t;
  using ::uint_fast16_t;
  using ::uint_fast32_t;
  using ::uint_fast64_t;

  using ::uint_least8_t;
  using ::uint_least16_t;
  using ::uint_least32_t;
  using ::uint_least64_t;

  using ::uintmax_t;
  using ::uintptr_t;
# 144 "/usr/local/include/c++/16.1.0/cstdint" 3
}
# 3 "/home/dev/code/aura/src/compiler/type_checker.ixx" 2


# 4 "/home/dev/code/aura/src/compiler/type_checker.ixx"
export module aura.compiler.type_checker;

import std;
import aura.core;
import aura.core.type;
import aura.diag;

namespace aura::compiler {


export class TypeEnv {
    aura::core::TypeRegistry& reg_;
    struct Binding {
        aura::core::TypeId type;
        bool is_poly = false;
        std::vector<aura::core::TypeId> type_args;
    };
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
public:
    explicit TypeEnv(aura::core::TypeRegistry& reg);
    void push_scope();
    void pop_scope();
    void bind(std::string name, aura::core::TypeId type);
    aura::core::TypeId lookup(const std::string& name);
    bool is_bound(const std::string& name) const;
};


export struct Constraint {
    enum Kind { EQUAL, CONSISTENT };
    Kind kind;
    aura::core::TypeId lhs, rhs;
};

export class ConstraintSystem {
    aura::core::TypeRegistry& reg_;
    std::vector<Constraint> constraints_;
    std::vector<aura::core::TypeId> subst_;
    uint64_t fresh_counter_ = 0;
public:
    explicit ConstraintSystem(aura::core::TypeRegistry& reg);
    void add(Constraint c);
    bool solve();
    void clear();
    aura::core::TypeId fresh_var();
    bool consistent_unify(aura::core::TypeId t1, aura::core::TypeId t2);
    bool occurs_check(aura::core::TypeId var, aura::core::TypeId ty);
    aura::core::TypeId normalize(aura::core::TypeId id);
};


export class InferenceEngine {
    aura::core::TypeRegistry& reg_;
    aura::diag::DiagnosticCollector& diag_;
    ConstraintSystem cs_;
    TypeEnv env_;
    aura::diag::SourceLocation cur_loc_;
public:
    InferenceEngine(aura::core::TypeRegistry& reg, aura::diag::DiagnosticCollector& diag);


    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool,
                                   aura::ast::NodeId node);
    void check_flat(aura::ast::FlatAST& flat,
                    aura::ast::StringPool& pool,
                    aura::ast::NodeId id,
                    aura::core::TypeId expected);


    void init_primitive_env();

private:

    aura::core::TypeId synthesize_flat(aura::ast::FlatAST& flat,
                                        aura::ast::StringPool& pool,
                                        aura::ast::NodeId id,
                                        aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_var(aura::ast::StringPool& pool,
                                            aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_call(aura::ast::FlatAST& flat,
                                             aura::ast::StringPool& pool,
                                             aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_lambda(aura::ast::FlatAST& flat,
                                               aura::ast::StringPool& pool,
                                               aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_if(aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool,
                                           aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_let(aura::ast::FlatAST& flat,
                                            aura::ast::StringPool& pool,
                                            aura::ast::NodeView v,
                                            bool is_rec);
    aura::core::TypeId synthesize_flat_begin(aura::ast::FlatAST& flat,
                                              aura::ast::StringPool& pool,
                                              aura::ast::NodeView v);
    aura::core::TypeId synthesize_flat_annotation(aura::ast::FlatAST& flat,
                                                   aura::ast::StringPool& pool,
                                                   aura::ast::NodeView v);

    void check_flat_call(aura::ast::FlatAST& flat,
                         aura::ast::StringPool& pool,
                         aura::ast::NodeView v,
                         aura::core::TypeId expected);
    void check_flat_lambda(aura::ast::FlatAST& flat,
                           aura::ast::StringPool& pool,
                           aura::ast::NodeView v,
                           aura::core::TypeId expected);

    aura::core::TypeId lub(aura::core::TypeId a, aura::core::TypeId b);


    void register_primitive(std::string name, std::vector<aura::core::TypeId> param_types, aura::core::TypeId ret_type);


    bool is_coercible(aura::core::TypeId from, aura::core::TypeId to);
};


export struct TypeChecker {
    aura::core::TypeRegistry& types;
    explicit TypeChecker(aura::core::TypeRegistry& reg) : types(reg) {}
    aura::core::TypeId infer_flat(aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool,
                                   aura::ast::NodeId node,
                                   aura::diag::DiagnosticCollector& diag);
};

}
