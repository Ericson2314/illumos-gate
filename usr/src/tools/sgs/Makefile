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

# Copyright 2019, Richard Lowe.

SUBDIRS =		\
	include		\
	ld		\
	libconv		\
	libelf		\
	libld		\
	liblddbg	\
	sgsmsg

all :=		TARGET= install
install :=	TARGET= install
clean :=	TARGET= clean
clobber :=	TARGET= clobber

sgsmsg: include
ld: sgsmsg include libconv libelf libld
libconv: sgsmsg include
libelf: sgsmsg include
libld: sgsmsg libelf libconv liblddbg include
liblddbg: sgsmsg libconv include

all install clean clobber: $(SUBDIRS)

#
# ROOTONBLD (set by Makefile.tools) and ONBLD_TOOLS (set by Makefile.master)
# both have defaults, so values given on this make's command line do not reach
# the sub-makes.  Forward them explicitly when set, so that this tree can be
# built and installed somewhere other than the in-tree proto area.  ONBLD_TOOLS
# matters because libconv and friends run the sgsmsg built here, and find it
# under $(ONBLD_TOOLS)/bin/$(MACH).
#
$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET) $(ROOTONBLD:%=ROOTONBLD=%) $(ONBLD_TOOLS:%=ONBLD_TOOLS=%)

FRC:
