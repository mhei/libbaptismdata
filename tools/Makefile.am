SUBDIRS =

if WITH_BDD
SUBDIRS += bdd
endif

DIST_SUBDIRS = bdd

# SPDX-License-Identifier: GPL-3-or-later
#
# Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
#

AM_CFLAGS = -I$(top_srcdir)/src -I$(top_builddir)
AM_CFLAGS += -Wall -Wextra -pedantic

sbin_PROGRAMS = bd bd-is-baptized

bd_SOURCES = bd.c
bd_LDADD = $(top_builddir)/src/libbaptismdata.la
bd_LDFLAGS = $(LIBUBOOTENV_LDFLAGS)

bd_is_baptized_SOURCES = bd-is-baptized.c
bd_is_baptized_LDADD = $(top_builddir)/src/libbaptismdata.la
bd_is_baptized_LDFLAGS = $(LIBUBOOTENV_LDFLAGS)

if WITH_AVAHI_MDNS
sbin_PROGRAMS += bd-mdns

bd_mdns_SOURCES = bd-mdns.c
bd_mdns_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir) $(AVAHI_CFLAGS)
bd_mdns_LDADD = $(top_builddir)/src/libbaptismdata.la
bd_mdns_LDADD += $(AVAHI_LIBS)
bd_mdns_LDFLAGS = $(LIBUBOOTENV_LDFLAGS)
endif

EXTRA_DIST = lighttpd-bd-upnp.conf.in network.yaml

if WITH_UPNP
sbin_PROGRAMS += bd-upnp

bd_upnp_SOURCES = bd-upnp.c
bd_upnp_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir) $(UPNP_CFLAGS)
bd_upnp_CPPFLAGS += -DBD_UPNP_HTTP_ROOT='"$(datadir)/bd-upnp"'
bd_upnp_CPPFLAGS += -DBD_UPNP_STATE_DIR='"$(localstatedir)/lib/bd-upnp"'
bd_upnp_LDADD = $(top_builddir)/src/libbaptismdata.la $(UPNP_LIBS)
bd_upnp_LDFLAGS = $(LIBUBOOTENV_LDFLAGS)

upnpexamplesdir = $(docdir)/examples
nodist_upnpexamples_DATA = lighttpd-bd-upnp.conf

lighttpd-bd-upnp.conf: $(srcdir)/lighttpd-bd-upnp.conf.in Makefile
	$(AM_V_GEN)$(SED) -e 's|@HTTP_ROOT@|$(datadir)/bd-upnp|g' $< > $@

CLEANFILES = lighttpd-bd-upnp.conf

install-data-hook:
	$(MKDIR_P) "$(DESTDIR)$(datadir)/bd-upnp"
if WITH_LIGHTTPD_CFG
	$(MKDIR_P) "$$(dirname "$(DESTDIR)$(LIGHTTPD_CFG)")"
	$(INSTALL_DATA) lighttpd-bd-upnp.conf "$(DESTDIR)$(LIGHTTPD_CFG)"

uninstall-hook:
	rm -f "$(DESTDIR)$(LIGHTTPD_CFG)"
endif
endif

if WITH_UPNP_MANAGER
sbin_PROGRAMS += bd-upnp-manager
bd_upnp_manager_SOURCES = bd-upnp-manager.c bd-upnp-manager-policy.c bd-upnp-manager-policy.h
bd_upnp_manager_CPPFLAGS = -I$(top_builddir) $(UPNP_MANAGER_CFLAGS)
bd_upnp_manager_CPPFLAGS += -DBD_UPNP_MANAGER_CONFIG='"$(datadir)/bd-upnp-manager/network.yaml"'
bd_upnp_manager_LDADD = $(UPNP_MANAGER_LIBS)

upnpmanagerdir = $(datadir)/bd-upnp-manager
dist_upnpmanager_DATA = network.yaml

check_PROGRAMS = bd-upnp-manager-policy-test bd-upnp-manager-state-test
TESTS = $(check_PROGRAMS)
bd_upnp_manager_policy_test_SOURCES = bd-upnp-manager-policy-test.c bd-upnp-manager-policy.c bd-upnp-manager-policy.h
bd_upnp_manager_policy_test_CPPFLAGS = $(UPNP_MANAGER_CFLAGS)
bd_upnp_manager_policy_test_LDADD = $(UPNP_MANAGER_LIBS)
bd_upnp_manager_state_test_SOURCES = bd-upnp-manager-state-test.c bd-upnp-manager-policy.c bd-upnp-manager-policy.h
bd_upnp_manager_state_test_CPPFLAGS = -I$(top_builddir) $(UPNP_MANAGER_CFLAGS)
bd_upnp_manager_state_test_LDADD = $(UPNP_MANAGER_LIBS)
endif
