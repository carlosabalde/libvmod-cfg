#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>

#include "cache/cache.h"
#include "vsb.h"
#include "vcl.h"

#include "helpers.h"
#include "remote.h"

static char *read_backup(VRT_CTX, remote_t *remote);
static char *read_path(VRT_CTX, remote_t *remote);
static char *read_url(VRT_CTX, remote_t *remote);

/******************************************************************************
 * BASICS.
 *****************************************************************************/

#define SET_STRING(value, field) \
    do { \
        result->field = strdup(value); \
        AN(result->field); \
    } while (0)

#define SET_OPTIONAL_STRING(value, field) \
    do { \
        if ((value != NULL) && (strlen(value) > 0)) { \
            SET_STRING(value, field); \
        } else { \
            result->field = NULL; \
        } \
    } while (0)

#define SET_LOCATION(type, offset) \
    do { \
        SET_STRING(location + offset, location.parsed); \
        result->read = &read_ ## type; \
    } while (0)

remote_t *
new_remote(
    const char *location, const char *backup, unsigned automated_backups,
    unsigned period, unsigned curl_connection_timeout,
    unsigned curl_transfer_timeout, unsigned curl_ssl_verify_peer,
    unsigned curl_ssl_verify_host, const char *curl_ssl_cafile,
    const char *curl_ssl_capath, const char *curl_proxy)
{
    remote_t *result;
    ALLOC_OBJ(result, REMOTE_MAGIC);
    AN(result);

    SET_STRING(location, location.raw);
    if (strncmp(location, "file://", 7) == 0) {
        SET_LOCATION(path, 7);
    } else if (strncmp(location, "http://", 7) == 0) {
        SET_LOCATION(url, 7);
    } else if (strncmp(location, "https://", 8) == 0) {
        SET_LOCATION(url, 0);
    } else {
        SET_LOCATION(path, 0);
    }
    if ((backup != NULL) && (strlen(backup) > 0)) {
        SET_STRING(backup, backup);
    } else {
        result->backup = NULL;
    }
    result->automated_backups = automated_backups;
    result->period = period;
    result->curl.connection_timeout = curl_connection_timeout;
    result->curl.transfer_timeout = curl_transfer_timeout;
    result->curl.ssl_verify_peer = curl_ssl_verify_peer;
    result->curl.ssl_verify_host = curl_ssl_verify_host;
    SET_OPTIONAL_STRING(curl_ssl_cafile, curl.ssl_cafile);
    SET_OPTIONAL_STRING(curl_ssl_capath, curl.ssl_capath);
    SET_OPTIONAL_STRING(curl_proxy, curl.proxy);
    result->state.tst = 0;
    AZ(pthread_mutex_init(&result->state.mutex, NULL));
    result->state.reloading = 0;
    result->state.contents = NULL;

    return result;
}

#undef SET_STRING
#undef SET_OPTIONAL_STRING
#undef SET_LOCATION

#define FREE_STRING(field) \
    do { \
        free((void *) remote->field); \
        remote->field = NULL; \
    } while (0)

#define FREE_OPTIONAL_STRING(field) \
    do { \
        if (remote->field != NULL) { \
            FREE_STRING(field); \
        } \
    } while (0)

void
free_remote(remote_t *remote)
{
    CHECK_OBJ_NOTNULL(remote, REMOTE_MAGIC);

    FREE_STRING(location.raw);
    FREE_STRING(location.parsed);
    remote->backup = NULL;
    remote->automated_backups = 0;
    remote->period = 0;
    remote->curl.connection_timeout = 0;
    remote->curl.transfer_timeout = 0;
    remote->curl.ssl_verify_peer = 0;
    remote->curl.ssl_verify_host = 0;
    FREE_OPTIONAL_STRING(curl.ssl_cafile);
    FREE_OPTIONAL_STRING(curl.ssl_capath);
    FREE_OPTIONAL_STRING(curl.proxy);
    remote->read = NULL;
    remote->state.tst = 0;
    AZ(pthread_mutex_destroy(&remote->state.mutex));
    remote->state.reloading = 0;
    FREE_OPTIONAL_STRING(state.contents);

    FREE_OBJ(remote);
}

#undef FREE_STRING
#undef FREE_OPTIONAL_STRING

/******************************************************************************
 * CHECK.
 *****************************************************************************/

