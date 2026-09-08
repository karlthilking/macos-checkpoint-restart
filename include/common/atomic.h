/* atomic.h */
#ifndef XND_ATOMIC_H
#define XND_ATOMIC_H

#ifndef __cplusplus /* Not adopted for c++ */

#include <stdatomic.h>
#if __has_include(<os/atomic.h>)
# include <os/atomic.h>
# define USE_OS_ATOMIC 1
#else
# define USE_OS_ATOMIC 0
#endif

#include "compiler.h"

#if USE_OS_ATOMIC
# define xnd_cast_to_atomic_pointer(p) os_cast_to_atomic_pointer(p)
# define xnd_atomic_basetypeof(p) os_atomic_basetypeof(p)
# define xnd_cast_to_nonatomic_pointer(p) os_cast_to_nonatomic_pointer(p)
#else
# define xnd_cast_to_atomic_pointer(p) \
	(_Atomic volatile typeof(*(p)) *)(uintptr_t)(p)
# define xnd_atomic_basetypeof(p) \
	typeof(atomic_load(xnd_cast_to_atomic_pointer(p)))
# define xnd_cast_to_nonatomic_pointer(p) \
	(xnd_atomic_basetypeof(p) *)(uintptr_t)(p)
#endif

static_assert(memory_order_relaxed == __ATOMIC_RELAXED, "");
static_assert(memory_order_acquire == __ATOMIC_ACQUIRE, "");
static_assert(memory_order_release == __ATOMIC_RELEASE, "");
static_assert(memory_order_acq_rel == __ATOMIC_ACQ_REL, "");
static_assert(memory_order_seq_cst == __ATOMIC_SEQ_CST, "");

#define xnd_release_barrier_relaxed memory_order_relaxed
#define xnd_release_barrier_acquire memory_order_relaxed
#define xnd_release_barrier_release memory_order_release
#define xnd_release_barrier_acq_rel memory_order_release
#define xnd_release_barrier_seq_cst memory_order_release

#define xnd_acquire_barrier_relaxed memory_order_relaxed
#define xnd_acquire_barrier_acquire memory_order_acquire
#define xnd_acquire_barrier_release memory_order_relaxed
#define xnd_acquire_barrier_acq_rel memory_order_acquire
#define xnd_acquire_barrier_seq_cst memory_order_acquire

