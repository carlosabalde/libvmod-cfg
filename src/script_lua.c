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

#include "script_lua.h"
#include "helpers.h"

static lua_State *new_context(VRT_CTX, struct vmod_cfg_script *script);

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
        AN(where.value.string);

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

    // Push 'varnish' table and Varnish context into the stack. Then execute
    // 'varnish._ctx = ctx', keeping the 'varnish' table in the top of the
    // stack. Same for 'varnish._script = script'.
    lua_getglobal(engine->ctx.L, "varnish");
    AN(lua_istable(engine->ctx.L, -1));
    lua_pushlightuserdata(engine->ctx.L, (struct vrt_ctx *) ctx);
    lua_setfield(engine->ctx.L, -2, "_ctx");
    lua_pushlightuserdata(engine->ctx.L, (struct vmod_cfg_script *) script);
    lua_setfield(engine->ctx.L, -2, "_script");

    // Push the value of 'varnish._error_handler' into the stack. This will
    // keep the 'varnish' table in the stack, just under the the error handler
    // function.
    lua_getfield(engine->ctx.L, -1, "_error_handler");
    AN(lua_isfunction(engine->ctx.L, -1));

    // Try to lookup the function to be executed. Result will be pushed
    // into the stack.
    lua_getglobal(engine->ctx.L, *name);
    if (!lua_isfunction(engine->ctx.L, -1)) {
        // Remove the non-function value from the stack.
        lua_pop(engine->ctx.L, 1);

        // Update stats.
        Lck_Lock(&script->state.mutex);
        script->state.stats.executions.unknown++;
        Lck_Unlock(&script->state.mutex);

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
    //   - 'varnish' table.

    if (result != NULL) {
        // Assertions.
        AN(argv);
        AN(result);

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
    } else {
        // Everything looks correct. Full execution is not required: simply
        // remove function to be executed & error handler from the stack.
        success = 1;
        lua_pop(engine->ctx.L, 2);
    }

done:
    // Current state of the stack at this point (top to bottom):
    //   - 'varnish' table.

    // Execute 'varnish._ctx = nil' and 'varnish._script = nil', then
    // remove 'varnish' table from the stack.
    lua_pushnil(engine->ctx.L);
    lua_setfield(engine->ctx.L, -2, "_ctx");
    lua_pushnil(engine->ctx.L);
    lua_setfield(engine->ctx.L, -2, "_script");
    lua_pop(engine->ctx.L, 1);

    // Update stats.
    Lck_Lock(&script->state.mutex);
    script->state.stats.executions.total++;
    if (!success) {
        script->state.stats.executions.failed++;
    }
    Lck_Unlock(&script->state.mutex);

    // Call the garbage collector from time to time to avoid a full cycle
    // performed by Lua, which adds too much latency.
    engine->ncycles++;
    if (gc_collect) {
        lua_gc(engine->ctx.L, LUA_GCCOLLECT, 0);
        Lck_Lock(&script->state.mutex);
        script->state.stats.executions.gc++;
        Lck_Unlock(&script->state.mutex);
    } else {
        if (engine->ncycles % script->min_gc_cycles == 0) {
            lua_gc(engine->ctx.L, LUA_GCSTEP, script->engine_cfg.lua.gc_step_size);
            Lck_Lock(&script->state.mutex);
            script->state.stats.executions.gc++;
            Lck_Unlock(&script->state.mutex);
        }
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
        "local dbg=debug\n"
        "\n"
        "-- http://metalua.luaforge.net/src/lib/strict.lua.html\n"
        "setmetatable(_G, {\n"
        "  __index = function(table, key)\n"
        "    if dbg.getinfo(2) and dbg.getinfo(2, 'S').what ~= 'C' then\n"
        "      error('Script attempted to access nonexistent global variable \\'' .. tostring(key) .. '\\'')\n"
        "    end\n"
        "    return rawget(table, key)\n"
        "  end,\n"
        "  __newindex = function(table, key, value)\n"
        "    if dbg.getinfo(2) then\n"
        "      local w = dbg.getinfo(2, 'S').what\n"
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
        "varnish = readonly_table(varnish, {_ctx = true, _script = true})\n"
        "\n"
        "readonly_table = nil\n"
        "debug = nil\n";
    AZ(luaL_loadbuffer(L, protections, strlen(protections), "@enable_lua_protections"));
    AZ(lua_pcall(L, 0, 0, 0));
}

// Extract field from 'varnish.field'. Both 'varnish' table and user data are
// pushed into the stack and the removed.
#define GET_VARNISH_TABLE_FOO_FIELD(L, field, where, MAGIC) \
    do { \
        lua_getglobal(L, "varnish"); \
        AN(lua_istable(L, -1)); \
        lua_getfield(L, -1, field); \
        AN(lua_islightuserdata(L, -1)); \
        void *data = lua_touserdata(L, -1); \
        AN(data); \
        CAST_OBJ_NOTNULL(where, data, MAGIC); \
        lua_pop(L, 2); \
    } while (0)

#define GET_VARNISH_TABLE_CTX(L, where) \
    GET_VARNISH_TABLE_FOO_FIELD(L, "_ctx", where, VRT_CTX_MAGIC)

#define GET_VARNISH_TABLE_SCRIPT(L, where) \
    GET_VARNISH_TABLE_FOO_FIELD(L, "_script", where, VMOD_CFG_SCRIPT_MAGIC)

static int
varnish_log_lua_command(lua_State *L)
{
    // Extract input arguments.
    int argc = lua_gettop(L);
    if (argc < 1) {
        lua_pushstring(L, "varnish.log() requires one argument.");
        lua_error(L);
    }
    const char *message = lua_tostring(L, -1);

    // Check input arguments.
    if (message != NULL) {
        // Execute 'ctx = varnish._ctx'.
        VRT_CTX;
        GET_VARNISH_TABLE_CTX(L, ctx);

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
    const char *name = lua_tostring(L, -1 * argc);
    const char *where;
    if (argc >= 2) {
        where = lua_tostring(L, -1 * argc + 1);
    } else {
        where = "req";
    }

    // Check input arguments.
    if (name != NULL && strlen(name) > 0) {
        // Execute 'ctx = varnish._ctx'.
        VRT_CTX;
        GET_VARNISH_TABLE_CTX(L, ctx);

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
    const char *name = lua_tostring(L, -1 * argc);
    const char *value = lua_tostring(L, -1 * argc + 1);
    const char *where;
    if (argc >= 3) {
        where = lua_tostring(L, -1 * argc + 2);
    } else {
        where = "req";
    }

    // Check input arguments.
    if (name != NULL && strlen(name) > 0 &&
        value != NULL && strlen(value) > 0) {
        // Execute 'ctx = varnish._ctx'.
        VRT_CTX;
        GET_VARNISH_TABLE_CTX(L, ctx);

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
    const char *string = lua_tostring(L, -1 * argc);
    const char *regexp = lua_tostring(L, -1 * argc + 1);
    unsigned cache;
    if (argc >= 3) {
        cache = lua_toboolean(L, -1 * argc + 2);
    } else {
        cache = 1;
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL) {
        // Execute 'ctx = varnish._ctx' & 'script = varnish._script'.
        VRT_CTX;
        GET_VARNISH_TABLE_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_TABLE_SCRIPT(L, script);

        // Execute command.
        const char *error;
        result = varnish_regmatch_command(ctx, script, string, regexp, cache, &error);
        if (error != NULL) {
            lua_pushstring(L, error);
            lua_error(L);
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
    const char *string = lua_tostring(L, -1 * argc);
    const char *regexp = lua_tostring(L, -1 * argc + 1);
    const char *sub = lua_tostring(L, -1 * argc + 2);
    unsigned cache;
    if (argc >= 4) {
        cache = lua_toboolean(L, -1 * argc + 3);
    } else {
        cache = 1;
    }

    // Check input arguments.
    if (string != NULL && regexp != NULL && sub != NULL) {
        // Execute 'ctx = varnish._ctx' & 'script = varnish._script'.
        VRT_CTX;
        GET_VARNISH_TABLE_CTX(L, ctx);
        struct vmod_cfg_script *script;
        GET_VARNISH_TABLE_SCRIPT(L, script);

        // Execute command.
        const char *error;
        result = varnish_regsub_command(
            ctx, script, string, regexp, sub, cache, all, &error);
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
varnish_regsubone_lua_command(lua_State *L)
{
    return varnish_regsub_lua_command(L, 0);
}

static int
varnish_regsuball_lua_command(lua_State *L)
{
    return varnish_regsub_lua_command(L, 1);
}

#undef GET_VARNISH_TABLE_FOO_FIELD
#undef GET_VARNISH_TABLE_CTX
#undef GET_VARNISH_TABLE_SCRIPT

static lua_State *
new_context(VRT_CTX, struct vmod_cfg_script *script)
{
    // Create base context.
    lua_State *result = luaL_newstate();
    AN(result);

    // Load libraries & disable unsupported functions.
    load_lua_libs(script, result);
    remove_unsupported_lua_functions(script, result);

    // Add support for varnish._ctx, varnish._script, varnish._error_handler(),
    // varnish.log(), etc.
    lua_newtable(result);
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

    // Add a helper function for error reporting.
    // Note that when the error is in the C function we want to report the
    // information about the caller, that's what makes sense from the point of
    // view of the user debugging a script.
    char *error_handler =
        "local dbg = debug\n"
        "varnish._error_handler = function(error)\n"
        "  local i = dbg.getinfo(2, 'nSl')\n"
        "  if i and i.what == 'C' then\n"
        "    i = dbg.getinfo(3, 'nSl')\n"
        "  end\n"
        "  if i then\n"
        "    return i.source .. ':' .. i.currentline .. ': ' .. error\n"
        "  else\n"
        "    return error\n"
        "  end\n"
        "end\n";
    AZ(luaL_loadbuffer(result, error_handler, strlen(error_handler), "@varnish_error_handler"));
    AZ(lua_pcall(result, 0, 0, 0));

    // Protect accesses to global variables, set global 'varnish' table as
    // read only, disable the 'debug' library, etc.
    enable_lua_protections(result);

    // Done!
    return result;
}
