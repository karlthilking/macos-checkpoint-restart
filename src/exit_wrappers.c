/* exit_wrappers.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include "types.h"
#include "pac.h"
#include "exit_wrappers.h"

extern void (*__cleanup)(void);

void __exit(int status)
{
        if (PTRAUTH_SIGNED((u64)__cleanup)) {
                /**
                 * If the __cleanup function pointer was signed, then
                 * it was the signed with IB key, possibly the IB key
                 * of the previous process if this is a restart.
                 *
                 * Thus, the function pointer needs to be stripped and
                 * re-signed with the current IB key and implementation
                 * defined modifier to avoid PAC failure.
                 *
                 * modifier = &__cleanup | (constant << 48)
                 *
                 * The exact authentication sequence used:
                 *   mov x16, %[__cleanup]
                 *   mov x17, %[storage address of __cleanup]
                 *   movk x17, #0x211b, lsl #48
                 *   autib x16, x17 <- authenticate branch target
                 *   mov x17, x16
                 *   xpaci x17
                 *   cmp x16, x17 
                 *   b.ne <- abort/trap if neq
                 */
                pac_strip_resign(__cleanup, PAC_IB_KEY, 0x211b, 1);
        }

        exit(status);
}

void __abort(void)
{
        if (PTRAUTH_SIGNED((u64)__cleanup))
                pac_strip_resign(__cleanup, PAC_IB_KEY, 0x211b, 1);

        abort();
}
