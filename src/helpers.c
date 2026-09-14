#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#include "cache/cache.h"
#include "vcl.h"
#include "vrt_obj.h"

#include "helpers.h"

vmod_state_t vmod_state = {
    .refs = 0,
    .libs.lua = NULL,
    .locks.vsc_seg = NULL,
    .locks.script = NULL,
    .log.syslog_enabled = 0,
    .log.stderr_enabled = 0
};

void
append_response_body(VRT_CTX, const char *value)
{
    if (ctx->method == VCL_MET_BACKEND_ERROR) {
        VRT_l_beresp_body(ctx, LBODY_ADD_STRING, NULL, TOSTRAND(value));
    } else {
        assert(ctx->method == VCL_MET_SYNTH);
        VRT_l_resp_body(ctx, LBODY_ADD_STRING, NULL, TOSTRAND(value));
    }
}
