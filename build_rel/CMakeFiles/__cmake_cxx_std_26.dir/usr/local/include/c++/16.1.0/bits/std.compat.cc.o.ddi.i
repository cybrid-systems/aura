# 0 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
# 24 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
module;

# 1 "/usr/local/include/c++/16.1.0/stdbit.h" 1 3
# 33 "/usr/local/include/c++/16.1.0/stdbit.h" 3
# 1 "/usr/local/include/c++/16.1.0/bits/version.h" 1 3
# 51 "/usr/local/include/c++/16.1.0/bits/version.h" 3
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
# 52 "/usr/local/include/c++/16.1.0/bits/version.h" 2 3
# 34 "/usr/local/include/c++/16.1.0/stdbit.h" 2 3



# 1 "/usr/local/include/c++/16.1.0/bit" 1 3
# 38 "/usr/local/include/c++/16.1.0/bit" 3
# 1 "/usr/local/include/c++/16.1.0/concepts" 1 3
# 38 "/usr/local/include/c++/16.1.0/concepts" 3
# 1 "/usr/local/include/c++/16.1.0/bits/version.h" 1 3
# 39 "/usr/local/include/c++/16.1.0/concepts" 2 3
# 48 "/usr/local/include/c++/16.1.0/concepts" 3
# 1 "/usr/local/include/c++/16.1.0/type_traits" 1 3
# 69 "/usr/local/include/c++/16.1.0/type_traits" 3
# 1 "/usr/local/include/c++/16.1.0/bits/version.h" 1 3
# 70 "/usr/local/include/c++/16.1.0/type_traits" 2 3

extern "C++"
{
namespace std __attribute__ ((__visibility__ ("default")))
{


  template<typename _Tp>
    class reference_wrapper;
# 94 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp, _Tp __v>
    struct integral_constant
    {
      static constexpr _Tp value = __v;
      using value_type = _Tp;
      using type = integral_constant<_Tp, __v>;
      constexpr operator value_type() const noexcept { return value; }


      constexpr value_type operator()() const noexcept { return value; }

    };
# 114 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<bool __v>
    using __bool_constant = integral_constant<bool, __v>;



  using true_type = __bool_constant<true>;


  using false_type = __bool_constant<false>;




  template<bool __v>
    using bool_constant = __bool_constant<__v>;






  template<bool, typename _Tp = void>
    struct enable_if
    { };


  template<typename _Tp>
    struct enable_if<true, _Tp>
    { using type = _Tp; };


  template<bool _Cond, typename _Tp = void>
    using __enable_if_t = typename enable_if<_Cond, _Tp>::type;

  template<bool>
    struct __conditional
    {
      template<typename _Tp, typename>
 using type = _Tp;
    };

  template<>
    struct __conditional<false>
    {
      template<typename, typename _Up>
 using type = _Up;
    };


  template<bool _Cond, typename _If, typename _Else>
    using __conditional_t
      = typename __conditional<_Cond>::template type<_If, _Else>;


  template <typename _Type>
    struct __type_identity
    { using type = _Type; };

  template<typename _Tp>
    using __type_identity_t = typename __type_identity<_Tp>::type;

  namespace __detail
  {

    template<typename _Tp, typename...>
      using __first_t = _Tp;


    template<typename... _Bn>
      auto __or_fn(int) -> __first_t<false_type,
         __enable_if_t<!bool(_Bn::value)>...>;

    template<typename... _Bn>
      auto __or_fn(...) -> true_type;

    template<typename... _Bn>
      auto __and_fn(int) -> __first_t<true_type,
          __enable_if_t<bool(_Bn::value)>...>;

    template<typename... _Bn>
      auto __and_fn(...) -> false_type;
  }




  template<typename... _Bn>
    struct __or_
    : decltype(__detail::__or_fn<_Bn...>(0))
    { };

  template<typename... _Bn>
    struct __and_
    : decltype(__detail::__and_fn<_Bn...>(0))
    { };

  template<typename _Pp>
    struct __not_
    : __bool_constant<!bool(_Pp::value)>
    { };





  template<typename... _Bn>
    inline constexpr bool __or_v = __or_<_Bn...>::value;
  template<typename... _Bn>
    inline constexpr bool __and_v = __and_<_Bn...>::value;

  namespace __detail
  {
    template<typename , typename _B1, typename... _Bn>
      struct __disjunction_impl
      { using type = _B1; };

    template<typename _B1, typename _B2, typename... _Bn>
      struct __disjunction_impl<__enable_if_t<!bool(_B1::value)>, _B1, _B2, _Bn...>
      { using type = typename __disjunction_impl<void, _B2, _Bn...>::type; };

    template<typename , typename _B1, typename... _Bn>
      struct __conjunction_impl
      { using type = _B1; };

    template<typename _B1, typename _B2, typename... _Bn>
      struct __conjunction_impl<__enable_if_t<bool(_B1::value)>, _B1, _B2, _Bn...>
      { using type = typename __conjunction_impl<void, _B2, _Bn...>::type; };
  }


  template<typename... _Bn>
    struct conjunction
    : __detail::__conjunction_impl<void, _Bn...>::type
    { };

  template<>
    struct conjunction<>
    : true_type
    { };

  template<typename... _Bn>
    struct disjunction
    : __detail::__disjunction_impl<void, _Bn...>::type
    { };

  template<>
    struct disjunction<>
    : false_type
    { };

  template<typename _Pp>
    struct negation
    : __not_<_Pp>::type
    { };




  template<typename... _Bn>
    inline constexpr bool conjunction_v = conjunction<_Bn...>::value;

  template<typename... _Bn>
    inline constexpr bool disjunction_v = disjunction<_Bn...>::value;

  template<typename _Pp>
    inline constexpr bool negation_v = negation<_Pp>::value;





  template<typename>
    struct is_object;
  template<typename>
    struct remove_cv;
  template<typename>
    struct is_const;


  template<typename>
    struct __is_array_unknown_bounds;




  template<typename _Tp>
    using __maybe_complete_object_type
      = __and_<is_object<_Tp>, __not_<__is_array_unknown_bounds<_Tp>>>;





  template<typename _Tp,
    typename = __enable_if_t<__maybe_complete_object_type<_Tp>::value>,
    size_t = sizeof(_Tp)>
    constexpr true_type
    __is_complete_or_unbounded(__type_identity<_Tp>)
    { return {}; };



  template<typename _TypeIdentity,
    typename _NestedType = typename _TypeIdentity::type>
    constexpr typename __not_<__maybe_complete_object_type<_NestedType>>::type
    __is_complete_or_unbounded(_TypeIdentity)
    { return {}; }


  template<typename _Tp>
    using __remove_cv_t = typename remove_cv<_Tp>::type;





  template<typename _Tp>
    struct is_void
    : public false_type { };

  template<>
    struct is_void<void>
    : public true_type { };

  template<>
    struct is_void<const void>
    : public true_type { };

  template<>
    struct is_void<volatile void>
    : public true_type { };

  template<>
    struct is_void<const volatile void>
    : public true_type { };







  enum class _Integer_kind { _None, _Signed, _Unsigned };

  template<typename>
    struct __is_integral_helper
    : public false_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };

  template<>
    struct __is_integral_helper<bool>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };

  template<>
    struct __is_integral_helper<char>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };

  template<>
    struct __is_integral_helper<signed char>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Signed; };

  template<>
    struct __is_integral_helper<unsigned char>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Unsigned; };




  template<>
    struct __is_integral_helper<wchar_t>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };


  template<>
    struct __is_integral_helper<char8_t>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };


  template<>
    struct __is_integral_helper<char16_t>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };

  template<>
    struct __is_integral_helper<char32_t>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_None; };

  template<>
    struct __is_integral_helper<short>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Signed; };

  template<>
    struct __is_integral_helper<unsigned short>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Unsigned; };

  template<>
    struct __is_integral_helper<int>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Signed; };

  template<>
    struct __is_integral_helper<unsigned int>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Unsigned; };

  template<>
    struct __is_integral_helper<long>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Signed; };

  template<>
    struct __is_integral_helper<unsigned long>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Unsigned; };

  template<>
    struct __is_integral_helper<long long>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Signed; };

  template<>
    struct __is_integral_helper<unsigned long long>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Unsigned; };
# 504 "/usr/local/include/c++/16.1.0/type_traits" 3
  __extension__
  template<>
    struct __is_integral_helper<__int128>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Signed; };

  __extension__
  template<>
    struct __is_integral_helper<unsigned __int128>
    : public true_type
    { static constexpr auto _S_kind = _Integer_kind::_Unsigned; };



  template<typename _Tp>
    using __is_signed_integer
      = __bool_constant<__is_integral_helper<_Tp>::_S_kind
     == _Integer_kind::_Signed>;


  template<typename _Tp>
    using __is_unsigned_integer
      = __bool_constant<__is_integral_helper<_Tp>::_S_kind
     == _Integer_kind::_Unsigned>;



  template<typename _Tp>
    using __is_signed_or_unsigned_integer
      = __bool_constant<__is_integral_helper<_Tp>::_S_kind
     != _Integer_kind::_None>;




  template<typename _Tp>
    struct is_integral
    : public __is_integral_helper<__remove_cv_t<_Tp>>::type
    { };


  template<typename>
    struct __is_floating_point_helper
    : public false_type { };

  template<>
    struct __is_floating_point_helper<float>
    : public true_type { };

  template<>
    struct __is_floating_point_helper<double>
    : public true_type { };

  template<>
    struct __is_floating_point_helper<long double>
    : public true_type { };


  template<>
    struct __is_floating_point_helper<_Float16>
    : public true_type { };



  template<>
    struct __is_floating_point_helper<_Float32>
    : public true_type { };



  template<>
    struct __is_floating_point_helper<_Float64>
    : public true_type { };



  template<>
    struct __is_floating_point_helper<_Float128>
    : public true_type { };



  template<>
    struct __is_floating_point_helper<__gnu_cxx::__bfloat16_t>
    : public true_type { };
# 599 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_floating_point
    : public __is_floating_point_helper<__remove_cv_t<_Tp>>::type
    { };



  template<typename _Tp>
    struct is_array
    : public __bool_constant<__is_array(_Tp)>
    { };
# 626 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_pointer
    : public __bool_constant<__is_pointer(_Tp)>
    { };
# 653 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename>
    struct is_lvalue_reference
    : public false_type { };

  template<typename _Tp>
    struct is_lvalue_reference<_Tp&>
    : public true_type { };


  template<typename>
    struct is_rvalue_reference
    : public false_type { };

  template<typename _Tp>
    struct is_rvalue_reference<_Tp&&>
    : public true_type { };



  template<typename _Tp>
    struct is_member_object_pointer
    : public __bool_constant<__is_member_object_pointer(_Tp)>
    { };
# 697 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_member_function_pointer
    : public __bool_constant<__is_member_function_pointer(_Tp)>
    { };
# 721 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_enum
    : public __bool_constant<__is_enum(_Tp)>
    { };


  template<typename _Tp>
    struct is_union
    : public __bool_constant<__is_union(_Tp)>
    { };


  template<typename _Tp>
    struct is_class
    : public __bool_constant<__is_class(_Tp)>
    { };



  template<typename _Tp>
    struct is_function
    : public __bool_constant<__is_function(_Tp)>
    { };
# 783 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_null_pointer
    : public false_type { };

  template<>
    struct is_null_pointer<std::nullptr_t>
    : public true_type { };

  template<>
    struct is_null_pointer<const std::nullptr_t>
    : public true_type { };

  template<>
    struct is_null_pointer<volatile std::nullptr_t>
    : public true_type { };

  template<>
    struct is_null_pointer<const volatile std::nullptr_t>
    : public true_type { };



  template<typename _Tp>
    struct __is_nullptr_t
    : public is_null_pointer<_Tp>
    { } __attribute__ ((__deprecated__ ("use '" "std::is_null_pointer" "' instead")));






  template<typename _Tp>
    struct is_reference
    : public __bool_constant<__is_reference(_Tp)>
    { };
# 837 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_arithmetic
    : public __or_<is_integral<_Tp>, is_floating_point<_Tp>>::type
    { };


  template<typename _Tp>
    struct is_fundamental
    : public __or_<is_arithmetic<_Tp>, is_void<_Tp>,
     is_null_pointer<_Tp>



     >::type
    { };



  template<typename _Tp>
    struct is_object
    : public __bool_constant<__is_object(_Tp)>
    { };
# 867 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename>
    struct is_member_pointer;


  template<typename _Tp>
    struct is_scalar
    : public __or_<is_arithmetic<_Tp>, is_enum<_Tp>, is_pointer<_Tp>,
                   is_member_pointer<_Tp>, is_null_pointer<_Tp>>::type
    { };


  template<typename _Tp>
    struct is_compound
    : public __bool_constant<!is_fundamental<_Tp>::value> { };



  template<typename _Tp>
    struct is_member_pointer
    : public __bool_constant<__is_member_pointer(_Tp)>
    { };
# 905 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename, typename>
    struct is_same;


  template<typename _Tp, typename... _Types>
    using __is_one_of = __or_<is_same<_Tp, _Types>...>;


  template<typename...> using __void_t = void;






  template<typename _Tp>
    struct is_const
    : public __bool_constant<__is_const(_Tp)>
    { };
# 936 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_volatile
    : public __bool_constant<__is_volatile(_Tp)>
    { };
