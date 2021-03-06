#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <curl/curl.h>

#include "cache/cache.h"

#include "helpers.h"

static void *
dlreopen(void *addr)
{
    // See: http://lua-users.org/lists/lua-l/2013-10/msg00690.html.
    Dl_info info;
    AN(dladdr(addr, &info));
    AN(info.dli_fname);
    void *handle = dlopen(info.dli_fname, RTLD_GLOBAL|RTLD_NOW);
    AN(handle);
    return handle;
}

int
vmod_event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    switch (e) {
        case VCL_EVENT_LOAD:
            curl_global_init(CURL_GLOBAL_ALL);
            if (vmod_state.refs == 0) {
                vmod_state.libs.lua = dlreopen(&luaL_newstate);
                vmod_state.locks.script = Lck_CreateClass(
                    &vmod_state.locks.vsc_seg, "cfg.script");
                AN(vmod_state.locks.script);
            }
            vmod_state.refs++;
            break;

        case VCL_EVENT_DISCARD:
            assert(vmod_state.refs > 0);
            vmod_state.refs--;
            if (vmod_state.refs == 0) {
                AZ(dlclose(vmod_state.libs.lua));
                Lck_DestroyClass(&vmod_state.locks.vsc_seg);
            }
            break;

        default:
            break;
    }

    return 0;
}
