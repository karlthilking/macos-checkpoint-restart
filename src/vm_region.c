/* vm_region.c */
#include <err.h>
#include <assert.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include "readckpt.h"
#include "vm_region.h"

int ckpt_vm_region_valid(vm_region_submap_info_data_64_t *info,
                         mach_vm_address_t addr, mach_vm_size_t size)
{
        if (PAGEZERO(addr, size) || info->max_protection == VM_PROT_NONE)
                return 0;
        else if (info->inheritance == VM_INHERIT_NONE ||
                 info->behavior == VM_BEHAVIOR_DONTNEED) {
                /**
                 * The restart process marks its own pages with
                 * inheritance = VM_INHERIT_NONE and
                 * behavior = VM_BEHAVIOR_DONTNEED, so this region
                 * should be one of the restart program's regions
                 * (can be safely ignored during checkpoint).
                 */
                return 0;
        } else if (DYLD_SHARED_CACHE_REGION(addr, size)) {
                /**
                 * Only checkpoint dyld shared cache regions that are
                 * private or COW, dirty, and writable.
                 */
                return (VM_REGION_PRIVATE(info) && info->pages_dirtied &&
                        (info->max_protection & VM_PROT_WRITE));
        }

        switch (info->user_tag) {
        /* Save all heap pages (that aren't guard pages) */
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
        /* Save stack pages */
        case VM_MEMORY_STACK:
        /* Save dyld memory */
        case VM_MEMORY_DYLD:
        case VM_MEMORY_DYLD_MALLOC:
                return 1;
        case VM_MEMORY_REALLOC:
        case VM_MEMORY_GUARD:
        case VM_MEMORY_SHARED_PMAP:
                return 0;
        case VM_MEMORY_DYLIB:
                if (VM_REGION_SHARED(info))
                        return 0;
                assert(info->max_protection & VM_PROT_WRITE);
                return 1;
        default:
                break;
        }
        
        return 1;
}


u32 ckpt_vm_regions_save(ckpt_vm_region_t *regions)
{
        kern_return_t                   ret;
        u32                             nr_rgns = 0;
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
                } else if (!ckpt_vm_region_valid(&info, addr, size)) {
                        addr += size;
                        continue;
                }

                regions[nr_rgns].start    = (const void *)addr;
                regions[nr_rgns].size     = (size_t)size;
                regions[nr_rgns].end      = (const void *)(addr + size);
                regions[nr_rgns].prot     = info.protection;
                regions[nr_rgns].max_prot = info.max_protection;
                regions[nr_rgns].mode     = info.share_mode;
                regions[nr_rgns].tag      = info.user_tag;

                addr += size;
                nr_rgns++;
        }

        return nr_rgns;
}

int ckpt_vm_regions_mark(vm_inherit_t new_inherit,
                         vm_behavior_t new_behavior)
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
                else if (info.is_submap) {
                        depth++;
                        continue;
                } else if (PAGEZERO(addr, size) ||
                           info.protection == VM_PROT_NONE ||
                           DYLD_SHARED_CACHE_REGION(addr, size)) {
                        /**
                         * Don't need to mark which will already be
                         * ignored or handled during a checkpoint for
                         * obvious reasons.
                         */
                        addr += size;
                        continue;
                }

                ret = mach_vm_inherit(
                        mach_task_self(), addr, size, new_inherit
                );
                if (ret != KERN_SUCCESS) {
                        fprintf(stderr, "mach_vm_inherit: %s\n",
                                mach_error_string(ret));
                        return -1;
                }

                ret = mach_vm_behavior_set(
                        mach_task_self(), addr, size, new_behavior
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

int ckpt_vm_region_restore(int fd, ckpt_vm_region_t *rgn)
{
        kern_return_t           ret;
        mach_vm_address_t       addr;

        if (rgn->start == 0 || rgn->end == 0) {
                fprintf(stderr, 
                        "%s:ckpt_vm_region_restore: "
                        "Invalid Region:\t%p-%p\n", 
                        __FILE__, rgn->start, rgn->end);
                return -1;
        }
        
        /**
         * Map saved vm region at fixed address where it
         * was located in the checkpoint process.
         */
        addr = (mach_vm_address_t)rgn->start;
        ret = mach_vm_map(
                mach_task_self(), &addr, rgn->size, 0,
                VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_DEFAULT,
                VM_PROT_ALL, VM_INHERIT_DEFAULT
        );

        if (ret != KERN_SUCCESS || (void *)addr != rgn->start) {
                fprintf(stderr, "%s: mach_vm_map: %s\n",
                        __FILE__, mach_error_string(ret));
                return -1;
        }
        
        /**
         * Read save vm region contents from checkpoint file into
         * freshly mapped vm region.
         */
        if (readall(fd, (void *)addr, rgn->size) < 0)
                return -1;
        
        /**
         * rgn was mapped in with rw-/rwx to allow restoring rgn
         * contents. If these initial protections to not match the
         * original protections that this region had in the checkpointed
         * process, set the protections back to what they were previously.
         */
        if (rgn->prot != VM_PROT_DEFAULT) {
                if (ckpt_vm_region_protect(rgn, 0, rgn->prot) < 0)
                        return -1;
        }

        if (rgn->max_prot != VM_PROT_ALL) {
                if (ckpt_vm_region_protect(rgn, 1, rgn->max_prot) < 0)
                        return -1;
        }

        return 0;
}

int ckpt_vm_region_protect(const ckpt_vm_region_t *rgn, 
                           int set_max, vm_prot_t new_prot)
{
        kern_return_t ret;

        ret = mach_vm_protect(
                mach_task_self(), (mach_vm_address_t)rgn->start, 
                (mach_vm_size_t)rgn->size, set_max, new_prot
        );

        if (ret != KERN_SUCCESS) {
                fprintf(stderr, "%s: mach_vm_protect: %s\n",
                        __FILE__, mach_error_string(ret));
                return -1;
        }

        return 0;
}
