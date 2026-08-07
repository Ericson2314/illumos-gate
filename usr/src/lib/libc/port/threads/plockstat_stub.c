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
 * Stub definitions for the plockstat USDT probes.
 *
 * Normally these symbols come from plockstat.o, which `dtrace -G` generates
 * from port/threads/plockstat.d; dtrace patches the call sites into nops, so
 * the probes cost nothing when not enabled. dtrace is not available when
 * cross-building libc from a non-illumos host, so define the probe functions
 * as empty instead. The result is a libc that behaves identically except that
 * the plockstat provider reports nothing.
 *
 * The parameter lists are deliberately unspecified: the call sites pass
 * varying numbers of arguments, and an empty function is a valid target for
 * all of them under the SysV AMD64 calling convention.
 */

void __dtrace_plockstat___mutex__acquire() {}
void __dtrace_plockstat___mutex__block() {}
void __dtrace_plockstat___mutex__blocked() {}
void __dtrace_plockstat___mutex__error() {}
void __dtrace_plockstat___mutex__release() {}
void __dtrace_plockstat___mutex__spin() {}
void __dtrace_plockstat___mutex__spun() {}
void __dtrace_plockstat___rw__acquire() {}
void __dtrace_plockstat___rw__block() {}
void __dtrace_plockstat___rw__blocked() {}
void __dtrace_plockstat___rw__error() {}
void __dtrace_plockstat___rw__release() {}
