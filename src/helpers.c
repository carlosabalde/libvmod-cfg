#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#include "cache/cache.h"
#include "vsb.h"
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

/******************************************************************************
 * SYNTH RESPONSE BODY
 *****************************************************************************/

static void
fini_task_synth_vsb(VRT_CTX, void *ptr)
{
    struct vsb *vsb;
    CAST_OBJ_NOTNULL(vsb, ptr, VSB_MAGIC);
    VSB_destroy(&vsb);
}

static const struct vmod_priv_methods task_synth_vsb_priv_methods[1] = {{
    .magic = VMOD_PRIV_METHODS_MAGIC,
    .type = "task_synth_vsb",
    .fini = fini_task_synth_vsb
}};

struct vsb *
new_task_synth_vsb(VRT_CTX)
{
    struct vsb *vsb = VSB_new_auto();
    AN(vsb);

    struct vmod_priv *priv = VRT_priv_task(ctx, vsb);
    AN(priv);
    AZ(priv->priv);
    priv->priv = vsb;
    priv->methods = task_synth_vsb_priv_methods;

    return vsb;
}

void
append_synth_response_body(VRT_CTX, struct vsb *vsb)
{
    CHECK_OBJ_NOTNULL(vsb, VSB_MAGIC);

    // 'vsb' must have been returned by 'new_task_synth_vsb()' during this same
    // task: a foreign VSB would either leak or be destroyed while the synth
    // storage engine still holds references into it. Instead of encoding that
    // in a wrapper type, the contract is enforced looking the VSB up in the
    // task priv registry, where 'new_task_synth_vsb()' keyed it by its own
    // pointer.
    struct vmod_priv *priv = VRT_priv_task_get(ctx, vsb);
    AN(priv);
    assert(priv->priv == vsb);
    assert(priv->methods == task_synth_vsb_priv_methods);

    AZ(VSB_finish(vsb));

    if (ctx->method == VCL_MET_BACKEND_ERROR) {
        VRT_l_beresp_body(ctx, LBODY_ADD_STRING, NULL, TOSTRAND(VSB_data(vsb)));
    } else {
        assert(ctx->method == VCL_MET_SYNTH);
        VRT_l_resp_body(ctx, LBODY_ADD_STRING, NULL, TOSTRAND(VSB_data(vsb)));
    }
}
