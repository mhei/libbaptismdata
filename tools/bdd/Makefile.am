AUTOMAKE_OPTIONS = subdir-objects

AM_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir) $(BDD_CFLAGS) \
 -D_POSIX_C_SOURCE=200809L -DBDD_VENDOR_DIR='"$(BDD_VENDOR_DIR)"' \
 -DBDD_WEB_ROOT='"$(datadir)/bdd"'

AM_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

sbin_PROGRAMS = bdd

bdd_SOURCES = \
 main.c \
 config.c \
 server.c \
 mdns.c \
 bdd.h
bdd_LDADD = $(top_builddir)/src/libbaptismdata.la $(BDD_LIBS)

bdddatadir = $(datadir)/bdd
dist_bdddata_DATA = \
 openapi.yaml \
 web/index.html \
 web/app.js \
 web/app.css \
 web/bootstrap.min.css \
 web/bootstrap-LICENSE

bddvendordir = $(BDD_VENDOR_DIR)
dist_bddvendor_DATA = bdd.conf

systemdsystemunit_DATA = bdd.service

bdd.service: $(srcdir)/bdd.service.in Makefile
	$(AM_V_GEN)$(SED) -e 's|@SBINDIR@|$(sbindir)|g' $< > $@

CLEANFILES = bdd.service

check_PROGRAMS = bdd-test-server bdd-config-test

bdd_test_server_SOURCES = $(bdd_SOURCES) tests/fake_baptismdata.c
bdd_test_server_LDADD = $(BDD_LIBS)
bdd_test_server_CPPFLAGS = $(AM_CPPFLAGS) -UBDD_WEB_ROOT -DBDD_WEB_ROOT='"$(abs_builddir)/test-webroot"'

# Exercise the same flat webroot layout that make install creates.
bdd_test_server_DEPENDENCIES = test-webroot/.stamp

test-webroot/.stamp: $(addprefix $(srcdir)/,$(dist_bdddata_DATA))
	$(AM_V_GEN)$(MKDIR_P) test-webroot
	$(AM_V_at)for asset in $(dist_bdddata_DATA); do \
		$(INSTALL_DATA) "$(srcdir)/$$asset" test-webroot/ || exit 1; \
	done
	$(AM_V_at)touch $@

CLEANFILES += test-webroot/.stamp $(addprefix test-webroot/,$(notdir $(dist_bdddata_DATA)))

bdd_config_test_SOURCES = config.c bdd.h tests/config-dump.c
bdd_config_test_LDADD = $(BDD_LIBS)
bdd_config_test_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir) -UBDD_VENDOR_DIR \
 -DBDD_VENDOR_DIR='"$(abs_builddir)/tests/config-root/vendor"' \
 -DBDD_RUN_DIR='"$(abs_builddir)/tests/config-root/run"' \
 -DBDD_ETC_DIR='"$(abs_builddir)/tests/config-root/etc"'

TESTS = \
 tests/run-integration.sh \
 tests/run-configuration.sh \
 tests/run-frontend.sh \
 tests/run-contract.sh \
 tests/run-zeroconf.sh

TESTS_ENVIRONMENT = PYTHON='$(PYTHON)' BDD_TEST_SERVER='$(abs_builddir)/bdd-test-server' \
 BDD_CONFIG_TEST='$(abs_builddir)/bdd-config-test' BDD_CONFIG_ROOT='$(abs_builddir)/tests/config-root'

EXTRA_DIST = \
 README.md \
 bdd.service.in \
 tests/integration.py \
 tests/contract.py \
 tests/configuration.py \
 tests/zeroconf.py \
 tests/frontend.cjs \
 tests/requirements.txt \
 $(TESTS)
