/* pthread_wrappers.c */
#define _XOPEN_SOURCE
#include <stdio.h>
#include <assert.h>
#include "pthread_wrappers.h"
#include "thread_info.h"
#include "types.h"
#include "pac.h"

static uintptr_t pthread_xor_cookie;

void __pthread_create_hook(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start_routine)(void *), void *arg)
{
        int             retval;
        thread_info_t   *self, *new;
        
        self = thread_list_self();
        new = thread_init(start_routine, arg);
        assert(new != NULL);

        assert(thread_state_cas(self, ST_RUNNING, ST_THREAD_CREATE));
        retval = pthread_create(thread, attr, start_routine, arg);
        assert(thread_state_cas(self, ST_THREAD_CREATE, ST_RUNNING));

        if (retval != 0)
                thread_destroy(new);

        return retval;
}

void __pthread_exit_hook(void *value_ptr)
{

}

void __pthread_cookie()
{
        uintptr_t tls, self, signed_ptr, slot;
        
        asm volatile("mrs %0, tpidrro_el0" : "=r" (tls) :: "memory");
        
        /**
         * Find libpthread xor cookie by signing pthread_self pointer
         * with libpthread constant discriminator xor'd with the
         * slot value (offset 0 of pthread_self pointer). Then, the
         * cookie will used be at restart to calculate the new slot value
         * that reconstructs the pthread_self signed pointer (given the
         * DB key in the new process).
         *
         * pthread_self = tls - PTHREAD_SELF_TLS_OFFSET
         * slot = pthread_self[0]
         * signed_ptr = pacdb(pthread_self, PTHREAD_SELF_DISCRIMINATOR)
         *
         * pthread_xor_cookie = slot ^ signed_ptr
         */
        self            = tls - PTHREAD_SELF_TLS_OFFSET;
        slot            = *(uintptr_t *)self;
        signed_ptr      = self;

        PACDB(signed_ptr, (u64)PTHREAD_SELF_DISCRIMINATOR);
        pthread_xor_cookie = slot ^ signed_ptr;
}

void __pthread_slot_fixup()
{
        uintptr_t tls, *slot_ptr, old_ptr, new_ptr;

        asm volatile("mrs %0, tpidrro_el0" : "=r" (tls) :: "memory");
        
        /**
         * Find new pthread_self pointer address signed with DB key
         * and reconstruct slot value with signed_ptr ^ cookie.
         * Then, libpthread will use the slot value and cookie to
         * reconstruct the signed pointer at runtime, and because the
         * slot has been fixed-up, the autdb will succeed.
         *
         * slot_ptr     = (uintptr_t *)(tpidrro_el0 - 0xe0)
         * old_ptr      = pthread_xor_cookie ^ slot_ptr[0]
         *
         * new_ptr      = pac_strip_and_resign(old_ptr)
         * slot_ptr[0]  = new_ptr ^ pthread_xor_cookie
         */
        slot_ptr        = (uintptr_t *)(tls - PTHREAD_SELF_TLS_OFFSET);
        old_ptr         = pthread_xor_cookie ^ slot_ptr[0];

        new_ptr = old_ptr;
        XPACD(new_ptr);
        PACDB(new_ptr, (u64)PTHREAD_SELF_DISCRIMINATOR);

        slot_ptr[0] = new_ptr ^ pthread_xor_cookie;
}
