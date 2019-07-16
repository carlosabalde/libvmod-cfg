#ifndef CFG_SCRIPT_H_INCLUDED
#define CFG_SCRIPT_H_INCLUDED

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "vtree.h"

#include "duktape.h"
#include "remote.h"
#include "variables.h"
#include "helpers.h"

// Required lock ordering to avoid deadlocks:
//   1. vmod_cfg_script->state.mutex.
//   2. vmod_cfg_script->state.rwlock.

enum ENGINE_TYPE {
    ENGINE_TYPE_LUA,
    ENGINE_TYPE_JAVASCRIPT,
};

// regexp_t & regexps_t.

typedef struct regexp {
    unsigned magic;
    #define REGEXP_MAGIC 0xb581daff

    const char *text;
    vre_t *vre;

    VRBT_ENTRY(regexp) tree;
} regexp_t;

typedef VRBT_HEAD(regexps, regexp) regexps_t;

VRBT_PROTOTYPE(regexps, regexp, tree, regexpcmp);

// engine_t & engines_t.

typedef struct engine {
    unsigned magic;
    #define ENGINE_MAGIC 0xe4eed712

    enum ENGINE_TYPE type;
    union {
        lua_State *L;
        duk_context *D;
    } ctx;
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
    enum ENGINE_TYPE type;
    unsigned max_engines;
    unsigned max_cycles;
    unsigned min_gc_cycles;
    unsigned enable_sandboxing;
    union {
        struct {
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
        } javascript;
    } engine_cfg;

    struct {
        engine_t *(*new_engine)(VRT_CTX, struct vmod_cfg_script *script);
        int (*get_engine_used_memory)(engine_t *engine);
        int (*get_engine_stack_size)(engine_t *engine);
        unsigned (*execute)(VRT_CTX, struct vmod_cfg_script *script, const char *code, const char **name, int argc, const char *argv[], result_t *result, unsigned gc_collect, unsigned flush_jemalloc_tcache);
    } api;

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
            unsigned n;
            variables_t list;
        } variables;

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

engine_t *lock_engine(VRT_CTX, struct vmod_cfg_script *script);
void unlock_engine(VRT_CTX, struct vmod_cfg_script *script, engine_t *engine);
const char *new_function_name(const char *code);

task_state_t *new_task_state();
void reset_task_state(task_state_t *state);
void free_task_state(task_state_t *state);
task_state_t *get_task_state(VRT_CTX, struct vmod_priv *task_priv, unsigned reset);

engine_t *new_engine(enum ENGINE_TYPE type, void *ctx);
void free_engine(engine_t *engine);
void flush_engines(engines_t *engines);

regexp_t *new_regexp(const char *text, vre_t *vre);
void free_regexp(regexp_t *regexp);
vre_t * init_regexp(
    VRT_CTX, struct vmod_cfg_script *script,
    const char *regexp, unsigned cache);

void varnish_log_command(VRT_CTX, const char *message);
const char *varnish_get_header_command(
    VRT_CTX, const char *name, const char *where, const char **error);
void varnish_set_header_command(
    VRT_CTX, const char *name, const char *value, const char *where,
    const char **error);
unsigned varnish_regmatch_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *string,
    const char *regexp, unsigned cache, const char **error);
const char *varnish_regsub_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *string,
    const char *regexp, const char *sub, unsigned cache, unsigned all,
    const char **error);

const char *varnish_shared_get_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *key,
    unsigned is_locked);
void varnish_shared_set_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *key, const char *value,
    unsigned is_locked);
void varnish_shared_delete_command(
    VRT_CTX, struct vmod_cfg_script *script, const char *key,
    unsigned is_locked);

#endif
