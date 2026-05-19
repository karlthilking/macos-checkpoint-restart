/* time_wrappers.h */
#ifndef __CKPT_TIME_WRAPPERS_H__
#define __CKPT_TIME_WRAPPERS_H__
#include "inject.h"

#define TIMESPEC_TO_TIMEVAL(__tsp, __tvp) do {                  \
        (__tvp)->tv_sec  = (__tsp)->tv_sec;                     \
        (__tvp)->tv_usec = ((__tsp)->tv_nsec / 1000);           \
} while (0)

#define TIMEVAL_TO_TIMESPEC(__tvp, __tsp) do {                  \
        (__tsp)->tv_sec  = (__tvp)->tv_sec;                     \
        (__tsp)->tv_nsec = (__tvp)->tv_usec * 1000;             \
} while (0)

#define TIMESPEC_TO_NSEC(__tsp) \
        (((__tsp)->tv_sec * NSEC_PER_SEC) + (__tsp)->tv_nsec)

#define TIMESPEC_NSEC_DIFF(__first, __second) \
        (TIMESPEC_TO_NSEC((__first))) - (TIMESPEC_TO_NSEC((__second)))

#define NSEC_PER_USEC   (1000L)
#define NSEC_PER_MSEC   (1000000L)
#define USEC_PER_SEC    (1000000L)
#define NSEC_PER_SEC    (1000000000L)

extern unsigned int sleep(unsigned int);
extern int nanosleep(const struct timespec *, struct timespec *);

unsigned int __sleep(unsigned int);
int __usleep(useconds_t);
int __nanosleep(const struct timespec *, struct timespec *);

INTERPOSE(__sleep, sleep)
INTERPOSE(__usleep, usleep);
INTERPOSE(__nanosleep, nanosleep)

#endif // __CKPT_TIME_WRAPPERS_H__
