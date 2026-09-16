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

#include "script_lua.h"
#include "script_lua_helpers.h"
#include "helpers.h"

static lua_State *new_context(VRT_CTX, struct vmod_cfg_script *script);

// Keys used to store per-engine values in the Lua registry, which is not
// reachable from scripts:
//   - The error handler, cached once by 'new_context()' instead of being
//     looked up through the 'varnish' global table on every execution. This
//     also freezes the binding: whatever a script does to 'varnish' afterwards,
//     the handler defined by the builtin helpers is the one used.
//   - The 'ctx', 'script' & 'state' pointers (light userdata), set by
//     'execute()' around each execution and used by the varnish.* commands.
//     Light userdata is a plain value (no allocation, nothing for the garbage
//     collector to track) and the registry is a plain table, so setting them
//     is a raw assignment instead of a '__newindex' metamethod call on the
//     read-only 'varnish' table. As a bonus, the pointers are no longer
//     readable (nor overwritable) from scripts.
//   - The 'is_locked' flag used by the varnish.shared.* commands, for the
//     same reasons.
//   - The per-engine regexp cache table, mapping pattern strings to 'vre_t'
//     pointers (light userdata) borrowed from the script-wide regexp cache.
//     It lets the varnish.reg*() commands resolve compiled regexps without
//     touching the shared cache (and its rwlock) on every call; see
//     'get_regexp()'.
#define REGISTRY_KEY_ERROR_HANDLER "varnish._error_handler"
#define REGISTRY_KEY_CTX "varnish._ctx"
#define REGISTRY_KEY_SCRIPT "varnish._script"
#define REGISTRY_KEY_STATE "varnish._state"
#define REGISTRY_KEY_SHARED_IS_LOCKED "varnish.shared._is_locked"
#define REGISTRY_KEY_REGEXP_CACHE "varnish.regexps"

/******************************************************************************
 * BASICS.
 *****************************************************************************/

engine_t *
new_lua_engine(VRT_CTX, struct vmod_cfg_script *script)
{
    AN(script->type == ENGINE_TYPE_LUA);

    engine_t *result = new_engine(script->type, new_context(ctx, script));
    result->memory = get_lua_engine_used_memory(result);
    return result;
}

int
get_lua_engine_used_memory(engine_t * engine)
{
    AN(engine->type == ENGINE_TYPE_LUA);

    return lua_gc(engine->ctx.L, LUA_GCCOUNT, 0);
}

int
get_lua_engine_stack_size(engine_t * engine)
{
    AN(engine->type == ENGINE_TYPE_LUA);

    return lua_gettop(engine->ctx.L);
}

