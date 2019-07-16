#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "cache/cache.h"
#include "vsb.h"
#include "vre.h"
#include "vcc_cfg_if.h"

#include "script_javascript.h"
#include "script_lua.h"

/******************************************************************************
 * BASICS.
 *****************************************************************************/

static unsigned
script_check_callback(VRT_CTX, void *ptr, char *contents, unsigned is_backup)
{
    unsigned result = 0;

    struct vmod_cfg_script *script;
    CAST_OBJ_NOTNULL(script, ptr, VMOD_CFG_SCRIPT_MAGIC);

    const char *name = NULL;
    if (script->api.execute(ctx, script, contents, &name, 0, NULL, NULL, 0, 1)) {
        LOG(ctx, LOG_INFO,
            "Remote successfully compiled (script=%s, location=%s, is_backup=%d, function=%s, code=%.80s...)",
            script->name, script->remote->location.raw, is_backup, name, contents);

        Lck_Lock(&script->state.mutex);
        if (script->state.function.code != NULL) {
            free((void *) script->state.function.code);
        }
        script->state.function.code = strdup(contents);
        AN(script->state.function.code);
        if (script->state.function.name != NULL) {
            free((void *) script->state.function.name);
        }
        script->state.function.name = name;
        Lck_Unlock(&script->state.mutex);

        result = 1;
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to compile remote (script=%s, location=%s, is_backup=%d, code=%.80s...)",
            script->name, script->remote->location.raw, is_backup, contents);

        free((void *) name);
    }

    return result;
}

static unsigned
script_check(VRT_CTX, struct vmod_cfg_script *script, unsigned force)
{
    if (script->remote != NULL) {
        return check_remote(ctx, script->remote, force, &script_check_callback, script);
    } else {
        return 1;
    }
}

VCL_VOID
vmod_script__init(
    VRT_CTX, struct vmod_cfg_script **script, const char *vcl_name,
    VCL_STRING location, VCL_STRING backup, VCL_INT period, VCL_ENUM type,
    VCL_INT max_engines, VCL_INT max_cycles,
    VCL_INT min_gc_cycles, VCL_BOOL enable_sandboxing, VCL_INT lua_gc_step_size,
    VCL_BOOL lua_remove_loadfile_function, VCL_BOOL lua_remove_dotfile_function,
    VCL_BOOL lua_load_package_lib, VCL_BOOL lua_load_io_lib, VCL_BOOL lua_load_os_lib,
    VCL_INT curl_connection_timeout, VCL_INT curl_transfer_timeout,
    VCL_BOOL curl_ssl_verify_peer, VCL_BOOL curl_ssl_verify_host,
    VCL_STRING curl_ssl_cafile, VCL_STRING curl_ssl_capath,
    VCL_STRING curl_proxy)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(script);
    AZ(*script);

    struct vmod_cfg_script *instance = NULL;

    if ((period >= 0) &&
        (max_engines > 0) &&
        (max_cycles >= 0) &&
        (min_gc_cycles > 0) &&
        (lua_gc_step_size > 0) &&
        (curl_connection_timeout >= 0) &&
        (curl_transfer_timeout >= 0)) {
        ALLOC_OBJ(instance, VMOD_CFG_SCRIPT_MAGIC);
        AN(instance);

        instance->name = strdup(vcl_name);
        AN(instance->name);
        if ((location != NULL) && (strlen(location) > 0)) {
            instance->remote = new_remote(
                location, backup, period, curl_connection_timeout, curl_transfer_timeout,
                curl_ssl_verify_peer, curl_ssl_verify_host, curl_ssl_cafile,
                curl_ssl_capath, curl_proxy);
        } else {
            instance->remote = NULL;
        }
        instance->max_engines = max_engines;
        instance->max_cycles = max_cycles;
        instance->min_gc_cycles = min_gc_cycles;
        instance->enable_sandboxing = enable_sandboxing;
        if (type == vmod_enum_lua) {
            instance->type = ENGINE_TYPE_LUA;
            instance->engine_cfg.lua.gc_step_size = lua_gc_step_size;
            instance->engine_cfg.lua.functions.loadfile = !lua_remove_loadfile_function;
            instance->engine_cfg.lua.functions.dotfile = !lua_remove_dotfile_function;
            instance->engine_cfg.lua.libraries.package = lua_load_package_lib;
            instance->engine_cfg.lua.libraries.io = lua_load_io_lib;
            instance->engine_cfg.lua.libraries.os = lua_load_os_lib;
            instance->api.new_engine = new_lua_engine;
            instance->api.get_engine_used_memory = get_lua_engine_used_memory;
            instance->api.get_engine_stack_size = get_lua_engine_stack_size;
            instance->api.execute = execute_lua;
        } else if (type == vmod_enum_javascript) {
            instance->type = ENGINE_TYPE_JAVASCRIPT;
            instance->api.new_engine = new_javascript_engine;
            instance->api.get_engine_used_memory = get_javascript_engine_used_memory;
            instance->api.get_engine_stack_size = get_javascript_engine_stack_size;
            instance->api.execute = execute_javascript;
        } else {
            WRONG("Illegal type value.");
        }
        Lck_New(&instance->state.mutex, vmod_state.locks.script);
        instance->state.function.code = NULL;
        instance->state.function.name = NULL;
        AZ(pthread_cond_init(&instance->state.engines.cond, NULL));
        instance->state.engines.n = 0;
        VTAILQ_INIT(&instance->state.engines.free);
        VTAILQ_INIT(&instance->state.engines.busy);
        AZ(pthread_rwlock_init(&instance->state.regexps.rwlock, NULL));
        instance->state.regexps.n = 0;
        VRBT_INIT(&instance->state.regexps.list);
        AZ(pthread_rwlock_init(&instance->state.variables.rwlock, NULL));
        instance->state.variables.n = 0;
        VRBT_INIT(&instance->state.variables.list);
        memset(&instance->state.stats, 0, sizeof(instance->state.stats));

        script_check(ctx, instance, 1);
    }

    *script = instance;
}

