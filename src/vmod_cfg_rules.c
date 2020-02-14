#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "cache/cache.h"
#include "vre.h"
#include "vcc_cfg_if.h"

#include "helpers.h"
#include "remote.h"

typedef struct rule {
    unsigned magic;
    #define RULE_MAGIC 0xeaf3110d

    vre_t *vre;
    const char *value;

    VTAILQ_ENTRY(rule) list;
} rule_t;

typedef VTAILQ_HEAD(rules, rule) rules_t;

struct vmod_cfg_rules {
    unsigned magic;
    #define VMOD_CFG_RULES_MAGIC 0x91baff91

    const char *name;

    remote_t *remote;

    struct {
        pthread_rwlock_t rwlock;
        rules_t *rules;
    } state;
};

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

static rule_t *
new_rule(vre_t *vre, const char *value)
{
    rule_t *result;
    ALLOC_OBJ(result, RULE_MAGIC);
    AN(result);

    result->vre = vre;

    result->value = strdup(value);
    AN(result->value);

    return result;
}

static void
free_rule(rule_t *rule)
{
    VRT_re_fini(rule->vre);
    rule->vre = NULL;

    free((void *) rule->value);
    rule->value = NULL;

    FREE_OBJ(rule);
}

static void
flush_rules(rules_t *rules)
{
    rule_t *irule;
    while (!VTAILQ_EMPTY(rules)) {
        irule = VTAILQ_FIRST(rules);
        CHECK_OBJ_NOTNULL(irule, RULE_MAGIC);
        VTAILQ_REMOVE(rules, irule, list);
        free_rule(irule);
    }
}

/******************************************************************************
 * BASICS.
 *****************************************************************************/

rules_t *
rules_parse(VRT_CTX, struct vmod_cfg_rules *rules, char *contents)
{
    rules_t *result = malloc(sizeof(rules_t));
    AN(result);
    VTAILQ_INIT(result);
    unsigned error = 0;

    char *line, *line_end, *regexp, *regexp_end, *value, *value_end;
    char line_end_char, regexp_end_char, value_end_char;
    const char *error_string;
    int error_offset;
    unsigned row = 1;
    line = contents;
    while (*line != '\0') {
        // Isolate line.
        line_end = strchr(line, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
            line_end_char = '\n';
        } else {
            line_end = line + strlen(line);
            line_end_char = '\0';
        }

        // Skip empty lines.
        regexp = line;
        for (; (*regexp != '\0') && isspace(*regexp); regexp++);
        if (*regexp == '\0') {
            goto skip;
        }

        // Extract regexp.
        value = strstr(regexp, "->");
        if (value == NULL) {
            error = 1;
            goto skip;
        }
        if (value == regexp) {
            error = 2;
            goto skip;
        }
        regexp_end = value;
        for (; (regexp_end > regexp) && isspace(*(regexp_end - 1)); regexp_end--);

        // Extract value.
        value += 2;
        for (; (*value != '\0') && isspace(*value); value++);
        value_end = line_end;
        for (; (value_end > value) && isspace(*(value_end - 1)); value_end--);

        // Isolate regexp & value.
        regexp_end_char = *regexp_end;
        *regexp_end = '\0';
        value_end_char = *value_end;
        *value_end = '\0';

        // Compile & create rule.
        vre_t *vre = VRE_compile(regexp, 0, &error_string, &error_offset);
        if (vre == NULL) {
            LOG(ctx, LOG_ERR,
                "Got error while compiling regexp at line %d (%s): %s",
                row, regexp, error_string);
            error = 3;
        } else {
            rule_t *rule = new_rule(vre, value);
            VTAILQ_INSERT_TAIL(result, rule, list);
        }

        // Restore regexp & value.
        *regexp_end = regexp_end_char;
        *value_end = value_end_char;

skip:
        // Restore line.
        *line_end = line_end_char;

        // Jump to next line?
        if (error) {
            LOG(ctx, LOG_ERR,
                "Got error while parsing rules (rules=%s, line=%d, error=%d)",
                rules->name, row, error);
            flush_rules(result);
            free((void *) result);
            result = NULL;
            break;
        } else {
            line =  (*line_end == '\n') ? (line_end + 1) : line_end;
            row++;
        }
    }

    return result;
}

