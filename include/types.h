/* types.h */
#ifndef __TYPES_H__
#define __TYPES_H__
#include <stdint.h>

typedef int8_t          i8;
typedef uint8_t         u8;
typedef int16_t         i16;
typedef uint16_t        u16;
typedef int32_t         i32;
typedef uint32_t        u32;
typedef int64_t         i64;
typedef uint64_t        u64;

typedef enum    ckpt_header     ckpt_header_t;
typedef struct  ckpt_metadata   ckpt_metadata_t;
typedef struct  ckpt_vm_region  ckpt_vm_region_t;
typedef struct  ckpt_context    ckpt_context_t;
typedef struct  ckpt_callframe  ckpt_callframe_t;

#endif // __TYPE_H__
