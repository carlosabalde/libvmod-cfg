#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cache/cache.h"
#include "vsb.h"
#include "vsha256.h"
#include "vre.h"

#include "vtree.h"

#include "script_helpers.h"
#include "helpers.h"

/******************************************************************************
 * BASICS.
 *****************************************************************************/

static unsigned
is_valid_engine(VRT_CTX, struct vmod_cfg_script *script, engine_t *engine)
{
    Lck_AssertHeld(&script->state.mutex);

    if ((script->max_cycles > 0) &&
        (engine->ncycles >= script->max_cycles)) {
        script->state.stats.engines.dropped.cycles++;
        return 0;
    }

    return 1;
}

engine_t *
lock_engine(VRT_CTX, struct vmod_cfg_script *script)
{
    engine_t *result = NULL;

    Lck_Lock(&script->state.mutex);

retry:
    while (!VTAILQ_EMPTY(&script->state.engines.free)) {
        // Extract engine.
        result = VTAILQ_FIRST(&script->state.engines.free);
        CHECK_OBJ_NOTNULL(result, ENGINE_MAGIC);

        // Mark the engine as busy.
        VTAILQ_REMOVE(&script->state.engines.free, result, list);
        VTAILQ_INSERT_TAIL(&script->state.engines.busy, result, list);

        // Is the engine valid?
        if (!is_valid_engine(ctx, script, result)) {
            VTAILQ_REMOVE(&script->state.engines.busy, result, list);
            script->state.engines.n--;
            free_engine(result);
            result = NULL;
        } else {
            break;
        }
    }

    // If required, create new engine. If maximum number of engines has been
    // reached, wait for another thread releasing an engine.
    if (result == NULL) {
        if (script->state.engines.n >= script->max_engines) {
            Lck_CondWait(&script->state.engines.cond, &script->state.mutex, 0);
            script->state.stats.workers.blocked++;
            goto retry;
        } else {
            result = script->api.new_engine(ctx, script);
            script->state.stats.engines.total++;
            VTAILQ_INSERT_TAIL(&script->state.engines.busy, result, list);
            script->state.engines.n++;
        }
    }

    Lck_Unlock(&script->state.mutex);

    AN(result);

    return result;
}

void
unlock_engine(VRT_CTX, struct vmod_cfg_script *script, engine_t *engine)
{
    CHECK_OBJ_NOTNULL(engine, ENGINE_MAGIC);

    engine->memory = script->api.get_engine_used_memory(engine);

    int size = script->api.get_engine_stack_size(engine);
    if (size > 0) {
        LOG(ctx, LOG_ERR,
            "Found non-zero stack when unlocking engine (script=%s, size=%d)",
            script->name, size);
    }

    Lck_Lock(&script->state.mutex);
    VTAILQ_REMOVE(&script->state.engines.busy, engine, list);
    VTAILQ_INSERT_TAIL(&script->state.engines.free, engine, list);
    AZ(pthread_cond_signal(&script->state.engines.cond));
    Lck_Unlock(&script->state.mutex);
}

const char *
new_function_name(const char *code)
{
    unsigned char sha256[SHA256_LEN];

    struct SHA256Context sha_ctx;
    SHA256_Init(&sha_ctx);
    SHA256_Update(&sha_ctx, code, strlen(code));
    SHA256_Final(sha256, &sha_ctx);

    char *result = malloc(2 + SHA256_LEN * 2 + 1);
    AN(result);
    strcpy(result, "f_");
    char *ptr = result + 2;
    for (int i = 0; i < SHA256_LEN; i++) {
        sprintf(ptr, "%.2x", sha256[i]);
        ptr += 2;
    }

    return result;
}

/******************************************************************************
 * TASK STATE.
 *****************************************************************************/

task_state_t *
new_task_state()
{
    task_state_t *result;
    ALLOC_OBJ(result, TASK_STATE_MAGIC);
    AN(result);

    reset_task_state(result);

    return result;
}

