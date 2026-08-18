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
# Copyright 2026 Oxide Computer Company
#

include ../Makefile.tools

#
# libcompat has two halves, and they are shaped after two different existing
# things in this tree, because each half already has a precedent here:
#
#  * The header half follows tools/sgs/include/Makefile: a headers-only
#    directory in tools/ whose `all install' target gathers a hand-picked list
#    of "ELF-y" headers out of the workspace with $(INS.file), into a directory
#    the build then searches with -I.  As there, the point is to avoid putting
#    the whole illumos header tree on the include path, where <sys/types.h> and
#    friends collide head-on with the host libc's.
#
#  * The library half follows tools/ctf/libctf/Makefile.com: build an object
#    and install it under $(ROOTONBLDLIBMACH), so that consumers link it rather
#    than each compiling the source again.  ($(ROOTONBLDLIB) and
#    $(ROOTONBLDLIBMACH) are defined by Makefile.tools.)
#
# The two output directories are deliberately NOT the same, and this is the
# subtle part.  common/ holds the checked-in shims, and include/ holds the
# headers gathered out of the workspace.  They must stay apart because
# common/sys/elf.h is a shim that forwards to the *host's* <elf.h> -- for the
# CTF tools, which link the host libelf -- while the gathered include/sys/elf.h
# is illumos' real one, for the tools that want illumos' ELF types.  A consumer
# picks a side by which directory it -I's first.  Merging them would make that
# choice unexpressible.
#
# That split is also why the .gitignore entry is `libcompat/include' and not
# `libcompat/common': common/ is source, include/ is generated.  Compare
# `sgs/include/sys', ignored for exactly this reason.
#

#
# The gathered header list.  Judgement is required to extend it: prefer letting
# the host libc win, and add a header only when the host genuinely has no
# counterpart, or -- as with <sys/elf.h> -- when the host's counterpart refuses
# to compile at all.
#
# A WARNING worth heeding, because it has already cost one backed-out change:
# these headers have dependencies on each other that are not visible in the
# list.  Staging <sys/auxv.h> without also staging <sys/auxv_386.h> breaks
# elfextract, and nothing about the list says so.  tools/sgs/include/Makefile
# names both together for precisely this reason.  Neither is needed here today;
# if you add one, add the other.
#
SYSHDRS=	sys/elf.h \
		sys/elftypes.h \
		sys/elf_386.h \
		sys/elf_amd64.h \
		sys/elf_SPARC.h \
		sys/elf_notes.h \
		sys/isa_defs.h \
		sys/feature_tests.h \
		sys/ccompile.h \
		sys/note.h \
		sys/multiboot.h \
		sys/multiboot2.h \
		sys/queue.h \
		sys/types32.h \
		sys/containerof.h \
		sys/stddef.h \
		sys/auxv.h \
		sys/auxv_386.h \
		sys/auxv_SPARC.h \
		sys/avl.h \
		sys/link.h \
		sys/machelf.h

#
# <sys/machtypes.h> is per-ISA; everything above is machine-independent.  The
# ISA directory under uts/ is not spelled $(MACH) -- x86 is `intel' there,
# while $(MACH) is `i386' -- so map it.
#
i386_UTSDIR=	intel
sparc_UTSDIR=	sparc
UTSDIR=		$($(MACH)_UTSDIR)

MACHSYSHDRS=	sys/machtypes.h

#
# The ELF API headers, from $(SRC)/head.  These are the other half of what
# tools/sgs/include stages: that directory copies them into itself and the sgs
# makefiles then -I it, which is why it cannot be consumed from outside the
# workspace.  Gathering them here, installed, makes tools/sgs/include
# redundant.
#
# NOTE the auxv trio.  <sys/auxv.h> does not compile without <sys/auxv_386.h>
# and <sys/auxv_SPARC.h> beside it, and nothing in the list says so; staging
# auxv.h alone is a known way to break elfextract.  They move together.
#
HEADHDRS=	dlfcn.h \
		gelf.h \
		libelf.h \
		proc_service.h \
		rtld_db.h \
		link.h

HDRS=		$(SYSHDRS:%=include/%) $(MACHSYSHDRS:%=include/%) \
		$(HEADHDRS:%=include/%)

