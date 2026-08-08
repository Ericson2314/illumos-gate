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
 * Copyright 2025 the illumos project.
 */

/*
 * A <widec.h> stand-in for building the build-host tools (lex, yacc) on a
 * system that does not have one.
 *
 * illumos' <widec.h> is <wchar.h> plus the EUC codeset machinery and the
 * pre-standard "wide string" functions -- ws*() rather than wcs*().  lex and
 * yacc use only the latter, and only five of them, so map those onto their
 * ISO C equivalents and leave everything else to the host's <wchar.h>.
 *
 * On illumos itself this header must not shadow the real one. It is reached
 * through -I$(SGSDIR)/include, so defer to whatever <widec.h> comes later on
 * the include path if there is one. Testing for that directly, rather than
 * for __sun, is deliberate: Makefile.master passes -D__sun to the *native*
 * compiles too.
 */

#ifndef	_SGS_NATIVE_WIDEC_H
#define	_SGS_NATIVE_WIDEC_H

#if defined(__has_include_next) && __has_include_next(<widec.h>)

#include_next <widec.h>

#else

#include <ctype.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define	wslen(s)		wcslen(s)
#define	wscpy(d, s)		wcscpy(d, s)
#define	wscmp(a, b)		wcscmp(a, b)

/*
 * wsprintf(3) takes a *narrow* format string and writes wide characters,
 * so swprintf() is not a drop-in.  Both callers in yacc format a short
 * fixed-shape string, so render it narrow and widen the result.
 */
#define	wsprintf(buf, ...)						\
	({								\
		char _wsbuf[128];					\
		int _wsn = snprintf(_wsbuf, sizeof (_wsbuf), __VA_ARGS__); \
		(void) mbstowcs((buf), _wsbuf, sizeof (_wsbuf));	\
		_wsn;							\
	})

/*
 * watoi(3) is atoi(3) over a wide string.  lex uses it for the numeric
 * argument of a %-directive, which is always plain ASCII digits.
 */
#define	watoi(s)		((int)wcstol((s), NULL, 10))

/*
 * wcsetno(3) returns which of the EUC codesets a wide character belongs to:
 * 0 for the single-byte (ASCII) set, 1-3 for the supplementary ones. lex uses
 * it for one diagnostic -- rejecting a [a-z] range whose endpoints come from
 * different codesets. Off illumos there is no EUC partitioning to consult, so
 * distinguish only "ASCII" from "not", which gives the identical answer for
 * every range that stays inside ASCII and still catches one that does not.
 */
#define	wcsetno(c)		(((c) < 0x80) ? 0 : 1)

#endif	/* __sun */

#endif	/* _SGS_NATIVE_WIDEC_H */
