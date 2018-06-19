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
            break;

        case VCL_EVENT_WARM:
            AZ(pthread_mutex_lock(&vmod_state.mutex));
            vmod_state.version++;
            AZ(pthread_mutex_unlock(&vmod_state.mutex));
            break;

        default:
            break;
    }

    return 0;
}