VCL_VOID
vmod_script__fini(struct vmod_cfg_script **script)
{
    AN(script);
    AN(*script);

    struct vmod_cfg_script *instance = *script;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_SCRIPT_MAGIC);

    free((void *) instance->name);
    instance->name = NULL;
    if (instance->remote != NULL) {
        free_remote(instance->remote);
        instance->remote = NULL;
    }
    instance->max_engines = 0;
    instance->max_cycles = 0;
    instance->min_gc_cycles = 0;
    instance->enable_sandboxing = 0;
    if (instance->type == ENGINE_TYPE_LUA) {
        instance->engine_cfg.lua.gc_step_size = 0;
        instance->engine_cfg.lua.functions.loadfile = 0;
        instance->engine_cfg.lua.functions.dotfile = 0;
        instance->engine_cfg.lua.libraries.package = 0;
        instance->engine_cfg.lua.libraries.io = 0;
        instance->engine_cfg.lua.libraries.os = 0;
    } else if (instance->type == ENGINE_TYPE_JAVASCRIPT) {
    }
    instance->api.new_engine = NULL;
    instance->api.get_engine_used_memory = NULL;
    instance->api.get_engine_stack_size = NULL;
    instance->api.execute = NULL;
    Lck_Delete(&instance->state.mutex);
    if (instance->state.function.code != NULL) {
        free((void *) instance->state.function.code);
        instance->state.function.code = NULL;
    }
    if (instance->state.function.name != NULL) {
        free((void *) instance->state.function.name);
        instance->state.function.name = NULL;
    }
    AZ(pthread_cond_destroy(&instance->state.engines.cond));
    instance->state.engines.n = 0;
    flush_engines(&instance->state.engines.free);
    flush_engines(&instance->state.engines.busy);
    AZ(pthread_rwlock_destroy(&instance->state.regexps.rwlock));
    instance->state.regexps.n = 0;
    regexp_t *regexp, *regexp_tmp;
    VRBT_FOREACH_SAFE(regexp, regexps, &instance->state.regexps.list, regexp_tmp) {
        CHECK_OBJ_NOTNULL(regexp, REGEXP_MAGIC);
        VRBT_REMOVE(regexps, &instance->state.regexps.list, regexp);
        free_regexp(regexp);
    }
    AZ(pthread_rwlock_destroy(&instance->state.variables.rwlock));
    instance->state.variables.n = 0;
    variable_t *variable, *variable_tmp;
    VRBT_FOREACH_SAFE(variable, variables, &instance->state.variables.list, variable_tmp) {
        CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
        VRBT_REMOVE(variables, &instance->state.variables.list, variable);
        free_variable(variable);
    }
    memset(&instance->state.stats, 0, sizeof(instance->state.stats));

    FREE_OBJ(instance);

    *script = NULL;
}

