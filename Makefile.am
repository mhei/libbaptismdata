#
# Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#

ACLOCAL_AMFLAGS = -I m4

SUBDIRS		= src tools
EXTRA_DIST	= autogen.sh autogen-clean.sh LICENSES README.md

# Keep distcheck's test installation inside its temporary prefix.
AM_DISTCHECK_CONFIGURE_FLAGS = \
 --with-bdd-vendor-dir='$${prefix}/lib/bdd' \
 --with-systemdsystemunitdir='$${prefix}/lib/systemd/system'
