/* pthread_wrappers.h */
#ifndef __PTHREAD_WRAPPERS_H__
#define __PTHREAD_WRAPPERS_H__
#include <pthread.h>
#include "inject.h"

#define PTHREAD_SELF_DISCRIMINATOR      0x5b9
#define PTHREAD_SELF_TLS_OFFSET         0xe0

void pthread_init(void);
void pthread_fixup(void);

#endif // __PTHREAD_WRAPPERS_H__
