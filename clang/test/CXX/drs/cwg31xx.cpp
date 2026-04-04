// RUN: %clang_cc1 -std=c++98 -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected
// RUN: %clang_cc1 -std=c++11 -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected
// RUN: %clang_cc1 -std=c++14 -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected
// RUN: %clang_cc1 -std=c++17 -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected
// RUN: %clang_cc1 -std=c++20 -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected
// RUN: %clang_cc1 -std=c++23 -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected
// RUN: %clang_cc1 -std=c++2c -fexceptions -fcxx-exceptions -pedantic-errors %s -verify-directives -verify=expected

#if __cplusplus < 201103L
// expected-no-diagnostics
#endif

namespace cwg3106 { // cwg3106: 2.7
#if __cplusplus >= 201103L
const char str[9] = R"(\u{1234})";
#endif
} // namespace cwg3106

namespace cwg3179 { // cwg3179: 23 tentatively ready 2026-04-30
#if __cplusplus >= 201103L
  template<class> using void_t = void;
  template<class T> struct S {
    void f(void_t<T*>);
    // expected-error@-1 {{'void' as parameter must not involve template parameters}}

    using X = int(void_t<T*>);
    // expected-error@-1 {{'void' as parameter must not involve template parameters}}
  };
  template<class T> void g(decltype((void)(T*)0));
  // expected-error@-1 {{'void' as parameter must not involve template parameters}}
#endif
#if __cplusplus >= 202002L
  template<class T> bool v = requires(void_t<T>) { true; };
  // expected-error@-1 {{'void' as parameter must not involve template parameters}}
#endif
} // namespace cwg3179