#define xnd_compiler_barrier_before_atomic(m) \
	atomic_signal_fence(xnd_release_barrier_##m)
#define xnd_compiler_barrier_after_atomic(m) \
	atomic_signal_fence(xnd_acquire_barrier_##m)
#define xnd_compiler_barrier(m) \
	atomic_signal_fence(memory_order_##m)

#define xnd_release_fence_relaxed memory_order_relaxed
#define xnd_release_fence_acquire memory_order_relaxed
#define xnd_release_fence_release memory_order_release
#define xnd_release_fence_acq_rel memory_order_release
#define xnd_release_fence_seq_cst memory_order_release

#define xnd_acquire_fence_relaxed memory_order_relaxed
#define xnd_acquire_fence_acquire memory_order_acquire
#define xnd_acquire_fence_release memory_order_relaxed
#define xnd_acquire_fence_acq_rel memory_order_acquire
#define xnd_acquire_fence_seq_cst memory_order_acquire

#define xnd_memory_fence_before_atomic(m) \
	atomic_thread_fence(xnd_release_barrier_##m)
#define xnd_memory_fence_after_atomic(m) \
	atomic_thread_fence(xnd_acquire_barrier_##m)
#define xnd_memory_fence(m) \
	atomic_thread_fence(memory_order_##m)

#define xnd_atomic_value_cast(p, v)		      \
	({					      \
		xnd_atomic_basetypeof(p) __val = (v); \
		__val;				      \
	})

#define xnd_atomic_load(p, m)				  \
	({						  \
		xnd_compiler_barrier_before_atomic(m);	  \
		__auto_type __val = atomic_load_explicit( \
			xnd_cast_to_atomic_pointer(p),	  \
			memory_order_##m);		  \
		xnd_compiler_barrier_after_atomic(m);	  \
		__val;					  \
	})

#define xnd_atomic_store(p, v, m)		       \
	({					       \
		__auto_type __val = (v);	       \
		xnd_compiler_barrier_before_atomic(m); \
		atomic_store_explicit(		       \
			xnd_cast_to_atomic_pointer(p), \
			__val, memory_order_##m);      \
		xnd_compiler_barrier_after_atomic(m);  \
		__val;				       \
	})

#define xnd_atomic_cmpxchg(p, e, v, m, type)			  \
	({							  \
		bool __ok;					  \
		xnd_atomic_basetypeof(p) __exp = (e);		  \
		xnd_compiler_barrier_before_atomic(m);		  \
		__ok = atomic_compare_exchange_##type##_explicit( \
			xnd_cast_to_atomic_pointer(p), &__exp,	  \
			xnd_atomic_value_cast(p, v),		  \
			memory_order_##m, memory_order_relaxed);  \
		xnd_compiler_barrier_after_atomic(m);		  \
		__ok;						  \
	})

#define xnd_atomic_cmpxchg_weak(p, e, v, m) \
	xnd_atomic_cmpxchg(p, e, v, m, weak)
#define xnd_atomic_cmpxchg_strong(p, e, v, m) \
	xnd_atomic_cmpxchg(p, e, v, m, strong)

#define xnd_atomic_c11_fetch_op(o, p, v, m)		      \
	({						      \
		xnd_compiler_barrier_before_atomic(m);	      \
		__auto_type __result = atomic_##o##_explicit( \
			xnd_cast_to_atomic_pointer(p),	      \
			xnd_atomic_value_cast(p, v),	      \
			memory_order_##m);		      \
		xnd_compiler_barrier_after_atomic(m);	      \
		__result;				      \
	})

#define xnd_atomic_c11_op(o, p, v, m, op)			 \
	({							 \
		xnd_atomic_basetypeof(p) __old;			 \
		__auto_type __val = xnd_atomic_value_cast(p, v); \
		__old = xnd_atomic_c11_fetch_op(o, p, v, m);	 \
		__old op __val;					 \
	})

#define xnd_atomic_builtin_fetch_op(o, p, v, m)		  \
	({						  \
		xnd_compiler_barrier_before_atomic(m);	  \
		__auto_type __result = __atomic_##o(	  \
			xnd_cast_to_nonatomic_pointer(p), \
			xnd_atomic_value_cast(p, v),	  \
			memory_order_##m);		  \
		xnd_compiler_barrier_after_atomic(m);	  \
		__result;				  \
	})

#define xnd_atomic_builtin_op(o, p, v, m, op)			 \
	({							 \
		xnd_atomic_basetypeof(p) __old;			 \
		__auto_type __val = xnd_atomic_value_cast(p, v); \
		__old = xnd_atomic_builtin_fetch_op(o, p, v, m); \
		op(__old, __val);				 \
	})

#define xnd_atomic_increment(p, m) \
	xnd_atomic_c11_op(fetch_add, p, 1, m, +)
#define xnd_atomic_fetch_increment(p, m) \
	xnd_atomic_c11_fetch_op(fetch_add, p, 1, m)

#define xnd_atomic_decrement(p, m) \
	xnd_atomic_c11_op(fetch_sub, p, 1, m, -)
#define xnd_atomic_fetch_decrement(p, m) \
	xnd_atomic_c11_fetch_op(fetch_sub, p, 1, m)

#define xnd_atomic_add(p, v, m) \
	xnd_atomic_c11_op(fetch_add, p, v, m, +)
#define xnd_atomic_fetch_add(p, v, m) \
	xnd_atomic_c11_fetch_op(fetch_add, p, v, m)

#define xnd_atomic_sub(p, v, m) \
	xnd_atomic_c11_op(fetch_sub, p, v, m, -)
#define xnd_atomic_fetch_sub(p, v, m) \
	xnd_atomic_c11_fetch_op(fetch_sub, p, v, m)

#define xnd_atomic_xchg(p, v, m) \
	xnd_atomic_c11_fetch_op(exchange, p, v, m)

#define xnd_atomic_and(p, v, m) \
	xnd_atomic_c11_op(fetch_and, p, v, m, &)
#define xnd_atomic_fetch_and(p, v, m) \
	xnd_atomic_c11_fetch_op(fetch_and, p, v, m)

#define xnd_atomic_or(p, v, m) \
	xnd_atomic_c11_op(fetch_or, p, v, m, |)
#define xnd_atomic_fetch_or(p, v, m) \
	xnd_atomic_c11_fetch_op(fetch_or, p, v, m)

#define xnd_atomic_xor(p, v, m) \
	xnd_atomic_c11_op(fetch_xor, p, v, m, ^)
#define xnd_atomic_fetch_xor(p, v, m) \
	xnd_atomic_c11_fetch_op(fetch_xor, p, v, m)

#ifdef max
# define xnd_atomic_fetch_max(p, v, m) \
	xnd_atomic_builtin_fetch_op(fetch_max, p, v, m)
# define xnd_atomic_max(p, v, m) \
	xnd_atomic_builtin_op(fetch_max, p, v, m, max)
#else
# error "max(x, y) is not defined, needed by xnd_atomic_(fetch_)max"
#endif

#ifdef min
# define xnd_atomic_fetch_min(p, v, m) \
	xnd_atomic_builtin_fetch_op(fetch_min, p, v, m)
# define xnd_atomic_min(p, v, m) \
	xnd_atomic_builtin_op(fetch_min, p, v, m, min)
#else
# error "min(x, y) is not defined, needed by xnd_atomic_(fetch_)min"
#endif

#endif /* !__cplusplus */
#endif /* XND_ATOMIC_H */
