#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"

#include "helpers.h"
#include "variables.h"

/******************************************************************************
 * BASICS.
 *****************************************************************************/

static int
variablecmp(const variable_t *v1, const variable_t *v2)
{
    return strcmp(v1->name, v2->name);
}

VRB_GENERATE(variables, variable, tree, variablecmp);

variable_t *
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

void
free_variable(variable_t *variable)
{
    free((void *) variable->name);
    variable->name = NULL;

    free((void *) variable->value);
    variable->value = NULL;

    FREE_OBJ(variable);
}

variable_t *
find_variable(variables_t *variables, const char *name)
{
    variable_t variable;
    variable.name = name;
    return VRB_FIND(variables, variables, &variable);
}

void
flush_variables(variables_t *variables)
{
    variable_t *variable, *tmp;
    VRB_FOREACH_SAFE(variable, variables, variables, tmp) {
        CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
        VRB_REMOVE(variables, variables, variable);
        free_variable(variable);
    }
}

/******************************************************************************
 * HELPERS.
 *****************************************************************************/

unsigned
is_set_variable(VRT_CTX, variables_t *variables, const char *name)
{
    if (name != NULL) {
        return find_variable(variables, name) != NULL;
    }
    return 0;
}

const char *
get_variable(VRT_CTX, variables_t *variables, const char *name, const char *fallback)
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

static const char *json_hex_chars = "0123456789abcdef";

#define DUMP_CHAR(value) \
    do { \
        if (vsb != NULL) { \
            VSB_putc(vsb, value); \
        } else { \
            *end = value; \
            assert(free_ws > 0); \
            end++; \
            free_ws--; \
        } \
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

const char *
dump_variables(VRT_CTX, variables_t *variables, unsigned stream)
{
    struct vsb *vsb = NULL;
    if (stream && (
        (ctx->method == VCL_MET_SYNTH) ||
        (ctx->method == VCL_MET_BACKEND_ERROR))) {
        CAST_OBJ_NOTNULL(vsb, ctx->specific, VSB_MAGIC);
    }

    AN(ctx->ws);
    char *result, *end;
    variable_t *variable;
    unsigned i = 0;
    unsigned free_ws = WS_Reserve(ctx->ws, 0);
    assert(free_ws > 0);
    result = end = ctx->ws->f;

    DUMP_CHAR('{');
    VRB_FOREACH(variable, variables, variables) {
        CHECK_OBJ_NOTNULL(variable, VARIABLE_MAGIC);
        if (i > 0) {
            DUMP_CHAR(',');
        }
        DUMP_STRING(variable->name);
        DUMP_CHAR(':');
        DUMP_STRING(variable->value);
        i++;
    }
    DUMP_CHAR('}');
    *end = '\0';

    WS_Release(ctx->ws, end - result + 1);

    return result;
}

#undef DUMP_CHAR
#undef DUMP_STRING
