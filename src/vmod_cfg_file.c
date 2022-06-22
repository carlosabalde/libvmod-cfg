#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <curl/curl.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "cJSON.h"
#include "ini.h"

#include "helpers.h"
#include "remote.h"
#include "variables.h"

struct vmod_cfg_file {
    unsigned magic;
    #define VMOD_CFG_FILE_MAGIC 0x9774a43f

    const char *name;

    remote_t *remote;
    const char *name_delimiter;
    const char *value_delimiter;
    variables_t *(*parse)(VRT_CTX, struct vmod_cfg_file *, const char *, unsigned);

    struct {
        pthread_rwlock_t rwlock;
        variables_t *variables;
    } state;
};

struct file_parse_ctx {
    struct vmod_cfg_file *file;
    variables_t *variables;
};

/******************************************************************************
 * INI PARSER.
 *****************************************************************************/

struct file_ini_stream_ctx {
    const char *ptr;
    int left;
};

static char *
file_ini_stream_reader(char *str, int size, void *stream)
{
    struct file_ini_stream_ctx* ctx = (struct file_ini_stream_ctx*) stream;

    if (ctx->left <= 0) {
        return NULL;
    }

    int idx = 0;
    char newline = '\0';

    for (idx = 0; idx < size - 1; ++idx) {
        if (idx == ctx->left) {
            break;
        }

        if (ctx->ptr[idx] == '\n') {
            newline = '\n';
            break;
        } else if (ctx->ptr[idx] == '\r') {
            newline = '\r';
            break;
        }
    }
    memcpy(str, ctx->ptr, idx);
    str[idx] = '\0';

    ctx->ptr += idx + 1;
    ctx->left -= idx + 1;
    if (newline && (ctx->left > 0) && (ctx->ptr[0] == newline)) {
        ctx->left--;
        ctx->ptr++;
    }

    return str;
}

static int
file_parse_ini_handler(void *c, const char *section, const char *name, const char *value)
{
    struct file_parse_ctx *ctx = (struct file_parse_ctx *) c;

    char *buffer;
    unsigned flatten = (section != NULL) && (strlen(section) > 0);
    assert(asprintf(
        &buffer,
        "%s%s%s",
            flatten ? section : "",
            flatten ? ctx->file->name_delimiter : "",
            name) > 0);

    variable_t *variable = find_variable(ctx->variables, buffer);
    if (variable == NULL) {
        variable = new_global_variable(buffer, strlen(buffer), value);
        AZ(VRBT_INSERT(variables, ctx->variables, variable));
    } else {
        variable->value = realloc(
            variable->value,
            strlen(variable->value) + strlen(ctx->file->value_delimiter) + strlen(value) + 1);
        AN(variable->value);
        if (strlen(variable->value) > 0) {
            strcat(variable->value, ctx->file->value_delimiter);
        }
        strcat(variable->value, value);
    }

    free((void *) buffer);

    return 1;
}

static variables_t *
file_parse_ini(VRT_CTX, struct vmod_cfg_file *file, const char *contents, unsigned is_backup)
{
    variables_t *result = NULL;

    struct file_ini_stream_ctx file_ini_stream_ctx = {
        .ptr = contents,
        .left = strlen(contents)
    };

    struct file_parse_ctx file_parse_ctx = {
        .file = file,
        .variables = malloc(sizeof(variables_t))
    };
    AN(file_parse_ctx.variables);
    VRBT_INIT(file_parse_ctx.variables);

    int rc = ini_parse_stream(
        (ini_reader) file_ini_stream_reader, &file_ini_stream_ctx,
        file_parse_ini_handler, &file_parse_ctx);

    if (rc == 0) {
        result = file_parse_ctx.variables;

        LOG(ctx, LOG_INFO,
            "Remote successfully parsed (file=%s, location=%s, is_backup=%d, format=ini)",
            file->name, file->remote->location.raw, is_backup);
    } else {
        flush_global_variables(file_parse_ctx.variables);
        free((void *) file_parse_ctx.variables);

        LOG(ctx, LOG_ERR,
            "Failed to parse remote (file=%s, location=%s, is_backup=%d, format=ini, error=%d)",
            file->name, file->remote->location.raw, is_backup, rc);
    }

    return result;
}

