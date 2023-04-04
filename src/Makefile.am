#
# Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#

AM_CFLAGS = -Wall -Wextra -pedantic

lib_LTLIBRARIES = libbaptismdata.la

libbaptismdataincludedir = $(includedir)/baptismdata

libbaptismdatainclude_HEADERS = \
	baptismdata.h \
	baptismdata-version.h

libbaptismdata_la_SOURCES = \
    baptismdata.c \
    $(libbaptismdatainclude_HEADERS)

libbaptismdata_la_CPPFLAGS = \
	-I $(top_builddir)

libbaptismdata_la_LDFLAGS = \
	$(AM_LDFLAGS) \
	-version-info $(LIBBAPTISMDATA_LT_VERSION_INFO) \
	-no-undefined
	-export-dynamic

libbaptismdata_la_LIBADD = \
	$(LIBUBOOTENV_LIBS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libbaptismdata.pc

DISTCLEANFILES = baptismdata-version.h
EXTRA_DIST = baptismdata-version.h.in
CLEANFILES = *~
