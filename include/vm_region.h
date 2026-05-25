/* vm_region.h */
#ifndef __CKPT_VM_REGION_H__
#define __CKPT_VM_REGION_H__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include <mach/shared_region.h>
#include <mach/vm_prot.h>
#include <mach/vm_inherit.h>
#include <mach/vm_behavior.h>
#include <mach/vm_param.h>
#include "types.h"

typedef struct ckpt_vm_region {
        const void      *start;
        const void      *end;
        size_t          size;
        vm_inherit_t    inherit;
        int             prot;
        int             max_prot;
        u32             mode;
        u32             tag;
} ckpt_vm_region_t;

#define VM_PROT_STRING(__prot) \
        (((__prot) == (VM_PROT_NONE))                   ? "---" : \
         ((__prot) == (VM_PROT_READ))                   ? "r--" : \
         ((__prot) == (VM_PROT_DEFAULT))                ? "rw-" : \
         ((__prot) == (VM_PROT_READ | VM_PROT_EXECUTE)) ? "r-x" : \
         ((__prot) == (VM_PROT_ALL))                    ? "rwx" : "---")

#define PAGEZERO_BASE   (0x0000000000000000ULL)
#define PAGEZERO_END    (0x0000000100000000ULL)
#define PAGEZERO(__addr, __size) \
        (((__addr) >= PAGEZERO_BASE) && \
        (((__addr) + (__size)) < PAGEZERO_END))

#define DYLD_SHARED_CACHE_BASE \
        (SHARED_REGION_BASE_ARM64)
#define DYLD_SHARED_CACHE_SIZE \
        (SHARED_REGION_SIZE_ARM64)
#define DYLD_SHARED_CACHE_END \
        (DYLD_SHARED_CACHE_BASE + DYLD_SHARED_CACHE_SIZE)
#define DYLD_SHARED_CACHE_REGION(__addr, __size) \
        (((u64)(__addr) >= (DYLD_SHARED_CACHE_BASE)) && \
        (((u64)(__addr) + (u64)(__size)) < (DYLD_SHARED_CACHE_END)))

#define VM_REGION_PRIVATE(__info) \
        ((__info)->share_mode == SM_COW || \
         (__info)->share_mode == SM_PRIVATE)

#define VM_REGION_SHARED(__info) \
        ((__info)->share_mode == SM_SHARED || \
         (__info)->share_mode == SM_TRUESHARED)

#define VM_REGION_ALIASED(__info) \
        ((__info)->share_mode == SM_PRIVATE_ALIASED || \
         (__info)->share_mode == SM_SHARED_ALIASED)

/**
 * Sentinel user_tag, and behavior and inheritance attributes
 * used to indicate the restart process's own regions so
 * that they will be ignored on checkpoint (or deallocated
 * when the restart phase finishes).
 */
#define VM_MEMORY_RESTART_STACK (240)
#define RESTART_REGION_BEHAVIOR_FLAG    VM_BEHAVIOR_RSEQNTL
#define RESTART_REGION_INHERIT_FLAG     VM_INHERIT_NONE

#define RESTART_REGION(__info) \
        (((__info)->inheritance == RESTART_REGION_INHERIT_FLAG &&       \
         ((__info)->behavior == RESTART_REGION_BEHAVIOR_FLAG)) ||       \
         ((__info)->user_tag == VM_MEMORY_RESTART_STACK))

/* Functions implemented in vm_restore.c and used during restart */
int ckpt_vm_mark_regions();
int ckpt_vm_restore_region(int, const ckpt_vm_region_t *);

/* Functions implemented in vm_checkpoint.c and used by libckpt.dylib */
int ckpt_vm_valid_region(const vm_region_submap_info_data_64_t *,
                         mach_vm_address_t, mach_vm_size_t);
u32 ckpt_vm_save_regions(ckpt_vm_region_t *);
void ckpt_vm_deallocate_regions();

/**
 * General purpose virtual memory functions used during checkpoint
 * and restart phases, defined in vm_common.c.
 */
int ckpt_vm_protect(const ckpt_vm_region_t *, int, vm_prot_t);


#endif // __CKPT_VM_REGION_H__