# 957 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct
    __attribute__ ((__deprecated__ ("use '" "is_trivially_default_constructible && is_trivially_copyable" "' instead")))
    is_trivial
    : public __bool_constant<__is_trivial(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_trivially_copyable
    : public __bool_constant<__is_trivially_copyable(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_standard_layout
    : public __bool_constant<__is_standard_layout(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };






  template<typename _Tp>
    struct
    __attribute__ ((__deprecated__ ("use '" "is_standard_layout && is_trivial" "' instead")))
    is_pod
    : public __bool_constant<__is_pod(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };





  template<typename _Tp>
    struct
    [[__deprecated__]]
    is_literal_type
    : public __bool_constant<__is_literal_type(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_empty
    : public __bool_constant<__is_empty(_Tp)>
    { };


  template<typename _Tp>
    struct is_polymorphic
    : public __bool_constant<__is_polymorphic(_Tp)>
    { };




  template<typename _Tp>
    struct is_final
    : public __bool_constant<__is_final(_Tp)>
    { };



  template<typename _Tp>
    struct is_abstract
    : public __bool_constant<__is_abstract(_Tp)>
    { };


  template<typename _Tp,
    bool = is_arithmetic<_Tp>::value>
    struct __is_signed_helper
    : public false_type { };

  template<typename _Tp>
    struct __is_signed_helper<_Tp, true>
    : public __bool_constant<_Tp(-1) < _Tp(0)>
    { };



  template<typename _Tp>
    struct is_signed
    : public __is_signed_helper<_Tp>::type
    { };


  template<typename _Tp>
    struct is_unsigned
    : public __and_<is_arithmetic<_Tp>, __not_<is_signed<_Tp>>>::type
    { };


  template<typename _Tp, typename _Up = _Tp&&>
    _Up
    __declval(int);

  template<typename _Tp>
    _Tp
    __declval(long);


  template<typename _Tp>
    auto declval() noexcept -> decltype(__declval<_Tp>(0));

  template<typename>
    struct remove_all_extents;


  template<typename _Tp>
    struct __is_array_known_bounds
    : public false_type
    { };

  template<typename _Tp, size_t _Size>
    struct __is_array_known_bounds<_Tp[_Size]>
    : public true_type
    { };

  template<typename _Tp>
    struct __is_array_unknown_bounds
    : public false_type
    { };

  template<typename _Tp>
    struct __is_array_unknown_bounds<_Tp[]>
    : public true_type
    { };






  template<typename _Tp>
    struct is_destructible
    : public __bool_constant<__is_destructible(_Tp)>
    { };
# 1169 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_nothrow_destructible
    : public __bool_constant<__is_nothrow_destructible(_Tp)>
    { };
# 1230 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp, typename... _Args>
    using __is_constructible_impl
      = __bool_constant<__is_constructible(_Tp, _Args...)>;



  template<typename _Tp, typename... _Args>
    struct is_constructible
      : public __is_constructible_impl<_Tp, _Args...>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_default_constructible
    : public __is_constructible_impl<_Tp>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp>
    using __add_lval_ref_t = __add_lvalue_reference(_Tp);
# 1272 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_copy_constructible
    : public __is_constructible_impl<_Tp, __add_lval_ref_t<const _Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp>
    using __add_rval_ref_t = __add_rvalue_reference(_Tp);
# 1299 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_move_constructible
    : public __is_constructible_impl<_Tp, __add_rval_ref_t<_Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp, typename... _Args>
    using __is_nothrow_constructible_impl
      = __bool_constant<__is_nothrow_constructible(_Tp, _Args...)>;



  template<typename _Tp, typename... _Args>
    struct is_nothrow_constructible
    : public __is_nothrow_constructible_impl<_Tp, _Args...>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_nothrow_default_constructible
    : public __is_nothrow_constructible_impl<_Tp>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_nothrow_copy_constructible
    : public __is_nothrow_constructible_impl<_Tp, __add_lval_ref_t<const _Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_nothrow_move_constructible
    : public __is_nothrow_constructible_impl<_Tp, __add_rval_ref_t<_Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp, typename _Up>
    using __is_assignable_impl = __bool_constant<__is_assignable(_Tp, _Up)>;



  template<typename _Tp, typename _Up>
    struct is_assignable
    : public __is_assignable_impl<_Tp, _Up>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_copy_assignable
    : public __is_assignable_impl<__add_lval_ref_t<_Tp>,
      __add_lval_ref_t<const _Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_move_assignable
    : public __is_assignable_impl<__add_lval_ref_t<_Tp>, __add_rval_ref_t<_Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp, typename _Up>
    using __is_nothrow_assignable_impl
      = __bool_constant<__is_nothrow_assignable(_Tp, _Up)>;



  template<typename _Tp, typename _Up>
    struct is_nothrow_assignable
    : public __is_nothrow_assignable_impl<_Tp, _Up>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_nothrow_copy_assignable
    : public __is_nothrow_assignable_impl<__add_lval_ref_t<_Tp>,
       __add_lval_ref_t<const _Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_nothrow_move_assignable
    : public __is_nothrow_assignable_impl<__add_lval_ref_t<_Tp>,
       __add_rval_ref_t<_Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp, typename... _Args>
    using __is_trivially_constructible_impl
      = __bool_constant<__is_trivially_constructible(_Tp, _Args...)>;



  template<typename _Tp, typename... _Args>
    struct is_trivially_constructible
    : public __is_trivially_constructible_impl<_Tp, _Args...>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_trivially_default_constructible
    : public __is_trivially_constructible_impl<_Tp>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    constexpr bool __is_implicitly_default_constructible_v
      = requires (void(&__f)(_Tp)) { __f({}); };

  template<typename _Tp>
    struct __is_implicitly_default_constructible
    : __bool_constant<__is_implicitly_default_constructible_v<_Tp>>
    { };
# 1483 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_trivially_copy_constructible
    : public __is_trivially_constructible_impl<_Tp, __add_lval_ref_t<const _Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_trivially_move_constructible
    : public __is_trivially_constructible_impl<_Tp, __add_rval_ref_t<_Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp, typename _Up>
    using __is_trivially_assignable_impl
      = __bool_constant<__is_trivially_assignable(_Tp, _Up)>;



  template<typename _Tp, typename _Up>
    struct is_trivially_assignable
    : public __is_trivially_assignable_impl<_Tp, _Up>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_trivially_copy_assignable
    : public __is_trivially_assignable_impl<__add_lval_ref_t<_Tp>,
         __add_lval_ref_t<const _Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_trivially_move_assignable
    : public __is_trivially_assignable_impl<__add_lval_ref_t<_Tp>,
         __add_rval_ref_t<_Tp>>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp>
    struct is_trivially_destructible
    : public __bool_constant<__is_trivially_destructible(_Tp)>
    { };
# 1554 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct has_virtual_destructor
    : public __bool_constant<__has_virtual_destructor(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };





  template<typename _Tp>
    struct alignment_of
    : public integral_constant<std::size_t, alignof(_Tp)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };




  template<typename _Tp>
    struct rank
    : public integral_constant<std::size_t, __array_rank(_Tp)> { };
# 1595 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename, unsigned _Uint = 0>
    struct extent
    : public integral_constant<size_t, 0> { };

  template<typename _Tp, size_t _Size>
    struct extent<_Tp[_Size], 0>
    : public integral_constant<size_t, _Size> { };

  template<typename _Tp, unsigned _Uint, size_t _Size>
    struct extent<_Tp[_Size], _Uint>
    : public extent<_Tp, _Uint - 1>::type { };

  template<typename _Tp>
    struct extent<_Tp[], 0>
    : public integral_constant<size_t, 0> { };

  template<typename _Tp, unsigned _Uint>
    struct extent<_Tp[], _Uint>
    : public extent<_Tp, _Uint - 1>::type { };






  template<typename _Tp, typename _Up>
    struct is_same
    : public __bool_constant<__is_same(_Tp, _Up)>
    { };
# 1637 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Base, typename _Derived>
    struct is_base_of
    : public __bool_constant<__is_base_of(_Base, _Derived)>
    { };




  template<typename _Base, typename _Derived>
    struct is_virtual_base_of
    : public bool_constant<__builtin_is_virtual_base_of(_Base, _Derived)>
    { };



  template<typename _From, typename _To>
    struct is_convertible
    : public __bool_constant<__is_convertible(_From, _To)>
    { };
# 1695 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _ToElementType, typename _FromElementType>
    using __is_array_convertible
      = is_convertible<_FromElementType(*)[], _ToElementType(*)[]>;





  template<typename _From, typename _To>
    inline constexpr bool is_nothrow_convertible_v
      = __is_nothrow_convertible(_From, _To);


  template<typename _From, typename _To>
    struct is_nothrow_convertible
    : public bool_constant<is_nothrow_convertible_v<_From, _To>>
    { };
# 1755 "/usr/local/include/c++/16.1.0/type_traits" 3
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++14-extensions"
  template<typename _Tp, typename... _Args>
    struct __is_nothrow_new_constructible_impl
    : __bool_constant<
 noexcept(::new(std::declval<void*>()) _Tp(std::declval<_Args>()...))
      >
    { };

  template<typename _Tp, typename... _Args>
    inline constexpr bool __is_nothrow_new_constructible
      = __and_<is_constructible<_Tp, _Args...>,
        __is_nothrow_new_constructible_impl<_Tp, _Args...>>::value;
#pragma GCC diagnostic pop




  template<typename _Tp>
    struct remove_const
    { using type = _Tp; };

  template<typename _Tp>
    struct remove_const<_Tp const>
    { using type = _Tp; };


  template<typename _Tp>
    struct remove_volatile
    { using type = _Tp; };

  template<typename _Tp>
    struct remove_volatile<_Tp volatile>
    { using type = _Tp; };



  template<typename _Tp>
    struct remove_cv
    { using type = __remove_cv(_Tp); };
# 1814 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct add_const
    { using type = _Tp const; };


  template<typename _Tp>
    struct add_volatile
    { using type = _Tp volatile; };


  template<typename _Tp>
    struct add_cv
    { using type = _Tp const volatile; };



  template<typename _Tp>
    using remove_const_t = typename remove_const<_Tp>::type;


  template<typename _Tp>
    using remove_volatile_t = typename remove_volatile<_Tp>::type;


  template<typename _Tp>
    using remove_cv_t = typename remove_cv<_Tp>::type;


  template<typename _Tp>
    using add_const_t = typename add_const<_Tp>::type;


  template<typename _Tp>
    using add_volatile_t = typename add_volatile<_Tp>::type;


  template<typename _Tp>
    using add_cv_t = typename add_cv<_Tp>::type;






  template<typename _Tp>
    struct remove_reference
    { using type = __remove_reference(_Tp); };
# 1876 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct add_lvalue_reference
    { using type = __add_lval_ref_t<_Tp>; };


  template<typename _Tp>
    struct add_rvalue_reference
    { using type = __add_rval_ref_t<_Tp>; };



  template<typename _Tp>
    using remove_reference_t = typename remove_reference<_Tp>::type;


  template<typename _Tp>
    using add_lvalue_reference_t = typename add_lvalue_reference<_Tp>::type;


  template<typename _Tp>
    using add_rvalue_reference_t = typename add_rvalue_reference<_Tp>::type;







  template<typename _Unqualified, bool _IsConst, bool _IsVol>
    struct __cv_selector;

  template<typename _Unqualified>
    struct __cv_selector<_Unqualified, false, false>
    { using __type = _Unqualified; };

  template<typename _Unqualified>
    struct __cv_selector<_Unqualified, false, true>
    { using __type = volatile _Unqualified; };

  template<typename _Unqualified>
    struct __cv_selector<_Unqualified, true, false>
    { using __type = const _Unqualified; };

  template<typename _Unqualified>
    struct __cv_selector<_Unqualified, true, true>
    { using __type = const volatile _Unqualified; };

  template<typename _Qualified, typename _Unqualified,
    bool _IsConst = is_const<_Qualified>::value,
    bool _IsVol = is_volatile<_Qualified>::value>
    class __match_cv_qualifiers
    {
      using __match = __cv_selector<_Unqualified, _IsConst, _IsVol>;

    public:
      using __type = typename __match::__type;
    };


  template<typename _Tp>
    struct __make_unsigned
    { using __type = _Tp; };

  template<>
    struct __make_unsigned<char>
    { using __type = unsigned char; };

  template<>
    struct __make_unsigned<signed char>
    { using __type = unsigned char; };

  template<>
    struct __make_unsigned<short>
    { using __type = unsigned short; };

  template<>
    struct __make_unsigned<int>
    { using __type = unsigned int; };

  template<>
    struct __make_unsigned<long>
    { using __type = unsigned long; };

  template<>
    struct __make_unsigned<long long>
    { using __type = unsigned long long; };
# 1988 "/usr/local/include/c++/16.1.0/type_traits" 3
  __extension__
  template<>
    struct __make_unsigned<__int128>
    { using __type = unsigned __int128; };



  template<typename _Tp,
    bool _IsInt = is_integral<_Tp>::value,
    bool _IsEnum = __is_enum(_Tp)>
    class __make_unsigned_selector;

  template<typename _Tp>
    class __make_unsigned_selector<_Tp, true, false>
    {
      using __unsigned_type
 = typename __make_unsigned<__remove_cv_t<_Tp>>::__type;

    public:
      using __type
 = typename __match_cv_qualifiers<_Tp, __unsigned_type>::__type;
    };

  class __make_unsigned_selector_base
  {
  protected:
    template<typename...> struct _List { };

    template<typename _Tp, typename... _Up>
      struct _List<_Tp, _Up...> : _List<_Up...>
      { static constexpr size_t __size = sizeof(_Tp); };

    template<size_t _Sz, typename _Tp, bool = (_Sz <= _Tp::__size)>
      struct __select;

    template<size_t _Sz, typename _Uint, typename... _UInts>
      struct __select<_Sz, _List<_Uint, _UInts...>, true>
      { using __type = _Uint; };

    template<size_t _Sz, typename _Uint, typename... _UInts>
      struct __select<_Sz, _List<_Uint, _UInts...>, false>
      : __select<_Sz, _List<_UInts...>>
      { };
  };


  template<typename _Tp>
    class __make_unsigned_selector<_Tp, false, true>
    : __make_unsigned_selector_base
    {

      __extension__
      using _UInts = _List<unsigned char, unsigned short, unsigned int,
      unsigned long, unsigned long long

      , unsigned __int128

     >;

      using __unsigned_type = typename __select<sizeof(_Tp), _UInts>::__type;

    public:
      using __type
 = typename __match_cv_qualifiers<_Tp, __unsigned_type>::__type;
    };





  template<>
    struct __make_unsigned<wchar_t>
    {
      using __type
 = typename __make_unsigned_selector<wchar_t, false, true>::__type;
    };


  template<>
    struct __make_unsigned<char8_t>
    {
      using __type
 = typename __make_unsigned_selector<char8_t, false, true>::__type;
    };


  template<>
    struct __make_unsigned<char16_t>
    {
      using __type
 = typename __make_unsigned_selector<char16_t, false, true>::__type;
    };

  template<>
    struct __make_unsigned<char32_t>
    {
      using __type
 = typename __make_unsigned_selector<char32_t, false, true>::__type;
    };






  template<typename _Tp>
    struct make_unsigned
    { using type = typename __make_unsigned_selector<_Tp>::__type; };


  template<> struct make_unsigned<bool>;
  template<> struct make_unsigned<bool const>;
  template<> struct make_unsigned<bool volatile>;
  template<> struct make_unsigned<bool const volatile>;




  template<typename _Tp>
    struct __make_signed
    { using __type = _Tp; };

  template<>
    struct __make_signed<char>
    { using __type = signed char; };

  template<>
    struct __make_signed<unsigned char>
    { using __type = signed char; };

  template<>
    struct __make_signed<unsigned short>
    { using __type = signed short; };

  template<>
    struct __make_signed<unsigned int>
    { using __type = signed int; };

  template<>
    struct __make_signed<unsigned long>
    { using __type = signed long; };

  template<>
    struct __make_signed<unsigned long long>
    { using __type = signed long long; };
# 2159 "/usr/local/include/c++/16.1.0/type_traits" 3
  __extension__
  template<>
    struct __make_signed<unsigned __int128>
    { using __type = __int128; };



  template<typename _Tp,
    bool _IsInt = is_integral<_Tp>::value,
    bool _IsEnum = __is_enum(_Tp)>
    class __make_signed_selector;

  template<typename _Tp>
    class __make_signed_selector<_Tp, true, false>
    {
      using __signed_type
 = typename __make_signed<__remove_cv_t<_Tp>>::__type;

    public:
      using __type
 = typename __match_cv_qualifiers<_Tp, __signed_type>::__type;
    };


  template<typename _Tp>
    class __make_signed_selector<_Tp, false, true>
    {
      using __unsigned_type = typename __make_unsigned_selector<_Tp>::__type;

    public:
      using __type = typename __make_signed_selector<__unsigned_type>::__type;
    };





  template<>
    struct __make_signed<wchar_t>
    {
      using __type
 = typename __make_signed_selector<wchar_t, false, true>::__type;
    };


  template<>
    struct __make_signed<char8_t>
    {
      using __type
 = typename __make_signed_selector<char8_t, false, true>::__type;
    };


  template<>
    struct __make_signed<char16_t>
    {
      using __type
 = typename __make_signed_selector<char16_t, false, true>::__type;
    };

  template<>
    struct __make_signed<char32_t>
    {
      using __type
 = typename __make_signed_selector<char32_t, false, true>::__type;
    };






  template<typename _Tp>
    struct make_signed
    { using type = typename __make_signed_selector<_Tp>::__type; };


  template<> struct make_signed<bool>;
  template<> struct make_signed<bool const>;
  template<> struct make_signed<bool volatile>;
  template<> struct make_signed<bool const volatile>;



  template<typename _Tp>
    using make_signed_t = typename make_signed<_Tp>::type;


  template<typename _Tp>
    using make_unsigned_t = typename make_unsigned<_Tp>::type;






  template<typename _Tp>
    struct remove_extent
    { using type = __remove_extent(_Tp); };
# 2274 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct remove_all_extents
    { using type = __remove_all_extents(_Tp); };
# 2293 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    using remove_extent_t = typename remove_extent<_Tp>::type;


  template<typename _Tp>
    using remove_all_extents_t = typename remove_all_extents<_Tp>::type;






  template<typename _Tp>
    struct remove_pointer
    { using type = __remove_pointer(_Tp); };
# 2325 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct add_pointer
    { using type = __add_pointer(_Tp); };
# 2353 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    using remove_pointer_t = typename remove_pointer<_Tp>::type;


  template<typename _Tp>
    using add_pointer_t = typename add_pointer<_Tp>::type;





  struct __attribute__((__aligned__)) __aligned_storage_max_align_t
  { };

  constexpr size_t
  __aligned_storage_default_alignment([[__maybe_unused__]] size_t __len)
  {
# 2384 "/usr/local/include/c++/16.1.0/type_traits" 3
    return alignof(__aligned_storage_max_align_t);

  }
# 2420 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<size_t _Len,
    size_t _Align = __aligned_storage_default_alignment(_Len)>
    struct
    [[__deprecated__]]
    aligned_storage
    {
      struct type
      {
 alignas(_Align) unsigned char __data[_Len];
      };
    };

  template <typename... _Types>
    struct __strictest_alignment
    {
      static const size_t _S_alignment = 0;
      static const size_t _S_size = 0;
    };

  template <typename _Tp, typename... _Types>
    struct __strictest_alignment<_Tp, _Types...>
    {
      static const size_t _S_alignment =
        alignof(_Tp) > __strictest_alignment<_Types...>::_S_alignment
 ? alignof(_Tp) : __strictest_alignment<_Types...>::_S_alignment;
      static const size_t _S_size =
        sizeof(_Tp) > __strictest_alignment<_Types...>::_S_size
 ? sizeof(_Tp) : __strictest_alignment<_Types...>::_S_size;
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
# 2465 "/usr/local/include/c++/16.1.0/type_traits" 3
  template <size_t _Len, typename... _Types>
    struct
    [[__deprecated__]]
    aligned_union
    {
    private:
      static_assert(sizeof...(_Types) != 0, "At least one type is required");

      using __strictest = __strictest_alignment<_Types...>;
      static const size_t _S_len = _Len > __strictest::_S_size
 ? _Len : __strictest::_S_size;
    public:

      static const size_t alignment_value = __strictest::_S_alignment;

      using type = typename aligned_storage<_S_len, alignment_value>::type;
    };

  template <size_t _Len, typename... _Types>
    const size_t aligned_union<_Len, _Types...>::alignment_value;
#pragma GCC diagnostic pop




  template<typename _Tp>
    struct decay
    { using type = __decay(_Tp); };
# 2530 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct __strip_reference_wrapper
    {
      using __type = _Tp;
    };

  template<typename _Tp>
    struct __strip_reference_wrapper<reference_wrapper<_Tp> >
    {
      using __type = _Tp&;
    };


  template<typename _Tp>
    using __decay_t = typename decay<_Tp>::type;

  template<typename _Tp>
    using __decay_and_strip = __strip_reference_wrapper<__decay_t<_Tp>>;





  template<typename... _Cond>
    using _Require = __enable_if_t<__and_<_Cond...>::value>;


  template<typename _Tp>
    using __remove_cvref_t
     = typename remove_cv<typename remove_reference<_Tp>::type>::type;




  template<bool _Cond, typename _Iftrue, typename _Iffalse>
    struct conditional
    { using type = _Iftrue; };


  template<typename _Iftrue, typename _Iffalse>
    struct conditional<false, _Iftrue, _Iffalse>
    { using type = _Iffalse; };


  template<typename... _Tp>
    struct common_type;
# 2586 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct __success_type
    { using type = _Tp; };

  struct __failure_type
  { };

  struct __do_common_type_impl
  {
    template<typename _Tp, typename _Up>
      using __cond_t
 = decltype(true ? std::declval<_Tp>() : std::declval<_Up>());



    template<typename _Tp, typename _Up>
      static __success_type<__decay_t<__cond_t<_Tp, _Up>>>
      _S_test(int);




    template<typename _Tp, typename _Up>
      static __success_type<__remove_cvref_t<__cond_t<const _Tp&, const _Up&>>>
      _S_test_2(int);


    template<typename, typename>
      static __failure_type
      _S_test_2(...);

    template<typename _Tp, typename _Up>
      static decltype(_S_test_2<_Tp, _Up>(0))
      _S_test(...);
  };


  template<>
    struct common_type<>
    { };


  template<typename _Tp0>
    struct common_type<_Tp0>
    : public common_type<_Tp0, _Tp0>
    { };


  template<typename _Tp1, typename _Tp2,
    typename _Dp1 = __decay_t<_Tp1>, typename _Dp2 = __decay_t<_Tp2>>
    struct __common_type_impl
    {


      using type = common_type<_Dp1, _Dp2>;
    };

  template<typename _Tp1, typename _Tp2>
    struct __common_type_impl<_Tp1, _Tp2, _Tp1, _Tp2>
    : private __do_common_type_impl
    {


      using type = decltype(_S_test<_Tp1, _Tp2>(0));
    };


  template<typename _Tp1, typename _Tp2>
    struct common_type<_Tp1, _Tp2>
    : public __common_type_impl<_Tp1, _Tp2>::type
    { };

  template<typename...>
    struct __common_type_pack
    { };

  template<typename, typename, typename = void>
    struct __common_type_fold;


  template<typename _Tp1, typename _Tp2, typename... _Rp>
    struct common_type<_Tp1, _Tp2, _Rp...>
    : public __common_type_fold<common_type<_Tp1, _Tp2>,
    __common_type_pack<_Rp...>>
    { };




  template<typename _CTp, typename... _Rp>
    struct __common_type_fold<_CTp, __common_type_pack<_Rp...>,
         __void_t<typename _CTp::type>>
    : public common_type<typename _CTp::type, _Rp...>
    { };


  template<typename _CTp, typename _Rp>
    struct __common_type_fold<_CTp, _Rp, void>
    { };

  template<typename _Tp, bool = __is_enum(_Tp)>
    struct __underlying_type_impl
    {
      using type = __underlying_type(_Tp);
    };

  template<typename _Tp>
    struct __underlying_type_impl<_Tp, false>
    { };



  template<typename _Tp>
    struct underlying_type
    : public __underlying_type_impl<_Tp>
    { };


  template<typename _Tp>
    struct __declval_protector
    {
      static const bool __stop = false;
    };






  template<typename _Tp>
    auto declval() noexcept -> decltype(__declval<_Tp>(0))
    {
      static_assert(__declval_protector<_Tp>::__stop,
      "declval() must not be used!");
      return __declval<_Tp>(0);
    }


  template<typename _Signature>
    struct result_of;




  struct __invoke_memfun_ref { };
  struct __invoke_memfun_deref { };
  struct __invoke_memobj_ref { };
  struct __invoke_memobj_deref { };
  struct __invoke_other { };


  template<typename _Tp, typename _Tag>
    struct __result_of_success : __success_type<_Tp>
    { using __invoke_type = _Tag; };


  struct __result_of_memfun_ref_impl
  {
    template<typename _Fp, typename _Tp1, typename... _Args>
      static __result_of_success<decltype(
      (std::declval<_Tp1>().*std::declval<_Fp>())(std::declval<_Args>()...)
      ), __invoke_memfun_ref> _S_test(int);

    template<typename...>
      static __failure_type _S_test(...);
  };

  template<typename _MemPtr, typename _Arg, typename... _Args>
    struct __result_of_memfun_ref
    : private __result_of_memfun_ref_impl
    {
      using type = decltype(_S_test<_MemPtr, _Arg, _Args...>(0));
    };


  struct __result_of_memfun_deref_impl
  {
    template<typename _Fp, typename _Tp1, typename... _Args>
      static __result_of_success<decltype(
      ((*std::declval<_Tp1>()).*std::declval<_Fp>())(std::declval<_Args>()...)
      ), __invoke_memfun_deref> _S_test(int);

    template<typename...>
      static __failure_type _S_test(...);
  };

  template<typename _MemPtr, typename _Arg, typename... _Args>
    struct __result_of_memfun_deref
    : private __result_of_memfun_deref_impl
    {
      using type = decltype(_S_test<_MemPtr, _Arg, _Args...>(0));
    };


  struct __result_of_memobj_ref_impl
  {
    template<typename _Fp, typename _Tp1>
      static __result_of_success<decltype(
      std::declval<_Tp1>().*std::declval<_Fp>()
      ), __invoke_memobj_ref> _S_test(int);

    template<typename, typename>
      static __failure_type _S_test(...);
  };

  template<typename _MemPtr, typename _Arg>
    struct __result_of_memobj_ref
    : private __result_of_memobj_ref_impl
    {
      using type = decltype(_S_test<_MemPtr, _Arg>(0));
    };


  struct __result_of_memobj_deref_impl
  {
    template<typename _Fp, typename _Tp1>
      static __result_of_success<decltype(
      (*std::declval<_Tp1>()).*std::declval<_Fp>()
      ), __invoke_memobj_deref> _S_test(int);

    template<typename, typename>
      static __failure_type _S_test(...);
  };

  template<typename _MemPtr, typename _Arg>
    struct __result_of_memobj_deref
    : private __result_of_memobj_deref_impl
    {
      using type = decltype(_S_test<_MemPtr, _Arg>(0));
    };

  template<typename _MemPtr, typename _Arg>
    struct __result_of_memobj;

  template<typename _Res, typename _Class, typename _Arg>
    struct __result_of_memobj<_Res _Class::*, _Arg>
    {
      using _Argval = __remove_cvref_t<_Arg>;
      using _MemPtr = _Res _Class::*;
      using type = typename __conditional_t<__or_<is_same<_Argval, _Class>,
        is_base_of<_Class, _Argval>>::value,
        __result_of_memobj_ref<_MemPtr, _Arg>,
        __result_of_memobj_deref<_MemPtr, _Arg>
      >::type;
    };

  template<typename _MemPtr, typename _Arg, typename... _Args>
    struct __result_of_memfun;

  template<typename _Res, typename _Class, typename _Arg, typename... _Args>
    struct __result_of_memfun<_Res _Class::*, _Arg, _Args...>
    {
      using _Argval = typename remove_reference<_Arg>::type;
      using _MemPtr = _Res _Class::*;
      using type = typename __conditional_t<is_base_of<_Class, _Argval>::value,
        __result_of_memfun_ref<_MemPtr, _Arg, _Args...>,
        __result_of_memfun_deref<_MemPtr, _Arg, _Args...>
      >::type;
    };






  template<typename _Tp, typename _Up = __remove_cvref_t<_Tp>>
    struct __inv_unwrap
    {
      using type = _Tp;
    };

  template<typename _Tp, typename _Up>
    struct __inv_unwrap<_Tp, reference_wrapper<_Up>>
    {
      using type = _Up&;
    };

  template<bool, bool, typename _Functor, typename... _ArgTypes>
    struct __result_of_impl
    {
      using type = __failure_type;
    };

  template<typename _MemPtr, typename _Arg>
    struct __result_of_impl<true, false, _MemPtr, _Arg>
    : public __result_of_memobj<__decay_t<_MemPtr>,
    typename __inv_unwrap<_Arg>::type>
    { };

  template<typename _MemPtr, typename _Arg, typename... _Args>
    struct __result_of_impl<false, true, _MemPtr, _Arg, _Args...>
    : public __result_of_memfun<__decay_t<_MemPtr>,
    typename __inv_unwrap<_Arg>::type, _Args...>
    { };


  struct __result_of_other_impl
  {
    template<typename _Fn, typename... _Args>
      static __result_of_success<decltype(
      std::declval<_Fn>()(std::declval<_Args>()...)
      ), __invoke_other> _S_test(int);

    template<typename...>
      static __failure_type _S_test(...);
  };

  template<typename _Functor, typename... _ArgTypes>
    struct __result_of_impl<false, false, _Functor, _ArgTypes...>
    : private __result_of_other_impl
    {
      using type = decltype(_S_test<_Functor, _ArgTypes...>(0));
    };


  template<typename _Functor, typename... _ArgTypes>
    struct __invoke_result
    : public __result_of_impl<
        is_member_object_pointer<
          typename remove_reference<_Functor>::type
        >::value,
        is_member_function_pointer<
          typename remove_reference<_Functor>::type
        >::value,
 _Functor, _ArgTypes...
      >::type
    { };


  template<typename _Fn, typename... _Args>
    using __invoke_result_t = typename __invoke_result<_Fn, _Args...>::type;


  template<typename _Functor, typename... _ArgTypes>
    struct result_of<_Functor(_ArgTypes...)>
    : public __invoke_result<_Functor, _ArgTypes...>
    { } __attribute__ ((__deprecated__ ("use '" "std::invoke_result" "' instead")));


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

  template<size_t _Len,
    size_t _Align = __aligned_storage_default_alignment(_Len)>
    using aligned_storage_t [[__deprecated__]] = typename aligned_storage<_Len, _Align>::type;

  template <size_t _Len, typename... _Types>
    using aligned_union_t [[__deprecated__]] = typename aligned_union<_Len, _Types...>::type;
#pragma GCC diagnostic pop


  template<typename _Tp>
    using decay_t = typename decay<_Tp>::type;


  template<bool _Cond, typename _Tp = void>
    using enable_if_t = typename enable_if<_Cond, _Tp>::type;


  template<bool _Cond, typename _Iftrue, typename _Iffalse>
    using conditional_t = typename conditional<_Cond, _Iftrue, _Iffalse>::type;


  template<typename... _Tp>
    using common_type_t = typename common_type<_Tp...>::type;


  template<typename _Tp>
    using underlying_type_t = typename underlying_type<_Tp>::type;


  template<typename _Tp>
    using result_of_t = typename result_of<_Tp>::type;




  template<typename...> using void_t = void;
# 2973 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Def, template<typename...> class _Op, typename... _Args>
    struct __detected_or
    {
      using type = _Def;
      using __is_detected = false_type;
    };


  template<typename _Def, template<typename...> class _Op, typename... _Args>
    requires requires { typename _Op<_Args...>; }
    struct __detected_or<_Def, _Op, _Args...>
    {
      using type = _Op<_Args...>;
      using __is_detected = true_type;
    };
# 3013 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Default, template<typename...> class _Op,
    typename... _Args>
    using __detected_or_t
      = typename __detected_or<_Default, _Op, _Args...>::type;
# 3032 "/usr/local/include/c++/16.1.0/type_traits" 3
  template <typename _Tp>
    struct __is_swappable;

  template <typename _Tp>
    struct __is_nothrow_swappable;

  template<typename>
    struct __is_tuple_like_impl : false_type
    { };


  template<typename _Tp>
    struct __is_tuple_like
    : public __is_tuple_like_impl<__remove_cvref_t<_Tp>>::type
    { };


  template<typename _Tp>
    constexpr
    inline
    _Require<__not_<__is_tuple_like<_Tp>>,
      is_move_constructible<_Tp>,
      is_move_assignable<_Tp>>
    swap(_Tp&, _Tp&)
    noexcept(__and_<is_nothrow_move_constructible<_Tp>,
             is_nothrow_move_assignable<_Tp>>::value);

  template<typename _Tp, size_t _Nm>
    constexpr
    inline
    __enable_if_t<__is_swappable<_Tp>::value>
    swap(_Tp (&__a)[_Nm], _Tp (&__b)[_Nm])
    noexcept(__is_nothrow_swappable<_Tp>::value);


  namespace __swappable_details {
    using std::swap;

    struct __do_is_swappable_impl
    {
      template<typename _Tp, typename
               = decltype(swap(std::declval<_Tp&>(), std::declval<_Tp&>()))>
        static true_type __test(int);

      template<typename>
        static false_type __test(...);
    };

    struct __do_is_nothrow_swappable_impl
    {
      template<typename _Tp>
        static __bool_constant<
          noexcept(swap(std::declval<_Tp&>(), std::declval<_Tp&>()))
        > __test(int);

      template<typename>
        static false_type __test(...);
    };

  }

  template<typename _Tp>
    struct __is_swappable_impl
    : public __swappable_details::__do_is_swappable_impl
    {
      using type = decltype(__test<_Tp>(0));
    };

  template<typename _Tp>
    struct __is_nothrow_swappable_impl
    : public __swappable_details::__do_is_nothrow_swappable_impl
    {
      using type = decltype(__test<_Tp>(0));
    };

  template<typename _Tp>
    struct __is_swappable
    : public __is_swappable_impl<_Tp>::type
    { };

  template<typename _Tp>
    struct __is_nothrow_swappable
    : public __is_nothrow_swappable_impl<_Tp>::type
    { };






  template<typename _Tp>
    struct is_swappable
    : public __is_swappable_impl<_Tp>::type
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp>
    struct is_nothrow_swappable
    : public __is_nothrow_swappable_impl<_Tp>::type
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp>
    inline constexpr bool is_swappable_v =
      is_swappable<_Tp>::value;


  template<typename _Tp>
    inline constexpr bool is_nothrow_swappable_v =
      is_nothrow_swappable<_Tp>::value;



  namespace __swappable_with_details {
    using std::swap;

    struct __do_is_swappable_with_impl
    {
      template<typename _Tp, typename _Up, typename
               = decltype(swap(std::declval<_Tp>(), std::declval<_Up>())),
               typename
               = decltype(swap(std::declval<_Up>(), std::declval<_Tp>()))>
        static true_type __test(int);

      template<typename, typename>
        static false_type __test(...);
    };

    struct __do_is_nothrow_swappable_with_impl
    {
      template<typename _Tp, typename _Up>
        static __bool_constant<
          noexcept(swap(std::declval<_Tp>(), std::declval<_Up>()))
          &&
          noexcept(swap(std::declval<_Up>(), std::declval<_Tp>()))
        > __test(int);

      template<typename, typename>
        static false_type __test(...);
    };

  }

  template<typename _Tp, typename _Up>
    struct __is_swappable_with_impl
    : public __swappable_with_details::__do_is_swappable_with_impl
    {
      using type = decltype(__test<_Tp, _Up>(0));
    };


  template<typename _Tp>
    struct __is_swappable_with_impl<_Tp&, _Tp&>
    : public __swappable_details::__do_is_swappable_impl
    {
      using type = decltype(__test<_Tp&>(0));
    };

  template<typename _Tp, typename _Up>
    struct __is_nothrow_swappable_with_impl
    : public __swappable_with_details::__do_is_nothrow_swappable_with_impl
    {
      using type = decltype(__test<_Tp, _Up>(0));
    };


  template<typename _Tp>
    struct __is_nothrow_swappable_with_impl<_Tp&, _Tp&>
    : public __swappable_details::__do_is_nothrow_swappable_impl
    {
      using type = decltype(__test<_Tp&>(0));
    };



  template<typename _Tp, typename _Up>
    struct is_swappable_with
    : public __is_swappable_with_impl<_Tp, _Up>::type
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "first template argument must be a complete class or an unbounded array");
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Up>{}),
 "second template argument must be a complete class or an unbounded array");
    };


  template<typename _Tp, typename _Up>
    struct is_nothrow_swappable_with
    : public __is_nothrow_swappable_with_impl<_Tp, _Up>::type
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "first template argument must be a complete class or an unbounded array");
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Up>{}),
 "second template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp, typename _Up>
    inline constexpr bool is_swappable_with_v =
      is_swappable_with<_Tp, _Up>::value;


  template<typename _Tp, typename _Up>
    inline constexpr bool is_nothrow_swappable_with_v =
      is_nothrow_swappable_with<_Tp, _Up>::value;
