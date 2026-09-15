#include "config.h"

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

/*
 * Return an engine to the pool of free engines and record the outcome of the
 * execution it just completed ('unknown': the function had to be compiled &
 * registered first; 'success': the execution completed; 'gc': the garbage
 * collector ran). Both are done under a single acquisition of the script lock
 * on purpose: this runs once per execution.
 */
void
release_engine(
    VRT_CTX, struct vmod_cfg_script *script, engine_t *engine,
    unsigned unknown, unsigned success, unsigned gc)
{
    CHECK_OBJ_NOTNULL(engine, ENGINE_MAGIC);

    engine->memory = script->api.get_engine_used_memory(engine);

    int size = script->api.get_engine_stack_size(engine);
    if (size > 0) {
        LOG(ctx, LOG_ERR,
            "Found non-zero stack when releasing engine (script=%s, size=%d)",
            script->name, size);
    }

    Lck_Lock(&script->state.mutex);

    VTAILQ_REMOVE(&script->state.engines.busy, engine, list);

    // Beware engines are stacked (i.e. LIFO) on purpose: the engine that just
    // finished is the one with warm CPU caches, so it should be the next one
    // picked by 'lock_engine()'. This also keeps the working set of engines
    // (i.e. scripting engine heaps) as small as the load allows.
    //
    // The trade-off is RSS retention: engines beyond the working set sink to
    // the tail of the free list and are never popped again, so they never run
    // GC steps (only run after an execution) and never accrue cycles towards
    // 'max_cycles' (only checked at pop time by 'is_valid_engine()'). Heaps
    // inflated during a traffic burst therefore stay resident until the next
    // script reload or VCL discard. Should it ever matter in practice, the
    // cheap mitigation is to stamp a last-used timestamp on release and reap
    // long-idle tail engines periodically.
    VTAILQ_INSERT_HEAD(&script->state.engines.free, engine, list);

    script->state.stats.executions.total++;
    if (unknown) {
        script->state.stats.executions.unknown++;
    }
    if (!success) {
        script->state.stats.executions.failed++;
    }
    if (gc) {
        script->state.stats.executions.gc++;
    }

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

    reset_task_state(result, 1, 1);

    return result;
}

void
reset_task_state(
    task_state_t *state, unsigned reset_variables, unsigned reset_execution)
{
    if (reset_variables) {
        VRBT_INIT(&state->variables);
    }

    if (reset_execution) {
        state->execution.code = NULL;
        state->execution.argc = -1;
        memset(&state->execution.argv[0], 0, sizeof(state->execution.argv));
        memset(&state->execution.result, 0, sizeof(state->execution.result));
        state->execution.result.nvalues = -1;
    }
}

void
free_task_state(task_state_t *state)
{
    CHECK_OBJ_NOTNULL(state, TASK_STATE_MAGIC);

    reset_task_state(state, 1, 1);

    FREE_OBJ(state);
}

