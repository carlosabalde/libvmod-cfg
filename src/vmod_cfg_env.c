#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "cache/cache.h"
#include "vcc_cfg_if.h"

#include "helpers.h"
#include "variables.h"

extern char **environ;

struct vmod_cfg_env {
    unsigned magic;
    #define VMOD_CFG_ENV_MAGIC 0x44baed10

    const char *name;

    variables_t variables;
};

static void
load_env(VRT_CTX, struct vmod_cfg_env *env)
{
    flush_global_variables(&env->variables);
    for (int i = 0; environ[i]; i++) {
        char *ptr = strchr(environ[i], '=');
        if (ptr != NULL) {
            variable_t *variable = new_global_variable(environ[i], ptr - environ[i], ptr + 1);
            AZ(VRBT_INSERT(variables, &env->variables, variable));
        }
    }
}

VCL_VOID
vmod_env__init(VRT_CTX, struct vmod_cfg_env **env, const char *vcl_name)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(env);
    AZ(*env);

    struct vmod_cfg_env *instance;
    ALLOC_OBJ(instance, VMOD_CFG_ENV_MAGIC);
    AN(instance);

    instance->name = strdup(vcl_name);
    AN(instance->name);
    VRBT_INIT(&instance->variables);

    load_env(ctx, instance);

    *env = instance;
}

VCL_VOID
vmod_env__fini(struct vmod_cfg_env **env)
{
    AN(env);
    AN(*env);

    struct vmod_cfg_env *instance = *env;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_ENV_MAGIC);

    free((void *) instance->name);
    instance->name = NULL;
    flush_global_variables(&instance->variables);

    FREE_OBJ(instance);

    *env = NULL;
}

VCL_STRING
vmod_env_dump(VRT_CTX, struct vmod_cfg_env *env, VCL_BOOL stream, VCL_STRING prefix)
{
    return dump_variables(ctx, &env->variables, stream, prefix);
}

VCL_BOOL
vmod_env_is_set(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name)
{
    return is_set_variable(ctx, &env->variables, name);
}

VCL_STRING
vmod_env_get(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name, VCL_STRING fallback)
{
    return get_variable(ctx, &env->variables, name, fallback);
}
