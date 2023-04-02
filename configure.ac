dnl
dnl Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
dnl
dnl SPDX-License-Identifier: LGPL-2.1-or-later
dnl

dnl
dnl Process this file with autoconf to produce a configure script.
dnl

dnl
dnl Take care to update the libtool versioning when required (LIBBAPTISMDATA_LD_*).
dnl http://www.gnu.org/software/libtool/manual/html_node/Libtool-versioning.html
dnl
m4_define([libbaptismdata_version_major], [0])
m4_define([libbaptismdata_version_minor], [1])
m4_define([libbaptismdata_version_micro], [0])

m4_define([libbaptismdata_version],
    [libbaptismdata_version_major.libbaptismdata_version_minor.libbaptismdata_version_micro])

AC_PREREQ([2.63])
AC_INIT([libbaptismdata], [libbaptismdata_version], [https://github.com/mhei/libbaptismdata/issues])

AM_INIT_AUTOMAKE([foreign no-dist-gzip dist-xz])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])

# enable nice build output on automake1.11
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

LIBBAPTISMDATA_VERSION_MAJOR=libbaptismdata_version_major
LIBBAPTISMDATA_VERSION_MINOR=libbaptismdata_version_minor
LIBBAPTISMDATA_VERSION_MICRO=libbaptismdata_version_micro
LIBBAPTISMDATA_VERSION=libbaptismdata_version
AC_SUBST(LIBBAPTISMDATA_VERSION_MAJOR)
AC_SUBST(LIBBAPTISMDATA_VERSION_MINOR)
AC_SUBST(LIBBAPTISMDATA_VERSION_MICRO)
AC_SUBST(LIBBAPTISMDATA_VERSION)

# ABI version
# http://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
LIBBAPTISMDATA_LD_CURRENT=1
LIBBAPTISMDATA_LD_REVISION=0
LIBBAPTISMDATA_LD_AGE=0
LIBBAPTISMDATA_LT_VERSION_INFO=$LIBBAPTISMDATA_LD_CURRENT:$LIBBAPTISMDATA_LD_REVISION:$LIBBAPTISMDATA_LD_AGE
AC_SUBST(LIBBAPTISMDATA_LT_VERSION_INFO)

PACKAGE_DESCRIPTION="Library for accessing baptism data"
AC_SUBST(PACKAGE_DESCRIPTION)

AC_CANONICAL_HOST

AC_PROG_CC
AC_PROG_CXX
AC_PROG_MAKE_SET
LT_INIT

PKG_CHECK_MODULES([LIBUBOOTENV], [libubootenv])

AC_CONFIG_FILES([
        Makefile
        src/Makefile
        src/baptismdata-version.h
        src/libbaptismdata.pc
        tools/Makefile
])
AC_OUTPUT
