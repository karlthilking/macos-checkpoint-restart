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

extern uintptr_t __stack_chk_guard;

void ckpt_handler(int sig, siginfo_t *_, void *uctx)
{
        ckpt_header_t           headers[MAX_CKPT_HEADERS];
        ckpt_vm_region_t        regions[MAX_CKPT_VM_REGIONS];
        ckpt_callframe_t        frames[MAX_CKPT_CALLFRAMES];
        ckpt_context_t          ctx;
        ckpt_metadata_t         meta;
        u64                     *fp;
        ucontext_t              uc;
        static int              restart;
        
        meta.nr_headers = 0;
        restart         = 0;
        
        if (getcontext(&uc) < 0)
                err(EXIT_FAILURE, "%s: getcontext", __FILE__);
        
        if (restart) {
                restart         = 0;
                ucontext_t *ucp = (ucontext_t *)uctx;
                
                /* Re-initialize signal handler for SIGUSR2 */
                setup();

                /**
                 * On restart, return from signal handler via setcontext
                 * to avoid _sigtramp -> __sigreturn path.
                 */
                pac_patch_ucontext(ucp);
                if (setcontext(ucp) < 0)
                        err(EXIT_FAILURE, "%s: setcontext", __FILE__);
        }

        restart = 1;

        /* Enumerate and save memory regions */
        meta.nr_regions = ckpt_vm_regions_save(regions);
        meta.nr_headers += meta.nr_regions;
        for (u32 i = 0; i < meta.nr_regions; i++)
                headers[i] = CKPT_VM_REGION_HEADER;
        
        /**
         * Save thread context and strip pac signatures,
         * pac_strip_context() will mark bitmap and modifier
         * values to save information about how pointer values
         * in registers were signed.
         */
        ucontext_memcpy(&ctx.uc, (ucontext_t *)&uc);
        
        ctx.bitmap                      = 0;
        meta.nr_contexts                = 1;
        headers[meta.nr_headers++]      = CKPT_CONTEXT_HEADER;

        if (pac_strip_context(&ctx) < 0) {
                /**
                 * Abort the checkpoint, without pac signing information
                 * a restore can not occur properly.
                 */
                return;
        }

        /**
         * Walk stack frames and strip pac signed pointers,
         * call frame information is saved in the process for
         * re-signing in the restart process.
         */
        fp = (u64 *)get_ucontext_fp(&ctx.uc);
        meta.nr_callframes = pac_strip_frames(frames, fp);
        for (u32 i = 0; i < meta.nr_callframes; i++)
                headers[meta.nr_headers++] = CKPT_CALLFRAME_HEADER;
        
        if (write_ckpt(&meta, headers, regions, &ctx, frames) < 0) {
                fprintf(stderr, "%s: Failed to write checkpoint file "
                                "(%d-ckpt.dat)\n", __FILE__, getpid());
        } else {
                printf("Checkpoint written to %d-ckpt.dat\n", getpid());
        }

        pac_sign_frames(frames, fp, meta.nr_callframes);
        pac_sign_context(&ctx);
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

#if defined(__arm64e__)
        pac_check();
#endif
}

__attribute__((destructor))
void cleanup()
{
        return;
}