task_state_t *
get_task_state(VRT_CTX, struct vmod_cfg_script *script, unsigned reset_execution)
{
    task_state_t *result = NULL;

    // One state per script instance and per task. Beware in Varnish 6.0
    // VRT_priv_task() may fail returning a NULL value when the task workspace
    // is exhausted, so callers must be ready to receive a NULL state (the
    // task has already been failed here in that case).
    struct vmod_priv *task_priv = VRT_priv_task(ctx, script);
    if (task_priv == NULL) {
        FAIL_WS(ctx, NULL);
    }

    if (task_priv->priv == NULL) {
        task_priv->priv = new_task_state();
        task_priv->free = (vmod_priv_free_f *)free_task_state;
        result = task_priv->priv;
    } else {
        result = task_priv->priv;
        CHECK_OBJ(result, TASK_STATE_MAGIC);
    }

    if (reset_execution) {
        reset_task_state(result, 0, 1);
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
    CHECK_OBJ_NOTNULL(engine, ENGINE_MAGIC);

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
    CHECK_OBJ_NOTNULL(regexp, REGEXP_MAGIC);

    free((void *) regexp->text);
    regexp->text = NULL;

    VRE_free(&regexp->vre);
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

// Builds the error message reported when a regexp cannot be instantiated.
// Returns NULL (after failing the VCL) if the workspace is exhausted.
const char *
regexp_error(VRT_CTX, const char *regexp)
{
    AN(regexp);

    const char *error = WS_Printf(ctx->ws, "Failed to instantiate '%s' regexp.", regexp);
    if (error == NULL) {
        FAIL_WS(ctx, NULL);
    }

    return error;
}

unsigned
varnish_regmatch_re_command(VRT_CTX, const char *string, vre_t *re)
{
    AN(string);
    AN(re);

    return VRT_re_match(ctx, string, re);
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
        result = varnish_regmatch_re_command(ctx, string, re);
        if (!cache) {
            VRE_free(&re);
        }
    } else {
        *error = regexp_error(ctx, regexp);
    }

    return result;
}

const char *
varnish_regsub_re_command(
    VRT_CTX, const char *string, vre_t *re, const char *sub, unsigned all)
{
    AN(string);
    AN(re);
    AN(sub);

    return VRT_regsub(ctx, all, string, re, sub);
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
        result = varnish_regsub_re_command(ctx, string, re, sub, all);
        if (!cache) {
            VRE_free(&re);
        }
    } else {
        *error = regexp_error(ctx, regexp);
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
    VRT_CTX, struct vmod_cfg_script *script, task_state_t *state,
    const char *key, const char *scope, unsigned is_locked)
{
    AN(ctx->ws);

    const char *result = NULL;

    if (strcmp(scope, "task") == 0 ||
        strcmp(scope, "global") == 0 ||
        strcmp(scope, "all") == 0) {
        unsigned done = 0;

        // Task scope.
        if (strcmp(scope, "task") == 0 || strcmp(scope, "all") == 0) {
            variable_t *variable = find_variable(&state->variables, key);
            if (variable != NULL) {
                // Is not required to duplicate the returned value. The value
                // was already allocated in the thread workspace when setting
                // the value in the scope of the task.
                result = variable->value;
                done = 1;
            }
        }

        // Global scope.
        if (!done && (strcmp(scope, "global") == 0 || strcmp(scope, "all") == 0)) {
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
        }
    }

    return result;
}

void
varnish_shared_set_command(
    VRT_CTX, struct vmod_cfg_script *script, task_state_t *state,
    const char *key, const char *value, const char *scope,
    unsigned is_locked)
{
    AN(ctx->ws);

    if (strcmp(scope, "task") == 0 ||
        strcmp(scope, "global") == 0) {
        // Task scope.
        if (strcmp(scope, "task") == 0) {
            variable_t *variable = find_variable(&state->variables, key);
            if (variable == NULL) {
                variable = (void *)WS_Alloc(ctx->ws, sizeof(variable_t));
                if (variable == NULL) {
                    FAIL_WS(ctx, );
                }
                variable->magic = VARIABLE_MAGIC;
                variable->name = WS_Copy(ctx->ws, key, -1);
                if (variable->name == NULL) {
                    FAIL_WS(ctx, );
                }
                AZ(VRBT_INSERT(variables, &state->variables, variable));
            }

            variable->value = WS_Copy(ctx->ws, value, -1);
            if (variable->value == NULL) {
                FAIL_WS(ctx, );
            }

        // Global scope.
        } else {
            if (!is_locked) {
                AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
            }

            variable_t *variable = find_variable(&script->state.variables.list, key);
            if (variable == NULL) {
                variable = new_global_variable(key, strlen(key), value);
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
    }
}

void
varnish_shared_unset_command(
    VRT_CTX, struct vmod_cfg_script *script, task_state_t *state,
    const char *key, const char *scope, unsigned is_locked)
{
    AN(ctx->ws);

    if (strcmp(scope, "task") == 0 ||
        strcmp(scope, "global") == 0 ||
        strcmp(scope, "all") == 0) {
        // Task scope.
        if (strcmp(scope, "task") == 0 || strcmp(scope, "all") == 0) {
            variable_t *variable = find_variable(&state->variables, key);
            if (variable != NULL) {
                VRBT_REMOVE(variables, &state->variables, variable);
            }
        }

        // Global scope.
        if (strcmp(scope, "global") == 0 || strcmp(scope, "all") == 0) {
            if (!is_locked) {
                AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
            }

            variable_t *variable = find_variable(&script->state.variables.list, key);
            if (variable != NULL) {
                VRBT_REMOVE(variables, &script->state.variables.list, variable);
                script->state.variables.n--;
                free_global_variable(variable);
            }

            if (!is_locked) {
                AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
            }
        }
    }
}
