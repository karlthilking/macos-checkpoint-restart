# Internals on Checkpoint-Restart for macOS on Apple Silicon
This document intends to cover the internals of transparent
checkpoint-restart on Apple Silicon Devices which use the arm64e 
architecture. Comparisons between checkpointing on Linux and macOS will
also be detailed, highlighting the differences between Linux and Darwin
from the perspective of a checkpoint library. Lastly, pointer
authentication (PAC) will be covered, including explanation on the
implications of PAC (on checkpoint-restart) and how PAC-aware
checkpoint-restart is implemented.

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

## 2. Checkpoint Memory Regions and Mach Virtual Memory
On Linux, a checkpoint library can read `/proc/self/maps` in order to
identify memory segments which are mapped in a target's address space when
performing a checkpoint. macOS does not implement the `/proc` filesystem,
so Mach VM APIs are used as an alternative. Despite being slightly 
esoteric, the Mach VM APIs allow for low-level interaction with the VM 
subsystem and are generally more powerful than the `sys/mman.h` family
of memory-management functions (`mmap`, `mprotect`, `msync`, etc.).

The capabilities that Mach VM exports to user programs, and how these
capabilities are used to implement checkpoint-restart are detailed in
the following sections.

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

The specific struct used in `ckpt_vm_save_regions` was of type
`vm_region_submap_info_data_64_t`. The fields of this struct which get
populated by the kernel are explained further.

- **`protection` and `max_protection`** - the current and maximum 
permissions for a given memory region. The `max_protection` of a region is
an upper limit on what permissions can be set via `mach_vm_protect`. An
equivalent concept of `max_protection` does not exist on Linux.
- **`share_mode`** - `SM_COW`, `SM_PRIVATE`, `SM_SHARED`, etc. Describes
the exact way in which a memory region may be shared (or not).
- **`user_tag`** - A constant value which describes the region such as
`VM_MEMORY_STACK`, `VM_MEMORY_MALLOC` (heap regions), `VM_MEMORY_DYLD`
(dyld allocations), etc. See §2.2 for more information on user tags.
- **`page_dirited`** - A count of pages within a memory which have been
written to.
- **`inheritance`** - Inheritance attributes such as `VM_INHERIT_NONE` or
`VM_INHERIT_COPY` which are relevant to system calls such as `fork`.
- **`behavior`** - A constant such as `VM_BEHAVIOR_DONTNEED` or
`VM_BEHAVIOR_FREE` and analagous to Posix `madvise` advice constants.

### 2.2 Mach VM `user_tag`
It is important to understand that a Mach VM region's `user_tag` field
is an application/library level construct, and not interpreted by the
kernel itself. Libraries which allocate their own memory or are responsible
allocating the user process's regions can choose to fill in the `user_tag` 
field. For example, `libsystem_malloc.dylib` will label regions with a
`user_tag` such as `VM_MEMORY_MALLOC_NANO`, `VM_MEMORY_MALLOC_LARGE`, etc.,
and `libsystem_pthread.dylib` assigns the `VM_MEMORY_STACK` label as it 
responsible for each thread's private stack.

A user application can create there own `user_tag`:
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
value; Although a process's stack and heap segments happen to be given
a `user_tag` by certain libraries (`libsystem_pthread`, `libsystem_malloc`),
other memory segments such as text and data have no such tag. Thus, a
`user_tag` is not a definitive measure for identifying the memory regions
which reside in a target's address space. The memory region checkpointing
implementation is discussed further in the following section.

### 2.3 Choosing Which Regions to Save
The fuction `ckpt_vm_valid_region` (in `vm_checkpoint.c`) implements the 
policy that decides which region should be serialized during a checkpoint, 
and which should be ignored. Pseudo-code is provided to demonstrate how
this policy is implemented.

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

`__PAGEZERO` is a special memory segment on macOS used to detect null 
pointer derefences. By default, `__PAGEZERO` will be present in every
macOS address space (unless using the linker flag `-pagezero_size,0`)
and contains no valid data.

Because Mach VM has the concept of current and max protections for memory
regions, `max_protection == NONE` identifies true guard pages. Current
protections never exceed the maximum, so these regions are inaccessible.

After the `restart` program has restored the state of a checkpoint, its
memory regions are still present in the address space, but should not
be saved. The following section (§2.4) will detail exactly how the
`restart` program's own regions are identified.

Regions within the dyld shared cache (system library cache) are only saved
if they have been written to by the target process. The dyld shared cache 
is discussed further in §4.

