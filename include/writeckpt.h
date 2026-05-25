/* writeckpt.h */
#ifndef __CKPT_WRITECKPT_H__
#define __CKPT_WRITECKPT_H__
#define _XOPEN_SOURCE
#include <unistd.h>
#include "types.h"

int writeall(int, const void *, size_t);

int write_vm_region(int, const ckpt_vm_region_t *);
int write_context(int, const ckpt_context_t *);

int write_ckpt(const ckpt_metadata_t *, const ckpt_header_t *, 
               const ckpt_vm_region_t *, const ckpt_context_t *);

#endif // __CKPT_WRITECKPT_H__