static unsigned
pre_execute(
    VRT_CTX, struct vmod_cfg_script * script, engine_t *engine,
    const char *code, const char *name)
{
    AN(script->type == ENGINE_TYPE_LUA);
    AN(engine->type == ENGINE_TYPE_LUA);

    unsigned sucess = 0;

    struct vsb *vsb = VSB_new_auto();
    AN(vsb);
    AZ(VSB_printf(vsb, "function %s()\n%s\nend", name, code));
    AZ(VSB_finish(vsb));

    if (!luaL_loadbuffer(engine->ctx.L, VSB_data(vsb), VSB_len(vsb), "@varnish_script")) {
        if (!lua_pcall(engine->ctx.L, 0, 0, 0)) {
            sucess = 1;
        } else {
            LOG(ctx, LOG_ERR,
                "Failed to execute new Lua script (script=%s, function=%s, code=%.80s...): %s",
                script->name, name, code, lua_tostring(engine->ctx.L, -1));
            lua_pop(engine->ctx.L, 1);
        }
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to compile new Lua script (script=%s, function=%s, code=%.80s...): %s",
            script->name, name, code, lua_tostring(engine->ctx.L, -1));
        lua_pop(engine->ctx.L, 1);
    }

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
        where.value.boolean = (unsigned) lua_toboolean(engine->ctx.L, -1)

    #define STORE_NUMBER(where) \
        where.type = RESULT_VALUE_TYPE_NUMBER; \
        where.value.number = (double) lua_tonumber(engine->ctx.L, -1)

    #define STORE_STRING(where) \
        where.type = RESULT_VALUE_TYPE_STRING; \
        where.value.string = WS_Copy(ctx->ws, lua_tostring(engine->ctx.L, -1), -1); \
        if (where.value.string == NULL) { \
            FAIL_WS(ctx, 0); \
        }

    #define STORE_ERROR(where) \
        where.type = RESULT_VALUE_TYPE_ERROR

    switch (lua_type(engine->ctx.L, -1)) {
        case LUA_TNIL:
            STORE_NIL(result->values[0]);
            return 1;

        case LUA_TBOOLEAN:
            STORE_BOOLEAN(result->values[0]);
            return 1;

        case LUA_TNUMBER:
            STORE_NUMBER(result->values[0]);
            return 1;

        case LUA_TSTRING:
            STORE_STRING(result->values[0]);
            return 1;

        case LUA_TTABLE:
            result->nvalues = 0;
            while (1) {
                lua_pushnumber(engine->ctx.L, result->nvalues + 1);
                lua_gettable(engine->ctx.L, -2);
                if (lua_type(engine->ctx.L, -1) == LUA_TNIL) {
                    lua_pop(engine->ctx.L, 1);
                    break;
                }
                switch (lua_type(engine->ctx.L, -1)) {
                    case LUA_TBOOLEAN:
                        STORE_BOOLEAN(result->values[result->nvalues]);
                        break;
                    case LUA_TNUMBER:
                        STORE_NUMBER(result->values[result->nvalues]);
                        break;
                    case LUA_TSTRING:
                        STORE_STRING(result->values[result->nvalues]);
                        break;
                    case LUA_TTABLE:
                        result->values[result->nvalues].type = RESULT_VALUE_TYPE_TABLE;
                        break;
                    default:
                        LOG(ctx, LOG_ERR,
                            "Got invalid Lua script result table value (script=%s, index=%d, type=%d)",
                            script->name, result->nvalues, lua_type(engine->ctx.L, -1));
                        STORE_ERROR(result->values[result->nvalues]);
                        break;

                }
                lua_pop(engine->ctx.L, 1);
                result->nvalues++;
                if (result->nvalues == MAX_RESULT_VALUES) {
                    LOG(ctx, LOG_ERR,
                        "Failed to store Lua table value (script=%s, limit=%d)",
                        script->name, MAX_RESULT_VALUES);
                    break;
                }
            }
            return 1;

        // LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD, LUA_TLIGHTUSERDATA, etc.
        default:
            LOG(ctx, LOG_ERR,
                "Got invalid Lua script result value (script=%s, type=%d)",
                script->name, lua_type(engine->ctx.L, -1));
            STORE_ERROR(result->values[0]);
            return 0;
    }

    #undef STORE_NIL
    #undef STORE_BOOLEAN
    #undef STORE_NUMBER
    #undef STORE_STRING
    #undef STORE_ERROR
}

