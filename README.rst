
.. image:: https://travis-ci.org/carlosabalde/libvmod-cfg.svg?branch=master
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
