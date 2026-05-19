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

static inline char *arm64_register_string(u32 reg_nr)
{
        char *buf = malloc(sizeof(char) * 3);

        if (reg_nr < 29) {
                snprintf(buf, 3, "x%u", reg_nr);
                return buf;
        }

        switch (reg_nr) {
        case ARM64_FP:
                strncpy(buf, "fp", 3);
                break;
        case ARM64_LR:
                strncpy(buf, "lr", 3);
                break;
        case ARM64_SP:
                strncpy(buf, "sp", 3);
                break;
        case ARM64_PC:
                strncpy(buf, "pc", 3);
                break;
        default:
                __builtin_trap();
        }
        
        assert(buf[2] == '\0');
        return buf;
}

static inline const char *pac_key_string(u32 keyno)
{
        switch (keyno) {
        case PAC_IA_KEY:
                return "IA";
        case PAC_IB_KEY:
                return "IB";
        case PAC_DA_KEY:
                return "DA";
        case PAC_DB_KEY:
                return "DB";
        default:
                __builtin_trap();
        }

        return NULL;
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

int readckpt(int fd, const ckpt_metadata_t *meta,
             ckpt_header_t *headers, ckpt_vm_region_t *regions,
             ckpt_context_t *contexts, ckpt_callframe_t *frames)
{
        int                     retval;
        ckpt_vm_region_t        *rgn    = regions;
        ckpt_context_t          *ctx    = contexts;
        ckpt_callframe_t        *cf     = frames;

        for (u32 i = 0; i < meta->nr_headers; i++) {
                if (readall(fd, headers + i, sizeof(headers[i])) != 0) {
                        fprintf(stderr, 
                                "%s: Erorr reading checkpoint header\n",
                                __FILE__);
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
                case CKPT_CALLFRAME_HEADER:
                        retval = readall(fd, cf, sizeof(*cf));
                        cf++;
                        break;
                default:
                        __builtin_trap();
                }

                if (retval != 0) {
                        fprintf(stderr, "%s: Error reading %s data\n",
                                __FILE__, CKPT_HEADER_STRING(headers[i]));
                        return -1;
                }
        }

        return 0;
}

void print_vm_regions(ckpt_vm_region_t *regions, u32 nr_regions)
{
        ckpt_vm_region_t *rgn;
        
printf(
"***************** Checkpointed Virtual Memory Regions ******************\n"
);
        for (rgn = regions; rgn < regions + nr_regions; rgn++) {
                printf("Memory Region %d:\n"
                       " start=%p\n"
                       "   end=%p\n"
                       "  size=%zu\n"
                       "  prot=%s/%s\n",
                       (int)(rgn - regions),
                       rgn->start, rgn->end, rgn->size,
                       VM_PROT_STRING(rgn->prot),
                       VM_PROT_STRING(rgn->max_prot));
        }
printf(
"***********************************************************************\n"
);
}

void print_contexts(ckpt_context_t *contexts, u32 nr_contexts)
{
        ckpt_context_t *ctx;
printf(
"********************* Checkpointed Thread Contexts ********************\n"
);

        for (ctx = contexts; ctx < contexts + nr_contexts; ctx++) {
                ctx->uc.uc_mcontext = &ctx->uc.__mcontext_data;
                printf("Thread Context %d:\n", (int)(ctx - contexts));
                for (u32 i = 0; i < ARM_THREAD_STATE64_GPREGCOUNT; i++) {
                        printf(" x%u:\t0x%llx\n", 
                               i, ctx->uc.uc_mcontext->__ss.__x[i]);
                }
                printf(" fp:\t0x%llx\n", get_ucontext_fp(&ctx->uc));
                printf(" lr:\t0x%llx\n", get_ucontext_lr(&ctx->uc));
                printf(" sp:\t0x%llx\n", get_ucontext_sp(&ctx->uc));
        }

printf(
"***********************************************************************\n"
);
}

void print_callframes(ckpt_callframe_t *frames, u32 nr_callframes)
{
        ckpt_callframe_t        *cf;
        char                    *mod;
        const char              *key;

printf(
"******************** Checkpoint Stack Frame Records *******************\n"
);

        for (cf = frames; cf < frames + nr_callframes; cf++) {
                mod     = arm64_register_string(FRAME_PAC_MODIFIER(cf));
                key     = pac_key_string(FRAME_PAC_KEY(cf));
                printf("Call Frame %d:\n"
                       "       fp=0x%llx\n"
                       "       lr=0x%llx\n"
                       "      key=%s\n"
                       " modifier=%s\n",
                       (int)(cf - frames), cf->fp, cf->lr, key, mod);
                free(mod);
        }

printf(
"***********************************************************************\n"
);
}

void printckpt(int fd)
{
        int             retval;
        ckpt_metadata_t meta;

        if (readall(fd, &meta, sizeof(meta)) < 0)
                exit(EXIT_FAILURE);

        ckpt_header_t           headers[meta.nr_headers];
        ckpt_vm_region_t        regions[meta.nr_regions];
        ckpt_context_t          contexts[meta.nr_contexts];
        ckpt_callframe_t        frames[meta.nr_callframes];

        retval = readckpt(fd, &meta, headers, 
                          regions, contexts, frames);
        if (retval != 0)
                exit(EXIT_FAILURE);

        print_vm_regions(regions, meta.nr_regions);
        print_contexts(contexts, meta.nr_contexts);
        print_callframes(frames, meta.nr_callframes);
}

int main(int argc, char **argv)
{
        int fd;

        if (argc < 2) {
                fprintf(stderr, "Usage: ./ckpt -p [ckpt-file]\n");
                exit(EXIT_FAILURE);
        } else if ((fd = open(argv[1], O_RDONLY)) < 0)
                err(EXIT_FAILURE, "open (%s)", argv[1]);

        printckpt(fd);
        exit(EXIT_SUCCESS);
}