unsigned
execute_lua(
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

    // Push the error handler cached in the registry by 'new_context()' into
    // the stack.
    lua_getfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_ERROR_HANDLER);
    AN(lua_isfunction(engine->ctx.L, -1));

    // Try to lookup the function to be executed. Result will be pushed
    // into the stack.
    lua_getglobal(engine->ctx.L, *name);
    if (!lua_isfunction(engine->ctx.L, -1)) {
        // Remove the non-function value from the stack.
        lua_pop(engine->ctx.L, 1);
        unknown = 1;

        // Compile & register the function to be executed.
        if (pre_execute(ctx, script, engine, code, *name)) {
            // Push the function to be executed into the stack.
            lua_getglobal(engine->ctx.L, *name);
            AN(lua_isfunction(engine->ctx.L, -1));
        } else {
            // Remove the error handler from the stack.
            lua_pop(engine->ctx.L, 1);

            // Stop execution.
            goto done;
        }
    }

    // Current state of the stack at this point (top to bottom):
    //   - Function to be executed.
    //   - Error handler function.

    if (result != NULL) {
        // Assertions.
        AN(state);
        AN(argv);
        AN(result);

        // Store the 'ctx', 'script' & 'state' pointers in the registry (see
        // REGISTRY_KEY_CTX, etc.). They are used by the varnish.* commands.
        lua_pushlightuserdata(engine->ctx.L, TRUST_ME(ctx));
        lua_setfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_CTX);
        lua_pushlightuserdata(engine->ctx.L, script);
        lua_setfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_SCRIPT);
        lua_pushlightuserdata(engine->ctx.L, state);
        lua_setfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_STATE);

        // Populate 'ARGV' table accordingly to the input arguments.
        lua_newtable(engine->ctx.L);
        for (int i = 0; i < argc; i++) {
            lua_pushstring(engine->ctx.L, argv[i]);
            lua_rawseti(engine->ctx.L, -2, i);
        }
        lua_setglobal(engine->ctx.L, "ARGV");

        // At this point whether the function was never seen before or if it was
        // already defined, we can call it. We have zero arguments and expect
        // a single return value.
        if (!lua_pcall(engine->ctx.L, 0, 1, -2)) {
            if (post_execute(ctx, script, engine, result)) {
                success = 1;
            } else {
                result->values[0].type = RESULT_VALUE_TYPE_ERROR;
                LOG(ctx, LOG_ERR,
                    "Failed to process Lua script result (script=%s, function=%s, code=%.80s...)",
                    script->name, *name, code);
            }
        } else {
            result->values[0].type = RESULT_VALUE_TYPE_ERROR;
            LOG(ctx, LOG_ERR,
                "Failed to execute Lua script (script=%s, function=%s, code=%.80s...): %s",
                script->name, *name, code, lua_tostring(engine->ctx.L, -1));
        }

        // Remove the function result and the error handler from the stack.
        lua_pop(engine->ctx.L, 2);

        // Clear the pointers stored in the registry: the engine is about to
        // be released and they must not outlive this execution (a varnish.*
        // command executed outside an execution must fail loudly instead of
        // silently using stale pointers).
        lua_pushnil(engine->ctx.L);
        lua_setfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_CTX);
        lua_pushnil(engine->ctx.L);
        lua_setfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_SCRIPT);
        lua_pushnil(engine->ctx.L);
        lua_setfield(engine->ctx.L, LUA_REGISTRYINDEX, REGISTRY_KEY_STATE);
    } else {
        // Everything looks correct. Full execution is not required: simply
        // remove function to be executed & error handler from the stack.
        success = 1;
        lua_pop(engine->ctx.L, 2);
    }

