
.. image:: https://github.com/carlosabalde/libvmod-cfg/actions/workflows/main.yml/badge.svg?branch=master
   :alt: GitHub Actions CI badge
   :target: https://github.com/carlosabalde/libvmod-cfg/actions
.. image:: https://codecov.io/gh/carlosabalde/libvmod-cfg/branch/master/graph/badge.svg
   :alt: Codecov badge
   :target: https://codecov.io/gh/carlosabalde/libvmod-cfg

VMOD useful to access to contents of environment variables and local or remote files from VCL, usually for configuration purposes.

Currently (1) JSON files; (2) Python's ConfigParser .INI-like files; (3) files containing collections of pattern matching rules; (4) Lua 5.1 scripts; and (5) ECMAScript (i.e. JavaScript) scripts are supported. Remote files can be accessed via HTTP or HTTPS.

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
        BOOL automated_backups=1,
        INT period=60,
        BOOL ignore_load_failures=1,
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
    Method BOOL .reload(BOOL force_backup=0)
    Method STRING .dump(BOOL stream=0, STRING prefix="")
    Method VOID .inspect()

    Method BOOL .is_set(STRING name)
    Method STRING .get(STRING name, STRING fallback="")

    ##
    ## Pattern matching rules.
    ##

    Object rules(
        STRING location,
        STRING backup="",
        BOOL automated_backups=1,
        INT period=60,
        BOOL ignore_load_failures=1,
        INT curl_connection_timeout=0,
        INT curl_transfer_timeout=0,
        BOOL curl_ssl_verify_peer=0,
        BOOL curl_ssl_verify_host=0,
        STRING curl_ssl_cafile="",
        STRING curl_ssl_capath="",
        STRING curl_proxy="")
    Method BOOL .reload(BOOL force_backup=0)
    Method VOID .inspect()

    Method STRING .get(STRING value, STRING fallback="")

    ##
    ## Lua & JavaScript scripts.
    ##

    Object script(
        STRING location="",
        STRING backup="",
        BOOL automated_backups=1,
        INT period=60,
        BOOL ignore_load_failures=1,
        ENUM { lua, javascript } type="lua",
        INT max_engines=128,
        INT max_cycles=0,
        INT min_gc_cycles=100,
        BOOL enable_sandboxing=1,
        INT lua_gc_step_size=100,
        BOOL lua_remove_loadfile_function=1,
        BOOL lua_remove_dotfile_function=1,
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
    Method BOOL .reload(BOOL force_backup=0)
    Method VOID .inspect()

    Method VOID .init(STRING code="")
    Method VOID .push(STRING arg)
    Method VOID .execute(BOOL gc_collect=0, BOOL flush_jemalloc_tcache=1)

    Method BOOL .result_is_error()
    Method BOOL .result_is_{nil,null}()
    Method BOOL .result_is_boolean()
    Method BOOL .result_is_number()
    Method BOOL .result_is_string()
    Method BOOL .result_is_{table,array}()

    Method STRING .get_result()

    Method BOOL .get_boolean_result()
    Method REAL .get_decimal_result()
    Method INT .get_integer_result()
    Method STRING .get_string_result()

    Method INT .get_{table,array}_result_length()
    Method BOOL .{table,array}_result_is_error(INT index)
    Method BOOL .{table,array}_result_is_{nil/null}(INT index)
    Method BOOL .{table,array}_result_is_boolean(INT index)
    Method BOOL .{table,array}_result_is_number(INT index)
    Method BOOL .{table,array}_result_is_string(INT index)
    Method BOOL .{table,array}_result_is_{table/array}(INT index)
    Method STRING .get_{table,array}_result_value(INT index)

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

    -- Remember Lua's pattern matching is not equivalent to POSIX regular
    -- expressions. Check https://www.lua.org/pil/20.2.html and
    -- http://lua-users.org/wiki/PatternsTutorial for details.
    -- Keep in mind varnish.regmatch(), varnish.regsub() and
    -- varnish.regsuball() are available in order to circumvent this
    -- inconvenience.
    if host == 'www.foo.com' or host == 'www.bar.com' then
        if string.match(url, '^/admin/') then
            return 'new'
        elseif varnish.regmatch(url, '^/(?:new|old)/') then
            return varnish.regsub(url, '^/(new|old)/.*$', '\1')
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
            period=60,
            type=lua);
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

ADVANCED SCRIPTING
==================

The original goal of this VMOD was offering efficient strategies to parametrize
VCL behavior based on information provided by external local or remote data
sources. That evolved from environment variables and configuration JSON / INI
files, to simple Lua / JavaScript programs executed in local interpreters
embedded in the Varnish Cache core. All these strategies, specially the one based on
INI files and the one based on Lua scripts interpreted by LuaJIT, have been
successfully and extensively tested in several highly trafficked environments.

