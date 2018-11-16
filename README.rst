
.. image:: https://travis-ci.org/carlosabalde/libvmod-cfg.svg?branch=5.2
   :alt: Travis CI badge
   :target: https://travis-ci.org/carlosabalde/libvmod-cfg/
.. image:: https://codecov.io/gh/carlosabalde/libvmod-cfg/branch/5.2/graph/badge.svg
   :alt: Codecov badge
   :target: https://codecov.io/gh/carlosabalde/libvmod-cfg

VMOD useful to access to contents of environment variables and local or remote files from VCL, usually for configuration purposes.

Currently (1) JSON files; (2) Python's ConfigParser .INI-like files; (3) files containing collections of pattern matching rules; and (4) Lua 5.1 scripts are supported. Remote files can be accessed via HTTP or HTTPS.

Wondering why I created this VMOD? How it could make your life easier? I wrote a blog post with some answers: `Moving logic to the caching edge (and back) <https://www.carlosabalde.com/blog/2018/06/27/moving-logic-to-the-caching-edge-and-back>`_.

Looking for official support for this VMOD? Please, contact `Allenta Consulting <https://www.allenta.com>`_, a `Varnish Software Premier Partner <https://www.varnish-software.com/partner/allenta-consulting>`_.

SYNOPSIS
========

import cfg;

::

    ##
    ## Environment variables.
    ##

    Object env()
    Method STRING .dump(BOOL stream=0, STRING prefix="")

    Method BOOL .is_set(STRING name)
    Method STRING .get(STRING name, STRING fallback="")

    ##
    ## JSON & INI files.
    ##

    Object file(
        STRING location,
        STRING backup="",
        INT period=60,
        INT curl_connection_timeout=0,
        INT curl_transfer_timeout=0,
        BOOL curl_ssl_verify_peer=0,
        BOOL curl_ssl_verify_host=0,
        STRING curl_ssl_cafile="",
        STRING curl_ssl_capath="",
        STRING curl_proxy="",
        ENUM { ini, json } format="ini",
        STRING name_delimiter=":",
        STRING value_delimiter=";")
    Method BOOL .reload()
    Method STRING .dump(BOOL stream=0, STRING prefix="")

    Method BOOL .is_set(STRING name)
    Method STRING .get(STRING name, STRING fallback="")

    ##
    ## Pattern matching rules.
    ##

    Object rules(
        STRING location,
        STRING backup="",
        INT period=60,
        INT curl_connection_timeout=0,
        INT curl_transfer_timeout=0,
        BOOL curl_ssl_verify_peer=0,
        BOOL curl_ssl_verify_host=0,
        STRING curl_ssl_cafile="",
        STRING curl_ssl_capath="",
        STRING curl_proxy="")
    Method BOOL .reload()

    Method STRING .get(STRING value, STRING fallback="")

    ##
    ## Lua scripts.
    ##

    Object script(
        STRING location="",
        STRING backup="",
        INT period=60,
        INT lua_max_engines=128,
        INT lua_max_cycles=0,
        INT lua_min_gc_cycles=100,
        INT lua_gc_step_size=100,
        BOOL lua_remove_loadfile_function=1,
        BOOL lua_remove_dofile_function=1,
        BOOL lua_load_package_lib=0,
        BOOL lua_load_io_lib=0,
        BOOL lua_load_os_lib=0,
        INT curl_connection_timeout=0,
        INT curl_transfer_timeout=0,
        BOOL curl_ssl_verify_peer=0,
        BOOL curl_ssl_verify_host=0,
        STRING curl_ssl_cafile="",
        STRING curl_ssl_capath="",
        STRING curl_proxy="")
    Method BOOL .reload()

    Method VOID .init(STRING code="")
    Method VOID .push(STRING arg)
    Method VOID .execute(BOOL gc_collect=0, BOOL flush_jemalloc_tcache=1)

    Method BOOL .result_is_error()
    Method BOOL .result_is_nil()
    Method BOOL .result_is_boolean()
    Method BOOL .result_is_number()
    Method BOOL .result_is_string()
    Method BOOL .result_is_table()

    Method STRING .get_result()

    Method BOOL .get_boolean_result()
    Method REAL .get_decimal_result()
    Method INT .get_integer_result()
    Method STRING .get_string_result()

    Method INT .get_table_result_length()
    Method BOOL .table_result_is_error(INT index)
    Method BOOL .table_result_is_nil(INT index)
    Method BOOL .table_result_is_boolean(INT index)
    Method BOOL .table_result_is_number(INT index)
    Method BOOL .table_result_is_string(INT index)
    Method BOOL .table_result_is_table(INT index)
    Method STRING .get_table_result_value(INT index)

    Method VOID .free_result()

    Method STRING .stats()
    Method INT .counter(STRING name)

EXAMPLE
=======

Environment variables
---------------------

::

    export VCL_SETTINGS=file:///etc/varnish/vcl.ini

/etc/varnish/vcl.ini
--------------------

::

    server: ACME

    [joke]
    start: 1459468800
    stop: 1459555200

https://www.example.com/ttls.rules
----------------------------------

::

    (?i)\.(?:jpg|png|svg)(?:\?.*)?$      -> 7d
    (?i)^www\.(?:foo|bar)\.com(?::\d+)?/ -> 1h

