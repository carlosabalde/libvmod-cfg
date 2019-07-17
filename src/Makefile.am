AM_CFLAGS = $(VARNISHAPI_CFLAGS) $(CURL_CFLAGS) $(LUA_CFLAGS) $(CODE_COVERAGE_CFLAGS) -Wall -Werror \
	-D_GNU_SOURCE=1 \
	-DINI_ALLOW_MULTILINE=1 \
	-DINI_ALLOW_BOM=1 \
	-DINI_ALLOW_INLINE_COMMENTS=1 \
	-DINI_INLINE_COMMENT_PREFIXES='";"' \
	-DINI_USE_STACK=1 \
	-DINI_STOP_ON_FIRST_ERROR=1 \
	-DINI_MAX_LINE=16384
AM_LDFLAGS = $(VARNISHAPI_LIBS) $(CURL_LIBS) $(LUA_LIBS) $(VMOD_LDFLAGS) $(CODE_COVERAGE_LDFLAGS)

vmod_LTLIBRARIES = libvmod_cfg.la

libvmod_cfg_la_SOURCES = \
	ini.c ini.h \
	cJSON.c cJSON.h \
	duktape.c duktape.h duk_config.h \
	helpers.c helpers.h \
	remote.c remote.h \
	script_helpers.c script_helpers.h \
	script_javascript_helpers.h script_javascript.c script_javascript.h \
	script_lua_helpers.h script_lua.c script_lua.h \
	variables.c variables.h \
	vmod_cfg.c \
	vmod_cfg_env.c \
	vmod_cfg_file.c \
	vmod_cfg_rules.c \
	vmod_cfg_script.c \
	vtree.h

nodist_libvmod_cfg_la_SOURCES = \
	vcc_cfg_if.c \
	vcc_cfg_if.h

dist_man_MANS = vmod_cfg.3

@BUILD_VMOD_CFG@

AM_TESTS_ENVIRONMENT = \
	PATH="$(VMOD_TEST_PATH)" \
	LD_LIBRARY_PATH="$(VARNISH_LIBRARY_PATH)"
TEST_EXTENSIONS = .vtc
VTC_LOG_COMPILER = $(abs_srcdir)/tests/runner.sh varnishtest -v
AM_VTC_LOG_FLAGS = -Dvmod_cfg="$(VMOD_CFG)"

TESTS = @VMOD_TESTS@

EXTRA_DIST = \
	script_javascript_helpers.js \
	script_lua_helpers.lua \
	tests/runner.sh \
	vmod_cfg.vcc \
	$(VMOD_TESTS)

script_javascript_helpers.h: script_javascript_helpers.js
	@( \
		set -e; \
		cd '$(abs_srcdir)'; \
		xxd -i script_javascript_helpers.js script_javascript_helpers.h; \
	)

script_lua_helpers.h: script_lua_helpers.lua
	@( \
		set -e; \
		cd '$(abs_srcdir)'; \
		xxd -i script_lua_helpers.lua script_lua_helpers.h; \
	)

clean-vmod-cfg:
	rm -f vcc_cfg_if.c vcc_cfg_if.h
	rm -f vmod_cfg.rst vmod_cfg.man.rst vmod_cfg.3
	rm -f script_javascript_helpers.h script_lua_helpers.h
