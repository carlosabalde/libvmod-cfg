#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "helpers.h"

struct vmod_cfg_rules {
    unsigned magic;
    #define VMOD_CFG_RULES_MAGIC 0x91baff91
};

VCL_VOID
vmod_rules__init(
    VRT_CTX, struct vmod_cfg_rules **rules, const char *vcl_name,
    VCL_STRING location, VCL_INT period,
    VCL_INT curl_connection_timeout, VCL_INT curl_transfer_timeout,
    VCL_BOOL curl_ssl_verify_peer, VCL_BOOL curl_ssl_verify_host,
    VCL_STRING curl_ssl_cafile, VCL_STRING curl_ssl_capath,
    VCL_STRING curl_proxy)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(rules);
    AZ(*rules);

    struct vmod_cfg_rules *instance;
    ALLOC_OBJ(instance, VMOD_CFG_RULES_MAGIC);
    AN(instance);

    // TODO.

    *rules = instance;
}

VCL_VOID
vmod_rules__fini(struct vmod_cfg_rules **rules)
{
    AN(rules);
    AN(*rules);

    struct vmod_cfg_rules *instance = *rules;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_RULES_MAGIC);

    // TODO.

    FREE_OBJ(instance);

    *rules = NULL;
}

VCL_STRING
vmod_rules_get(VRT_CTX, struct vmod_cfg_rules *rules, VCL_STRING value, VCL_STRING fallback)
{
    // TODO.
    return NULL;
}

VCL_BOOL
vmod_rules_reload(VRT_CTX, struct vmod_cfg_rules *rules)
{
    // TODO.
    return 0;
}
