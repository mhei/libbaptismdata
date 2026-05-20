# SPDX-License-Identifier: GPL-3-or-later
#
# Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
#

AM_CFLAGS = -I$(top_srcdir)/src -I$(top_builddir)
AM_CFLAGS += -Wall -Wextra -pedantic

sbin_PROGRAMS = bd

bd_SOURCES = bd.c
bd_LDADD = $(top_builddir)/src/libbaptismdata.la
bd_LDFLAGS = $(LIBUBOOTENV_LDFLAGS)

if WITH_AVAHI_MDNS
sbin_PROGRAMS += bd-mdns

bd_mdns_SOURCES = bd-mdns.c
bd_mdns_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir) $(AVAHI_CFLAGS)
bd_mdns_LDADD = $(top_builddir)/src/libbaptismdata.la
bd_mdns_LDADD += $(AVAHI_LIBS)
bd_mdns_LDFLAGS = $(LIBUBOOTENV_LDFLAGS)
endif
