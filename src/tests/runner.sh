#! /bin/bash

set -x

cleanup() {
    rm -rf "$1"
}

TMP=`mktemp -d`
chmod o+rwx "$TMP"

unset  LIBVMOD_CFG_value
export LIBVMOD_CFG_VALUE="hello world!"

trap "cleanup $TMP" EXIT

"$1" -Dtmp=$TMP "${@:2}"
