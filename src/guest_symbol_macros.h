#pragma once

// Each recompiled guest is built with its own crackdown_guest_symbols.h, which renames the guest's functions and image
// tables by macro (cmake/crackdown_guests.cmake writes it). These definitions replace the generated ones so the
// __imp__ half of a function follows that rename; the kernel imports (__imp__Name) are left alone.
// cmake/crackdown_guests.cmake checks the generated header still defines these two macros as written here.

#define CRACKDOWN_GUEST_JOIN_(a, b) a##b
#define CRACKDOWN_GUEST_JOIN(a, b) CRACKDOWN_GUEST_JOIN_(a, b)
#define CRACKDOWN_GUEST_STR_(x) #x
#define CRACKDOWN_GUEST_STR(x) CRACKDOWN_GUEST_STR_(x)

#define DECLARE_REX_FUNC(name) \
  REX_EXTERN(name);            \
  REX_EXTERN(CRACKDOWN_GUEST_JOIN(__imp__, name))

#if defined(__APPLE__)
// Mach-O cannot provide overridable aliases. Keep the SDK's weak tail thunk,
// applying the guest prefix to both entry points before declaring them.
#define DEFINE_REX_FUNC(name)                                                     \
  REX_EXTERN(CRACKDOWN_GUEST_JOIN(__imp__, name));                                  \
  __attribute__((weak, noinline)) extern "C" REX_FUNC(name) {                       \
    [[clang::musttail]] return CRACKDOWN_GUEST_JOIN(__imp__, name)(ctx, base);        \
  }                                                                              \
  REX_EXTERN(CRACKDOWN_GUEST_JOIN(__imp__, name))
#else
#define DEFINE_REX_FUNC(name)                                                     \
  __attribute__((alias("__imp__" CRACKDOWN_GUEST_STR(name))))                     \
  __attribute__((weak, noinline)) extern "C" REX_FUNC(name);                      \
  REX_EXTERN(CRACKDOWN_GUEST_JOIN(__imp__, name))
#endif
