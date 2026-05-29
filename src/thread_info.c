/* thread_info.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <assert.h>
#include "ckpt.h"
#include "pac.h"
#include "thread_info.h"

__thread thread_info_t  *self = NULL;
static thread_info_t    ckpt_thread;
static thread_list_t    thread_list;

static int              threads_expected;
static int              threads_arrived;
static int              ckpt_epoch      = 0;
static pthread_cond_t   cond_arrived    = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   cond_released   = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t  ckpt_mtx        = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor(101)))
void thread_list_init(void)
{
        uintptr_t ckpt_thread_ready = 0;
        
        pthread_mutex_init(&thread_list.lock, NULL);

        /* Initialize main thread info */
        thread_list.head = malloc(sizeof(thread_info_t));
        assert(thread_list.head != NULL);
        
        self            = thread_list.head;
        self->self      = pthread_self();
        self->state     = ST_RUNNING;
        self->next      = NULL;

        pthread_mutex_init(&self->lock, NULL);
        pthread_cond_init(&self->cond, NULL);

        /* Initialize and launch checkpoint thread */
        ckpt_thread.state = ST_CKPT_THREAD;
        pthread_mutex_init(&ckpt_thread.lock);
        pthread_cond_init(&ckpt_thread.cond);
        
        /* Wait for checkpoint thread to initialize */
        pthread_mutex_lock(&ckpt_thread.lock);
        pthread_create(&ckpt_thread.self, NULL, ckpt_thread_work,
                       (void *)&ckpt_thread_ready);
        while (!ckpt_thread_ready)
                pthread_cond_wait(&ckpt_thread.cond, &ckpt_thread.lock);
        pthread_mutex_unlock(&ckpt_thread.lock);
}

__attribute__((destructor))
void thread_list_destroy(void)
{
        thread_info_t *th, *next;

        thread_list_acquire();
        for (th = thread_list.head; th; th = next) {
                next = th->next;
                pthread_mutex_destroy(&th->lock);
                pthread_cond_destroy(&th->cond);
                free(th);
        }
        
        thread_list_release();
        pthread_mutex_destroy(&thread_list.lock);
}

void thread_list_acquire(void)
{
        pthread_mutex_lock(&thread_list.lock);
}

void thread_list_release(void)
{
        pthread_mutex_unlock(&thread_list.lock);
}

/**
 * thread_list_add:
 *  Add a thread from the thread list and opportunistically remove dead
 *  threads from the list in order to free their resources. The lock is 
 *  acquired during thread_list_add and should not be held by the caller.
 */
void thread_list_add(thread_info_t *new)
{
        thread_info_t   *th, *next;
        int             err;
        
        self = new;
        thread_list_acquire();

        for (th = thread_list.head; th; th = next) {
                next = th->next;
                if (th->exiting) {
                        if ((err = pthread_kill(th->self, 0)) != 0) {
                                assert(err = ESRCH);
                                thread_reap(th);
                        }
                }
        }
        
        self->next = thread_list.head;
        self->prev = NULL;
        if (self->next)
                self->next->prev = self;
        
        thread_list.head = self;
        thread_list_release();
}

/**
 * thread_reap:
 *  Remove thread from active thread list and free resources
 *  associated with it. Assume the lock is already held by the
 *  caller.
 */
void thread_reap(thread_info_t *zombie)
{
        if (zombie->prev)
                zombie->prev->next = zombie->next;
        if (zombie->next)
                zombie->next->prev = zombie->prev;
        if (zombie == thread_list.head)
                thread_list.head = thread_list.head->next;
        
        pthread_mutex_destroy(&th->lock);
        pthread_cond_destroy(&th->cond);
        free(th);
}

void ckpt_thread_wait(void)
{
        int             sig;
        sigset_t        set;

        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        
        while (sigwait(&set, &sig) != 0 || sig != SIGUSR2)
                ;
}

void *ckpt_thread_work(void *arg)
{
        static volatile int     restart;
        {
                sigset_t sigblock, sigunblock;

                sigemptyset(&sigblock);
                sigaddset(&sigblock, SIGUSR1);
                pthread_sigmask(SIG_BLOCK, &sigblock, NULL);

                sigemptyset(&sigunblock);
                sigaddset(&sigunblock, SIGUSR2);
                pthread_sigmask(SIG_UNBLOCK, &sigunblock, NULL);
        }

        self            = &ckpt_thread;
        self->self      = pthread_self();
        
        /**
         * Signal to main thread that the checkpoint thread has
         * initialized itself and is ready
         */
        pthread_mutex_lock(&self->lock);
        *(uintptr_t *)arg = 1;
        pthread_cond_signal(&self->cond);
        pthread_mutex_unlock(&self->lock);

        restart = 0;
        getcontext(&self->uc);

        if (restart) {
                self    = &ckpt_thread;
                restart = 0;

                thread_restore_sig_state(self);
                postrestart();

                restore_threads();
                resume_threads();
        }
        
        restart = 1;
        for (;;) {
                ckpt_thread_wait();
                
                thread_save_tls(&self);
                thread_save_sig_state(&self);

                suspend_threads();
                wait_for_exiting_threads();

                precheckpoint();
                docheckpoint(&self->uc);

                resume_threads();
        }
}

