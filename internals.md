# Internals on Checkpoint-Restart for macOS on Apple Silicon
This document will cover the internals of transparent checkpoint-restart 
on Apple Silicon Devices that use the arm64e architecture. The document will
assume the reader has a systems background on Linux, and additionally some
knowledege of checkpoint-restart. A main theme of this document will be
pointer authentcation (PAC); The impact that PAC has on checkpointing and
the implementation of PAC-aware checkpoint-restart will be discussed.
For those who are unfamiliar with PAC, see [LLVM Pointer Authentication](
https://clang.llvm.org/docs/PointerAuthentication.html#ptrauth-h) or
the primer included as a foreword on PAC (§5.1, §5.2).

## 1. Overall Structure
- `libckpt.dylib` - A checkpoint library which is injected into a target
using the environment variable `DYLD_INSERT_LIBRARIES`. When loaded, 
`libckpt.dylib` will register a handler for `SIGUSR2` which checkpoints 
the target process transparently and produces `<pid>-ckpt.dat`.
- `restart` - A standalone binary which reads a checkpoint file and
restores the state of the checkpoint.
- `ckpt` - A launcher binary which takes three subcommands. `-c` (as in
checkpoint) will set `DYLD_INSERT_LIBRARIES=libckpt.dylib` and exec into 
the target process. `-r` (restart) will invoke the restart binary on a given
checkpoint file. Finally, `-p` will execute the `printckpt` program on a 
given checkpoint file, and `printckpt` will display the contents of the
checkpoint file in a human-readable way.

## 2. Checkpointing Memory Regions and Mach Virtual Memory
On Linux, a checkpoint library can read `/proc/self/maps` in order to
identify memory segments which are mapped in a target's address space.
On macOS, the `/proc` filesystem in not avaiable. Therefore, Mach VM APIs
are used as an alternative. Mach VM APIs permit low-level interaction with
the Darwin VM subsystem and are generally more powerful than the 
`sys/mman.h` family of memory-management functions (`mmap`, `mprotect`, 
`msync`, etc.).

Although the internals of the Linux's and Mach's VM subsystems differ
in their own ways, a Mach 'VM Region' is roughly equivalent to a
'memory segment' on Linux in the context of checkpointing. These two
terms will be used interchangeably.

The following sections will explain how Mach VM functions are used to
query and interact with the macOS virtual memory subsystem. As it will be
seen, these functions provide the memory-management capabilities that are
needed by a checkpoint library.

### 2.1 Region Enumeration
`ckpt_vm_save_regions` (in `vm_checkpoint.c`) iterates through the target
process's address space using `mach_vm_region_recurse`. On each call to
this function, the kernel populates a struct with information about one
particular virtual memory region in the address space.

```c
ret = mach_vm_region_recurse(
        mach_task_self(), &addr, &size, &depth,
        (vm_region_recurse_info_t)&info, &count
);
```

The specific struct used in `ckpt_vm_save_regions` is of type
`vm_region_submap_info_data_64_t`. The relevant fields of this struct are
explained further below.

- **`protection` and `max_protection`** - the current and maximum 
  permissions for a given memory region. The `max_protection` of a region 
  is an upper limit on what permissions can be set via `mach_vm_protect`. 
  An equivalent concept of `max_protection` does not exist on Linux.

- **`share_mode`** - `SM_COW`, `SM_PRIVATE`, `SM_SHARED`, etc., describes
  the exact way in which a memory region may be shared (or not).

- **`user_tag`** - A constant value that describes the region such as
  `VM_MEMORY_STACK`, `VM_MEMORY_MALLOC` (heap regions), `VM_MEMORY_DYLD`
  (dyld allocations), etc. See §2.2 for more information on user tags.

- **`page_dirtied`** - The number of pages (16KB each on Apple Silicon 
  Macs) within a given memory region that have been written to.

- **`inheritance`** - Inheritance attributes such as `VM_INHERIT_NONE` or
  `VM_INHERIT_COPY`. These are relevant to system calls such as `fork`.

- **`behavior`** - A constant such as `VM_BEHAVIOR_DONTNEED` or
  `VM_BEHAVIOR_FREE`, analagous to POSIX `madvise` advice constants.

### 2.2 Mach VM `user_tag`
It is important to understand that a Mach VM region's `user_tag` field
is an application/library level construct. A region's `user_tag` does not 
affect how the underlying VM subsystem treats the region. 

Libraries which allocate memory, either for their own purposes or for user
programs, can choose to populate the `user_tag` of a memory region. For 
example, `libsystem_malloc.dylib` will label allocation zones with a 
`user_tag` such as `VM_MEMORY_MALLOC_NANO` or `VM_MEMORY_MALLOC_LARGE`. 
`libsystem_pthread.dylib` will assign the `VM_MEMORY_STACK` `user_tag` 
when it allocates a thread's private stack.

A user application can create their own `user_tag`:
```c
#define MY_CUSTOM_TAG  240

int main(void) 
{
        ...
        ret = mach_vm_allocate(mach_task_self(), &addr, size,
                               VM_FLAGS_ANYWHERE | 
                               VM_MAKE_TAG(MY_CUSTOM_TAG));
        ...
}
```

The main point to note is that not every region will have a `user_tag` 
value. Although a process's stack and heap segments happen to be given
a `user_tag` by certain libraries (`libsystem_pthread`, 
`libsystem_malloc`), other memory segments such as text and data will not 
be tagged. Thus, a `user_tag` is not a definitive measure for identifying 
the memory regions in a target program's address space. 

### 2.3 Choosing Which Regions to Save
As a brief aside, note that the dyld shared cache will be briefly 
mentioned in this section. While prior knowledge on macOS's shared cache 
region is not strictly required to understand that following information, 
consider reading ahead if any details are unclear (§3).

The fuction `ckpt_vm_valid_region` (in `vm_checkpoint.c`) implements the 
policy that decides which regions should be saved during a checkpoint. The
following pseudo-code describes the policy:

```
if region in __PAGEZERO segment or max_protection == NONE:
        skip
elif RESTART_REGION (a region from the `restart` program):
        skip (see §2.3 for details)
elif region in dyld shared cache:
        save iff private/COW, dirty, and writable
switch user_tag:
        case VM_MEMORY_MALLOC*, VM_MEMORY_STACK, VM_MEMORY_DYLD*:
                save iff dirty and writable
        case VM_MEMORY_REALLOC, VM_MEMORY_GUARD, VM_MEMORY_SHARED_PMAP:
                skip
        case VM_MEMORY_DYLIB:
                save (only user dylibs)
        default:
                save
```

`__PAGEZERO` is a special memory segment on macOS used to catch null 
pointer derefences. By default, `__PAGEZERO` will be present in every
macOS address space (unless using the linker flag `-pagezero_size,0`).
The `__PAGEZERO` segment does not contain any valid data.

Because Mach VM has the concept of a current and maximum protection level 
for memory regions, the predicate `max_protection == NONE` will identify 
true guard pages. Current protections can never exceed the maximum, so 
these regions are inaccessible and never contain valid data.

After the `restart` program has restored the state of a checkpoint, its
memory regions are still present in the address space. However, `restart`'s
memory regions should not be saved in a checkpoint. The following section 
(§2.4)  will detail exactly how the `restart` program's own regions are 
identified.

Regions within the dyld shared cache (system library cache) are only saved
if they have been written to by the target process. See §3 for further
information on the dyld shared cache.

As mentioned before (§2.2), certain libraries label their allocations with
a `user_tag`. This way, `user_tag` can help to distinguish between 
different regions in the address space. Memory regions with a 
`VM_MEMORY_MALLOC*` or `VM_MEMORY_STACK` tag are saved as long as they 
contain dirty pages (and are writable, but this is likely implied). 
Checking to see if heap or stack regions have dirty pages helps to filter 
out guard pages or unused regions. For example, an application that makes 
allocations of similar sizes will likely only use one of 
`libsystem_malloc.dylib`'s arenas. Thus, certain heap zones will contain
zero dirty pages, and can be skipped during a checkpoint. `VM_MEMORY_DYLD*`
regions are also saved when they contain dirty pages; Any state that the
`dyld` has built up in memory should be restored in the `restart` program.

The `VM_MEMORY_DYLIB` case of the switch statement is strictly only reached
by user dynamic library regions. This is because every system library, 
such as `libsystem_c.dylib` or `libsystem_pthread.dylib`, will be in a 
shared cache region. Thus, every region belonging to a user dynamic 
library is saved. Unlike system libraries, user libraries will not be 
automatically loaded by `dyld` into the `restart` program. Therefore, 
their memory regions must be saved explicitly.

### 2.4 The `restart` Program's Memory Segments
The `restart` program itself will occupy memory in the form of its
text, data, stack, and heap regions. There a two considerations which
need to be made. 

First, it is critical that, when mapping in a checkpointed memory region, 
the restart process does not overwrite its own memory. For example, if the 
restart process were to overwrite its own stack or text with a restored 
region, the restart would fail. 

Secondly, if a subsequent checkpoint is sent after restore, checkpointing 
the restart process's own memory regions should be strictly avoided. After 
the `restart` program has successfully restored the state of a checkpoint, 
its memory regions will still be present in the address space. Thus, 
certain precautions have to be taken in order to ensure that the `restart` 
program's regions do not get included in a checkpoint.

To prevent the first issue, the `-segaddr` linker flag is used to pin
the `restart` binary's segments to fixed addresses. The hope is that the
chosen addresses will be unlikely to conflict with any of the memory 
regions which are restored at runtime.

```make
restart: ...
        -Wl,-segaddr,__TEXT,0x300000000
        -Wl,-segaddr,__DATA,0x300004000
        -Wl,-segaddr,__LINKEDIT,0x300008000
```

In order to ensure that the fixed segment addresses are honored, ASLR must
be disabled for the `restart` program. In order to support this, a restart 
is invoked by the `ckpt` program using: `./ckpt -r <ckpt-file>`. Seeing 
the `-r` command line flag, the `ckpt` program will exec into the 
`restart` program with `posix_spawn` and 
`flags = POSIX_SPAWN_DISABLE_ASLR | POSIX_SPAWN_EXEC`.

Additionally, to avoid having `restart` program's stack conflict with the 
stack that will be restored, the `restart` program allocates its own 
temporary stack.

Furthermore, the `restart` program must take measures to ensure that its
stack won't conflict with the restored stack. In order to accomplish this, 
the `restart` program allocates a temporary stack at an address that is
unlikely to conflict with any restored region. Then, the `restart` program
switches to its new stack and begins to restore the checkpoint's state.

```c
mach_vm_address_t addr = 0x320000000;
...
ret = mach_vm_map(mach_task_self(), &addr, size, 0,
                  VM_FLAGS_FIXED | VM_FLAGS_PURGABLE | 
                  VM_MAKE_TAG(VM_MEMORY_RESTART_STACK),
                  MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_DEFAULT,
                  VM_PROT_ALL, VM_INHERIT_NONE);
...
asm volatile(
        "mov    sp, %[sp]       \n"
        "mov    x0, %[fildes]   \n"
        "blraaz %[restart]      \n"
        :
        : [sp] "r" (sp), [fildes] "r" ((long)fd),
          [restart] "r" (restart)
);
...
```

The `restart` program uses `VM_MAKE_TAG(VM_MEMORY_RESTART_STACK)` as an
argument to `mach_vm_map` in order to create its own `user_tag` for its
temporary stack. This helps to address the second issue regarding the
`restart` program's memory regions.

The second challenge with the `restart` program's memory regions is now 
discussed. The question is as follows: How can one ensure that the 
`restart` program's memory regions will not get saved by the next 
checkpoint? To prevent this, the `restart` program marks its own memory 
regions with special flags that help to distinguish them from other 
regions in the address space.

```c
#define RESTART_REGION_BEHAVIOR_FLAG    VM_BEHAVIOR_RSEQNTL
#define RESTART_REGION_INHERIT_FLAG     VM_INHERIT_NONE
```

Mach VM allows for a user program to set the inheritance and behavior
attributes of a memory region via `mach_vm_inherit` and 
`mach_vm_behavior_set`. Two specific behavior and inheritance attributes,
which are unlikely to both be set by any application, are used as sentinel
flags. In `vm_restore.c`, the function `ckpt_vm_mark_regions` iterates 
through the `restart` program's address space and applies these flags to 
each region.

```c
...
ret = mach_vm_inherit(mach_task_self(), addr, size, 
                      RESTART_REGION_INHERIT_FLAG);
...
ret = mach_vm_behavior_set(mach_task_self(), addr, size, 
                           RESTART_REGION_BEHAVIOR_FLAG);
...
```

Then, `ckpt_vm_valid_region` is able to identify and properly skip the
`restart` program's regions during a subsequent checkpoint.

```c
...
else if ((info->inheritance == RESTART_REGION_INHERIT_FLAG &&
          info->behavior == RESTART_REGION_BEHAVIOR_FLAG) ||
         (info->user_tag == VM_MEMORY_RESTART_STACK)) {
        /* Restart region, skip */
        ...
}
...
```

### 2.5 Restoring Memory Regions
`ckpt_vm_region_restore` in `vm_restore.c` is responsible for restoring the
memory regions that are saved as part of a checkpoint file. When each 
memory region is restored, it must be allocated at the fixed virtual 
address where it resided in the checkpointed process.

In order to do this, `mach_vm_map` is used with `VM_FLAGS_FIXED` and
`VM_FLAGS_OVERWRITE`. This achieves the same functionality as `mmap` with
`MAP_FIXED` on Linux.

```c
int ckpt_vm_restore_region(int fd, const ckpt_vm_region_t *region)
{
        ...
        ret = mach_vm_map(mach_task_self(), &addr, region->size, 0,
                          VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE |
                          VM_MAKE_TAG(region->tag), MEMORY_OBJECT_NULL,
                          0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, 
                          VM_INHERIT_DEFAULT);
        ...
        if (readall(fd, (void *)addr, region->size) < 0)
                ...

        if (region->prot != VM_PROT_DEFAULT &&
            ckpt_vm_protect(region, 0, region->prot))
                ...
        else if (region->max_prot != VM_PROT_ALL &&
                 ckpt_vm_protect(region, 1, region->max_prot) < 0)
                ...
}
```

After calling `mach_vm_map` with `VM_FLAGS_FIXED`, a virtual memory region
will be allocated with the same virtual address and size that it had in
the checkpointed process. Then, the next `region->size` bytes from the
checkpoint file are read directly into the freshly-allocated memory region.
This will restore the memory contents that were present at checkpoint
byte-for-byte.

Restored memory regions are created with `cur_protection = VM_PROT_DEFAULT`
(`rw-`) and `max_protection = VM_PROT_ALL` (`rwx`). The current protection
level at least requires the region to be writable. This is due to the fact
that the serialized memory contents (in the checkpoint file associated with
the file descriptor `fd`) must be written to the region after it has been
mapped into the address space. Secondly, the maximum protection must be
set to `VM_PROT_ALL` in order to reset the region's current protection to 
its value in the checkpoint process. Because a region's maximum protection
is an upper limit, any value more restrictive than `VM_PROT_ALL` would
impose a limitation on what the `cur_protection` could be set to 
thereafter. For example, if `mach_vm_map` was called with 
`VM_PROT_READ | VM_PROT_WRITE`, attempting to set the execute permission 
for the region (`VM_PROT_EXECUTE`) would fail.

As an additional note, `VM_PROT_WRITE` and `VM_PROT_EXECUTE` can not be
set for a region's current protections simultaneously without a specific
`com.apple.security.cs.allow-jit` entitlement. Therefore, both 
`mach_vm_map` with `cur_protection = VM_PROT_ALL`, and `mmap` with 
`prot = PROT_READ | PROT_WRITE | PROT_EXEC` will fail on macOS.

## 3. The dyld Shared Cache
The dyld shared cache is a collection of macOS/Darwin system libraries
pre-linked into a single, on-disk file. On Apple Silicon devices, this
file is named `dyld_shared_cache_arm64e`. There are possibly additional
'subcaches' with a unique file suffix (e.g. `dyld_shared_cache_arm64e.01`).
When a process is created on macOS, this file is memory-mapped into the
process's address space, making all system libraries available at runtime.

The architecture of the shared cache is intended to be a `dyld` 
optimization for application startup time. By including every system 
library in a single file, `dyld` only needs to load the shared cache once, 
rather than loading several libraries on demand. Furthermore, a large 
majority of the shared cache is comprised of executable code and constant 
data. Thus, many of the pages within the in-memory shared cache will be 
physically shared between processes.

However, the in-memory shared cache also contains mutable data regions.
A running process will receive Copy-on-Write pages for these regions.
If a COW page within the shared cache is written to by a process, the page
will be faulted in, providing the process with its own private copy. When 
checkpointing a target process, it is quite likely that the target has 
modified certain pages in the shared cache's data regions. Thus, these
specific shared cache regions will be private to the target process and
must be saved during a checkpoint.

### 3.1 Shared Cache COW Regions
The exact policy used to decide which regions within the shared cache get
checkpointed is now explained.

```c
int ckpt_vm_valid_region(const vm_region_submap_info_data_64_t *info,
                         mach_vm_address_t addr, mach_vm_size_t size)
{
        ...
        else if (DYLD_SHARED_CACHE_REGION(addr, size)) {
                if ((info->max_protection & VM_PROT_WRITE) &&
                    (info->share_mode == SM_COW ||
                     info->share_mode == SM_PRIVATE) &&
                    (info->pages_dirtied > 0)) {
                        return 1;
                }
                return 0;
        }
        ...
}
```

The basic policy for deciding if a shared cache region should be
checkpointed is as follows: If the region is writable, private to the 
target process, and contains dirty pages, it should be checkpointed.
Under these conditions, the data within the target's private copy of a
shared cache region must be included in a checkpoint. Otherwise, the
version of the shared cache that will be loaded by `dyld` on restart will
discard any state that the target process has built up in the shared cache.

However, one drawback to this policy relates to efficiency and checkpoint
file sizes. Although the shared cache file divides into several individual
virtual memory regions, each individual region still tends to be quite
large. A typical size for a shared cache region is likely multiple
megabytes. While the given policy correctly identifies regions containing
dirty pages, it would be excessive to checkpoint multiple megabytes of data
when possibly on a few pages have been written to. A more refined approach
would be to probe shared cache regions at the page granularity, 
snapshotting dirty pages individually.

### 3.2 Shared Cache UUID and Slide
Earlier, it was noted that the shared cache file is present in every
process's virtual address space. It is important to state that the shared
cache is not only present in every process's address space, but also
present at the same virtual address. Thus, it is easy to restore
checkpointed shared cache regions across the same boot. Each saved region
from the shared cache can be mapped into the `restart` program's address
space at the same virtual address where it had been before.

On Applie Silicon devices, the base address of the in-memory shared cache
will always be equal or great to `SHARED_REGION_BASE_ARM64`, where
`SHARED_REGION_BASE_ARM64` is a constant defined in `mach/shared_region.h`.
In order to restore shared cache regions after the base address of the
shared cache has changed, we define `slide` to be 
`base - SHARED_REGION_BASE_ARM64`. A private `dyld` API can be used in
order to retrieve the base address of the shared cache region:
`const void *_dyld_get_shared_cache_range(size_t *length)`. The base
address and size of the shared cache are included as part of a checkpoint
file's metadata. 

During a restart, we can account for a different shared cache base address:

```
if old_base == new_base:
        return

old_slide = old_base - SHARED_REGION_BASE_ARM64
new_slide = new_base - SHARED_REGION_BASE_ARM64
shift     = new_base - old_base

for each saved memory region:
        region.start += shift
...
```

However, accounting for shared cache regions to be shifted based on the
difference between the old and new `slide` does not protect against certain
problems. Namely, if a restored shared cache region contains pointers to
other regions in the shared cache, the pointer addresses will be relative 
to the old shared cache's base address. Furthermore, if a the checkpoint 
target had pointers into the shared cache, the pointer addresses will also 
be stale after a restore.

## 4. Signals handler and the `ckpt_handler`
In order to coordinate a checkpoint, `libckpt.dylib` uses a signal
signal handler that is configured to run on a `SIGUSR2`. The signal
handler, `ckpt_handler`, is installed after `libckpt.dylib` is loaded.
In order to allow `ckpt_handler` to be installed before the target
program enters its `main` routine, the environment variable
`DYLD_INSERT_LIBRARIES` is set before the target program is exec'd.
Then, by defining a setup function that uses
`__attribute__((constructor))`, `libckpt.dylib` will successfully
install its signal handler in a timely manner.

### 4.1 `ckpt_handler` Control Flow
```c
void ckpt_handler(int sig, siginfo_t *info, void *uctx)
{
        ckpt_context_t          ctx;
        ...
        static int              restart;
        
        ...
        is_restart = 0;
        if (getcontext(&ctx.uc) < 0)
                ...

        if (is_restart) {
                /* branch taken on restart */
                is_restart      = 0;
                ucp             = (ucontext_t *)uctx;
                ...
                pac_patch_ucontext(ucp);
                setcontext(ucp);
        }
        ...
        /* checkpoint path */
        is_restart = 1;
        ...

        /* save memory regions, call frame data, write checkpoint... */
        ...
}
```

Within the checkpoint handler, the live thread context is saved by
calling `getcontext`. The `ucontext_t` which is filled in by this call
to `getcontext` will later be saved to the checkpoint file. Thus,
when the `restart` program calls `setcontext`, it will effectively
return from the original call to `getcontext`.

Given this observation, it becomes clear that `ckpt_handler` needs
some way to take a different branch after a restart in order to
prevent the `restart` program from initiating another checkpoint.
In order to achieve this, a static variable `is_restart` can be used
to distinguish between a checkpoint and a restart. When a checkpoint
is first initiated, `is_restart` is assigned `0`. Then, only after
passing the `is_restart` branch will the checkpoint path set
`is_restart = 1`. With this implementation, the `restart` program will
see `is_restart == 1` given that it properly restores the memory region
where `is_restart` resides, namely, `__DATA,__bss`. With the correct
machinery, `restart` will arrive in `ckpt_handler` via `setcontext`
and branch on `is_restart`.

Another key detail regarding the implementation of `ckpt_handler` is that
the restart branch returns via `setcontext` rather than a normal return.
The call to `setcontext` along this path uses a second `ucontext_t`
which is filled in by the kernel when the signal handler's frame is
initialized. This `ucontext_t`, not to be confused with the `ucontext_t`
that was saved in the checkpoint file, describes the thread context
before the signal handler was invoked. Thus, this call to `setcontext`
will allow the `restart` program to resume execution at the point right
before the original target was sent a `SIGUSR2`.

While unclear now, the following section will explain why, rather than
using a normal `return`, a call to `setcontext` is used to return from the 
signal handler.

### 4.2 `_sigtramp` and `__sigreturn`
On Darwin, when a signal is sent to a process that has registered a
signal-catching function, the function `_sigtramp` is called before
the signal handler executes. The reponsibility of `_sigtramp` is
to invoke the user's signal handler, and later call `__sigreturn` to
restore the previous context.

The implementation of `_sigtramp` is in `libsystem_platform.dylib`
on macOS:
```c
void _sigtramp(union __sigaction_u __sigaction_u, int sigstyle, int sig,
               siginfo_t *sinfo, ucontext_t *uctx, uintptr_t token)
{
        ...
        /**
         * Note the sa_sigaction is a macro which expands to
         * __sigaction_u.__sa_sigaction. This is a function pointer to
         * a user's signal handler of the type:
         *  void (*__sa_sigaction)(int, siginfo_t *, void *)
         */
        sa_sigaction(sig, sinfo, uctx);
        ...
        __sigreturn(uctx, ctxstyle, token);
        __builtin_trap(); /* fatal error */
}
```

What this information reveals is that `__sigreturn` on Darwin uses
a `token` for validation. Presumably, `__sigreturn` will fail if the
given `token` is not validated, and the user program will abort/trap.

In practice, it was found that `__sigreturn` validation would fail during
a restart. Rather than attempting to patch up the user context and token
that are received by `__sigreturn`, a second `setcontext` is used to
restore the user context that was executing before the signal handler
was invoked. This achieves the same functionality as `__sigreturn`, but
bypasses validation.

### 4.3 Reinstalling `ckpt_handler` on Restart
Even after the `restart` program has restored all memory regions and
called `setcontext` to restore a user context, there is still more
work to be done. The `restart` program has the additional responsibility
of restoring `ckpt_handler` such that additional `SIGUSR2`'s can be
sent, allowing for several checkpoints and restarts. This provides the
justification for why the `ucontext_t` saved to a checkpoint file will be
a user context from within the signal handler. By allowing the `restart`
program to jump back into `libckpt.dylib`, it is possible for the
signal handler to be reinstalled during a restart.

Alternatively, the `restart` program could have used `dlopen` on
`libckpt.dylib` and `dlsym` on `ckpt_handler` to reinstall the signal
handler from outside `libckpt.dylib`. However, this approach would be
wasteful, as `libckpt.dylib` will already be present in `restart`'s
address space once it has called `mach_vm_map` to restore memory regions
from a checkpoint.

Additionally, allowing the `restart` program to end up back inside
`libckpt.dylib` after a `setcontext` provides other opportunities.
In the current implementation of `libckpt.dylib`, there is a function,
`postrestart`, which takes advantage of having the `restart` program 
jump back inside `libckpt.dylib`. Concretely, `postrestart` will
opportunistically deallocate the `restart` program's memory regions,
reset a PAC-related value in `libsystem_pthread` (§5.5), reinstall
`ckpt_handler`, and restore file descriptor state.

## 5. Pointer Authentication (PAC)
This section will detail the implementation of PAC-aware, transparent
checkpoint-restart. The approaches taken for handling PAC from the
perspective of a checkpoint library comprise the largest portion of this
work. §5.1 and §5.2 are included as primers on PAC and provide the 
context necessary to understanding how PAC impacts checkpoint-restart.
Thereafter, the remaining sections will explain the implementation of 
PAC-aware checkpoint-restart in addition to discussing limitations.

### 5.1 PAC Basics: Registers, Instructions, Bit Layout, Conventions
PAC is a hardware security feature that introduced in ARMv8.3 and is
available on Apple Silicon Devices. PAC is enabled by the arm64e ABI and 
will be discussed in more detail throughout this section.

Four relevant PAC key registers are used:

|      Key      |   Purpose   | Per-process |
|---------------|-------------|-------------|
| `APIAKey_EL1` | Instruction |    **No**   |
| `APIBKey_EL1` | Instruction |   **Yes**   |
| `APDAKey_EL1` |     Data    |    **No**   |
| `APDBKey_EL1` |     Data    |   **Yes**   |

A brief note on terminology: The names `IA`, `IB`, `DA`, and `DB` will
be used to refer to `APIAKey_EL1`, `APIBKey_EL1`, `APDAKey_EL1`, and
`APDBKey_EL1`, respectively. Furthermore, the term `A` keys will be
used to refer to the `IA` and `DA` keys, and the term `B` keys will be
used to refer to the `IB` and `DB` keys.

`B` keys are process-dependent keys, meaning that each process's `IB` and
`DB` keys are randomized. Accordingly, `A` keys are process-independent, 
meaning that the `DA` and `IA` key registers are shared by every process.
Additionally, notice that `I` corresponds to a PAC key used for
instruction addresses, while `D` corresponds to data address signing.
Lastly, note that `EL1` denotes a register that is only accessible to
the kernel (whereas `EL0` would be user level). While certain `EL1`
system registers are readable, but not writable from userspace, the
PAC key registers are both not readable or writable at `EL0`.

In order to implement pointer authentication, the arm64e ABI introduces
`PAC` and `AUT` instructions. A `PAC` instruction will sign a pointer
(instruction or data address) using one of the PAC key registers and
an additional modifier. `AUT` instructions authenticate a signed pointer.
When an `AUT` instruction, using the correct key and modifier, 
authenticates a valid PAC-signed pointer, the authenticated pointer will
hold a valid address (data or instruction). On the other hand, an `AUT`
instruction used to authenticate a corrupt pointer will leave an invalid
address in the pointer. Thereafter, derefencing this pointer will generate
an exception (such as a segfault or bus error).

There are additional PAC instructions which extend existing arm64
instructions with PAC-related functionality. For example, the `blraa` 
instruction executes a normal `blr` (branch with link) after 
authenticating the branch target with the `IA` key and a modifier.
Another exampe is the `retab` instruction. In constrast to a normal `ret`,
`retab` will authenticate the return address (in `x30`/`lr`) with the
`IB` key and the stack pointer as a modifier.

```asm
pacibsp
stp     fp, lr, [sp, #-16]!
mov     fp, sp
...
// Subroutine implementation here
...
mov     sp, fp
ldp     fp, lr, [sp], #16
retab
```

The above example demonstrates a standard function prologue and epilogue
on arm64e. The link register is signed with the `IB` key and `sp` at
function entry using the `pacibsp` instruction. Before the function 
returns, the frame is popped of the stack. Then, a `retab` instruction 
authenticates the link register with the `IB` key and `sp` as a modifier 
before returning.

```c
#define PAC_BIT_MASK            (0xFF7F000000000000ULL)
#define PTRAUTH_SIGNED(p)       (((p) & ~(1ull << 55)) & PAC_BIT_MASK)
#define PTRAUTH_FAILED(p)       (((p)>>62) == 0b01 || ((p)>>62) == 0b10)
```

PAC signatures take advantage of the fact that the high bits of a pointer
are commonly unused. On a typical 64-bit system, a virtual address size
will not be 64 bits, but rather somewhere around 48 bits. Given a
48 bit virtual address size, bits 63:48 will be available for storing
a PAC signature. However, bit 55 is an exception to this as it is used
to differentiate between user and kernel address (`0x000..` vs `0xFFF..`).

When an `AUT` instruction fails, it is possible than an immediate trap
will not occur. Rather, a pattern may be set in the high bits of the
pointer that will cause a segfault or bus error if the pointer is 
dereferenced.

In order to motivate the challenges that PAC presents for checkpointing,
it is necessary to discuss exactly how it is used in real systems.
The first key point to make is that, on macOS, every system library or 
binary will compiled for the arm64e architecture. 

At the minimum, the standard PAC conventions used by the compiler will
protect return address and branch targets. This implies that every
return address will be protected by emitting a `pacibsp` before a
function's prologue, and a `retab` instruction after the epilogue.
Additionally, `C` function pointers will be signed with `paciza`
instruction, and branched to with the `blraaz` instruction. For a
complete guide how macOS compilers enforce pointer authentication, see
[clang/llvm ptrauth](https://clang.llvm.org/docs/PointerAuthentication.html#non-triviality-from-address-diversity). Note that macOS ships with
Apple's own fork of `clang`, but the standard for PAC enforced by Apple's
clang reflects the implementation described in the LLVM's document.

A more difficult issue for checkpoint-restart is encountered when system
libraries use pointer authentication in their source code. This is
trivially implemented by using the `__ptrauth` qualifier and other 
instrinsics that are available in `<ptrauth.h>`. The difficulty imposed
by libraries using PAC is that the standards and conventions that they
use are completely flexible. Whereas Apple's `clang` uses deterministic
and predictable PAC conventions, system library implementers can use
pointer authentication however it may fit their needs. Thus, it is
infeasible for a checkpoint-restart library to know exactly which 
pointers a system library may sign, and where they might reside.

In certain cases, source level pointer authentication usage in system 
libraries was successfully reverse engineered (§5.5). Despite this,
libraries using `__ptrauth` qualifiers and `ptrauth.h` intrinsics
still impose one of the greatest limitations for checkpoint-restart on
macOS. For more on limitations and considerations, see §6.

### 5.2 PAC Observations
It is important to state certain subtleties about the functionality of
PAC in the context of checkpoint-restart, in order to provide more
context.

First, it is important to distinguish exactly how PAC instructions
behave when executing both arm64 and arm64e binaries. As opposed to what
one might assume, not all PAC instructions are NOPs when executing an
arm64 binary. Although arm64 compiled binaries (`-arch arm64`) will not
emit PAC instructions, they will load macOS system libraries which do.
In this case, PAC instructions using the `IB` key will continue to work,
while instructions using the `IA`, `DA`, and `DB` keys will be NOPs.
Thus, when an arm64 process call a function within a system library,
that function will emit a `pacibsp` in the function prologue and `retab`
in the epilogue. Thus, checkpointing an arm64 binary on macOS requires 
consideration of signed link registers (or anything else signed with the
`IB` key). Unsurprisingly, an arm64e process will execute all PAC 
instructions.

The implications of the previous observation are as follows. First, when
checkpointing an arm64 binary, the `restart` binary should be compiled
for the arm64 architecture. If one wishes to checkpoint an arm64e binary,
the `restart` binary should be compiled as arm64e. The reason for this is
straightforward: The `restart` program transforms itself into the
checkpointed program by restoring its state from a checkpoint file. Thus,
if any of the `restart` binary's PAC instructions are NOPs where the
target's PAC instructions would have executed, or vice-versa, the restart
program will fail.

Consider the following sequence:
```asm
paciza  x16
...
// checkpoint occurs
...
blraaz  x16
```

Assume `x16` is a `C` function pointer. If the original target was an arm64e
binary, it would have signed `x16` with the `IA` key and zero as a modifer.
Then, once the restart program has restored the checkpointed state of the
original target, it will execute `blraaz`. If the `restart` binary is
compiled as arm64, the `blraaz` will execute as a normal `blr` (branch
with link) as thus, not strip the PAC signature from the branch target.
Then, the NOP `blraaz` in the restart process will leave a PAC signature
in the high bits and the branch will cause a segmentation fault.

On the other hand, consider the case where the target is an arm64 binary
and `restart` is compiled as arm64e. The `paciza` in the target process
will be a NOP, and `x16` won't have a PAC signature. Then, the restart
process comes along and executes `blraaz`, attempting to authenticate a
branch target with no PAC signature. The `blraaz` instruction will then
generate an invalid address and the branch will segfault.

### 5.3 Pointer Authentication with `getcontext` and `setcontext`

On macOS, `getcontext` and `setcontext` are implemented in a system library
called `libsystem_platform.dylib`. `getcontext` is written entirely in
assembly while `setcontext` calls the `_setcontext` subroutine which is
implemented in assembly. Both use PAC instructions.

```asm
.macro PTR_SIGN_FP_SP_LR fp, sp, lr, flags
#if defined(__arm64e__)
        mov     \fp, fp
        mov     x9, #17687
        pacda   \fp, x9

        mov     \sp, sp
        mov     x9, #52205
        pacda   \sp, x9

        mov     \lr, lr
        mov     \flags, LR_SIGNED_WITH_IB
        ...
```

The above arm64 assembly demonstrates how `getcontext` uses PAC. Before
`getcontext` saves the frame pointer, stack pointer, and link register
to a `ucontext_t`'s register context, a macro is used to sign the frame
pointer and stack pointer. With the arm64e ABI, every non-leaf function
signs the link register before it pushes a frame on the stack, so
`getcontext` does not explictly sign the link register (it is already
signed). Rather, a flag is set by this sequence which indicates that
the link register is signed.

```asm
.macro PTR_AUTH_FP_SP_LR fp, sp, lr, flags
        mov     x9, #52205      // ptrauth_sting_discriminator("sp")
        autda   \sp, x9
        ldr     xzr, [\sp]
        mov     sp, \sp
        
        mov     x9, #17687      // ptrauth_string_discriminator("fp")
        autda   \fp, x9
        mov     fp, \fp

        mov     lr, \lr
        tbnz    \flags, LR_SIGNED_WITH_IB_BIT, 2f

        mov     x16, \lr
        mov     x17, x16
        mov     x9, #30675      // ptrauth_string_discriminator("lr")
        autia   x16, x9
        xpaci   x17
        cmp     x16, x17
        b.eq    1f
        brk     #666            // trap instruction
1:
        mov     lr, x16
        pacibsp
2:
        mov     sp, \sp
        mov     fp, \fp
        mov     lr, \lr
```

This sequence is show a macro which is used in `_setcontext` briefly before
it returns. Hopefully the picture is clear. Before `getcontext` saved the
frame pointer and stack pointer, they were signed with constant
discriminators and then saved in the `ucontext_t`. `_setcontext` uses the
same discriminator and PAC key pair in order to validate the `ucontext_t`
which it receives. The only caveat is that `_setcontext` will branch
depending on the value of `flags` which is pointer to by a machine
specific thread state deep inside a `ucontext_t`. If the same `ucontext_t`
written by `getcontext` is passed to `setcontext` the specific bit which
`_setcontext` branches on (`LR_SIGNED_WITH_IB = 1 << LR_SIGNED_WITH_IB_BIT`)
would be set. Otherwise, `_setcontext` will expect the saved link register
to be signed with another constant discriminator (`30675`) and authenticate
it.

Given the background of how `getcontext` and `setcontext` operate on Darwin,
the way in which a saved `ucontext_t` is handled during restart can now
be explained. Keep in mind that the `restart` process will read a saved
`ucontext_t` from the checkpoint file including a saved register state.
This context will contain signed pointers (as made clear by the 
implementation `getcontext`) that were valid for the checkpointed process,
so the `restart` program must be able to fixup the saved `ucontext_t` 
before calling `setcontext`.

```c
void pac_patch_ucontext(ckpt_context_t *ctx)
{
        ...
        XPACI(lr);
        PACIA(lr, LR_DISCRIMINATOR);
        set_ucontext_lr(ctx, lr);
        set_ucontext_flags(ctx, 0);

        XPACD(fp);
        PACDA(fp, FP_DISCRIMINATOR);
        set_ucontext_fp(ctx, fp);

        XPACD(sp);
        PACDA(sp, SP_DISCRIMINATOR);
        set_ucontext_sp(ctx, sp);
        ...
}
```

Patching up a `ucontext_t` such that its register state will be valid
in the restart process is quite simple. It is known that the frame pointer,
stack pointer, and link register will all have been signed by the
implementation of `getcontext`. However, these signed register values are
only guaranteed to have been valid in the checkpointed process. Thus, the
PAC signatures must be stripped using `xpaci` and `xpacd` for instruction 
and data addresses respectively. Furthermore, `getcontext` and 
`_setcontext` reveal the use of constant discriminator values, and thus
the key and modifier for re-signing each register is known. Also note
that setting the flags variable to 0 (`set_ucontext_flags`) forces
`_setcontext` to take the path where it authenticates the saved link
register with the `IA` key and a constant discriminator. This is merely a
lazy approach that guarantees the link register will be re-signed properly
by allowing `_setcontext` to take charge in doing so.

### 5.4 Frame Walking: `pac_resign_frames`
In the previous section, it was demonstrated that `restart` program could
quite easily salvage a stale register state with PAC'd pointers, simply
by stripping their signatures and re-signing with restart process's keys.
However, the live context which gets included in a checkpoint file is
merely possible area where one may observe PAC'd pointers. The greater
source of PAC'd pointers are part of the restored stack contents, within
frame records. First, remember that the approach to restoring memory
regions will restore the checkpointed process's stack at the same fixed
virtual address and restore its contents byte-identically. What this
means is that every call frame that was on the stack at checkpoint is
now on the restart process's stack. Each frame record will contain a link
register in order to inform the function where to return to, and by
convention, each link register will have a PAC signature.

```
+---------------------+  <- sp at function entry
| saved link register |  <- fp[1]
+---------------------+
| saved frame pointer |  <- fp[0]
+---------------------+  <- fp
|     calle-saved     |
+---------------------+
|   local variables   |
+---------------------+
```

This depiction demonstrates a standard frame record on arm64/arm64e. A
frame record holds the saved link register and the saved frame pointer.
The saved link register informs the function where to return, and the saved
frame pointer points to the previous frame. Given the usage of the frame
pointer and standard frame records in the arm64/arm64e ABI, it is possible
to walk each call frame on the stack like a linked list. What we are
interested in now is how frame walking can be be used to re-sign each
link register on the stack. The arm64e PAC convention for link registers
is to use the `pacibsp` instruction before pushing a frame record on the
stack. This means that every link register saved in a call frame on the
stack would have been signed with the stack pointer's address at the
function's entry point and the `IB` key. Now, the implementation can be
made concrete: Use the frame pointer to walk each frame, strip the PAC
signature from each link register, and re-sign the link register with
the calculated `sp` value and the `IB` key.

```c
void pac_resign_frames(u64 *__fp)
{
        for (u64 *fp = __fp; fp != NULL; fp = (u64 *)fp[0]) {
                if (!PTRAUTH_SIGNED(fp[1]))
                        continue;
                XPACI(fp[1]);
                PACIB(fp[1], (u64)fp + 16);
        }
}
```

The frame walking implementation is extremely simple. The frame pointer 
which is used to call the function is from the register context which was
saved as part of the checkpoint file. For each loop iteration, the link
register can be found at an offset of 8 bytes from where the frame pointer
points to. Additionally, because an arm64/arm64e frame record is exactly
16 bytes, the `sp` which should be used to re-sign the link register is
`fp + 16`. To walk to the next frame, the previous frame address is
found in the frame record at an offset of 0 bytes. The frame pointer 
eventually becomes `NULL` from sentinel outermost frame which indicates
their are no more frames.

### 5.5 Reverse Engineering PAC
There were a few, rare cases where Darwin/macOS system libraries used 
pointer authentication explicitly with the process-dependent `B` keys.
This meant that, after restoring system library state, pointers in
memory would be restored and hold invalid PAC signatures for the restart
process. A couple of these cases were successfully reverse engineered.

```c
void exit(int status)
{
        _tlv_exit();
        __cxa_finalize();
        
        if (__cleanup)
                (*__cleanup)();

        __exit(status);
}
```

In `libsystem_c.dylib`, `exit` uses a function pointer to hold the
instruction address of a cleanup function. In testing, it was discovered
that this function pointer was being authenicated with the `IB` and
would cause PAC failure if not re-signed by the restart process.
The solution used to prevent an authentication failure interposes
`libsystem_c.dylib`'s `exit` and re-signs the function pointer on demand
before `exit` is called. In order to properly re-sign the function pointer,
the signing convention used had to be inferred from the assembly
instructions executed when loading the function pointer. The following
pseudo code demonstrates the rough sequence.

```
x16 =   __cleanup       // address pointer to by __cleanup
x17 =   &__cleanup      // storage address of __cleanup
x17 |=  (0x211b << 48)  // 16 bit constant shifted into high bits

autib x16, x17          // aut branch target (x16) with modifier
```

In order to protect branches to `__cleanup`, it is signed with the `IB`
key and a modifier, where the modifier is the storage address of
`__cleanup` with a 16 bit discriminator (`0x211b`) shifted into the
high bits of the pointer.

If the restart process call `exit` without re-signing this function
pointer, the `autib` before branching to `__cleanup`  will fail because
it would have been signed with the `IB` key from the previous process.
In order to re-sign this function pointer on demand, the `exit` function
is interposed and the `__cleanup` function pointer is re-signed before
the real `exit` gets called.

```c
extern void (*__cleanup)(void);

void __exit_hook(int stat)
{
        if (PTRAUTH_SIGNED((u64)__cleanup))
                pac_strip_resign(__cleanup, APIBKey, 0x211b, 1);

        exit(stat);
}
```

The next PAC failure which was successfully reverse-engineered was
found in `libsystem_pthread.dylib` (Darwin's libphtread implementation).
Many of the Posix thread APIs take, as a function argument, a `pthread_t`
thread identifier. Rather than trusting a `pthread_t` from a user program,
`libsystem_pthread` first validates that the identifier is legitimate 
before completing continuing. In order to do this, an particular value is
stored at offset 0 inside the opaque internal `pthread` struct. Another
value in `libsystem_pthread`'s data segment is xor'd with this value
to construct a PAC signed pointer to an internal `pthread` struct. This
means that if a user program passes an invalid `pthread_t` to 
`libsystem_pthread`, it will load at offset 0 from the invalid `pthread_t`
and xor whatever data is there with a cookie value. Because the `pthread_t`
argument was invalid, the xor will produce an incoherent value and
`libsystem_pthread` will try to authenticate it will an `autdb`
instruction, that causes the program to crash.

The issue this imposes is that a legitimate `pthread_t` passed to
`libsystem_pthread` after restart will follow the same failure path as
described above. Let `slot_value` be the data at an offset of 0 into
the opaque `pthread` struct. When the `restart` program restores all
memory regions, it will restore `libsystem_pthread`'s data as well. Then,
the `xor_cookie` and `slot_value` will be the same as they were at
checkpoint time. However, the `xor_cookie` and `slot_value` were designed
specifically such that their xor would produce a signed pointer which could
be validated with an `autdb`. In the restart process, the `B` keys have
been rotated, so the xor of these values will produce an invalid pointer.

```
pthread_self_ptr = TPIDRRO_EL0 - PTHREAD_SELF_TLS_OFFSET 
slot_value       = pthread_self_ptr[0]
signed_ptr       = pacdb(pthread_self_ptr, PTHREAD_SELF_DISCRIMINATOR)
xor_cookie       = slot_value ^ signed_ptr
```

In order to address this, the `xor_cookie` is calculated when 
`libckpt.dylib` is first loaded as shown above, and it is stored in a
static variable. Then, after the `restart` program has jumped back into 
`libckpt.dylib`, the new correct `slot_value` is recomputed with the
theoretical `signed_ptr` and saved `xor_cookie`.

```
pthread_self_ptr = TPIDRRO_EL0 - PTHREAD_SELF_TLS_OFFSET
slot_ptr         = pthread_self_ptr

old_ptr          = xor_cookie ^ slot_ptr[0]
new_ptr          = pac_strip_and_resign(old_ptr)

slot_ptr[0]      = new_ptr ^ xor_cookie
```

### 6. Limitations and Future Work

# Appendix: macOS for Linux Users
The following appendix is intended for Linux systems programmers as a brief 
guide on macOS/Darwin concepts. This is not a comprehensive overview of macOS 
and the Darwin kernel, but rather short reference on specific topics that
are relevant to checkpoint-restart on macOS.

## A.1 The Darwin Kernel: XNU, BSD, Mach
The Darwin kernel has a hybrid architecture that combines the Mach
microkernel with a BSD layer. A macOS process can use both BSD syscalls
and Mach traps. The BSD syscall layer provides POSIX system calls such as
`open`, `read`, `mmap`, `fork`, and others. Mach traps can be used for
IPC, memory-management, and thread operations.

## A.2 dyld and the dyld shared cache 
`ld.so` on Linux resolves individual shared objects (`.so`'s) independently.
On macOS, **dyld** does the same for user libraries, but system libraries
are bundled into a file called the dyld shared cache. When a system boots,
the shared cache file is mapped at a fixed virtual address and shared
between all processes. Executable code is physically-shared between all
processes, while writable segments contain Copy-on-Write pages that are
faulted in when written to.

## A.3 macOS Binary Format: Mach-O
macOS's binary format is Mach-O.

- **Mach-O Segments** - A segment is a contiguous range of virtual 
  addresses. Examples include `__TEXT`, `__DATA`, `__LINKEDIT`, and
  `__PAGEZERO`. A section is a fine-grained subset of one segment.
  Examples of section are the following: `__TEXT,__text`, `__DATA,__bss`,
  and `__DATA,__interpose`.
- **`__DATA,__interpose`** - a particularly important section in the
  context of checkpoint-restart. This section contains entries that are
  pairs of `(replacement, original)`. Each entry instructs `dyld` to 
  re-bind calls to `original` to go to `replacement`. This section allows
  one to interpose function calls without needing `LD_PRELOAD`, and
  without neeeding `dlsym` with `RTLD_NEXT` to find the real function
  definition.

## A.4 Loaders and Shared Objects
| Linux | macOS |
|---|---|
| `ld-linux.so` | `dyld` |
| `LD_PRELOAD` | `DYLD_INSERT_LIBRARIES` |
| `.so` | `.dylib` |

As with `LD_PRELOAD` on Linux, `DYLD_INSERT_LIBRARIES` will allow a
shared object to be loaded before a program's `main` executes. On Linux,
this is useful because it allows interposers to override functions in
other libraries (such a `libc.so`). On macOS, preloading a library is not a
requirement for function interposition (see §A.3). However,
`DLYD_INSERT_LIBRARIES` is still used for macOS checkpoint-restart in order
to allow `libckpt.dylib` to install its signal handler in a constructor
function (`__attribute__((constructor))`).

## A.5 macOS and Linux Checkpoint-Restart Cheat Sheet
| Concept | Linux | macOS |
|---|---|---|
| Saving Memory Segments | `/proc/self/maps` | `mach_vm_region_recurse` |
| Library Injection | `LD_PRELOAD` | `DYLD_INSERT_LIBRARIES` |
| Interposing | `LD_PRELOAD` + Override symbol + `dlsym(RTLD_NEXT)` | `__DATA,__interpose` section |
| Linker | `ld.so` | `dyld` |
| Binary Format | ELF | Mach-O |
| Disable ASLR | `personality(ADDR_NO_RANDOMIZE)` | `POSIX_SPAWN_DISABLE_ASLR` |
| Map at Fixed Address | `mmap` + `MAP_FIXED` | `mach_vm_map` + `VM_FLAGS_FIXED` + `VM_FLAGS_OVERWRITE` |
