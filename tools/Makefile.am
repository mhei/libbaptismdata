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
