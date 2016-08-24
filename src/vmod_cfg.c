#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <curl/curl.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

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
        if ((ctx)->vsl != NULL) \
            VSLb((ctx)->vsl, slt, "[CFG][%s] " fmt, __func__, __VA_ARGS__); \
        else \
            VSL(slt, 0, "[CFG][%s] " fmt, __func__, __VA_ARGS__); \
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
    if (name != NULL) {
        return find_variable(variables, name) != NULL;
    }
    return 0;
}

static const char *
cfg_get(VRT_CTX, variables_t *variables, const char *name, const char *fallback)
{
    const char *result = fallback;

    if (name != NULL) {
        variable_t *variable = find_variable(variables, name);
        if (variable != NULL) {
            CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
            result = variable->value;
        }
    }

    if ((result != NULL) && (ctx->ws != NULL)) {
        result = WS_Copy(ctx->ws, result, -1);
        AN(result);
    }

    return result;
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
    return cfg_is_set(ctx, &env->list, name);
}

VCL_STRING
vmod_env_get(VRT_CTX, struct vmod_cfg_env *env, VCL_STRING name, VCL_STRING fallback)
{
    return cfg_get(ctx, &env->list, name, fallback);
}

/******************************************************************************
 * FILE OBJECT.
 *****************************************************************************/

enum FILE_LOCATION_TYPE {
    FILE_LOCATION_PATH_TYPE,
    FILE_LOCATION_URL_TYPE
};

struct vmod_cfg_file {
    unsigned magic;
    #define VMOD_CFG_FILE 0x9774a43f
    struct {
        const char *raw;
        enum FILE_LOCATION_TYPE type;
        union {
            const char *url;
            const char *path;
        } parsed;
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
    pthread_mutex_t mutex;
    unsigned reloading;
    char *buffer;
    unsigned (*parse)(VRT_CTX, struct vmod_cfg_file *);
    unsigned version;
    time_t tst;
    pthread_rwlock_t rwlock;
    variables_t list;
};

static unsigned
file_read_path(VRT_CTX, struct vmod_cfg_file *file)
{
    assert(file->location.type == FILE_LOCATION_PATH_TYPE);
    AZ(file->buffer);

    FILE *fp = fopen(file->location.parsed.path, "r");
    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        unsigned long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        file->buffer = malloc(fsize);
        AN(file->buffer);
        size_t nitems = fread(file->buffer, 1, fsize, fp);
        fclose(fp);

        if (nitems == fsize) {
            return 1;
        } else {
            LOG(
                ctx, LOG_ERR,
                "Failed to read configuration file (location=%s)",
                file->location.raw);
            return 0;
        }
    } else {
        LOG(
            ctx, LOG_ERR,
            "Failed to open configuration file (location=%s)",
            file->location.raw);
        return 0;
    }
}

static size_t
file_read_url_body(void *block, size_t size, size_t nmemb, void *f)
{
    struct vmod_cfg_file *file;
    CAST_OBJ_NOTNULL(file, f, VMOD_CFG_FILE);

    size_t current_size = (file->buffer == NULL) ? 0 : strlen(file->buffer);
    size_t block_size = size * nmemb;
    file->buffer = realloc(file->buffer, current_size + block_size + 1);
    AN(file->buffer);

    memcpy(&(file->buffer[current_size]), block, block_size);
    file->buffer[current_size + block_size] = 0;

    return block_size;
}

static unsigned
file_read_url(VRT_CTX, struct vmod_cfg_file *file)
{
    assert(file->location.type == FILE_LOCATION_URL_TYPE);
    AZ(file->buffer);

    file->buffer = strdup("");
    AN(file->buffer);

    CURL *ch = curl_easy_init();
    AN(ch);
    curl_easy_setopt(ch, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(ch, CURLOPT_URL, file->location.parsed.url);
    curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(ch, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, file_read_url_body);
    curl_easy_setopt(ch, CURLOPT_WRITEDATA, file);
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

    unsigned result = 0;
    CURLcode cr = curl_easy_perform(ch);
    if (cr == 0) {
        long status;
        curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &status);
        if (status == 200) {
            result = 1;
        } else {
            LOG(
                ctx, LOG_ERR,
                "Failed to fetch configuration file (location=%s, status=%ld)",
                file->location.raw, status);
        }
    } else {
        LOG(
            ctx, LOG_ERR,
            "Failed to fetch configuration file (location=%s): %s",
            file->location.raw, curl_easy_strerror(cr));
    }

    curl_easy_cleanup(ch);
    return result;
}

