/* thread_info.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <ucontext.h>
#include <errno.h>
#include <unistd.h>
#include "ckpt.h"
#include "pac.h"
#include "thread_info.h"

_Thread_local struct thread_info        *myself = NULL;
static struct thread_info               ckpt_thread;
static struct thread_list               thread_list;

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
        thread_list.head = malloc(sizeof(struct thread_info));
        assert(thread_list.head != NULL);
        
        myself          = thread_list.head;
        myself->self    = pthread_self();
        myself->state   = ST_RUNNING;
        myself->next    = NULL;
        myself->prev    = NULL;
        myself->exiting = 0;

        pthread_mutex_init(&myself->lock, NULL);
        pthread_cond_init(&myself->cond, NULL);
        
        /**
         * Initialize and create the checkpoint thread, then
         * wait for the checkpoint thread to start up before returning
         */
        ckpt_thread.state = ST_CKPT_THREAD;
        pthread_mutex_init(&ckpt_thread.lock, NULL);
        pthread_cond_init(&ckpt_thread.cond, NULL);

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
        struct thread_info *th, *next;
        
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

struct thread_info *thread_list_find(pthread_t thread)
{
        struct thread_info *th;
        
        thread_list_acquire();
        for (th = thread_list.head; th; th = th->next) {
                if (pthread_equal(th->self, thread))
                        return th;
        }
        thread_list_release();

        return NULL;
}

/**
 * thread_list_add:
 *  Add a thread from the thread list and opportunistically remove dead
 *  threads from the list in order to free their resources. The lock is 
 *  acquired during thread_list_add and should not be held by the caller.
 *
 *  Note: Caller of thread_list_add should be the new thread that is
 *  being insterted into the list (called during thread_start trampoline
 *  function which wraps pthread_create start_routine function).
 */
void thread_list_add(void)
{
        struct thread_info      *th, *next;
        int                     err;
        
        thread_list_acquire();
        /**
         * Find threads that have exited by sending signal 0 and
         * removed zombie threads from the thread list.
         */
        for (th = thread_list.head; th; th = next) {
                next = th->next;
                if (th->exiting) {
                        if ((err = pthread_kill(th->self, 0)) != 0) {
                                assert(err == ESRCH);
                                thread_reap(th);
                        }
                }
        }
        
        /**
         * Add new thread to the thread list. The caller of
         * thread_list_add should be the new thread itself.
         */
        assert(myself != NULL);
        myself->next = thread_list.head;
        myself->prev = NULL;
        if (myself->next)
                myself->next->prev = myself;
        
        thread_list.head = myself;
        thread_list_release();
}

/**
 * thread_init:
 *  Initialize new thread with start routine and argument that were
 *  included as arguments to pthread_create.
 *
 *  thread_init should be called by the pthread_create wrapper to
 *  initialize a new thread, and the thread should added to the thread
 *  list by calling thread_list_add in the thread start routine
 *  wrapper/trampoline function.
 */
struct thread_info *thread_init(void *(*fn)(void *), void *arg)
{
        struct thread_info *new;

        new = malloc(sizeof(struct thread_info));
        assert(new != NULL);

        new->fn         = fn;
        new->arg        = arg;
        new->exiting    = 0;
        new->state      = ST_RUNNING;
        new->next       = NULL;
        new->prev       = NULL;

        pthread_mutex_init(&new->lock, NULL);
        pthread_cond_init(&new->cond, NULL);

        return new;
}

/**
 * thread_reap:
 *  Remove thread from active thread list and free resources
 *  associated with it. Assume the lock is already held by the
 *  caller.
 */
void thread_reap(struct thread_info *zombie)
{
        if (zombie == thread_list.head) {
                thread_list.head = zombie->next;
                thread_list.head->prev = NULL;
        } else if (zombie->prev && zombie->next) {
                zombie->prev->next = zombie->next;
                zombie->next->prev = zombie->prev;
        } else if (zombie->prev)
                zombie->prev->next = NULL;

        pthread_mutex_destroy(&zombie->lock);
        pthread_cond_destroy(&zombie->cond);
        free(zombie);
}

void thread_exit(void)
{
        assert(myself != NULL);
        myself->exiting = 1;
        
        /**
         * If any threads are in the pthread_join wrapper waiting 
         * on this thread's condition variable (in pthread_cond_timedwait),
         * notify by sending a signal so the can now call the real
         * pthread_join without blocking.
         */
        pthread_mutex_lock(&myself->lock);
        pthread_cond_signal(&myself->cond);
        pthread_mutex_unlock(&myself->lock);
}

struct thread_info *thread_self(void)
{
        assert(myself != NULL);
        return myself;
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
        static volatile int restart;
        {
                /**
                 * Block SIGUSR1 and unblock SIGUSR2. SIGUSR2 will be
                 * used to signal to the checkpoint thread to initiate
                 * a checkpoint, while SIGUSR1 will be used to signal
                 * user threads to save their context and suspend.
                 */
                sigset_t sigblock, sigunblock;

                sigemptyset(&sigblock);
                sigaddset(&sigblock, SIGUSR1);
                pthread_sigmask(SIG_BLOCK, &sigblock, NULL);

                sigemptyset(&sigunblock);
                sigaddset(&sigunblock, SIGUSR2);
                pthread_sigmask(SIG_UNBLOCK, &sigunblock, NULL);
        }
        
