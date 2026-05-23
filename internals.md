# macOS Checkpoint-Restart Internals
This document intends to cover the internals of performing 
checkpoint-restart on macOS targeting the arm64e architecture used on
Apple Silicon Devices. This document seeks to illustrate the differences
between Linux CRIU/DMTCP style checkpointing: Mach virtual memory, dyld and
the dyld shared cache, and pointer authentication (PAC). The overall
checkpoint-restart architecture is similar a Linux/Posix implementation:
a checkpoint library is injected into a target process and a standalone
binary restores a checkpoint. The areas where implementation of
checkpoint-restart must diverge from Linux will be the main focus.

## 1. Project Structure
Three main artifacts are compiled:
- `libckpt.dylib` - A checkpoint library which is injected using the
environment variable `DYLD_INSERT_LIBRARIES`. Its constructor will run
before the target process's main and install a handler for SIGUSR2. Sending
a `SIGUSR2` invokes the signal handler which transparently checkpoints
the target process and produces `<pid>-ckpt.dat`.
- `restart` - A standalone binary which reads a checkpoint image file and
restores the state of the checkpointed process.
- `ckpt` - A thin launcher implementing three subcommands: `-c` will
execute a binary with `libckpt.dylib` injected, `-r` will invoke the
restart binary on a given checkpoint file, and `-p` will execute the
`printckpt` program to interpret a checkpoint image in human-readable form.

## 2. Checkpoint File Format 
The file format used for checkpoint is straightfoward. First, a
`ckpt_metadata_t` structure will begin the checkpoint file, containing
auxiliary information about the checkpoint. Then, a number of
`ckpt_header_t` with a value of `CKPT_VM_REGION_HEADER`, 
`CKPT_CONTEXT_HEADER`, or `CKPT_CALLFRAME_HEADER`, and after each header
is the corresponding struct or data which it associates with. For example,
a header equal to `CKPT_VM_REGION_HEADER` will be followed by the struct
`ckpt_vm_region_t` which is followed by the raw bytes that resided in that
region at the time of checkpointing.

The function `read_ckpt` in `readckpt.c` implements the functionality of
interpreting the checkpoint file's contents and invoking other functions
(e.g. `ckpt_vm_region_restore`) as the checkpoint image is read.

## 3. Mach Virtual Memory
On Linux, a checkpoint library can read `/proc/self/maps` in order to
identify memory segments in the target's address space and perform
a checkpoint. However, macOS does not implement the `/proc` filesystem, so
Mach VM APIs are used as an alternative. The function 
`mach_vm_region_recurse` is responsible for allowing region enumeration
when performing a checkpoint.

### 3.1 Region Enumeration
`ckpt_vm_regions_save` (in `vm_region.c`) iterates through the target
process's address space using `mach_vm_region_recurse`. On each call to
this function, the kernel populates the a Mach VM specific struct,
`vm_region_submap_info_data_64_t`, which allows for obtaining information
about each memory region in the address space.

The Mach VM APIs provide detailed metadata about each memory region, 
including some information that would not be accessible using 
`/proc/self/maps` or alternative Linux approaches. The specific information
that can be obtained for each memory region using `mach_vm_region_recurse`
is detailed below.

- **`protection` and `max_protection`** - the current and maximum 
permissions for a given memory region. The `max_protection` of a region is
an upper limit on what permissions can be set via `mach_vm_protect`. (The
`mmap`, `mprotect`, etc. family of Posix system calls do not have an
equivalent concept of maximum permissions)
- **`share_mode`** - `SM_COW`, `SM_PRIVATE`, `SM_SHARED`, `SM_TRUESHARED`,
`SM_PRIVATE_ALIASED`, `SM_SHARED_ALIASED`, `SM_EMPTY` and `SM_LARGE_PAGE`.
The flags are comparable to `MAP_PRIVATE`/`MAP_SHARED` but with slightly
more detail.
- **`user_tag`** - Assigned a constant value that describes a region, such 
as `VM_MEMORY_STACK` for a process's stack, `VM_MEMORY_MALLOC` for heap 
regions, or `VM_MEMORY_DYLD` for regions that dyld uses for its own 
allocations.
- **`pages_dirtied`** - A count of pages within the memory region which
have been written.
- **`inheritance`** - Inheritance attributes such as `VM_INHERIT_NONE` or
`VM_INHERIT_COPY`
- **`behavior`** - A behavior constant such as `VM_BEHAVIOR_DONTNEED` or
`VM_BEHAVIOR_FREE`, analogous to `madvise` advice constants.