int scan_threads(int *nthreads)
{
        thread_info_t   *th, *next;
        int             rescan, err;
        
        rescan = 0;
        *nthreads = 0;
        for (th = thread_list.head; th; th = next) {
                next = th->next;
        
                if (th->exiting || th->state == ST_CKPT_THREAD)
                        continue;

                if (th->state == ST_RUNNING &&
                    thread_state_cas(th, ST_RUNNING, ST_SIGNALED)) {
                        err = pthread_kill(th->self, SIGUSR1);
                        if (err != 0) {
                                assert(err == ESRCH);
                                thread_reap(th);
                                continue;
                        }
                        rescan = 1;
                } else if (th->state == ST_SIGNALED) {
                        err = pthread_kill(th->self, 0);
                        if (err != 0) {
                                assert(err == ESRCH);
                                thread_reap(th);
                                continue;
                        }
                        rescan = 1;
                } else if (th->state == ST_SUSPENDED ||
                           th->state == ST_SUSPINPROG) {
                        ++(*nthreads);
                } else if (th->state == ST_THREAD_CREATE)
                        rescan = 1;
        }

        return rescan;
}

void suspend_threads(void)
{
        int nthreads, rescan;
        
        pthread_mutex_lock(&ckpt_mtx);
        thread_list_acquire();
        while ((rescan = scan_threads(&nthreads)))
                ;
        thread_list_release();

        /* Wait for all threads to reach barrier */
        threads_arrived = 0;
        threads_expected = nthreads;
        while (threads_arrived != threads_expected)
                pthread_cond_wait(&cond_arrived, &ckpt_mtx);
        pthread_mutex_unlock(&ckpt_mtx);
}

void resume_threads(void)
{
        pthread_mutex_lock(&ckpt_mtx);
        ckpt_epoch++;
        pthread_cond_broadcast(&cond_released);
        pthread_mutex_unlock(&ckpt_mtx);
}

void restore_threads(void)
{
        thread_info_t *th;
        
        thread_list_acquire();
        pthread_mutex_lock(&ckpt_mtx);

        threads_arrived = 0;
        threads_expected = 0;
        
        for (th = thread_list.head; th; th = th->next) {
                threads_expected++;
                pthread_create(&th->self, NULL, thread_restart, th);
        }

        while (threads_arrived != threads_expected)
                pthread_cond_wait(&cond_arrived, &ckpt_mtx);
        
        thread_list_release();
        pthread_mutex_unlock(&ckpt_mtx);
}

void wait_for_exiting_threads(void)
{
        thread_info_t   *th, *next;
        int             rescan, err;
        
        thread_list_acquire();
        do {
                rescan = 0;
                for (th = thread_list.head; th; th = next) {
                        next = th->next;
                        if (th->exiting) {
                                err = pthread_kill(th->self, 0);
                                if (err != 0) {
                                        assert(err == ESRCH);
                                        /**
                                         * Kill thread and free
                                         * resources associated with it
                                         */
                                        continue;
                                }
                                rescan = 1;
                        }
                }
                if (rescan)
                        usleep(100);
        } while (rescan);
        thread_list_release();
}

void thread_barrier(void)
{
        int epoch;

        pthread_mutex_lock(&ckpt_mtx);
        epoch = ckpt_epoch;
        threads_arrived++;
        
        if (threads_arrived == threads_expected)
                pthread_cond_signal(&cond_arrived);

        while (epoch == ckpt_epoch)
                pthread_cond_wait(&cond_released, &ckpt_mtx);

        pthread_mutex_unlock(&ckpt_mtx);
}

void thread_sighandler(int sig, siginfo_t *info, void *uctx)
{
        int epoch;

        assert(self != NULL);
        if (self->state == ST_CKPT_THREAD)
                return;
        
        if (!thread_state_cas(self, ST_SIGNALED, ST_SUSPINPROG)) {
                /**
                 * Prevent user threads from entering signal handler
                 * more than once
                 */
                return;
        }
        
        /* Save state and transition to suspended */
        thread_save_tls(self);
        thread_save_sig_state(self);
        thread_save_context(self, (ucontext_t *)uctx);
        assert(thread_state_cas(self, ST_SUSPINPROG, ST_SUSPENDED));
        
        /* Wait in barrier and then resume */
        thread_barrier();
        assert(thread_state_cas(self, ST_SUSPENDED, ST_RUNNING));
}

void *thread_start(void *thread)
{
        void *retval;
        
        self            = (thread_info_t *)thread;
        self->self      = pthread_self();
        self->state     = ST_RUNNING;

        pthread_cleanup_push(thread_exit, self);
        retval = self->fn(self->arg);
        pthread_cleanup_pop(1);

        return retval;
}

void *thread_restart(void *arg)
{
        self            = (thread_info_t *)arg;
        self->self      = pthread_self();
        self->state     = ST_SUSPENDED;

        thread_restore_sig_state(self);
        thread_barrier();
        
        assert(thread_state_cas(self, ST_SUSPENDED, ST_RUNNING));
        thread_restore_context(self);

        __builtin_unreachable();
}

void thread_save_tls(thread_info_t *th)
{
        asm volatile("mrs %0, tpidrro_el0" : "=r" (th->tls) :: "memory");
}

void thread_save_context(thread_info_t *th, ucontext_t *ucp)
{
        memcpy(&th->uc, ucp, sizeof(ucontext_t));
        memcpy(&th->uc.__mcontext_data, ucp->uc_mcontext,
               sizeof(th->uc.__mcontext_data));
        th->uc.uc_mcontext = (mcontext_t)&th->uc.__mcontext_data;
}

void thread_restore_context(thread_info_t *th)
{
        u64 fp;
        
        pac_patch_context(&th->uc);
        fp = get_ucontext_fp(&th->uc);
        if (PTRAUTH_SIGNED(fp))
                XPACD(fp);
        pac_resign_frames((u64 *)fp);

        if (setcontext(&th->uc) < 0)
                perror("setcontext");
}

void thread_save_sig_state(thread_info_t *th)
{
        pthread_sigmask(SIG_SETMASK, NULL, &th->sigblocked);
}

void thread_restore_sig_state(thread_info_t *th)
{
        pthread_sigmask(SIG_SETMASK, &th->sigblocked, NULL);
}