        myself          = &ckpt_thread;
        myself->self    = pthread_self();
        
        /**
         * Signal to main thread that the checkpoint thread has
         * initialized itself and is ready
         */
        pthread_mutex_lock(&myself->lock);
        *(uintptr_t *)arg = 1;
        pthread_cond_signal(&myself->cond);
        pthread_mutex_unlock(&myself->lock);

        restart = 0;
        getcontext(&myself->uc);

        if (restart) {
                myself  = &ckpt_thread;
                restart = 0;

                thread_restore_sig_state();
                postrestart();

                restore_threads();
                resume_threads();
        }
        
        restart = 1;
        for (;;) {
                ckpt_thread_wait();
                
                thread_save_tls();
                thread_save_sig_state();

                suspend_threads();
                wait_for_exiting_threads();

                precheckpoint();
                docheckpoint(&myself->uc);

                resume_threads();
        }

        return NULL;
}

/**
 * scan_threads:
 *  Scan thread list and suspend thread by sending SIGUSR1, and increment
 *  a counter to indentify how many user threads are active and should
 *  be participating in the checkpoint.
 *
 * thread_list.lock should be held by the caller to scan_threads.
 * A positive integer will be returned if any threads are not suspended
 * or confirmed to have exited. Thus, a rescan will be necessary before
 * the suspend_threads phase can complete.
 */
int scan_threads(int *nthreads)
{
        struct thread_info      *th, *next;
        int                     rescan, err;
        
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
                usleep(10);
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
        struct thread_info *th;
        
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
        struct thread_info      *th, *next;
        int                     exiting, killed;

        thread_list_acquire();
        do {
                killed  = 0;
                exiting = 0;
                for (th = thread_list.head; th; th = next) {
                        next = th->next;
                        if (th->exiting) {
                                exiting++;
                                if (pthread_kill(th->self, 0) == ESRCH) {
                                        killed++;
                                        thread_reap(th);
                                }
                        }
                }
                if (killed != exiting)
                        usleep(10);
        } while (killed != exiting);
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
        assert(myself != NULL);
        if (myself->state == ST_CKPT_THREAD)
                return;
        
        if (!thread_state_cas(myself, ST_SIGNALED, ST_SUSPINPROG)) {
                /**
                 * Prevent user threads from entering signal handler
                 * more than once
                 */
                return;
        }
        
        /* Save state and transition to suspended */
        thread_save_tls();
        thread_save_sig_state();
        thread_save_context((ucontext_t *)uctx);
        assert(thread_state_cas(myself, ST_SUSPINPROG, ST_SUSPENDED));
        
        /* Wait in barrier and then resume */
        thread_barrier();
        assert(thread_state_cas(myself, ST_SUSPENDED, ST_RUNNING));
}

void *thread_start(void *thread)
{
        void *retval;
        
        /**
         * Set thread local pointer to thread descriptor to point to
         * newly allocated thread struct, and initialize pthread_t
         * field.
         */
        myself          = (struct thread_info *)thread;
        myself->self    = pthread_self();
        
        thread_list_add();
        retval = myself->fn(myself->arg);
        thread_exit();

        return retval;
}

void *thread_restart(void *thread)
{
        /* Reinitialize and set state to suspended */
        myself          = (struct thread_info *)thread;
        myself->self    = pthread_self();
        myself->state   = ST_SUSPENDED;
        
        /**
         * Restore signal state and wait at barrier before restoring
         * user context.
         */
        thread_restore_sig_state();
        thread_barrier();
        
        assert(thread_state_cas(myself, ST_SUSPENDED, ST_RUNNING));
        thread_restore_context();

        __builtin_unreachable();
}

void thread_save_tls(void)
{
        assert(myself != NULL);
        asm volatile(
                "mrs %[tls], tpidrro_el0"
                : [tls] "=r" (myself->tls)
                :
                : "memory"
        );
}

void thread_save_context(ucontext_t *ucp)
{
        memcpy(&myself->uc, ucp, sizeof(ucontext_t));
        memcpy(&myself->uc.__mcontext_data, ucp->uc_mcontext,
               sizeof(myself->uc.__mcontext_data));

        myself->uc.uc_mcontext = (mcontext_t)&myself->uc.__mcontext_data;
}

void thread_restore_context(void)
{
        u64 fp;

        pac_patch_context(&myself->uc);
        fp = get_ucontext_fp(&myself->uc);
        if (PTRAUTH_SIGNED(fp))
                XPACD(fp);
        pac_resign_frames((u64 *)fp);

        if (setcontext(&myself->uc) < 0) {
                perror("setcontext");
                thread_exit();
                pthread_exit(NULL);
        }

        __builtin_unreachable();
}

void thread_save_sig_state(void)
{
        pthread_sigmask(SIG_SETMASK, NULL, &myself->sigblocked);
}

void thread_restore_sig_state(void)
{
        pthread_sigmask(SIG_SETMASK, &myself->sigblocked, NULL);
}
