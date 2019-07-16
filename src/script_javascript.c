#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef JEMALLOC_TCACHE_FLUSH_ENABLED
    #include <jemalloc/jemalloc.h>
#endif

#include "cache/cache.h"
#include "vsb.h"
#include "vre.h"

#include "script_javascript.h"
#include "helpers.h"

static duk_context *new_context(VRT_CTX, struct vmod_cfg_script *script);

/******************************************************************************
 * BASICS.
 *****************************************************************************/

engine_t *
new_javascript_engine(VRT_CTX, struct vmod_cfg_script *script)
{
    AN(script->type == ENGINE_TYPE_JAVASCRIPT);

    engine_t *result = new_engine(script->type, new_context(ctx, script));
    result->memory = get_javascript_engine_used_memory(result);
    return result;
}

int
get_javascript_engine_used_memory(engine_t * engine)
{
    AN(engine->type == ENGINE_TYPE_JAVASCRIPT);

    // See: https://github.com/svaarala/duktape/issues/2130.
    return 0;
}

int
get_javascript_engine_stack_size(engine_t * engine)
{
    AN(engine->type == ENGINE_TYPE_JAVASCRIPT);

    return duk_get_top(engine->ctx.D);
}

static unsigned
pre_execute(
    VRT_CTX, struct vmod_cfg_script * script, engine_t *engine,
    const char *code, const char *name)
{
    AN(script->type == ENGINE_TYPE_JAVASCRIPT);
    AN(engine->type == ENGINE_TYPE_JAVASCRIPT);

    unsigned sucess = 0;

    struct vsb *vsb = VSB_new_auto();
    AN(vsb);
    if (script->enable_sandboxing) {
        AZ(VSB_printf(vsb, "function %s() {\n'use strict';\n%s\n}", name, code));
    } else {
        AZ(VSB_printf(vsb, "function %s() {\n%s\n}", name, code));
    }
    AZ(VSB_finish(vsb));

    if (!duk_peval_lstring(engine->ctx.D, VSB_data(vsb), VSB_len(vsb))) {
        sucess = 1;
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to compile new JavaScript script (script=%s, function=%s, code=%.80s...): %s",
            script->name, name, code, duk_safe_to_string(engine->ctx.D, -1));
    }
    duk_pop(engine->ctx.D);

    VSB_destroy(&vsb);

    return sucess;
}

