/* pac.h */
#ifndef __CKPT_PAC_H__
#define __CKPT_PAC_H__
#define _XOPEN_SOURCE
#include <ucontext.h>
#include <assert.h>
#include <mach/mach.h>
#include <mach/arm/_structs.h>
#include "types.h"

#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
/**
 * Access/modify ucontext_t, mcontext_t register values for 
 * arm64e version of struct __darwin_arm_thread_state64 
 * in struct __darwin_mcontext64
 */
#define get_ucontext_lr(__uctx) \
        ((u64)((__uctx)->uc_mcontext->__ss.__opaque_lr))
#define get_ucontext_fp(__uctx) \
        ((u64)((__uctx)->uc_mcontext->__ss.__opaque_fp))
#define get_ucontext_sp(__uctx) \
        ((u64)((__uctx)->uc_mcontext->__ss.__opaque_sp))
#define get_ucontext_flags(__uctx) \
        ((u32)((__uctx)->uc_mcontext->__ss.__opaque_flags))

#define set_ucontext_lr(__uctx, __lr) \
        ((__uctx)->uc_mcontext->__ss.__opaque_lr = (void *)(__lr))
#define set_ucontext_fp(__uctx, __fp) \
        ((__uctx)->uc_mcontext->__ss.__opaque_fp = (void *)(__fp))
#define set_ucontext_sp(__uctx, __sp) \
        ((__uctx)->uc_mcontext->__ss.__opaque_sp = (void *)(__sp))
#define set_ucontext_flags(__uctx, __flags) \
        ((__uctx)->uc_mcontext->__ss.__opaque_flags = (u32)(__flags))

#define get_mcontext_lr(__mctx) \
        ((u64)((__mctx)->__ss.__opaque_lr))
#define get_mcontext_fp(__mctx) \
        ((u64)((__mctx)->__ss.__opaque_fp))
#define get_mcontext_sp(__mctx) \
        ((u64)((__mctx)->__ss.__opaque_sp))
#define get_mcontext_flags(__mctx)      \
        ((u32)((__mctx)->__ss.__opaque_flags))

#define set_mcontext_lr(__mctx, __lr) \
        ((__mctx)->__ss.__opaque_lr = (void *)(__lr))
#define set_mcontext_fp(__mctx, __fp) \
        ((__mctx)->__ss.__opaque_fp = (void *)(__fp))
#define set_mcontext_sp(__mctx, __sp) \
        ((__mctx)->__ss.__opaque_sp = (void *)(__sp))
#define set_mcontext_flags(__mctx, __flags) \
        ((__mctx)->__ss.__opaque_flags = (u32)(__flags))

#else // __arm64e_ || __ARM_FEATURE_PAUTH
/**
 * Access/modify ucontext_t, mcontext_t register values for arm64 version
 * of struct __darwin_arm_thread_state64 in struct __darwin_mcontext64
 */
#define get_ucontext_lr(__uctx) \
        ((__uctx)->uc_mcontext->__ss.__lr) 
#define get_ucontext_fp(__uctx) \
        ((__uctx)->uc_mcontext->__ss.__fp)
#define get_ucontext_sp(__uctx) \
        ((__uctx)->uc_mcontext->__ss.__sp)
#define get_ucontext_flags(__uctx) \
        ((__uctx)->uc_mcontext->__ss.__pad)

#define set_ucontext_lr(__uctx, lr) \
        ((__uctx)->uc_mcontext->__ss.__lr = (u64)(lr))
#define set_ucontext_fp(__uctx, fp) \
        ((__uctx)->uc_mcontext->__ss.__fp = (u64)(fp))
#define set_ucontext_sp(__uctx, sp) \
        ((__uctx)->uc_mcontext->__ss.__sp = (u64)(sp))
#define set_ucontext_flags(__uctx, flags) \
        ((__uctx)->uc_mcontext->__ss.__pad = (u32)(flags))

#define get_mcontext_lr(__mctx) \
        ((__mctx)->__ss.__lr)
#define get_mcontext_fp(__mctx) \
        ((__mctx)->__ss.__fp)
#define get_mcontext_sp(__mctx) \
        ((__mctx)->__ss.__sp)
#define get_mcontext_flags(__mctx) \
        ((__mctx)->__ss.__pad)

#define set_mcontext_lr(__mctx, lr) \
        ((__mctx)->__ss.__lr = (u64)(lr))
#define set_mcontext_fp(__mctx, fp) \
        ((__mctx)->__ss.__fp = (u64)(fp))
#define set_mcontext_sp(__mctx, sp) \
        ((__mctx)->__ss.__sp = (u64)(sp))
#define set_mcontext_flags(__mctx, flags) \
        ((__mctx)->__ss.__pad = (u32)(flags))

#endif

/**
 * Constant discriminator values used for signing and authenticating
 * saved/restored registers in getcontext() and setcontext()
 */
#define FP_DISCRIMINATOR        ((u64)0x4517)   // =17687
#define SP_DISCRIMINATOR        ((u64)0xcbed)   // =52205
#define LR_DISCRIMINATOR        ((u64)0x77d3)   // =30675

