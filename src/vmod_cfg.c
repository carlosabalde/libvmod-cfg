#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "ini.h"
#include "vtree.h"

#define LOG(ctx, slt, fmt, ...) \
    do { \
        if ((ctx)->vsl != NULL) \
            VSLb((ctx)->vsl, slt, "[CFG] " fmt, __VA_ARGS__); \
        else \
            VSL(slt, 0, "[CFG] " fmt, __VA_ARGS__); \
    } while (0)

typedef struct variable {
    unsigned magic;
    #define VARIABLE_MAGIC 0xcb181fe6
    const char *name;
    char *value;
    VRB_ENTRY(variable) tree;
} variable_t;

typedef VRB_HEAD(variables, variable) variables_t;

static int
variablecmp(const variable_t *v1, const variable_t *v2)
{
    return strcmp(v1->name, v2->name);
}

VRB_PROTOTYPE_STATIC(variables, variable, tree, variablecmp);
VRB_GENERATE_STATIC(variables, variable, tree, variablecmp);

static variable_t *new_variable(const char *name, size_t len, const char *value);
static void free_variable(variable_t *variable);

static variable_t *find_variable(variables_t *variables, const char *name);
static const char *copy_variable(VRT_CTX, variables_t *variables, const char *name);
static void flush_variables(variables_t *variables);

static unsigned version = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/******************************************************************************
 * VMOD EVENTS.
 *****************************************************************************/

int
event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    if (e == VCL_EVENT_WARM) {
        AZ(pthread_mutex_lock(&mutex));
        version++;
        AZ(pthread_mutex_unlock(&mutex));
    }

    return 0;
}

/******************************************************************************
 * ENV OBJECT.
 *****************************************************************************/

extern char **environ;

struct vmod_cfg_env {
    unsigned magic;
    #define VMOD_CFG_ENV 0x44baed10
    variables_t list;
};


static void
env_load(VRT_CTX, struct vmod_cfg_env *env)
{
    flush_variables(&env->list);
    for (int i = 0; environ[i]; i++) {
        char *ptr = strchr(environ[i], '=');
        if (ptr != NULL) {
            variable_t *variable = new_variable(environ[i], ptr - environ[i], ptr + 1);
            AZ(VRB_INSERT(variables, &env->list, variable));
        }
    }
}

VCL_VOID
vmod_env__init(VRT_CTX, struct vmod_cfg_env **env, const char *vcl_name)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(env);
    AZ(*env);

    struct vmod_cfg_env *instance;
    ALLOC_OBJ(instance, VMOD_CFG_ENV);
    AN(instance);

    VRB_INIT(&instance->list);

    env_load(ctx, instance);

    *env = instance;
}

VCL_VOID
vmod_env__fini(struct vmod_cfg_env **env)
{
    AN(env);
    AN(*env);

    struct vmod_cfg_env *instance = *env;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_ENV);

    flush_variables(&instance->list);

    *env = NULL;
}

VCL_BOOL
vmod_env_is_set(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name)
{
    if (name != NULL) {
        return find_variable(&env->list, name) != NULL;
    }
    return 0;
}

VCL_STRING
vmod_env_get(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name)
{
    if (name != NULL) {
        return copy_variable(ctx, &env->list, name);
    }
    return NULL;
}

/******************************************************************************
 * FILE OBJECT.
 *****************************************************************************/

struct vmod_cfg_file {
    unsigned magic;
    #define VMOD_CFG_FILE 0x9774a43f
    pthread_rwlock_t rwlock;
    const char *location;
    const char *name_delimiter;
    const char *value_delimiter;
    unsigned version;
    variables_t list;
    void (*load)(VRT_CTX, struct vmod_cfg_file *);
};

#include <syslog.h>

static int
ini_file_handler(void *user, const char *section, const char *name, const char *value)
{
    struct vmod_cfg_file *file;
    CAST_OBJ_NOTNULL(file, user, VMOD_CFG_FILE);

    unsigned len = strlen(name);
    if ((section != NULL) && (strlen(section) > 0)) {
        len += strlen(section) + strlen(file->name_delimiter);
    }
    char buffer[len + 1];
    if ((section != NULL) && (strlen(section) > 0)) {
        snprintf(buffer, len + 1, "%s%s%s", section, file->name_delimiter, name);
    } else {
        strcpy(buffer, name);
    }

    variable_t *variable = find_variable(&file->list, buffer);
    if (variable == NULL) {
        variable = new_variable(buffer, strlen(buffer), value);
        AZ(VRB_INSERT(variables, &file->list, variable));
    } else {
        variable->value = realloc(
            variable->value,
            strlen(variable->value) + strlen(file->value_delimiter) + strlen(value) + 1);
        AN(variable->value);
        if (strlen(variable->value) > 0) {
            strcat(variable->value, file->value_delimiter);
        }
        strcat(variable->value, value);
    }

    return 1;
}