static unsigned
check_remote_backup(
    VRT_CTX, remote_t *remote,
    unsigned (*callback)(VRT_CTX, void *, char *, unsigned), void *ptr)
{
    unsigned result = 0;
    char *contents = read_backup(ctx, remote);

    if (contents != NULL && strlen(contents) > 0) {
        result = (*callback)(ctx, ptr, contents, 1);
    }

    if (result) {
        AZ(pthread_mutex_lock(&remote->state.mutex));
        if (remote->state.contents != NULL) {
            free((void *) remote->state.contents);
        }
        remote->state.contents = contents;
        AZ(pthread_mutex_unlock(&remote->state.mutex));

        LOG(ctx, LOG_INFO,
            "Settings loaded from backup (location=%s, backup=%s)",
            remote->location.raw, remote->backup);
    } else {
        if (contents != NULL) {
            free((void *) contents);
        }

        LOG(ctx, LOG_ERR,
            "Failed to load backup file (location=%s, backup=%s)",
            remote->location.raw, remote->backup);
    }

    return result;
}

unsigned
check_remote(
    VRT_CTX, remote_t *remote, unsigned force_load, unsigned force_backup,
    unsigned (*callback)(VRT_CTX, void *, char *, unsigned), void *ptr)
{
    unsigned result = 0;

    time_t now = time(NULL);
    unsigned winner = 0;

    if (!force_load &&
        !remote->state.reloading &&
        ((remote->period > 0) && (now - remote->state.tst > remote->period))) {
        AZ(pthread_mutex_lock(&remote->state.mutex));
        if (!remote->state.reloading) {
            remote->state.reloading = 1;
            winner = 1;
        }
        AZ(pthread_mutex_unlock(&remote->state.mutex));
    }

    if (force_load || winner) {
        char *contents = (*remote->read)(ctx, remote);

        if (contents != NULL && strlen(contents) > 0) {
            result = (*callback)(ctx, ptr, contents, 0);
        }

        if (result) {
            AZ(pthread_mutex_lock(&remote->state.mutex));
            if (remote->state.contents != NULL) {
                free((void *) remote->state.contents);
            }
            remote->state.contents = contents;
            AZ(pthread_mutex_unlock(&remote->state.mutex));

            if (remote->backup != NULL) {
                if (remote->automated_backups || force_backup) {
                    FILE *backup = fopen(remote->backup, "wb");
                    if (backup != NULL) {
                        int rc = fputs(contents, backup);
                        if (rc < 0) {
                            // Not possible to use GNU strerror_r() due to Linux Alpine
                            // issue. See:
                            //   - https://stackoverflow.com/questions/41953104/strerror-r-is-incorrectly-declared-on-alpine-linux
                            char buffer[256];
#ifdef STRERROR_R_CHAR_P
                            LOG(ctx, LOG_ERR,
                                "Failed to write backup file (location=%s, backup=%s, error=%s)",
                                remote->location.raw, remote->backup, strerror_r(rc, buffer, sizeof(buffer)));
#else
                            rc = strerror_r(rc, buffer, sizeof(buffer));
                            if (rc == 0) {
                                LOG(ctx, LOG_ERR,
                                    "Failed to write backup file (location=%s, backup=%s, error=%s)",
                                    remote->location.raw, remote->backup, buffer);
                            } else {
                                LOG(ctx, LOG_ERR,
                                    "Failed to write backup file (location=%s, backup=%s, error=%d)",
                                    remote->location.raw, remote->backup, rc);
                            }
#endif
                        } else {
                            LOG(ctx, LOG_INFO,
                                "Successfully write to backup file (location=%s, backup=%s)",
                                remote->location.raw, remote->backup);
                        }
                        fclose(backup);
                    } else {
                        LOG(ctx, LOG_ERR,
                            "Failed to open backup file (location=%s, backup=%s)",
                            remote->location.raw, remote->backup);
                    }
                } else {
                    LOG(ctx, LOG_INFO,
                        "Automated backups are disabled (location=%s, backup=%s)",
                        remote->location.raw, remote->backup);
                }
            }
        } else {
            if (contents != NULL) {
                free((void *) contents);
            }

            if (remote->backup != NULL) {
                struct stat st;
                if ((stat(remote->backup, &st) == 0) && (st.st_size > 0)) {
                    result = check_remote_backup(ctx, remote, callback, ptr);
                } else {
                    LOG(ctx, LOG_ERR,
                        "Backup file is empty or doesn't exist (location=%s, backup=%s)",
                        remote->location.raw, remote->backup);
                }
            }
        }

        if (result || winner) {
            AZ(pthread_mutex_lock(&remote->state.mutex));
            if (result) {
                remote->state.tst = time(NULL);
            }
            if (winner) {
                remote->state.reloading = 0;
            }
            AZ(pthread_mutex_unlock(&remote->state.mutex));
        }
    } else {
        result = 1;
    }

    return result;
}

/******************************************************************************
 * INSPECT.
 *****************************************************************************/