# 3254 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Result, typename _Ret,
    bool = is_void<_Ret>::value, typename = void>
    struct __is_invocable_impl
    : false_type
    {
      using __nothrow_conv = false_type;
    };


  template<typename _Result, typename _Ret>
    struct __is_invocable_impl<_Result, _Ret,
                                true,
          __void_t<typename _Result::type>>
    : true_type
    {
      using __nothrow_conv = true_type;
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wctor-dtor-privacy"

  template<typename _Result, typename _Ret>
    struct __is_invocable_impl<_Result, _Ret,
                                false,
          __void_t<typename _Result::type>>
    {
    private:

      using _Res_t = typename _Result::type;



      static _Res_t _S_get() noexcept;


      template<typename _Tp>
 static void _S_conv(__type_identity_t<_Tp>) noexcept;


      template<typename _Tp,
        bool _Nothrow = noexcept(_S_conv<_Tp>(_S_get())),
        typename = decltype(_S_conv<_Tp>(_S_get())),

        bool _Dangle = __reference_converts_from_temporary(_Tp, _Res_t)



       >
 static __bool_constant<_Nothrow && !_Dangle>
 _S_test(int);

      template<typename _Tp, bool = false>
 static false_type
 _S_test(...);

    public:

      using type = decltype(_S_test<_Ret, true>(1));


      using __nothrow_conv = decltype(_S_test<_Ret>(1));
    };
#pragma GCC diagnostic pop

  template<typename _Fn, typename... _ArgTypes>
    struct __is_invocable

    : __bool_constant<__is_invocable(_Fn, _ArgTypes...)>



    { };

  template<typename _Fn, typename _Tp, typename... _Args>
    constexpr bool __call_is_nt(__invoke_memfun_ref)
    {
      using _Up = typename __inv_unwrap<_Tp>::type;
      return noexcept((std::declval<_Up>().*std::declval<_Fn>())(
     std::declval<_Args>()...));
    }

  template<typename _Fn, typename _Tp, typename... _Args>
    constexpr bool __call_is_nt(__invoke_memfun_deref)
    {
      return noexcept(((*std::declval<_Tp>()).*std::declval<_Fn>())(
     std::declval<_Args>()...));
    }

  template<typename _Fn, typename _Tp>
    constexpr bool __call_is_nt(__invoke_memobj_ref)
    {
      using _Up = typename __inv_unwrap<_Tp>::type;
      return noexcept(std::declval<_Up>().*std::declval<_Fn>());
    }

  template<typename _Fn, typename _Tp>
    constexpr bool __call_is_nt(__invoke_memobj_deref)
    {
      return noexcept((*std::declval<_Tp>()).*std::declval<_Fn>());
    }

  template<typename _Fn, typename... _Args>
    constexpr bool __call_is_nt(__invoke_other)
    {
      return noexcept(std::declval<_Fn>()(std::declval<_Args>()...));
    }

  template<typename _Result, typename _Fn, typename... _Args>
    struct __call_is_nothrow
    : __bool_constant<
 std::__call_is_nt<_Fn, _Args...>(typename _Result::__invoke_type{})
      >
    { };

  template<typename _Fn, typename... _Args>
    using __call_is_nothrow_
      = __call_is_nothrow<__invoke_result<_Fn, _Args...>, _Fn, _Args...>;


  template<typename _Fn, typename... _Args>
    struct __is_nothrow_invocable

    : __bool_constant<__is_nothrow_invocable(_Fn, _Args...)>




    { };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wctor-dtor-privacy"
  struct __nonesuchbase {};
  struct __nonesuch : private __nonesuchbase {
    ~__nonesuch() = delete;
    __nonesuch(__nonesuch const&) = delete;
    void operator=(__nonesuch const&) = delete;
  };
#pragma GCC diagnostic pop




  template<typename _Functor, typename... _ArgTypes>
    struct invoke_result
    : public __invoke_result<_Functor, _ArgTypes...>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Functor>{}),
 "_Functor must be a complete class or an unbounded array");
      static_assert((std::__is_complete_or_unbounded(
 __type_identity<_ArgTypes>{}) && ...),
 "each argument type must be a complete class or an unbounded array");
    };


  template<typename _Fn, typename... _Args>
    using invoke_result_t = typename invoke_result<_Fn, _Args...>::type;


  template<typename _Fn, typename... _ArgTypes>
    struct is_invocable

    : public __bool_constant<__is_invocable(_Fn, _ArgTypes...)>



    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Fn>{}),
 "_Fn must be a complete class or an unbounded array");
      static_assert((std::__is_complete_or_unbounded(
 __type_identity<_ArgTypes>{}) && ...),
 "each argument type must be a complete class or an unbounded array");
    };


  template<typename _Ret, typename _Fn, typename... _ArgTypes>
    struct is_invocable_r
    : __is_invocable_impl<__invoke_result<_Fn, _ArgTypes...>, _Ret>::type
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Fn>{}),
 "_Fn must be a complete class or an unbounded array");
      static_assert((std::__is_complete_or_unbounded(
 __type_identity<_ArgTypes>{}) && ...),
 "each argument type must be a complete class or an unbounded array");
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Ret>{}),
 "_Ret must be a complete class or an unbounded array");
    };


  template<typename _Fn, typename... _ArgTypes>
    struct is_nothrow_invocable

    : public __bool_constant<__is_nothrow_invocable(_Fn, _ArgTypes...)>




    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Fn>{}),
 "_Fn must be a complete class or an unbounded array");
      static_assert((std::__is_complete_or_unbounded(
 __type_identity<_ArgTypes>{}) && ...),
 "each argument type must be a complete class or an unbounded array");
    };





  template<typename _Result, typename _Ret>
    using __is_nt_invocable_impl
      = typename __is_invocable_impl<_Result, _Ret>::__nothrow_conv;



  template<typename _Ret, typename _Fn, typename... _ArgTypes>
    struct is_nothrow_invocable_r
    : __and_<__is_nt_invocable_impl<__invoke_result<_Fn, _ArgTypes...>, _Ret>,
             __call_is_nothrow_<_Fn, _ArgTypes...>>::type
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Fn>{}),
 "_Fn must be a complete class or an unbounded array");
      static_assert((std::__is_complete_or_unbounded(
 __type_identity<_ArgTypes>{}) && ...),
 "each argument type must be a complete class or an unbounded array");
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Ret>{}),
 "_Ret must be a complete class or an unbounded array");
    };