static unsigned
file_read(VRT_CTX, struct vmod_cfg_file *file)
{
    unsigned result = 0;

    if (file->buffer != NULL) {
        free((void *) file->buffer);
        file->buffer = NULL;
    }

    switch (file->location.type) {
        case FILE_LOCATION_PATH_TYPE:
            result = file_read_path(ctx, file);
            break;
        case FILE_LOCATION_URL_TYPE:
            result = file_read_url(ctx, file);
            break;
        default:
            result = 0;
    }

    if (!result && (file->buffer != NULL)) {
        free((void *) file->buffer);
        file->buffer = NULL;
    }

    return result;
}

struct file_init_stream_ctx {
    char *ptr;
    int left;
};

static char *
file_ini_stream_reader(char *str, int size, void *stream)
{
    struct file_init_stream_ctx* ctx = (struct file_init_stream_ctx*)stream;
    int idx = 0;
    char newline = 0;

    if (ctx->left <= 0) {
        return NULL;
    }

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
    str[idx] = 0;

    ctx->ptr += idx + 1;
    ctx->left -= idx + 1;
    if (newline && (ctx->left > 0) && (ctx->ptr[0] == newline)) {
        ctx->left--;
        ctx->ptr++;
    }

    return str;
}

static int
file_parse_ini_handler(void *user, const char *section, const char *name, const char *value)
{
    struct vmod_cfg_file *file;
    CAST_OBJ_NOTNULL(file, user, VMOD_CFG_FILE);

    char *buffer;
    unsigned flatten = (section != NULL) && (strlen(section) > 0);
    AN(asprintf(
        &buffer,
        "%s%s%s",
            flatten ? section : "",
            flatten ? file->name_delimiter : "",
            name));

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

    free((void *) buffer);

    return 1;
}

static unsigned
file_parse_ini(VRT_CTX, struct vmod_cfg_file *file)
{
    AN(file->buffer);

    struct file_init_stream_ctx file_init_stream_ctx = {
        .ptr = file->buffer,
        .left = strlen(file->buffer)
    };

    if (ini_parse_stream(
            (ini_reader) file_ini_stream_reader, &file_init_stream_ctx,
            file_parse_ini_handler, file) >= 0) {
        LOG(
            ctx, LOG_INFO,
            "Configuration file successfully parsed (location=%s, format=ini)",
            file->location.raw);
        return 1;
    } else {
        LOG(
            ctx, LOG_ERR,
            "Failed to parse configuration file (location=%s, format=ini)",
            file->location.raw);
        return 0;
    }
}

static void
file_check(VRT_CTX, struct vmod_cfg_file *file, unsigned force)
{
    time_t now = time(NULL);
    if (!file->reloading &&
        (force ||
         (file->version != version) ||
         ((file->period > 0) && (now - file->tst > file->period)))) {
        unsigned winner = 0;
        AZ(pthread_mutex_lock(&file->mutex));
        if (!file->reloading) {
            file->reloading = 1;
            winner = 1;
        }
        AZ(pthread_mutex_unlock(&file->mutex));

        if (winner) {
            if (file_read(ctx, file)) {
                AZ(pthread_rwlock_wrlock(&file->rwlock));
                flush_variables(&file->list);
                if ((*file->parse)(ctx, file)) {
                    file->version = version;
                    file->tst = now;
                }
                AZ(pthread_rwlock_unlock(&file->rwlock));

                free((void *) file->buffer);
                file->buffer = NULL;
            }

            AZ(pthread_mutex_lock(&file->mutex));
            file->reloading = 0;
            AZ(pthread_mutex_unlock(&file->mutex));
        }
    }
}

#define SET_LOCATION(low, high, offset) \
    do { \
        instance->location.type = FILE_LOCATION_ ## high ## _TYPE; \
        instance->location.parsed.low = strdup(location + offset); \
        AN(instance->location.parsed.low); \
    } while (0)

