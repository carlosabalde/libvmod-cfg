
.. image:: https://travis-ci.org/carlosabalde/libvmod-cfg.svg?branch=4.1
   :alt: Travis CI badge
   :target: https://travis-ci.org/carlosabalde/libvmod-cfg/

VMOD useful to access to contents of environment variables and configuration files from VCL.

Currently only Python's ConfigParser .INI-like configuration files are supported.

Looking for official support for this VMOD? Please, contact `Allenta Consulting <https://www.allenta.com>`_, a `Varnish Software Premium partner <https://www.varnish-software.com/partner/allenta-consulting>`_.

SYNOPSIS
========

import cfg;

::

    Object env()
    Method BOOL .is_set(STRING name)
    Method STRING .get(STRING name)

    Object file(
        STRING location,
        ENUM { ini } format="ini",
        STRING name_delimiter=":",
        STRING value_delimiter=";")
    Method BOOL .is_set(STRING name)
    Method STRING .get(STRING name)

EXAMPLE
=======

Environment variables
---------------------

::

    export VCL_SETTINGS=/etc/varnish/vcl.ini

/etc/varnish/vcl.ini
--------------------

::

    server: ACME

    [joke]
    start: 1459468800
    stop: 1459555200

/etc/varnish/default.vcl
------------------------

::

    vcl 4.0;

    import cfg;
    import std;

    backend default {
        .host = "127.0.0.1";
        .port = "8080";
    }

    sub vcl_init {
        new env = cfg.env();

        if (env.is_set("VCL_SETTINGS")) {
            new settings = cfg.file(env.get("VCL_SETTINGS"));
        } else {
            return (fail);
        }
    }

    sub vcl_recv {
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
        }
    }

    sub set_server {
        if (settings.is_set("server")) {
            set resp.http.Server = settings.get("server");
        }
    }

INSTALLATION
============

The source tree is based on autotools to configure the building, and does also have the necessary bits in place to do functional unit tests using the varnishtest tool.

COPYRIGHT
=========

See LICENSE for details.

BSD's implementation of the .INI file parser by Ben Hoyt has been borrowed from the `inih project <https://github.com/benhoyt/inih/>`_:

* https://github.com/benhoyt/inih/blob/master/ini.c
* https://github.com/benhoyt/inih/blob/master/ini.h

BSD's implementation of the red–black tree and the splay tree data structures by Niels Provos has been borrowed from the `Varnish Cache project <https://github.com/varnishcache/varnish-cache>`_:

* https://github.com/varnishcache/varnish-cache/blob/master/include/vtree.h

Copyright (c) 2016 Carlos Abalde <carlos.abalde@gmail.com>