done:
    // Call the garbage collector from time to time to avoid a full cycle
    // performed by Lua, which adds too much latency.
    engine->ncycles++;
    if (gc_collect) {
        lua_gc(engine->ctx.L, LUA_GCCOLLECT, 0);
        gc = 1;
    } else {
        if (engine->ncycles % script->min_gc_cycles == 0) {
            lua_gc(engine->ctx.L, LUA_GCSTEP, script->engine_cfg.lua.gc_step_size);
            gc = 1;
        }
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

// Beware of error handling inside these lua_CFunction command handlers: many
// Lua API calls (e.g. 'lua_pushstring()', 'lua_rawset()') allocate Lua memory
// and raise a Lua error on allocation failure, which longjmp()s back to the
// innermost 'lua_pcall()', skipping the rest of the handler. Any C resource
// still owned by the handler at that point is leaked: the child process
// survives (the failed execution is logged and the engine is reused) but the
// memory is never reclaimed. The convention here is that explicit error paths
// free C resources *before* raising with 'lua_error()'; the remaining exposure
// is on allocating calls made *while* a C resource is still owned (none at the
// moment). The only owned resource in these handlers is an uncached regexp,
// freed right after use; workspace allocations are reclaimed with the task. If
// there were other owned C resources, calls can only trigger on Lua allocation
// failure (i.e., the engine is already dying of OOM), so the practical risk is
// bounded; removing it entirely would require protected pushes (an inner
// 'lua_pcall()' around the allocating calls) or Lua-owned guards (full
// userdata + '__gc').

// Extract a pointer stored in the Lua registry (see REGISTRY_KEY_CTX, etc.).
// The light userdata is pushed into the stack and then removed.
#define GET_REGISTRY_FOO_FIELD(L, key, where, MAGIC) \
    do { \
        lua_getfield(L, LUA_REGISTRYINDEX, key); \
        AN(lua_islightuserdata(L, -1)); \
        void *data = lua_touserdata(L, -1); \
        AN(data); \
        CAST_OBJ_NOTNULL(where, data, MAGIC); \
        lua_pop(L, 1); \
    } while (0)

#define GET_REGISTRY_CTX(L, where) \
    GET_REGISTRY_FOO_FIELD(L, REGISTRY_KEY_CTX, where, VRT_CTX_MAGIC)

#define GET_REGISTRY_SCRIPT(L, where) \
    GET_REGISTRY_FOO_FIELD(L, REGISTRY_KEY_SCRIPT, where, VMOD_CFG_SCRIPT_MAGIC)

#define GET_REGISTRY_STATE(L, where) \
    GET_REGISTRY_FOO_FIELD(L, REGISTRY_KEY_STATE, where, TASK_STATE_MAGIC)

/*
 * Resolve the compiled regexp for the pattern at stack index 'regexp_idx'
 * ('regexp' being its C string), going through the per-engine cache table
 * stored in the Lua registry (see REGISTRY_KEY_REGEXP_CACHE). A hit is a cheap
 * table lookup on memory owned by this engine: no shared state (i.e. the
 * script-wide regexp cache and its rwlock) is touched. The table is keyed with
 * the pattern argument itself: Lua strings are interned, so 'lua_pushvalue()'
 * copies a tagged value already hashed, instead of 'lua_pushstring()'
 * rediscovering the string object (strlen, hash and memcmp over the pattern).
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
    lua_State *L, int regexp_idx, VRT_CTX, struct vmod_cfg_script *script,
    const char *regexp, unsigned cache)
{
    vre_t *result;

    if (!cache) {
        return init_regexp(ctx, script, regexp, 0);
    }

    lua_getfield(L, LUA_REGISTRYINDEX, REGISTRY_KEY_REGEXP_CACHE);
    AN(lua_istable(L, -1));
    lua_pushvalue(L, regexp_idx);
    lua_rawget(L, -2);
    if (lua_islightuserdata(L, -1)) {
        result = lua_touserdata(L, -1);
        AN(result);
        lua_pop(L, 2);
        return result;
    }
    lua_pop(L, 1);

    result = init_regexp(ctx, script, regexp, 1);
    if (result != NULL) {
        lua_pushvalue(L, regexp_idx);
        lua_pushlightuserdata(L, result);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);

    return result;
}

static int
varnish_log_lua_command(lua_State *L)
{
    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc != 1) {
        lua_pushstring(L, "varnish.log() requires one argument.");
        lua_error(L);
    }
    const char *message = lua_tostring(L, 1);

    // Check input arguments.
    if (message != NULL) {
        // Extract 'ctx' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);

        // Execute command.
        varnish_log_command(ctx, message);
    }

    // Done!
    return 0;
}

static int
varnish_get_header_lua_command(lua_State *L)
{
    // Initializations.
    const char *result = NULL;

    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 1) {
        lua_pushstring(L, "varnish.get_header() requires one argument.");
        lua_error(L);
    }
    const char *name = lua_tostring(L, 1);
    const char *where = NULL;
    if (argc >= 2) {
        where = lua_tostring(L, 2);
    }
    where = where ? where : "req";

    // Check input arguments.
    if (name != NULL && strlen(name) > 0) {
        // Extract 'ctx' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);

        // Execute command.
        const char *error;
        result = varnish_get_header_command(ctx, name, where, &error);
        if (error != NULL) {
            lua_pushstring(L, error);
            lua_error(L);
        }
    }

    // Done!
    lua_pushstring(L, result);
    return 1;
}

static int
varnish_set_header_lua_command(lua_State *L)
{
    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 2) {
        lua_pushstring(L, "varnish.set_header() requires two arguments.");
        lua_error(L);
    }
    const char *name = lua_tostring(L, 1);
    const char *value = lua_tostring(L, 2);
    const char *where = NULL;
    if (argc >= 3) {
        where = lua_tostring(L, 3);
    }
    where = where ? where : "req";

    // Check input arguments.
    if (name != NULL && strlen(name) > 0 &&
        value != NULL && strlen(value) > 0) {
        // Extract 'ctx' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);

        // Execute command.
        const char *error;
        varnish_set_header_command(ctx, name, value, where, &error);
        if (error != NULL) {
            lua_pushstring(L, error);
            lua_error(L);
        }
    }

    // Done!
    return 0;
}

static int
varnish_regmatch_lua_command(lua_State *L)
{
    // Initializations.
    unsigned result = 0;

    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 2) {
        lua_pushstring(L, "varnish.regmatch() requires two arguments.");
        lua_error(L);
    }
    const char *string = lua_tostring(L, 1);
    size_t regexp_len;
    const char *regexp = lua_tolstring(L, 2, &regexp_len);
    unsigned cache;
    if (argc >= 3) {
        cache = lua_toboolean(L, 3);
    } else {
        cache = 1;
    }

    // Reject patterns with embedded NUL bytes. Patterns are compiled as C
    // strings and the shared regexp cache keys on the same prefix, so such a
    // pattern would quietly behave as its pre-NUL prefix ('a\0x' and 'a\0y'
    // would both compile to /a/), while the per-engine cache would key on the
    // whole Lua string. A NUL byte in a pattern is always a script bug: fail
    // loudly, like a pattern that fails to compile. Beware this runs before
    // any C resource is acquired, so raising is longjmp-safe.
    if (regexp != NULL && strlen(regexp) != regexp_len) {
        lua_pushstring(L, "varnish.regmatch() pattern contains a NUL byte.");
        lua_error(L);
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL) {
        // Extract 'ctx' & 'script' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_REGISTRY_SCRIPT(L, script);

        // Resolve the compiled regexp (the pattern is the second argument)
        // & execute command.
        vre_t *re = get_regexp(L, 2, ctx, script, regexp, cache);
        if (re != NULL) {
            result = varnish_regmatch_re_command(ctx, string, re);
            if (!cache) {
                VRE_free(&re);
            }
        } else {
            const char *error = regexp_error(ctx, regexp);
            if (error != NULL) {
                lua_pushstring(L, error);
                lua_error(L);
            }
        }
    }

    // Done!
    lua_pushboolean(L, result);
    return 1;
}

static int
varnish_regsub_lua_command(lua_State *L, unsigned all)
{
    // Initializations.
    const char *result = NULL;

    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 3) {
        lua_pushstring(L, "varnish.regsub() & varnish.regsuball() require three arguments.");
        lua_error(L);
    }
    const char *string = lua_tostring(L, 1);
    size_t regexp_len;
    const char *regexp = lua_tolstring(L, 2, &regexp_len);
    const char *sub = lua_tostring(L, 3);
    unsigned cache;
    if (argc >= 4) {
        cache = lua_toboolean(L, 4);
    } else {
        cache = 1;
    }

    // Reject patterns with embedded NUL bytes. Patterns are compiled as C
    // strings and the shared regexp cache keys on the same prefix, so such a
    // pattern would quietly behave as its pre-NUL prefix ('a\0x' and 'a\0y'
    // would both compile to /a/), while the per-engine cache would key on the
    // whole Lua string. A NUL byte in a pattern is always a script bug: fail
    // loudly, like a pattern that fails to compile. Beware this runs before
    // any C resource is acquired, so raising is longjmp-safe.
    if (regexp != NULL && strlen(regexp) != regexp_len) {
        lua_pushstring(L, "varnish.regsub() & varnish.regsuball() pattern contains a NUL byte.");
        lua_error(L);
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL && sub != NULL) {
        // Extract 'ctx' & 'script' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_REGISTRY_SCRIPT(L, script);

        // Resolve the compiled regexp (the pattern is the second argument)
        // & execute command.
        vre_t *re = get_regexp(L, 2, ctx, script, regexp, cache);
        if (re != NULL) {
            result = varnish_regsub_re_command(ctx, string, re, sub, all);
            if (!cache) {
                VRE_free(&re);
            }
        } else {
            const char *error = regexp_error(ctx, regexp);
            if (error != NULL) {
                lua_pushstring(L, error);
                lua_error(L);
            }
        }
    }

    // Done! On no match 'VRT_regsub()' returns the subject itself: push a copy
    // of argument 1 instead of re-interning it with 'lua_pushstring()' (hash,
    // string table probe and a full memcmp against the very string already
    // held). Beware argument 1 is guaranteed to be a Lua string at this point:
    // it either was one, or 'lua_tostring()' converted the number in place.
    // The match path is unchanged: the result is a new workspace string and
    // has to be interned anyway.
    if (result != NULL && result == string) {
        lua_pushvalue(L, 1);
    } else {
        lua_pushstring(L, result);
    }
    return 1;
}

static int
varnish_regsubone_lua_command(lua_State *L)
{
    return varnish_regsub_lua_command(L, 0);
}

static int
varnish_regsuball_lua_command(lua_State *L)
{
    return varnish_regsub_lua_command(L, 1);
}

/******************************************************************************
 * VARNISH.SHARED.* COMMANDS.
 *****************************************************************************/

// Extract the 'is_locked' flag stored in the registry (see
// REGISTRY_KEY_SHARED_IS_LOCKED).
#define GET_REGISTRY_SHARED_IS_LOCKED(L, where) \
    do { \
        lua_getfield(L, LUA_REGISTRYINDEX, REGISTRY_KEY_SHARED_IS_LOCKED); \
        AN(lua_isboolean(L, -1)); \
        where = lua_toboolean(L, -1); \
        lua_pop(L, 1); \
    } while (0)

// Update the 'is_locked' flag stored in the registry.
#define SET_REGISTRY_SHARED_IS_LOCKED(L, value) \
    do { \
        lua_pushboolean(L, value); \
        lua_setfield(L, LUA_REGISTRYINDEX, REGISTRY_KEY_SHARED_IS_LOCKED); \
    } while (0)

static int
varnish_shared_get_lua_command(lua_State *L)
{
    // Initializations.
    const char *result = NULL;

    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 1) {
        lua_pushstring(L, "varnish.shared.get() requires one argument.");
        lua_error(L);
    }
    const char *key = lua_tostring(L, 1);
    const char *scope = NULL;
    if (argc >= 2) {
        scope = lua_tostring(L, 2);
    }
    scope = scope ? scope : "all";

    // Check input arguments.
    if (key != NULL && strlen(key) > 0) {
        // Extract 'is_locked' from the registry.
        unsigned is_locked;
        GET_REGISTRY_SHARED_IS_LOCKED(L, is_locked);

        // Extract 'ctx', 'script' & 'state' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_REGISTRY_SCRIPT(L, script);
        task_state_t *state;
        GET_REGISTRY_STATE(L, state);

        // Execute command.
        result = varnish_shared_get_command(ctx, script, state, key, scope, is_locked);
    }

    // Done!
    lua_pushstring(L, result);
    return 1;
}

