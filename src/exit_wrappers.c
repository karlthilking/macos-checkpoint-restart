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
        u64 raw, mod;
        
        if (PTRAUTH_SIGNED((u64)__cleanup)) {
                /**
                 * If __cleanup function pointer is signed, then
                 * it was possibly signed with the IB key from a
                 * previous process.
                 *
                 * Strip the function pointer and re-sign it with
                 * the current IB key for this process.
                 *
                 * The modifier used for signing and authenticating is
                 * the storage address of the function pointer blended
                 * with the 16-bit immediate value 0x211b, i.e.
                 * libsystem_c.dylib is using ptrauth_blend_discriminator
                 * to sign function pointers (storage address + constant
                 * discriminator).
                 */
                XPACI(__cleanup);
                raw = (u64)__cleanup;
                mod = (u64)&__cleanup;
                
                asm volatile(
                        "movk %0, #0x211b, lsl #48"
                        : "+r" (mod)
                        :
                        : "memory"
                );
                
                PACIB(raw, mod);
                *(u64 *)&__cleanup = raw;
        }

        exit(status);
}