VCL_BOOL
vmod_script_reload(VRT_CTX, struct vmod_cfg_script *script)
{
    return script_check(ctx, script, 1);
}

VCL_VOID
vmod_script_init(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv,
    VCL_STRING code)
{
    task_state_t *state = get_task_state(ctx, task_priv, 1);

    if ((code != NULL) && (strlen(code) > 0)) {
        state->execution.code = code;
    }
    state->execution.argc = 0;
}

VCL_VOID
vmod_script_push(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv,
    VCL_STRING arg)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to .init() was not executed.
    if ((state->execution.argc >= 0) &&
        (state->execution.argc < MAX_EXECUTION_ARGS)) {
        // Handle NULL arguments as empty strings.
        if (arg == NULL) {
            arg = WS_Copy(ctx->ws, "", -1);
            if (arg == NULL) {
                FAIL_WS(ctx,);
            }
        }
        state->execution.argv[state->execution.argc++] = arg;
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to push argument (script=%s, limit=%d)",
            script->name, MAX_EXECUTION_ARGS);
    }
}

VCL_VOID
vmod_script_execute(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv,
    VCL_BOOL gc_collect, VCL_BOOL flush_jemalloc_tcache)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    // Do not continue if the initial call to .init() was not executed.
    if (state->execution.argc >= 0) {
        const char *code, *name;
        // Copy code -and optionally function name- (both will be allocated
        // in the workspace) to be executed.
        if (state->execution.code == NULL) {
            unsigned stop = 0;
            Lck_Lock(&script->state.mutex);
            if ((script->state.function.code != NULL) &&
                (script->state.function.name != NULL)) {
                code = WS_Copy(ctx->ws, script->state.function.code, -1);
                name = WS_Copy(ctx->ws, script->state.function.name, -1);
            } else {
                stop = 1;
            }
            Lck_Unlock(&script->state.mutex);
            if (stop) {
                state->execution.result.values[0].type = RESULT_VALUE_TYPE_ERROR;
                LOG(ctx, LOG_ERR,
                    "Code not available when trying to execute script (script=%s)",
                    script->name);
                return;
            } else if (code == NULL || name == NULL) {
                FAIL_WS(ctx,);
            }
        } else {
            code = state->execution.code;
            name = NULL;
        }

        // Execute the script.
        if (!script->api.execute(
                ctx,  script, code, &name,
                state->execution.argc, state->execution.argv,
                &state->execution.result, gc_collect, flush_jemalloc_tcache)) {
            LOG(ctx, LOG_ERR,
                "Got error while executing script (script=%s, function=%s, code=%.80s...)",
                script->name, name, code);
        }

        // Release function name if it was allocated (in the heap) during
        // script execution.
        if (state->execution.code != NULL) {
            free((void *) name);
        }
    }
}

