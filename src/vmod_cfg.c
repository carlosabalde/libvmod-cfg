#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <math.h>
#include <curl/curl.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_cfg_if.h"

#include "cJSON.h"
#include "ini.h"
#include "vtree.h"

#define LOG(ctx, level, fmt, ...) \
    do { \
        syslog(level, "[CFG][%s] " fmt, __func__, __VA_ARGS__); \
        unsigned slt; \
        if (level <= LOG_ERR) { \
            slt = SLT_VCL_Error; \
        } else { \
            slt = SLT_VCL_Log; \
        } \
        if ((ctx)->vsl != NULL) { \
            VSLb((ctx)->vsl, slt, "[CFG][%s] " fmt, __func__, __VA_ARGS__); \
        } else { \
            VSL(slt, 0, "[CFG][%s] " fmt, __func__, __VA_ARGS__); \
        } \
    } while (0)

#define FAIL_WS(ctx, result) \
    do { \
        syslog(LOG_ALERT, "[CFG][%s] Workspace overflow (line=%d)", __func__, __LINE__); \
        VRT_fail(ctx, "[CFG][%s] Workspace overflow (line=%d)", __func__, __LINE__); \
        return result; \
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
static void flush_variables(variables_t *variables);

static unsigned version = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *json_hex_chars = "0123456789abcdef";

/******************************************************************************
 * VMOD EVENTS.
 *****************************************************************************/

int
event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    switch (e) {
        case VCL_EVENT_LOAD:
            curl_global_init(CURL_GLOBAL_ALL);
            break;

        case VCL_EVENT_WARM:
            AZ(pthread_mutex_lock(&mutex));
            version++;
            AZ(pthread_mutex_unlock(&mutex));
            break;

        default:
            break;
    }

    return 0;
}

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static unsigned
cfg_is_set(VRT_CTX, variables_t *variables, const char *name)
{
    AN(ctx->ws);
    if (name != NULL) {
        return find_variable(variables, name) != NULL;
    }
    return 0;
}

static const char *
cfg_get(VRT_CTX, variables_t *variables, const char *name, const char *fallback)
{
    AN(ctx->ws);
    const char *result = fallback;

    if (name != NULL) {
        variable_t *variable = find_variable(variables, name);
        if (variable != NULL) {
            CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
            result = variable->value;
        }
    }

    if (result != NULL) {
        result = WS_Copy(ctx->ws, result, -1);
        if (result == NULL) {
            FAIL_WS(ctx, NULL);
        }
    }

    return result;
}

#define DUMP_CHAR(value) \
    do { \
        *end = value; \
        if (free_ws <= 0) { \
            WS_Release(ctx->ws, 0); \
            FAIL_WS(ctx, NULL); \
        } \
        end++; \
        free_ws--; \
    } while (0)

#define DUMP_STRING(value) \
    do { \
        DUMP_CHAR('"'); \
        for (int i = 0; value[i]; i++) { \
            if (value[i] > 31 && value[i] != '\"' && value[i] != '\\') { \
                DUMP_CHAR(value[i]); \
            } else { \
                DUMP_CHAR('\\'); \
                switch (value[i]) { \
                    case '\\': \
                        DUMP_CHAR('\\'); \
                        break; \
                    \
                    case '"': \
                        DUMP_CHAR('"'); \
                        break; \
                    \
                    case '\b': \
                        DUMP_CHAR('b'); \
                        break; \
                    \
                    case '\f': \
                        DUMP_CHAR('f'); \
                        break; \
                    \
                    case '\n': \
                        DUMP_CHAR('n'); \
                        break; \
                    \
                    case '\r': \
                        DUMP_CHAR('r'); \
                        break; \
                    \
                    case '\t': \
                        DUMP_CHAR('t'); \
                        break; \
                    \
                    default: \
                        DUMP_CHAR('u'); \
                        DUMP_CHAR('0'); \
                        DUMP_CHAR('0'); \
                        DUMP_CHAR(json_hex_chars[(value[i] >> 4) & 0xf]); \
                        DUMP_CHAR(json_hex_chars[value[i] & 0xf]); \
                        break; \
                } \
            } \
        } \
        DUMP_CHAR('"'); \
    } while (0)