void
inspect_remote(VRT_CTX, remote_t *remote)
{
    if ((ctx->method == VCL_MET_SYNTH) ||
        (ctx->method == VCL_MET_BACKEND_ERROR)) {
        struct vsb *vsb = NULL;
        CAST_OBJ_NOTNULL(vsb, ctx->specific, VSB_MAGIC);

        AZ(pthread_mutex_lock(&remote->state.mutex));
        if (remote->state.contents != NULL) {
            AZ(VSB_cat(vsb, remote->state.contents));
        }
        AZ(pthread_mutex_unlock(&remote->state.mutex));
    }
}

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static char *
read_file(VRT_CTX, remote_t *remote, const char *file)
{
    char *result = NULL;

    FILE *bf = fopen(file, "rb");
    if (bf != NULL) {
        fseek(bf, 0, SEEK_END);
        unsigned long fsize = ftell(bf);
        fseek(bf, 0, SEEK_SET);

        result = malloc(fsize + 1);
        AN(result);
        size_t nitems = fread(result, 1, fsize, bf);
        fclose(bf);
        result[fsize] = '\0';

        if (nitems != fsize) {
            free((void *) result);
            result = NULL;

            LOG(ctx, LOG_ERR,
                "Failed to read file (location=%s, file=%s)",
                remote->location.raw, file);
        }
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to open file (location=%s, file=%s)",
            remote->location.raw, file);
    }

    return result;
}

static char *
read_backup(VRT_CTX, remote_t *remote)
{
    AN(remote->backup);
    return read_file(ctx, remote, remote->backup);
}

static char *
read_path(VRT_CTX, remote_t *remote)
{
    return read_file(ctx, remote, remote->location.parsed);
}

struct read_url_ctx {
    char *body;
    size_t bodylen;
};

static size_t
read_url_body(void *block, size_t size, size_t nmemb, void *c)
{
    struct read_url_ctx *ctx = (struct read_url_ctx *) c;

    size_t block_size = size * nmemb;
    ctx->body = realloc(ctx->body, ctx->bodylen + block_size + 1);
    AN(ctx->body);

    memcpy(&(ctx->body[ctx->bodylen]), block, block_size);
    ctx->bodylen += block_size;
    ctx->body[ctx->bodylen] = '\0';

    return block_size;
}

static char *
read_url(VRT_CTX, remote_t *remote)
{
    struct read_url_ctx read_url_ctx = {
        .body = strdup(""),
        .bodylen = 0
    };
    AN(read_url_ctx.body);

    CURL *ch = curl_easy_init();
    AN(ch);
    curl_easy_setopt(ch, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(ch, CURLOPT_URL, remote->location.parsed);
    curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(ch, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, read_url_body);
    curl_easy_setopt(ch, CURLOPT_WRITEDATA, &read_url_ctx);
    if (remote->curl.connection_timeout > 0) {
#ifdef HAVE_CURLOPT_CONNECTTIMEOUT_MS
        curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT_MS, remote->curl.connection_timeout);
#else
        curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT, remote->curl.connection_timeout / 1000);
#endif
    }
    if (remote->curl.transfer_timeout > 0) {
#ifdef HAVE_CURLOPT_TIMEOUT_MS
        curl_easy_setopt(ch, CURLOPT_TIMEOUT_MS, remote->curl.transfer_timeout);
#else
        curl_easy_setopt(ch, CURLOPT_TIMEOUT, remote->curl.transfer_timeout / 1000);
#endif
    }
    if (remote->curl.ssl_verify_peer) {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 1L);
    } else {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
    }
    if (remote->curl.ssl_verify_host) {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 1L);
    } else {
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (remote->curl.ssl_cafile != NULL) {
        curl_easy_setopt(ch, CURLOPT_CAINFO, remote->curl.ssl_cafile);
    }
    if (remote->curl.ssl_capath != NULL) {
        curl_easy_setopt(ch, CURLOPT_CAPATH, remote->curl.ssl_capath);
    }
    if (remote->curl.proxy != NULL) {
        curl_easy_setopt(ch, CURLOPT_PROXY, remote->curl.proxy);
    }

    char *result = NULL;
    CURLcode cr = curl_easy_perform(ch);
    if (cr == CURLE_OK) {
        long status;
        curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &status);
        if (status == 200) {
            result = read_url_ctx.body;
        } else {
            LOG(ctx, LOG_ERR,
                "Failed to fetch remote (location=%s, status=%ld)",
                remote->location.raw, status);
        }
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to fetch remote (location=%s): %s",
            remote->location.raw, curl_easy_strerror(cr));
    }

    if (result == NULL) {
        free((void *) read_url_ctx.body);
    }

    curl_easy_cleanup(ch);
    return result;
}
