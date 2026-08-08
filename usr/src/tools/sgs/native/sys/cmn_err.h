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
 * A stand-in for illumos' <sys/cmn_err.h> for the native build.  common/avl
 * includes it solely for panic(); the rest of that header is kernel console
 * plumbing that has no meaning here.
 */

#ifndef	_SGS_NATIVE_SYS_CMN_ERR_H
#define	_SGS_NATIVE_SYS_CMN_ERR_H

#ifdef	__cplusplus
extern "C" {
#endif

extern void panic(const char *, ...) __attribute__((__noreturn__));

#ifdef	__cplusplus
}
#endif

#endif	/* _SGS_NATIVE_SYS_CMN_ERR_H */