static unsigned
post_execute(
    VRT_CTX, struct vmod_cfg_script *script, engine_t *engine,
    result_t *result)
{
    #define STORE_NIL(where) \
        where.type = RESULT_VALUE_TYPE_NIL

    #define STORE_BOOLEAN(where) \
        where.type = RESULT_VALUE_TYPE_BOOLEAN; \
        where.value.boolean = (unsigned) duk_to_boolean(engine->ctx.D, -1)

    #define STORE_NUMBER(where) \
        where.type = RESULT_VALUE_TYPE_NUMBER; \
        where.value.number = (double) duk_to_number(engine->ctx.D, -1)

    #define STORE_STRING(where) \
        where.type = RESULT_VALUE_TYPE_STRING; \
        where.value.string = WS_Copy(ctx->ws, duk_safe_to_string(engine->ctx.D, -1), -1); \
        if (where.value.string == NULL) { \
            FAIL_WS(ctx, 0); \
        }

    #define STORE_ERROR(where) \
        where.type = RESULT_VALUE_TYPE_ERROR

    switch (duk_get_type(engine->ctx.D, -1)) {
        case DUK_TYPE_UNDEFINED:
        case DUK_TYPE_NULL:
            STORE_NIL(result->values[0]);
            return 1;

        case DUK_TYPE_BOOLEAN:
            STORE_BOOLEAN(result->values[0]);
            return 1;

        case DUK_TYPE_NUMBER:
            STORE_NUMBER(result->values[0]);
            return 1;

        case DUK_TYPE_STRING:
            STORE_STRING(result->values[0]);
            return 1;

        case DUK_TYPE_OBJECT:
            if (duk_is_array(engine->ctx.D, -1)) {
                int length = duk_get_length(engine->ctx.D, -1);
                for (result->nvalues = 0; result->nvalues < length; result->nvalues++) {
                    if (result->nvalues == MAX_RESULT_VALUES) {
                        LOG(ctx, LOG_ERR,
                            "Failed to store JavaScript object value (script=%s, length=%d, limit=%d)",
                            script->name, length, MAX_RESULT_VALUES);
                        break;
                    }
                    duk_get_prop_index(engine->ctx.D, -1, result->nvalues);
                    switch (duk_get_type(engine->ctx.D, -1)) {
                        case DUK_TYPE_UNDEFINED:
                        case DUK_TYPE_NULL:
                            STORE_NIL(result->values[result->nvalues]);
                            break;
                        case DUK_TYPE_BOOLEAN:
                            STORE_BOOLEAN(result->values[result->nvalues]);
                            break;
                        case DUK_TYPE_NUMBER:
                            STORE_NUMBER(result->values[result->nvalues]);
                            break;
                        case DUK_TYPE_STRING:
                            STORE_STRING(result->values[result->nvalues]);
                            break;
                        case DUK_TYPE_OBJECT:
                            result->values[result->nvalues].type = RESULT_VALUE_TYPE_TABLE;
                            break;
                        default:
                            LOG(ctx, LOG_ERR,
                                "Got invalid JavaScript script result array value (script=%s, index=%d, type=%d)",
                                script->name, result->nvalues, duk_get_type(engine->ctx.D, -1));
                            STORE_ERROR(result->values[result->nvalues]);
                            break;
                    }
                    duk_pop(engine->ctx.D);
                }
            } else {
                LOG(ctx, LOG_ERR,
                    "Got invalid JavaScript script result object value (script=%s, type=%d)",
                    script->name, duk_get_type(engine->ctx.D, -1));
                STORE_ERROR(result->values[result->nvalues]);
            }
            return 1;

        // DUK_TYPE_BUFFER, DUK_TYPE_POINTER,etc.
        default:
            LOG(ctx, LOG_ERR,
                "Got invalid JavaScript script result value (script=%s, type=%d)",
                script->name, duk_get_type(engine->ctx.D, -1));
            STORE_ERROR(result->values[0]);
            return 0;
    }

    #undef STORE_NIL
    #undef STORE_BOOLEAN
    #undef STORE_NUMBER
    #undef STORE_STRING
    #undef STORE_ERROR

    return 0;
}

