AC_PREREQ([2.68])
AC_INIT([libvmod-cfg], [3.1], [], [vmod-cfg])
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

LIBS="$PTHREAD_LIBS $LIBS -lcurl"
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