void
reset_task_state(task_state_t *state)
{
    state->execution.code = NULL;
    state->execution.argc = -1;
    memset(&state->execution.argv[0], 0, sizeof(state->execution.argv));
    memset(&state->execution.result, 0, sizeof(state->execution.result));
    state->execution.result.nvalues = -1;
}

void
free_task_state(task_state_t *state)
{
    reset_task_state(state);

    FREE_OBJ(state);
}

task_state_t *
get_task_state(VRT_CTX, struct vmod_priv *task_priv, unsigned reset)
{
    task_state_t *result = NULL;

    if (task_priv->priv == NULL) {
        task_priv->priv = new_task_state();
        task_priv->free = (vmod_priv_free_f *)free_task_state;
        result = task_priv->priv;
    } else {
        result = task_priv->priv;
        CHECK_OBJ(result, TASK_STATE_MAGIC);
    }

    if (reset) {
        reset_task_state(result);
    }

    return result;
}

/******************************************************************************
 * ENGINES.
 *****************************************************************************/

engine_t *
new_engine(enum ENGINE_TYPE type, void *ctx)
{
    engine_t *result;
    ALLOC_OBJ(result, ENGINE_MAGIC);
    AN(result);

    result->type = type;
    if (result->type == ENGINE_TYPE_LUA) {
        result->ctx.L = (lua_State *) ctx;
    } else if (result->type == ENGINE_TYPE_JAVASCRIPT) {
        result->ctx.D = (duk_context *) ctx;
    }
    result->ncycles = 0;
    result->memory = 0;

    return result;
}

void
free_engine(engine_t *engine)
{
    if (engine->type == ENGINE_TYPE_LUA) {
        lua_close(engine->ctx.L);
        engine->ctx.L = NULL;
    } else if (engine->type == ENGINE_TYPE_JAVASCRIPT) {
        duk_destroy_heap(engine->ctx.D);
        engine->ctx.D = NULL;
    }
    engine->ncycles = 0;
    engine->memory = 0;

    FREE_OBJ(engine);
}

void
flush_engines(engines_t *engines)
{
    engine_t *iengine;
    while (!VTAILQ_EMPTY(engines)) {
        iengine = VTAILQ_FIRST(engines);
        CHECK_OBJ_NOTNULL(iengine, ENGINE_MAGIC);
        VTAILQ_REMOVE(engines, iengine, list);
        free_engine(iengine);
    }
}

/******************************************************************************
 * REGEXPS.
 *****************************************************************************/

static int
regexpcmp(const regexp_t *v1, const regexp_t *v2)
{
    return strcmp(v1->text, v2->text);
}

VRBT_GENERATE(regexps, regexp, tree, regexpcmp);

regexp_t *
new_regexp(const char *text, vre_t *vre)
{
    regexp_t *result = NULL;
    ALLOC_OBJ(result, REGEXP_MAGIC);
    AN(result);

    result->text = strdup(text);
    AN(result->text);
    result->vre = vre;

    return result;
}

void
free_regexp(regexp_t *regexp)
{
    free((void *) regexp->text);
    regexp->text = NULL;

    VRT_re_fini(regexp->vre);
    regexp->vre = NULL;

    FREE_OBJ(regexp);
}

