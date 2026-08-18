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
 * Copyright 2026 John Ericson
 */

#ifndef	_MKSH_HOSTCOMPAT_H
#define	_MKSH_HOSTCOMPAT_H

/*
 * make(1) is the first thing a build of the gate needs, so on a foreign
 * build host it has to be compiled before anything else in usr/src exists
 * -- including the headers in uts/ and head/.  It is therefore built
 * straight against the host's own libc (see Makefile.bootstrap), and this
 * header supplies the handful of illumos spellings that a non-illumos libc
 * does not have.
 *
 * On illumos this header is empty; the real definitions come from the
 * system headers, as they always have.
 */

#ifndef	__sun

#include <sys/time.h>
#include <sys/param.h>

/*
 * <sys/time.h> gives us `struct timespec'; illumos additionally spells it
 * `timestruc_t', which is the name the file-time code throughout make uses.
 */
typedef struct timespec timestruc_t;

/*
 * <sys/ccompile.h> on illumos.
 */
#ifndef	__NORETURN
#define	__NORETURN	__attribute__((__noreturn__))
#endif

/*
 * <sys/param.h> on illumos.  Used only to size the host and user name
 * buffers, so the exact value does not matter as long as it is generous.
 */
#ifndef	MAXNAMELEN
#define	MAXNAMELEN	256
#endif

/*
 * <sys/loadavg.h> on illumos.  getloadavg(3C) fills the same three slots
 * everywhere; only the index macros are missing.
 */
#ifndef	LOADAVG_1MIN
#define	LOADAVG_1MIN	0
#endif

/*
 * <stdlib.h> on illumos and on the BSDs.  glibc has the same information
 * under a different name.
 */
#ifdef	__GLIBC__
#include <errno.h>
#include <stdlib.h>

static inline const char *
getprogname(void)
{
	return (program_invocation_short_name);
}
#endif

#endif	/* !__sun */

#endif	/* _MKSH_HOSTCOMPAT_H */