This is more information available via the Mach VM family of functions not
mentioned here, but the above mentioned are most relevant to checkpointing
the state of a target process's address space. Additionally, the 
`inheritance` and `behavior` attributes are used to implement a
page-tagging routine used in the restart process (see §3.3).

### 3.2 Deciding Which Regions to Save
The function `ckpt_vm_region_valid` implements the policy for selecting
which regions should be serialized to a checkpoint file. Others regions
are ignored. The general logic of this function is demonstrated by the
following pseudo-code:

```
if region in __PAGEZERO or max_prot == NONE:
        skip
elif inheritance == VM_INHERIT_NONE or behavior == VM_BEHAVIOR_DONTNEED:
        skip (see §3.3)
elif region in dyld shared cache:
        save iff private/COW, dirty, and writable
switch user_tag:
        case VM_MEMORY_MALLOC*, VM_MEMORY_STACK, VM_MEMORY_DYLD*:
                save iff pages_dirtied > 0
        case VM_MEMORY_REALLOC, VM_MEMORY_GUARD, VM_MEMORY_SHARED_PMAP:
                skip
        case VM_MEMORY_DYLIB:
                if share_mode == SM_SHARED:
                        skip
                else:
                        save
        default:
                save
```

### 3.3 Tagging the Restart Process's Regions
The `restart` binary itself occupies memory in the form of its text, data, 
stack, and heap regions, and thus, there are two main considerations to
take when implementing the `restart` program. First, it is critical that,
when mapping in the in checkpointed memory regions, that the `restart`
process does not overwrite its own memory regions, causing erroneous
program behavior or subsequent failure. Secondly, we would like that, after
one checkpoint and restart, if the `restart` process receives another
checkpoint (via `SIGUSR2`), the `restart` process's own regions do not get
included in the checkpoint image.

There are two strategies used to prevent these issues:

First, the `-segaddr` linker flag is passed when compiling the `restart`
binary in order to pin the text and data segments to fixed addresses which
are unlikely to conflict with the memory regions that will be restored.

```make
restart: ...
        -Wl,-segaddr,__TEXT,0x300000000
        -Wl,-segaddr,__DATA,0x300004000
```

In order for these fixed segment addresses to be honored, ASLR is disabled
via the undocumented `POSIX_SPAWN_DISABLE_ASLR` in `ckpt.c`.

Second, before the `restart` program begins to read the checkpoint image
or restore any of the checkpointed state, it makes a call to
`ckpt_vm_regions_mark(VM_INHERIT_NONE, VM_BEHAVIOR_DONTNEED)`. This
function iterates through every region in the `restart` process, and it sets
the inheritance and behavior attributes of every region according to the
function parameters. Then, if the `restart` process receives a subsequent
checkpoint (`SIGUSR2`), `ckpt_vm_region_valid` will be able to identify
regions belonging to the restart process itself and know that they can
be safely ignored.

Note that `VM_INHERIT_NONE` attribute is relevant to `fork` semantics and
`VM_BEHAVIOR_DONTNEED` advises paging behavior to the VM subsystem, but
in use case of the restart program, these are simply chosen as sentinel
values to 'mark' regions as discardable in the checkpoint path which
iterates through memory regions.

### 3.4 Restoring Regions
`ckpt_vm_region_restore` is responsible for mapping saved regions at
their original virtual address. In order to do so, `mach_vm_map` is used
to re-map checkpointed memory regions.

