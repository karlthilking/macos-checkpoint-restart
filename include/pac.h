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

enum {
        PAC_IA_KEY = 0u, // Instruction A Key   (process independent)
        PAC_IB_KEY = 1u, // Instruction B Key   (process dependent)
        PAC_DA_KEY = 2u, // Data A Key          (process independent)
        PAC_DB_KEY = 3u  // Data B Key          (process dependent)
};

enum {
        ARM64_FP = 29, ARM64_LR = 30, ARM64_SP = 31, ARM64_PC = 32
};
#define ARM_THREAD_STATE64_GPREGCOUNT   29

/**
 * ckpt_callframe_t:
 *  Stack frame live during checkpoint which contains a signed
 *  link register in its frame record.
 *
 * @fp:         Frame pointer in frame record (points to previous frame)
 * @lr:         Link register in frame record (signed with pac* instr)
 * @flags:      Metadata, additional information
 */
typedef struct ckpt_callframe {
        u64     fp;
        u64     lr;
        u64     flags;
} ckpt_callframe_t;

#define FRAME_LR_SIGNED(__cf) \
        ((__cf)->flags & 0x1)
#define FRAME_PAC_KEY(__cf) \
        (((__cf)->flags >> 1) & 0x3)
#define FRAME_PAC_MODIFIER(__cf) \
        ((__cf)->flags >> 3)

#define FRAME_SET_FLAGS(__cf, __sign, __key, __mod) \
        ((__cf)->flags = \
        (((__mod) << 3) | ((__key) << 1) | (__sign)))

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

/**
 * ckpt_context_t:
 *  Register context including ucontext_t struct and additional
 *  pointer authentication related information including modifier values
 *  and pointer bitmap.
 *
 * @uc:         General user context information
 * @modifiers:  Modifier values for pointers which were signed
 * @bitmap:     Bitmap marking which pointers were signed
 *
* ****** TO-DO ******
 *  - Only certain registers are saved during getcontext() so it is not
 *    necessary to make space for 33 possible modifiers
 */
typedef struct ckpt_context {
        ucontext_t      uc;
        u64             modifiers[33];
        u64             bitmap;
} ckpt_context_t;

static inline void ucontext_memcpy(ucontext_t *dst, const ucontext_t *src)
{
        /**
         * Copy ucontext_t bytes from source to destination.
         * Additionally, copy data pointed to by src->uc_mcontext to
         * dst->__mcontext_data because uc_mcontext is a pointer.
         */
        memcpy(dst, src, sizeof(ucontext_t));
        memcpy(&dst->__mcontext_data, src->uc_mcontext,
               sizeof(dst->__mcontext_data));
        
        /* Make dst->uc_mcontext point to dst->__mcontext_data */
        dst->uc_mcontext = (mcontext_t)&dst->__mcontext_data;
}

void pac_patch_ucontext(ucontext_t *);

/* Strip signed registers in arm64e context */
int pac_strip_context(ckpt_context_t *);
/* Re-sign previous signed registers in arm64e context */
void pac_sign_context(ckpt_context_t *);

/* Strip link registers in live stack frame records */
u32 pac_strip_frames(ckpt_callframe_t *, u64 *);
/* Re-sign link registers in live stack frame records */
void pac_sign_frames(const ckpt_callframe_t *, u64 *, const u32);

void pac_check();

#endif // __CKPT_PAC_H__
