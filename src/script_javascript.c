#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if HAVE_DECL_VRT_FLUSHTHREADCACHE == 0
#ifdef JEMALLOC_TCACHE_FLUSH_ENABLED
    #include <jemalloc/jemalloc.h>
#endif
#endif

#include "cache/cache.h"
#include "vsb.h"
#include "vre.h"

#include "script_javascript.h"
#include "script_javascript_helpers.h"
#include "helpers.h"

static duk_context *new_context(VRT_CTX, struct vmod_cfg_script *script);

// Keys used to store per-engine values in the Duktape heap stash, which is not
// reachable from scripts:
//   - The 'ctx', 'script' & 'state' pointers, set by 'execute()' around each
//     execution and used by the varnish.* commands. Storing them in the stash
//     instead of as hidden properties of the global 'varnish' object saves a
//     global lookup per command call and, more importantly, decouples the
//     commands from the 'varnish' global binding: a script clobbering it now
//     gets a regular execution error instead of an assertion failure in the
//     next command.
//   - The 'is_locked' flag used by the varnish.shared.* commands, for the same
//     reasons.
//   - The per-engine regexp cache object, mapping pattern strings to 'vre_t'
//     pointers borrowed from the script-wide regexp cache. It lets the
//     varnish.reg*() commands resolve compiled regexps without touching the
//     shared cache (and its rwlock) on every call; see 'get_regexp()'.
#define STASH_KEY_CTX "varnish._ctx"
#define STASH_KEY_SCRIPT "varnish._script"
#define STASH_KEY_STATE "varnish._state"
#define STASH_KEY_SHARED_IS_LOCKED "varnish.shared._is_locked"
#define STASH_KEY_REGEXP_CACHE "varnish.regexps"

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
    unsigned unknown = 0;
    unsigned gc = 0;

    // If name (i.e. f_<SHA256(code)>) wasn't provided a new one will be generated
    // here (allocated in the heap) and it will be returned to the caller.
    if (*name == NULL) {
        *name = new_function_name(code);
    }
    AN(*name);

    // Lock a script execution engine.
    engine_t *engine = lock_engine(ctx, script);
    AN(engine);

    // Try to lookup the function to be executed. Result will be pushed
    // into the stack.
    duk_get_global_string(engine->ctx.D, *name);
    if (!duk_is_function(engine->ctx.D, -1)) {
        // Remove the non-function value from the stack.
        duk_pop(engine->ctx.D);
        unknown = 1;

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

    if (result != NULL) {
        // Assertions.
        AN(state);
        AN(argv);
        AN(result);

        // Store the 'ctx', 'script' & 'state' pointers in the heap stash (see
        // STASH_KEY_CTX, etc.). They are used by the varnish.* commands.
        duk_push_heap_stash(engine->ctx.D);
        duk_push_pointer(engine->ctx.D, TRUST_ME(ctx));
        duk_put_prop_string(engine->ctx.D, -2, STASH_KEY_CTX);
        duk_push_pointer(engine->ctx.D, script);
        duk_put_prop_string(engine->ctx.D, -2, STASH_KEY_SCRIPT);
        duk_push_pointer(engine->ctx.D, state);
        duk_put_prop_string(engine->ctx.D, -2, STASH_KEY_STATE);
        duk_pop(engine->ctx.D);

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

        // Clear the pointers stored in the heap stash: the engine is about to
        // be released and they must not outlive this execution (a varnish.*
        // command executed outside an execution must fail loudly instead of
        // silently using stale pointers).
        duk_push_heap_stash(engine->ctx.D);
        duk_push_null(engine->ctx.D);
        duk_put_prop_string(engine->ctx.D, -2, STASH_KEY_CTX);
        duk_push_null(engine->ctx.D);
        duk_put_prop_string(engine->ctx.D, -2, STASH_KEY_SCRIPT);
        duk_push_null(engine->ctx.D);
        duk_put_prop_string(engine->ctx.D, -2, STASH_KEY_STATE);
        duk_pop(engine->ctx.D);
    } else {
        // Everything looks correct. Full execution is not required: simply
        // remove function to be executed from the stack.
        success = 1;
        duk_pop(engine->ctx.D);
    }

done:
    // Call the garbage collector from time to time.
    engine->ncycles++;
    if (gc_collect || engine->ncycles % script->min_gc_cycles == 0) {
        duk_gc(engine->ctx.D, DUK_GC_COMPACT);
        duk_gc(engine->ctx.D, DUK_GC_COMPACT);
        gc = 1;
    }

    // Flush calling thread's jemalloc tcache in order to keep memory usage
    // controlled.
#ifdef JEMALLOC_TCACHE_FLUSH_ENABLED
    if (flush_jemalloc_tcache) {
#if HAVE_DECL_VRT_FLUSHTHREADCACHE == 1
        VRT_FlushThreadCache();
#else
        AZ(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0));
#endif
    }
