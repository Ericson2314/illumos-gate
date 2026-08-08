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
 * Compatibility shims for building the native link-editor on a host whose
 * libc is not illumos'.  Force-included (via -include) from
 * tools/sgs/Makefile.com, so it must be safe to include first, before
 * anything else.
 *
 * The headers staged by tools/sgs/include are illumos' own, and they expect
 * illumos' <sys/types.h> to have already supplied the "_t" spellings of the
 * base integer types and boolean_t.  A foreign libc supplies none of that,
 * so define it here.  Nothing in here is illumos-specific behaviour; it is
 * purely the vocabulary the staged headers assume.
 */

#ifndef	_SGS_NATIVE_COMPAT_H
#define	_SGS_NATIVE_COMPAT_H


/*
 * On illumos every sys header reaches <sys/isa_defs.h> by way of
 * <sys/param.h>.  Here <sys/param.h> is the host's, so the staged illumos
 * headers would never see _BIT_FIELDS_LTOH, _LP64 and friends.  Pull it in
 * up front instead.
 */
#include <sys/isa_defs.h>

/*
 * <sys/ccompile.h> supplies __GNU_INLINE and the __sun_attr__ family that the
 * staged headers use; on illumos it too arrives via <sys/types.h>.
 */
#include <sys/ccompile.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
/*
 * illumos' <strings.h> pulls in <string.h>; the host's does not, and the sgs
 * sources rely on the illumos behaviour.
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
#ifndef	P2PHASE
#define	P2PHASE(x, align)	((x) & ((align) - 1))
#endif
#ifndef	P2ALIGN
#define	P2ALIGN(x, align)	((x) & -(align))
#endif
#ifndef	P2NPHASE
#define	P2NPHASE(x, align)	(-(x) & ((align) - 1))
#endif
#ifndef	IS_P2ALIGNED
#define	IS_P2ALIGNED(v, a)	((((uintptr_t)(v)) & ((uintptr_t)(a) - 1)) == 0)
#endif

/*
 * auxv_t, which liblddbg's interfaces take by pointer.  On illumos it
 * arrives through the <sys/procfs.h> chain; here that chain is the host's.
 * This resolves to the staged illumos header, not the host's <sys/auxv.h>,
 * and must come after the typedefs above.
 */
#include <sys/auxv.h>

/*
 * illumos' <sys/int_types.h> announces the availability of 64-bit integer
 * types this way, and <gelf.h> refuses to compile without it.  We are not
 * staging <sys/int_types.h> -- it redefines the whole intN_t family and
 * collides with the host's <stdint.h> -- so state the fact directly.
 */
#ifndef	_INT64_TYPE
#define	_INT64_TYPE
#endif
#ifndef	_LONGLONG_TYPE
#define	_LONGLONG_TYPE
#endif

/*
 * illumos' <sys/param.h> exposes the runtime page size as PAGESIZE.
 */
#ifndef	PAGESIZE
#include <unistd.h>
#define	PAGESIZE	((size_t)sysconf(_SC_PAGESIZE))
#endif

/*
 * From illumos' <sys/time.h>.
 */
#ifndef	SEC
#define	SEC		1
#endif
#ifndef	MILLISEC
#define	MILLISEC	1000
#endif
#ifndef	MICROSEC
#define	MICROSEC	1000000
#endif
#ifndef	NANOSEC
#define	NANOSEC		1000000000
#endif

/*
 * From illumos' <limits.h>; the host's has no notion of message catalogues.
 */
#ifndef	NL_MSGMAX
#define	NL_MSGMAX	32767
#endif
#ifndef	NL_SETMAX
#define	NL_SETMAX	255
#endif
#ifndef	NL_TEXTMAX
#define	NL_TEXTMAX	2048
#endif

#ifdef	__cplusplus
}
#endif

#endif	/* _SGS_NATIVE_COMPAT_H */
