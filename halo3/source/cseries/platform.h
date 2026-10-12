#ifndef __PLATFORM_H__
#define __PLATFORM_H__
#pragma once

/* ---------- headers */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- constants */

// standard _Pragma rather than __pragma, which clang only accepts with MS extensions enabled (Windows)
#define _PRAGMA(x) _Pragma(#x)

#define _WARNINGS_PUSH() _PRAGMA(clang diagnostic push)
#define _WARNINGS_POP() _PRAGMA(clang diagnostic pop)
#define WARNINGS_PUSH() _WARNINGS_PUSH() (void)(0)
#define WARNINGS_POP() _WARNINGS_POP() (void)(0)

#define _IGNORE_WARNING(warning) _PRAGMA(clang diagnostic ignored warning)
#define IGNORE_WARNING(warning) _IGNORE_WARNING(warning); (void)(0)

#define _IGNORE_WARNING_PUSH(warning) _WARNINGS_PUSH(); _PRAGMA(clang diagnostic ignored warning)
#define IGNORE_WARNING_PUSH(warning) _IGNORE_WARNING_PUSH(warning); (void)(0)
#define _IGNORE_WARNING_POP() _WARNINGS_POP();
#define IGNORE_WARNING_POP() _IGNORE_WARNING_POP(); (void)(0)

#define _IGNORE_ALL_WARNINGS_PUSH() _IGNORE_WARNING_PUSH("-Weverything")
#define IGNORE_ALL_WARNINGS_PUSH() _IGNORE_ALL_WARNINGS_PUSH(); (void)(0)
#define _IGNORE_ALL_WARNINGS_POP() _WARNINGS_POP()
#define IGNORE_ALL_WARNINGS_POP() _IGNORE_ALL_WARNINGS_POP(); (void)(0)

// opaque enum declaration with a fixed underlying type (int32_t, matching MSVC's implicit int)
// so it is standard C++ and not a Microsoft extension
#define FORWARD_DECLARE_ENUM(_name) \
enum _name : int32_t;


#define ALWAYS_INLINE __attribute__((always_inline))
#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline ALWAYS_INLINE
#else
#define FORCE_INLINE inline ALWAYS_INLINE
#endif

template <typename t_type>
constexpr bool is_enum = __is_enum(t_type);

/* ---------- definitions */

/* ---------- prototypes */

/* ---------- globals */

/* ---------- public code */

/* ---------- private code */

#endif // __PLATFORM_H__