static int
varnish_shared_set_lua_command(lua_State *L)
{
    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 2) {
        lua_pushstring(L, "varnish.shared.set() requires two arguments.");
        lua_error(L);
    }
    const char *key = lua_tostring(L, 1);
    const char *value = lua_tostring(L, 2);
    const char *scope = NULL;
    if (argc >= 3) {
        scope = lua_tostring(L, 3);
    }
    scope = scope ? scope : "task";

    // Check input arguments.
    if (key != NULL && strlen(key) > 0 &&
        value != NULL) {
        // Extract 'is_locked' from the registry.
        unsigned is_locked;
        GET_REGISTRY_SHARED_IS_LOCKED(L, is_locked);

        // Extract 'ctx', 'script' & 'state' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_REGISTRY_SCRIPT(L, script);
        task_state_t *state;
        GET_REGISTRY_STATE(L, state);

        // Execute command.
        varnish_shared_set_command(ctx, script, state, key, value, scope, is_locked);
    }

    // Done!
    return 0;
}

static int
varnish_shared_unset_lua_command(lua_State *L)
{
    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 1) {
        lua_pushstring(L, "varnish.shared.unset() requires one argument.");
        lua_error(L);
    }
    const char *key = lua_tostring(L, 1);
    const char *scope = NULL;
    if (argc >= 2) {
        scope = lua_tostring(L, 2);
    }
    scope = scope ? scope : "all";

    // Check input arguments.
    if (key != NULL && strlen(key) > 0) {
        // Extract 'is_locked' from the registry.
        unsigned is_locked;
        GET_REGISTRY_SHARED_IS_LOCKED(L, is_locked);

        // Extract 'ctx', 'script' & 'state' from the registry.
        VRT_CTX;
        GET_REGISTRY_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_REGISTRY_SCRIPT(L, script);
        task_state_t *state;
        GET_REGISTRY_STATE(L, state);

        // Execute command.
        varnish_shared_unset_command(ctx, script, state, key, scope, is_locked);
    }

    // Done!
    return 0;
}

