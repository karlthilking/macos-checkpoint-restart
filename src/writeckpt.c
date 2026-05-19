/* writeckpt.c */
#define _XOPEN_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <string.h>
#include <errno.h>
#include "ckpt.h"
#include "pac.h"
#include "vm_region.h"
#include "writeckpt.h"

int writeall(int fd, const void *buf, size_t size)
{
        size_t  bytes;
        ssize_t retval;

        for (bytes = 0; bytes < size; bytes += retval) {
                if ((retval = write(fd, buf + bytes, size - bytes)) < 0) {
                        perror("write");
                        break;
                }
        }

        return (bytes == size) ? 0 : -1;
}

int write_vm_region(int fd, const ckpt_vm_region_t *rgn)
{
        int retval = 0;

        /* Write vm region information (range, protections, etc) */
        if (writeall(fd, (void *)rgn, sizeof(*rgn)) < 0)
                return -1;
        
        /**
         * Write vm region contents. If the region has no protection
         * bits, temporarily grant read permission in order to save
         * the contents of the region to the checkpoint file.
         */
        if (rgn->prot == VM_PROT_NONE) {
                retval = ckpt_vm_region_protect(rgn, 0, VM_PROT_READ);
                retval |= writeall(fd, (void *)rgn->start, rgn->size);
                retval |= ckpt_vm_region_protect(rgn, 0, VM_PROT_NONE);
        } else if (writeall(fd, (void *)rgn->start, rgn->size) < 0)
                retval = -1;

        return retval;
}

int write_callframe(int fd, const ckpt_callframe_t *cf)
{
        if (writeall(fd, (void *)cf, sizeof(*cf)) < 0)
                return -1;

        return 0;
}

int write_context(int fd, const ckpt_context_t *ctx)
{
        if (writeall(fd, (void *)ctx, sizeof(*ctx)) < 0)
                return -1;

        return 0;
}

int write_ckpt(const ckpt_metadata_t *meta, 
               const ckpt_header_t *headers, 
               const ckpt_vm_region_t *regions, 
               const ckpt_context_t *contexts, 
               const ckpt_callframe_t *callframes)
{
        int                     fd, retval;
        char                    ckptfile[128];
        const ckpt_vm_region_t  *rgn    = regions;
        const ckpt_context_t    *ctx    = contexts;
        const ckpt_callframe_t  *cf     = callframes;
        
        snprintf(ckptfile, sizeof(ckptfile), "%d-ckpt.dat", getpid());
        fd = open(ckptfile, O_CREAT | O_WRONLY |
                  O_TRUNC, S_IRUSR | S_IWUSR);

        if (fd < 0) {
                fprintf(stderr, "%s: open: %s\n", 
                        __FILE__, strerror(errno));
                return -1;
        }

        /* Write checkpoint metadata to beginning of file */
        if (writeall(fd, meta, sizeof(*meta)) < 0) {
                fprintf(stderr, 
                        "%s: Error writing checkpoint metedata\n",
                        __FILE__);
                close(fd);
                return -1;
        }

        for (u32 i = 0; i < meta->nr_headers; i++) {
                retval = writeall(fd, &headers[i], sizeof(headers[i]));
                if (retval != 0) {
                        fprintf(stderr, 
                                "%s: Error writing checkpoint header\n",
                                __FILE__);
                        close(fd);
                        return -1;
                }

                switch (headers[i]) {
                case CKPT_VM_REGION_HEADER:
                        retval = write_vm_region(fd, rgn);
                        rgn++;
                        break;
                case CKPT_CONTEXT_HEADER:
                        retval = writeall(fd, ctx, sizeof(*ctx));
                        ctx++;
                        break;
                case CKPT_CALLFRAME_HEADER:
                        retval = writeall(fd, cf, sizeof(*cf));
                        cf++;
                        break;
                default:
                        /* Unrecognized header */
                        __builtin_trap();
                }

                if (retval < 0) {
                        fprintf(stderr, 
                                "%s: Error writing %s to checkpoint\n",
                                __FILE__, CKPT_HEADER_STRING(headers[i]));
                        close(fd);
                        return -1;
                }
        }

        close(fd);
        return 0;
}
