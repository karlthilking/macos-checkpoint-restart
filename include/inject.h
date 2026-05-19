/* inject.h */
#ifndef __CKPT_INJECT_H__
#define __CKPT_INJECT_H__

#define INTERPOSE(__new, __old)                                 \
        __attribute__((used))                                   \
        static const struct {                                   \
                const void *__new;                              \
                const void *__old;                              \
        } __interpose_##__old                                   \
        __attribute__((section("__DATA,__interpose"))) = {      \
                (const void *)&__new,                           \
                (const void *)&__old,                           \
        };

#endif // __CKPT_INJECT_H__
