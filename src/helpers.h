#ifndef CFG_HELPERS_H_INCLUDED
#define CFG_HELPERS_H_INCLUDED

#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>

typedef struct vmod_state {
    struct {
        unsigned refs;
        struct VSC_C_lck *script;
    } locks;
} vmod_state_t;

extern vmod_state_t vmod_state;

typedef struct shared_task_state {
    unsigned magic;
    #define SHARED_TASK_STATE_MAGIC 0x0384d1b2

    struct {
        void *state;
        void (*free)(void *);
    } script;
} shared_task_state_t;

#define LOG(ctx, level, fmt, ...) \
    do { \
        syslog(level, "[CFG][%s:%d] " fmt, __func__, __LINE__, __VA_ARGS__); \
        unsigned slt; \
        if (level <= LOG_ERR) { \
            slt = SLT_VCL_Error; \
        } else { \
            slt = SLT_VCL_Log; \
        } \
        if ((ctx)->vsl != NULL) { \
            VSLb((ctx)->vsl, slt, "[CFG][%s:%d] " fmt, __func__, __LINE__, __VA_ARGS__); \
        } else { \
            VSL(slt, 0, "[CFG][%s:%d] " fmt, __func__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

void init_shared_task_state();
shared_task_state_t *get_shared_task_state();

#endif
