/* libckpt.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>
#include <assert.h>
#include <ucontext.h>
#include <unistd.h>
#include <signal.h>
#include "ckpt.h"
#include "writeckpt.h"
#include "vm_region.h"
#include "pac.h"
#include "pthread_wrappers.h"
#include "file_wrappers.h"

extern uintptr_t __stack_chk_guard;

void precheckpoint(void)
{
#if defined(__arm64e__)
        __pthread_cookie();
#endif
        if (save_file_states() < 0)
                fprintf(stderr, "Failed save file/fd state\n");
}

void postrestart(void)
{
        ckpt_vm_deallocate_regions();
#if defined(__arm64e__)
        __pthread_slot_fixup();
#endif
        setup();
        if (reopen_files() < 0)
                fprintf(stderr, "Failed to restore file/fd state\n");
}

void ckpt_handler(int sig, siginfo_t *info, void *uctx)
{
        ckpt_header_t           headers[MAX_CKPT_HEADERS];
        ckpt_vm_region_t        regions[MAX_CKPT_VM_REGIONS];
        ckpt_context_t          ctx;
        ckpt_metadata_t         meta;
        static int              restart;
        static uintptr_t        tls;

        bzero(&meta, sizeof(meta));
        precheckpoint();
        restart = 0;
        if (getcontext(&ctx) < 0)
                err(EXIT_FAILURE, "getcontext");
        
        if (restart) {
                ckpt_context_t  *prevctx;
                uintptr_t       check;
                
                asm volatile("mrs %0, tpidrro_el0" : "=r" (check));
                if (check != tls) {
                        fprintf(stderr,
                                " tpiddro_el0 before checkpoint: 0x%lx\n"
                                "     tpidrro_el0 after restart: 0x%lx\n",
                                tls, check);
                        __builtin_trap();
                }

                restart = 0;
                prevctx = (ckpt_context_t *)uctx;
                
                postrestart();

                /**
                 * On restart, return from signal handler via setcontext
                 * to avoid _sigtramp -> __sigreturn path.
                 */
                pac_patch_context(prevctx);
                if (setcontext(prevctx) < 0)
                        err(EXIT_FAILURE, "setcontext");
        }
        
        asm volatile("mrs %0, tpidrro_el0" : "=r" (tls));
        restart = 1;

        /* Save shared cache information to checkpoint metadata */
        if (shared_cache_get_info(&meta.shared_cache_info) < 0) {
                fprintf(stderr, 
                        "Failed to get shared cache info, "
                        "aborting checkpoint...\n");
                return;
        }
                

        /* Enumerate and save memory regions */
        meta.nr_regions = ckpt_vm_save_regions(regions);
        meta.nr_headers += meta.nr_regions;
        for (u32 i = 0; i < meta.nr_regions; i++)
                headers[i] = CKPT_VM_REGION_HEADER;
        
        meta.nr_contexts                = 1;
        headers[meta.nr_headers++]      = CKPT_CONTEXT_HEADER;
        
        if (write_ckpt(&meta, headers, regions, &ctx) < 0) {
                fprintf(stderr, "Failed to write checkpoint file "
                                "(%d-ckpt.dat)\n", getpid());
        } else {
                printf("Checkpoint written to %d-ckpt.dat\n", getpid());
        }

        return;
}

__attribute__((constructor))
void setup()
{
        struct sigaction sa;

        /* Register ckpt_handler to run on SIGUSR2 */
        sigemptyset(&sa.sa_mask);
        sa.sa_flags     = SA_SIGINFO | SA_RESTART;
        sa.sa_sigaction = ckpt_handler;
        sigaction(SIGUSR2, &sa, NULL);
}

__attribute__((destructor))
void cleanup()
{
        return;
}