As mentioned before (§2.2), certain libraries mark their allocations with
a `user_tag` which explains what the memory region is. The target process's
heap and stack regions are saved as long as they are dirty and writable.
The additional check to see if a heap or stack region is dirty and writable
helps to filter out guard pages or unused regions. For example, an 
application which calls `malloc` with consistently similar allocation sizes 
will likely only use one of `libsystem_malloc.dylib's` arenas, and thus,
not all regions with a `VM_MEMORY_MALLOC*` tag need to be saved.
`VM_MEMORY_DYLD*` tagged regions are allocations belonging to `dyld` which
contains stateful data and should be restored.

Every dynamic library evaluated by the `VM_MEMORY_DYLIB` case will be a
user dynamic library due to the fact that macOS system libraries live in
the dyld shared cache region. Thus, user dynamic libraries are always
saved because, unlike system libraries (Darwin's `libsystem_c.dylib`,
`libsystem_pthread.dylib`, etc.), they will not be automically loaded into 
the restart program by `dyld`.

### 2.4 The `restart` Program's Memory Segments
Because the `restart` binary itself occupies memory in the form of its
text, data, stack, and heap regions, there a two main considerations which
need to be made. First, it is critical that, when mapping in a checkpointed
memory region, the restart process does not overwrite own of its own
regions. If they restart process were to overwrite its own stack or text
with a restored region, the restart would fail. Secondly, it is would
unreasonable to checkpoint the restart processes own regions if a 
checkpoint (`SIGUSR2`) is sent after a restart has completed. After the
restart process has essentially transformed itself into the checkpointed
process, its own regions are still present in the address space. Thus,
certain precautions have to be taken to ensure that the restart programs
regions are not included in a subsequent checkpoint.

To prevent the first issue, the `-segaddr` linker flag is used to pin
the `restart` binary's segments to fixed addresses. The goal is that the
chosen addresses will be unlikely to conflict with any of the memory
regions that will need to be restored at runtime.

```make
restart: ...
        -Wl,-segaddr,__TEXT,0x300000000
        -Wl,-segaddr,__DATA,0x300004000
        -Wl,-segaddr,__LINKEDIT,0x300008000
```

In order to ensure that the fixed segment addresses are honored, ASLR
is disabled via the `POSIX_SPAWN_DISABLE_ASLR` flag to `posix_spawn` in
`ckpt.c`. A restart is then initiated with `ckpt -r <ckpt-file>`, where
the `ckpt` program execs into the restart program (with `posix_spawn` and
`flags = POSIX_SPAWN_DISABLE_ASLR | POSIX_SPAWN_SETEXEC`). 

To avoid having restart program's stack conflict with the restored stack,
the restart program allocates in own temporary stack which it jumps to
before initiated the restart phase.