#define SET_OPTINAL_STRING(value, field) \
    do { \
        if ((value != NULL) && (strlen(value) > 0)) { \
            instance->field = strdup(value); \
            AN(instance->field); \
        } else { \
            instance->field = NULL; \
        } \
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

        instance->location.raw = strdup(location);
        AN(instance->location.raw);
        if (strncmp(location, "file://", 7) == 0) {
            SET_LOCATION(path, PATH, 7);
        } else if (strncmp(location, "http://", 7) == 0) {
            SET_LOCATION(url, URL, 7);
        } else if (strncmp(location, "https://", 8) == 0) {
            SET_LOCATION(url, URL, 8);
        } else {
            SET_LOCATION(path, PATH, 0);
        }
        instance->period = period;
        instance->curl.connection_timeout = curl_connection_timeout;
        instance->curl.transfer_timeout = curl_transfer_timeout;
        instance->curl.ssl_verify_peer = curl_ssl_verify_peer;
        instance->curl.ssl_verify_host = curl_ssl_verify_host;
        SET_OPTINAL_STRING(curl_ssl_cafile, curl.ssl_cafile);
        SET_OPTINAL_STRING(curl_ssl_capath, curl.ssl_capath);
        SET_OPTINAL_STRING(curl_proxy, curl.proxy);
        instance->name_delimiter = strdup(name_delimiter);
        AN(instance->name_delimiter);
        instance->value_delimiter = strdup(value_delimiter);
        AN(instance->value_delimiter);
        AZ(pthread_mutex_init(&instance->mutex, NULL));
        instance->reloading = 0;
        instance->buffer = NULL;
        if (strcmp(format, "ini") == 0) {
            instance->parse = &file_parse_ini;
        } else {
            WRONG("Illegal format value.");
        }
        instance->version = version;
        instance->tst = 0;
        AZ(pthread_rwlock_init(&instance->rwlock, NULL));
        VRB_INIT(&instance->list);

        file_check(ctx, instance, 1);
    }

    *file = instance;
}

#undef SET_LOCATION
#undef SET_OPTINAL_STRING

#define FREE_OPTINAL_STRING(field) \
    do { \
        if (instance->field != NULL) { \
            free((void *) instance->field); \
            instance->field = NULL; \
        } \
    } while (0)

VCL_VOID
vmod_file__fini(struct vmod_cfg_file **file)
{
    AN(file);
    AN(*file);

    struct vmod_cfg_file *instance = *file;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_FILE);

    free((void *) instance->location.raw);
    instance->location.raw = NULL;
    if (instance->location.type == FILE_LOCATION_PATH_TYPE) {
        free((void *) instance->location.parsed.path);
        instance->location.parsed.path = NULL;
    } else if (instance->location.type == FILE_LOCATION_URL_TYPE) {
        free((void *) instance->location.parsed.url);
        instance->location.parsed.url = NULL;
    }
    instance->location.type = 0;
    instance->period = 0;
    instance->curl.connection_timeout = 0;
    instance->curl.transfer_timeout = 0;
    instance->curl.ssl_verify_peer = 0;
    instance->curl.ssl_verify_host = 0;
    FREE_OPTINAL_STRING(curl.ssl_cafile);
    FREE_OPTINAL_STRING(curl.ssl_capath);
    FREE_OPTINAL_STRING(curl.proxy);
    free((void *) instance->name_delimiter);
    instance->name_delimiter = NULL;
    free((void *) instance->value_delimiter);
    instance->value_delimiter = NULL;
    AZ(pthread_mutex_destroy(&instance->mutex));
    instance->reloading = 0;
    FREE_OPTINAL_STRING(buffer);
    instance->parse = NULL;
    instance->version = 0;
    instance->tst = 0;
    AZ(pthread_rwlock_destroy(&instance->rwlock));
    flush_variables(&instance->list);

    *file = NULL;
}

#undef FREE_OPTINAL_STRING

VCL_VOID
vmod_file_reload(VRT_CTX, struct vmod_cfg_file *file)
{
    file_check(ctx, file, 1);
}

VCL_BOOL
vmod_file_is_set(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->rwlock));
    unsigned result = cfg_is_set(ctx, &file->list, name);
    AZ(pthread_rwlock_unlock(&file->rwlock));
    return result;
}

VCL_STRING
vmod_file_get(VRT_CTX, struct vmod_cfg_file *file, VCL_STRING name, VCL_STRING fallback)
{
    file_check(ctx, file, 0);
    AZ(pthread_rwlock_rdlock(&file->rwlock));
    const char *result = cfg_get(ctx, &file->list, name, fallback);
    AZ(pthread_rwlock_unlock(&file->rwlock));
    return result;
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
