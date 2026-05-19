/* ckpt.h */
#ifndef __CKPT_H__
#define __CKPT_H__
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <ucontext.h>
#include "types.h"

/**
 * ckpt_metadata_t: Metadata written to the start of a checkpoint
 *                  file describing the expected contents when
 *                  reading the checkpoint image
 */
typedef struct ckpt_metadata {
        u32             nr_headers;
        u32             nr_regions;
        u32             nr_contexts;
        u32             nr_callframes;
} ckpt_metadata_t;

/** 
 * ckpt_hdr_t:  
 *      Header that written before each saved segment of
 *      data in a checkpoint file, indicating the type of
 *      data following any header.
 */
typedef enum ckpt_header {
        CKPT_VM_REGION_HEADER           = 0u,
        CKPT_CONTEXT_HEADER             = 1u,
        CKPT_CALLFRAME_HEADER           = 2u,
        CKPT_SHARED_CACHE_HEADER        = 3u
} ckpt_header_t;

#define CKPT_HEADER_STRING(__header) \
        (((__header) == CKPT_VM_REGION_HEADER)    ? "vm region"    : \
         ((__header) == CKPT_CONTEXT_HEADER)      ? "context"      : \
         ((__header) == CKPT_CALLFRAME_HEADER)    ? "callframe"    : \
         ((__header) == CKPT_SHARED_CACHE_HEADER) ? "shared cache" : \
         "unrecognized")


#define MAX_CKPT_HEADERS        (512)
#define MAX_CKPT_VM_REGIONS     (128)
#define MAX_CKPT_CONTEXTS       (1) // only single-threaded for now
#define MAX_CKPT_CALLFRAMES     (64)

/* Checkpoint handler to run on SIGUSR2 */
void ckpt_handler(int, siginfo_t *, void *);

/* Constructor to setup signal handler */
__attribute__((constructor))
void setup(void);

__attribute__((destructor))
void cleanup(void);

#endif // __CKPT_H__
