#! /bin/bash

set -x

cleanup() {
    rm -rf "$1"
}

TMP=`mktemp -d`

unset  LIBVMOD_CFG_value
export LIBVMOD_CFG_VALUE="hello world!"

trap "cleanup $TMP" EXIT

"$@" -Dtmp=$TMP