# 3498 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_void_v = is_void<_Tp>::value;
template <typename _Tp>
  inline constexpr bool is_null_pointer_v = is_null_pointer<_Tp>::value;
template <typename _Tp>
  inline constexpr bool is_integral_v = is_integral<_Tp>::value;
template <typename _Tp>
  inline constexpr bool is_floating_point_v = is_floating_point<_Tp>::value;


template <typename _Tp>
  inline constexpr bool is_array_v = __is_array(_Tp);
# 3520 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_pointer_v = __is_pointer(_Tp);
# 3535 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_lvalue_reference_v = false;
template <typename _Tp>
  inline constexpr bool is_lvalue_reference_v<_Tp&> = true;
template <typename _Tp>
  inline constexpr bool is_rvalue_reference_v = false;
template <typename _Tp>
  inline constexpr bool is_rvalue_reference_v<_Tp&&> = true;


template <typename _Tp>
  inline constexpr bool is_member_object_pointer_v =
    __is_member_object_pointer(_Tp);







template <typename _Tp>
  inline constexpr bool is_member_function_pointer_v =
    __is_member_function_pointer(_Tp);
# 3577 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_enum_v = __is_enum(_Tp);
template <typename _Tp>
  inline constexpr bool is_union_v = __is_union(_Tp);
template <typename _Tp>
  inline constexpr bool is_class_v = __is_class(_Tp);



template <typename _Tp>
  inline constexpr bool is_reference_v = __is_reference(_Tp);
# 3597 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_arithmetic_v = is_arithmetic<_Tp>::value;
template <typename _Tp>
  inline constexpr bool is_fundamental_v = is_fundamental<_Tp>::value;


template <typename _Tp>
  inline constexpr bool is_object_v = __is_object(_Tp);





template <typename _Tp>
  inline constexpr bool is_scalar_v = is_scalar<_Tp>::value;
template <typename _Tp>
  inline constexpr bool is_compound_v = !is_fundamental_v<_Tp>;


template <typename _Tp>
  inline constexpr bool is_member_pointer_v = __is_member_pointer(_Tp);






template <typename _Tp>
  inline constexpr bool is_const_v = __is_const(_Tp);
# 3634 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_function_v = __is_function(_Tp);
# 3646 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp>
  inline constexpr bool is_volatile_v = __is_volatile(_Tp);







template <typename _Tp>
  __attribute__ ((__deprecated__ ("use '" "is_trivially_default_constructible_v && is_trivially_copyable_v" "' instead")))
  inline constexpr bool is_trivial_v = __is_trivial(_Tp);
template <typename _Tp>
  inline constexpr bool is_trivially_copyable_v = __is_trivially_copyable(_Tp);
template <typename _Tp>
  inline constexpr bool is_standard_layout_v = __is_standard_layout(_Tp);
template <typename _Tp>
  __attribute__ ((__deprecated__ ("use '" "is_standard_layout_v && is_trivial_v" "' instead")))
  inline constexpr bool is_pod_v = __is_pod(_Tp);
template <typename _Tp>
  [[__deprecated__]]
  inline constexpr bool is_literal_type_v = __is_literal_type(_Tp);
template <typename _Tp>
  inline constexpr bool is_empty_v = __is_empty(_Tp);
template <typename _Tp>
  inline constexpr bool is_polymorphic_v = __is_polymorphic(_Tp);
template <typename _Tp>
  inline constexpr bool is_abstract_v = __is_abstract(_Tp);
template <typename _Tp>
  inline constexpr bool is_final_v = __is_final(_Tp);

template <typename _Tp>
  inline constexpr bool is_signed_v = is_signed<_Tp>::value;
template <typename _Tp>
  inline constexpr bool is_unsigned_v = is_unsigned<_Tp>::value;

template <typename _Tp, typename... _Args>
  inline constexpr bool is_constructible_v = __is_constructible(_Tp, _Args...);
template <typename _Tp>
  inline constexpr bool is_default_constructible_v = __is_constructible(_Tp);
template <typename _Tp>
  inline constexpr bool is_copy_constructible_v
    = __is_constructible(_Tp, __add_lval_ref_t<const _Tp>);
template <typename _Tp>
  inline constexpr bool is_move_constructible_v
    = __is_constructible(_Tp, __add_rval_ref_t<_Tp>);

template <typename _Tp, typename _Up>
  inline constexpr bool is_assignable_v = __is_assignable(_Tp, _Up);
template <typename _Tp>
  inline constexpr bool is_copy_assignable_v
    = __is_assignable(__add_lval_ref_t<_Tp>, __add_lval_ref_t<const _Tp>);
template <typename _Tp>
  inline constexpr bool is_move_assignable_v
    = __is_assignable(__add_lval_ref_t<_Tp>, __add_rval_ref_t<_Tp>);


template <typename _Tp>
  inline constexpr bool is_destructible_v = __is_destructible(_Tp);





template <typename _Tp, typename... _Args>
  inline constexpr bool is_trivially_constructible_v
    = __is_trivially_constructible(_Tp, _Args...);
template <typename _Tp>
  inline constexpr bool is_trivially_default_constructible_v
    = __is_trivially_constructible(_Tp);
template <typename _Tp>
  inline constexpr bool is_trivially_copy_constructible_v
    = __is_trivially_constructible(_Tp, __add_lval_ref_t<const _Tp>);
template <typename _Tp>
  inline constexpr bool is_trivially_move_constructible_v
    = __is_trivially_constructible(_Tp, __add_rval_ref_t<_Tp>);

template <typename _Tp, typename _Up>
  inline constexpr bool is_trivially_assignable_v
    = __is_trivially_assignable(_Tp, _Up);
template <typename _Tp>
  inline constexpr bool is_trivially_copy_assignable_v
    = __is_trivially_assignable(__add_lval_ref_t<_Tp>,
    __add_lval_ref_t<const _Tp>);
template <typename _Tp>
  inline constexpr bool is_trivially_move_assignable_v
    = __is_trivially_assignable(__add_lval_ref_t<_Tp>,
    __add_rval_ref_t<_Tp>);


template <typename _Tp>
  inline constexpr bool is_trivially_destructible_v
    = __is_trivially_destructible(_Tp);
# 3760 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp, typename... _Args>
  inline constexpr bool is_nothrow_constructible_v
    = __is_nothrow_constructible(_Tp, _Args...);
template <typename _Tp>
  inline constexpr bool is_nothrow_default_constructible_v
    = __is_nothrow_constructible(_Tp);
template <typename _Tp>
  inline constexpr bool is_nothrow_copy_constructible_v
    = __is_nothrow_constructible(_Tp, __add_lval_ref_t<const _Tp>);
template <typename _Tp>
  inline constexpr bool is_nothrow_move_constructible_v
    = __is_nothrow_constructible(_Tp, __add_rval_ref_t<_Tp>);

template <typename _Tp, typename _Up>
  inline constexpr bool is_nothrow_assignable_v
    = __is_nothrow_assignable(_Tp, _Up);
template <typename _Tp>
  inline constexpr bool is_nothrow_copy_assignable_v
    = __is_nothrow_assignable(__add_lval_ref_t<_Tp>,
         __add_lval_ref_t<const _Tp>);
template <typename _Tp>
  inline constexpr bool is_nothrow_move_assignable_v
    = __is_nothrow_assignable(__add_lval_ref_t<_Tp>, __add_rval_ref_t<_Tp>);


template <typename _Tp>
  inline constexpr bool is_nothrow_destructible_v
    = __is_nothrow_destructible(_Tp);






template <typename _Tp>
  inline constexpr bool has_virtual_destructor_v
    = __has_virtual_destructor(_Tp);

template <typename _Tp>
  inline constexpr size_t alignment_of_v = alignment_of<_Tp>::value;



template <typename _Tp>
  inline constexpr size_t rank_v = __array_rank(_Tp);
# 3814 "/usr/local/include/c++/16.1.0/type_traits" 3
template <typename _Tp, unsigned _Idx = 0>
  inline constexpr size_t extent_v = 0;
template <typename _Tp, size_t _Size>
  inline constexpr size_t extent_v<_Tp[_Size], 0> = _Size;
template <typename _Tp, unsigned _Idx, size_t _Size>
  inline constexpr size_t extent_v<_Tp[_Size], _Idx> = extent_v<_Tp, _Idx - 1>;
template <typename _Tp>
  inline constexpr size_t extent_v<_Tp[], 0> = 0;
template <typename _Tp, unsigned _Idx>
  inline constexpr size_t extent_v<_Tp[], _Idx> = extent_v<_Tp, _Idx - 1>;


template <typename _Tp, typename _Up>
  inline constexpr bool is_same_v = __is_same(_Tp, _Up);






template <typename _Base, typename _Derived>
  inline constexpr bool is_base_of_v = __is_base_of(_Base, _Derived);

template <typename _Base, typename _Derived>
  inline constexpr bool is_virtual_base_of_v = __builtin_is_virtual_base_of(_Base, _Derived);


template <typename _From, typename _To>
  inline constexpr bool is_convertible_v = __is_convertible(_From, _To);




template<typename _Fn, typename... _Args>
  inline constexpr bool is_invocable_v

    = __is_invocable(_Fn, _Args...);



template<typename _Fn, typename... _Args>
  inline constexpr bool is_nothrow_invocable_v

    = __is_nothrow_invocable(_Fn, _Args...);



template<typename _Ret, typename _Fn, typename... _Args>
  inline constexpr bool is_invocable_r_v
    = is_invocable_r<_Ret, _Fn, _Args...>::value;
template<typename _Ret, typename _Fn, typename... _Args>
  inline constexpr bool is_nothrow_invocable_r_v
    = is_nothrow_invocable_r<_Ret, _Fn, _Args...>::value;






  template<typename _Tp>
    struct has_unique_object_representations
    : bool_constant<__has_unique_object_representations(
      remove_cv_t<remove_all_extents_t<_Tp>>
      )>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{}),
 "template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp>
    inline constexpr bool has_unique_object_representations_v
      = has_unique_object_representations<_Tp>::value;






  template<typename _Tp>
    struct is_aggregate
    : bool_constant<__is_aggregate(remove_cv_t<_Tp>)>
    { };






  template<typename _Tp>
    inline constexpr bool is_aggregate_v = __is_aggregate(remove_cv_t<_Tp>);






  template<typename _Tp>
    struct is_structural
    : bool_constant<__builtin_is_structural(_Tp)>
    { };





  template<typename _Tp>
    inline constexpr bool is_structural_v
      = __builtin_is_structural(_Tp);
# 3932 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct remove_cvref
    { using type = __remove_cvref(_Tp); };
# 3949 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    using remove_cvref_t = typename remove_cvref<_Tp>::type;
# 3959 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct type_identity { using type = _Tp; };

  template<typename _Tp>
    using type_identity_t = typename type_identity<_Tp>::type;
# 3972 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct unwrap_reference { using type = _Tp; };

  template<typename _Tp>
    struct unwrap_reference<reference_wrapper<_Tp>> { using type = _Tp&; };

  template<typename _Tp>
    using unwrap_reference_t = typename unwrap_reference<_Tp>::type;






  template<typename _Tp>
    struct unwrap_ref_decay { using type = unwrap_reference_t<decay_t<_Tp>>; };

  template<typename _Tp>
    using unwrap_ref_decay_t = typename unwrap_ref_decay<_Tp>::type;
# 3999 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    inline constexpr bool is_bounded_array_v = __is_bounded_array(_Tp);
# 4013 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    inline constexpr bool is_unbounded_array_v = __is_unbounded_array(_Tp);
# 4025 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_bounded_array
    : public bool_constant<is_bounded_array_v<_Tp>>
    { };



  template<typename _Tp>
    struct is_unbounded_array
    : public bool_constant<is_unbounded_array_v<_Tp>>
    { };





  template<typename _Tp, typename _Up>
    struct is_layout_compatible
    : bool_constant<__is_layout_compatible(_Tp, _Up)>
    { };



  template<typename _Tp, typename _Up>
    constexpr bool is_layout_compatible_v
      = __is_layout_compatible(_Tp, _Up);







  template<typename _S1, typename _S2, typename _M1, typename _M2>
    constexpr bool
    is_corresponding_member(_M1 _S1::*__m1, _M2 _S2::*__m2) noexcept
    { return __builtin_is_corresponding_member(__m1, __m2); }







  template<typename _Base, typename _Derived>
    struct is_pointer_interconvertible_base_of
    : bool_constant<__is_pointer_interconvertible_base_of(_Base, _Derived)>
    { };



  template<typename _Base, typename _Derived>
    constexpr bool is_pointer_interconvertible_base_of_v
      = __is_pointer_interconvertible_base_of(_Base, _Derived);
# 4088 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp, typename _Mem>
    constexpr bool
    is_pointer_interconvertible_with_class(_Mem _Tp::*__mp) noexcept
    { return __builtin_is_pointer_interconvertible_with_class(__mp); }
# 4100 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_scoped_enum
    : bool_constant<__is_scoped_enum(_Tp)>
    { };
# 4121 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    inline constexpr bool is_scoped_enum_v = __is_scoped_enum(_Tp);
# 4133 "/usr/local/include/c++/16.1.0/type_traits" 3
  template<typename _Tp>
    struct is_implicit_lifetime
    : bool_constant<__builtin_is_implicit_lifetime(_Tp)>
    { };



  template<typename _Tp>
    inline constexpr bool is_implicit_lifetime_v
      = __builtin_is_implicit_lifetime(_Tp);







  template<typename _Tp, typename _Up>
    struct reference_constructs_from_temporary
    : public bool_constant<__reference_constructs_from_temporary(_Tp, _Up)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{})
      && std::__is_complete_or_unbounded(__type_identity<_Up>{}),
 "template argument must be a complete class or an unbounded array");
    };





  template<typename _Tp, typename _Up>
    struct reference_converts_from_temporary
    : public bool_constant<__reference_converts_from_temporary(_Tp, _Up)>
    {
      static_assert(std::__is_complete_or_unbounded(__type_identity<_Tp>{})
      && std::__is_complete_or_unbounded(__type_identity<_Up>{}),
 "template argument must be a complete class or an unbounded array");
    };



  template<typename _Tp, typename _Up>
    inline constexpr bool reference_constructs_from_temporary_v
      = reference_constructs_from_temporary<_Tp, _Up>::value;



  template<typename _Tp, typename _Up>
    inline constexpr bool reference_converts_from_temporary_v
      = reference_converts_from_temporary<_Tp, _Up>::value;





  [[__gnu__::__always_inline__]]
  constexpr bool
  is_constant_evaluated() noexcept
  {

    if consteval { return true; } else { return false; }



  }




  template<typename _From, typename _To>
    using __copy_cv = typename __match_cv_qualifiers<_From, _To>::__type;

  template<typename _Xp, typename _Yp>
    using __cond_res
      = decltype(false ? declval<_Xp(&)()>()() : declval<_Yp(&)()>()());

  template<typename _Ap, typename _Bp, typename = void>
    struct __common_ref_impl
    { };


  template<typename _Ap, typename _Bp>
    using __common_ref = typename __common_ref_impl<_Ap, _Bp>::type;


  template<typename _Xp, typename _Yp>
    using __condres_cvref
      = __cond_res<__copy_cv<_Xp, _Yp>&, __copy_cv<_Yp, _Xp>&>;


  template<typename _Xp, typename _Yp>
    struct __common_ref_impl<_Xp&, _Yp&, __void_t<__condres_cvref<_Xp, _Yp>>>
    : enable_if<is_reference_v<__condres_cvref<_Xp, _Yp>>,
  __condres_cvref<_Xp, _Yp>>
    { };


  template<typename _Xp, typename _Yp>
    using __common_ref_C = remove_reference_t<__common_ref<_Xp&, _Yp&>>&&;


  template<typename _Xp, typename _Yp>
    struct __common_ref_impl<_Xp&&, _Yp&&,
      _Require<is_convertible<_Xp&&, __common_ref_C<_Xp, _Yp>>,
        is_convertible<_Yp&&, __common_ref_C<_Xp, _Yp>>>>
    { using type = __common_ref_C<_Xp, _Yp>; };


  template<typename _Xp, typename _Yp>
    using __common_ref_D = __common_ref<const _Xp&, _Yp&>;


  template<typename _Xp, typename _Yp>
    struct __common_ref_impl<_Xp&&, _Yp&,
      _Require<is_convertible<_Xp&&, __common_ref_D<_Xp, _Yp>>>>
    { using type = __common_ref_D<_Xp, _Yp>; };


  template<typename _Xp, typename _Yp>
    struct __common_ref_impl<_Xp&, _Yp&&>
    : __common_ref_impl<_Yp&&, _Xp&>
    { };


  template<typename _Tp, typename _Up,
    template<typename> class _TQual, template<typename> class _UQual>
    struct basic_common_reference
    { };


  template<typename _Tp>
    struct __xref
    { template<typename _Up> using __type = __copy_cv<_Tp, _Up>; };

  template<typename _Tp>
    struct __xref<_Tp&>
    { template<typename _Up> using __type = __copy_cv<_Tp, _Up>&; };

  template<typename _Tp>
    struct __xref<_Tp&&>
    { template<typename _Up> using __type = __copy_cv<_Tp, _Up>&&; };

  template<typename _Tp1, typename _Tp2>
    using __basic_common_ref
      = typename basic_common_reference<remove_cvref_t<_Tp1>,
     remove_cvref_t<_Tp2>,
     __xref<_Tp1>::template __type,
     __xref<_Tp2>::template __type>::type;


  template<typename... _Tp>
    struct common_reference;

  template<typename... _Tp>
    using common_reference_t = typename common_reference<_Tp...>::type;


  template<>
    struct common_reference<>
    { };


  template<typename _Tp0>
    struct common_reference<_Tp0>
    { using type = _Tp0; };


  template<typename _Tp1, typename _Tp2, int _Bullet = 1>
    struct __common_reference_impl
    : __common_reference_impl<_Tp1, _Tp2, _Bullet + 1>
    { };


  template<typename _Tp1, typename _Tp2>
    struct common_reference<_Tp1, _Tp2>
    : __common_reference_impl<_Tp1, _Tp2>
    { };


  template<typename _Tp1, typename _Tp2>
    requires is_reference_v<_Tp1> && is_reference_v<_Tp2>
      && requires { typename __common_ref<_Tp1, _Tp2>; }

      && is_convertible_v<add_pointer_t<_Tp1>,
     add_pointer_t<__common_ref<_Tp1, _Tp2>>>
      && is_convertible_v<add_pointer_t<_Tp2>,
     add_pointer_t<__common_ref<_Tp1, _Tp2>>>

    struct __common_reference_impl<_Tp1, _Tp2, 1>
    { using type = __common_ref<_Tp1, _Tp2>; };


  template<typename _Tp1, typename _Tp2>
    requires requires { typename __basic_common_ref<_Tp1, _Tp2>; }
    struct __common_reference_impl<_Tp1, _Tp2, 2>
    { using type = __basic_common_ref<_Tp1, _Tp2>; };


  template<typename _Tp1, typename _Tp2>
    requires requires { typename __cond_res<_Tp1, _Tp2>; }
    struct __common_reference_impl<_Tp1, _Tp2, 3>
    { using type = __cond_res<_Tp1, _Tp2>; };


  template<typename _Tp1, typename _Tp2>
    requires requires { typename common_type_t<_Tp1, _Tp2>; }
    struct __common_reference_impl<_Tp1, _Tp2, 4>
    { using type = common_type_t<_Tp1, _Tp2>; };


  template<typename _Tp1, typename _Tp2>
    struct __common_reference_impl<_Tp1, _Tp2, 5>
    { };


  template<typename _Tp1, typename _Tp2, typename... _Rest>
    struct common_reference<_Tp1, _Tp2, _Rest...>
    : __common_type_fold<common_reference<_Tp1, _Tp2>,
    __common_type_pack<_Rest...>>
    { };


  template<typename _Tp1, typename _Tp2, typename... _Rest>
    struct __common_type_fold<common_reference<_Tp1, _Tp2>,
         __common_type_pack<_Rest...>,
         void_t<common_reference_t<_Tp1, _Tp2>>>
    : public common_reference<common_reference_t<_Tp1, _Tp2>, _Rest...>
    { };







  template<size_t... _Indexes> struct _Index_tuple { };


  template<size_t _Num>
    struct _Build_index_tuple
    {
# 4382 "/usr/local/include/c++/16.1.0/type_traits" 3
      using __type = _Index_tuple<__integer_pack(_Num)...>;

    };





}
}
# 49 "/usr/local/include/c++/16.1.0/concepts" 2 3

