/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * illumos' <thread.h> over POSIX threads, for the native build.
 *
 * Staging illumos' own <thread.h> is not an option: it reaches <synch.h>,
 * <sys/machlock.h>, <sys/time_impl.h> and <sys/int_types.h>, i.e. the whole
 * illumos type system, which then collides head-on with the host libc's.
 * The link-editor's actual use of the Solaris threads API is tiny -- libelf
 * locking and one thread-specific error buffer -- and every piece of it maps
 * onto pthreads directly.
 */

#ifndef	_ONBLD_COMPAT_THREAD_H
#define	_ONBLD_COMPAT_THREAD_H

#include <pthread.h>
#include <synch.h>
#include <errno.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef pthread_t	thread_t;
typedef pthread_key_t	thread_key_t;

/*
 * illumos spells "this key has not been created yet" as THR_ONCE_KEY, a
 * reserved key value of 0.  pthread_key_create() hands out 0 as an ordinary
 * key, so pick a sentinel it will never return instead.
 */
#define	THR_ONCE_KEY	((pthread_key_t)-1)

#define	THR_BOUND	0x00000001
#define	THR_DETACHED	0x00000040

static inline int
thr_keycreate(thread_key_t *keyp, void (*destructor)(void *))
{
	return (pthread_key_create(keyp, destructor));
}

/*
 * illumos' thr_keycreate_once() creates the key exactly once across all
 * threads, keyed on *keyp still holding THR_ONCE_KEY.  With ld being single
 * threaded, "has it been created yet" is an ordinary test.
 */
static inline int
thr_keycreate_once(thread_key_t *keyp, void (*destructor)(void *))
{
	if (*keyp == (thread_key_t)THR_ONCE_KEY)
		return (pthread_key_create(keyp, destructor));
	return (0);
}

static inline int
thr_setspecific(thread_key_t key, void *value)
{
	return (pthread_setspecific(key, value));
}

static inline void *
thr_getspecific(thread_key_t key)
{
	return (pthread_getspecific(key));
}

/*
 * illumos' thr_create() takes a stack address and size up front and returns
 * the new thread's id through its last argument.  workq.c passes NULL/0 for
 * the stack and 0 for the flags, which is "give me the defaults" in both
 * spellings, so the shim only has to handle that case honestly: a non-default
 * request is refused rather than silently ignored.
 */
static inline int
thr_create(void *stack_base, size_t stack_size, void *(*start)(void *),
    void *arg, long flags, thread_t *new_thread)
{
	if (stack_base != NULL || stack_size != 0 || flags != 0)
		return (EINVAL);

	return (pthread_create(new_thread, NULL, start, arg));
}

/*
 * illumos' thr_join() can wait for "any thread" when passed 0, and reports
 * which one it reaped through its second argument.  workq.c always names a
 * thread and does not ask, which is exactly pthread_join().
 */
static inline int
thr_join(thread_t wait_for, thread_t *departed, void **status)
{
	if (wait_for == 0)
		return (EINVAL);
	if (departed != NULL)
		*departed = wait_for;

	return (pthread_join(wait_for, status));
}

static inline thread_t
thr_self(void)
{
	return (pthread_self());
}

/*
 * illumos' thr_main() reports whether the caller is the initial thread, and
 * returns -1 when libthread is not linked in at all -- which is what callers
 * treat as "single threaded, no locking needed".  ld is single threaded, so
 * that is always the honest answer here.
 */
static inline int
thr_main(void)
{
	return (-1);
}

#ifdef	__cplusplus
}
#endif

#endif	/* _ONBLD_COMPAT_THREAD_H */