/******************************************************************************
 * JSON PARSER.
 *****************************************************************************/

static void
file_parse_json_emit(struct file_parse_ctx *ctx, const char *prefix, cJSON *item)
{
    char *value = NULL;

    if (cJSON_IsFalse(item)) {
        value = "false";
    } else if (cJSON_IsTrue(item)) {
        value = "true";
    } else if (cJSON_IsNumber(item)) {
        double intpart;
        if (modf(item->valuedouble, &intpart) == 0) {
            assert(asprintf(&value, "%d", item->valueint) > 0);
        } else {
            assert(asprintf(&value, "%.3f", item->valuedouble) > 0);
        }
    } else if (cJSON_IsRaw(item) || cJSON_IsString(item)) {
        value = item->valuestring;
    }

    if (value != NULL) {
        char *name;
        AN(item->string);
        assert(asprintf(&name, "%s%s", prefix, item->string) > 0);

        variable_t *variable = new_global_variable(name, strlen(name), value);

        free((void *) name);
        if (cJSON_IsNumber(item)) {
            free((void *) value);
        }

        AZ(VRBT_INSERT(variables, ctx->variables, variable));
    }
}

static void
file_parse_json_walk(struct file_parse_ctx *ctx, cJSON *items, const char *prefix)
{
    assert(cJSON_IsObject(items));

    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        AN(item->string);

        if (cJSON_IsObject(item)) {
            char *new_prefix;
            assert(asprintf(
                &new_prefix, "%s%s%s",
                prefix,
                item->string,
                ctx->file->name_delimiter) >= 0);
            file_parse_json_walk(ctx, item, new_prefix);
            free((void *) new_prefix);
        } else {
            file_parse_json_emit(ctx, prefix, item);
        }
    }
}

static variables_t *
file_parse_json(VRT_CTX, struct vmod_cfg_file *file, const char *contents, unsigned is_backup)
{
    variables_t *result = NULL;

    struct file_parse_ctx file_parse_ctx = {
        .file = file,
        .variables = malloc(sizeof(variables_t))
    };
    AN(file_parse_ctx.variables);
    VRBT_INIT(file_parse_ctx.variables);

    const char *error;
    cJSON *root = cJSON_ParseWithOpts(contents, &error, 0);

    if (root != NULL) {
        if (root->type == cJSON_Object) {
            file_parse_json_walk(&file_parse_ctx, root, "");

            result = file_parse_ctx.variables;

            LOG(ctx, LOG_INFO,
                "Remote successfully parsed (file=%s, location=%s, is_backup=%d, format=json)",
                file->name, file->remote->location.raw, is_backup);
        } else {
            free((void *) file_parse_ctx.variables);

            LOG(ctx, LOG_ERR,
                "Unexpected JSON type (file=%s, location=%s, is_backup=%d, format=json, type=%d)",
                file->name, file->remote->location.raw, is_backup, root->type);
        }

        cJSON_Delete(root);
    } else {
        free((void *) file_parse_ctx.variables);

        LOG(ctx, LOG_ERR,
            "Failed to parse remote (file=%s, location=%s, is_backup=%d, format=json)",
            file->name, file->remote->location.raw, is_backup);
    }

    return result;
}

/******************************************************************************
 * BASICS.
 *****************************************************************************/

static unsigned
file_check_callback(VRT_CTX, void *ptr, char *contents, unsigned is_backup)
{
    unsigned result = 0;

    struct vmod_cfg_file *file;
    CAST_OBJ_NOTNULL(file, ptr, VMOD_CFG_FILE_MAGIC);

    variables_t *variables = (*file->parse)(ctx, file, contents, is_backup);
    if (variables != NULL) {
        AZ(pthread_rwlock_wrlock(&file->state.rwlock));
        variables_t *old = file->state.variables;
        file->state.variables = variables;
        AZ(pthread_rwlock_unlock(&file->state.rwlock));

        flush_global_variables(old);
        free((void *) old);

        result = 1;
    }

    return result;
}

static unsigned
file_check(VRT_CTX, struct vmod_cfg_file *file, unsigned force)
{
    return check_remote(ctx, file->remote, force, &file_check_callback, file);
}

#define SET_STRING(value, field) \
    do { \
        instance->field = strdup(value); \
        AN(instance->field); \
    } while (0)

