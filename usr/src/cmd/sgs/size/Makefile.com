#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
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
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Copyright 2019 OmniOS Community Edition (OmniOSce) Association.
#

PROG=		size

include		$(SRC)/cmd/Makefile.cmd
include		$(SRC)/cmd/sgs/Makefile.com

COMOBJS=	main.o process.o fcns.o

SRCS=		$(COMOBJS:%.o=../common/%.c)

CERRWARN +=	-_gcc=-Wno-unused-variable

OBJS =		$(COMOBJS)
.PARALLEL:	$(OBJS)

$(NOT_AARCH64_BLD)RPATH =	'-R$$ORIGIN/../../lib/$(MACH64)'
$(AARCH64_BLD)RPATH =	'-R$$ORIGIN/../lib/'
LDFLAGS +=	$(RPATH)

LDLIBS +=	$(CONVLIBDIR64) -lconv $(ELFLIBDIR64) -lelf

CLEANFILES +=	$(OBJS)

# XXXARM: This went missing, and I don't know how
$(ROOTPROG) := FILEMODE = 0555
