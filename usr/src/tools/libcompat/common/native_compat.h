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
 * Compatibility shims for building gate source on a host whose libc is not
 * illumos.  Force-included (via -include) by every consumer, so it must be
 * safe to include first, before anything else.
 *
 * This is the merge of what used to be tools/ctf/native/native_compat.h and
 * tools/sgs/native/native_compat.h.  Those were two spellings of one job --
 * "gate source, foreign libc" -- kept apart only because each grew next to
 * the tool that first needed it.  Neither is specific to the CTF tools or to
 * the link-editor, and a third consumer would have had to pick one or copy a
 * third.
 *
 * The gate headers a foreign host reaches expect illumos' <sys/types.h> to
 * have already supplied the "_t" spellings of the base integer types and
 * boolean_t; a foreign libc supplies none of that, so define it here.
 * Nothing in here is illumos-specific behaviour.  It is purely the vocabulary
 * the gate headers assume.
 */

#ifndef	_ONBLD_COMPAT_H
#define	_ONBLD_COMPAT_H

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


/*
 * illumos' <sys/sysmacros.h> carries ARRAY_SIZE, but that header is
 * kernel-flavoured and the rest of it collides with the host's.
 * common/ctf/ctf_types.c reaches it through <sys/debug.h> on illumos and
 * finds nothing here.
 */
#ifndef	ARRAY_SIZE
#define	ARRAY_SIZE(x)	(sizeof (x) / sizeof (x[0]))
#endif

/*
 * From illumos' <sys/cdefs.h>.  Used by common/lz4, which vtfontcvt links in.
 */
#ifndef	__DECONST
#define	__DECONST(type, var)	((type)(uintptr_t)(const void *)(var))
#endif

/*
 * strtonum(3C) is illumos libc, not the host's; native_support.c below
 * supplies the implementation and this is its declaration.  Not reached
 * through the gate's <stdlib.h>, which a foreign host does not see.
 */
extern long long strtonum(const char *, long long, long long, const char **);
#ifdef	__cplusplus
}
#endif

#endif	/* _ONBLD_COMPAT_H */
