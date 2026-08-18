#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2018 Joyent, Inc.
#

include $(SRC)/lib/libctf/Makefile.shared.com
include ../../Makefile.ctf

CSTD = $(CSTD_GNU99)

CPPFLAGS +=	-I$(SRC)/lib/libctf/common/ \
		-I$(SRC)/lib/libdwarf/common/ \
		-I$(SRC)/lib/mergeq \
		-include ../../common/ctf_headers.h \
		-DCTF_OLD_VERSIONS \
		-DCTF_TOOLS_BUILD
#
# -lavl is illumos'.  Only ctf_dwarf.c uses the AVL interfaces, and the
# implementation is plain C in $(SRC)/common/avl, so build it in rather than
# ask a foreign host for a library it does not have.
#
#
# ...and, since this library is what ctfconvert and ctfmerge link against, the
# stand-ins for the libc entry points a foreign host does not have.  Nothing
# scopes libctf's symbols here (MAPFILES is empty), so the programs pick these
# up from it too.
#
OBJECTS +=	native_support.o

OBJECTS +=	avl.o
LDLIBS += -lc -lelf -L$(ROOTONBLDLIBMACH) -ldwarf
NATIVE_LIBS += libelf.so libc.so

pics/native_support.o:	$(COMPAT_SUPPORT_SRC)
	$(COMPILE.c) $(C_PICFLAGS) -o $@ $(COMPAT_SUPPORT_SRC)
	$(POST_PROCESS_O)

pics/avl.o:	$(SRC)/common/avl/avl.c
	$(COMPILE.c) $(C_PICFLAGS) -o $@ $(SRC)/common/avl/avl.c
	$(POST_PROCESS_O)

# As a bootstrapping issue, we can't use the real mapfile because we build
# early in tools and thus don't have support for assertions.
MAPFILES=

.KEEP_STATE:

all: $(LIBS)

install: all $(ROOTONBLDLIBMACH)/libctf.so.1 $(ROOTONBLDLIBMACH)/libctf.so

$(ROOTONBLDLIBMACH)/%: %
	$(INS.file)

$(ROOTONBLDLIBMACH)/$(LIBLINKS): $(ROOTONBLDLIBMACH)/$(LIBLINKS)$(VERS)
	$(INS.liblink)

#
# Just like with libdwarf, we can't actually add ctf to ourselves,
# because we're part of the tools for creating CTF.
#
$(DYNLIB) := CTFMERGE_POST= :
CTFCONVERT_O= :

#
# The `-R' that used to be appended to DYNFLAGS here is restated as GNU ld's
# -rpath by Makefile.ctf.native, which also empties the illumos-ld-only macros
# Makefile.lib feeds into DYNFLAGS.  It must come after every include that
# pulls in Makefile.master, which would otherwise undo it.
#
include ../../Makefile.ctf.native

include $(SRC)/lib/Makefile.targ
include $(SRC)/lib/libctf/Makefile.shared.targ