static const char *
cfg_dump(VRT_CTX, variables_t *variables)
{
    AN(ctx->ws);
    char *result, *end;
    variable_t *variable;
    unsigned free_ws = WS_Reserve(ctx->ws, 0);
    if (free_ws <= 0) {
        WS_Release(ctx->ws, 0);
        FAIL_WS(ctx, NULL);
    }
    result = end = WS_Front(ctx->ws);

    DUMP_CHAR('{');
    VRB_FOREACH(variable, variables, variables) {
        CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
        DUMP_STRING(variable->name);
        DUMP_CHAR(':');
        DUMP_STRING(variable->value);
        DUMP_CHAR(',');
    }
    if (*(end - 1) == ',') {
        *(end - 1) = '}';
    } else {
        DUMP_CHAR('}');
    }
    *end = '\0';

    WS_Release(ctx->ws, end - result + 1);

    return result;
}

#undef DUMP_CHAR
#undef DUMP_STRING

/******************************************************************************
 * ENV OBJECT.
 *****************************************************************************/

extern char **environ;

struct vmod_cfg_env {
    unsigned magic;
    #define VMOD_CFG_ENV 0x44baed10
    variables_t variables;
};

static void
env_load(VRT_CTX, struct vmod_cfg_env *env)
{
    flush_variables(&env->variables);
    for (int i = 0; environ[i]; i++) {
        char *ptr = strchr(environ[i], '=');
        if (ptr != NULL) {
            variable_t *variable = new_variable(environ[i], ptr - environ[i], ptr + 1);
            AZ(VRB_INSERT(variables, &env->variables, variable));
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

    VRB_INIT(&instance->variables);

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

    flush_variables(&instance->variables);

    FREE_OBJ(instance);

    *env = NULL;
}

VCL_BOOL
vmod_env_is_set(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name)
{
    return cfg_is_set(ctx, &env->variables, name);
}

VCL_STRING
vmod_env_get(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name, VCL_STRING fallback)
{
    return cfg_get(ctx, &env->variables, name, fallback);
}

VCL_STRING
vmod_env_dump(VRT_CTX, struct vmod_cfg_env *env)
{
    return cfg_dump(ctx, &env->variables);
}

/******************************************************************************
 * FILE OBJECT.
 *****************************************************************************/

struct vmod_cfg_file {
    unsigned magic;
    #define VMOD_CFG_FILE 0x9774a43f
    struct {
        const char *raw;
        const char *parsed;
    } location;
    unsigned period;
    struct {
        unsigned connection_timeout;
        unsigned transfer_timeout;
        unsigned ssl_verify_peer;
        unsigned ssl_verify_host;
        const char *ssl_cafile;
        const char *ssl_capath;
        const char *proxy;
    } curl;
    const char *name_delimiter;
    const char *value_delimiter;
    const char *(*read)(VRT_CTX, struct vmod_cfg_file *);
    variables_t *(*parse)(VRT_CTX, struct vmod_cfg_file *, const char *);
    struct {
        pthread_mutex_t mutex;
        unsigned reloading;

        pthread_rwlock_t rwlock;
        unsigned version;
        time_t tst;
        variables_t *variables;
    } state;
};

static const char *
file_read_path(VRT_CTX, struct vmod_cfg_file *file)
{
    char *result = NULL;

    FILE *fp = fopen(file->location.parsed, "rb");
    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        unsigned long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        result = malloc(fsize + 1);
        AN(result);
        size_t nitems = fread(result, 1, fsize, fp);
        fclose(fp);
        result[fsize] = '\0';

        if (nitems != fsize) {
            free((void *) result);
            result = NULL;

            LOG(ctx, LOG_ERR,
                "Failed to read configuration file (location=%s)",
                file->location.raw);
        }
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to open configuration file (location=%s)",
            file->location.raw);
    }

    return result;
}

struct file_read_url_ctx {
    char *body;
    size_t bodylen;
};

static size_t
file_read_url_body(void *block, size_t size, size_t nmemb, void *c)
{
    struct file_read_url_ctx *ctx = (struct file_read_url_ctx *) c;

    size_t block_size = size * nmemb;
    ctx->body = realloc(ctx->body, ctx->bodylen + block_size + 1);
    AN(ctx->body);

    memcpy(&(ctx->body[ctx->bodylen]), block, block_size);
    ctx->bodylen += block_size;
    ctx->body[ctx->bodylen] = '\0';

    return block_size;
}