#define VMOD_SCRIPT_RESULT_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_script_result_is_ ## lower(VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv) \
{ \
    task_state_t *state = get_task_state(ctx, task_priv, 0); \
    \
    return \
        (state->execution.argc >= 0) && \
        (state->execution.result.nvalues == -1) && \
        (state->execution.result.values[0].type == RESULT_VALUE_TYPE_ ## upper); \
}

VMOD_SCRIPT_RESULT_IS_FOO(error, ERROR)
VMOD_SCRIPT_RESULT_IS_FOO(nil, NIL)
VMOD_SCRIPT_RESULT_IS_FOO(null, NIL)
VMOD_SCRIPT_RESULT_IS_FOO(boolean, BOOLEAN)
VMOD_SCRIPT_RESULT_IS_FOO(number, NUMBER)
VMOD_SCRIPT_RESULT_IS_FOO(string, STRING)

#undef VMOD_SCRIPT_RESULT_IS_FOO

VCL_BOOL
vmod_script_result_is_table(VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    return
        (state->execution.argc >= 0) &&
        (state->execution.result.nvalues >= 0);
}

VCL_BOOL
vmod_script_result_is_array(VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    return vmod_script_result_is_table(ctx, script, task_priv);
}

static const char *
get_result(VRT_CTX, result_value_t *result_value)
{
    const char *value;

    switch (result_value->type) {
        case RESULT_VALUE_TYPE_BOOLEAN:
            value = WS_Copy(ctx->ws, result_value->value.boolean ? "true" : "false", -1);
            if (value == NULL) {
                FAIL_WS(ctx, NULL);
            }
            return value;

        case RESULT_VALUE_TYPE_NUMBER:
            value = WS_Printf(ctx->ws, "%g", result_value->value.number);
            if (value == NULL) {
                FAIL_WS(ctx, NULL);
            }
            return value;

        case RESULT_VALUE_TYPE_STRING:
            return result_value->value.string;

        default:
            return NULL;
    }
}

VCL_STRING
vmod_script_get_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    if ((state->execution.argc >= 0) &&
        (state->execution.result.nvalues == -1)) {
        return get_result(ctx, &state->execution.result.values[0]);
    } else {
        return NULL;
    }
}

VCL_BOOL
vmod_script_get_boolean_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    if ((state->execution.argc >= 0) &&
        (state->execution.result.nvalues == -1) &&
        (state->execution.result.values[0].type == RESULT_VALUE_TYPE_BOOLEAN)) {
        return state->execution.result.values[0].value.boolean;
    } else {
        return 0;
    }
}

VCL_REAL
vmod_script_get_decimal_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    if ((state->execution.argc >= 0) &&
        (state->execution.result.nvalues == -1) &&
        (state->execution.result.values[0].type == RESULT_VALUE_TYPE_NUMBER)) {
        return state->execution.result.values[0].value.number;
    } else {
        return 0.0;
    }
}

VCL_INT
vmod_script_get_integer_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    return (VCL_INT) vmod_script_get_decimal_result(ctx, script, task_priv);
}

VCL_STRING
vmod_script_get_string_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    if ((state->execution.argc >= 0) &&
        (state->execution.result.nvalues == -1) &&
        (state->execution.result.values[0].type == RESULT_VALUE_TYPE_STRING)) {
        return state->execution.result.values[0].value.string;
    } else {
        return NULL;
    }
}

VCL_INT
vmod_script_get_table_result_length(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    if ((state->execution.argc >= 0) &&
        (state->execution.result.nvalues >= 0)) {
        return state->execution.result.nvalues;
    } else {
        return 0;
    }
}

VCL_INT
vmod_script_get_array_result_length(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    return vmod_script_get_table_result_length(ctx, script, task_priv);
}

#define VMOD_SCRIPT_TABLE_RESULT_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_script_table_result_is_ ## lower(VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv, VCL_INT index) \
{ \
    task_state_t *state = get_task_state(ctx, task_priv, 0); \
    \
    return \
        (state->execution.argc >= 0) && \
        (state->execution.result.nvalues >= 0) && \
        (index < state->execution.result.nvalues) && \
        (state->execution.result.values[index].type == RESULT_VALUE_TYPE_ ## upper); \
} \
VCL_BOOL \
vmod_script_array_result_is_ ## lower(VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv, VCL_INT index) \
{ \
    return vmod_script_table_result_is_ ## lower(ctx, script, task_priv, index); \
} \

VMOD_SCRIPT_TABLE_RESULT_IS_FOO(error, ERROR)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(nil, NIL)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(null, NIL)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(boolean, BOOLEAN)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(number, NUMBER)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(string, STRING)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(table, TABLE)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(array, TABLE)

#undef VMOD_SCRIPT_TABLE_RESULT_IS_FOO

VCL_STRING
vmod_script_get_table_result_value(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv,
    VCL_INT index)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    if ((state->execution.argc >= 0) &&
        (state->execution.result.nvalues >= 0) &&
        (index < state->execution.result.nvalues)) {
        return get_result(ctx, &state->execution.result.values[index]);
    } else {
        return NULL;
    }
}

