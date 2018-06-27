AM_CPPFLAGS = @VMOD_INCLUDES@ $(CURL_CFLAGS) $(LUA_CFLAGS) -Wall -Werror \
	-D_GNU_SOURCE=1 \
	-DINI_ALLOW_MULTILINE=1 \
	-DINI_ALLOW_BOM=1 \
	-DINI_ALLOW_INLINE_COMMENTS=1 \
	-DINI_INLINE_COMMENT_PREFIXES='";"' \
	-DINI_USE_STACK=1 \
	-DINI_STOP_ON_FIRST_ERROR=1 \
	-DINI_MAX_LINE=2048

vmoddir = @VMOD_DIR@
vmod_LTLIBRARIES = libvmod_cfg.la

libvmod_cfg_la_LDFLAGS = -module -export-dynamic -avoid-version
libvmod_cfg_la_LIBADD = $(CURL_LIBS) $(LUA_LIBS)

libvmod_cfg_la_SOURCES = \
	ini.c ini.h \
	cJSON.c cJSON.h \
	helpers.c helpers.h \
	remote.c remote.h \
	variables.c variables.h \
	vmod_cfg.c \
	vmod_cfg_env.c \
	vmod_cfg_file.c \
	vmod_cfg_rules.c \
	vmod_cfg_script.c \
	vtree.h

nodist_libvmod_cfg_la_SOURCES = \
	vcc_if.c \
	vcc_if.h

vmod_cfg.lo: vcc_if.c vcc_if.h

vcc_if.c: vcc_if.h

vcc_if.h: @VMODTOOL@ $(top_srcdir)/src/vmod_cfg.vcc
	@VMODTOOL@ $(top_srcdir)/src/vmod_cfg.vcc

VMOD_TESTS = $(top_srcdir)/src/tests/*.vtc
.PHONY: $(VMOD_TESTS)

$(top_srcdir)/src/tests/*.vtc: libvmod_cfg.la
	$(abs_srcdir)/tests/runner.sh @VARNISHTEST@ $@ -Dvarnishd=@VARNISHD@ -Dvmod_topbuild=$(abs_top_builddir)

check: $(VMOD_TESTS)

EXTRA_DIST = \
	tests/runner.sh \
	vmod_cfg.vcc \
	$(VMOD_TESTS)

CLEANFILES = \
	$(builddir)/vcc_if.c \
	$(builddir)/vcc_if.h \
	$(builddir)/vmod_cfg.rst \
	$(builddir)/vmod_cfg.man.rst
