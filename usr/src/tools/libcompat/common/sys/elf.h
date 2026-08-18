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
 * On illumos, <sys/elf.h> is where the ELF type and constant definitions live,
 * and $(SRC)/common/ctf/ctf_impl.h includes it by that name.  glibc puts them
 * in <elf.h> and ships a <sys/elf.h> whose entire content is
 *
 *	#error This header is unsupported on x86-64.
 *
 * Because a -I is searched ahead of the host's own directories and the
 * -idirafter on $(SRC)/uts/common is searched behind them, this shim is what
 * <sys/elf.h> resolves to for the CTF tools -- and illumos' own <sys/elf.h>
 * is deliberately not used, since it would drag in the illumos <sys/types.h>
 * chain that collides head-on with the host libc.
 */

#ifndef	_ONBLD_COMPAT_SYS_ELF_H
#define	_ONBLD_COMPAT_SYS_ELF_H

#include <elf.h>

#endif	/* _ONBLD_COMPAT_SYS_ELF_H */