static const char *
file_read_url(VRT_CTX, struct vmod_cfg_file *file)
{
    struct file_read_url_ctx file_read_url_ctx = {
        .body = strdup(""),
        .bodylen = 0
    };
    AN(file_read_url_ctx.body);

    CURL *ch = curl_easy_init();
    AN(ch);
    curl_easy_setopt(ch, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(ch, CURLOPT_URL, file->location.parsed);
    curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(ch, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, file_read_url_body);
    curl_easy_setopt(ch, CURLOPT_WRITEDATA, &file_read_url_ctx);
    if (file->curl.connection_timeout > 0) {
#ifdef HAVE_CURLOPT_CONNECTTIMEOUT_MS
        curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT_MS, file->curl.connection_timeout);
#else
        curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT, file->curl.connection_timeout / 1000);
#endif
    }
    if (file->curl.transfer_timeout > 0) {
#ifdef HAVE_CURLOPT_TIMEOUT_MS
        curl_easy_setopt(ch, CURLOPT_TIMEOUT_MS, file->curl.transfer_timeout);
#else
        curl_easy_setopt(ch, CURLOPT_TIMEOUT, file->curl.transfer_timeout / 1000);
#endif
    }
    if (file->curl.ssl_verify_peer) {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 1L);
    } else {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
    }
    if (file->curl.ssl_verify_host) {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 1L);
    } else {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (file->curl.ssl_cafile != NULL) {
        curl_easy_setopt(ch, CURLOPT_CAINFO, file->curl.ssl_cafile);
    }
    if (file->curl.ssl_capath != NULL) {
        curl_easy_setopt(ch, CURLOPT_CAPATH, file->curl.ssl_capath);
    }
    if (file->curl.proxy != NULL) {
        curl_easy_setopt(ch, CURLOPT_PROXY, file->curl.proxy);
    }

    char *result = NULL;
    CURLcode cr = curl_easy_perform(ch);
    if (cr == CURLE_OK) {
        long status;
        curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &status);
        if (status == 200) {
            result = file_read_url_ctx.body;
        } else {
            LOG(ctx, LOG_ERR,
                "Failed to fetch configuration file (location=%s, status=%ld)",
                file->location.raw, status);
        }
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to fetch configuration file (location=%s): %s",
            file->location.raw, curl_easy_strerror(cr));
    }

    if (result == NULL) {
        free((void *) file_read_url_ctx.body);
    }

    curl_easy_cleanup(ch);
    return result;
}

struct file_parse_ctx {
    struct vmod_cfg_file *file;
    variables_t *variables;
};

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
        variable = new_variable(buffer, strlen(buffer), value);
        AZ(VRB_INSERT(variables, ctx->variables, variable));
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
file_parse_ini(VRT_CTX, struct vmod_cfg_file *file, const char *contents)
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
    VRB_INIT(file_parse_ctx.variables);

    int rc = ini_parse_stream(
        (ini_reader) file_ini_stream_reader, &file_ini_stream_ctx,
        file_parse_ini_handler, &file_parse_ctx);

    if (rc == 0) {
        result = file_parse_ctx.variables;

        LOG(ctx, LOG_INFO,
            "Configuration file successfully parsed (location=%s, format=ini)",
            file->location.raw);
    } else {
        flush_variables(file_parse_ctx.variables);
        free((void *) file_parse_ctx.variables);

        LOG(ctx, LOG_ERR,
            "Failed to parse configuration file (location=%s, format=ini, error=%d)",
            file->location.raw, rc);
    }

    return result;
}

static void
file_parse_json_emit(struct file_parse_ctx *ctx, const char *name, cJSON *item)
{
    char *value = NULL;
    double intpart;

    switch (item->type) {
        case cJSON_False:
            value = "false";
            break;

        case cJSON_True:
            value = "true";
            break;

        case cJSON_Number:
            if (modf(item->valuedouble, &intpart) == 0) {
                assert(asprintf(&value, "%d", item->valueint) > 0);
            } else {
                assert(asprintf(&value, "%.3f", item->valuedouble) > 0);
            }
            break;

        case cJSON_Raw:
        case cJSON_String:
            value = item->valuestring;
            break;
    }

    if (value != NULL) {
        variable_t *variable = new_variable(name, strlen(name), value);
        AZ(VRB_INSERT(variables, ctx->variables, variable));
        if (item->type == cJSON_Number) {
            free((void *) value);
        }
    }
}

static void
file_parse_json_walk(
    struct file_parse_ctx *ctx, cJSON *item, const char *prefix, unsigned delimit)
{
    char *buffer;

