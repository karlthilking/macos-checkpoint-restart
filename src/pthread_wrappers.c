/* pthread_wrappers.c */
#define _XOPEN_SOURCE
#include <stdio.h>
#include <assert.h>
#include "pthread_wrappers.h"
#include "types.h"
#include "pac.h"

// int pthread(pthread_t thread, int sig)
// mov    x20, x0       ; pthread_t argument
// ...
// mrs    x22, TPIDRRO_EL0
// sub    x8, x22, #0xe0
// cmp    x8, x0
// cbz    w9, 0x19246eb80           ; <+128>
// b.eq   0x19246ebdc               ; <+220>
// ...
// ldr    x8, [x20]
// adrp   x9, 425542
// ldr    x9, [x9, #0x68]
// eor    x16, x9, x8
// mov    x17, #0x5b9
// autdb  x16, x17
// mov    x17, x16
// xpacd  x17

/**
 *
 * Find internal libpthread xor cookie value:
 *
 * tls_base     = tpidrro_el0
 * pthread_self = tls_base - 0xe0
 * signed_ptr   = pacdb(pthread_self, 0x5b9)    <- x16
 * slot         = ((u64 *)pthread_self)[0]      <- x8
 * cookie       = slot ^ signed_ptr             <- x9
 *
 * Save xor cookie, then at restart:
 *
 * tls_base     = tpidrro_el0
 * pthread_self = tls_base - 0xe0
 * slot         = ((u64 *)pthread_self)[0]
 * stale_ptr    = cookie ^ slot
 * new_ptr      = pac_strip_resign(stale_ptr)
 * *(u64 *)slot = new_ptr ^ cookie
 *
 */

static uintptr_t xor_cookie;

void pthread_init()
{
        uintptr_t tls, self, signed_ptr, slot;
        
        asm volatile("mrs %0, tpidrro_el0" : "=r" (tls) :: "memory");
        
        /**
         * Find libpthread xor cookie by signing pthread_self pointer
         * with libpthread constant discriminator xor'd with the
         * slot value (offset 0 of pthread_self pointer). Then, the
         * cookie will used be at restart to calculate the new slot value
         * that reconstructs the pthread_self signed pointer (given the
         * DB key in the new process).
         *
         * pthread_self = tls - PTHREAD_SELF_TLS_OFFSET
         * slot = pthread_self[0]
         * signed_ptr = pacdb(pthread_self, PTHREAD_SELF_DISCRIMINATOR)
         *
         * xor_cookie = slot ^ signed_ptr
         */
        self            = tls - PTHREAD_SELF_TLS_OFFSET;
        slot            = *(uintptr_t *)self;
        signed_ptr      = self;

        PACDB(signed_ptr, (u64)PTHREAD_SELF_DISCRIMINATOR);
        xor_cookie = slot ^ signed_ptr;
}

void pthread_fixup()
{
        uintptr_t tls, *slot_ptr, old_ptr, new_ptr;

        asm volatile("mrs %0, tpidrro_el0" : "=r" (tls) :: "memory");
        
        /**
         * Find new pthread_self pointer address signed with DB key
         * and reconstruct slot value with signed_ptr ^ cookie.
         * Then, libpthread will use the slot value and cookie to
         * reconstruct the signed pointer at runtime, and because the
         * slot has been fixed-up, the autdb will succeed.
         *
         * slot_ptr     = (uintptr_t *)(tpidrro_el0 - 0xe0)
         * old_ptr      = xor_cookie ^ slot_ptr[0]
         *
         * new_ptr      = pac_strip_and_resign(old_ptr)
         * slot_ptr[0]  = new_ptr ^ xor_cookie
         */
        slot_ptr        = (uintptr_t *)(tls - PTHREAD_SELF_TLS_OFFSET);
        old_ptr         = xor_cookie ^ slot_ptr[0];

        new_ptr = old_ptr;
        XPACD(new_ptr);
        PACDB(new_ptr, (u64)PTHREAD_SELF_DISCRIMINATOR);

        slot_ptr[0] = new_ptr ^ xor_cookie;
}