LIBRARY=	libcompat.a
OBJS=		native_support.o

#
# $(COMPAT_CPPFLAGS) is -I on common/ plus the force-include of
# native_compat.h.  That header reaches <sys/isa_defs.h>, which is one of the
# gathered ones, so include/ has to be searched too -- and the gather has to
# have happened, hence the $(HDRS) dependency on the object below.
#
CPPFLAGS +=	$(COMPAT_CPPFLAGS) -Iinclude

.KEEP_STATE:

all: $(HDRS) $(LIBRARY)

include/sys:
	$(MKDIR) -p include/sys

include/sys/%: include/sys $(SRC)/uts/common/sys/%
	$(RM) $@; $(CP) $(SRC)/uts/common/sys/$(@F) $@; $(CHMOD) 644 $@

include/%: include $(SRC)/head/%
	$(RM) $@; $(CP) $(SRC)/head/$(@F) $@; $(CHMOD) 644 $@

include:
	$(MKDIR) -p include

include/sys/machtypes.h: include/sys $(SRC)/uts/$(UTSDIR)/sys/machtypes.h
	$(RM) $@; $(CP) $(SRC)/uts/$(UTSDIR)/sys/machtypes.h $@; $(CHMOD) 644 $@

#
# <sys/types32.h> is nothing but the int32_t/uint32_t typedefs, but it reaches
# them through illumos' <sys/int_types.h>, which redefines the whole intN_t
# family and so collides with the host's <stdint.h>.  Point it at <stdint.h>;
# the typedefs it defines are unchanged either way.
#
include/sys/types32.h: include/sys $(SRC)/uts/common/sys/types32.h
	$(RM) $@
	$(SED) -e 's|<sys/int_types\.h>|<stdint.h>|' \
	    $(SRC)/uts/common/sys/types32.h > $@
	$(CHMOD) 644 $@

native_support.o: common/native_support.c $(HDRS)
	$(NATIVECC) $(NATIVE_CFLAGS) $(CPPFLAGS) -c -o $@ common/native_support.c

#
# $(AR) is deliberately the *build host's* ar, not the target's: libcompat is
# linked into tools that run on the build machine.  Makefile.master defines
# ARFLAGS; AR itself falls back to make's builtin `ar'.
#
$(LIBRARY): $(OBJS)
	$(RM) $@; $(AR) $(ARFLAGS) $@ $(OBJS)

#
# The headers are outputs too, not just scratch for this directory's own -I.
# They install into two separate trees on purpose, for the reason given at the
# top: include/ is the gathered illumos headers, include-native/ is the shims,
# and include-native/sys/elf.h deliberately shadows include/sys/elf.h for a
# consumer that links the host libelf.  Merging them would make that choice
# unexpressible.
#
ROOTONBLDINC=		$(ROOTONBLD)/include
ROOTONBLDINCNATIVE=	$(ROOTONBLD)/include-native
ROOTONBLDINCHOSTELF=	$(ROOTONBLD)/include-host-elf

install: all $(ROOTONBLDLIBMACH)/$(LIBRARY) install_hdrs

install_hdrs: $(HDRS)
	$(MKDIR) -p $(ROOTONBLDINC)/sys $(ROOTONBLDINCNATIVE)/sys
	$(CP) include/sys/*.h $(ROOTONBLDINC)/sys/
	$(CP) include/*.h $(ROOTONBLDINC)/
	$(CP) common/*.h $(ROOTONBLDINCNATIVE)/
	$(CP) common/sys/*.h $(ROOTONBLDINCNATIVE)/sys/
	$(MKDIR) -p $(ROOTONBLDINCHOSTELF)/sys
	$(CP) host-elf/sys/*.h $(ROOTONBLDINCHOSTELF)/sys/

$(ROOTONBLDLIBMACH):
	$(MKDIR) -p $@

$(ROOTONBLDLIBMACH)/$(LIBRARY): $(ROOTONBLDLIBMACH) $(LIBRARY)
	$(RM) $@; $(CP) $(LIBRARY) $@; $(CHMOD) 644 $@

clean:
	$(RM) $(OBJS) $(LIBRARY)

clobber: clean
	$(RM) -r include

FRC:

include ../Makefile.targ
