/* printckpt.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <err.h>
#include <assert.h>
#include <fcntl.h>
#include <ucontext.h>
#include "types.h"
#include "ckpt.h"
#include "pac.h"
#include "vm_region.h"

const char *vm_inherit_string(const ckpt_vm_region_t *region)
{
        static_assert(VM_INHERIT_COPY == VM_INHERIT_DEFAULT &&
                      VM_INHERIT_LAST_VALID == VM_INHERIT_NONE, "");

        switch (region->inherit) {
        case VM_INHERIT_SHARE:
                return "VM_INHERIT_SHARE";
        case VM_INHERIT_COPY:
                return "VM_INHERIT_COPY";
        case VM_INHERIT_NONE:
                return "VM_INHERIT_NONE";
        case VM_INHERIT_DONATE_COPY:
                return "VM_INHERIT_DONATE_COPY";
        default:
                __builtin_trap();
        }
}

const char *vm_share_mode_string(const ckpt_vm_region_t *region)
{
        switch (region->mode) {
        case SM_COW:
                return "SM_COW";
        case SM_PRIVATE:
                return "SM_PRIVATE";
        case SM_EMPTY:
                return "SM_EMPTY";
        case SM_SHARED:
                return "SM_SHARED";
        case SM_TRUESHARED:
                return "SM_TRUESHARED";
        case SM_PRIVATE_ALIASED:
                return "SM_PRIVATE_ALIASED";
        case SM_SHARED_ALIASED:
                return "SM_SHARED_ALIASED";
        case SM_LARGE_PAGE:
                return "SM_LARGE_PAGE";
        default:
                __builtin_trap();
        }
}

const char *vm_user_tag_string(const ckpt_vm_region_t *region)
{
        switch (region->tag) {
        case VM_MEMORY_MALLOC:
                return "VM_MEMORY_MALLOC";
        case VM_MEMORY_MALLOC_SMALL:
                return "VM_MEMORY_MALLOC_SMALL";
        case VM_MEMORY_MALLOC_LARGE:
                return "VM_MEMORY_MALLOC_LARGE";
        case VM_MEMORY_MALLOC_HUGE:
                return "VM_MEMORY_MALLOC_HUGE";
        case VM_MEMORY_SBRK:
                return "VM_MEMORY_SBRK";
        case VM_MEMORY_REALLOC:
                return "VM_MEMORY_REALLOC";
        case VM_MEMORY_MALLOC_TINY:
                return "VM_MEMORY_MALLOC_TINY";
        case VM_MEMORY_MALLOC_LARGE_REUSABLE:
                return "VM_MEMORY_MALLOC_LARGE_REUSABLE";
        case VM_MEMORY_MALLOC_LARGE_REUSED:
                return "VM_MEMORY_MALLOC_LARGE_REUSED";
        case VM_MEMORY_MALLOC_NANO:
                return "VM_MEMORY_MALLOC_NANO";
        case VM_MEMORY_MALLOC_MEDIUM:
                return "VM_MEMORY_MALLOC_MEDIUM";
        case VM_MEMORY_MALLOC_PROB_GUARD:
                return "VM_MEMORY_MALLOC_PROB_GUARD";
        case VM_MEMORY_STACK:
                return "VM_MEMORY_STACK";
        case VM_MEMORY_GUARD:
                return "VM_MEMORY_GUARD";
        case VM_MEMORY_SHARED_PMAP:
                return "VM_MEMORY_SHARED_PMAP";
        case VM_MEMORY_DYLIB:
                return "VM_MEMORY_DYLIB";
        case VM_MEMORY_DYLD:
                return "VM_MEMORY_DYLD";
        case VM_MEMORY_DYLD_MALLOC:
                return "VM_MEMORY_DYLD_MALLOC";
        case VM_MEMORY_LIBDISPATCH:
                return "VM_MEMORY_LIBDISPATCH";
        case VM_MEMORY_RESTART_STACK:
                return "VM_MEMORY_RESTART_STACK";
        default:
                return "";
        }
}

int readall(int fd, void *buf, size_t size)
{
        size_t  bytes;
        ssize_t retval;

        for (bytes = 0; bytes < size; bytes += retval) {
                if ((retval = read(fd, buf + bytes, size - bytes)) < 0) {
                        perror("read");
                        break;
                }
        }

        return (bytes == size) ? 0 : -1;
}

int readckpt(int fd, const ckpt_metadata_t *meta, ckpt_header_t *headers, 
             ckpt_vm_region_t *regions, ckpt_context_t *contexts)
{
        int                     retval;
        ckpt_vm_region_t        *rgn    = regions;
        ckpt_context_t          *ctx    = contexts;

        for (u32 i = 0; i < meta->nr_headers; i++) {
                if (readall(fd, headers + i, sizeof(headers[i])) != 0) {
                        fprintf(stderr, 
                                "Error reading checkpoint header\n");
                        return -1;
                }

                switch (headers[i]) {
                case CKPT_VM_REGION_HEADER:
                        retval = readall(fd, rgn, sizeof(*rgn));
                        assert(lseek(fd, rgn->size, SEEK_CUR) != -1);
                        rgn++;
                        break;
                case CKPT_CONTEXT_HEADER:
                        retval = readall(fd, ctx, sizeof(*ctx));
                        ctx++;
                        break;
                default:
                        __builtin_trap();
                }

                if (retval != 0) {
                        fprintf(stderr, "Error reading %s data\n",
                                CKPT_HEADER_STRING(headers[i]));
                        return -1;
                }
        }

        return 0;
}

void print_vm_regions(const ckpt_vm_region_t *regions, u32 nr_rgns)
{
        const ckpt_vm_region_t *rgn;

printf(
"********************* Checkpointed Memory Regions *********************\n"
);
        for (rgn = regions; rgn < regions + nr_rgns; rgn++) {
                printf("Memory Region #%d:\n"
                       "        start=%p\n"
                       "          end=%p\n"
                       "         size=%zu\n"
                       "         prot=%s/%s\n"
                       "   share mode=%s\n"
                       "     user tag=%s\n"
                       "  inheritance=%s\n",
                       (int)(rgn - regions), rgn->start, rgn->end, 
                       rgn->size, VM_PROT_STRING(rgn->prot),
                       VM_PROT_STRING(rgn->max_prot),
                       vm_share_mode_string(rgn), 
                       vm_user_tag_string(rgn),
                       vm_inherit_string(rgn));
        }
printf(
"**********************************************************************\n"
);

}

void print_contexts(ckpt_context_t *contexts, u32 nr_contexts)
{
        ckpt_context_t  *ctx;
        mcontext_t      mctx;
printf(
"********************* Checkpointed Thread Contexts ********************\n"
);

        for (ctx = contexts; ctx < contexts + nr_contexts; ctx++) {
                mctx = (mcontext_t)&ctx->__mcontext_data;
                printf("Thread Context #%d:\n", (int)(ctx - contexts));
                
                /* General purpose registers */
                for (u32 i = 19; i <= 28; i++)
                        printf("\tx%u:\t0x%llx\n", i, mctx->__ss.__x[i]);

                /* FP/Vector registers */
                for (u32 i = 8; i <= 15; i++) {
                        printf("\td%u:\t0x%llx\n", 
                               i, (u64)mctx->__ns.__v[i]);
                }

                printf("\tfp:\t0x%llx\n", get_mcontext_fp(mctx));
                printf("\tlr:\t0x%llx\n", get_mcontext_lr(mctx));
                printf("\tsp:\t0x%llx\n", get_mcontext_sp(mctx));
        }

printf(
"***********************************************************************\n"
);
}

void printckpt(int fd)
{
        ckpt_metadata_t meta;

        if (readall(fd, &meta, sizeof(meta)) < 0)
                exit(EXIT_FAILURE);

        ckpt_header_t           headers[meta.nr_headers];
        ckpt_vm_region_t        regions[meta.nr_regions];
        ckpt_context_t          contexts[meta.nr_contexts];

        if (readckpt(fd, &meta, headers, regions, contexts) < 0)
                exit(EXIT_FAILURE);

        print_vm_regions(regions, meta.nr_regions);
        print_contexts(contexts, meta.nr_contexts);
}

void usage()
{
        fprintf(stderr, "USAGE: ./printckpt <file>\n");
        exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
        int fd;

        if (argc < 2)
                usage();
        
        if ((fd = open(argv[1], O_RDONLY)) < 0)
                err(EXIT_FAILURE, "open (%s)", argv[1]);

        printckpt(fd);
        exit(EXIT_SUCCESS);
}