unsigned execute_javascript(
    VRT_CTX, struct vmod_cfg_script *script, task_state_t *state,
    const char *code, const char **name, int argc, const char *argv[],
    result_t *result, unsigned gc_collect, unsigned flush_jemalloc_tcache)
{
    // Initializations.
    unsigned success = 0;

    // If name (i.e. f_<SHA256(code)>) wasn't provided a new one will be generated
    // here (allocated in the heap) and it will be returned to the caller.
    if (*name == NULL) {
        *name = new_function_name(code);
    }
    AN(*name);

    // Lock a script execution engine.
    engine_t *engine = lock_engine(ctx, script);
    AN(engine);

    // Push 'varnish' object into the stack.
    duk_get_global_string(engine->ctx.D, "varnish");
    AN(duk_is_object(engine->ctx.D, -1));

    // Try to lookup the function to be executed. Result will be pushed
    // into the stack.
    duk_get_global_string(engine->ctx.D, *name);
    if (!duk_is_function(engine->ctx.D, -1)) {
        // Remove the non-function value from the stack.
        duk_pop(engine->ctx.D);

        // Update stats.
        Lck_Lock(&script->state.mutex);
        script->state.stats.executions.unknown++;
        Lck_Unlock(&script->state.mutex);

        // Compile & register the function to be executed.
        if (pre_execute(ctx, script, engine, code, *name)) {
            // Push the function to be executed into the stack.
            duk_get_global_string(engine->ctx.D, *name);
            AN(duk_is_function(engine->ctx.D, -1));
        } else {
            // Stop execution.
            goto done;
        }
    }

    // Current state of the stack at this point (top to bottom):
    //   - Function to be executed.
    //   - 'varnish' object.

    if (result != NULL) {
        // Assertions.
        AN(state);
        AN(argv);
        AN(result);

        // Execute 'varnish._ctx = ctx', 'varnish._script = script' and
        // 'varnish._state = state'.
        duk_push_pointer(engine->ctx.D, (struct vrt_ctx *) ctx);
        duk_put_prop_string(engine->ctx.D, -3, DUK_HIDDEN_SYMBOL("_ctx"));
        duk_push_pointer(engine->ctx.D, (struct vmod_cfg_script *) script);
        duk_put_prop_string(engine->ctx.D, -3, DUK_HIDDEN_SYMBOL("_script"));
        duk_push_pointer(engine->ctx.D, (task_state_t *) state);
        duk_put_prop_string(engine->ctx.D, -3, DUK_HIDDEN_SYMBOL("_state"));

        // Populate 'ARGV' array accordingly to the input arguments.
        duk_idx_t idx = duk_push_array(engine->ctx.D);
        for (int i = 0; i < argc; i++) {
            duk_push_string(engine->ctx.D, argv[i]);
            duk_put_prop_index(engine->ctx.D, idx, i);
        }
        duk_put_global_string(engine->ctx.D, "ARGV");

        // At this point whether the function was never seen before or if it was
        // already defined, we can call it. We have zero arguments and expect
        // a single return value.
        if (duk_pcall(engine->ctx.D, 0) == DUK_EXEC_SUCCESS) {
            if (post_execute(ctx, script, engine, result)) {
                success = 1;
            } else {
                result->values[0].type = RESULT_VALUE_TYPE_ERROR;
                LOG(ctx, LOG_ERR,
                    "Failed to process JavaScript script result (script=%s, function=%s, code=%.80s...)",
                    script->name, *name, code);

            }
        } else {
            result->values[0].type = RESULT_VALUE_TYPE_ERROR;
            LOG(ctx, LOG_ERR,
                "Failed to execute JavaScript script (script=%s, function=%s, code=%.80s...): %s",
                script->name, *name, code, duk_safe_to_string(engine->ctx.D, -1));
        }

        // Remove the function result from the stack.
        duk_pop(engine->ctx.D);

        // Execute 'varnish._ctx = nil', 'varnish._script = nil' and
        // 'varnish._state = nil'.
        duk_push_null(engine->ctx.D);
        duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_ctx"));
        duk_push_null(engine->ctx.D);
        duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_script"));
        duk_push_null(engine->ctx.D);
        duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_state"));
    } else {
        // Everything looks correct. Full execution is not required: simply
        // remove function to be executed from the stack.
        success = 1;
        duk_pop(engine->ctx.D);
    }

done:
    // Current state of the stack at this point (top to bottom):
    //   - 'varnish' object.

    // Remove 'varnish' object from the stack.
    duk_pop(engine->ctx.D);

    // Update stats.
    Lck_Lock(&script->state.mutex);
    script->state.stats.executions.total++;
    if (!success) {
        script->state.stats.executions.failed++;
    }
    Lck_Unlock(&script->state.mutex);

    // Call the garbage collector from time to time.
    engine->ncycles++;
    if (gc_collect || engine->ncycles % script->min_gc_cycles == 0) {
        duk_gc(engine->ctx.D, DUK_GC_COMPACT);
        duk_gc(engine->ctx.D, DUK_GC_COMPACT);
        Lck_Lock(&script->state.mutex);
        script->state.stats.executions.gc++;
        Lck_Unlock(&script->state.mutex);
    }

    // Flush calling thread's jemalloc tcache in order to keep memory usage
    // controlled.
#ifdef JEMALLOC_TCACHE_FLUSH_ENABLED
    if (flush_jemalloc_tcache) {
        AZ(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0));
    }
#endif

    // Unlock script execution engine.
    unlock_engine(ctx, script, engine);

    // Done!
    return success;
}

/******************************************************************************
 * VARNISH.* COMMANDS.
 *****************************************************************************/

// Extract field from 'varnish.field'. Both 'varnish' table and user data are
// pushed into the stack and the removed.
#define GET_VARNISH_OBJECT_FOO_FIELD(D, field, where, MAGIC) \
    do { \
        duk_get_global_string(D, "varnish"); \
        AN(duk_is_object(D, -1)); \
        duk_get_prop_string(D, -1, DUK_HIDDEN_SYMBOL(field)); \
        AN(duk_is_pointer(D, -1)); \
        void *data = duk_get_pointer(D, -1); \
        AN(data); \
        CAST_OBJ_NOTNULL(where, data, MAGIC); \
        duk_pop_2(D); \
    } while (0)

#define GET_VARNISH_OBJECT_CTX(D, where) \
    GET_VARNISH_OBJECT_FOO_FIELD(D, "_ctx", where, VRT_CTX_MAGIC)

