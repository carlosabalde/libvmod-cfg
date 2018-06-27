#include <stdlib.h>
#include <stdio.h>
#include <curl/curl.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"

#include "helpers.h"

int
event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    switch (e) {
        case VCL_EVENT_LOAD:
            curl_global_init(CURL_GLOBAL_ALL);
            if (vmod_state.locks.refs == 0) {
                vmod_state.locks.script = Lck_CreateClass("cfg.script");
                AN(vmod_state.locks.script);
            }
            vmod_state.locks.refs++;
            break;

        case VCL_EVENT_DISCARD:
            assert(vmod_state.locks.refs > 0);
            vmod_state.locks.refs--;
            if (vmod_state.locks.refs == 0) {
                VSM_Free(vmod_state.locks.script);
            }
            break;

        default:
            break;
    }

    return 0;
}
