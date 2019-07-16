#ifndef CFG_VARIABLES_H_INCLUDED
#define CFG_VARIABLES_H_INCLUDED

#include "vtree.h"

typedef struct variable {
    unsigned magic;
    #define VARIABLE_MAGIC 0xcb181fe6

    const char *name;
    char *value;

    VRBT_ENTRY(variable) tree;
} variable_t;

typedef VRBT_HEAD(variables, variable) variables_t;

VRBT_PROTOTYPE(variables, variable, tree, variablecmp);

variable_t *new_global_variable(const char *name, size_t len, const char *value);
void free_global_variable(variable_t *variable);
void flush_global_variables(variables_t *variables);

variable_t *find_variable(variables_t *variables, const char *name);
unsigned is_set_variable(VRT_CTX, variables_t *variables, const char *name);
const char *get_variable(VRT_CTX, variables_t *variables, const char *name, const char *fallback);
const char *dump_variables(VRT_CTX, variables_t *variables, unsigned stream, const char *prefix);

#endif