#define GET_VARNISH_OBJECT_SCRIPT(D, where) \
    GET_VARNISH_OBJECT_FOO_FIELD(D, "_script", where, VMOD_CFG_SCRIPT_MAGIC)

#define GET_VARNISH_OBJECT_STATE(D, where) \
    GET_VARNISH_OBJECT_FOO_FIELD(D, "_state", where, TASK_STATE_MAGIC)

static duk_ret_t
varnish_log_javascript_command(duk_context *D)
{
    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc != 1) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.log() requires one argument.");
    }
    const char *message = duk_safe_to_string(D, -1);

    // Check input arguments.
    if (message != NULL) {
        // Execute 'ctx = varnish._ctx'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);

        // Execute command.
        varnish_log_command(ctx, message);
    }

    // Done!
    return 0;
}

static duk_ret_t
varnish_get_header_javascript_command(duk_context *D)
{
    // Initializations.
    const char *result = NULL;

    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 1) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.get_header() requires one argument.");
    }
    const char *name = duk_safe_to_string(D, -1 * argc);
    const char *where;
    if (argc >= 2) {
        where = duk_safe_to_string(D, -1 * argc + 1);
    } else {
        where = "req";
    }
    // Check input arguments.
    if (name != NULL && strlen(name) > 0) {
        // Execute 'ctx = varnish._ctx'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);

        // Execute command.
        const char *error;
        result = varnish_get_header_command(ctx, name, where, &error);
        if (error != NULL) {
            (void) duk_error(D, DUK_ERR_TYPE_ERROR, error);
        }
    }

    // Done!
    duk_push_string(D, result);
    return 1;
}

static duk_ret_t
varnish_set_header_javascript_command(duk_context *D)
{
    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 2) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.set_header() requires two arguments.");
    }
    const char *name = duk_safe_to_string(D, -1 * argc);
    const char *value = duk_safe_to_string(D, -1 * argc + 1);
    const char *where;
    if (argc >= 3) {
        where = duk_safe_to_string(D, -1 * argc + 2);
    } else {
        where = "req";
    }

    // Check input arguments.
    if (name != NULL && strlen(name) > 0 &&
        value != NULL && strlen(value) > 0) {
        // Execute 'ctx = varnish._ctx'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);

        // Execute command.
        const char *error;
        varnish_set_header_command(ctx, name, value, where, &error);
        if (error != NULL) {
            (void) duk_error(D, DUK_ERR_TYPE_ERROR, error);
        }
    }

    // Done!
    return 0;
}

static duk_ret_t
varnish_regmatch_javascript_command(duk_context *D)
{
    // Initializations.
    unsigned result = 0;

    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 2) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.regmatch() requires two arguments.");
    }
    const char *string = duk_safe_to_string(D, -1 * argc);
    const char *regexp = duk_safe_to_string(D, -1 * argc + 1);
    unsigned cache;
    if (argc >= 3) {
        cache = duk_to_boolean(D, -1 * argc + 2);
    } else {
        cache = 1;
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL) {
        // Execute 'ctx = varnish._ctx' & 'script = varnish._script'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_OBJECT_SCRIPT(D, script);

        // Execute command.
        const char *error;
        result = varnish_regmatch_command(ctx, script, string, regexp, cache, &error);
        if (error != NULL) {
            (void) duk_error(D, DUK_ERR_TYPE_ERROR, error);
        }
    }

    // Done!
    duk_push_boolean(D, result);
    return 1;
}

static int
varnish_regsub_javascript_command(duk_context *D, unsigned all)
{
    // Initializations.
    const char *result = NULL;

    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 3) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.regsub() & varnish.regsuball() require three arguments.");
    }
    const char *string = duk_safe_to_string(D, -1 * argc);
    const char *regexp = duk_safe_to_string(D, -1 * argc + 1);
    const char *sub = duk_safe_to_string(D, -1 * argc + 2);
    unsigned cache;
    if (argc >= 4) {
        cache = duk_to_boolean(D, -1 * argc + 3);
    } else {
        cache = 1;
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL && sub != NULL) {
        // Execute 'ctx = varnish._ctx' & 'script = varnish._script'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_OBJECT_SCRIPT(D, script);

        // Execute command.
        const char *error;
        result = varnish_regsub_command(
            ctx, script, string, regexp, sub, cache, all, &error);
        if (error != NULL) {
            (void) duk_error(D, DUK_ERR_TYPE_ERROR, error);
        }
    }

    // Done!
    duk_push_string(D, result);
    return 1;
}

