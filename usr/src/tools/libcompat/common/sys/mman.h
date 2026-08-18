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
 * The host's <sys/mman.h> plus the mmapobj(2) vocabulary from illumos'.
 * rtld.h and liblddbg describe mappings in terms of mmapobj_result_t; the
 * link-editor never calls mmapobj(2) itself, it only needs the shape of the
 * results the runtime linker reports.
 */

#ifndef	_ONBLD_COMPAT_SYS_MMAN_H
#define	_ONBLD_COMPAT_SYS_MMAN_H

#include_next <sys/mman.h>

#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	MMOBJ_PADDING		0x10000
#define	MMOBJ_INTERPRET		0x20000
#define	MMOBJ_ALL_FLAGS		(MMOBJ_PADDING | MMOBJ_INTERPRET)

#define	MR_PADDING	0x1
#define	MR_HDR_ELF	0x2
#define	MR_HDR_AOUT	0x3

#define	MR_TYPE_MASK	0x0000ffff
#define	MR_GET_TYPE(val)	((val) & MR_TYPE_MASK)

typedef struct mmapobj_result {
	caddr_t		mr_addr;	/* mapping address */
	size_t		mr_msize;	/* mapping size */
	size_t		mr_fsize;	/* file size */
	size_t		mr_offset;	/* offset into file */
	uint_t		mr_prot;	/* the protections provided */
	uint_t		mr_flags;	/* info on the mapping */
} mmapobj_result_t;

#ifdef	__cplusplus
}
#endif

#endif	/* _ONBLD_COMPAT_SYS_MMAN_H */