#endif

    // Release script execution engine & update stats.
    release_engine(ctx, script, engine, unknown, success, gc);

    // Done!
    return success;
}

/******************************************************************************
 * VARNISH.* COMMANDS.
 *****************************************************************************/

// Extract a pointer stored in the heap stash (see STASH_KEY_CTX, etc.). Both
// the stash and the pointer are pushed into the stack and then removed.
#define GET_STASH_FOO_FIELD(D, key, where, MAGIC) \
    do { \
        duk_push_heap_stash(D); \
        duk_get_prop_string(D, -1, key); \
        AN(duk_is_pointer(D, -1)); \
        void *data = duk_get_pointer(D, -1); \
        AN(data); \
        CAST_OBJ_NOTNULL(where, data, MAGIC); \
        duk_pop_2(D); \
    } while (0)

#define GET_STASH_CTX(D, where) \
    GET_STASH_FOO_FIELD(D, STASH_KEY_CTX, where, VRT_CTX_MAGIC)

#define GET_STASH_SCRIPT(D, where) \
    GET_STASH_FOO_FIELD(D, STASH_KEY_SCRIPT, where, VMOD_CFG_SCRIPT_MAGIC)

#define GET_STASH_STATE(D, where) \
    GET_STASH_FOO_FIELD(D, STASH_KEY_STATE, where, TASK_STATE_MAGIC)

/*
 * Resolve the compiled regexp for the pattern at stack index 'regexp_idx'
 * ('regexp' being its C string), going through the per-engine cache object
 * stored in the heap stash (see STASH_KEY_REGEXP_CACHE). A hit is a cheap
 * property lookup on memory owned by this engine: no shared state (i.e. the
 * script-wide regexp cache and its rwlock) is touched. The object is keyed
 * with the pattern argument itself ('duk_dup()' of its stack index): Duktape
 * interns every string, so this reuses the already hashed string instead of
 * 'duk_push_string()' rediscovering it (strlen, hash and memcmp over the
 * pattern). Beware the cache is a bare object (no prototype) on purpose: a
 * regular object would resolve patterns spelled like inherited property names
 * ('constructor', 'toString', ...) through 'Object.prototype', and
 * '__proto__' would even reach the prototype setter.
 *
 * On a miss the pattern is resolved through the shared cache ('init_regexp()',
 * compiling and registering it if needed) and the resulting pointer is
 * remembered for the lifetime of this engine. Borrowing the pointer is safe
 * because entries in the shared cache are never destroyed until the whole
 * script instance is destroyed, after all its engines (see the note next to
 * the cache declaration).
 *
 * Uncached lookups (i.e. 'cache' disabled) bypass both caches, preserving the
 * compile-use-free behavior: the caller owns the returned regexp.
 */
static vre_t *
get_regexp(
    duk_context *D, duk_idx_t regexp_idx, VRT_CTX,
    struct vmod_cfg_script *script, const char *regexp, unsigned cache)
{
    vre_t *result;

    if (!cache) {
        return init_regexp(ctx, script, regexp, 0);
    }