```c
mach_vm_map(vm_map_t target_task, mach_vm_address_t *address,
            mach_vm_size_t size, mach_vm_offset_t mask, int flags,
            mem_entry_name_port_t object, memory_object_offset_t offset,
            boolean_t copy, vm_prot_t cur_protection, 
            vm_prot_t max_protection, vm_inherit_t inheritance);
```

When `mach_vm_map` is called by `ckpt_vm_region_restore`, the value of
flags is set to `VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE`. `VM_FLAGS_FIXED`
is the equivalent to `mmap` with `MAP_FIXED` and allows us to re-map
regions at the exact address that they resided at in the checkpointed
process. `VM_FLAGS_OVERWRITE` informs the kernel to discard any region
that may occupy the virtual address where we would like to map in the
saved region.

The restored region is initially mapped with 
`cur_protection = VM_PROT_DEFAULT` (rw-) and `max_protection = VM_PROT_ALL` 
(rwx) such that there will be no issue writing the saved bytes (from the 
checkpoint file) into the freshly-mapped, restored region. Except for
when the re-mapped region actually had its protections set to `rw-/rwx` in
the original process, `mach_vm_protect` will be called after restoring
the contents of the region in order to reset its permission bits.

## 4. The dyld Shared Cache
The dyld shared cache is quite unlike anything on Linux, and imposes
certain difficulties for implementing checkpoint-restart.

### 4.1 What is the dyld shared cache?

`dyld` is the dynamic linker on macOS and caches all system libraries into
a single on-disk file which gets memory-mapped into every processes'
address space such that the shared cache can be shared between them.
On Apple Silicon devices, the specific file is `dyld_shared_cache_arm64e`.
Every running process observes the shared cache at the same fixed virtual
address, equal to `SHARED_REGION_BASE_ARM64 + some random slide`. Within
the shared cache are thousands of sub-regions of text and data segments
from different system libraries.

