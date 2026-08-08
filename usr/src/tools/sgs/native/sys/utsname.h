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
 * The host's <sys/utsname.h> plus SYS_NMLN, which is how illumos spells the
 * size of a struct utsname field.
 */

#ifndef	_SGS_NATIVE_SYS_UTSNAME_H
#define	_SGS_NATIVE_SYS_UTSNAME_H

#include_next <sys/utsname.h>

#ifndef	SYS_NMLN
#ifdef	_UTSNAME_LENGTH
#define	SYS_NMLN	_UTSNAME_LENGTH
#else
#define	SYS_NMLN	257
#endif
#endif

#endif	/* _SGS_NATIVE_SYS_UTSNAME_H */