At some point the VMOD evolved towards a more general framework useful to
execute arbitrarily complex Lua and JavaScript programs. Somehow something
similar to OpenResty in the Nginx arena. For example, using the cfg VMOD you
can write crazy Lua-flavoured VCL. That includes loading any rocks
you might need, facilities to safely share state among execution engines or among
Varnish threads, etc. Used with caution, this allows you to go beyond the
limits of VCL as a language and help you to model complex logic in the
caching layer. Of course, you can also use the VMOD to shoot yourself in
the foot.

Next you can see a simple useless example showing the power of the VMOD.
Beware it assumes a local Redis Server running and it depends on the
``http``, ``redis-lua`` and ``lua-cjson`` rocks. As well, beware Varnish
should be started with the right environment variables properly configured
(i.e. ``eval `luarocks path```).

::

    ...

    sub vcl_init {
        ...

        new script = cfg.script(
            "/dev/null",
            period=0,
            type=lua,
            lua_remove_loadfile_function=false,
            lua_load_package_lib=true,
            lua_load_io_lib=true,
            lua_load_os_lib=true);
    }

    sub vcl_deliver {
        ...

        script.init({"
            local http = require 'http.request'
            local redis = require 'redis'
            local json = require 'cjson'

            if varnish.engine.client == nil then
                varnish.engine.client = redis.connect('127.0.0.1', 6379)
                assert(varnish.engine.client ~= nil)
            end

            local status, city = pcall(
                varnish.engine.client.get, varnish.engine.client, ARGV[0])
            if not status then
                varnish.engine.client = nil
                error(city)
            end

            local hit = city ~= nil

            if not hit then
                varnish.shared.incr('api-requests', 1, 'global')
                local url = 'https://ipapi.co/' .. ARGV[0] .. '/json/'
                local headers, stream = http.new_from_uri(url):go()
                if headers:get(':status') == '200' then
                    local info = json.decode(stream:get_body_as_string())
                    city = info.city or '?'
                else
                    city = '?'
                end
                varnish.engine.client:set(ARGV[0], city, 'EX', 600)
            end

            varnish.set_header(
                'X-Script-Redis-Hit',
                hit and 'true' or 'false',
                'resp')

            varnish.set_header(
                'X-Script-City',
                city,
                'resp')

            varnish.set_header(
                'X-Script-Executions-Counter',
                varnish.shared.incr('executions', 1, 'global'),
                'resp')

            varnish.set_header(
                'X-Script-API-Requests-Counter',
                varnish.shared.get('api-requests', 'global'),
                'resp')
        "});
        script.push(client.ip);
        script.execute();
        script.free_result();
    }

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

**Beware this project contains multiples branches (master, 4.1, etc.). Please, select the branch to be used depending on your Varnish Cache version (Varnish trunk → master, Varnish 4.1.x → 4.1, etc.).**

Dependencies:

* `libcurl <https://curl.haxx.se/libcurl/>`_ - multi-protocol file transfer library.
* `luajit <http://luajit.org>`_ (recommended; disabled with ``--disable-luajit``) or `lua 5.1 <https://www.lua.org>`_ - powerful, efficient, lightweight, embeddable scripting language.

Beware using LuaJIT GC64 mode is recommended is order to avoid ``not enough memory`` errors due to the 2 GiB (os much less) limitation. See `this excellent post by OpenResty <https://blog.openresty.com/en/luajit-gc64-mode/>`_ for details.

COPYRIGHT
=========

See LICENSE for details.

BSD's implementation of the .INI file parser by Ben Hoyt has been borrowed from the `inih project <https://github.com/benhoyt/inih/>`_:

* https://github.com/benhoyt/inih/blob/master/ini.c
* https://github.com/benhoyt/inih/blob/master/ini.h

MIT's implementation of the JSON parser by Max Bruckner has been borrowed from the `cJSON project <https://github.com/DaveGamble/cJSON/>`_:

* https://github.com/DaveGamble/cJSON/blob/master/cJSON.c
* https://github.com/DaveGamble/cJSON/blob/master/cJSON.h

MIT's implementation of the JavaScript engine by Sami Vaarala has been built using the `Duktape project <https://github.com/svaarala/duktape/>`_:

::

    $ python tools/configure.py \
          --output-directory /tmp/duktape \
          --source-directory src-input \
          --config-metadata config

BSD's implementation of the red–black tree and the splay tree data structures by Niels Provos has been borrowed from the `Varnish Cache project <https://github.com/varnishcache/varnish-cache>`_:

* https://github.com/varnishcache/varnish-cache/blob/master/include/vtree.h

Copyright (c) Carlos Abalde <carlos.abalde@gmail.com>