static duk_ret_t
varnish_regsubone_javascript_command(duk_context *D)
{
    return varnish_regsub_javascript_command(D, 0);
}

static duk_ret_t
varnish_regsuball_javascript_command(duk_context *D)
{
    return varnish_regsub_javascript_command(D, 1);
}

/******************************************************************************
 * VARNISH.SHARED.* COMMANDS.
 *****************************************************************************/

// Extract value from 'varnish.shared._is_locked'.
#define GET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, where) \
    do { \
        duk_get_global_string(D, "varnish"); \
        AN(duk_is_object(D, -1)); \
        duk_get_prop_string(D, -1, "shared"); \
        AN(duk_is_object(D, -1)); \
        duk_get_prop_string(D, -1, DUK_HIDDEN_SYMBOL("_is_locked")); \
        AN(duk_is_boolean(D, -1)); \
        where = duk_get_boolean(D, -1); \
        duk_pop_3(D); \
    } while (0)

// Update value in 'varnish.shared._is_locked'.
#define SET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, value) \
    do { \
        duk_get_global_string(D, "varnish"); \
        AN(duk_is_object(D, -1)); \
        duk_get_prop_string(D, -1, "shared"); \
        AN(duk_is_object(D, -1)); \
        duk_push_boolean (D, value); \
        duk_put_prop_string(D, -2, DUK_HIDDEN_SYMBOL("_is_locked")); \
        duk_pop_2(D); \
    } while (0)

static duk_ret_t
varnish_shared_get_javascript_command(duk_context *D)
{
    // Initializations.
    const char *result = NULL;

    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 1) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.shared.get() requires one argument.");
    }
    const char *key = duk_safe_to_string(D, -1 * argc);
    const char *scope;
    if (argc >= 2) {
        scope = duk_safe_to_string(D, -1 * argc + 1);
    } else {
        scope = "all";
    }

    // Check input arguments.
    if (key != NULL && strlen(key) > 0) {
        // Execute 'is_locked = varnish.shared._is_locked'.
        unsigned is_locked;
        GET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, is_locked);

        // Execute 'ctx = varnish._ctx', 'script = varnish._script' &
        // 'state = varnish._state'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_OBJECT_SCRIPT(D, script);
        task_state_t *state;
        GET_VARNISH_OBJECT_STATE(D, state);

        // Execute command.
        result = varnish_shared_get_command(ctx, script, state, key, scope, is_locked);
    }

    // Done!
    duk_push_string(D, result);
    return 1;
}

static duk_ret_t
varnish_shared_set_javascript_command(duk_context *D)
{
    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 2) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.shared.set() requires two arguments.");
    }
    const char *key = duk_safe_to_string(D, -1 * argc);
    const char *value = duk_safe_to_string(D, -1 * argc + 1);
    const char *scope;
    if (argc >= 3) {
        scope = duk_safe_to_string(D, -1 * argc + 2);
    } else {
        scope = "task";
    }

    // Check input arguments.
    if (key != NULL && strlen(key) > 0 &&
        value != NULL && strlen(value) > 0) {
        // Execute 'is_locked = varnish.shared._is_locked'.
        unsigned is_locked;
        GET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, is_locked);

        // Execute 'ctx = varnish._ctx', 'script = varnish._script' &
        // 'state = varnish._state'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_OBJECT_SCRIPT(D, script);
        task_state_t *state;
        GET_VARNISH_OBJECT_STATE(D, state);

        // Execute command.
        varnish_shared_set_command(ctx, script, state, key, value, scope, is_locked);
    }

    // Done!
    return 0;
}

static duk_ret_t
varnish_shared_unset_javascript_command(duk_context *D)
{
    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 1) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.shared.unset() requires one argument.");
    }
    const char *key = duk_safe_to_string(D, -1 * argc);
    const char *scope;
    if (argc >= 2) {
        scope = duk_safe_to_string(D, -1 * argc + 1);
    } else {
        scope = "all";
    }

    // Check input arguments.
    if (key != NULL && strlen(key) > 0) {
        // Execute 'is_locked = varnish.shared._is_locked'.
        unsigned is_locked;
        GET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, is_locked);

        // Execute 'ctx = varnish._ctx', 'script = varnish._script' &
        // 'state = varnish._state'.
        VRT_CTX;
        GET_VARNISH_OBJECT_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_OBJECT_SCRIPT(D, script);
        task_state_t *state;
        GET_VARNISH_OBJECT_STATE(D, state);

        // Execute command.
        varnish_shared_unset_command(ctx, script, state, key, scope, is_locked);
    }

    // Done!
    return 0;
}

