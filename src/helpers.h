#ifndef CFG_HELPERS_H_INCLUDED
#define CFG_HELPERS_H_INCLUDED

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <syslog.h>

typedef struct vmod_state {
    pthread_mutex_t mutex;
    unsigned version;
} vmod_state_t;

extern vmod_state_t vmod_state;

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

#endif
