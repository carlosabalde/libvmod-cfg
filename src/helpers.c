#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#include "helpers.h"

vmod_state_t vmod_state = {
    .refs = 0,
    .libs.lua = NULL,
    .locks.vsc_seg = NULL,
    .locks.script = NULL,
    .log.syslog_disabled = 0,
    .log.stderr_disabled = 0
};
