/* readckpt.c */
#define _XOPEN_SOURCE
#include <err.h>
#include <unistd.h>
#include <assert.h>
#include <ucontext.h>
#include "ckpt.h"
#include "readckpt.h"
#include "pac.h"
#include "vm_region.h"

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

int read_vm_region(int fd, ckpt_vm_region_t *rgn)
{
        int retval;

        retval = readall(fd, rgn, sizeof(*rgn));
        retval |= ckpt_vm_restore_region(fd, rgn);
        
        return retval;
}

int read_context(int fd, ckpt_context_t *ctx)
{
        if (readall(fd, ctx, sizeof(*ctx)) < 0)
                return -1;
        
        ctx->uc_mcontext = &ctx->__mcontext_data;
        return 0;
}

int read_ckpt(int fd, const ckpt_metadata_t *meta, ckpt_header_t *headers,
              ckpt_vm_region_t *regions, ckpt_context_t *contexts)
{
        int                     retval;
        ckpt_vm_region_t        *rgn    = regions;
        ckpt_context_t          *ctx    = contexts;

        for (u32 i = 0; i < meta->nr_headers; i++) {
                if (readall(fd, &headers[i], sizeof(headers[i])) < 0) {
                        fprintf(stderr, 
                                "Failed to read checkpoint header\n");
                        return -1;
                }

                switch (headers[i]) {
                case CKPT_VM_REGION_HEADER:
                        retval = read_vm_region(fd, rgn);
                        rgn++;
                        break;
                case CKPT_CONTEXT_HEADER:
                        retval = read_context(fd, ctx);
                        ctx++;
                        break;
                default:
                        __builtin_trap();
                }

                if (retval < 0) {
                        fprintf(stderr,
                                "Error reading %s from checkpoint\n",
                                CKPT_HEADER_STRING(headers[i]));
                        close(fd);
                        return -1;
                }
        }

        close(fd);
        return 0;
}
