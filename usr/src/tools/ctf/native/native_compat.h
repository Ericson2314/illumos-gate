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
 * Compatibility shims for building the CTF tools on a host whose libc is not
 * illumos'.  Force-included (via -include) from tools/ctf/Makefile.ctf.native,
 * so it must be safe to include before anything else.
 *
 * The CTF sources reach for illumos headers that illumos does not ship --
 * <sys/ctf.h>, <sys/ctf_api.h>, <sys/list.h>, <sys/avl.h> -- which
 * Makefile.ctf.native supplies with an -idirafter on $(SRC)/uts/common.  Those
 * headers expect illumos' <sys/types.h> to have already supplied the "_t"
 * spellings of the base integer types; a foreign libc supplies none of it.
 *
 * Nothing here is illumos-specific behaviour.  It is purely the vocabulary the
 * illumos headers assume.  Compare tools/sgs/native/native_compat.h, which
 * does the same job for the native link-editor.
 */

#ifndef	_CTF_NATIVE_COMPAT_H
#define	_CTF_NATIVE_COMPAT_H

/*
 * <sys/ctf_api.h> uses off64_t, which glibc only declares under this feature
 * test macro.  It has to be defined before any libc header is included, which
 * is why this file is force-included first.
 */
#ifndef	_LARGEFILE64_SOURCE
#define	_LARGEFILE64_SOURCE
#endif

/*
 * On illumos every sys header reaches <sys/isa_defs.h> by way of
 * <sys/param.h>.  Here <sys/param.h> is the host's, so the illumos headers
 * would never see _BIT_FIELDS_LTOH, _LP64 and friends.  Pull it in up front.
 * <sys/ccompile.h> likewise supplies __GNU_INLINE and the __sun_attr__ family.
 */
#include <sys/isa_defs.h>
#include <sys/ccompile.h>

#include <stddef.h>
/*
 * illumos' <sys/types.h> declares the <stdint.h> types; glibc's does not.
 * libdwarf (dwarf_frame2.c, among others) includes only <sys/types.h> and then
 * uses uintptr_t.
 */
#include <stdint.h>
#include <sys/types.h>
/*
 * illumos' <strings.h> pulls in <string.h>; the host's does not.
 */
#include <string.h>
#include <strings.h>

#ifdef	__cplusplus
extern "C" {
#endif

#ifndef	_BOOLEAN_T
#define	_BOOLEAN_T
typedef enum { B_FALSE, B_TRUE } boolean_t;
#endif

typedef unsigned char		uchar_t;
typedef unsigned short		ushort_t;
typedef unsigned int		uint_t;
typedef unsigned long		ulong_t;
typedef long long		longlong_t;
typedef unsigned long long	u_longlong_t;
typedef long long		hrtime_t;

/*
 * illumos' <sys/sysmacros.h> carries these; the host's has only the
 * major/minor/makedev trio, so a bare #include silently loses them.
 */
#ifndef	MAX
#define	MAX(a, b)	((a) < (b) ? (b) : (a))
#endif
#ifndef	MIN
#define	MIN(a, b)	((a) > (b) ? (b) : (a))
#endif
#ifndef	ABS
#define	ABS(a)		((a) < 0 ? -(a) : (a))
#endif
#ifndef	howmany
#define	howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif
#ifndef	roundup
#define	roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef	P2ROUNDUP
#define	P2ROUNDUP(x, align)	(-(-(x) & -(align)))
#endif
#ifndef	IS_P2ALIGNED
#define	IS_P2ALIGNED(v, a)	((((uintptr_t)(v)) & ((uintptr_t)(a) - 1)) == 0)
#endif

/*
 * illumos' <sys/int_types.h> announces the availability of 64-bit integer
 * types this way.  We are not using illumos' <sys/int_types.h> -- it redefines
 * the whole intN_t family and collides with the host's <stdint.h> -- so state
 * the fact directly.
 */
#ifndef	_INT64_TYPE
#define	_INT64_TYPE
#endif
#ifndef	_LONGLONG_TYPE
#define	_LONGLONG_TYPE
#endif

#ifdef	__cplusplus
}
#endif

#endif	/* _CTF_NATIVE_COMPAT_H */