### 4.2 Dealing with the Shared Cache
First of all, despite being a shared cache of system libraries (such as
Darwin's libc, libpthread, libmalloc, etc.), there are data segments
within the shared cache which are writable to any given process. Thus,
these regions are marked as copy-on-write such that any given process
will have a COW region served from the shared cache file, and each process
will receive a private copy of the region when it writes to it. When a
target process (which will be checkpointed) writes to a COW page/region in
the shared cache, the region it has written to must later be saved when
a checkpoint takes place.

As demonstrated earlier in `ckpt_vm_region_valid` this uses the following
predicate for shared cache regions:

```c
if ((info->share_mode == SM_COW || info->share_mode == SM_PRIVATE) &&
    info->pages_dirtied && (info->max_protection & VM_PROT_WRITE)) {
        /* save region */
        ...
} else {
        /* skip region */
        ...
}
```

### 4.3 Checking the Shared Cache's UUID
In `shared_cache.c`, the cache's base virtual address, size, and uuid
is recorded by calling `_dyld_get_shared_cache_range` and
`dyld_get_shared_cache_uuid`. This information is part of the auxiliary
information saved in the `ckpt_metadata_t` portion of a checkpoint image
file. On restart, `shared_cache_check()` compares the current shared cache
UUID and the UUID saved in the checkpoint image file. If the UUIDs differ,
the restart process fails as it assumed that the shared cache has either
been rebuilt or modified by a system-wide update. 

Additionally, if the shared cache has been slid to a different virtual 
address, a restart will fail if any saved shared cache regions consist of 
pointers to regions that were not restored by the restart process. If
the cache is slid, but not rebuilt, the restart process can compute the
difference between the old and new slide and map saved shared cache regions
accordingly. However, if a pointer within a restored shared cache region
contains a pointer to a region which was not restored during restart,
the pointer's address will hold a stale address which was only valid in
the previous process.

### 5. Signal Handlers and `ckpt_handler`
The checkpoint is initiated by sending `SIGUSR2` to a process that has
been injected with `libckpt.dylib`. The signal handler `ckpt_handler` is
set up such that it will be invoked when a `SIGUSR2` is sent to the 
process. However, the `restart` program also takes a path through the
signal handler because it will call `setcontext` on saved register state
that was originally saved via `getcontext` at checkpoint time.

The semantics of macOS signal handlers, the choice of bypassing
`_sigtramp`, and what happens inside `ckpt_handler` during a checkpoint
and a restart are covered in the following sections.

### 5.1 Double `setcontext`
```c
void ckpt_handler(int sig, siginfo_t *info, void *uctx)
{
        ckpt_context_t          ctx;
        ...
        static int              restart;
        static uintptr_t        tls;

        ...
        restart = 0;
        if (getcontext(&ctx.uc) < 0) {
                /* fail */
                ...
        }

        if (restart) {
                /* branch taken on restart */
                restart = 0;
                ...
                pac_patch_ucontext((ucontext_t *)uctx);
                setcontext((ucontext_t *)uctx);
        }
        
        /* checkpoint path */
        asm volatile("mrs %0, tpidrro_el0" : "=r" (tls));
        restart = 1;

        /**
         * save all memory regions, register context, 
         * write checkpoint file...
         */
        ...
}
```

The first trick used for control flow is the static variable `restart`.
Due to static storage duration, the checkpoint path will set `restart = 1`,
and once the restart process has restored all memory regions 
(`__DATA,__bss` specifically) and calls `setcontext()` to end up inside 
the signal handler, it will know take the restart path within the signal 
handler. (Note that `setcontext` will return from `getcontext`).

The above implementation of the `ckpt_handler` demonstrates the use
of two different contexts (`ucontext_t`). The first call to `getcontext`
saves the live thread register state into `ctx.uc`; this is the context
which is saved as part of the checkpoint file. The second thread context
used is the third parameter to the signal handler, using the `SA_SIGINFO`
signal handler flavor. This context is the one used by the restart 
process to return from the signal handler after calling `setcontext()` a
first time (in `restart.c`) in order to end up inside the signal handler.

The reason why a second `setcontext` is used for returning from the signal 
handler is in order to bypass the `_sigtramp → sigreturn`. This is
explained further below (§5.2).

### 5.2 Avoiding `_sigtramp` and `__sigreturn`
Explained simply, when a signal is received on Darwin and the signal 
handler has been registered for the specific signal, the `_sigtramp`
function is called, and `_sigtramp` is responsible for finding and invoking
the user's handler. When the user's handler returns, it will return to
`_sigtramp` which will then call `__sigreturn` in order to restore the
previous context before the signal was received. However, when the restart
thread would call `setcontext` to return to `ckpt_handler` and then return
to `_sigtramp`, the subsequent call to `__sigreturn` would fail and an
abort/trap would occur. The exact implementation of `_sigtramp` on Darwin 
is as follows:

```sigtramp.c in libsystem_platform.dylib
void _sigtramp(union __sigaction_u __sigaction_u, int sigstyle, int sig,
               siginfo_t *sinfo, ucontext_t *uctx, uintptr_t token)
{
        ...
        /* invoke handler */
        sa_sigaction(sig, sinfo, uctx);
        ...
        __sigreturn(uctx, ctxstyle, token);
        __builtin_trap(); /* __sigreturn returning is a fatal error */
}
```

Rather than debugging the exact inconsistency or validation failure which
causes `__sigreturn` to fail, a second call to `setcontext` is used to
return from `ckpt_handler` during a restart. This does not affect program
correctness as it restores the `ucontext_t` which is pushed onto the signal
handler's frame, but bypasses any validation performed by `__sigreturn`.
Note that the `ucontext_t` included as the third argument of an
`SA_SIGINFO` style signal handler can also include PAC-signed registers,
and thus `pac_patch_ucontext` strips and re-signs PAC'd pointers before
calling `setcontext` to return from the signal handler. Pointer 
authentication (and its implications on checkpoint-restart) will be 
discussed in more detail later on (§6).

### 5.3 TLS check via `TPIDRRO_EL0`
```
mrs Xd, tpidrro_el0 
```

`TPIDRRO_EL0` is the thread-pointer register on arm64, used by Darwin for
each thread's TLS base address. This register is only readable from 
userspace, whereas the kernel reads and writes to this register, for
example, on a context switch. The checkpoint handler, `ckpt_handler` reads
and records `TPIDRRO_EL0` in a static variable, and the restart process
reads the new value of `TPIDRRO_EL0` and compares it with the value from
checkpoint time. If the TLS addresses were to differ, any load or store
using TLS (`errno`, `pthread_self`, etc.) would either corrupt program
behavior silently or cause a hard failure. 

In testing single-threaded programs, it was found that `TPIDRRO_EL0` was 
consistent between checkpoint and restart. `c/c++` test programs using the 
`thread_local` qualifier exhibited the correct checkpoint-restart behavior. 
This is possibly related to the main thread's pthread struct (`pthread_s`) 
being a global variable in Darwin's libpthread implementation 
(`libsystem_pthread.dylib`). Thus, because `libsystem_pthread's` data
segment will live in the shared cache, `TPIDRRO_EL0` will point to the
same global variable in `libsystem_pthread` as long as the shared cache
is not slid or rebuilt.

Note that, on x86 Linux, the FS register is used by threading libraries to
point to TLS, and the FS register can be read or written to from userspace
via `arch_prctl` or equivalently, `syscall(SYS_arch_prctl, ...)`. 
On arm64, there is also an additional system register, `TPIDR_EL0`, which 
is intended to be used for thread-identifying information, and it is 
readable and writeable from `ELO` (userspace). However, Darwin's 
`libsystem_pthread.dylib` chooses to use `TPIDRRO_EL0` for thread-local 
storage, and thus a checkpoint-restart implementation can not rely on
manually restoring the TLS base.

### 5.4 Reinstalling the Signal Handler on Restart
Although `libckpt.dylib's` memory segments will be restored by the
`restart` program, the signal handler, `ckpt_handler`, must be also
re-registered to run on `SIGUSR2` in order for checkpointing a process
restored by `restart`. Note that `libckpt.dylib` uses the compiler
attribute, `__attribute__((constructor))` and `DYLD_INSERT_LIBRARIES` in
order to allow it to run the `setup` function which registers a signal
handler before a target process's main function begins execution. However,
in order to register `ckpt_handler` to run on `SIGUSR2` after a restart,
the `restart` program itself must explicitly call the `setup` function.

Earlier, the issues of returning from a signal handler via the 
`__sigreturn` path were highlighted (see §5.2). This raises the following
question: Why not use the `ucontext_t` passed as an argument to the
signal handler as the context to call `setcontext` on during restart, and
thus avoid returning from a signal handler altogether. 

The requirement of restoring `ckpt_handler` in the restart process is 
exactly the reason. Without allowing the restart program to jump somewhere 
within `libckpt.dylib`, the `restart` binary should either need to link 
against `libckpt.dylib` or use `dlopen` and `dlsym` in order to call 
`setup` (which register `ckpt_handler` to run on `SIGUSR2`). Either of 
these approaches would be wasteful due to the fact that `libckpt.dylib` is 
already implicitly within the `restart` process once its text and data have 
been restored. Thus, the only restriction is that the restart program can
not be compiled if directly referencing symbols within `libckpt.dylib`, so
having the restart process call `setcontext` back into `libckpt.dylib` is
essential. From within `ckpt_handler` the restart path can simply call
`setup` and the signal handler will be registered, allowing for the
restart process to receive additional, subsequent checkpoints.

## 6. Pointer Authentication (PAC)
Handling pointer authencation for checkpoint-restart is the largest 
contribution of this work. Apple Silicon devices implement the ARMv8.3
pointer authentication feature and is used by the `arm64e` ABI. In order
to provide the reader with the sufficient context on PAC and the relevant
vocabulary, the following section explains PAC basics. Then, its 
implications on checkpoint-restart, and then approach taken to accomplish
transparent checkpoint-restart of arm64e binaries (or arm64 binaries which
load arm64e shared objects), are discussed in detailed.

### 6.1 PAC Basics
ARMv8.3 uses four relevant PAC key registers:

|     Key       |   Purpose   | Per-process? |
|---------------|-------------|--------------|
|  APIAKey_EL1  | Instruction |     No       |
|  APIBKey_EL1  | Instruction |   **Yes**    |
|  APDAKey_EL1  |     Data    |     No       |
|  APDBKey_EL1  |     Data    |   **Yes**    |

The `B` keys are per-process, meaning that they are generated randomly
for each process executing on the system. For example, on `exec` call,
the PAC system registers will be changed. The `A` keys are
process-independent, meaning that every process executing on the system
shares the same `DA` (`APDAKey_EL1`) and `IA` (`APIAKey_EL1`) keys, and
these keys will only be randomized be system reboot. Here on out, the
PAC family of system registers will be referred to as the 'IA', 'IB', 'DA'
and 'DB' keys.

In order to implement pointer authentication functionality, the arm64e ABI 
uses `PAC` and `AUT` instructions, where the former signs a pointer with a 
PAC and the latter verifies a PAC'd pointer before a dereference. PAC 
instructions additionally use a modifier when signing and authenticating
pointers. Modifiers are their possible values are discussed in more detail 
in section 6.3.

There are additional PAC related instructions which extend existings arm64 
instructions with PAC functionality such as `blraa Xn, Xm` (branch with 
link and authenticate target address, `Xn`, with `IA` and modifier, `Xm`)
as well as `retab` (authenticate link register with `IB` and `sp` as
modifier and branch to return address). Lastly, there are a pair of
instructions, `xpaci` and `xpacd`, which unconditionally strip a PAC'd
pointer.

In general, the most common use cases of pointer authentcation are related
to link register signing. The basic function prologue and epilogue used
on the arm64e ABI is as follows:
```
pacibsp
sub sp, sp, CALLFRAME_SIZE
stp x29, x30, [sp, CALLFRAME_SIZE - 16]
...
ldp x29, x30, [sp, CALLFRAME_SIZE - 16]
add sp, sp, CALLFRAME_SIZE
retab
```
The `pacibsp` instruction signs the link register (`x30`) with the `IB`
key and the stack pointer (`sp`) at function entry. Then, the frame is
pushed onto the stack. Once the function epilogue is reached, the frame
is popped off the stack and `retab` authenticates the link register with
the `IB` key and the stack pointer as modifier, then branching to the
return address. Note that using the stack pointer as a PAC modifier
dynamically diversifies the PAC signature while guaranteeing that the
values at the time of signing and authentication are equivalent.
Additionally, one might notice that the standard for return address signing
with PAC implies that, for a checkpoint library, beyond a saved register
context, the contents of the stack will also include PAC'd pointers.

### 6.2 PAC Bit Layout
```c
#define PAC_BIT_MASK       (0xFF7F000000000000ULL)
#define PTRAUTH_SIGNED(p)  (((p) & ~(1ull << 55)) & PAC_BIT_MASK)
#define PTRAUTH_FAILED(p)  (((p) >> 61) == 0b01 || (p) >> 62) == 0b10)
```

ARMv8.3 PAC stores a PAC signature in the upper bits of a pointer which
are unused due to the fact that 64 bit systems will typically only use
a virtual address size of around 48 bits. Assuming a 48-bit virtual 
address size, bits 63:48 are available for storing a PAC signature.
Note in the macro `PTRAUTH_SIGNED` that bit 55 is masked. This is because
PAC instructions intentionally leave bit 55 untouched in order to 
differentiate between kernel and user addresses (0xFFF... vs 0x000...).
From the definition of `PTRAUTH_FAILED`, observe that a specific bit
pattern in the high bits of a pointer can be used to see if an `aut*`
instruction failed. This is because authentication failures do not generate
immediate traps, but rather set a specific bit pattern that will likely
cause a subsequent pointer derefence to cause a segmentation fault.

### 6.3 PAC Modifiers
PAC instructions use a 64-bit **modifier** such that the same pointer
signed with the same key, but a different modifier, generates two distinct
PAC'd pointers. Apple's ABI uses several PAC conventions which utilize
modifiers:

- **`pacibsp`** - sign LR (`x30`) with IB key and `sp` (at function entry). 
This instruction is emitted in the prologue of every non-leaf function; 
non-leaf functions will push the link register signed by a `pacibsp` 
instruction onto the stack. In the function epilogue, an `autibsp` or 
`retab` instruction authenticates the link register.
- **Contant Modifiers** - Certain system calls and library functions use
constant modifiers for PAC signatures. For example, `getcontext` signs
the frame pointer and stack pointer saved into a  `ucontext_t` with 
constant modifiers. Reflexively, `setcontext` will authenticate the frame
pointer and stack pointer found in the `ucontext_t` passed to it with
the same constant modifiers used by `getcontext`. 
- **Blended Modifiers/Storage Addresses** - Function pointers commonly use
the storage address, and possibly an additional 16-bit constant, as a
modifier for pointer authentication. 

### 6.4 Stripping and Re-signing a Saved Context: `pac_strip_context`
After a call to `getcontext`, the saved `ucontext_t` will contain, among
the calle-saved general-purpose and NEON registers saved by `getcontext`,
a PAC-signed link register, frame pointer and stack pointer. The relevant
code from `getcontext.s` in `libsystem_platform.dylib` included:

```
...
#if defined(__arm64e__)
        mov     \fp, fp
        mov     x9, #17687 // ptrauth_string_discriminator("fp")
        pacda   \fp, x9

        mov     \sp, sp
        mov     x9, #52205 // ptrauth_string_discriminator("sp")
        pacda   \sp, x9

        mov     \lr, lr
        mov     \flags, LR_SIGNED_WITH_IB
...
```

`getcontext` explicitly signs the `ucontext_t's` saved `fp` and `sp` 
values with the DA key and a modifer from `ptrauth_string_discriminator`.
The saved `lr` is not signed; as mentioned previously, every non-leaf 
function in an arm64e binary will, by default, sign the link register in
every function prologue. Thus, `getcontext` only sets a flag indicating 
that the link register from `getcontext` is signed.

Some time later, a call to `setcontext` will reflexively authenticate
the registers saved within the `ucontext_t`:

```
...
#if defined(__arm64e__)
        // authenticate sp with constant discriminator
        mov     x9, #52205
        autda   \sp, x9
        ldr     xzr, [\sp] // will seg fault if restored sp is corrupt
        
        // authenticate fp with constant discriminator
        mov     x9, #17687
        autda   \fp, x9
        mov     fp, \fp
        
        // if LR_SIGNED_WITH_IB_BIT is set, skip authenticating lr
        mov     lr, \lr
        tbnz    \flags, LR_SIGNED_IB_BIT, 2f
        
        // authenticate lr with constant discriminator
        mov     x16, \lr
        mov     x17, x16
        mov     x9, #30675
        autia   x16, x9         // authenticate x16
        xpaci   x17             // strip x17
        cmp     x16, x17        // if x16 = x17, authentication succeeded
        b.eq    1f
        brk     #666
...
1:
        mov     lr, x16
        pacibsp
2:
        mov     sp, \sp
        mov     fp, \fp
        mov     lr, \lr
```

First, `setcontext` authenticates the restored `sp` and `fp` using the
same constant discriminators that were used during `getcontext`. Then,
if the bit, `LR_SIGNED_WITH_IB_BIT`, is set in the `ucontext_t's` flags,
`lr` is not manually authenticated. Otherwise, the link register is
authenticated using the IA key and a constant discriminator.

Given the macOS implementations of `getcontext`/`setcontext`, saving
a register context as part of a checkpoint image, and restoring the
context upon restart is straightfoward. The context saved by `getcontext`
is fully stripped of any PAC signatures before a checkpoint. The
checkpoint implementation will additionally record the PAC key and modifier
that was used for each signed pointer in the register context. This is
quite easy to accomplish, as the saved `fp` and `sp` are hard-coded 
constants discoverable in `libsystem_platform's` source or in a debugger.
For the link register specifically, it will not be directly signed by
`getcontext`, but the `ucontext_t's` saved `lr` would have been signed
before the previous call frame was pushed onto the stack. Thus, it can
be inferred that the link register was signed with the IB key and `sp` as
it would be in any arm64e function prologue.

The responsibility of stripping PAC-signed registers in a `ucontext_t` and
recording information describing how each PAC'd register was signed is
handled in `pac_strip_context`:

```
int pac_strip_context(ckpt_context_t *ctx)
{
        u64 lr = get_ucontext_lr(&ctx->uc);

        if (PTRAUTH_SIGNED(lr)) {
                /* mark bitmap */
                ctx->bitmap |= (1ull << ARM64_LR);
                u64 old = lr;
                
                /* modifier candidates */
                u64 uctx_sp = get_ucontext_sp(&ctx->uc);
                u64 prev_sp = get_ucontext_fp(&ctx->uc) + 0x10;

                /* strip lr and assign to saved lr in ucontext_t */
                XPACI(lr);
                set_ucontext_lr(&ctx->uc, lr);
                
                u64 getcontext_pacibsp  = lr;
                u64 prev_pacibsp        = lr;
                u64 const_pacia         = lr;
                
                /* test different candidate signing schemes */
                PACIB(getcontext_pacibsp, uctx_sp);
                PACIB(prev_pacibsp, prev_sp);
                PACIA(const_pacia, LR_DISCRIMINATOR);
                
                /* check which reproduced the correctly signed lr */
                if (getcontext_pacibsp == old)
                        ctx->modifiers[ARM64_LR] = uctx_sp;
                else if (prev_pacibsp == old)
                        ctx->modifiers[ARM64_lr] = prev_sp;
                else if (const_pacia == old)
                        ctx->modifers[ARM64_LR] = LR_DISCRIMINATOR;
                else
                        return -1; /* could not identify key + modifier */
        }

        /* do similar for fp */
        ...

        /* do similar for sp */
        ...

        return 0;
}
```

The implementation of `pac_strip_context` essentially allows the
restart program, after reading a checkpoint image file, to know exactly
enough about how the registers in the saved context were signed, such
that the signatures can be reproduced in the restart process, only now
signed with the restart process's PAC keys. Concretely, once the restart
program has read the save context as well as the PAC-related auxiliary
information, it will call `pac_sign_context` to reproduce PAC-signed 
registers before a call to `setcontext`.

```
void pac_sign_context(ckpt_context_t *ctx)
{
        u64 lr = get_ucontext_lr(&ctx->uc);
        if (ctx->bitmap & (1ull << ARM64_LR)) {
                /**
                 * properly re-sign the saved link register using
                 * the correct key and modifier pair
                 */
        } else {
                /**
                 * sign with constant lr discriminator used for
                 * authentication in setcontext
                 */
        }

        u64 sp = get_ucontext_sp(&ctx->uc);
        PACDA(sp, SP_DISCRIMINATOR);
        set_ucontext_sp(&ctx->uc, sp);

        u64 fp = get_ucontext_fp(&ctx->uc);
        PACDA(fp, FP_DISCRIMINATOR);
        set_ucontext_fp(&ctx->uc, fp);
}
```
