AC_PREREQ([2.68])
AC_INIT([libvmod-cfg], [3.4], [], [vmod-cfg])
AC_COPYRIGHT([Copyright (c) 2016-2018 Carlos Abalde <carlos.abalde@gmail.com>])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR(src/vmod_cfg.vcc)
AC_CONFIG_HEADER([config.h])

AM_INIT_AUTOMAKE([1.12 -Wall -Werror foreign parallel-tests])
AM_SILENT_RULES([yes])
AM_PROG_AR

LT_PREREQ([2.2.6])
LT_INIT([dlopen disable-static])

ax_enable_compile_warnings=error
AX_CODE_COVERAGE
AX_COMPILER_FLAGS_CFLAGS
AX_PTHREAD(,[AC_MSG_ERROR([Could not configure pthreads support])])

LIBS="$PTHREAD_LIBS $LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"

AC_ARG_WITH([rst2man],
	AS_HELP_STRING(
		[--with-rst2man=PATH],
		[Location of rst2man (auto)]),
	[RST2MAN="$withval"],
	AC_CHECK_PROGS(RST2MAN, [rst2man rst2man.py], []))

# Check for pkg-config
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([CURL], [libcurl])

# --enable-luajit / --disable-luajit
AC_ARG_ENABLE(luajit,
    AS_HELP_STRING(
        [--enable-luajit],
        [use LuaJIT (default is YES)]),
        ,
        [enable_luajit=yes])
if test "x$enable_luajit" = xyes; then
    PKG_CHECK_MODULES(
        [LUA],
        [luajit],
        [AC_DEFINE([LUAJIT_ENABLED], [1], [LuaJIT enabled])],
        [])
else
    PKG_CHECK_MODULES([LUA], [lua >= 5.1], [], [
        PKG_CHECK_MODULES([LUA], [lua51 >= 5.1], [], [
            PKG_CHECK_MODULES([LUA], [lua5.1 >= 5.1], [], [
                PKG_CHECK_MODULES(LUA, [lua-5.1 >= 5.1])])
        ])
    ])
fi

# --enable-jemalloc / --disable-jemalloc
AC_ARG_ENABLE(jemalloc,
    AS_HELP_STRING(
        [--enable-jemalloc],
        [use jemalloc (default is YES)]),
        ,
        [enable_jemalloc=yes])
if test "x$enable_jemalloc" = xyes; then
    AC_DEFINE([JEMALLOC_ENABLED], [1], [jemalloc enabled])
fi

m4_ifndef([VARNISH_PREREQ], AC_MSG_ERROR([Need varnish.m4 -- see README.rst]))

VARNISH_PREREQ([5.1], [5.2])
VARNISH_VMODS([cfg])

VMOD_TESTS="$(cd $srcdir/src && echo tests/*.vtc)"
AC_SUBST(VMOD_TESTS)

PKG_CHECK_VAR([LIBVARNISHAPI_LIBDIR], [varnishapi], [libdir])
AC_SUBST([VARNISH_LIBRARY_PATH],
	[$LIBVARNISHAPI_LIBDIR:$LIBVARNISHAPI_LIBDIR/varnish])

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

VARNISHAPI_CFLAGS: $VARNISHAPI_CFLAGS
VARNISHAPI_LIBS:   $VARNISHAPI_LIBS

CURL_CFLAGS:       $CURL_CFLAGS
CURL_LIBS:         $CURL_LIBS

LUA_CFLAGS:        $LUA_CFLAGS
LUA_LIBS:          $LUA_LIBS
")
