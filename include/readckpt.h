/* readckpt.h */
#ifndef __CKPT_READCKPT_H__
#define __CKPT_READCKPT_H__
#include <unistd.h>
#include <stdio.h>
#include "types.h"

int readall(int, void *, size_t);

int read_vm_region(int, ckpt_vm_region_t *);
int read_context(int, ckpt_context_t *);
int read_callframe(int, ckpt_callframe_t *);

int read_ckpt(int, const ckpt_metadata_t *, ckpt_header_t *,
              ckpt_vm_region_t *, ckpt_context_t *, ckpt_callframe_t *);

#endif // __CKPT_READCKPT_H__