static unsigned
rules_check_callback(VRT_CTX, void *ptr, char *contents, unsigned is_backup)
{
    unsigned result = 0;

    struct vmod_cfg_rules *vmod_cfg_rules;
    CAST_OBJ_NOTNULL(vmod_cfg_rules, ptr, VMOD_CFG_RULES_MAGIC);

    rules_t *rules = rules_parse(ctx, vmod_cfg_rules, contents);
    if (rules != NULL) {
        LOG(ctx, LOG_INFO,
            "Remote successfully parsed (rules=%s, location=%s, is_backup=%d)",
            vmod_cfg_rules->name, vmod_cfg_rules->remote->location.raw, is_backup);

        AZ(pthread_rwlock_wrlock(&vmod_cfg_rules->state.rwlock));
        rules_t *old = vmod_cfg_rules->state.rules;
        vmod_cfg_rules->state.rules = rules;
        AZ(pthread_rwlock_unlock(&vmod_cfg_rules->state.rwlock));

        flush_rules(old);
        free((void *) old);

        result = 1;
    } else {
        LOG(ctx, LOG_ERR,
            "Failed to parse remote (rules=%s, location=%s, is_backup=%d)",
            vmod_cfg_rules->name, vmod_cfg_rules->remote->location.raw, is_backup);
    }

    return result;
}

static unsigned
rules_check(VRT_CTX, struct vmod_cfg_rules *rules, unsigned force)
{
    return check_remote(ctx, rules->remote, force, &rules_check_callback, rules);
}

VCL_VOID
vmod_rules__init(
    VRT_CTX, struct vmod_cfg_rules **rules, const char *vcl_name,
    VCL_STRING location, VCL_STRING backup, VCL_INT period,
    VCL_INT curl_connection_timeout, VCL_INT curl_transfer_timeout,
    VCL_BOOL curl_ssl_verify_peer, VCL_BOOL curl_ssl_verify_host,
    VCL_STRING curl_ssl_cafile, VCL_STRING curl_ssl_capath,
    VCL_STRING curl_proxy)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(rules);
    AZ(*rules);

    struct vmod_cfg_rules *instance = NULL;

    if ((location != NULL) && (strlen(location) > 0) &&
        (period >= 0) &&
        (curl_connection_timeout >= 0) &&
        (curl_transfer_timeout >= 0)) {
        ALLOC_OBJ(instance, VMOD_CFG_RULES_MAGIC);
        AN(instance);

        instance->name = strdup(vcl_name);
        AN(instance->name);
        instance->remote = new_remote(
            location, backup, period, curl_connection_timeout, curl_transfer_timeout,
            curl_ssl_verify_peer, curl_ssl_verify_host, curl_ssl_cafile,
            curl_ssl_capath, curl_proxy);
        AZ(pthread_rwlock_init(&instance->state.rwlock, NULL));
        instance->state.rules = malloc(sizeof(rules_t));
        AN(instance->state.rules);
        VTAILQ_INIT(instance->state.rules);

        rules_check(ctx, instance, 1);
    }

    *rules = instance;
}

VCL_VOID
vmod_rules__fini(struct vmod_cfg_rules **rules)
{
    AN(rules);
    AN(*rules);

    struct vmod_cfg_rules *instance = *rules;
    CHECK_OBJ_NOTNULL(instance, VMOD_CFG_RULES_MAGIC);

    free((void *) instance->name);
    instance->name = NULL;
    free_remote(instance->remote);
    instance->remote = NULL;
    AZ(pthread_rwlock_destroy(&instance->state.rwlock));
    flush_rules(instance->state.rules);
    free((void *) instance->state.rules);
    instance->state.rules = NULL;

    FREE_OBJ(instance);

    *rules = NULL;
}

VCL_BOOL
vmod_rules_reload(VRT_CTX, struct vmod_cfg_rules *rules)
{
    return rules_check(ctx, rules, 1);
}

VCL_VOID
vmod_rules_inspect(VRT_CTX, struct vmod_cfg_rules *rules)
{
    rules_check(ctx, rules, 0);
    inspect_remote(ctx, rules->remote);
}

VCL_STRING
vmod_rules_get(VRT_CTX, struct vmod_cfg_rules *rules, VCL_STRING value, VCL_STRING fallback)
{
    AN(ctx->ws);
    const char *result = fallback;

    rules_check(ctx, rules, 0);

    AZ(pthread_rwlock_rdlock(&rules->state.rwlock));
    rule_t *irule;
    VTAILQ_FOREACH(irule, rules->state.rules, list) {
        CHECK_OBJ_NOTNULL(irule, RULE_MAGIC);
        if (VRT_re_match(ctx, value, irule->vre)) {
            result = irule->value;
            break;
        }
    }
    AZ(pthread_rwlock_unlock(&rules->state.rwlock));

    if (result != NULL) {
        result = WS_Copy(ctx->ws, result, -1);
        if (result == NULL) {
            FAIL_WS(ctx, NULL);
        }
    }

    return result;
}
