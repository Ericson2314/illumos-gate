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
 * illumos' sysinfo(2) over uname(2), for the native build.
 *
 * Note that illumos' own <sys/systeminfo.h> is deliberately NOT staged into
 * ../include: it only declares sysinfo(), and the host libc has no such
 * function -- `sysinfo' there is an unrelated Linux syscall taking a `struct
 * sysinfo *'.  Defining our own here keeps the two from meeting.
 *
 * ld(1) reaches sysinfo(2) for exactly three things: expanding $MACHINE and
 * $PLATFORM in runpaths, and building the isalist.
 */

#ifndef	_ONBLD_COMPAT_SYS_SYSTEMINFO_H
#define	_ONBLD_COMPAT_SYS_SYSTEMINFO_H

#include <string.h>
#include <sys/utsname.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	SI_SYSNAME		1
#define	SI_HOSTNAME		2
#define	SI_RELEASE		3
#define	SI_VERSION		4
#define	SI_MACHINE		5
#define	SI_ARCHITECTURE		6
#define	SI_HW_SERIAL		7
#define	SI_HW_PROVIDER		8
#define	SI_SRPC_DOMAIN		9
#define	SI_PLATFORM		513
#define	SI_ISALIST		514

static inline long
sysinfo(int command, char *buf, long count)
{
	struct utsname	un;
	const char	*val;
	size_t		len;

	switch (command) {
	case SI_ISALIST:
		/*
		 * The host has no isalist.  illumos' own sysinfo() returns -1
		 * here on machines that predate the interface, and every
		 * caller in the tree copes with that, so say the same.
		 */
		return (-1);
	default:
		break;
	}

	if (uname(&un) < 0)
		return (-1);

	switch (command) {
	case SI_SYSNAME:
		val = un.sysname;
		break;
	case SI_HOSTNAME:
		val = un.nodename;
		break;
	case SI_RELEASE:
		val = un.release;
		break;
	case SI_VERSION:
		val = un.version;
		break;
	case SI_MACHINE:
	case SI_ARCHITECTURE:
	case SI_PLATFORM:
		val = un.machine;
		break;
	default:
		return (-1);
	}

	/* sysinfo(2) returns the size including the terminating null. */
	len = strlen(val) + 1;
	if (buf != NULL && count > 0) {
		size_t n = ((size_t)count < len) ? (size_t)count : len;

		(void) memcpy(buf, val, n);
		buf[n - 1] = '\0';
	}
	return ((long)len);
}

#ifdef	__cplusplus
}
#endif

#endif	/* _ONBLD_COMPAT_SYS_SYSTEMINFO_H */