namespace std __attribute__ ((__visibility__ ("default")))
{




  namespace __detail
  {
    template<typename _Tp, typename _Up>
      concept __same_as = std::is_same_v<_Tp, _Up>;
  }


  template<typename _Tp, typename _Up>
    concept same_as
      = __detail::__same_as<_Tp, _Up> && __detail::__same_as<_Up, _Tp>;

  namespace __detail
  {
    template<typename _Tp, typename _Up>
      concept __different_from
 = !same_as<remove_cvref_t<_Tp>, remove_cvref_t<_Up>>;
  }


  template<typename _Derived, typename _Base>
    concept derived_from = __is_base_of(_Base, _Derived)
      && is_convertible_v<const volatile _Derived*, const volatile _Base*>;


  template<typename _From, typename _To>
    concept convertible_to = is_convertible_v<_From, _To>
      && requires { static_cast<_To>(std::declval<_From>()); };


  template<typename _Tp, typename _Up>
    concept common_reference_with
      = same_as<common_reference_t<_Tp, _Up>, common_reference_t<_Up, _Tp>>
      && convertible_to<_Tp, common_reference_t<_Tp, _Up>>
      && convertible_to<_Up, common_reference_t<_Tp, _Up>>;


  template<typename _Tp, typename _Up>
    concept common_with
      = same_as<common_type_t<_Tp, _Up>, common_type_t<_Up, _Tp>>
      && requires {
 static_cast<common_type_t<_Tp, _Up>>(std::declval<_Tp>());
 static_cast<common_type_t<_Tp, _Up>>(std::declval<_Up>());
      }
      && common_reference_with<add_lvalue_reference_t<const _Tp>,
          add_lvalue_reference_t<const _Up>>
      && common_reference_with<add_lvalue_reference_t<common_type_t<_Tp, _Up>>,
          common_reference_t<
     add_lvalue_reference_t<const _Tp>,
     add_lvalue_reference_t<const _Up>>>;



  template<typename _Tp>
    concept integral = is_integral_v<_Tp>;

  template<typename _Tp>
    concept signed_integral = integral<_Tp> && is_signed_v<_Tp>;

  template<typename _Tp>
    concept unsigned_integral = integral<_Tp> && !signed_integral<_Tp>;

  template<typename _Tp>
    concept floating_point = is_floating_point_v<_Tp>;

  namespace __detail
  {
    template<typename _Tp>
      using __cref = const remove_reference_t<_Tp>&;

    template<typename _Tp>
      concept __class_or_enum
 = is_class_v<_Tp> || is_union_v<_Tp> || is_enum_v<_Tp>;

    template<typename _Tp>
      constexpr bool __destructible_impl = false;
    template<typename _Tp>
      requires requires(_Tp& __t) { { __t.~_Tp() } noexcept; }
      constexpr bool __destructible_impl<_Tp> = true;

    template<typename _Tp>
      constexpr bool __destructible = __destructible_impl<_Tp>;
    template<typename _Tp>
      constexpr bool __destructible<_Tp&> = true;
    template<typename _Tp>
      constexpr bool __destructible<_Tp&&> = true;
    template<typename _Tp, size_t _Nm>
      constexpr bool __destructible<_Tp[_Nm]> = __destructible<_Tp>;

  }


  template<typename _Lhs, typename _Rhs>
    concept assignable_from
      = is_lvalue_reference_v<_Lhs>
      && common_reference_with<__detail::__cref<_Lhs>, __detail::__cref<_Rhs>>
      && requires(_Lhs __lhs, _Rhs&& __rhs) {
 { __lhs = static_cast<_Rhs&&>(__rhs) } -> same_as<_Lhs>;
      };


  template<typename _Tp>
    concept destructible = __detail::__destructible<_Tp>;


  template<typename _Tp, typename... _Args>
    concept constructible_from
      = destructible<_Tp> && is_constructible_v<_Tp, _Args...>;


  template<typename _Tp>
    concept default_initializable = constructible_from<_Tp>
      && requires
      {
 _Tp{};
 (void) ::new _Tp;
      };


  template<typename _Tp>
    concept move_constructible
    = constructible_from<_Tp, _Tp> && convertible_to<_Tp, _Tp>;


  template<typename _Tp>
    concept copy_constructible
      = move_constructible<_Tp>
      && constructible_from<_Tp, _Tp&> && convertible_to<_Tp&, _Tp>
      && constructible_from<_Tp, const _Tp&> && convertible_to<const _Tp&, _Tp>
      && constructible_from<_Tp, const _Tp> && convertible_to<const _Tp, _Tp>;



  namespace ranges
  {

    namespace __swap
    {
      template<typename _Tp> void swap(_Tp&, _Tp&) = delete;

      template<typename _Tp, typename _Up>
 concept __adl_swap
   = (std::__detail::__class_or_enum<remove_reference_t<_Tp>>
     || std::__detail::__class_or_enum<remove_reference_t<_Up>>)
   && requires(_Tp&& __t, _Up&& __u) {
     swap(static_cast<_Tp&&>(__t), static_cast<_Up&&>(__u));
   };

      struct _Swap
      {
      private:
 template<typename _Tp, typename _Up>
   static consteval bool
   _S_noexcept()
   {
     if constexpr (__adl_swap<_Tp, _Up>)
       return noexcept(swap(std::declval<_Tp>(), std::declval<_Up>()));
     else
       return is_nothrow_move_constructible_v<remove_reference_t<_Tp>>
     && is_nothrow_move_assignable_v<remove_reference_t<_Tp>>;
   }

      public:
 template<typename _Tp, typename _Up>
   requires __adl_swap<_Tp, _Up>
   || (same_as<_Tp, _Up> && is_lvalue_reference_v<_Tp>
       && move_constructible<remove_reference_t<_Tp>>
       && assignable_from<_Tp, remove_reference_t<_Tp>>)
   constexpr void
   operator()(_Tp&& __t, _Up&& __u) const
   noexcept(_S_noexcept<_Tp, _Up>())
   {
     if constexpr (__adl_swap<_Tp, _Up>)
       swap(static_cast<_Tp&&>(__t), static_cast<_Up&&>(__u));
     else
       {
  auto __tmp = static_cast<remove_reference_t<_Tp>&&>(__t);
  __t = static_cast<remove_reference_t<_Tp>&&>(__u);
  __u = static_cast<remove_reference_t<_Tp>&&>(__tmp);
       }
   }

 template<typename _Tp, typename _Up, size_t _Num>
   requires requires(const _Swap& __swap, _Tp& __e1, _Up& __e2) {
     __swap(__e1, __e2);
   }
   constexpr void
   operator()(_Tp (&__e1)[_Num], _Up (&__e2)[_Num]) const
   noexcept(noexcept(std::declval<const _Swap&>()(*__e1, *__e2)))
   {
     for (size_t __n = 0; __n < _Num; ++__n)
       (*this)(__e1[__n], __e2[__n]);
   }
      };
    }


    inline namespace _Cpo {
      inline constexpr __swap::_Swap swap{};
    }
  }

  template<typename _Tp>
    concept swappable
      = requires(_Tp& __a, _Tp& __b) { ranges::swap(__a, __b); };

  template<typename _Tp, typename _Up>
    concept swappable_with = common_reference_with<_Tp, _Up>
      && requires(_Tp&& __t, _Up&& __u) {
 ranges::swap(static_cast<_Tp&&>(__t), static_cast<_Tp&&>(__t));
 ranges::swap(static_cast<_Up&&>(__u), static_cast<_Up&&>(__u));
 ranges::swap(static_cast<_Tp&&>(__t), static_cast<_Up&&>(__u));
 ranges::swap(static_cast<_Up&&>(__u), static_cast<_Tp&&>(__t));
      };



  template<typename _Tp>
    concept movable = is_object_v<_Tp> && move_constructible<_Tp>
      && assignable_from<_Tp&, _Tp> && swappable<_Tp>;

  template<typename _Tp>
    concept copyable = copy_constructible<_Tp> && movable<_Tp>
      && assignable_from<_Tp&, _Tp&> && assignable_from<_Tp&, const _Tp&>
      && assignable_from<_Tp&, const _Tp>;

  template<typename _Tp>
    concept semiregular = copyable<_Tp> && default_initializable<_Tp>;




  namespace __detail
  {
    template<typename _Tp>
      concept __boolean_testable_impl = convertible_to<_Tp, bool>;

    template<typename _Tp>
      concept __boolean_testable
 = __boolean_testable_impl<_Tp>
   && requires(_Tp&& __t)
   { { !static_cast<_Tp&&>(__t) } -> __boolean_testable_impl; };
  }


  namespace __detail
  {
    template<typename _Tp, typename _Up,
      typename _Cref = common_reference_t<const _Tp&, const _Up&>>
      concept __comparison_common_type_with_impl
 = same_as<common_reference_t<const _Tp&, const _Up&>,
    common_reference_t<const _Up&, const _Tp&>>
     && requires {
       requires convertible_to<const _Tp&, const _Cref&>
  || convertible_to<_Tp, const _Cref&>;
       requires convertible_to<const _Up&, const _Cref&>
  || convertible_to<_Up, const _Cref&>;
     };

    template<typename _Tp, typename _Up>
      concept __comparison_common_type_with
 = __comparison_common_type_with_impl<remove_cvref_t<_Tp>,
          remove_cvref_t<_Up>>;
  }



  namespace __detail
  {
    template<typename _Tp, typename _Up>
      concept __weakly_eq_cmp_with
 = requires(__detail::__cref<_Tp> __t, __detail::__cref<_Up> __u) {
   { __t == __u } -> __boolean_testable;
   { __t != __u } -> __boolean_testable;
   { __u == __t } -> __boolean_testable;
   { __u != __t } -> __boolean_testable;
 };
  }

  template<typename _Tp>
    concept equality_comparable = __detail::__weakly_eq_cmp_with<_Tp, _Tp>;

  template<typename _Tp, typename _Up>
    concept equality_comparable_with
      = equality_comparable<_Tp> && equality_comparable<_Up>
      && __detail::__comparison_common_type_with<_Tp, _Up>
      && equality_comparable<common_reference_t<__detail::__cref<_Tp>,
      __detail::__cref<_Up>>>
      && __detail::__weakly_eq_cmp_with<_Tp, _Up>;

  namespace __detail
  {
    template<typename _Tp, typename _Up>
      concept __partially_ordered_with
 = requires(const remove_reference_t<_Tp>& __t,
     const remove_reference_t<_Up>& __u) {
   { __t < __u } -> __boolean_testable;
   { __t > __u } -> __boolean_testable;
   { __t <= __u } -> __boolean_testable;
   { __t >= __u } -> __boolean_testable;
   { __u < __t } -> __boolean_testable;
   { __u > __t } -> __boolean_testable;
   { __u <= __t } -> __boolean_testable;
   { __u >= __t } -> __boolean_testable;
 };
  }


  template<typename _Tp>
    concept totally_ordered
      = equality_comparable<_Tp>
      && __detail::__partially_ordered_with<_Tp, _Tp>;

  template<typename _Tp, typename _Up>
    concept totally_ordered_with
      = totally_ordered<_Tp> && totally_ordered<_Up>
      && equality_comparable_with<_Tp, _Up>
      && totally_ordered<common_reference_t<__detail::__cref<_Tp>,
         __detail::__cref<_Up>>>
      && __detail::__partially_ordered_with<_Tp, _Up>;

  template<typename _Tp>
    concept regular = semiregular<_Tp> && equality_comparable<_Tp>;




  template<typename _Fn, typename... _Args>
    concept invocable = is_invocable_v<_Fn, _Args...>;


  template<typename _Fn, typename... _Args>
    concept regular_invocable = invocable<_Fn, _Args...>;


  template<typename _Fn, typename... _Args>
    concept predicate = regular_invocable<_Fn, _Args...>
      && __detail::__boolean_testable<invoke_result_t<_Fn, _Args...>>;


  template<typename _Rel, typename _Tp, typename _Up>
    concept relation
      = predicate<_Rel, _Tp, _Tp> && predicate<_Rel, _Up, _Up>
      && predicate<_Rel, _Tp, _Up> && predicate<_Rel, _Up, _Tp>;


  template<typename _Rel, typename _Tp, typename _Up>
    concept equivalence_relation = relation<_Rel, _Tp, _Up>;


  template<typename _Rel, typename _Tp, typename _Up>
    concept strict_weak_order = relation<_Rel, _Tp, _Up>;

  namespace __detail
  {


    template<typename _Tp, typename _Up>
      concept __not_overloaded_spaceship
        = ! requires(_Tp&& __t, _Up&& __u)
     { operator<=>(static_cast<_Tp&&>(__t), static_cast<_Up&&>(__u)); }
 && ! requires(_Tp&& __t, _Up&& __u)
     { static_cast<_Tp&&>(__t).operator<=>(static_cast<_Up&&>(__u)); }
 && (is_same_v<_Tp, _Up>
     || (! requires(_Tp&& __t, _Up&& __u)
          { operator<=>(static_cast<_Up&&>(__u), static_cast<_Tp&&>(__t)); }
        && ! requires(_Tp&& __t, _Up&& __u)
          { static_cast<_Up&&>(__u).operator<=>(static_cast<_Tp&&>(__t)); }));
  }

}
# 39 "/usr/local/include/c++/16.1.0/bit" 2 3



# 1 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 1 3
# 36 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 3
# 1 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 1 3
# 40 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 3
# 1 "/usr/local/include/c++/16.1.0/bits/version.h" 1 3
# 41 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 2 3




#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wlong-long"
# 76 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 3
extern "C++" {

namespace std __attribute__ ((__visibility__ ("default")))
{


  struct __true_type { };
  struct __false_type { };

  template<bool>
    struct __truth_type
    { typedef __false_type __type; };

  template<>
    struct __truth_type<true>
    { typedef __true_type __type; };



  template<class _Sp, class _Tp>
    struct __traitor
    {
      enum { __value = bool(_Sp::__value) || bool(_Tp::__value) };
      typedef typename __truth_type<__value>::__type __type;
    };


  template<typename, typename>
    struct __are_same
    {
      enum { __value = 0 };
      typedef __false_type __type;
    };

  template<typename _Tp>
    struct __are_same<_Tp, _Tp>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };




  template<typename _Tp>
    struct __is_integer
    {
      enum { __value = 0 };
      typedef __false_type __type;
    };



  template<>
    struct __is_integer<bool>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<signed char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };


  template<>
    struct __is_integer<wchar_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_integer<char8_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_integer<char16_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<char32_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };


  template<>
    struct __is_integer<short>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned short>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<int>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned int>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<long>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned long>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<long long>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned long long>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };
# 279 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 3
__extension__ template<> struct __is_integer<__int128> { enum { __value = 1 }; typedef __true_type __type; }; __extension__ template<> struct __is_integer<unsigned __int128> { enum { __value = 1 }; typedef __true_type __type; };







  template<typename _Tp>
    struct __is_floating
    {
      enum { __value = 0 };
      typedef __false_type __type;
    };


  template<>
    struct __is_floating<float>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_floating<double>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_floating<long double>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };
# 326 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 3
  template<>
    struct __is_floating<_Float16>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_floating<_Float32>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_floating<_Float64>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_floating<_Float128>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_floating<__gnu_cxx::__bfloat16_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };





  template<typename _Tp>
    struct __is_arithmetic
    : public __traitor<__is_integer<_Tp>, __is_floating<_Tp> >
    { };




  template<typename _Tp>
    struct __is_char
    {
      enum { __value = 0 };
      typedef __false_type __type;
    };

  template<>
    struct __is_char<char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };


  template<>
    struct __is_char<wchar_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };


  template<typename _Tp>
    struct __is_byte
    {
      enum { __value = 0 };
      typedef __false_type __type;
    };

  template<>
    struct __is_byte<char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_byte<signed char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_byte<unsigned char>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };


  enum class byte : unsigned char;

  template<>
    struct __is_byte<byte>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<>
    struct __is_byte<char8_t>
    {
      enum { __value = 1 };
      typedef __true_type __type;
    };



  template<typename _Tp>
    struct __is_nonvolatile_trivially_copyable
    {
      enum { __value = __is_trivially_copyable(_Tp) };
    };




  template<typename _Tp>
    struct __is_nonvolatile_trivially_copyable<volatile _Tp>
    {
      enum { __value = 0 };
    };


  template<typename _OutputIter, typename _InputIter>
    struct __memcpyable
    {
      enum { __value = 0 };
    };


  template<typename _Tp>
    struct __memcpyable<_Tp*, _Tp*>
    : __is_nonvolatile_trivially_copyable<_Tp>
    { };


  template<typename _Tp>
    struct __memcpyable<_Tp*, const _Tp*>
    : __is_nonvolatile_trivially_copyable<_Tp>
    { };

  template<typename _Tp> struct __memcpyable_integer;




  template<typename _Tp, typename _Up>
    struct __memcpyable<_Tp*, _Up*>
    {
      enum {
 __value = __memcpyable_integer<_Tp>::__width != 0
      && ((int)__memcpyable_integer<_Tp>::__width
     == (int)__memcpyable_integer<_Up>::__width)
      };
    };


  template<typename _Tp, typename _Up>
    struct __memcpyable<_Tp*, const _Up*>
    : __memcpyable<_Tp*, _Up*>
    { };

  template<typename _Tp>
    struct __memcpyable_integer
    {
      enum {
 __width = __is_integer<_Tp>::__value ? (sizeof(_Tp) * 8) : 0
      };
    };


  template<typename _Tp>
    struct __memcpyable_integer<volatile _Tp>
    { enum { __width = 0 }; };




  template<>
    struct __memcpyable_integer<bool>
    { enum { __width = 0 }; };
# 578 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 3
  template<>
    struct __memcpyable<_Float32*, float*> { enum { __value = true }; };
  template<>
    struct __memcpyable<float*, _Float32*> { enum { __value = true }; };



  template<>
    struct __memcpyable<_Float64*, double*> { enum { __value = true }; };
  template<>
    struct __memcpyable<double*, _Float64*> { enum { __value = true }; };



  template<>
    struct __memcpyable<_Float128*, long double*> { enum { __value = true }; };
  template<>
    struct __memcpyable<long double*, _Float128*> { enum { __value = true }; };







  template<typename _Iter1, typename _Iter2>
    struct __memcmpable
    {
      enum { __value = 0 };
    };


  template<typename _Tp>
    struct __memcmpable<_Tp*, _Tp*>
    : __is_nonvolatile_trivially_copyable<_Tp>
    { };

  template<typename _Tp>
    struct __memcmpable<const _Tp*, _Tp*>
    : __is_nonvolatile_trivially_copyable<_Tp>
    { };

  template<typename _Tp>
    struct __memcmpable<_Tp*, const _Tp*>
    : __is_nonvolatile_trivially_copyable<_Tp>
    { };







  template<typename _Tp, bool _TreatAsBytes =



 __is_byte<_Tp>::__value

    >
    struct __is_memcmp_ordered
    {
      static const bool __value = _Tp(-1) > _Tp(1);
    };

  template<typename _Tp>
    struct __is_memcmp_ordered<_Tp, false>
    {
      static const bool __value = false;
    };


  template<typename _Tp, typename _Up, bool = sizeof(_Tp) == sizeof(_Up)>
    struct __is_memcmp_ordered_with
    {
      static const bool __value = __is_memcmp_ordered<_Tp>::__value
 && __is_memcmp_ordered<_Up>::__value;
    };

  template<typename _Tp, typename _Up>
    struct __is_memcmp_ordered_with<_Tp, _Up, false>
    {
      static const bool __value = false;
    };
# 672 "/usr/local/include/c++/16.1.0/bits/cpp_type_traits.h" 3
  template<>
    struct __is_memcmp_ordered_with<std::byte, std::byte, true>
    { static constexpr bool __value = true; };

  template<typename _Tp, bool _SameSize>
    struct __is_memcmp_ordered_with<_Tp, std::byte, _SameSize>
    { static constexpr bool __value = false; };

  template<typename _Up, bool _SameSize>
    struct __is_memcmp_ordered_with<std::byte, _Up, _SameSize>
    { static constexpr bool __value = false; };



  template<typename _ValT, typename _Tp>
    constexpr bool __can_use_memchr_for_find

      = __is_byte<_ValT>::__value

   && (is_same_v<_Tp, _ValT> || is_integral_v<_Tp>);





  template<typename _Tp>
    struct __is_move_iterator
    {
      enum { __value = 0 };
      typedef __false_type __type;
    };



  template<typename _Iterator>
    constexpr
    inline _Iterator
    __miter_base(_Iterator __it)
    { return __it; }


}
}

#pragma GCC diagnostic pop
# 37 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 2 3
# 1 "/usr/local/include/c++/16.1.0/ext/type_traits.h" 1 3
# 39 "/usr/local/include/c++/16.1.0/ext/type_traits.h" 3
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wlong-long"

extern "C++" {

namespace __gnu_cxx __attribute__ ((__visibility__ ("default")))
{



  template<bool, typename>
    struct __enable_if
    { };

  template<typename _Tp>
    struct __enable_if<true, _Tp>
    { typedef _Tp __type; };



  template<bool _Cond, typename _Iftrue, typename _Iffalse>
    struct __conditional_type
    { typedef _Iftrue __type; };

  template<typename _Iftrue, typename _Iffalse>
    struct __conditional_type<false, _Iftrue, _Iffalse>
    { typedef _Iffalse __type; };



  template<typename _Tp>
    struct __add_unsigned
    {
    private:
      typedef __enable_if<std::__is_integer<_Tp>::__value, _Tp> __if_type;

    public:
      typedef typename __if_type::__type __type;
    };

  template<>
    struct __add_unsigned<char>
    { typedef unsigned char __type; };

  template<>
    struct __add_unsigned<signed char>
    { typedef unsigned char __type; };

  template<>
    struct __add_unsigned<short>
    { typedef unsigned short __type; };

  template<>
    struct __add_unsigned<int>
    { typedef unsigned int __type; };

  template<>
    struct __add_unsigned<long>
    { typedef unsigned long __type; };

  template<>
    struct __add_unsigned<long long>
    { typedef unsigned long long __type; };


  template<>
    struct __add_unsigned<bool>;

  template<>
    struct __add_unsigned<wchar_t>;



  template<typename _Tp>
    struct __remove_unsigned
    {
    private:
      typedef __enable_if<std::__is_integer<_Tp>::__value, _Tp> __if_type;

    public:
      typedef typename __if_type::__type __type;
    };

  template<>
    struct __remove_unsigned<char>
    { typedef signed char __type; };

  template<>
    struct __remove_unsigned<unsigned char>
    { typedef signed char __type; };

  template<>
    struct __remove_unsigned<unsigned short>
    { typedef short __type; };

  template<>
    struct __remove_unsigned<unsigned int>
    { typedef int __type; };

  template<>
    struct __remove_unsigned<unsigned long>
    { typedef long __type; };

  template<>
    struct __remove_unsigned<unsigned long long>
    { typedef long long __type; };


  template<>
    struct __remove_unsigned<bool>;

  template<>
    struct __remove_unsigned<wchar_t>;



  template<typename _Type>
    constexpr
    inline bool
    __is_null_pointer(_Type* __ptr)
    { return __ptr == 0; }

  template<typename _Type>
    constexpr
    inline bool
    __is_null_pointer(_Type)
    { return false; }


  constexpr bool
  __is_null_pointer(std::nullptr_t)
  { return true; }




  template<typename _Tp, bool = std::__is_integer<_Tp>::__value>
    struct __promote
    { typedef double __type; };




  template<typename _Tp>
    struct __promote<_Tp, false>
    { };

  template<>
    struct __promote<long double>
    { typedef long double __type; };

  template<>
    struct __promote<double>
    { typedef double __type; };

  template<>
    struct __promote<float>
    { typedef float __type; };


  template<>
    struct __promote<_Float16>
    { typedef _Float16 __type; };



  template<>
    struct __promote<_Float32>
    { typedef _Float32 __type; };



  template<>
    struct __promote<_Float64>
    { typedef _Float64 __type; };



  template<>
    struct __promote<_Float128>
    { typedef _Float128 __type; };



  template<>
    struct __promote<__gnu_cxx::__bfloat16_t>
    { typedef __gnu_cxx::__bfloat16_t __type; };




  template<typename... _Tp>
    using __promoted_t = decltype((typename __promote<_Tp>::__type(0) + ...));



  template<typename _Tp, typename _Up>
    using __promote_2 = __promote<__promoted_t<_Tp, _Up>>;

  template<typename _Tp, typename _Up, typename _Vp>
    using __promote_3 = __promote<__promoted_t<_Tp, _Up, _Vp>>;

  template<typename _Tp, typename _Up, typename _Vp, typename _Wp>
    using __promote_4 = __promote<__promoted_t<_Tp, _Up, _Vp, _Wp>>;
# 274 "/usr/local/include/c++/16.1.0/ext/type_traits.h" 3

}
}

#pragma GCC diagnostic pop
# 38 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 2 3

namespace __gnu_cxx __attribute__ ((__visibility__ ("default")))
{

# 52 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 3
  template<typename _Tp>
    struct __is_integer_nonstrict
    : public std::__is_integer<_Tp>
    {
      using std::__is_integer<_Tp>::__value;


      enum { __width = __value ? sizeof(_Tp) * 8 : 0 };
    };

  template<typename _Value>
    struct __numeric_traits_integer
    {

      static_assert(__is_integer_nonstrict<_Value>::__value,
      "invalid specialization");




      static const bool __is_signed = (_Value)(-1) < 0;
      static const int __digits
 = __is_integer_nonstrict<_Value>::__width - __is_signed;


      static const _Value __max = __is_signed
 ? (((((_Value)1 << (__digits - 1)) - 1) << 1) + 1)
 : ~(_Value)0;
      static const _Value __min = __is_signed ? -__max - 1 : (_Value)0;
    };

  template<typename _Value>
    const _Value __numeric_traits_integer<_Value>::__min;

  template<typename _Value>
    const _Value __numeric_traits_integer<_Value>::__max;

  template<typename _Value>
    const bool __numeric_traits_integer<_Value>::__is_signed;

  template<typename _Value>
    const int __numeric_traits_integer<_Value>::__digits;
# 133 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 3
  template<typename _Tp>
    using __int_traits = __numeric_traits_integer<_Tp>;
# 153 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 3
  template<typename _Value>
    struct __numeric_traits_floating
    {

      static const int __max_digits10 = (2 + (std::__are_same<_Value, float>::__value ? 24 : std::__are_same<_Value, double>::__value ? 53 : 113) * 643L / 2136);


      static const bool __is_signed = true;
      static const int __digits10 = (std::__are_same<_Value, float>::__value ? 6 : std::__are_same<_Value, double>::__value ? 15 : 33);
      static const int __max_exponent10 = (std::__are_same<_Value, float>::__value ? 38 : std::__are_same<_Value, double>::__value ? 308 : 4932);
    };

  template<typename _Value>
    const int __numeric_traits_floating<_Value>::__max_digits10;

  template<typename _Value>
    const bool __numeric_traits_floating<_Value>::__is_signed;

  template<typename _Value>
    const int __numeric_traits_floating<_Value>::__digits10;

  template<typename _Value>
    const int __numeric_traits_floating<_Value>::__max_exponent10;






  template<typename _Value>
    struct __numeric_traits
    : public __numeric_traits_integer<_Value>
    { };

  template<>
    struct __numeric_traits<float>
    : public __numeric_traits_floating<float>
    { };

  template<>
    struct __numeric_traits<double>
    : public __numeric_traits_floating<double>
    { };

  template<>
    struct __numeric_traits<long double>
    : public __numeric_traits_floating<long double>
    { };
# 234 "/usr/local/include/c++/16.1.0/ext/numeric_traits.h" 3

}
# 43 "/usr/local/include/c++/16.1.0/bit" 2 3
# 63 "/usr/local/include/c++/16.1.0/bit" 3
# 1 "/usr/local/include/c++/16.1.0/bits/version.h" 1 3
# 64 "/usr/local/include/c++/16.1.0/bit" 2 3

namespace std __attribute__ ((__visibility__ ("default")))
{

# 87 "/usr/local/include/c++/16.1.0/bit" 3
  template<typename _To, typename _From>
    [[nodiscard]]
    constexpr _To
    bit_cast(const _From& __from) noexcept

    requires (sizeof(_To) == sizeof(_From))
      && is_trivially_copyable_v<_To> && is_trivially_copyable_v<_From>

    {
      return __builtin_bit_cast(_To, __from);
    }
# 109 "/usr/local/include/c++/16.1.0/bit" 3
  template<integral _Tp>
    [[nodiscard]]
    constexpr _Tp
    byteswap(_Tp __value) noexcept
    {
      if constexpr (sizeof(_Tp) == 1)
 return __value;

      if !consteval
 {
   if constexpr (sizeof(_Tp) == 2)
     return __builtin_bswap16(__value);
   if constexpr (sizeof(_Tp) == 4)
     return __builtin_bswap32(__value);
   if constexpr (sizeof(_Tp) == 8)
     return __builtin_bswap64(__value);
   if constexpr (sizeof(_Tp) == 16)

     return __builtin_bswap128(__value);




 }



      using _Up = typename __make_unsigned<__remove_cv_t<_Tp>>::__type;
      size_t __diff = 8 * (sizeof(_Tp) - 1);
      _Up __mask1 = static_cast<unsigned char>(~0);
      _Up __mask2 = __mask1 << __diff;
      _Up __val = __value;
      for (size_t __i = 0; __i < sizeof(_Tp) / 2; ++__i)
 {
   _Up __byte1 = __val & __mask1;
   _Up __byte2 = __val & __mask2;
   __val = (__val ^ __byte1 ^ __byte2
     ^ (__byte1 << __diff) ^ (__byte2 >> __diff));
   __mask1 <<= 8;
   __mask2 >>= 8;
   __diff -= 2 * 8;
 }
      return __val;
    }



#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++17-extensions"

  template<typename _Tp>
    constexpr _Tp
    __rotl(_Tp __x, int __s) noexcept
    {
      constexpr auto _Nd = __gnu_cxx::__int_traits<_Tp>::__digits;
      if constexpr ((_Nd & (_Nd - 1)) == 0)
 {


   constexpr unsigned __uNd = _Nd;
   const auto __r = static_cast<unsigned>(__s);
   return (__x << (__r % __uNd)) | (__x >> ((-__r) % __uNd));
 }
      const int __r = __s % _Nd;
      if (__r == 0)
 return __x;
      else if (__r > 0)
 return (__x << __r) | (__x >> ((_Nd - __r) % _Nd));
      else
 return (__x >> -__r) | (__x << ((_Nd + __r) % _Nd));
    }

  template<typename _Tp>
    constexpr _Tp
    __rotr(_Tp __x, int __s) noexcept
    {
      constexpr auto _Nd = __gnu_cxx::__int_traits<_Tp>::__digits;
      if constexpr ((_Nd & (_Nd - 1)) == 0)
 {


   constexpr unsigned __uNd = _Nd;
   const auto __r = static_cast<unsigned>(__s);
   return (__x >> (__r % __uNd)) | (__x << ((-__r) % __uNd));
 }
      const int __r = __s % _Nd;
      if (__r == 0)
 return __x;
      else if (__r > 0)
 return (__x >> __r) | (__x << ((_Nd - __r) % _Nd));
      else
 return (__x << -__r) | (__x >> ((_Nd + __r) % _Nd));
    }

  template<typename _Tp>
    constexpr int
    __countl_zero(_Tp __x) noexcept
    {
      using __gnu_cxx::__int_traits;
      constexpr auto _Nd = __int_traits<_Tp>::__digits;


      return __builtin_clzg(__x, _Nd);
# 251 "/usr/local/include/c++/16.1.0/bit" 3
    }

  template<typename _Tp>
    constexpr int
    __countl_one(_Tp __x) noexcept
    {
      return std::__countl_zero<_Tp>((_Tp)~__x);
    }

  template<typename _Tp>
    constexpr int
    __countr_zero(_Tp __x) noexcept
    {
      using __gnu_cxx::__int_traits;
      constexpr auto _Nd = __int_traits<_Tp>::__digits;


      return __builtin_ctzg(__x, _Nd);
# 296 "/usr/local/include/c++/16.1.0/bit" 3
    }

  template<typename _Tp>
    constexpr int
    __countr_one(_Tp __x) noexcept
    {
      return std::__countr_zero((_Tp)~__x);
    }

  template<typename _Tp>
    constexpr int
    __popcount(_Tp __x) noexcept
    {

      return __builtin_popcountg(__x);
# 336 "/usr/local/include/c++/16.1.0/bit" 3
    }

  template<typename _Tp>
    constexpr bool
    __has_single_bit(_Tp __x) noexcept
    { return std::__popcount(__x) == 1; }

  template<typename _Tp>
    constexpr _Tp
    __bit_ceil(_Tp __x) noexcept
    {
      using __gnu_cxx::__int_traits;
      constexpr auto _Nd = __int_traits<_Tp>::__digits;
      if (__x == 0 || __x == 1)
        return 1;
      auto __shift_exponent = _Nd - std::__countl_zero((_Tp)(__x - 1u));




      if (!std::__is_constant_evaluated())
 {
   do { if (std::__is_constant_evaluated() && !bool(__shift_exponent != __int_traits<_Tp>::__digits)) std::__glibcxx_assert_fail(); } while (false);
 }

      using __promoted_type = decltype(__x << 1);
      if constexpr (!is_same<__promoted_type, _Tp>::value)
 {





   const int __extra_exp = sizeof(__promoted_type) / sizeof(_Tp) / 2;
   __shift_exponent |= (__shift_exponent & _Nd) << __extra_exp;
 }
      return (_Tp)1u << __shift_exponent;
    }