static int
varnish_shared_eval_lua_command(lua_State *L)
{
    // Check input arguments.
    int argc = lua_gettop(L);
    if (argc != 1) {
        lua_pushstring(L, "varnish.shared.eval() requires one argument.");
        lua_error(L);
    }
    if (!lua_isfunction(L, 1)) {
        lua_pushstring(L, "varnish.shared.eval() requires a function argument.");
        lua_error(L);
    }

    // Extract 'is_locked' from the registry.
    unsigned is_locked;
    GET_REGISTRY_SHARED_IS_LOCKED(L, is_locked);

    // Extract 'script' from the registry.
    struct vmod_cfg_script *script;
    GET_REGISTRY_SCRIPT(L, script);

    // Get lock if needed.
    if (!is_locked) {
        AZ(pthread_rwlock_wrlock(&script->state.variables.rwlock));
        SET_REGISTRY_SHARED_IS_LOCKED(L, 1);
    }

    // Execute function and leave result or error message on top
    // of the stack.
    unsigned error = lua_pcall(L, 0, 1, 0) != 0;

    // Release lock if needed.
    if (!is_locked) {
        SET_REGISTRY_SHARED_IS_LOCKED(L, 0);
        AZ(pthread_rwlock_unlock(&script->state.variables.rwlock));
    }

    // Done!
    if (error) {
        lua_error(L);
    }
    return 1;
}

