/* pthread_wrappers.h */
#ifndef __PTHREAD_WRAPPERS_H__
#define __PTHREAD_WRAPPERS_H__
#include <pthread.h>
#include "inject.h"

#define PTHREAD_SELF_DISCRIMINATOR      0x5b9
#define PTHREAD_SELF_TLS_OFFSET         0xe0

extern int      pthread_create(pthread_t *, const pthread_attr_t *,
                               void *(*)(void *), void *);
extern void     pthread_exit(void *);
extern int      pthread_join(pthread_t, void **);

void    __pthread_cookie(void);
void    __pthread_slot_fixup(void);
int     __pthread_create_hook(pthread_t *, const pthread_attr_t *,
                              void *(*)(void *), void *);
void    __pthread_exit_hook(void *);
int     __pthread_join_hook(pthread_t, void **);

INTERPOSE(__pthread_create_hook, pthread_create);
INTERPOSE(__pthread_exit_hook, pthread_exit);
INTERPOSE(__pthread_join_hook, pthread_join);

#endif // __PTHREAD_WRAPPERS_H__