```c
flags = VM_FLAGS_PURGABLE | VM_FLAGS_ANYWHERE | 
        VM_MAKE_TAG(VM_MEMORY_RESTART_STACK);

ret = mach_vm_map(mach_task_self(), &addr, size, 0, flags,
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
Note that the restart program uses `VM_MAKE_TAG(VM_MEMORY_RESTART_STACK)`
in order to associate its own specific tag with its temporary stack. This
simultaneously addresses the second problem of avoiding saving the restart
program's own segments during a checkpoint, as it distinguishes its
temporary stack from other mappings.

The second challenge is now discussed: How do we make sure that the restart
program's memory regions do not get saved when a subsequent checkpoint is
sent? In order to avoid unnecessarily saving the `restart`'s memory regions
when a another checkpoint (`SIGUSR2`) is sent, special flags are used
to mark the restart program's regions before it initiates the restart.

```c
#define RESTART_REGION_BEHAVIOR_FLAG    VM_BEHAVIOR_RSEQNTL
#define RESTART_REGION_INHERIT_FLAG     VM_INHERIT_NONE
```

Mach VM allows for a user program to set the inheritance and behavior
attributes of a memory region via `mach_vm_inherit` and 
`mach_vm_behavior_set`. Two specific behavior and inheritance attributes,
which are unlikely to both be set by any application, are used as sentinel
flags to mark the restart program's regions. In `vm_restore.c`, the function
`ckpt_vm_mark_regions` iterates through the restart programs address space
and applies these flags to each region belonging to `restart`.

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
restart program's regions during a subsequent checkpoint.

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
`ckpt_vm_region_restore` in `vm_restore.c` is responsible for mapping
saved regions back where the were in the checkpointed process. In order
to do this, `mach_vm_map` is used with `VM_FLAGS_FIXED` and
`VM_FLAGS_OVERWRITE`. This achieves the same functionality to `mmap` with
`MAP_FIXED`, but Mach VM APIs are preferred due to providing better
integration with the underlying VM subsystem in the Darwin kernel. For
example, `mach_vm_map` allows one to specify a `cur_protection` and
`max_protection`, whereas `mmap` does not allow for as much specificity.

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

After calling `mach_vm_map`, the saved bytes in the checkpoint file are
read directly into the freshly mapped memory region, restoring the live
memory contents that were present at the time of the checkpoint.
`mach_vm_map` is called with `cur_protection = VM_PROT_DEFAULT` (`rw-`) and
`max_protection = VM_PROT_ALL` (`rwx`) for two reasons. First, the mapping
needs to be writable in order to read the bytes from the file (associated
with the file descriptor `fd`) into the memory region to restore its data.
Second, the maximum protection needs to be `VM_PROT_ALL` in order to
flexibly reconfigure the restored region's permissions. Remember that 
`max_protection` is an upper limit on what a region's protections can be 
set to at any point. Consider if the restored region was a text segment and 
mapped in with `max_protection` set to `VM_PROT_READ | VM_PROT_WRITE`. 
Thereafter, the protections of the region would not be able to made
executable (`VM_PROT_EXECUTE`) and the restored page would cause a 
subsequent fault.

As an additional note, `VM_PROT_WRITE` and `VM_PROT_EXECUTE` can be
set for a region's current protections simultaneously without a specific
`com.apple.security.cs.allow-jit` entitlement (for JIT compilers).
Therefore, using `mach_vm_map` rather than `mmap` allows one to write
to a memory region (to restore checkpointed bytes) and later set the
execute permission bit. On macOS, `mmap` only specifies one `prot` field
which sets the current and maximum protections. Thus, if a restored region
was allocated with `PROT_WRITE` to restore the checkpointed contents,
the pages within the region can not be set to be executable thereafter.

## 3. The dyld Shared Cache
The dyld Shared Cache is a collection of all commonly used Darwin system
libraries pre-linked in a single file which lives on disk. On Apple Silicon
devices this file is named `dyld_shared_cache_arm64e` and there are
possibly more `subcaches` with an additional file suffix (e.g. 
`dlyd_shared_cache_arm64e.01`). When a process is created on macOS, this
file is essentially memory mapped into the process's address space in order
to make all system libraries available at runtime. This is intended to be
an optimization of `dyld`'s (the macOS linker/loader) to improve
application startup time. By including all system libraries in a single
file, `dyld` allows all processes to share one shared cache and avoids
loading several shared objects on demand. The relevance that the `dyld`
shared cache has on checkpointing is due to the fact that it contains the
data segments on the system libraries within. This means that each process
receives COW pages for data segments in the shared cache which are faulted
in once written to. Therefore, it is quite likely for certain virtual memory
regions within the shared cache to be faulted in during the execution of
a target process's lifetime, and a checkpoint is not able to simply ignore
the shared cache.

### 3.1 Shared Cache COW Regions
The exact policy used to decide which regions within the shared cache get
checkpointed is explained further.

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

The requirements for checkpointing a shared cache region require that
the region is writable, process private, and contains dirty pages.
When these conditions are met, it is necessary to include the region in
a checkpoint in order to restore the mutable state of system libraries.
While this policy reasonably identifies which regions in the shared cache
should be saved, it does include certain drawbacks. The main downside is
that one shared cache `region` as identified by the Mach VM subsystem
is often multiple megabytes in size. It is possible that certain shared
cache regions only contain a few dirty pages, meaning the multiple megabytes
of data from the shared cache will bloat the checkpoint file when only
an order of tens of kilobytes would have been necessary to save. In order
to prevent this, it would be desirable to examine shared cache regions at
the page granularity, but this would require support beyond the Mach VM
APIs which are currently used.

### 3.2 Shared Cache UUID and Slide
Earlier, it was mentioned that the shared cache file is essentially memory
mapped into every process's address space. It is important to note that
the shared cache is not only included in every process's address space,
but mapped in at the exact same virtual address. This makes it easy to
restore checkpointed shared cache regions during the same boot. On Apple
Silicon Devices, the shared cache's base address will be equal to
`SHARED_REGION_BASE_ARM64 + slide` where `SHARED_REGION_BASE_ARM64` is
defined in `mach/shared_region.h`. In the common case, the shared cache
regions which are included in a checkpoint can be restored at the exact
virtual addresses in the restart process because the slide (and thus the
base address) will be the same at the time of checkpoint and restart.
More difficulty is presented when the shared is cache is slid or rebuilt.

In this case where the shared cache is slid, each saved shared cache region
can be restored at the its original virtual address plus the difference
between the current shared cache slide and slide during the time of the
checkpoint. The shared cache base address is included in a checkpoint file
in order to allow for this calculation. However, this does not always
guarantee that a restore will be successful. If any of the restored shared
cache regions have internal pointers which were relative to the old
shared cache's base address, these pointers will be stale and possibly harm
program correctness.

```c
// slide = base - SHARED_REGION_BASE_ARM64
struct shared_cache_info {
        const void      *base;
        size_t          size;
        uuid_t          uuid;
};
```

The more difficult case is when the shared cache is entirely rebuilt.
It is assumed that if the `uuid` of the shared cache, also included as
part of the `shared_cache_info` in a checkpoint file, that the shared cache
has been rebuilt. If the shared cache is entirely restructed, it is much
more difficult than taking the difference of two slides in order to 
identify where saved regions should be restored. Restoring a from a
checkpoint that was taken before a rebuild of the shared cache is largely
infeasible. Luckily, this is only likely to happen across system updates
and when changes are made to system libraries, which would render old
checkpoints infeasible regardless.

## 4. Signals handler and the `ckpt_handler`
A checkpoint is initiated by sending a `SIGUSR2` signal the the target
process injected with `libckpt.dylib` (by using `ckpt -c <target>`).
When a `SIGUSR2` is sent, `ckpt_handler` in `libckpt.dylib` will be 
invoked. This signal handler is responsible for initiating the checkpoint
process and producing the final checkpoint image file.

The control flow used in `ckpt_handler`, the semantics of signal handlers
on macOS/Darwin, `_sigtramp` and `__sigreturn`, and reinstalling the
signal handler after a restart are covered in the following sections.

### 4.1 `ckpt_handler` Control Flow
```c
void ckpt_handler(int sig, siginfo_t *info, void *ctx)
{
        ckpt_context_t          ctx;
        ...
        static int              restart;
        
        ...
        restart = 0;
        if (getcontext(&ctx.uc) < 0)
                ...

        if (restart) {
                /* branch taken on restart */
                restart = 0;
                ucp     = (ucontext_t *)uctx;
                ...
                pac_patch_ucontext(ucp);
                setcontext(ucp);
        }
        ...
        /* checkpoint path */
        restart = 1;
        ...

        /* save memory regions, call frame data, write checkpoint... */
        ...
}
```

The first observation of `ckpt_handler`'s implementation is that it uses
`getcontext` in order to save the thread context. Reflexively, `restart`
uses `setcontext` in order to restore the saved context. Because,
`setcontext` returns from `getcontext`, this implies that both the
checkpoint and restart paths will go through `ckpt_handler`. In order to
allow the restart path to correctly return from the signal handler (and
not perform another checkpoint), a static variable, `restart`, is used.
`restart` restores all memory regions (including `__DATA,__bss`) and
then calls `setcontext`, causing it to end up in the signal handler.
Because it has restored the bss segment, it will see `restart = 1` after
returning from `getcontext` and will be able to take the correct branch.

Another key detail about the implementation of `ckpt_handler` is that
it uses two different thread contexts (`ucontext_t`'s). The first
`getcontext` saves the live register state into `ctx.uc`; this is the
context which is saved in the checkpoint file and the reason why `restart`
will return from this call to `getcontext` after restoring the register
context. The second `ucontext_t` is the third parameter to `ckpt_handler`
as part of the `SA_SIGINFO` signal handler flavor. This context is the
one that the restart path uses to return from the signal handler after
calling `setcontext` in `restart.c`.

The reason why `setcontext` is used a second time during restart to return
from the signal handler is explained in the next section (§4.2).

### 4.2 `_sigtramp` and `__sigreturn`
If a user program sets up for a signal to be caught by a signal handler,
a sent signal first invokes `_sigtramp` which then invokes the user's
signal handler. When this signal handler returns (into `_sigtramp`),
`_sigtramp` will then call `__sigreturn` which will validate a saved
context before restoring it. If `__sigreturn` happens to detect any
inconsistencies or validation issues, it will fail and return into
`_sigtramp`, generating an exception.

```c
void _sigtramp(union __sigaction_u __sigaction_u, int sigstyle, int sig,
               siginfo_t *sinfo, ucontext_t *uctx, uintptr_t token)
{
        ...
        sa_sigaction(sig, sinfo, uctx);
        ...
        __sigreturn(uctx, ctxstyle, token);
        __builtin_trap(); /* fatal error */
}
```


### 4.3 Reinstalling `ckpt_handler` on Restart

## 5. Pointer Authentication (PAC)
This section will detail the implementation of PAC-aware, transparent
checkpoint-restart. The approaches taken to handling PAC from the
perspective of a checkpoint library comprise the largest portion of this
work. §5.1 and §5.2 are included as primers on PAC and provide context
on the implications that PAC has on checkpoint-restart. Thereafter,
the remaining sections will explain the implementation of PAC-aware
checkpoint-restart in addition to discussing limitations.

### 5.1 PAC Basics: Registers, Instructions, Bit Layout, Conventions
PAC is a hardware security feature that introduced in ARMv8.3 and is
available on Apple Silicon Devices. PAC is enabled by the arm64e ABI which
will be discussed in more detail through this section.

Four relevant PAC key registers are used:

|      Key      |   Purpose   | Per-process |
|---------------|-------------|-------------|
| `APIAKey_EL1` | Instruction |    **No**   |
| `APIBKey_EL1` | Instruction |   **Yes**   |
| `APDAKey_EL1` |     Data    |    **No**   |
| `APDBKey_EL1` |     Data    |   **Yes**   |

`B` keys are process dependent keys, meaning that each process's `IB` and
`DB` register is randomized. The `A` keys are process-independent, meaning
that the `DA` and `IA` key registers are shared by every process on a
system. `EL1` means that the PAC key system registers are only accessible
by the kernel (whereas `EL0` is user level).

In order to implement pointer authentication, the arm64e ABI introduces
`PAC` and `AUT` instructions. A `PAC` instruction will sign a pointer
(instruction or data address) using one of the PAC key registers and
an additional modifier. `AUT` instructions authenticate a signed pointer.
If an `AUT` instruction is performed using the correct key and modifer,
and the relevant pointer is not corrupt, the pointer's PAC signature
will be stripped. On the other hand, an authentication failure (wrong
key, modifier or pointer has been corrupted) will leave an invalid
address in the pointer which will generate a fault if dereferenced.

There are additional PAC instructions which extended existing arm64
instructions with PAC-related functionality. For example, the `blraa`
instruction corresponds to `blr`, but additionally authenticates the
branch target destination using the `IA` key and a modifier. The `retab`
instruction (in contrast to a normal `ret`) will authenticate the
link register with the `IB` key and the stack pointer before returning
from a subroutine.

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
returns, the frame is popped of the stack, and the `retab` instruction 
authenticates the link register with the `IB` key and `sp` as a modifier 
before returning.

```c
#define PAC_BIT_MASK            (0xFF7F000000000000ULL)
#define PTRAUTH_SIGNED(p)       (((p) & ~(1ull << 55)) & PAC_BIT_MASK)
#define PTRAUTH_FAILED(p)       (((p)>>62) == 0b01 || ((p)>>62) == 0b10)
```

PAC signatures take advantage of the fact that the high bits of a pointer
are typically unused. On a typical 64-bit system, a virtual address size
won't actually be 64 bits, but rather somewhere around 48 bits. Assuming a
48 bit virtual address size, bits 63:48 will be available for storing
a PAC signature. However, bit 55 is left untouched by the PAC signature
generated by a `PAC` instruction in order to differentiate between user
and kernel addresses (0x000... vs 0xFFF...). When an `AUT` instructions
fails, rather than generating an immediate trap, a specific bit pattern
is set in the high bits of the pointer (`PTRAUTH_FAILED`). This pattern
set in the high bits of a pointer will likely cause a segmentation fault
or bus error if such a pointer is dereferenced.

In order to motivate the challenges that PAC presents for checkpointing,
it is necessary to discuss exactly how it is used by real systems.
The first key point to make is that, on macOS, all system libraries and
binaries are compiled for the arm64e architecture. At the minimum,
every return address with be protected with pointer authentication (using
the `IB` + `sp` convention). `C` function pointers are signed with the
`IA` key and a zero modifier. Function stubs (found in the 
`__TEXT,__auth_stubs` section) are signed with the `IA` key and as a
modifier, the GOT entry's storage address and possibly a constant 
discriminator. Beyond the conventions observed in arm64e code (with 
`-arch arm64e`), macOS system libraries can use the `__ptrauth` type 
qualifier and other instrinsics defined in `ptrauth.h` (see 
[llvm on ptrauth](https://clang.llvm.org/docs/PointerAuthentication.html#ptrauth-h)). The ways in which system libraries choose to use
pointer authentication is present the most difficulty related to PAC for
checkpoint-restart. Whereas clang's code generation uses deterministic
and predictable PAC conventions, implentation-specific usage of `__ptrauth`
qualifies types and signing schemes requires reverse engineering system
libraries. Certain PAC failures were possible to be reverse enginereed
(§5.5), but in general, libraries which implement PAC-signing schemes
impose the greatest limitation on checkpoint-restart (see §5.6).

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