#undef GET_REGISTRY_SHARED_IS_LOCKED
#undef SET_REGISTRY_SHARED_IS_LOCKED

#undef GET_REGISTRY_FOO_FIELD
#undef GET_REGISTRY_CTX
#undef GET_REGISTRY_SCRIPT
#undef GET_REGISTRY_STATE

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static void
load_lua_lib(lua_State *L, const char *name, lua_CFunction f)
{
#if LUA_VERSION_NUM >= 502
    luaL_requiref(L, name, f, 1);
    lua_pop(L, 1);
#else
    lua_pushcfunction(L, f);
    lua_pushstring(L, name);
    lua_call(L, 1, 0);
#endif
}

static void
load_lua_libs(struct vmod_cfg_script *script, lua_State *L)
{
#if LUA_VERSION_NUM >= 502
    load_lua_lib(L, "_G", luaopen_base);
#else
    load_lua_lib(L, "", luaopen_base);
#endif
    load_lua_lib(L, LUA_TABLIBNAME, luaopen_table);
    load_lua_lib(L, LUA_STRLIBNAME, luaopen_string);
    load_lua_lib(L, LUA_MATHLIBNAME, luaopen_math);
    load_lua_lib(L, LUA_DBLIBNAME, luaopen_debug);
    if (script->engine_cfg.lua.libraries.package) {
        load_lua_lib(L, LUA_LOADLIBNAME, luaopen_package);
    }
    if (script->engine_cfg.lua.libraries.io) {
        load_lua_lib(L, LUA_IOLIBNAME, luaopen_io);
    }
    if (script->engine_cfg.lua.libraries.os) {
        load_lua_lib(L, LUA_OSLIBNAME, luaopen_os);
    }
}

static void
remove_unsupported_lua_function(lua_State *L, const char *name)
{
    lua_pushnil(L);
    lua_setglobal(L, name);
}

static void
remove_unsupported_lua_functions(struct vmod_cfg_script *script, lua_State *L)
{
    if (!script->engine_cfg.lua.functions.loadfile) {
        remove_unsupported_lua_function(L, "loadfile");
    }
    if (!script->engine_cfg.lua.functions.dotfile) {
        remove_unsupported_lua_function(L, "dotfile");
    }
}

static void
enable_lua_protections(lua_State *L)
{
    // This function should be the last to be called in the scripting engine
    // initialization sequence!
    const char *protections =
        "-- http://metalua.luaforge.net/src/lib/strict.lua.html\n"
        "setmetatable(_G, {\n"
        "  __index = function(table, key)\n"
        "    if debug.getinfo(2) and debug.getinfo(2, 'S').what ~= 'C' then\n"
        "      error('Script attempted to access nonexistent global variable \\'' .. tostring(key) .. '\\'')\n"
        "    end\n"
        "    return rawget(table, key)\n"
        "  end,\n"
        "  __newindex = function(table, key, value)\n"
        "    if debug.getinfo(2) then\n"
        "      local w = debug.getinfo(2, 'S').what\n"
        "      if w ~= 'main' and w ~= 'C' then\n"
        "        error('Script attempted to create global variable \\'' .. tostring(key) .. '\\'')\n"
        "      end\n"
        "    end\n"
        "    rawset(table, key, value)\n"
        "  end\n"
        " });\n"
        "\n"
        "-- http://lua-users.org/wiki/ReadOnlyTables\n"
        "local function readonly_table(table, exceptions)\n"
        "   return setmetatable({}, {\n"
        "     __index = table,\n"
        "     __newindex = function(table, key, value)\n"
        "         if not exceptions[key] then\n"
        "           error('Script attempted to modify read-only table')\n"
        "         end\n"
        "         rawset(table, key, value)\n"
        "     end,\n"
        "     __metatable = false\n"
        "   });\n"
        "end\n"
        "varnish.shared = readonly_table(varnish.shared, {})\n"
        "varnish = readonly_table(varnish, {})\n"
        "\n"
        "readonly_table = nil\n";
    AZ(luaL_loadbuffer(L, protections, strlen(protections), "@enable_lua_protections"));
    AZ(lua_pcall(L, 0, 0, 0));
}

