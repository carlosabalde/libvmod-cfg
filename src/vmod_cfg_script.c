#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef JEMALLOC_TCACHE_FLUSH_ENABLED
    #include <jemalloc/jemalloc.h>
#endif

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vsb.h"
#include "vsha256.h"
#include "vre.h"
#include "vcc_cfg_if.h"

#include "helpers.h"
#include "remote.h"

// regexp_t & regexps_t.

typedef struct regexp {
    unsigned magic;
    #define REGEXP_MAGIC 0xb581daff

    const char *text;
    vre_t *vre;

    VRBT_ENTRY(regexp) tree;
} regexp_t;

typedef VRBT_HEAD(regexps, regexp) regexps_t;

static int
regexpcmp(const regexp_t *v1, const regexp_t *v2)
{
    return strcmp(v1->text, v2->text);
}

VRBT_PROTOTYPE(regexps, regexp, tree, regexpcmp);
VRBT_GENERATE(regexps, regexp, tree, regexpcmp);

// engine_t & engines_t.

typedef struct engine {
    unsigned magic;
    #define ENGINE_MAGIC 0xe4eed712

    lua_State *L;
    unsigned ncycles;
    int memory;

    VTAILQ_ENTRY(engine) list;
} engine_t;

typedef VTAILQ_HEAD(engines, engine) engines_t;

// result_t.

enum RESULT_VALUE_TYPE {
    RESULT_VALUE_TYPE_ERROR,
    RESULT_VALUE_TYPE_NIL,
    RESULT_VALUE_TYPE_BOOLEAN,
    RESULT_VALUE_TYPE_NUMBER,
    RESULT_VALUE_TYPE_STRING,
    RESULT_VALUE_TYPE_TABLE
};

typedef struct result_value {
    enum RESULT_VALUE_TYPE type;
    union {
        unsigned boolean;
        double number;
        const char *string;
    } value;
} result_value_t;

typedef struct result {
    int nvalues; // == -1: single value; >= 0: table with 'nvalues' values.
    #define MAX_RESULT_VALUES 128
    result_value_t values[MAX_RESULT_VALUES];
} result_t;

// task_state_t.

typedef struct task_state {
    unsigned magic;
    #define TASK_STATE_MAGIC 0x803118dd

    struct {
        // Allocated in workspace: code, argv[] & result.
        const char *code;
        int argc; // == -1: .init() not called; >= 0: 'argc' pushed arguments.
        #define MAX_EXECUTION_ARGS 128
        const char *argv[MAX_EXECUTION_ARGS];
        result_t result;
    } execution;
} task_state_t;

// struct vmod_cfg_script.

struct vmod_cfg_script {
    unsigned magic;
    #define VMOD_CFG_SCRIPT_MAGIC 0xed7302aa

    const char *name;

    remote_t *remote;
    struct {
        unsigned max_engines;
        unsigned max_cycles;
        unsigned min_gc_cycles;
        unsigned gc_step_size;
        struct {
            unsigned loadfile;
            unsigned dotfile;
        } functions;
        struct {
            unsigned package;
            unsigned io;
            unsigned os;
        } libraries;
    } lua;

    struct {
        struct lock mutex;
        pthread_rwlock_t rwlock;

        struct {
            const char *code;
            const char *name;
        } function;

        struct {
            pthread_cond_t cond;
            unsigned n;
            engines_t free;
            engines_t busy;
        } engines;

        struct {
            unsigned n;
            regexps_t list;
        } regexps;

        struct {
            struct {
                // Number of created scripting engines.
                unsigned total;
                // Number of scripting engines dropped.
                struct {
                    unsigned cycles;
                } dropped;
            } engines;
            struct {
                // Number of times some worker thread was blocked waiting for
                // a free scripting engine.
                unsigned blocked;
            } workers;
            struct {
                // Total number of executions (all of them, including failed
                // ones and syntax checks).
                unsigned total;
                // Number of executions registering new functions in the
                // scripting engine.
                unsigned unknown;
                // Number of failed executions.
                unsigned failed;
                // Number of executions triggering the garbage collector.
                unsigned gc;
            } executions;
        } stats;
    } state;
};