VCL_STRING
vmod_script_get_array_result_value(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv,
    VCL_INT index)
{
    return vmod_script_get_table_result_value(ctx, script, task_priv, index);
}

VCL_VOID
vmod_script_free_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    get_task_state(ctx, task_priv, 1);
}

static uint64_t
engines_memory(VRT_CTX, struct vmod_cfg_script *script, unsigned is_locked)
{
    if (!is_locked) {
        Lck_Lock(&script->state.mutex);
    } else {
        Lck_AssertHeld(&script->state.mutex);
    }

    engine_t *iengine;
    uint64_t memory = 0;
    VTAILQ_FOREACH(iengine, &script->state.engines.free, list) {
        memory += iengine->memory;
    }
    VTAILQ_FOREACH(iengine, &script->state.engines.busy, list) {
        memory += iengine->memory;
    }

    if (!is_locked) {
        Lck_Unlock(&script->state.mutex);
    }

    return memory;
}

VCL_STRING
vmod_script_stats(VRT_CTX, struct vmod_cfg_script *script)
{
    AZ(pthread_rwlock_rdlock(&script->state.variables.rwlock));
    AZ(pthread_rwlock_rdlock(&script->state.regexps.rwlock));
    Lck_Lock(&script->state.mutex);
    char *result = WS_Printf(ctx->ws,
        "{"
          "\"engines\": {"
            "\"current\": %d,"
            "\"total\": %d,"
            "\"memory\": %" PRIu64 ","
            "\"dropped\": {"
              "\"cycles\": %d"
            "}"
          "},"
          "\"regexps\": {"
            "\"current\": %d"
          "},"
          "\"variables\": {"
            "\"current\": %d"
          "},"
          "\"workers\": {"
              "\"blocked\": %d"
          "},"
          "\"executions\": {"
            "\"total\": %d,"
            "\"unknown\": %d,"
            "\"failed\": %d,"
            "\"gc\": %d"
          "}"
        "}",
        script->state.engines.n,
        script->state.stats.engines.total,
        engines_memory(ctx, script, 1),
        script->state.stats.engines.dropped.cycles,
        script->state.regexps.n,
        script->state.variables.n,
        script->state.stats.workers.blocked,
        script->state.stats.executions.total,
        script->state.stats.executions.unknown,
        script->state.stats.executions.failed,
        script->state.stats.executions.gc);
    Lck_Unlock(&script->state.mutex);
    AZ(pthread_rwlock_unlock(&script->state.regexps.rwlock));
    AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    if (result == NULL) {
        FAIL_WS(ctx, NULL);
    }
    return result;
}

VCL_INT
vmod_script_counter(VRT_CTX, struct vmod_cfg_script *script, VCL_STRING name)
{
    if (strcmp(name, "engines.current") == 0) {
        return script->state.engines.n;
    } else if (strcmp(name, "engines.total") == 0) {
        return script->state.stats.engines.total;
    } else if (strcmp(name, "engines.memory") == 0) {
        return engines_memory(ctx, script, 0);
    } else if (strcmp(name, "engines.dropped.cycles") == 0) {
        return script->state.stats.engines.dropped.cycles;
    } else if (strcmp(name, "regexps.current") == 0) {
        return script->state.regexps.n;
    } else if (strcmp(name, "variables.current") == 0) {
        return script->state.variables.n;
    } else if (strcmp(name, "workers.blocked") == 0) {
        return script->state.stats.workers.blocked;
    } else if (strcmp(name, "executions.total") == 0) {
        return script->state.stats.executions.total;
    } else if (strcmp(name, "executions.unknown") == 0) {
        return script->state.stats.executions.unknown;
    } else if (strcmp(name, "executions.failed") == 0) {
        return script->state.stats.executions.failed;
    } else if (strcmp(name, "executions.gc") == 0) {
        return script->state.stats.executions.gc;
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to fetch counter (script=%s, name=%s)",
            script->name, name);
        return 0;
    }
}
