#ifndef _ILLUMOS_COMPAT_SYS_TYPES_H_
#define _ILLUMOS_COMPAT_SYS_TYPES_H_

#include_next <sys/types.h>
// TODO should this be here?
#include <stdint.h>

/*
 * POSIX Extensions for Illumos
 */
typedef unsigned char   uchar_t;
typedef unsigned short  ushort_t;
typedef unsigned int    uint_t;
typedef unsigned long   ulong_t;

/*
 * The boolean_t type has had a varied amount of exposure over the years in
 * terms of how its enumeration constants have been exposed. In particular, it
 * originally used the __XOPEN_OR_POSIX macro to determine whether to prefix the
 * B_TRUE and B_FALSE with an underscore. This check never included the
 * question of if we were in a strict ANSI C environment or whether extensions
 * were defined.
 *
 * Compilers such as clang started defaulting to always including an
 * XOPEN_SOURCE declaration on behalf of users, but also noted __EXTENSIONS__.
 * This would lead most software that had used the non-underscore versions to
 * need it. As such, we have adjusted the non-strict XOPEN environment to retain
 * its old behavior so as to minimize namespace pollution; however, we instead
 * include both variants of the definitions in the generally visible version
 * allowing software written in either world to hopefully end up in a good
 * place.
 *
 * This isn't perfect, but should hopefully minimize the pain for folks actually
 * trying to build software.
 */
#if defined(__XOPEN_OR_POSIX) && !defined(__EXTENSIONS__)
typedef enum { _B_FALSE, _B_TRUE } boolean_t;
#else
typedef enum { B_FALSE = 0, B_TRUE = 1, _B_FALSE = 0, _B_TRUE = 1 } boolean_t;
#endif /* __XOPEN_OR_POSIX && !__EXTENSIONS__ */

#endif