static duk_ret_t
varnish_shared_eval_javascript_command(duk_context *D)
{
    // Check input arguments.
    int argc = duk_get_top(D);
    if (argc != 1) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.shared.eval() requires one argument.");
    }
    if (!duk_is_function(D, -1)) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.shared.eval() requires a function argument.");
    }

    // Execute 'is_locked = varnish.shared._is_locked'.
    unsigned is_locked;
    GET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, is_locked);

    // Execute 'script = varnish._script'.
    struct vmod_cfg_script *script;
    GET_VARNISH_OBJECT_SCRIPT(D, script);

    // Get lock if needed.
    if (!is_locked) {
        AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
        SET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, 1);
    }

    // Execute function and leave result or error message on top
    // of the stack.
    unsigned error = duk_pcall(D, 0) != DUK_EXEC_SUCCESS;

    // Release lock if needed.
    if (!is_locked) {
        SET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD(D, 0);
        AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    }

    // Done!
    if (error) {
        (void) duk_throw(D);
    }
    return 1;
}

static const char *varnish_shared_incr_javascript_command =
    "varnish.shared.incr = function(key, increment, scope) {\n"
    "  var key = key;\n"
    "  var increment = parseInt(increment)\n"
    "  if (isNaN(increment)) {\n"
    "    increment = 0;\n"
    "  }\n"
    "  \n"
    "  return varnish.shared.eval(function() {\n"
    "    var value = parseInt(varnish.shared.get(key, scope));\n"
    "    if (isNaN(value)) {\n"
    "      value = increment;\n"
    "    } else {\n"
    "      value += increment;\n"
    "    }\n"
    "    \n"
    "    varnish.shared.set(key, value, scope);\n"
    "    return value;\n"
    "  })\n"
    "}\n";

#undef GET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD
#undef SET_VARNISH_SHARED_OBJECT_IS_LOCKED_FIELD

#undef GET_VARNISH_OBJECT_FOO_FIELD
#undef GET_VARNISH_OBJECT_CTX
#undef GET_VARNISH_OBJECT_SCRIPT
#undef GET_VARNISH_OBJECT_STATE

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static duk_context *
new_context(VRT_CTX, struct vmod_cfg_script *script)
{
    // Create base context.
    duk_context *result = duk_create_heap_default();
    AN(result);

    // Add support for varnish.engine, varnish.shared, varnish._ctx,
    // varnish._script, varnish.log(), etc.
    duk_idx_t idx = duk_push_object(result);
    duk_push_object(result);
    duk_put_prop_string(result, idx, "engine");
    duk_push_object(result);
    duk_put_prop_string(result, idx, "shared");
    duk_push_c_function(result, varnish_log_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, idx, "log");
    duk_push_c_function(result, varnish_get_header_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, idx, "get_header");
    duk_push_c_function(result, varnish_set_header_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, idx, "set_header");
    duk_push_c_function(result, varnish_regmatch_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, idx, "regmatch");
    duk_push_c_function(result, varnish_regsubone_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, idx, "regsub");
    duk_push_c_function(result, varnish_regsuball_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, idx, "regsuball");
    duk_put_global_string(result, "varnish");

    // Add support for varnish.shared.* commands.
    duk_get_global_string(result, "varnish");
    AN(duk_is_object(result, -1));
    duk_get_prop_string(result, -1, "shared");
    AN(duk_is_object(result, -1));
    duk_push_boolean(result, 0);
    duk_put_prop_string(result, -2, DUK_HIDDEN_SYMBOL("_is_locked"));
    duk_push_c_function(result, varnish_shared_get_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "get");
    duk_push_c_function(result, varnish_shared_set_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "set");
    duk_push_c_function(result, varnish_shared_unset_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "unset");
    duk_push_c_function(result, varnish_shared_eval_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "eval");
    duk_pop_2(result);
    AZ(duk_peval_string(result, varnish_shared_incr_javascript_command));
    duk_pop(result);

    // Done!
    return result;
}
