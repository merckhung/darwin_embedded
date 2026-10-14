#ifndef __MACTYPES__
#define __MACTYPES__

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_extension
#define __has_extension(x) 0
#endif
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#ifndef __building_module
#define __building_module(x) 0
#endif

#ifndef typeof
#define typeof __typeof__
#endif

#define _Nullable
#define _Nonnull
#define _Returns_twice
#define __WATCHOS_PROHIBITED
#define __TVOS_PROHIBITED
#define OS_EXPORT
#define OS_OBJECT_RETURNS_RETAINED
#define OS_WORKGROUP_EXPORT
#define OS_WORKGROUP_RETURNS_RETAINED
#define OS_SWIFT_NAME(x)
#define API_AVAILABLE(...)
#define SPI_AVAILABLE(...)
#define OS_REFINED_FOR_SWIFT
#define OS_ENUM(name, type, ...) enum { __VA_ARGS__ }; typedef type name;

#ifdef __cplusplus
#ifndef _Static_assert
#define _Static_assert static_assert
#endif
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint32_t UInt32;
typedef int32_t  SInt32;
typedef uint64_t UInt64;
typedef int64_t  SInt64;
typedef uint16_t UInt16;
typedef int16_t  SInt16;
typedef uint8_t  UInt8;
typedef int8_t   SInt8;
typedef uint8_t  Byte;
typedef bool     Boolean;
typedef uint64_t AbsoluteTime;
typedef uint32_t ast_t;
typedef uint8_t mach_msg_qos_t;
typedef uint64_t kqueue_id_t;
typedef void (*IOInterruptHandler)(void *target, void *refCon, void *nub, int source);
typedef void (*ipi_handler_t)(void);
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif
