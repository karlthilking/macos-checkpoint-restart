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
        retval |= ckpt_vm_region_restore(fd, rgn);
        
        return retval;
}

int read_context(int fd, ckpt_context_t *ctx)
{
        if (readall(fd, ctx, sizeof(*ctx)) < 0)
                return -1;

        ctx->uc.uc_mcontext = &ctx->uc.__mcontext_data;
        return 0;
}

int read_callframe(int fd, ckpt_callframe_t *cf)
{
        if (readall(fd, cf, sizeof(*cf)) < 0)
                return -1;

        return 0;
}

int read_ckpt(int fd, const ckpt_metadata_t *meta,
              ckpt_header_t *headers, ckpt_vm_region_t *regions,
              ckpt_context_t *contexts, ckpt_callframe_t *frames)
{
        int                     retval;
        ckpt_vm_region_t        *rgn    = regions;
        ckpt_context_t          *ctx    = contexts;
        ckpt_callframe_t        *cf     = frames;

        for (u32 i = 0; i < meta->nr_headers; i++) {
                if (readall(fd, &headers[i], sizeof(headers[i])) < 0) {
                        fprintf(stderr, 
                                "%s: Failed to read checkpoint header\n",
                                __FILE__);
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
                case CKPT_CALLFRAME_HEADER:
                        retval = read_callframe(fd, cf);
                        cf++;
                        break;
                default:
                        __builtin_trap();
                }

                if (retval < 0) {
                        fprintf(stderr,
                                "%s: Error reading %s from checkpoint\n",
                                __FILE__, CKPT_HEADER_STRING(headers[i]));
                        close(fd);
                        return -1;
                }
        }

        close(fd);
        return 0;
}