    duk_push_heap_stash(D);
    duk_get_prop_string(D, -1, STASH_KEY_REGEXP_CACHE);
    AN(duk_is_object(D, -1));
    duk_dup(D, regexp_idx);
    duk_get_prop(D, -2);
    if (duk_is_pointer(D, -1)) {
        result = duk_get_pointer(D, -1);
        AN(result);
        duk_pop_3(D);
        return result;
    }
    duk_pop(D);

    result = init_regexp(ctx, script, regexp, 1);
    if (result != NULL) {
        duk_dup(D, regexp_idx);
        duk_push_pointer(D, result);
        duk_put_prop(D, -3);
    }
    duk_pop_2(D);

    return result;
}

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
    const char *message = duk_to_string(D, 0);

    // Check input arguments.
    if (message != NULL) {
        // Extract 'ctx' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);

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
    const char *name = duk_to_string(D, 0);
    const char *where = NULL;
    if (argc >= 2 && !duk_is_undefined(D, 1)) {
        where = duk_to_string(D, 1);
    }
    where = where ? where : "req";

    // Check input arguments.
    if (name != NULL && strlen(name) > 0) {
        // Extract 'ctx' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);

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
    const char *name = duk_to_string(D, 0);
    const char *value = duk_to_string(D, 1);
    const char *where = NULL;
    if (argc >= 3 && !duk_is_undefined(D, 2)) {
        where = duk_to_string(D, 2);
    }
    where = where ? where : "req";

    // Check input arguments.
    if (name != NULL && strlen(name) > 0 &&
        value != NULL && strlen(value) > 0) {
        // Extract 'ctx' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);

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
    const char *string = duk_to_string(D, 0);
    duk_size_t regexp_len;
    const char *regexp = duk_to_lstring(D, 1, &regexp_len);
    unsigned cache;
    if (argc >= 3) {
        cache = duk_to_boolean(D, 2);
    } else {
        cache = 1;
    }

    // Reject patterns with embedded NUL bytes. Patterns are compiled as C
    // strings and the shared regexp cache keys on the same prefix, so such a
    // pattern would quietly behave as its pre-NUL prefix ('a\0x' and 'a\0y'
    // would both compile to /a/), while the per-engine cache would key on the
    // whole JavaScript string. A NUL byte in a pattern is always a script bug:
    // fail loudly, like a pattern that fails to compile. Beware this runs
    // before any C resource is acquired, so raising is longjmp-safe.
    if (regexp != NULL && strlen(regexp) != regexp_len) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.regmatch() pattern contains a NUL byte.");
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL) {
        // Extract 'ctx' & 'script' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_STASH_SCRIPT(D, script);

        // Resolve the compiled regexp (the pattern is the second argument)
        // & execute command.
        vre_t *re = get_regexp(D, 1, ctx, script, regexp, cache);
        if (re != NULL) {
            result = varnish_regmatch_re_command(ctx, string, re);
            if (!cache) {
                VRE_free(&re);
            }
        } else {
            const char *error = regexp_error(ctx, regexp);
            if (error != NULL) {
                (void) duk_error(D, DUK_ERR_TYPE_ERROR, error);
            }
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
    const char *string = duk_to_string(D, 0);
    duk_size_t regexp_len;
    const char *regexp = duk_to_lstring(D, 1, &regexp_len);
    const char *sub = duk_to_string(D, 2);
    unsigned cache;
    if (argc >= 4) {
        cache = duk_to_boolean(D, 3);
    } else {
        cache = 1;
    }

    // Reject patterns with embedded NUL bytes. Patterns are compiled as C
    // strings and the shared regexp cache keys on the same prefix, so such a
    // pattern would quietly behave as its pre-NUL prefix ('a\0x' and 'a\0y'
    // would both compile to /a/), while the per-engine cache would key on the
    // whole JavaScript string. A NUL byte in a pattern is always a script bug:
    // fail loudly, like a pattern that fails to compile. Beware this runs
    // before any C resource is acquired, so raising is longjmp-safe.
    if (regexp != NULL && strlen(regexp) != regexp_len) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.regsub() & varnish.regsuball() pattern contains a NUL byte.");
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL && sub != NULL) {
        // Extract 'ctx' & 'script' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_STASH_SCRIPT(D, script);

        // Resolve the compiled regexp (the pattern is the second argument)
        // & execute command.
        vre_t *re = get_regexp(D, 1, ctx, script, regexp, cache);
        if (re != NULL) {
            result = varnish_regsub_re_command(ctx, string, re, sub, all);
            if (!cache) {
                VRE_free(&re);
            }
        } else {
            const char *error = regexp_error(ctx, regexp);
            if (error != NULL) {
                (void) duk_error(D, DUK_ERR_TYPE_ERROR, error);
            }
        }
    }

    // Done! On no match 'VRT_regsub()' returns the subject itself: push a copy
    // of argument 0 instead of pushing the C string again, which would
    // re-intern it (Duktape interns every string: hash, string table probe and
    // a full memcmp against the very string already held). Beware argument 0
    // is guaranteed to be a string at this point: it either was one, or
    // 'duk_to_string()' coerced it in place. The match path is unchanged: the
    // result is a new workspace string and has to be interned anyway.
    if (result != NULL && result == string) {
        duk_dup(D, 0);
    } else {
        duk_push_string(D, result);
    }
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

// Extract the 'is_locked' flag stored in the heap stash (see
// STASH_KEY_SHARED_IS_LOCKED).
#define GET_STASH_SHARED_IS_LOCKED(D, where) \
    do { \
        duk_push_heap_stash(D); \
        duk_get_prop_string(D, -1, STASH_KEY_SHARED_IS_LOCKED); \
        AN(duk_is_boolean(D, -1)); \
        where = duk_get_boolean(D, -1); \
        duk_pop_2(D); \
    } while (0)

// Update the 'is_locked' flag stored in the heap stash.
#define SET_STASH_SHARED_IS_LOCKED(D, value) \
    do { \
        duk_push_heap_stash(D); \
        duk_push_boolean(D, value); \
        duk_put_prop_string(D, -2, STASH_KEY_SHARED_IS_LOCKED); \
        duk_pop(D); \
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
    const char *key = duk_to_string(D, 0);
    const char *scope = NULL;
    if (argc >= 2 && !duk_is_undefined(D, 1)) {
        scope = duk_to_string(D, 1);
    }
    scope = scope ? scope : "all";

    // Check input arguments.
    if (key != NULL && strlen(key) > 0) {
        // Extract 'is_locked' from the heap stash.
        unsigned is_locked;
        GET_STASH_SHARED_IS_LOCKED(D, is_locked);

        // Extract 'ctx', 'script' & 'state' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_STASH_SCRIPT(D, script);
        task_state_t *state;
        GET_STASH_STATE(D, state);

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
    const char *key = duk_to_string(D, 0);
    const char *value = duk_to_string(D, 1);
    const char *scope = NULL;
    if (argc >= 3 && !duk_is_undefined(D, 2)) {
        scope = duk_to_string(D, 2);
    }
    scope = scope ? scope : "task";

    // Check input arguments.
    if (key != NULL && strlen(key) > 0 &&
        value != NULL) {
        // Extract 'is_locked' from the heap stash.
        unsigned is_locked;
        GET_STASH_SHARED_IS_LOCKED(D, is_locked);

        // Extract 'ctx', 'script' & 'state' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_STASH_SCRIPT(D, script);
        task_state_t *state;
        GET_STASH_STATE(D, state);

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
    const char *key = duk_to_string(D, 0);
    const char *scope = NULL;
    if (argc >= 2 && !duk_is_undefined(D, 1)) {
        scope = duk_to_string(D, 1);
    }
    scope = scope ? scope : "all";

    // Check input arguments.
    if (key != NULL && strlen(key) > 0) {
        // Extract 'is_locked' from the heap stash.
        unsigned is_locked;
        GET_STASH_SHARED_IS_LOCKED(D, is_locked);

        // Extract 'ctx', 'script' & 'state' from the heap stash.
        VRT_CTX;
        GET_STASH_CTX(D, ctx);
        struct vmod_cfg_script *script;
        GET_STASH_SCRIPT(D, script);
        task_state_t *state;
        GET_STASH_STATE(D, state);

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
    if (!duk_is_function(D, 0)) {
        (void) duk_error(
            D,
            DUK_ERR_TYPE_ERROR,
            "varnish.shared.eval() requires a function argument.");
    }

    // Extract 'is_locked' from the heap stash.
    unsigned is_locked;
    GET_STASH_SHARED_IS_LOCKED(D, is_locked);

    // Extract 'script' from the heap stash.
    struct vmod_cfg_script *script;
    GET_STASH_SCRIPT(D, script);

    // Get lock if needed.
    if (!is_locked) {
        AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
        SET_STASH_SHARED_IS_LOCKED(D, 1);
    }

    // Execute function and leave result or error message on top
    // of the stack.
    unsigned error = duk_pcall(D, 0) != DUK_EXEC_SUCCESS;

    // Release lock if needed.
    if (!is_locked) {
        SET_STASH_SHARED_IS_LOCKED(D, 0);
        AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    }

    // Done!
    if (error) {
        (void) duk_throw(D);
    }
    return 1;
}

#undef GET_STASH_SHARED_IS_LOCKED
#undef SET_STASH_SHARED_IS_LOCKED

#undef GET_STASH_FOO_FIELD
#undef GET_STASH_CTX
#undef GET_STASH_SCRIPT
#undef GET_STASH_STATE

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static duk_context *
new_context(VRT_CTX, struct vmod_cfg_script *script)
{
    // Create base context.
    duk_context *result = duk_create_heap_default();
    AN(result);

    // Initialize the 'is_locked' flag used by the varnish.shared.* commands
    // (see STASH_KEY_SHARED_IS_LOCKED).
    duk_push_heap_stash(result);
    duk_push_boolean(result, 0);
    duk_put_prop_string(result, -2, STASH_KEY_SHARED_IS_LOCKED);
    duk_pop(result);

    // Create the per-engine regexp cache object in the heap stash (see
    // STASH_KEY_REGEXP_CACHE & 'get_regexp()').
    duk_push_heap_stash(result);
    duk_push_bare_object(result);
    duk_put_prop_string(result, -2, STASH_KEY_REGEXP_CACHE);
    duk_pop(result);

    // Add support for varnish.engine, varnish.shared, varnish.log(), etc.
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
    duk_push_c_function(result, varnish_shared_get_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "get");
    duk_push_c_function(result, varnish_shared_set_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "set");
    duk_push_c_function(result, varnish_shared_unset_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "unset");
    duk_push_c_function(result, varnish_shared_eval_javascript_command, DUK_VARARGS);
    duk_put_prop_string(result, -2, "eval");
    duk_pop_2(result);

    // Add script helpers.
    AZ(duk_peval_lstring(
        result,
        (const char *) &script_javascript_helpers_js,
        script_javascript_helpers_js_len));
    duk_pop(result);

    // Freeze the 'varnish' & 'varnish.shared' objects: with no internal values
    // left in them (see the heap stash keys) nothing needs to change after
    // creation, so make them read-only for scripts, matching the Lua sandbox.
    // Sandboxed scripts run in strict mode (see 'pre_execute()'), so writes to
    // frozen properties throw a TypeError instead of failing silently. Beware
    // 'duk_freeze()' is shallow: 'varnish.engine' stays writable on purpose
    // (per-engine scratch space for scripts), only its binding is frozen. This
    // must be the last step of the engine initialization: the script helpers
    // above still add properties to 'varnish.shared'.
    if (script->enable_sandboxing) {
        duk_get_global_string(result, "varnish");
        AN(duk_is_object(result, -1));
        duk_get_prop_string(result, -1, "shared");
        AN(duk_is_object(result, -1));
        duk_freeze(result, -1);
        duk_pop(result);
        duk_freeze(result, -1);
        duk_pop(result);
    }

    // Done!
    return result;
}