vre_t *
init_regexp(
    VRT_CTX, struct vmod_cfg_script *script,
    const char *regexp, unsigned cache)
{

    // Initializations.
    vre_t *result = NULL;

    // Using the cache? Try to find existing compiled regexp.
    if (cache) {
        regexp_t search_regexp;
        search_regexp.text = regexp;
        AZ(pthread_rwlock_rdlock(&script->state.regexps.rwlock));
        regexp_t *cached_regexp = VRBT_FIND(regexps, &script->state.regexps.list, &search_regexp);
        if (cached_regexp != NULL) {
            result = cached_regexp->vre;
        }
        AZ(pthread_rwlock_unlock(&script->state.regexps.rwlock));
    }

    // Not using the cache / Not found? Compile the regexp.
    if (result == NULL) {
        const char *error;
        int erroroffset;
        result = VRE_compile(regexp, 0, &error, &erroroffset);

        // Error compiling regexp?
        if (result == NULL) {
            LOG(ctx, LOG_ERR,
                "Got error while compiling regexp (script=%s, regexp=%s): %s",
                script->name, regexp, error);

        // Cache result?
        } else if (cache) {
            regexp_t *a_regexp = new_regexp(regexp, result);
            AZ(pthread_rwlock_wrlock(&script->state.regexps.rwlock));
            regexp_t *cached_regexp = VRBT_FIND(regexps, &script->state.regexps.list, a_regexp);
            if (cached_regexp == NULL) {
                AZ(VRBT_INSERT(regexps, &script->state.regexps.list, a_regexp));
                script->state.regexps.n++;
            } else {
                free_regexp(a_regexp);
                result = cached_regexp->vre;
            }
            AZ(pthread_rwlock_unlock(&script->state.regexps.rwlock));
        }
    }

    // Done!
    return result;
}

/******************************************************************************
 * VARNISH.* COMMANDS.
 *****************************************************************************/

void
varnish_log_command(VRT_CTX, const char *message)
{
    AN(message);

    if (ctx->vsl != NULL) {
        VSLb(ctx->vsl, SLT_VCL_Log, "%s", message);
    } else {
        VSL(SLT_VCL_Log, 0, "%s", message);
    }
}

unsigned
varnish_regmatch_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *string,
    const char *regexp, unsigned cache, const char **error)
{
    AN(string);
    AN(regexp);

    unsigned result = 0;
    *error = NULL;

    vre_t *re = init_regexp(ctx, script, regexp, cache);
    if (re != NULL) {
        result = VRT_re_match(ctx, string, re);
        if (!cache) {
            VRT_re_fini(re);
        }
    } else {
        *error = WS_Printf(ctx->ws, "Failed to instantiate '%s' regexp.", regexp);
        if (*error == NULL) {
            FAIL_WS(ctx, 0);
        }
    }

    return result;
}

const char *
varnish_regsub_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *string,
    const char *regexp, const char *sub, unsigned cache, unsigned all,
    const char **error)
{
    AN(string);
    AN(regexp);
    AN(sub);

    const char *result = NULL;
    *error = NULL;

    vre_t *re = init_regexp(ctx, script, regexp, cache);
    if (re != NULL) {
        result = VRT_regsub(ctx, all, string, re, sub);
        if (!cache) {
            VRT_re_fini(re);
        }
    } else {
        *error = WS_Printf(ctx->ws, "Failed to instantiate '%s' regexp.", regexp);
        if (*error == NULL) {
            FAIL_WS(ctx, NULL);
        }
    }

    return result;
}

static enum gethdr_e
get_http_where(const char *name)
{
    if (strcmp(name, "req") == 0) {
        return HDR_REQ;
    } else if (strcmp(name, "req-top") == 0) {
        return HDR_REQ_TOP;
    } else if (strcmp(name, "bereq") == 0) {
        return HDR_BEREQ;
    } else if (strcmp(name, "resp") == 0) {
        return HDR_RESP;
    } else if (strcmp(name, "beresp") == 0) {
        return HDR_BERESP;
    } else if (strcmp(name, "obj") == 0) {
        return HDR_OBJ;
    } else {
        WRONG("Invalid header type value.");
    }
}

static unsigned
is_valid_http_where(VRT_CTX, enum gethdr_e where)
{
    if (where == HDR_REQ) {
        return ctx->http_req != NULL;
    } else if (where == HDR_REQ_TOP) {
        return ctx->http_req_top != NULL;
    } else if (where == HDR_BEREQ) {
        return ctx->http_bereq != NULL;
    } else if (where == HDR_RESP) {
        return ctx->http_resp != NULL;
    } else if (where == HDR_BERESP) {
        return ctx->http_beresp != NULL;
    } else if (where == HDR_OBJ) {
        return ctx->req != NULL && ctx->req->objcore != NULL;
    } else {
        return 0;
    }
}