VCL_VOID
vmod_file__init(
    VRT_CTX, struct vmod_cfg_file **file, const char *vcl_name,
    VCL_STRING location, VCL_STRING backup, VCL_INT period,
    VCL_BOOL ignore_load_failures, VCL_INT curl_connection_timeout,
    VCL_INT curl_transfer_timeout, VCL_BOOL curl_ssl_verify_peer,
    VCL_BOOL curl_ssl_verify_host, VCL_STRING curl_ssl_cafile,
    VCL_STRING curl_ssl_capath, VCL_STRING curl_proxy, VCL_ENUM format,
    VCL_STRING name_delimiter, VCL_STRING value_delimiter)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(file);
    AZ(*file);

    struct vmod_cfg_file *instance = NULL;

    if ((location != NULL) && (strlen(location) > 0) &&
        (period >= 0) &&
        (curl_connection_timeout >= 0) &&
        (curl_transfer_timeout >= 0) &&
        (name_delimiter != NULL) &&
        (value_delimiter != NULL)) {
        ALLOC_OBJ(instance, VMOD_CFG_FILE_MAGIC);
        AN(instance);

        instance->name = strdup(vcl_name);
        AN(instance->name);
        instance->remote = new_remote(
            location, backup, period, curl_connection_timeout, curl_transfer_timeout,
            curl_ssl_verify_peer, curl_ssl_verify_host, curl_ssl_cafile,
            curl_ssl_capath, curl_proxy);
        SET_STRING(name_delimiter, name_delimiter);
        SET_STRING(value_delimiter, value_delimiter);
        if (strcmp(format, "ini") == 0) {
            instance->parse = &file_parse_ini;
        } else if (strcmp(format, "json") == 0) {
            instance->parse = &file_parse_json;
        } else {
            WRONG("Illegal format value.");
        }
        AZ(pthread_rwlock_init(&instance->state.rwlock, NULL));
        instance->state.variables = malloc(sizeof(variables_t));
        AN(instance->state.variables);
        VRBT_INIT(instance->state.variables);

        if (!file_check(ctx, instance, 1) && !ignore_load_failures) {
            vmod_file__fini(&instance);
        }
    }

    *file = instance;
}

#undef SET_STRING

#define FREE_STRING(field) \
    do { \
        free((void *) instance->field); \
        instance->field = NULL; \
    } while (0)

VCL_VOID
vmod_file__fini(struct vmod_cfg_file **file)
{
    AN(file);
    AN(*file);

    struct vmod_cfg_file *instance = *file;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_FILE_MAGIC);

    free((void *) instance->name);
    instance->name = NULL;
    free_remote(instance->remote);
    instance->remote = NULL;
    FREE_STRING(name_delimiter);
    FREE_STRING(value_delimiter);
    instance->parse = NULL;
    AZ(pthread_rwlock_destroy(&instance->state.rwlock));
    flush_global_variables(instance->state.variables);
    free((void *) instance->state.variables);
    instance->state.variables = NULL;

    FREE_OBJ(instance);

    *file = NULL;
}

#undef FREE_STRING

VCL_BOOL
vmod_file_reload(VRT_CTX, struct vmod_cfg_file *file)
{
    return file_check(ctx, file, 1);
}

VCL_STRING
vmod_file_dump(VRT_CTX, struct vmod_cfg_file *file, VCL_BOOL stream, VCL_STRING prefix)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->state.rwlock));
    const char *result = dump_variables(ctx, file->state.variables, stream, prefix);
    AZ(pthread_rwlock_unlock(&file->state.rwlock));
    return result;
}

VCL_VOID
vmod_file_inspect(VRT_CTX, struct vmod_cfg_file *file)
{
    file_check(ctx, file, 0);
    inspect_remote(ctx, file->remote);
}

VCL_BOOL
vmod_file_is_set(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->state.rwlock));
    unsigned result = is_set_variable(ctx, file->state.variables, name);
    AZ(pthread_rwlock_unlock(&file->state.rwlock));
    return result;
}

VCL_STRING
vmod_file_get(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name, VCL_STRING fallback)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->state.rwlock));
    const char *result = get_variable(ctx, file->state.variables, name, fallback);
    AZ(pthread_rwlock_unlock(&file->state.rwlock));
    return result;
}
