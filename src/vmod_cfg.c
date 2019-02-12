#include <stdlib.h>
#include <stdio.h>
#include <curl/curl.h>

#include "cache/cache.h"

#include "helpers.h"

int
vmod_event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    switch (e) {
        case VCL_EVENT_LOAD:
            curl_global_init(CURL_GLOBAL_ALL);
            if (vmod_state.locks.refs == 0) {
                vmod_state.locks.script = Lck_CreateClass(
                    &vmod_state.locks.vsc_seg, "cfg.script");
                AN(vmod_state.locks.script);
            }
            vmod_state.locks.refs++;
            break;

        case VCL_EVENT_DISCARD:
            assert(vmod_state.locks.refs > 0);
            vmod_state.locks.refs--;
            if (vmod_state.locks.refs == 0) {
                Lck_DestroyClass(&vmod_state.locks.vsc_seg);
            }
            break;

        default:
            break;
    }

    return 0;
}
