#ifndef _STUFF_THREAD_STATES_H_
#define _STUFF_THREAD_STATES_H_

#include <stdint.h>

/* ARM Thread States */
struct __darwin_arm_thread_state {
    uint32_t __r[13];
    uint32_t __sp;
    uint32_t __lr;
    uint32_t __pc;
    uint32_t __cpsr;
};
#ifndef arm_thread_state_t
typedef struct __darwin_arm_thread_state arm_thread_state_t;
#endif

struct __darwin_arm_thread_state64 {
    uint64_t __x[29];
    uint64_t __fp;
    uint64_t __lr;
    uint64_t __sp;
    uint64_t __pc;
    uint32_t __cpsr;
    uint32_t __pad;
};
#ifndef arm_thread_state64_t
typedef struct __darwin_arm_thread_state64 arm_thread_state64_t;
#endif

struct __darwin_arm_exception_state64 {
    uint64_t __far;
    uint32_t __esr;
    uint32_t __exception;
};
#ifndef arm_exception_state64_t
typedef struct __darwin_arm_exception_state64 arm_exception_state64_t;
#endif

#ifndef ARM_THREAD_STATE
#define ARM_THREAD_STATE 1
#endif
#ifndef ARM_THREAD_STATE64
#define ARM_THREAD_STATE64 6
#endif
#ifndef ARM_EXCEPTION_STATE64
#define ARM_EXCEPTION_STATE64 7
#endif

#ifndef ARM_THREAD_STATE_COUNT
#define ARM_THREAD_STATE_COUNT (sizeof(struct __darwin_arm_thread_state) / sizeof(uint32_t))
#endif
#ifndef ARM_THREAD_STATE64_COUNT
#define ARM_THREAD_STATE64_COUNT (sizeof(struct __darwin_arm_thread_state64) / sizeof(uint32_t))
#endif
#ifndef ARM_EXCEPTION_STATE64_COUNT
#define ARM_EXCEPTION_STATE64_COUNT (sizeof(struct __darwin_arm_exception_state64) / sizeof(uint32_t))
#endif

#ifndef _STRUCT_ARM_THREAD_STATE
#define _STRUCT_ARM_THREAD_STATE struct __darwin_arm_thread_state
#endif

#ifndef _STRUCT_ARM_THREAD_STATE64
#define _STRUCT_ARM_THREAD_STATE64 struct __darwin_arm_thread_state64
#endif

#ifndef _STRUCT_ARM_EXCEPTION_STATE64
#define _STRUCT_ARM_EXCEPTION_STATE64 struct __darwin_arm_exception_state64
#endif

#endif /* _STUFF_THREAD_STATES_H_ */
