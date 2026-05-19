/* restart.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <fcntl.h>
#include <ucontext.h>
#include "readckpt.h"
#include "ckpt.h"
#include "types.h"
#include "pac.h"
#include "vm_region.h"

__attribute__((noreturn))
void restart(int fd, int depth)
{
        int                     retval;
        ckpt_metadata_t         meta;

        if (depth > 0)
                restart(fd, depth - 1);
        else {
                retval = ckpt_vm_regions_mark(VM_INHERIT_NONE,
                                              VM_BEHAVIOR_DONTNEED);

                if (retval < 0 || readall(fd, &meta, sizeof(meta)) < 0)
                        exit(EXIT_FAILURE);
                
                ckpt_header_t           headers[meta.nr_headers];
                ckpt_vm_region_t        regions[meta.nr_regions];
                ckpt_context_t          contexts[meta.nr_contexts];
                ckpt_callframe_t        frames[meta.nr_callframes];

                retval = read_ckpt(fd, &meta, headers,
                                   regions, contexts, frames);
                close(fd);

                if (retval < 0) {
                        fprintf(stderr, 
                                "%s: Failed to read checkpoint file, "
                                "aborting restart...\n", __FILE__);
                        abort();
                }

                u64 *fp = (u64 *)get_ucontext_fp(&contexts[0].uc);
                pac_sign_frames(frames, fp, meta.nr_callframes);
                pac_sign_context(&contexts[0]);
                
                if (setcontext(&contexts[0].uc) < 0) {
                        fprintf(stderr, 
                                "%s: setcontext() failed, aborting...\n",
                                __FILE__);
                        abort();
                }
        }
        
        abort();
}

int main(int argc, char **argv)
{
        int fd;

        if (argc != 2) {
                fprintf(stderr, "Usage: ./ckpt -r [ckpt-file\n");
                exit(EXIT_FAILURE);
        } else if ((fd = open(argv[1], O_RDONLY)) < 0)
                err(EXIT_FAILURE, "open (%s)", argv[1]);

        printf("Restarting from %s (pid=%d)\n", argv[1], getpid());
        restart(fd, 1);
}