static lua_State *
new_context(VRT_CTX, struct vmod_cfg_script *script)
{
    // Create base context.
    lua_State *result = luaL_newstate();
    AN(result);

    // Load libraries & disable unsupported functions.
    load_lua_libs(script, result);
    remove_unsupported_lua_functions(script, result);

    // Add support for varnish.engine, varnish.shared, varnish._ctx,
    // varnish._script, varnish._error_handler(), varnish.log(), etc.
    lua_newtable(result);
    lua_newtable(result);
    lua_setfield(result, -2, "engine");
    lua_newtable(result);
    lua_setfield(result, -2, "shared");
    lua_pushcfunction(result, varnish_log_lua_command);
    lua_setfield(result, -2, "log");
    lua_pushcfunction(result, varnish_get_header_lua_command);
    lua_setfield(result, -2, "get_header");
    lua_pushcfunction(result, varnish_set_header_lua_command);
    lua_setfield(result, -2, "set_header");
    lua_pushcfunction(result, varnish_regmatch_lua_command);
    lua_setfield(result, -2, "regmatch");
    lua_pushcfunction(result, varnish_regsubone_lua_command);
    lua_setfield(result, -2, "regsub");
    lua_pushcfunction(result, varnish_regsuball_lua_command);
    lua_setfield(result, -2, "regsuball");
    lua_setglobal(result, "varnish");

    // Add support for varnish.shared.* commands.
    lua_getglobal(result, "varnish");
    AN(lua_istable(result, -1));
    lua_getfield(result, -1, "shared");
    AN(lua_istable(result, -1));
    lua_pushcfunction(result, varnish_shared_get_lua_command);
    lua_setfield(result, -2, "get");
    lua_pushcfunction(result, varnish_shared_set_lua_command);
    lua_setfield(result, -2, "set");
    lua_pushcfunction(result, varnish_shared_unset_lua_command);
    lua_setfield(result, -2, "unset");
    lua_pushcfunction(result, varnish_shared_eval_lua_command);
    lua_setfield(result, -2, "eval");
    lua_pop(result, 2);

    // Add script helpers.
    AZ(luaL_loadbuffer(
        result,
        (const char *) &script_lua_helpers_lua,
        script_lua_helpers_lua_len,
        "@helpers"));
    AZ(lua_pcall(result, 0, 0, 0));

    // Cache the error handler (defined by the script helpers) in the registry
    // (see REGISTRY_KEY_ERROR_HANDLER) and initialize the 'is_locked' flag
    // used by the varnish.shared.* commands (see
    // REGISTRY_KEY_SHARED_IS_LOCKED).
    lua_getglobal(result, "varnish");
    AN(lua_istable(result, -1));
    lua_getfield(result, -1, "_error_handler");
    AN(lua_isfunction(result, -1));
    lua_setfield(result, LUA_REGISTRYINDEX, REGISTRY_KEY_ERROR_HANDLER);
    lua_pop(result, 1);
    lua_pushboolean(result, 0);
    lua_setfield(result, LUA_REGISTRYINDEX, REGISTRY_KEY_SHARED_IS_LOCKED);

    // Create the per-engine regexp cache table in the registry (see
    // REGISTRY_KEY_REGEXP_CACHE & 'get_regexp()').
    lua_newtable(result);
    lua_setfield(result, LUA_REGISTRYINDEX, REGISTRY_KEY_REGEXP_CACHE);

    // Protect accesses to global variables, set global 'varnish' table as
    // read only, disable the 'debug' library, etc.
    if (script->enable_sandboxing) {
        enable_lua_protections(result);
    }

    // Done!
    return result;
}
