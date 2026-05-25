/* vm_checkpoint.c */
#include <assert.h>
#include <stdio.h>
#include "vm_region.h"

int ckpt_vm_valid_region(const vm_region_submap_info_data_64_t *info,
                         mach_vm_address_t addr, mach_vm_size_t size)
{
        if (PAGEZERO(addr, size) || info->max_protection == VM_PROT_NONE)
                return 0;
        else if (RESTART_REGION(info)) {
                /* Restart region, can be discared */
                return 0;
        } else if (DYLD_SHARED_CACHE_REGION(addr, size)) {
                if ((info->max_protection & VM_PROT_WRITE) &&
                    VM_REGION_PRIVATE(info) && info->pages_dirtied) {
                        assert(!VM_REGION_ALIASED(info));
                        return 1;
                }
                return 0;
        }

        switch (info->user_tag) {
        case VM_MEMORY_MALLOC:
        case VM_MEMORY_MALLOC_NANO:
        case VM_MEMORY_MALLOC_TINY:
        case VM_MEMORY_MALLOC_SMALL:
        case VM_MEMORY_MALLOC_MEDIUM:
        case VM_MEMORY_MALLOC_LARGE:
        case VM_MEMORY_MALLOC_HUGE:
        case VM_MEMORY_MALLOC_LARGE_REUSABLE:
        case VM_MEMORY_MALLOC_LARGE_REUSED:
        case VM_MEMORY_MALLOC_PROB_GUARD:
                if (info->pages_dirtied) {
                        assert(info->max_protection & VM_PROT_WRITE);
                        return 1;
                }
                return 0;
        case VM_MEMORY_STACK:
                if (info->pages_dirtied > 0) {
                        assert((info->max_protection & VM_PROT_WRITE) &&
                               VM_REGION_PRIVATE(info));
                        return 1;
                }
                return 0;
        case VM_MEMORY_DYLD:
        case VM_MEMORY_DYLD_MALLOC:
                if (info->pages_dirtied) {
                        assert((info->max_protection & VM_PROT_WRITE) &&
                               VM_REGION_PRIVATE(info));
                        return 1;
                }
                return 0;
        case VM_MEMORY_REALLOC:
        case VM_MEMORY_GUARD:
        case VM_MEMORY_SHARED_PMAP:
                return 0;
        case VM_MEMORY_DYLIB:
                return 1;
        default:
                break;
        }
        
        return 1;
}

u32 ckpt_vm_save_regions(ckpt_vm_region_t *regions)
{
        kern_return_t                   ret;
        u32                             nr_rgns = 0;
        mach_vm_address_t               addr    = 0;
        mach_vm_size_t                  size    = 0;
        natural_t                       depth   = 0;
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
                } else if (!ckpt_vm_valid_region(&info, addr, size)) {
                        addr += size;
                        continue;
                }
                
                regions[nr_rgns].start          = (void *)addr;
                regions[nr_rgns].end            = (void *)(addr + size);
                regions[nr_rgns].size           = (size_t)size;
                regions[nr_rgns].inherit        = info.inheritance;
                regions[nr_rgns].prot           = info.protection;
                regions[nr_rgns].max_prot       = info.max_protection;
                regions[nr_rgns].mode           = info.share_mode;
                regions[nr_rgns].tag            = info.user_tag;

                addr += size;
                nr_rgns++;
        }

        return nr_rgns;
}

void ckpt_vm_deallocate_regions()
{
        kern_return_t                   ret;
        mach_vm_address_t               addr    = 0;
        mach_vm_size_t                  size    = 0;
        natural_t                       depth   = 0;
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
                else if (RESTART_REGION(&info)) {
                        ret = mach_vm_deallocate(mach_task_self(), 
                                                 addr, size);
                        if (ret != KERN_SUCCESS)
                                fprintf(stderr, "mach_vm_deallocate: %s\n",
                                        mach_error_string(ret));
                }

                addr += size;
        }
}
