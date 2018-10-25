AC_PREREQ(2.59)
AC_COPYRIGHT([Copyright (c) 2016-2018 Carlos Abalde <carlos.abalde@gmail.com>])
AC_INIT([libvmod-cfg], [1.9])
AC_CONFIG_MACRO_DIR([m4])
m4_ifndef([VARNISH_VMOD_INCLUDES], AC_MSG_ERROR([Need varnish.m4 -- see README.rst]))
AC_CONFIG_SRCDIR(src/vmod_cfg.vcc)
AM_CONFIG_HEADER(config.h)

AC_CANONICAL_SYSTEM
AC_LANG(C)

AM_INIT_AUTOMAKE([foreign])

AC_GNU_SOURCE
AC_PROG_CC
AC_PROG_CC_STDC
if test "x$ac_cv_prog_cc_c99" = xno; then
	AC_MSG_ERROR([Could not find a C99 compatible compiler])
fi
AC_PROG_CPP

LT_INIT([disable-static])
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_PROG_MAKE_SET

# Check for rst utilities
AC_CHECK_PROGS(RST2MAN, [rst2man rst2man.py], "no")
if test "x$RST2MAN" = "xno"; then
	AC_MSG_WARN([rst2man not found - not building man pages])
fi
AM_CONDITIONAL(HAVE_RST2MAN, [test "x$RST2MAN" != "xno"])

# Check for pkg-config
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([CURL], [libcurl])

# --enable-luajit / --disable-luajit
AC_ARG_ENABLE(
    luajit,
    [
        AS_HELP_STRING(
            [--enable-luajit],
            [use LuaJIT (default is YES)])
    ],
    [],
    [enable_luajit=yes])
if test "x$enable_luajit" = xyes; then
    PKG_CHECK_MODULES(
        [LUA],
        [luajit],
        [
            AC_DEFINE([LUAJIT_ENABLED], [1], [LuaJIT enabled])
        ],
        [
            AC_MSG_ERROR([LuaJIT not found. Try --disable-luajit])
        ])
else
    AX_PROG_LUA(
        [5.1],
        [5.4],
        [
            AX_LUA_HEADERS([], [AC_MSG_ERROR([No Lua headers found])])
            AX_LUA_LIBS([], [AC_MSG_ERROR([No Lua libs found])])
            [LUA_CFLAGS="${LUA_INCLUDE}"]
            [LUA_LIBS="${LUA_LIB}"]
        ],
        [
            AC_MSG_ERROR([Lua not found. Try --enable-luajit])
        ])
fi

# --enable-flush-jemalloc-tcache / --disable-flush-jemalloc-tcache
AC_ARG_ENABLE(
    flush-jemalloc-tcache,
    [
        AS_HELP_STRING(
            [--enable-flush-jemalloc-tcache],
            [use jemalloc tcache flush (default is YES)])
    ],
    [],
    [enable_flush_jemalloc_tcache=yes])
AC_MSG_CHECKING([for jemalloc tcache flush])
if test "x$enable_flush_jemalloc_tcache" = xyes; then
    AC_MSG_RESULT([enabled])
    AC_DEFINE(
        [JEMALLOC_TCACHE_FLUSH_ENABLED],
        [1],
        [jemalloc tcache flush enabled])
else
    AC_MSG_RESULT([disabled])
fi

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([sys/stdlib.h])

# backwards compat with older pkg-config
# - pull in AC_DEFUN from pkg.m4
m4_ifndef([PKG_CHECK_VAR], [
# PKG_CHECK_VAR(VARIABLE, MODULE, CONFIG-VARIABLE,
# [ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND])
# -------------------------------------------
# Retrieves the value of the pkg-config variable for the given module.
AC_DEFUN([PKG_CHECK_VAR],
[AC_REQUIRE([PKG_PROG_PKG_CONFIG])dnl
AC_ARG_VAR([$1], [value of $3 for $2, overriding pkg-config])dnl

_PKG_CONFIG([$1], [variable="][$3]["], [$2])
AS_VAR_COPY([$1], [pkg_cv_][$1])

AS_VAR_IF([$1], [""], [$5], [$4])dnl
])# PKG_CHECK_VAR
])

PKG_CHECK_MODULES([libvarnishapi], [varnishapi])
PKG_CHECK_VAR([LIBVARNISHAPI_PREFIX], [varnishapi], [prefix])
PKG_CHECK_VAR([LIBVARNISHAPI_DATAROOTDIR], [varnishapi], [datarootdir])
PKG_CHECK_VAR([LIBVARNISHAPI_BINDIR], [varnishapi], [bindir])
PKG_CHECK_VAR([LIBVARNISHAPI_SBINDIR], [varnishapi], [sbindir])
AC_SUBST([LIBVARNISHAPI_DATAROOTDIR])

# Varnish include files tree
VARNISH_VMOD_INCLUDES
VARNISH_VMOD_DIR
VARNISH_VMODTOOL
# inherit the prefix from Varnish.
# acessing ac_ variable because AC_PREFIX_DEFAULT acts too early
ac_default_prefix=$LIBVARNISHAPI_PREFIX

AC_PATH_PROG([VARNISHTEST], [varnishtest], [],
    [$LIBVARNISHAPI_BINDIR:$LIBVARNISHAPI_SBINDIR:$PATH])
AC_PATH_PROG([VARNISHD], [varnishd], [],
    [$LIBVARNISHAPI_SBINDIR:$LIBVARNISHAPI_BINDIR:$PATH])

AX_CURLOPT_CHECK([CURLOPT_TIMEOUT_MS])
AX_CURLOPT_CHECK([CURLOPT_CONNECTTIMEOUT_MS])

AC_CONFIG_FILES([
	Makefile
	src/Makefile
])
AC_OUTPUT

AS_ECHO("
==== $PACKAGE_STRING ====

CC:                $CC
CFLAGS:            $CFLAGS
LDFLAGS:           $LDFLAGS

CURL_CFLAGS:       $CURL_CFLAGS
CURL_LIBS:         $CURL_LIBS

LUA_CFLAGS:        $LUA_CFLAGS
LUA_LIBS:          $LUA_LIBS
")