const char *
varnish_get_header_command(
    VRT_CTX, const char *name, const char *where, const char **error)
{
    AN(name);
    AN(where);

    const char *result = NULL;
    *error = NULL;

    if (strcmp(where, "req") == 0 ||
        strcmp(where, "req-top") == 0 ||
        strcmp(where, "bereq") == 0 ||
        strcmp(where, "beresp") == 0 ||
        strcmp(where, "resp") == 0 ||
        strcmp(where, "obj") == 0) {
        enum gethdr_e he = get_http_where(where);
        if (is_valid_http_where(ctx, he)) {
            char buffer[strlen(name) + 3];
            sprintf(buffer, "%c%s:", (char) (strlen(name) + 1), name);
            const struct gethdr_s hs = {he, buffer};
            result = VRT_GetHdr(ctx, &hs);
        } else {
            *error = WS_Printf(
                ctx->ws,
                "varnish.get_header() called over unavailable '%s' object.",
                where);
            if (*error == NULL) {
                FAIL_WS(ctx, NULL);
            }
        }
    }

    return result;
}

void
varnish_set_header_command(
    VRT_CTX, const char *name, const char *value, const char *where,
    const char **error)
{
    AN(name);
    AN(value);
    AN(where);

    *error = NULL;

    if (strcmp(where, "req") == 0 ||
        strcmp(where, "req-top") == 0 ||
        strcmp(where, "bereq") == 0 ||
        strcmp(where, "beresp") == 0 ||
        strcmp(where, "resp") == 0) {
        enum gethdr_e he = get_http_where(where);
        if (is_valid_http_where(ctx, he)) {
            char buffer[strlen(name) + 3];
            sprintf(buffer, "%c%s:", (char) (strlen(name) + 1), name);
            const struct gethdr_s hs = {he, buffer};
            VRT_SetHdr(
                ctx,
                &hs,
                value,
                vrt_magic_string_end);
        } else {
            *error = WS_Printf(
                ctx->ws,
                "varnish.set_header() called over unavailable '%s' object.",
                where);
            if (*error == NULL) {
                FAIL_WS(ctx, );
            }
        }
    }
}

/******************************************************************************
 * VARNISH.SHARED.* COMMANDS.
 *****************************************************************************/

const char *
varnish_shared_get_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *key,
    unsigned is_locked)
{
    const char *result = NULL;
    unsigned fail = 0;

    if (!is_locked) {
        AZ(pthread_rwlock_rdlock(&script->state.variables.rwlock));
    }

    variable_t *variable = find_variable(&script->state.variables.list, key);
    if (variable != NULL) {
        result = WS_Copy(ctx->ws, variable->value, -1);
        fail = result == NULL;
    }

    if (!is_locked) {
        AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    }

    if (fail) {
        FAIL_WS(ctx, NULL);
    }

    return result;
}

void
varnish_shared_set_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *key, const char *value,
    unsigned is_locked)
{
    if (!is_locked) {
        AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
    }

    variable_t *variable = find_variable(&script->state.variables.list, key);
    if (variable == NULL) {
        variable = new_variable(key, strlen(key), value);
        AZ(VRBT_INSERT(variables, &script->state.variables.list, variable));
        script->state.variables.n++;
    } else {
        free((void *) variable->value);
        variable->value = strdup(value);
        AN(variable->value);
    }

    if (!is_locked) {
        AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    }
}

void
varnish_shared_unset_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *key,
    unsigned is_locked)
{
    if (!is_locked) {
        AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
    }

    variable_t *variable = find_variable(&script->state.variables.list, key);
    if (variable != NULL) {
        VRBT_REMOVE(variables, &script->state.variables.list, variable);
        script->state.variables.n--;
    }

    if (!is_locked) {
        AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    }
}
