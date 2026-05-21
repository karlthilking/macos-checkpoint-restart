/* restart.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>
#include <sys/mman.h>
#include "readckpt.h"
#include "ckpt.h"
#include "types.h"
#include "pac.h"
#include "vm_region.h"
#include "shared_cache.h"

__attribute__((noinline, noreturn))
void restart(int fd)
{
        int                     retval;
        u64                     *fp;
        ckpt_metadata_t         meta;

        retval = ckpt_vm_regions_mark(VM_INHERIT_NONE,
                                      VM_BEHAVIOR_DONTNEED);

        if (retval < 0 || readall(fd, &meta, sizeof(meta)) < 0 ||
            shared_cache_check(&meta.shared_cache_info) < 0)
                exit(EXIT_FAILURE);

        ckpt_header_t           headers[meta.nr_headers];
        ckpt_vm_region_t        regions[meta.nr_regions];
        ckpt_context_t          contexts[meta.nr_contexts];
        ckpt_callframe_t        frames[meta.nr_callframes];

        retval = read_ckpt(fd, &meta, headers,
                           regions, contexts, frames);
        if (retval < 0) {
                fprintf(stderr, 
                        "Failed to read checkpoint file, "
                        "aborting restart...\n");
                exit(EXIT_FAILURE);
        }
        
        fp = (u64 *)get_ucontext_fp(&contexts[0].uc);
        pac_sign_frames(frames, fp, meta.nr_callframes);
        pac_sign_context(&contexts[0]);
        
        if (setcontext(&contexts[0].uc) < 0)
                err(EXIT_FAILURE, "setcontext()");
        
        abort();
}

__attribute__((noreturn))
void jump(int fd)
{
        void            *stack, *sp;
        const size_t    stksz = 1024 * 1024;

        stack = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | VM_FLAGS_PURGABLE, -1, 0);
        if (stack == MAP_FAILED) {
                perror("mmap");
                __builtin_trap();
        }
        
        sp = (void *)(((u64)stack + stksz) & ~0xf);
        
        /* Switch to temporary stack and call restart function */
        asm volatile(
                "mov sp, %[sp]          \n"
                "mov x0, %[fildes]      \n"
                "blraaz %[restart]      \n"
                :
                : [sp] "r" (sp), [fildes] "r" ((long)fd),
                  [restart] "r" (restart)
        );

        __builtin_unreachable();
}

__attribute__((noreturn))
int main(int argc, char **argv)
{
        int fd;

        if (argc != 2) {
                fprintf(stderr, "Usage: ./ckpt -r [ckpt-file]\n");
                exit(EXIT_FAILURE);
        } else if ((fd = open(argv[1], O_RDONLY)) < 0)
                err(EXIT_FAILURE, "open (%s)", argv[1]);

        printf("Restarting from %s (pid=%d)\n", argv[1], getpid());
        jump(fd);

        __builtin_unreachable();
}