  template<typename _Tp>
    constexpr _Tp
    __bit_floor(_Tp __x) noexcept
    {
      constexpr auto _Nd = __gnu_cxx::__int_traits<_Tp>::__digits;
      if (__x == 0)
        return 0;
      return (_Tp)1u << (_Nd - std::__countl_zero((_Tp)(__x >> 1)));
    }

  template<typename _Tp>
    constexpr int
    __bit_width(_Tp __x) noexcept
    {
      constexpr auto _Nd = __gnu_cxx::__int_traits<_Tp>::__digits;
      return _Nd - std::__countl_zero(__x);
    }

#pragma GCC diagnostic pop





  template<typename _Tp>
    concept __unsigned_integer = __is_unsigned_integer<_Tp>::value;





  template<__unsigned_integer _Tp>
    [[nodiscard]] constexpr _Tp
    rotl(_Tp __x, int __s) noexcept
    { return std::__rotl(__x, __s); }


  template<__unsigned_integer _Tp>
    [[nodiscard]] constexpr _Tp
    rotr(_Tp __x, int __s) noexcept
    { return std::__rotr(__x, __s); }




  template<__unsigned_integer _Tp>
    constexpr int
    countl_zero(_Tp __x) noexcept
    { return std::__countl_zero(__x); }


  template<__unsigned_integer _Tp>
    constexpr int
    countl_one(_Tp __x) noexcept
    { return std::__countl_one(__x); }


  template<__unsigned_integer _Tp>
    constexpr int
    countr_zero(_Tp __x) noexcept
    { return std::__countr_zero(__x); }


  template<__unsigned_integer _Tp>
    constexpr int
    countr_one(_Tp __x) noexcept
    { return std::__countr_one(__x); }


  template<__unsigned_integer _Tp>
    constexpr int
    popcount(_Tp __x) noexcept
    { return std::__popcount(__x); }






  template<__unsigned_integer _Tp>
    constexpr bool
    has_single_bit(_Tp __x) noexcept
    { return std::__has_single_bit(__x); }


  template<__unsigned_integer _Tp>
    constexpr _Tp
    bit_ceil(_Tp __x) noexcept
    { return std::__bit_ceil(__x); }


  template<__unsigned_integer _Tp>
    constexpr _Tp
    bit_floor(_Tp __x) noexcept
    { return std::__bit_floor(__x); }




  template<__unsigned_integer _Tp>
    constexpr int
    bit_width(_Tp __x) noexcept
    { return std::__bit_width(__x); }
# 489 "/usr/local/include/c++/16.1.0/bit" 3
  enum class endian
  {
    little = 1234,
    big = 4321,
    native = 1234
  };





}
# 38 "/usr/local/include/c++/16.1.0/stdbit.h" 2 3
# 47 "/usr/local/include/c++/16.1.0/stdbit.h" 3
namespace __gnu_cxx __attribute__ ((__visibility__ ("default")))
{
# 57 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_leading_zeros(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::countl_zero(__value);
}

inline unsigned int
stdc_leading_zeros_uc(unsigned char __value)
{ return stdc_leading_zeros(__value); }

inline unsigned int
stdc_leading_zeros_us(unsigned short __value)
{ return stdc_leading_zeros(__value); }

inline unsigned int
stdc_leading_zeros_ui(unsigned int __value)
{ return stdc_leading_zeros(__value); }

inline unsigned int
stdc_leading_zeros_ul(unsigned long int __value)
{ return stdc_leading_zeros(__value); }

inline unsigned int
stdc_leading_zeros_ull(unsigned long long int __value)
{ return stdc_leading_zeros(__value); }
# 92 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_leading_ones(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::countl_one(__value);
}

inline unsigned int
stdc_leading_ones_uc(unsigned char __value)
{ return stdc_leading_ones(__value); }

inline unsigned int
stdc_leading_ones_us(unsigned short __value)
{ return stdc_leading_ones(__value); }

inline unsigned int
stdc_leading_ones_ui(unsigned int __value)
{ return stdc_leading_ones(__value); }

inline unsigned int
stdc_leading_ones_ul(unsigned long int __value)
{ return stdc_leading_ones(__value); }

inline unsigned int
stdc_leading_ones_ull(unsigned long long int __value)
{ return stdc_leading_ones(__value); }
# 127 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_trailing_zeros(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::countr_zero(__value);
}

inline unsigned int
stdc_trailing_zeros_uc(unsigned char __value)
{ return stdc_trailing_zeros(__value); }

inline unsigned int
stdc_trailing_zeros_us(unsigned short __value)
{ return stdc_trailing_zeros(__value); }

inline unsigned int
stdc_trailing_zeros_ui(unsigned int __value)
{ return stdc_trailing_zeros(__value); }

inline unsigned int
stdc_trailing_zeros_ul(unsigned long int __value)
{ return stdc_trailing_zeros(__value); }

inline unsigned int
stdc_trailing_zeros_ull(unsigned long long int __value)
{ return stdc_trailing_zeros(__value); }
# 162 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_trailing_ones(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::countr_one(__value);
}

inline unsigned int
stdc_trailing_ones_uc(unsigned char __value)
{ return stdc_trailing_ones(__value); }

inline unsigned int
stdc_trailing_ones_us(unsigned short __value)
{ return stdc_trailing_ones(__value); }

inline unsigned int
stdc_trailing_ones_ui(unsigned int __value)
{ return stdc_trailing_ones(__value); }

inline unsigned int
stdc_trailing_ones_ul(unsigned long int __value)
{ return stdc_trailing_ones(__value); }

inline unsigned int
stdc_trailing_ones_ull(unsigned long long int __value)
{ return stdc_trailing_ones(__value); }
# 199 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_first_leading_zero(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return __value == _Tp(-1) ? 0 : 1 + std::countl_one(__value);
}

inline unsigned int
stdc_first_leading_zero_uc(unsigned char __value)
{ return stdc_first_leading_zero(__value); }

inline unsigned int
stdc_first_leading_zero_us(unsigned short __value)
{ return stdc_first_leading_zero(__value); }

inline unsigned int
stdc_first_leading_zero_ui(unsigned int __value)
{ return stdc_first_leading_zero(__value); }

inline unsigned int
stdc_first_leading_zero_ul(unsigned long int __value)
{ return stdc_first_leading_zero(__value); }

inline unsigned int
stdc_first_leading_zero_ull(unsigned long long int __value)
{ return stdc_first_leading_zero(__value); }
# 236 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_first_leading_one(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return __value == 0 ? 0 : 1 + std::countl_zero(__value);
}

inline unsigned int
stdc_first_leading_one_uc(unsigned char __value)
{ return stdc_first_leading_one(__value); }

inline unsigned int
stdc_first_leading_one_us(unsigned short __value)
{ return stdc_first_leading_one(__value); }

inline unsigned int
stdc_first_leading_one_ui(unsigned int __value)
{ return stdc_first_leading_one(__value); }

inline unsigned int
stdc_first_leading_one_ul(unsigned long int __value)
{ return stdc_first_leading_one(__value); }

inline unsigned int
stdc_first_leading_one_ull(unsigned long long int __value)
{ return stdc_first_leading_one(__value); }
# 273 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_first_trailing_zero(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return __value == _Tp(-1) ? 0 : 1 + std::countr_one(__value);
}

inline unsigned int
stdc_first_trailing_zero_uc(unsigned char __value)
{ return stdc_first_trailing_zero(__value); }

inline unsigned int
stdc_first_trailing_zero_us(unsigned short __value)
{ return stdc_first_trailing_zero(__value); }

inline unsigned int
stdc_first_trailing_zero_ui(unsigned int __value)
{ return stdc_first_trailing_zero(__value); }

inline unsigned int
stdc_first_trailing_zero_ul(unsigned long int __value)
{ return stdc_first_trailing_zero(__value); }

inline unsigned int
stdc_first_trailing_zero_ull(unsigned long long int __value)
{ return stdc_first_trailing_zero(__value); }
# 310 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_first_trailing_one(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return __value == 0 ? 0 : 1 + std::countr_zero(__value);
}

inline unsigned int
stdc_first_trailing_one_uc(unsigned char __value)
{ return stdc_first_trailing_one(__value); }

inline unsigned int
stdc_first_trailing_one_us(unsigned short __value)
{ return stdc_first_trailing_one(__value); }

inline unsigned int
stdc_first_trailing_one_ui(unsigned int __value)
{ return stdc_first_trailing_one(__value); }

inline unsigned int
stdc_first_trailing_one_ul(unsigned long int __value)
{ return stdc_first_trailing_one(__value); }

inline unsigned int
stdc_first_trailing_one_ull(unsigned long long int __value)
{ return stdc_first_trailing_one(__value); }
# 346 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_count_zeros(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::popcount(_Tp(~__value));
}

inline unsigned int
stdc_count_zeros_uc(unsigned char __value)
{ return stdc_count_zeros(__value); }

inline unsigned int
stdc_count_zeros_us(unsigned short __value)
{ return stdc_count_zeros(__value); }

inline unsigned int
stdc_count_zeros_ui(unsigned int __value)
{ return stdc_count_zeros(__value); }

inline unsigned int
stdc_count_zeros_ul(unsigned long int __value)
{ return stdc_count_zeros(__value); }

inline unsigned int
stdc_count_zeros_ull(unsigned long long int __value)
{ return stdc_count_zeros(__value); }
# 382 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_count_ones(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::popcount(__value);
}

inline unsigned int
stdc_count_ones_uc(unsigned char __value)
{ return stdc_count_ones(__value); }

inline unsigned int
stdc_count_ones_us(unsigned short __value)
{ return stdc_count_ones(__value); }

inline unsigned int
stdc_count_ones_ui(unsigned int __value)
{ return stdc_count_ones(__value); }

inline unsigned int
stdc_count_ones_ul(unsigned long int __value)
{ return stdc_count_ones(__value); }

inline unsigned int
stdc_count_ones_ull(unsigned long long int __value)
{ return stdc_count_ones(__value); }
# 418 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline bool
stdc_has_single_bit(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::has_single_bit(__value);
}

inline bool
stdc_has_single_bit_uc(unsigned char __value)
{ return stdc_has_single_bit(__value); }

inline bool
stdc_has_single_bit_us(unsigned short __value)
{ return stdc_has_single_bit(__value); }

inline bool
stdc_has_single_bit_ui(unsigned int __value)
{ return stdc_has_single_bit(__value); }

inline bool
stdc_has_single_bit_ul(unsigned long int __value)
{ return stdc_has_single_bit(__value); }

inline bool
stdc_has_single_bit_ull(unsigned long long int __value)
{ return stdc_has_single_bit(__value); }
# 454 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline unsigned int
stdc_bit_width(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::bit_width(__value);
}

inline unsigned int
stdc_bit_width_uc(unsigned char __value)
{ return stdc_bit_width(__value); }

inline unsigned int
stdc_bit_width_us(unsigned short __value)
{ return stdc_bit_width(__value); }

inline unsigned int
stdc_bit_width_ui(unsigned int __value)
{ return stdc_bit_width(__value); }

inline unsigned int
stdc_bit_width_ul(unsigned long int __value)
{ return stdc_bit_width(__value); }

inline unsigned int
stdc_bit_width_ull(unsigned long long int __value)
{ return stdc_bit_width(__value); }
# 490 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline _Tp
stdc_bit_floor(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  return std::bit_floor(__value);
}

inline unsigned char
stdc_bit_floor_uc(unsigned char __value)
{ return stdc_bit_floor(__value); }

inline unsigned short
stdc_bit_floor_us(unsigned short __value)
{ return stdc_bit_floor(__value); }

inline unsigned int
stdc_bit_floor_ui(unsigned int __value)
{ return stdc_bit_floor(__value); }

inline unsigned long int
stdc_bit_floor_ul(unsigned long int __value)
{ return stdc_bit_floor(__value); }

inline unsigned long long int
stdc_bit_floor_ull(unsigned long long int __value)
{ return stdc_bit_floor(__value); }
# 529 "/usr/local/include/c++/16.1.0/stdbit.h" 3
template<typename _Tp>
inline _Tp
stdc_bit_ceil(_Tp __value)
{
  static_assert(std::__unsigned_integer<_Tp>);
  constexpr _Tp __msb = _Tp(1) << (__gnu_cxx::__int_traits<_Tp>::__digits - 1);
  return (__value & __msb) ? 0 : std::bit_ceil(__value);
}

inline unsigned char
stdc_bit_ceil_uc(unsigned char __value)
{ return stdc_bit_ceil(__value); }

inline unsigned short
stdc_bit_ceil_us(unsigned short __value)
{ return stdc_bit_ceil(__value); }

inline unsigned int
stdc_bit_ceil_ui(unsigned int __value)
{ return stdc_bit_ceil(__value); }

inline unsigned long int
stdc_bit_ceil_ul(unsigned long int __value)
{ return stdc_bit_ceil(__value); }

inline unsigned long long int
stdc_bit_ceil_ull(unsigned long long int __value)
{ return stdc_bit_ceil(__value); }



}







using __gnu_cxx::stdc_leading_zeros_uc; using __gnu_cxx::stdc_leading_zeros_us; using __gnu_cxx::stdc_leading_zeros_ui; using __gnu_cxx::stdc_leading_zeros_ul; using __gnu_cxx::stdc_leading_zeros_ull; using __gnu_cxx::stdc_leading_zeros;
using __gnu_cxx::stdc_leading_ones_uc; using __gnu_cxx::stdc_leading_ones_us; using __gnu_cxx::stdc_leading_ones_ui; using __gnu_cxx::stdc_leading_ones_ul; using __gnu_cxx::stdc_leading_ones_ull; using __gnu_cxx::stdc_leading_ones;
using __gnu_cxx::stdc_trailing_zeros_uc; using __gnu_cxx::stdc_trailing_zeros_us; using __gnu_cxx::stdc_trailing_zeros_ui; using __gnu_cxx::stdc_trailing_zeros_ul; using __gnu_cxx::stdc_trailing_zeros_ull; using __gnu_cxx::stdc_trailing_zeros;
using __gnu_cxx::stdc_trailing_ones_uc; using __gnu_cxx::stdc_trailing_ones_us; using __gnu_cxx::stdc_trailing_ones_ui; using __gnu_cxx::stdc_trailing_ones_ul; using __gnu_cxx::stdc_trailing_ones_ull; using __gnu_cxx::stdc_trailing_ones;
using __gnu_cxx::stdc_first_leading_zero_uc; using __gnu_cxx::stdc_first_leading_zero_us; using __gnu_cxx::stdc_first_leading_zero_ui; using __gnu_cxx::stdc_first_leading_zero_ul; using __gnu_cxx::stdc_first_leading_zero_ull; using __gnu_cxx::stdc_first_leading_zero;
using __gnu_cxx::stdc_first_leading_one_uc; using __gnu_cxx::stdc_first_leading_one_us; using __gnu_cxx::stdc_first_leading_one_ui; using __gnu_cxx::stdc_first_leading_one_ul; using __gnu_cxx::stdc_first_leading_one_ull; using __gnu_cxx::stdc_first_leading_one;
using __gnu_cxx::stdc_first_trailing_zero_uc; using __gnu_cxx::stdc_first_trailing_zero_us; using __gnu_cxx::stdc_first_trailing_zero_ui; using __gnu_cxx::stdc_first_trailing_zero_ul; using __gnu_cxx::stdc_first_trailing_zero_ull; using __gnu_cxx::stdc_first_trailing_zero;
using __gnu_cxx::stdc_first_trailing_one_uc; using __gnu_cxx::stdc_first_trailing_one_us; using __gnu_cxx::stdc_first_trailing_one_ui; using __gnu_cxx::stdc_first_trailing_one_ul; using __gnu_cxx::stdc_first_trailing_one_ull; using __gnu_cxx::stdc_first_trailing_one;
using __gnu_cxx::stdc_count_zeros_uc; using __gnu_cxx::stdc_count_zeros_us; using __gnu_cxx::stdc_count_zeros_ui; using __gnu_cxx::stdc_count_zeros_ul; using __gnu_cxx::stdc_count_zeros_ull; using __gnu_cxx::stdc_count_zeros;
using __gnu_cxx::stdc_count_ones_uc; using __gnu_cxx::stdc_count_ones_us; using __gnu_cxx::stdc_count_ones_ui; using __gnu_cxx::stdc_count_ones_ul; using __gnu_cxx::stdc_count_ones_ull; using __gnu_cxx::stdc_count_ones;
using __gnu_cxx::stdc_has_single_bit_uc; using __gnu_cxx::stdc_has_single_bit_us; using __gnu_cxx::stdc_has_single_bit_ui; using __gnu_cxx::stdc_has_single_bit_ul; using __gnu_cxx::stdc_has_single_bit_ull; using __gnu_cxx::stdc_has_single_bit;
using __gnu_cxx::stdc_bit_width_uc; using __gnu_cxx::stdc_bit_width_us; using __gnu_cxx::stdc_bit_width_ui; using __gnu_cxx::stdc_bit_width_ul; using __gnu_cxx::stdc_bit_width_ull; using __gnu_cxx::stdc_bit_width;
using __gnu_cxx::stdc_bit_floor_uc; using __gnu_cxx::stdc_bit_floor_us; using __gnu_cxx::stdc_bit_floor_ui; using __gnu_cxx::stdc_bit_floor_ul; using __gnu_cxx::stdc_bit_floor_ull; using __gnu_cxx::stdc_bit_floor;
using __gnu_cxx::stdc_bit_ceil_uc; using __gnu_cxx::stdc_bit_ceil_us; using __gnu_cxx::stdc_bit_ceil_ui; using __gnu_cxx::stdc_bit_ceil_ul; using __gnu_cxx::stdc_bit_ceil_ull; using __gnu_cxx::stdc_bit_ceil;
# 27 "/usr/local/include/c++/16.1.0/bits/std.compat.cc" 2
# 1 "/usr/local/include/c++/16.1.0/stdckdint.h" 1 3
# 33 "/usr/local/include/c++/16.1.0/stdckdint.h" 3
# 1 "/usr/local/include/c++/16.1.0/bits/version.h" 1 3
# 34 "/usr/local/include/c++/16.1.0/stdckdint.h" 2 3
# 44 "/usr/local/include/c++/16.1.0/stdckdint.h" 3
namespace __gnu_cxx __attribute__ ((__visibility__ ("default")))
{
# 65 "/usr/local/include/c++/16.1.0/stdckdint.h" 3
template<typename _Tp1, typename _Tp2, typename _Tp3>
  inline bool
  ckd_add(_Tp1* __result, _Tp2 __a, _Tp3 __b)
  {
    static_assert(std::__is_signed_or_unsigned_integer<_Tp1>::value);
    static_assert(std::__is_signed_or_unsigned_integer<_Tp2>::value);
    static_assert(std::__is_signed_or_unsigned_integer<_Tp3>::value);
    return __builtin_add_overflow(__a, __b, __result);
  }

template<typename _Tp1, typename _Tp2, typename _Tp3>
  inline bool
  ckd_sub(_Tp1* __result, _Tp2 __a, _Tp3 __b)
  {
    static_assert(std::__is_signed_or_unsigned_integer<_Tp1>::value);
    static_assert(std::__is_signed_or_unsigned_integer<_Tp2>::value);
    static_assert(std::__is_signed_or_unsigned_integer<_Tp3>::value);
    return __builtin_sub_overflow(__a, __b, __result);
  }

template<typename _Tp1, typename _Tp2, typename _Tp3>
  inline bool
  ckd_mul(_Tp1* __result, _Tp2 __a, _Tp3 __b)
  {
    static_assert(std::__is_signed_or_unsigned_integer<_Tp1>::value);
    static_assert(std::__is_signed_or_unsigned_integer<_Tp2>::value);
    static_assert(std::__is_signed_or_unsigned_integer<_Tp3>::value);
    return __builtin_mul_overflow(__a, __b, __result);
  }


}

