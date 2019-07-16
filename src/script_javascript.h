#ifndef CFG_SCRIPT_JAVASCRIPT_H_INCLUDED
#define CFG_SCRIPT_JAVASCRIPT_H_INCLUDED

#include "script_helpers.h"

engine_t *new_javascript_engine(VRT_CTX, struct vmod_cfg_script *script);

int get_javascript_engine_used_memory(engine_t * engine);

int get_javascript_engine_stack_size(engine_t * engine);

unsigned execute_javascript(
    VRT_CTX, struct vmod_cfg_script *script, task_state_t *state,
    const char *code, const char **name, int argc, const char *argv[],
    result_t *result, unsigned gc_collect, unsigned flush_jemalloc_tcache);

#endif
