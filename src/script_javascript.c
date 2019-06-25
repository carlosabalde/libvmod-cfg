#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef JEMALLOC_TCACHE_FLUSH_ENABLED
    #include <jemalloc/jemalloc.h>
#endif

#include "vcl.h"
#include "vrt.h"
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
    return new_engine(ENGINE_TYPE_JAVASCRIPT, new_context(ctx, script));
}

int
get_used_javascript_engine_memory(engine_t * engine)
{
    return 0;
}

static unsigned
pre_execute(
    VRT_CTX, struct vmod_cfg_script * script, engine_t *engine,
    const char *code, const char *name)
{
    unsigned sucess = 0;

    struct vsb *vsb = VSB_new_auto();
    AN(vsb);
    AZ(VSB_printf(vsb, "function %s() {\n%s\n}", name, code));
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
        AN(where.value.string);

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
            result->nvalues = 0;
            while (1) {
                if (!duk_get_prop_index(engine->ctx.D, -1, result->nvalues)) {
                    duk_pop(engine->ctx.D);
                    break;
                }
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
                            "Got invalid JavaScript script result object value (script=%s, type=%d)",
                            script->name, duk_get_type(engine->ctx.D, -1));
                        STORE_ERROR(result->values[result->nvalues]);
                        break;

                }
                duk_pop(engine->ctx.D);
                result->nvalues++;
                if (result->nvalues == MAX_RESULT_VALUES) {
                    LOG(ctx, LOG_ERR,
                        "Failed to store JavaScript object value (script=%s, limit=%d)",
                        script->name, MAX_RESULT_VALUES);
                    break;
                }
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
    VRT_CTX, struct vmod_cfg_script *script, const char *code, const char **name,
    int argc, const char *argv[], result_t *result, unsigned gc_collect,
    unsigned flush_jemalloc_tcache)
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

    // Push 'varnish' object and Varnish context into the stack. Then execute
    // 'varnish._ctx = ctx', keeping the 'varnish' object in the top of the
    // stack. Same for 'varnish._script = script'.
    duk_get_global_string(engine->ctx.D, "varnish");
    AN(duk_is_object(engine->ctx.D, -1));
    duk_push_pointer(engine->ctx.D, (struct vrt_ctx *) ctx);
    duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_ctx"));
    duk_push_pointer(engine->ctx.D, (struct vmod_cfg_script *) script);
    duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_script"));

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
        AN(argv);
        AN(result);

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
    } else {
        // Everything looks correct. Full execution is not required: simply
        // remove function to be executed from the stack.
        success = 1;
        duk_pop(engine->ctx.D);
    }

done:
    // Current state of the stack at this point (top to bottom):
    //   - 'varnish' object.

    // Execute 'varnish._ctx = null' and 'varnish._script = null', then
    // remove 'varnish' object from the stack.
    duk_push_null(engine->ctx.D);
    duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_ctx"));
    duk_push_null(engine->ctx.D);
    duk_put_prop_string(engine->ctx.D, -2, DUK_HIDDEN_SYMBOL("_script"));
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
 * HELPERS.
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

static duk_ret_t
varnish_log_javascript_command(duk_context *D)
{
    // Extract input arguments.
    int argc = duk_get_top(D);
    if (argc < 1) {
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

#undef GET_VARNISH_OBJECT_FOO_FIELD
#undef GET_VARNISH_OBJECT_CTX
#undef GET_VARNISH_OBJECT_SCRIPT

static duk_context *
new_context(VRT_CTX, struct vmod_cfg_script *script)
{
    // Create base context.
    // Beware unlike Lua, no sandboxing at all is enforced for Duktape engines.
    duk_context *result = duk_create_heap_default();
    AN(result);

    // Add support for varnish._ctx, varnish._script, varnish.log(), etc.
    duk_idx_t idx = duk_push_array(result);
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

    // Done!
    return result;
}
