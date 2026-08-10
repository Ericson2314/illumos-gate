#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Copyright 2013 Nexenta Systems, Inc.  All rights reserved.
#

#
# uts/common/rpc/Makefile
#
# include global definitions
include ../../../Makefile.master

i386_HDRS=

sparc_HDRS= ib.h

COMMHDRS= \
auth.h		auth_des.h	auth_sys.h	auth_unix.h \
bootparam.h	clnt.h		clnt_soc.h	clnt_stat.h	des_crypt.h \
nettype.h	pmap_clnt.h	pmap_rmt.h \
rpc.h		rpc_com.h	rpc_msg.h \
rpcb_clnt.h	rpcent.h	svc.h		svc_auth.h	svc_soc.h \
types.h		xdr.h		rpcsec_gss.h	svc_mt.h \
rpcsys.h	rpc_rdma.h	rpc_tags.h

HDRS=			\
	$(COMMHDRS)	\
	$($(MACH)_HDRS)

RPC_SRC=	pmap_prot.x	rpcb_prot.x

RPCSVC_SRC=	key_prot.x	rpc_sztypes.x

DERIVED_FILES=	key_prot.h	pmap_prot.h	rpcb_prot.h	rpc_sztypes.h

RPCHDRS=	$(HDRS)	$(RPC_SRC) $(DERIVED_FILES)

RPCSVCHDRS=	$(RPCSVC_SRC)

RPCDIRS=	$(ROOT)/usr/include/rpc
RPCSVCDIRS=	$(ROOT)/usr/include/rpcsvc

ROOTHDRS= $(RPCHDRS:%=$(RPCDIRS)/%) $(RPCSVCHDRS:%=$(RPCSVCDIRS)/%)

$(RPCDIRS)/%: %
	$(INS.file)

$(RPCSVCDIRS)/%: %
	$(INS.file)

# XXX: should really check the style of the derived files as well...
#	$(RPC_SRC:%.x=%.check) \
#	$(RPCSVC_SRC:%.x=%.check)
#
CHECKHDRS= $(HDRS:%.h=%.check)

.KEEP_STATE:

.PARALLEL: $(CHECKHDRS)

all: all_h

install_h: all_h $(RPCDIRS) $(RPCSVCDIRS) $(ROOTHDRS)

# all_h permits derived headers to be built here in the uts source area
# for the kernel to reference, without going so far as to install them.
#
all_h: $(DERIVED_FILES)

clean:
	$(RM) $(DERIVED_FILES)

clobber: clean

$(RPCDIRS):
	$(INS.dir)

$(RPCSVCDIRS):
	$(INS.dir)

key_prot.h: key_prot.x
	$(RPCGEN) -C -h key_prot.x > $@

#
# The trailing declaration blocks rpcgen appends -- the xdr_*() prototypes and
# the client/server stub prototypes -- name the lowercase typedefs it generated
# for the definitions in pmap_prot.x.  Those definitions sit inside the
# `#ifndef _KERNEL` arm of this header (see the %-passthrough at the top of
# pmap_prot.x), while the appended blocks land after the matching #endif, so a
# kernel compilation of <rpc/pmap_prot.h> sees prototypes referring to types
# that were never declared: "unknown type name 'pmap'".
#
# illumos' own rpcgen does not hit this, because it emits each xdr_*()
# prototype immediately after the definition it belongs to -- i.e. inside the
# guard.  A build using another rpcgen (NetBSD's, when cross-building from a
# host with no illumos rpcgen) appends them instead.  Guard them here rather
# than deleting them: this header is installed into /usr/include as well, and
# userland genuinely uses those prototypes.
#
# Only declarations are touched, so the #define PMAPPROG/PMAPVERS/PMAPPROC_*
# block stays visible to the kernel, which uses it (uts/common/rpc/rpc_subr.c,
# uts/common/fs/nfs/nfs_dlinet.c).
pmap_prot.h: pmap_prot.x
	$(RPCGEN) -M -h pmap_prot.x > $@
	$(SED) -i -e '/ndef _KERNEL \*\//,$$ s|^\([a-z].*(.*);\)$$|#ifndef _KERNEL\n\1\n#endif|' $@

rpc_sztypes.h: rpc_sztypes.x
	$(RPCGEN) -C -h rpc_sztypes.x > $@

rpcb_prot.h: rpcb_prot.x
	$(RPCGEN) -M -h rpcb_prot.x > $@

check:	$(CHECKHDRS)