static vre_t * init_regexp(
    VRT_CTX, struct vmod_cfg_script *script,
    const char *regexp, unsigned cache);

/******************************************************************************
 * LUA HELPERS.
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
    if (script->lua.libraries.package) {
        load_lua_lib(L, LUA_LOADLIBNAME, luaopen_package);
    }
    if (script->lua.libraries.io) {
        load_lua_lib(L, LUA_IOLIBNAME, luaopen_io);
    }
    if (script->lua.libraries.os) {
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
    if (!script->lua.functions.loadfile) {
        remove_unsupported_lua_function(L, "loadfile");
    }
    if (!script->lua.functions.dotfile) {
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
lua_varnish_regmatch_command(lua_State *L)
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

        // Execute match.
        vre_t *re = init_regexp(ctx, script, regexp, cache);
        if (re != NULL) {
            result = VRT_re_match(ctx, string, re);
            if (!cache) {
                VRT_re_fini(re);
            }
        }
    }

    // Done!
    lua_pushboolean(L, result);
    return 1;
}

static int
lua_varnish_regsub_command(lua_State *L, unsigned all)
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

        // Execute regsub.
        vre_t *re = init_regexp(ctx, script, regexp, cache);
        if (re != NULL) {
            result = VRT_regsub(ctx, all, string, re, sub);
            if (!cache) {
                VRT_re_fini(re);
            }
        }
    }

    // Done!
    if (result !=  NULL) {
        lua_pushstring(L, result);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int
lua_varnish_regsubone_command(lua_State *L)
{
    return lua_varnish_regsub_command(L, 0);
}

static int
lua_varnish_regsuball_command(lua_State *L)
{
    return lua_varnish_regsub_command(L, 1);
}

static int
lua_varnish_log_command(lua_State *L)
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

        // Log.
        if (ctx->vsl != NULL) {
            VSLb(ctx->vsl, SLT_VCL_Log, "%s", message);
        } else {
            VSL(SLT_VCL_Log, 0, "%s", message);
        }
    }

    // Done!
    return 0;
}

#undef GET_VARNISH_TABLE_FOO_FIELD
#undef GET_VARNISH_TABLE_CTX
#undef GET_VARNISH_TABLE_SCRIPT

static lua_State *
new_L(VRT_CTX, struct vmod_cfg_script *script)
{
    lua_State *result = luaL_newstate();

    // Load libraries & disable unsupported functions.
    load_lua_libs(script, result);
    remove_unsupported_lua_functions(script, result);

    // Add support for varnish._ctx, varnish._script, varnish._error_handler(),
    // varnish.log(), etc.
    lua_newtable(result);
    lua_pushcfunction(result, lua_varnish_log_command);
    lua_setfield(result, -2, "log");
    lua_pushcfunction(result, lua_varnish_regmatch_command);
    lua_setfield(result, -2, "regmatch");
    lua_pushcfunction(result, lua_varnish_regsubone_command);
    lua_setfield(result, -2, "regsub");
    lua_pushcfunction(result, lua_varnish_regsuball_command);
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

    return result;
}

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static regexp_t *
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

static void
free_regexp(regexp_t *regexp)
{
    free((void *) regexp->text);
    regexp->text = NULL;

    VRT_re_fini(regexp->vre);
    regexp->vre = NULL;

    FREE_OBJ(regexp);
}

static engine_t *
new_engine(lua_State *L)
{
    engine_t *result;
    ALLOC_OBJ(result, ENGINE_MAGIC);
    AN(result);

    result->L = L;
    result->ncycles = 0;
    result->memory = lua_gc(L, LUA_GCCOUNT, 0);

    return result;
}

static void
free_engine(engine_t *engine)
{
    lua_close(engine->L);
    engine->L = NULL;
    engine->ncycles = 0;
    engine->memory = 0;

    FREE_OBJ(engine);
}

static void
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

static void
reset_task_state(task_state_t *state)
{
    state->execution.code = NULL;
    state->execution.argc = -1;
    memset(&state->execution.argv[0], 0, sizeof(state->execution.argv));
    memset(&state->execution.result, 0, sizeof(state->execution.result));
    state->execution.result.nvalues = -1;
}

static task_state_t *
new_task_state()
{
    task_state_t *result;
    ALLOC_OBJ(result, TASK_STATE_MAGIC);
    AN(result);

    reset_task_state(result);

    return result;
}

static void
free_task_state(task_state_t *state)
{
    reset_task_state(state);

    FREE_OBJ(state);
}

static task_state_t *
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

static unsigned
is_valid_engine(VRT_CTX, struct vmod_cfg_script *script, engine_t *engine)
{
    Lck_AssertHeld(&script->state.mutex);

    if ((script->lua.max_cycles > 0) &&
        (engine->ncycles >= script->lua.max_cycles)) {
        script->state.stats.engines.dropped.cycles++;
        return 0;
    }

    return 1;
}

static engine_t *
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
        if (script->state.engines.n >= script->lua.max_engines) {
            Lck_CondWait(&script->state.engines.cond, &script->state.mutex, 0);
            script->state.stats.workers.blocked++;
            goto retry;
        } else {
            result = new_engine(new_L(ctx, script));
            script->state.stats.engines.total++;
            VTAILQ_INSERT_TAIL(&script->state.engines.busy, result, list);
            script->state.engines.n++;
        }
    }

    Lck_Unlock(&script->state.mutex);

    AN(result);

    return result;
}

static void
unlock_engine(VRT_CTX, struct vmod_cfg_script *script, engine_t *engine)
{
    CHECK_OBJ_NOTNULL(engine, ENGINE_MAGIC);

    engine->memory = lua_gc(engine->L, LUA_GCCOUNT, 0);

    Lck_Lock(&script->state.mutex);
    VTAILQ_REMOVE(&script->state.engines.busy, engine, list);
    VTAILQ_INSERT_TAIL(&script->state.engines.free, engine, list);
    AZ(pthread_cond_signal(&script->state.engines.cond));
    Lck_Unlock(&script->state.mutex);
}

static const char *
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

static unsigned
pre_execute(
    VRT_CTX, struct vmod_cfg_script *script, engine_t *engine,
    const char *code, const char *name)
{
    unsigned sucess = 0;

    struct vsb *vsb = VSB_new_auto();
    AN(vsb);
    AZ(VSB_printf(vsb, "function %s()\n%s\nend", name, code));
    AZ(VSB_finish(vsb));

    if (!luaL_loadbuffer(engine->L, VSB_data(vsb), VSB_len(vsb), "@varnish_script")) {
        if (!lua_pcall(engine->L, 0, 0, 0)) {
            sucess = 1;
        } else {
            LOG(ctx, LOG_ERR,
                "Failed to execute new script (script=%s, function=%s, code=%.80s...): %s",
                script->name, name, code, lua_tostring(engine->L, -1));
            lua_pop(engine->L, 1);
        }
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to compile new script (script=%s, function=%s, code=%.80s...): %s",
            script->name, name, code, lua_tostring(engine->L, -1));
        lua_pop(engine->L, 1);
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
        where.value.boolean = (unsigned) lua_toboolean(engine->L, -1)

    #define STORE_NUMBER(where) \
        where.type = RESULT_VALUE_TYPE_NUMBER; \
        where.value.number = (double) lua_tonumber(engine->L, -1)

    #define STORE_STRING(where) \
        where.type = RESULT_VALUE_TYPE_STRING; \
        where.value.string = WS_Copy(ctx->ws, lua_tostring(engine->L, -1), -1); \
        AN(where.value.string)

    #define STORE_ERROR(where) \
        where.type = RESULT_VALUE_TYPE_ERROR

    switch (lua_type(engine->L, -1)) {
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
                lua_pushnumber(engine->L, result->nvalues + 1);
                lua_gettable(engine->L, -2);
                if (lua_type(engine->L, -1) == LUA_TNIL) {
                    lua_pop(engine->L, 1);
                    break;
                }
                switch (lua_type(engine->L, -1)) {
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
                            "Got invalid script result table value (script=%s, type=%d)",
                            script->name, lua_type(engine->L, -1));
                        STORE_ERROR(result->values[result->nvalues]);
                        break;

                }
                lua_pop(engine->L, 1);
                result->nvalues++;
                if (result->nvalues == MAX_RESULT_VALUES) {
                    LOG(ctx, LOG_ERR,
                        "Failed to store table value (script=%s, limit=%d)",
                        script->name, MAX_RESULT_VALUES);
                    break;
                }
            }
            return 1;

        // LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD, LUA_TLIGHTUSERDATA, etc.
        default:
            LOG(ctx, LOG_ERR,
                "Got invalid script result value (script=%s, type=%d)",
                script->name, lua_type(engine->L, -1));
            STORE_ERROR(result->values[0]);
            return 0;
    }

    #undef STORE_NIL
    #undef STORE_BOOLEAN
    #undef STORE_NUMBER
    #undef STORE_STRING
    #undef STORE_ERROR
}

static unsigned
execute(
    VRT_CTX, struct vmod_cfg_script *script,
    const char *code, const char **name,
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
    lua_getglobal(engine->L, "varnish");
    AN(lua_istable(engine->L, -1));
    lua_pushlightuserdata(engine->L, (struct vrt_ctx *) ctx);
    lua_setfield(engine->L, -2, "_ctx");
    lua_pushlightuserdata(engine->L, (struct vmod_cfg_script *) script);
    lua_setfield(engine->L, -2, "_script");

    // Push the value of 'varnish._error_handler' into the stack. This will
    // keep the 'varnish' table in the stack, just under the the error handler
    // function.
    lua_getfield(engine->L, -1, "_error_handler");
    AN(lua_isfunction(engine->L, -1));

    // Try to lookup the function to be executed. Result will be pushed
    // into the stack.
    lua_getglobal(engine->L, *name);
    if (!lua_isfunction(engine->L, -1)) {
        // Remove the non-function value from the stack.
        lua_pop(engine->L, 1);

        // Update stats.
        Lck_Lock(&script->state.mutex);
        script->state.stats.executions.unknown++;
        Lck_Unlock(&script->state.mutex);

        // Compile & register the function to be executed.
        if (pre_execute(ctx, script, engine, code, *name)) {
            // Push the function to be executed into the stack.
            lua_getglobal(engine->L, *name);
            AN(lua_isfunction(engine->L, -1));
        } else {
            // Remove the error handler from the stack.
            lua_pop(engine->L, 1);

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
        lua_newtable(engine->L);
        for (int i = 0; i < argc; i++) {
            lua_pushstring(engine->L, argv[i]);
            lua_rawseti(engine->L, -2, i);
        }
        lua_setglobal(engine->L, "ARGV");

        // At this point whether the function was never seen before or if it was
        // already defined, we can call it. We have zero arguments and expect
        // a single return value.
        if (!lua_pcall(engine->L, 0, 1, -2)) {
            if (post_execute(ctx, script, engine, result)) {
                success = 1;
            } else {
                result->values[0].type = RESULT_VALUE_TYPE_ERROR;
                LOG(ctx, LOG_ERR,
                    "Failed to process script result (script=%s, function=%s, code=%.80s...)",
                    script->name, *name, code);
            }
        } else {
            result->values[0].type = RESULT_VALUE_TYPE_ERROR;
            LOG(ctx, LOG_ERR,
                "Failed to execute script (script=%s, function=%s, code=%.80s...): %s",
                script->name, *name, code, lua_tostring(engine->L, -1));
        }

        // Remove the function result and the error handler from the stack.
        lua_pop(engine->L, 2);
    } else {
        // Everything looks correct. Full execution is not required: simply
        // remove function to be executed & error handler from the stack.
        success = 1;
        lua_pop(engine->L, 2);
    }

done:
    // Current state of the stack at this point (top to bottom):
    //   - 'varnish' table.

    // Execute 'varnish._ctx = nil' and 'varnish._script = nil', then
    // remove 'varnish' table from the stack.
    lua_pushnil(engine->L);
    lua_setfield(engine->L, -2, "_ctx");
    lua_pushnil(engine->L);
    lua_setfield(engine->L, -2, "_script");
    lua_pop(engine->L, 1);

    // Update stats.
    Lck_Lock(&script->state.mutex);
    script->state.stats.executions.total++;
    if (!success) {
        script->state.stats.executions.failed++;
    }
    Lck_Unlock(&script->state.mutex);

    // Call the garbage collector from time to time to avoid a full cycle
    // performed by Lua, which adds too latency.
    engine->ncycles++;
    if (gc_collect) {
        lua_gc(engine->L, LUA_GCCOLLECT, 0);
        Lck_Lock(&script->state.mutex);
        script->state.stats.executions.gc++;
        Lck_Unlock(&script->state.mutex);
    } else {
        if (engine->ncycles % script->lua.min_gc_cycles == 0) {
            lua_gc(engine->L, LUA_GCSTEP, script->lua.gc_step_size);
            Lck_Lock(&script->state.mutex);
            script->state.stats.executions.gc++;
            Lck_Unlock(&script->state.mutex);
        }
    }

    // Flush calling thread's jemalloc tcache in order to keep memory usage
    // controlled. No required when using LuaJIT.
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

static vre_t *
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
        AZ(pthread_rwlock_rdlock(&script->state.rwlock));
        regexp_t *cached_regexp = VRBT_FIND(regexps, &script->state.regexps.list, &search_regexp);
        if (cached_regexp != NULL) {
            result = cached_regexp->vre;
        }
        AZ(pthread_rwlock_unlock(&script->state.rwlock));
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
            AZ(pthread_rwlock_wrlock(&script->state.rwlock));
            regexp_t *cached_regexp = VRBT_FIND(regexps, &script->state.regexps.list, a_regexp);
            if (cached_regexp == NULL) {
                AZ(VRBT_INSERT(regexps, &script->state.regexps.list, a_regexp));
                script->state.regexps.n++;
            } else {
                free_regexp(a_regexp);
                result = cached_regexp->vre;
            }
            AZ(pthread_rwlock_unlock(&script->state.rwlock));
        }
    }

    // Done!
    return result;
}

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
    if (execute(ctx, script, contents, &name, 0, NULL, NULL, 0, 1)) {
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

static const char *
get_result(VRT_CTX, result_value_t *result_value)
{
    const char *value;

    switch (result_value->type) {
        case RESULT_VALUE_TYPE_BOOLEAN:
            value = WS_Copy(ctx->ws, result_value->value.boolean ? "true" : "false", -1);
            AN(value);
            return value;

        case RESULT_VALUE_TYPE_NUMBER:
            value = WS_Printf(ctx->ws, "%g", result_value->value.number);
            AN(value);
            return value;

        case RESULT_VALUE_TYPE_STRING:
            return result_value->value.string;

        default:
            return NULL;
    }
}

static uint64_t
engines_memory(VRT_CTX, struct vmod_cfg_script *script, unsigned is_locked)
{
    if (is_locked) {
        Lck_AssertHeld(&script->state.mutex);
    } else {
        Lck_Lock(&script->state.mutex);
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

VCL_VOID
vmod_script__init(
    VRT_CTX, struct vmod_cfg_script **script, const char *vcl_name,
    VCL_STRING location, VCL_STRING backup, VCL_INT period,
    VCL_INT lua_max_engines, VCL_INT lua_max_cycles,
    VCL_INT lua_min_gc_cycles, VCL_INT lua_gc_step_size,
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
        (lua_max_engines > 0) &&
        (lua_max_cycles >= 0) &&
        (lua_min_gc_cycles > 0) &&
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
        instance->lua.max_engines = lua_max_engines;
        instance->lua.max_cycles = lua_max_cycles;
        instance->lua.min_gc_cycles = lua_min_gc_cycles;
        instance->lua.gc_step_size = lua_gc_step_size;
        instance->lua.functions.loadfile = !lua_remove_loadfile_function;
        instance->lua.functions.dotfile = !lua_remove_dotfile_function;
        instance->lua.libraries.package = lua_load_package_lib;
        instance->lua.libraries.io = lua_load_io_lib;
        instance->lua.libraries.os = lua_load_os_lib;
        Lck_New(&instance->state.mutex, vmod_state.locks.script);
        AZ(pthread_rwlock_init(&instance->state.rwlock, NULL));
        instance->state.function.code = NULL;
        instance->state.function.name = NULL;
        AZ(pthread_cond_init(&instance->state.engines.cond, NULL));
        instance->state.engines.n = 0;
        VTAILQ_INIT(&instance->state.engines.free);
        VTAILQ_INIT(&instance->state.engines.busy);
        instance->state.regexps.n = 0;
        VRBT_INIT(&instance->state.regexps.list);
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
    instance->lua.max_engines = 0;
    instance->lua.max_cycles = 0;
    instance->lua.min_gc_cycles = 0;
    instance->lua.gc_step_size = 0;
    instance->lua.functions.loadfile = 0;
    instance->lua.functions.dotfile = 0;
    instance->lua.libraries.package = 0;
    instance->lua.libraries.io = 0;
    instance->lua.libraries.os = 0;
    Lck_Delete(&instance->state.mutex);
    AZ(pthread_rwlock_destroy(&instance->state.rwlock));
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
    instance->state.regexps.n = 0;
    regexp_t *regexp, *regexp_tmp;
    VRBT_FOREACH_SAFE(regexp, regexps, &instance->state.regexps.list, regexp_tmp) {
        CHECK_OBJ_NOTNULL(regexp, REGEXP_MAGIC);
        VRBT_REMOVE(regexps, &instance->state.regexps.list, regexp);
        free_regexp(regexp);
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
            AN(arg);
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
            } else {
                AN(code);
                AN(name);
            }
        } else {
            code = state->execution.code;
            name = NULL;
        }

        // Execute the script.
        if (!execute(
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
}

VMOD_SCRIPT_TABLE_RESULT_IS_FOO(error, ERROR)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(nil, NIL)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(boolean, BOOLEAN)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(number, NUMBER)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(string, STRING)
VMOD_SCRIPT_TABLE_RESULT_IS_FOO(table, TABLE)

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

VCL_VOID
vmod_script_free_result(
    VRT_CTX, struct vmod_cfg_script *script, struct vmod_priv *task_priv)
{
    get_task_state(ctx, task_priv, 1);
}

VCL_STRING
vmod_script_stats(VRT_CTX, struct vmod_cfg_script *script)
{
    Lck_Lock(&script->state.mutex);
    AZ(pthread_rwlock_rdlock(&script->state.rwlock));
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
        script->state.stats.workers.blocked,
        script->state.stats.executions.total,
        script->state.stats.executions.unknown,
        script->state.stats.executions.failed,
        script->state.stats.executions.gc);
    AZ(pthread_rwlock_unlock(&script->state.rwlock));
    Lck_Unlock(&script->state.mutex);
    AN(result);
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
