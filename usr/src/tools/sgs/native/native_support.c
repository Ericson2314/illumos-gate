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
 * The handful of definitions that a foreign libc, and the objects dropped
 * from the native libconv, would otherwise leave undefined.  Built into
 * libconv, which everything else here links against.
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdarg.h>

/*
 * On illumos this is a .note section plus a string, generated from
 * libconv/common/bld_vernote.ksh by ksh93 and the illumos assembler.  Only
 * the string is ever read (ldmain.c stamps it into .comment, liblddbg prints
 * it), so build it with the C compiler instead.
 */
#ifndef	LINK_VER_STRING
#define	LINK_VER_STRING	"native (illumos)"
#endif
const char link_ver_string[] = LINK_VER_STRING;

/*
 * ASSERT3*()/VERIFY3*() from <sys/debug.h> call this; illumos has it in libc.
 * assfail() itself comes from cmd/sgs/common/assfail.c.
 */
void
assfail3(const char *a, uintmax_t lv, const char *op, uintmax_t rv,
    const char *f, int l)
{
	(void) fprintf(stderr,
	    "assertion failed: %s (0x%jx %s 0x%jx), file: %s, line: %d\n",
	    a, lv, op, rv, f, l);
	abort();
}

/*
 * common/avl calls panic() on an impossible tree state.  In the kernel that
 * is a system panic; here, the same thing an assertion failure does.
 */
void
panic(const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fputc('\n', stderr);
	abort();
}