https://www.example.com/backends.lua
------------------------------------

::

    local host = string.gsub(string.lower(ARGV[0]), ':%d+$', '')
    local url = string.lower(ARGV[1])

    varnish.log('Running Lua backend selection logic')

    -- Remember that Lua's pattern matching is not equivalent to POSIX regular
    -- expressions. Check https://www.lua.org/pil/20.2.html and
    -- http://lua-users.org/wiki/PatternsTutorial for details.
    if host == 'www.foo.com' or host == 'www.bar.com' then
        if string.match(url, '^/admin/') then
            return 'new'
        end
    end

    return 'old'

/etc/varnish/default.vcl
------------------------

::

    vcl 4.0;

    import cfg;
    import std;

    backend old_be {
        .host = "127.0.0.1";
        .port = "8080";
    }

    backend new_be {
        .host = "127.0.0.1";
        .port = "8888";
    }

    acl internal {
        "localhost";
    }

    sub vcl_init {
        new env = cfg.env();

        if (env.is_set("VCL_SETTINGS")) {
            new settings = cfg.file(env.get("VCL_SETTINGS"));
        } else {
            return (fail);
        }

        new ttls = cfg.rules(
            "https://www.example.com/ttls.rules",
            period=300);

        new backends = cfg.script(
            "https://www.example.com/backends.lua",
            period=60);
    }

    sub vcl_recv {
        if (req.url ~ "^/(?:settings|ttls|backends)/(?:reload|dump)/$") {
            if (client.ip ~ internal) {
                if (req.url == "/settings/reload/") {
                    if (settings.reload()) {
                        return (synth(200, "Settings reloaded."));
                    } else {
                        return (synth(500, "Failed to reload settings."));
                    }
                } elsif (req.url == "/ttls/reload/") {
                    if (ttls.reload()) {
                        return (synth(200, "TTLs rules reloaded."));
                    } else {
                        return (synth(500, "Failed to reload TTLs rules."));
                    }
                } elsif (req.url == "/backends/reload/") {
                    if (backends.reload()) {
                        return (synth(200, "Backends script reloaded."));
                    } else {
                        return (synth(500, "Failed to reload backends script."));
                    }
                } elsif (req.url == "/settings/dump/") {
                    return (synth(700, "OK"));
                } else {
                    return (synth(404, "Not found."));
                }
            } else {
                return (synth(405, "Not allowed."));
            }
        }

        if (std.time(settings.get("joke:start"), now) < now &&
            std.time(settings.get("joke:stop"), now) > now) {
            return (synth(418, "I'm a teapot (RFC 2324)"));
        }
    }

    sub vcl_deliver {
        call set_server;
    }

    sub vcl_synth {
        call set_server;
        if (resp.status == 418) {
            return (deliver);
        } elsif (resp.status == 700) {
            set resp.status = 200;
            set resp.http.Content-Type = "application/json";
            settings.dump(stream=true);
            return (deliver);
        }
    }

    sub vcl_backend_fetch {
        backends.init();
        backends.push(bereq.http.Host);
        backends.push(bereq.url);
        backends.execute();
        if (backends.get_result() == "new") {
            set bereq.backend = new_be;
        } else {
            set bereq.backend = old_be;
        }
        backends.free_result();
    }

    sub vcl_backend_response {
        set beresp.ttl = std.duration(
            ttls.get(bereq.http.Host + bereq.url),
            60s);
    }

    sub set_server {
        if (settings.is_set("server")) {
            set resp.http.Server = settings.get("server");
        }
    }

Access to variables
-------------------

::

    $ curl http://127.0.0.1/settings/dump/ | python -m json.tool
    {
        "joke:start": "1459468800",
        "joke:stop": "1459555200",
        "server": "ACME"
    }

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

**Beware this project contains multiples branches (master, 4.1, etc.). Please, select the branch to be used depending on your Varnish Cache version (Varnish trunk → master, Varnish 4.1.x → 4.1, etc.).**

Dependencies:

* `libcurl <https://curl.haxx.se/libcurl/>`_ - multi-protocol file transfer library.
* `luajit <http://luajit.org>`_ (recommended; disabled with `--disable-luajit`) or `lua 5.1 <https://www.lua.org>`_ - powerful, efficient, lightweight, embeddable scripting language.

COPYRIGHT
=========

See LICENSE for details.

BSD's implementation of the .INI file parser by Ben Hoyt has been borrowed from the `inih project <https://github.com/benhoyt/inih/>`_:

* https://github.com/benhoyt/inih/blob/master/ini.c
* https://github.com/benhoyt/inih/blob/master/ini.h

MIT's implementation of the JSON parser by Max Bruckner has been borrowed from the `cJSON project <https://github.com/DaveGamble/cJSON/>`_:

* https://github.com/DaveGamble/cJSON/blob/master/cJSON.c
* https://github.com/DaveGamble/cJSON/blob/master/cJSON.h

BSD's implementation of the red–black tree and the splay tree data structures by Niels Provos has been borrowed from the `Varnish Cache project <https://github.com/varnishcache/varnish-cache>`_:

* https://github.com/varnishcache/varnish-cache/blob/master/include/vtree.h

Copyright (c) 2016-2018 Carlos Abalde <carlos.abalde@gmail.com>