    while (item) {
        if (item->type != cJSON_Array) {
            assert(asprintf(
                &buffer, "%s%s%s",
                prefix,
                (delimit && item->string != NULL) ? ctx->file->name_delimiter: "",
                (item->string != NULL) ? item->string : "") >= 0);

            if (item->type != cJSON_Object) {
                file_parse_json_emit(ctx, buffer, item);
            } else if (item->child) {
                file_parse_json_walk(ctx, item->child, buffer, item->string != NULL);
            }

            free((void *) buffer);
        }

        item = item->next;
    }
}

static variables_t *
file_parse_json(VRT_CTX, struct vmod_cfg_file *file, const char *contents)
{
    variables_t *result = NULL;

    struct file_parse_ctx file_parse_ctx = {
        .file = file,
        .variables = malloc(sizeof(variables_t))
    };
    AN(file_parse_ctx.variables);
    VRB_INIT(file_parse_ctx.variables);

    const char *error;
    cJSON *root = cJSON_ParseWithOpts(contents, &error, 0);

    if (root != NULL) {
        if (root->type == cJSON_Object) {
            file_parse_json_walk(&file_parse_ctx, root, "", 0);

            result = file_parse_ctx.variables;

            LOG(ctx, LOG_INFO,
                "Configuration file successfully parsed (location=%s, format=json)",
                file->location.raw);
        } else {
            free((void *) file_parse_ctx.variables);

            LOG(ctx, LOG_ERR,
                "Unexpected JSON type (location=%s, format=json, type=%d)",
                file->location.raw, root->type);
        }

        cJSON_Delete(root);
    } else {
        free((void *) file_parse_ctx.variables);

        LOG(ctx, LOG_ERR,
            "Failed to parse configuration file (location=%s, format=json)",
            file->location.raw);
    }

    return result;
}

static unsigned
file_check(VRT_CTX, struct vmod_cfg_file *file, unsigned force)
{
    unsigned result = 0;

    unsigned winner = 0;
    time_t now = time(NULL);

    if (!force &&
        (!file->state.reloading &&
         ((file->state.version != version) ||
         ((file->period > 0) && (now - file->state.tst > file->period))))) {
        AZ(pthread_mutex_lock(&file->state.mutex));
        if (!file->state.reloading) {
            file->state.reloading = 1;
            winner = 1;
        }
        AZ(pthread_mutex_unlock(&file->state.mutex));
    }

    if (force || winner) {
        const char *contents = (*file->read)(ctx, file);
        if (contents != NULL) {
            variables_t *variables = (*file->parse)(ctx, file, contents);
            if (variables != NULL) {
                variables_t *old = file->state.variables;

                AZ(pthread_rwlock_wrlock(&file->state.rwlock));
                file->state.variables = variables;
                file->state.version = version;
                file->state.tst = now;
                AZ(pthread_rwlock_unlock(&file->state.rwlock));

                flush_variables(old);
                free((void *) old);

                result = 1;
            }
            free((void *) contents);
        }

        if (!force) {
            AZ(pthread_mutex_lock(&file->state.mutex));
            file->state.reloading = 0;
            AZ(pthread_mutex_unlock(&file->state.mutex));
        }
    } else {
        result = 1;
    }

    return result;
}

#define SET_STRING(value, field) \
    do { \
        instance->field = strdup(value); \
        AN(instance->field); \
    } while (0)

#define SET_OPTINAL_STRING(value, field) \
    do { \
        if ((value != NULL) && (strlen(value) > 0)) { \
            SET_STRING(value, field); \
        } else { \
            instance->field = NULL; \
        } \
    } while (0)

#define SET_LOCATION(type, offset) \
    do { \
        SET_STRING(location + offset, location.parsed); \
        instance->read = &file_read_ ## type; \
    } while (0)

