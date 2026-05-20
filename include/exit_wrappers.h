/* exit_wrappers.h */
#ifndef __CKPT_EXIT_WRAPPERS_H__
#define __CKPT_EXIT_WRAPPERS_H__
#include "inject.h"

/* libsystem_c.dylib exit() */
extern void exit(int);

void __exit(int);

INTERPOSE(__exit, exit);

#endif // __CKPT_EXIT_WRAPPERS_H__