using __gnu_cxx::ckd_add;
using __gnu_cxx::ckd_sub;
using __gnu_cxx::ckd_mul;
# 28 "/usr/local/include/c++/16.1.0/bits/std.compat.cc" 2


# 29 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
export module std.compat;
export import std;



export
{







using __gnu_cxx::stdc_leading_zeros_uc; using __gnu_cxx::stdc_leading_zeros_us; using __gnu_cxx::stdc_leading_zeros_ui; using __gnu_cxx::stdc_leading_zeros_ul; using __gnu_cxx::stdc_leading_zeros_ull; using __gnu_cxx::stdc_leading_zeros;
using __gnu_cxx::stdc_leading_ones_uc; using __gnu_cxx::stdc_leading_ones_us; using __gnu_cxx::stdc_leading_ones_ui; using __gnu_cxx::stdc_leading_ones_ul; using __gnu_cxx::stdc_leading_ones_ull; using __gnu_cxx::stdc_leading_ones;
using __gnu_cxx::stdc_trailing_zeros_uc; using __gnu_cxx::stdc_trailing_zeros_us; using __gnu_cxx::stdc_trailing_zeros_ui; using __gnu_cxx::stdc_trailing_zeros_ul; using __gnu_cxx::stdc_trailing_zeros_ull; using __gnu_cxx::stdc_trailing_zeros;
using __gnu_cxx::stdc_trailing_ones_uc; using __gnu_cxx::stdc_trailing_ones_us; using __gnu_cxx::stdc_trailing_ones_ui; using __gnu_cxx::stdc_trailing_ones_ul; using __gnu_cxx::stdc_trailing_ones_ull; using __gnu_cxx::stdc_trailing_ones;
using __gnu_cxx::stdc_first_leading_zero_uc; using __gnu_cxx::stdc_first_leading_zero_us; using __gnu_cxx::stdc_first_leading_zero_ui; using __gnu_cxx::stdc_first_leading_zero_ul; using __gnu_cxx::stdc_first_leading_zero_ull; using __gnu_cxx::stdc_first_leading_zero;
using __gnu_cxx::stdc_first_leading_one_uc; using __gnu_cxx::stdc_first_leading_one_us; using __gnu_cxx::stdc_first_leading_one_ui; using __gnu_cxx::stdc_first_leading_one_ul; using __gnu_cxx::stdc_first_leading_one_ull; using __gnu_cxx::stdc_first_leading_one;
using __gnu_cxx::stdc_first_trailing_zero_uc; using __gnu_cxx::stdc_first_trailing_zero_us; using __gnu_cxx::stdc_first_trailing_zero_ui; using __gnu_cxx::stdc_first_trailing_zero_ul; using __gnu_cxx::stdc_first_trailing_zero_ull; using __gnu_cxx::stdc_first_trailing_zero;
using __gnu_cxx::stdc_first_trailing_one_uc; using __gnu_cxx::stdc_first_trailing_one_us; using __gnu_cxx::stdc_first_trailing_one_ui; using __gnu_cxx::stdc_first_trailing_one_ul; using __gnu_cxx::stdc_first_trailing_one_ull; using __gnu_cxx::stdc_first_trailing_one;
using __gnu_cxx::stdc_count_zeros_uc; using __gnu_cxx::stdc_count_zeros_us; using __gnu_cxx::stdc_count_zeros_ui; using __gnu_cxx::stdc_count_zeros_ul; using __gnu_cxx::stdc_count_zeros_ull; using __gnu_cxx::stdc_count_zeros;
using __gnu_cxx::stdc_count_ones_uc; using __gnu_cxx::stdc_count_ones_us; using __gnu_cxx::stdc_count_ones_ui; using __gnu_cxx::stdc_count_ones_ul; using __gnu_cxx::stdc_count_ones_ull; using __gnu_cxx::stdc_count_ones;
using __gnu_cxx::stdc_has_single_bit_uc; using __gnu_cxx::stdc_has_single_bit_us; using __gnu_cxx::stdc_has_single_bit_ui; using __gnu_cxx::stdc_has_single_bit_ul; using __gnu_cxx::stdc_has_single_bit_ull; using __gnu_cxx::stdc_has_single_bit;
using __gnu_cxx::stdc_bit_width_uc; using __gnu_cxx::stdc_bit_width_us; using __gnu_cxx::stdc_bit_width_ui; using __gnu_cxx::stdc_bit_width_ul; using __gnu_cxx::stdc_bit_width_ull; using __gnu_cxx::stdc_bit_width;
using __gnu_cxx::stdc_bit_floor_uc; using __gnu_cxx::stdc_bit_floor_us; using __gnu_cxx::stdc_bit_floor_ui; using __gnu_cxx::stdc_bit_floor_ul; using __gnu_cxx::stdc_bit_floor_ull; using __gnu_cxx::stdc_bit_floor;
using __gnu_cxx::stdc_bit_ceil_uc; using __gnu_cxx::stdc_bit_ceil_us; using __gnu_cxx::stdc_bit_ceil_ui; using __gnu_cxx::stdc_bit_ceil_ul; using __gnu_cxx::stdc_bit_ceil_ull; using __gnu_cxx::stdc_bit_ceil;

}




export
{
  using __gnu_cxx::ckd_add;
  using __gnu_cxx::ckd_sub;
  using __gnu_cxx::ckd_mul;
}
# 89 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
export
{
  using std::isalnum;
  using std::isalpha;

  using std::isblank;

  using std::iscntrl;
  using std::isdigit;
  using std::isgraph;
  using std::islower;
  using std::isprint;
  using std::ispunct;
  using std::isspace;
  using std::isupper;
  using std::isxdigit;
  using std::tolower;
  using std::toupper;
}





export
{

  using std::feclearexcept;
  using std::fegetenv;
  using std::fegetexceptflag;
  using std::fegetround;
  using std::feholdexcept;
  using std::fenv_t;
  using std::feraiseexcept;
  using std::fesetenv;
  using std::fesetexceptflag;
  using std::fesetround;
  using std::fetestexcept;
  using std::feupdateenv;
  using std::fexcept_t;

}





export
{

  using std::imaxabs;
  using std::imaxdiv;
  using std::imaxdiv_t;
  using std::strtoimax;
  using std::strtoumax;

  using std::wcstoimax;
  using std::wcstoumax;


}





export
{
  using std::lconv;
  using std::localeconv;
  using std::setlocale;

}


export
{
  using std::abs;
  using std::acos;
  using std::acosf;
  using std::acosh;
  using std::acoshf;
  using std::acoshl;
  using std::acosl;
  using std::asin;
  using std::asinf;
  using std::asinh;
  using std::asinhf;
  using std::asinhl;
  using std::asinl;
  using std::atan;
  using std::atan2;
  using std::atan2f;
  using std::atan2l;
  using std::atanf;
  using std::atanh;
  using std::atanhf;
  using std::atanhl;
  using std::atanl;
  using std::cbrt;
  using std::cbrtf;
  using std::cbrtl;
  using std::ceil;
  using std::ceilf;
  using std::ceill;
  using std::copysign;
  using std::copysignf;
  using std::copysignl;
  using std::cos;
  using std::cosf;
  using std::cosh;
  using std::coshf;
  using std::coshl;
  using std::cosl;
  using std::double_t;
  using std::erf;
  using std::erfc;
  using std::erfcf;
  using std::erfcl;
  using std::erff;
  using std::erfl;
  using std::exp;
  using std::exp2;
  using std::exp2f;
  using std::exp2l;
  using std::expf;
  using std::expl;
  using std::expm1;
  using std::expm1f;
  using std::expm1l;
  using std::fabs;
  using std::fabsf;
  using std::fabsl;
  using std::fdim;
  using std::fdimf;
  using std::fdiml;
  using std::float_t;
  using std::floor;
  using std::floorf;
  using std::floorl;
  using std::fma;
  using std::fmaf;
  using std::fmal;
  using std::fmax;
  using std::fmaxf;
  using std::fmaxl;
  using std::fmin;
  using std::fminf;
  using std::fminl;
  using std::fmod;
  using std::fmodf;
  using std::fmodl;
  using std::fpclassify;
  using std::frexp;
  using std::frexpf;
  using std::frexpl;
  using std::hypot;
  using std::hypotf;
  using std::hypotl;
  using std::ilogb;
  using std::ilogbf;
  using std::ilogbl;
  using std::isfinite;
  using std::isgreater;
  using std::isgreaterequal;
  using std::isinf;
  using std::isless;
  using std::islessequal;
  using std::islessgreater;
  using std::isnan;
  using std::isnormal;
  using std::isunordered;
  using std::ldexp;
  using std::ldexpf;
  using std::ldexpl;



  using std::lgamma;
  using std::lgammaf;
  using std::lgammal;
  using std::llrint;
  using std::llrintf;
  using std::llrintl;
  using std::llround;
  using std::llroundf;
  using std::llroundl;
  using std::log;
  using std::log10;
  using std::log10f;
  using std::log10l;
  using std::log1p;
  using std::log1pf;
  using std::log1pl;
  using std::log2;
  using std::log2f;
  using std::log2l;
  using std::logb;
  using std::logbf;
  using std::logbl;
  using std::logf;
  using std::logl;
  using std::lrint;
  using std::lrintf;
  using std::lrintl;
  using std::lround;
  using std::lroundf;
  using std::lroundl;
  using std::modf;
  using std::modff;
  using std::modfl;
  using std::nan;
  using std::nanf;
  using std::nanl;
  using std::nearbyint;
  using std::nearbyintf;
  using std::nearbyintl;
  using std::nextafter;
  using std::nextafterf;
  using std::nextafterl;
  using std::nexttoward;
  using std::nexttowardf;
  using std::nexttowardl;
  using std::pow;
  using std::powf;
  using std::powl;
  using std::remainder;
  using std::remainderf;
  using std::remainderl;
  using std::remquo;
  using std::remquof;
  using std::remquol;
  using std::rint;
  using std::rintf;
  using std::rintl;
  using std::round;
  using std::roundf;
  using std::roundl;
  using std::scalbln;
  using std::scalblnf;
  using std::scalblnl;
  using std::scalbn;
  using std::scalbnf;
  using std::scalbnl;
  using std::signbit;
  using std::sin;
  using std::sinf;
  using std::sinh;
  using std::sinhf;
  using std::sinhl;
  using std::sinl;
  using std::sqrt;
  using std::sqrtf;
  using std::sqrtl;
  using std::tan;
  using std::tanf;
  using std::tanh;
  using std::tanhf;
  using std::tanhl;
  using std::tanl;
  using std::tgamma;
  using std::tgammaf;
  using std::tgammal;
  using std::trunc;
  using std::truncf;
  using std::truncl;
# 421 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
}


export
{
  using std::jmp_buf;
  using std::longjmp;

}


export
{
  using std::raise;
  using std::sig_atomic_t;
  using std::signal;

}


export
{
  using std::va_list;

}


export
{
  using std::max_align_t;
  using std::nullptr_t;
  using std::ptrdiff_t;
  using std::size_t;
# 470 "/usr/local/include/c++/16.1.0/bits/std.compat.cc"
}


export
{
  using std::int8_t;
  using std::int16_t;
  using std::int32_t;
  using std::int64_t;
  using std::int_fast16_t;
  using std::int_fast32_t;
  using std::int_fast64_t;
  using std::int_fast8_t;
  using std::int_least16_t;
  using std::int_least32_t;
  using std::int_least64_t;
  using std::int_least8_t;
  using std::intmax_t;
  using std::intptr_t;
  using std::uint8_t;
  using std::uint16_t;
  using std::uint32_t;
  using std::uint64_t;
  using std::uint_fast16_t;
  using std::uint_fast32_t;
  using std::uint_fast64_t;
  using std::uint_fast8_t;
  using std::uint_least16_t;
  using std::uint_least32_t;
  using std::uint_least64_t;
  using std::uint_least8_t;
  using std::uintmax_t;
  using std::uintptr_t;
}


export
{
  using std::clearerr;
  using std::fclose;
  using std::feof;
  using std::ferror;
  using std::fflush;
  using std::fgetc;
  using std::fgetpos;
  using std::fgets;
  using std::FILE;
  using std::fopen;
  using std::fpos_t;
  using std::fprintf;
  using std::fputc;
  using std::fputs;
  using std::fread;
  using std::freopen;
  using std::fscanf;
  using std::fseek;
  using std::fsetpos;
  using std::ftell;
  using std::fwrite;
  using std::getc;
  using std::getchar;
  using std::perror;
  using std::printf;
  using std::putc;
  using std::putchar;
  using std::puts;
  using std::remove;
  using std::rename;
  using std::rewind;
  using std::scanf;
  using std::setbuf;
  using std::setvbuf;
  using std::size_t;
  using std::snprintf;
  using std::sprintf;
  using std::sscanf;
  using std::tmpfile;

  using std::tmpnam;

  using std::ungetc;
  using std::vfprintf;
  using std::vfscanf;
  using std::vprintf;
  using std::vscanf;
  using std::vsnprintf;
  using std::vsprintf;
  using std::vsscanf;
}


export
{
  using std::_Exit;
  using std::abort;
  using std::abs;

  using std::aligned_alloc;


  using std::at_quick_exit;

  using std::atexit;
  using std::atof;
  using std::atoi;
  using std::atol;
  using std::atoll;
  using std::bsearch;
  using std::calloc;
  using std::div;
  using std::div_t;
  using std::exit;
  using std::free;
  using std::getenv;
  using std::labs;
  using std::ldiv;
  using std::ldiv_t;
  using std::llabs;
  using std::lldiv;
  using std::lldiv_t;
  using std::malloc;

  using std::mblen;
  using std::mbstowcs;
  using std::mbtowc;

  using std::qsort;

  using std::quick_exit;

  using std::rand;
  using std::realloc;
  using std::size_t;
  using std::srand;
  using std::strtod;
  using std::strtof;
  using std::strtol;
  using std::strtold;
  using std::strtoll;
  using std::strtoul;
  using std::strtoull;
  using std::system;

  using std::wcstombs;
  using std::wctomb;

}


export
{
  using std::memchr;
  using std::memcmp;
  using std::memcpy;
  using std::memmove;
  using std::memset;
  using std::size_t;
  using std::strcat;
  using std::strchr;
  using std::strcmp;
  using std::strcoll;
  using std::strcpy;
  using std::strcspn;
  using std::strerror;
  using std::strlen;
  using std::strncat;
  using std::strncmp;
  using std::strncpy;
  using std::strpbrk;
  using std::strrchr;
  using std::strspn;
  using std::strstr;
  using std::strtok;
  using std::strxfrm;
}


export
{
  using std::asctime;
  using std::clock;
  using std::clock_t;
  using std::ctime;
  using std::difftime;
  using std::gmtime;
  using std::localtime;
  using std::mktime;
  using std::size_t;
  using std::strftime;
  using std::time;
  using std::time_t;
  using std::tm;

  using std::timespec;
  using std::timespec_get;

}


export
{

  using std::mbrtoc8;
  using std::c8rtomb;


  using std::mbrtoc16;
  using std::c16rtomb;
  using std::mbrtoc32;
  using std::c32rtomb;

}



export
{
  using std::btowc;
  using std::fgetwc;
  using std::fgetws;
  using std::fputwc;
  using std::fputws;
  using std::fwide;
  using std::fwprintf;
  using std::fwscanf;
  using std::getwc;
  using std::getwchar;
  using std::mbrlen;
  using std::mbrtowc;
  using std::mbsinit;
  using std::mbsrtowcs;
  using std::mbstate_t;
  using std::putwc;
  using std::putwchar;
  using std::size_t;
  using std::swprintf;
  using std::swscanf;
  using std::tm;
  using std::ungetwc;
  using std::vfwprintf;

  using std::vfwscanf;


  using std::vswprintf;


  using std::vswscanf;

  using std::vwprintf;

  using std::vwscanf;

  using std::wcrtomb;
  using std::wcscat;
  using std::wcschr;
  using std::wcscmp;
  using std::wcscoll;
  using std::wcscpy;
  using std::wcscspn;
  using std::wcsftime;
  using std::wcslen;
  using std::wcsncat;
  using std::wcsncmp;
  using std::wcsncpy;
  using std::wcspbrk;
  using std::wcsrchr;
  using std::wcsrtombs;
  using std::wcsspn;
  using std::wcsstr;
  using std::wcstod;

  using std::wcstof;

  using std::wcstok;
  using std::wcstol;

  using std::wcstold;
  using std::wcstoll;

  using std::wcstoul;

  using std::wcstoull;

  using std::wcsxfrm;
  using std::wctob;
  using std::wint_t;
  using std::wmemchr;
  using std::wmemcmp;
  using std::wmemcpy;
  using std::wmemmove;
  using std::wmemset;
  using std::wprintf;
  using std::wscanf;
}




export
{
  using std::iswalnum;
  using std::iswalpha;

  using std::iswblank;

  using std::iswcntrl;
  using std::iswctype;
  using std::iswdigit;
  using std::iswgraph;
  using std::iswlower;
  using std::iswprint;
  using std::iswpunct;
  using std::iswspace;
  using std::iswupper;
  using std::iswxdigit;
  using std::towctrans;
  using std::towlower;
  using std::towupper;
  using std::wctrans;
  using std::wctrans_t;
  using std::wctype;
  using std::wctype_t;
  using std::wint_t;
}
