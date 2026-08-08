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
 * illumos' <synch.h> over POSIX threads, for the native build.  See the
 * comment in thread.h for why this is a shim rather than the real header.
 */

#ifndef	_SGS_NATIVE_SYNCH_H
#define	_SGS_NATIVE_SYNCH_H

#include <pthread.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef pthread_mutex_t		mutex_t;
typedef pthread_rwlock_t	rwlock_t;
typedef pthread_cond_t		cond_t;

#define	DEFAULTMUTEX	PTHREAD_MUTEX_INITIALIZER
#define	DEFAULTRWLOCK	PTHREAD_RWLOCK_INITIALIZER
#define	DEFAULTCV	PTHREAD_COND_INITIALIZER

#define	USYNC_THREAD	0
#define	USYNC_PROCESS	1

static inline int
mutex_init(mutex_t *mp, int type __attribute__((__unused__)),
    void *arg __attribute__((__unused__)))
{
	return (pthread_mutex_init(mp, NULL));
}

static inline int
mutex_destroy(mutex_t *mp)
{
	return (pthread_mutex_destroy(mp));
}

static inline int
mutex_lock(mutex_t *mp)
{
	return (pthread_mutex_lock(mp));
}

static inline int
mutex_trylock(mutex_t *mp)
{
	return (pthread_mutex_trylock(mp));
}

static inline int
mutex_unlock(mutex_t *mp)
{
	return (pthread_mutex_unlock(mp));
}

static inline int
rwlock_init(rwlock_t *rw, int type __attribute__((__unused__)),
    void *arg __attribute__((__unused__)))
{
	return (pthread_rwlock_init(rw, NULL));
}

static inline int
rwlock_destroy(rwlock_t *rw)
{
	return (pthread_rwlock_destroy(rw));
}

static inline int
rw_rdlock(rwlock_t *rw)
{
	return (pthread_rwlock_rdlock(rw));
}

static inline int
rw_wrlock(rwlock_t *rw)
{
	return (pthread_rwlock_wrlock(rw));
}

static inline int
rw_tryrdlock(rwlock_t *rw)
{
	return (pthread_rwlock_tryrdlock(rw));
}

static inline int
rw_trywrlock(rwlock_t *rw)
{
	return (pthread_rwlock_trywrlock(rw));
}

static inline int
rw_unlock(rwlock_t *rw)
{
	return (pthread_rwlock_unlock(rw));
}

/*
 * libelf asserts that it holds a lock before touching shared state.  POSIX
 * exposes no way to ask whether the calling thread holds a pthread_rwlock_t
 * or a default pthread_mutex_t, so these can only answer "yes" -- the
 * assertions become vacuous rather than wrong.  ld is single threaded, so
 * nothing is lost.
 */
#define	MUTEX_HELD(mp)		(1)
#define	RW_LOCK_HELD(rw)	(1)
#define	RW_READ_HELD(rw)	(1)
#define	RW_WRITE_HELD(rw)	(1)

#ifdef	__cplusplus
}
#endif

#endif	/* _SGS_NATIVE_SYNCH_H */
