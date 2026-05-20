/* exit_wrappers.h */
#ifndef __CKPT_EXIT_WRAPPERS_H__
#define __CKPT_EXIT_WRAPPERS_H__
#include "inject.h"

/* libsystem_c.dylib exit() */
extern void exit(int);
extern void abort(void);

void __exit(int);
void __abort(void);

INTERPOSE(__exit, exit);
INTERPOSE(__abort, abort);

#endif // __CKPT_EXIT_WRAPPERS_H__
