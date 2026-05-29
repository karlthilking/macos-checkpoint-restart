/* thread_info.h */
#ifndef __CKPT_THREAD_INFO_H__
#define __CKPT_THREAD_INFO_H__
#define _XOPEN_SOURCE
#include <ucontext.h>
#include <pthread.h>

typedef enum __thread_state     thread_state;
typedef struct thread_info_s    thread_info_t;
typedef struct thread_list_s    thread_list_t;

enum __thread_state {
        ST_NULL,
        ST_RUNNING,
        ST_SIGNALED,
        ST_SUSPINPROG,
        ST_SUSPENDED,
        ST_CKPT_THREAD,
        ST_THREAD_CREATE,
};

struct thread_list_s {
       thread_info_t    *head;
       pthread_mutex_t  lock;
};

struct thread_info_s {
        pthread_t               self;
        void                    *(*fn)(void *);
        void                    *arg;
        int                     exiting;
        
        _Atomic thread_state    state;
        ucontext_t              uc;
        uintptr_t               tls;
        sigset_t                sigblocked;

        pthread_mutex_t         lock;
        pthread_cond_t          cond;

        thread_info_t           *next;
        thread_info_t           *prev;
};

static inline int thread_state_cas(thread_info_t *th,
                                   thread_state expected, 
                                   thread_state desired)
{
        return atomic_compare_exchange_strong(&th->state, 
                                              &expected, desired);
}

__attribute__((constructor(101)))
void thread_list_init(void);

__attribute__((destructor))
void thread_list_destroy(void);

thread_info_t   *thread_list_self(void);
void            thread_list_acquire(void);
void            thread_list_release(void);
void            thread_list_add(thread_info_t *);
void            thread_list_remove(thread_info_t *);

thread_info_t   *thread_init(void *(*)(void *), void *);
void            thread_destroy(thread_info_t *);

void    ckpt_thread_wait(void);
void    *ckpt_thread_work(void *);

int     scan_threads(int *);
void    suspend_threads(void);
void    resume_threads(void);
void    restore_threads(void);
void    wait_for_exiting_threads(void);

void    thread_arrival_barrier(void);
void    thread_release_barrier(void);

void    thread_sighandler(int, siginfo_t *, void *);
void    *thread_start(void *);
void    *thread_restart(void *);

void    thread_save_tls(thread_info_t *);
void    thread_save_context(thread_info_t *, ucontext_t *);

#endif // __CKPT_THREAD_INFO_H__