/**
 * Flag values/bits in struct __darwin_arm_thread_state64 to manipulate
 * behavior of _setcontext (called from setcontext(ucontext_t *))
 *
 * If (ts->__opaque_flags & LR_SIGNED_WITH_IB), _setcontext will
 * not manually authenticate the link register with its own constant
 * discriminator (assuming it was already signed when obtained from
 * getcontext).
 */
#define LR_SIGNED_WITH_IB                                       0x2
#define LR_SIGNED_WITH_IB_BIT                                   0x1
#define __DARWIN_ARM_THREAD_STATE64_FLAGS_NO_PTRAUTH            0x1
#define __DARWIN_ARM_THREAD_STATE64_FLAGS_IB_SIGNED_LR          0x2
#define __DARWIN_ARM_THREAD_STATE64_FLAGS_KERNEL_SIGNED_PC      0x4
#define __DARWIN_ARM_THREAD_STATE64_FLAGS_KERNEL_SIGNED_LR      0x8
#define __DARWIN_ARM_THREAD_STATE64_USER_DIVERSIFIER_MASK       0xff000000

#if defined(__arm64e__) || defined(__ARM_FEATURE_PAUTH)
# define PAC_ENABLED 1
#endif

#define PAC_BIT_MASK   (0xFF7F000000000000ULL)

/**
 * Determine if pointer is signed (high bits other than bit 55 are set).
 * Bit 55 is not written by PAC signature to determine between userspace
 * and kernel space addresses.
 */
#define PTRAUTH_SIGNED(__ptr) \
        (((__ptr) & ~(1ull << 55)) & PAC_BIT_MASK)

/* If bits 62-61 are 10 or 01, aut* instruction failed */
#define PTRAUTH_FAILED(__ptr) \
        (((__ptr) >> 61) == 0b01 || ((__ptr) >> 62) == 0b10)

/**
 * PAC instruction macros for:
 *
 *  xpaci, strip PAC signature for pointer to instr address
 *  xpacd, strip PAC signature for pointer to data address
 *
 *  pacia, sign pointer with IA key + modifier
 *  pacib, sign pointer with IB key + modifier
 *  pacda, sign pointer with DA key + modifier
 *  pacdb, sign pointer with DB key + modifier
 *
 *  autia, authenticate with IA key + modifier
 *  autib, authenticate with IB key + modifier
 *  autda, authenticate with DA key + modifier
 *  autdb, authenticate with DB key + modifier
 */
#define XPACI(__ptr) do {               \
        __asm__ __volatile__ (          \
                "xpaci %[ptr]"          \
                : [ptr] "+r" (__ptr)    \
                :                       \
                : "memory"              \
        );                              \
} while (0)

#define XPACD(__ptr) do {               \
        __asm__ __volatile__ (          \
                "xpacd %[ptr]"          \
                : [ptr] "+r" (__ptr)    \
                :                       \
                : "memory"              \
        );                              \
} while (0)

#define PACIA(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "pacia %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define PACIB(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "pacib %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define PACDA(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "pacda %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define PACDB(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "pacdb %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define AUTIA(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "autia %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define AUTIB(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "autib %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define AUTDA(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "autda %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define AUTDB(__ptr, __mod) do {        \
        __asm__ __volatile__ (          \
                "autdb %[ptr], %[mod]"  \
                : [ptr] "+r" (__ptr)    \
                : [mod] "r" (__mod)     \
                : "memory"              \
        );                              \
} while (0)

#define APIAKey 0
#define APIBKey 1
#define APDAKey 2
#define APDBKey 3

#define pac_strip_resign(__ptr, __key, __constant, __blend) do { \
        u64 __mod, __result;                                     \
        if ((__key) == APIBKey)                                  \
                XPACI((__ptr));                                  \
        else if ((__key) == APDBKey)                             \
                XPACD((__ptr));                                  \
        if ((__blend) != 0) {                                    \
                __mod =  (u64)&(__ptr);                          \
                __mod |= ((u64)(__constant) << 48);              \
        } else                                                   \
                __mod = (__constant);                            \
        __result = (u64)(__ptr);                                 \
        if ((__key) == APIBKey)                                  \
                PACIB(__result, __mod);                          \
        else if ((__key) == APDBKey)                             \
                PACDB(__result, __mod);                          \
        *(u64 *)&(__ptr) = __result;                             \
} while (0)

#define for_each_frame(__fp) \
        for (; __fp != NULL; __fp = (u64 *)__fp[0])

#define for_each_signed_frame(__fp) \
        for (__fp = first_signed_frame(__fp); __fp != NULL; \
             __fp = next_signed_frame(__fp))

static inline u64 *first_signed_frame(u64 *fp)
{
        for (; fp != NULL; fp = (u64 *)fp[0]) {
                if (PTRAUTH_SIGNED(fp[1]))
                        return fp;
        }

        return NULL;
}

static inline u64 *next_signed_frame(u64 *fp)
{
        for (fp = (u64 *)fp[0]; fp != NULL; fp = (u64 *)fp[0]) {
                if (PTRAUTH_SIGNED(fp[1]))
                        return fp;
        }

        return NULL;
}

typedef ucontext_t ckpt_context_t;

void pac_patch_context(ckpt_context_t *);
void pac_resign_frames(u64 *);

#endif // __CKPT_PAC_H__
