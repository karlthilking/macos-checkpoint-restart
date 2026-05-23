/* vm_restore.c */
#include <assert.h>
#include "readckpt.h"
#include "vm_region.h"

int ckpt_vm_mark_regions()
{
        kern_return_t                   ret;
        mach_vm_address_t               addr = 0;
        mach_vm_size_t                  size = 0;
        natural_t                       depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t          count;

        for (;;) {
                count = VM_REGION_SUBMAP_INFO_COUNT_64;
                ret = mach_vm_region_recurse(
                        mach_task_self(), &addr, &size, &depth,
                        (vm_region_recurse_info_t)&info, &count
                );

                if (ret != KERN_SUCCESS)
                        break;
                else if (info.is_submap) {
                        depth++;
                        continue;
                } else if (PAGEZERO(addr, size) ||
                           info.protection == VM_PROT_NONE ||
                           DYLD_SHARED_CACHE_REGION(addr, size)) {
                        /**
                         * Skip regions that are already handled specially
                         * in ckpt_vm_valid_region(), marking them is
                         * unnecessary.
                         */
                        addr += size;
                        continue;
                }

                ret = mach_vm_inherit(
                        mach_task_self(), addr, size,
                        RESTART_REGION_INHERIT_FLAG
                );

                if (ret != KERN_SUCCESS) {
                        fprintf(stderr, "mach_vm_inherit: %s\n",
                                mach_error_string(ret));
                        return -1;
                }

                ret = mach_vm_behavior_set(
                        mach_task_self(), addr, size,
                        RESTART_REGION_BEHAVIOR_FLAG
                );

                if (ret != KERN_SUCCESS) {
                        fprintf(stderr, "mach_vm_behavior_set: %s\n",
                                mach_error_string(ret));
                        return -1;
                }

                addr += size;
        }

        return 0;
}

int ckpt_vm_restore_region(int fd, const ckpt_vm_region_t *region)
{
        kern_return_t           ret;
        mach_vm_address_t       addr;
        
        assert(region->start != NULL && region->end != NULL);
        addr = (mach_vm_address_t)region->start;
        
        /* Allocate checkpointed memory region */
        ret = mach_vm_map(mach_task_self(), &addr, region->size, 0,
                          VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE |
                          VM_MAKE_TAG(region->tag),
                          MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_DEFAULT,
                          VM_PROT_ALL, VM_INHERIT_DEFAULT);
        
        if (ret != KERN_SUCCESS) {
                fprintf(stderr, "mach_vm_map: %s\n",
                        mach_error_string(ret));
                return -1;
        }
        
        assert((void *)addr == region->start);
        /* Restore saved memory contents in checkpoint file to region */
        if (readall(fd, (void *)addr, region->size) < 0)
                return -1;

        if (region->prot != VM_PROT_DEFAULT &&
            ckpt_vm_protect(region, 0, region->prot) < 0)
                return -1;
        else if (region->max_prot != VM_PROT_ALL &&
                 ckpt_vm_protect(region, 1, region->max_prot) < 0)
                return -1;

        return 0;
}