VCL_VOID
vmod_file__init(
    VRT_CTX, struct vmod_cfg_file **file, const char *vcl_name,
    VCL_STRING location, VCL_INT period,
    VCL_INT curl_connection_timeout, VCL_INT curl_transfer_timeout,
    VCL_BOOL curl_ssl_verify_peer, VCL_BOOL curl_ssl_verify_host,
    VCL_STRING curl_ssl_cafile, VCL_STRING curl_ssl_capath,
    VCL_STRING curl_proxy, VCL_ENUM format,
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
        ALLOC_OBJ(instance, VMOD_CFG_FILE);
        AN(instance);

        SET_STRING(location, location.raw);
        if (strncmp(location, "file://", 7) == 0) {
            SET_LOCATION(path, 7);
        } else if (strncmp(location, "http://", 7) == 0) {
            SET_LOCATION(url, 7);
        } else if (strncmp(location, "https://", 8) == 0) {
            SET_LOCATION(url, 8);
        } else {
            SET_LOCATION(path, 0);
        }
        instance->period = period;
        instance->curl.connection_timeout = curl_connection_timeout;
        instance->curl.transfer_timeout = curl_transfer_timeout;
        instance->curl.ssl_verify_peer = curl_ssl_verify_peer;
        instance->curl.ssl_verify_host = curl_ssl_verify_host;
        SET_OPTINAL_STRING(curl_ssl_cafile, curl.ssl_cafile);
        SET_OPTINAL_STRING(curl_ssl_capath, curl.ssl_capath);
        SET_OPTINAL_STRING(curl_proxy, curl.proxy);
        SET_STRING(name_delimiter, name_delimiter);
        SET_STRING(value_delimiter, value_delimiter);
        if (strcmp(format, "ini") == 0) {
            instance->parse = &file_parse_ini;
        } else if (strcmp(format, "json") == 0) {
            instance->parse = &file_parse_json;
        } else {
            WRONG("Illegal format value.");
        }
        AZ(pthread_mutex_init(&instance->state.mutex, NULL));
        instance->state.reloading = 0;
        AZ(pthread_rwlock_init(&instance->state.rwlock, NULL));
        instance->state.version = version;
        instance->state.tst = 0;
        instance->state.variables = malloc(sizeof(variables_t));
        AN(instance->state.variables);
        VRB_INIT(instance->state.variables);

        file_check(ctx, instance, 1);
    }

    *file = instance;
}

#undef SET_STRING
#undef SET_OPTINAL_STRING
#undef SET_LOCATION

#define FREE_STRING(field) \
    do { \
        free((void *) instance->field); \
        instance->field = NULL; \
    } while (0)

#define FREE_OPTINAL_STRING(field) \
    do { \
        if (instance->field != NULL) { \
            FREE_STRING(field); \
        } \
    } while (0)

VCL_VOID
vmod_file__fini(struct vmod_cfg_file **file)
{
    AN(file);
    AN(*file);

    struct vmod_cfg_file *instance = *file;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_FILE);

    FREE_STRING(location.raw);
    FREE_STRING(location.parsed);
    instance->period = 0;
    instance->curl.connection_timeout = 0;
    instance->curl.transfer_timeout = 0;
    instance->curl.ssl_verify_peer = 0;
    instance->curl.ssl_verify_host = 0;
    FREE_OPTINAL_STRING(curl.ssl_cafile);
    FREE_OPTINAL_STRING(curl.ssl_capath);
    FREE_OPTINAL_STRING(curl.proxy);
    FREE_STRING(name_delimiter);
    FREE_STRING(value_delimiter);
    instance->read = NULL;
    instance->parse = NULL;
    AZ(pthread_mutex_destroy(&instance->state.mutex));
    instance->state.reloading = 0;
    AZ(pthread_rwlock_destroy(&instance->state.rwlock));
    instance->state.version = 0;
    instance->state.tst = 0;
    flush_variables(instance->state.variables);
    free((void *) instance->state.variables);
    instance->state.variables = NULL;

    FREE_OBJ(instance);

    *file = NULL;
}

#undef FREE_STRING
#undef FREE_OPTINAL_STRING

VCL_BOOL
vmod_file_is_set(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->state.rwlock));
    unsigned result = cfg_is_set(ctx, file->state.variables, name);
    AZ(pthread_rwlock_unlock(&file->state.rwlock));
    return result;
}

VCL_STRING
vmod_file_get(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name, VCL_STRING fallback)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->state.rwlock));
    const char *result = cfg_get(ctx, file->state.variables, name, fallback);
    AZ(pthread_rwlock_unlock(&file->state.rwlock));
    return result;
}

VCL_STRING
vmod_file_dump(VRT_CTX, struct vmod_cfg_file *file)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->state.rwlock));
    const char *result = cfg_dump(ctx, file->state.variables);
    AZ(pthread_rwlock_unlock(&file->state.rwlock));
    return result;
}

VCL_BOOL
vmod_file_reload(VRT_CTX, struct vmod_cfg_file *file)
{
    return file_check(ctx, file, 1);
}

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