static void
ini_file_load(VRT_CTX, struct vmod_cfg_file *file)
{
    flush_variables(&file->list);
    if (ini_parse(file->location, ini_file_handler, file) < 0) {
        LOG(
            ctx, SLT_Error,
            "Failed to parse configuration file (location=%s, format=ini)",
            file->location);
    }
}

VCL_VOID
vmod_file__init(
    VRT_CTX, struct vmod_cfg_file **file, const char *vcl_name,
    VCL_STRING location, VCL_ENUM format,
    VCL_STRING name_delimiter, VCL_STRING value_delimiter)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(file);
    AZ(*file);

    struct vmod_cfg_file *instance = NULL;

    if ((location != NULL) && (strlen(location) > 0) &&
        (name_delimiter != NULL) &&
        (value_delimiter != NULL)) {
        ALLOC_OBJ(instance, VMOD_CFG_FILE);
        AN(instance);

        AZ(pthread_rwlock_init(&instance->rwlock, NULL));
        instance->location = strdup(location);
        AN(instance->location);
        instance->name_delimiter = strdup(name_delimiter);
        AN(instance->name_delimiter);
        instance->value_delimiter = strdup(value_delimiter);
        AN(instance->value_delimiter);
        instance->version = version;
        VRB_INIT(&instance->list);
        if (strcmp(format, "ini") == 0) {
            instance->load = &ini_file_load;
        } else {
            WRONG("Illegal format value.");
        }

        (*instance->load)(ctx, instance);
    }

    *file = instance;
}

VCL_VOID
vmod_file__fini(struct vmod_cfg_file **file)
{
    AN(file);
    AN(*file);

    struct vmod_cfg_file *instance = *file;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_FILE);

    AZ(pthread_rwlock_destroy(&instance->rwlock));
    free((void *) instance->location);
    instance->location = NULL;
    free((void *) instance->name_delimiter);
    instance->name_delimiter = NULL;
    free((void *) instance->value_delimiter);
    instance->value_delimiter = NULL;
    instance->version = 0;
    flush_variables(&instance->list);
    instance->load = NULL;

    *file = NULL;
}

#define CHECK_VERSION() \
    do { \
        if (file->version != version) { \
            AZ(pthread_rwlock_wrlock(&file->rwlock)); \
            (*file->load)(ctx, file); \
            AZ(pthread_rwlock_unlock(&file->rwlock)); \
        } \
    } while (0)

VCL_BOOL
vmod_file_is_set(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name)
{
    if (name != NULL) {
        CHECK_VERSION();
        AZ(pthread_rwlock_rdlock(&file->rwlock));
        unsigned result = find_variable(&file->list, name) != NULL;
        AZ(pthread_rwlock_unlock(&file->rwlock));
        return result;
    }
    return 0;
}

VCL_STRING
vmod_file_get(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name)
{
    if (name != NULL) {
        CHECK_VERSION();
        AZ(pthread_rwlock_rdlock(&file->rwlock));
        const char *result = copy_variable(ctx, &file->list, name);
        AZ(pthread_rwlock_unlock(&file->rwlock));
        return result;
    }
    return NULL;
}

#undef CHECK_VERSION

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static variable_t *
new_variable(const char *name, size_t len, const char *value)
{
    variable_t *result;
    ALLOC_OBJ(result, VARIABLE_MAGIC);
    AN(result);

    result->name = strndup(name, len);
    AN(result->name);

    result->value = strdup(value);
    AN(result->value);

    return result;
}

static void
free_variable(variable_t *variable)
{
    free((void *) variable->name);
    variable->name = NULL;

    free((void *) variable->value);
    variable->value = NULL;

    FREE_OBJ(variable);
}

static variable_t *
find_variable(variables_t *variables, const char *name)
{
    variable_t variable;
    variable.name = name;
    return VRB_FIND(variables, variables, &variable);
}

static const char *
copy_variable(VRT_CTX, variables_t *variables, const char *name)
{
    const char *result = NULL;

    variable_t *variable = find_variable(variables, name);
    if (variable != NULL) {
        CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
        if (ctx->ws != NULL) {
            result = WS_Copy(ctx->ws, variable->value, -1);
            AN(result);
        } else {
            result = variable->value;
        }
    }

    return result;
}

static void
flush_variables(variables_t *variables)
{
    variable_t *variable, *tmp;
    VRB_FOREACH_SAFE(variable, variables, variables, tmp) {
        CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
        VRB_REMOVE(variables, variables, variable);
        free_variable(variable);
    }
}
