/* time_wrappers.c */
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "time_wrappers.h"

unsigned int __sleep(unsigned int seconds)
{
        int             retval;
        struct timespec rq = { seconds, 0 };
        struct timespec rm = { 0, 0 };
        
        retval = __nanosleep(&rq, &rm);
        if (retval != 0)
                return rm.tv_sec;

        return 0;
}

int __usleep(useconds_t microseconds)
{
        int             retval;
        struct timespec rq, rm = { 0, 0 };
        long            nanos;

        nanos           = microseconds * NSEC_PER_USEC;
        rq.tv_sec       = nanos / NSEC_PER_SEC;
        rq.tv_nsec      = nanos % NSEC_PER_SEC;

        retval = __nanosleep(&rq, &rm);
        if (retval != 0)
                return -1;

        return 0;
}

int __nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
        int                     retval;
        struct timeval          tv;
        struct timespec         start, end;

        TIMESPEC_TO_TIMEVAL(rqtp, &tv);
        if (rmtp != NULL)
                clock_gettime(CLOCK_MONOTONIC, &start);
        
        retval = select(0, NULL, NULL, NULL, &tv);
        if (retval != 0 && errno == EINTR && rmtp) {
                long rq, el, rm;

                clock_gettime(CLOCK_MONOTONIC, &end);
                
                rq = TIMESPEC_TO_NSEC(rqtp);
                el = TIMESPEC_NSEC_DIFF(&end, &start);
                rm = rq - el;
                
                rmtp->tv_sec = rm / NSEC_PER_SEC;
                rmtp->tv_nsec = rm % NSEC_PER_SEC;
        }

        return retval;
}
