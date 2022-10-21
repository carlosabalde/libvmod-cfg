#ifndef CFG_HELPERS_H_INCLUDED
#define CFG_HELPERS_H_INCLUDED

#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>

typedef struct vmod_state {
    unsigned refs;
    struct {
        void *lua;
    } libs;
    struct {
        struct vsc_seg *vsc_seg;
        struct VSC_lck *script;
    } locks;
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
            VSL(slt, NO_VXID, "[CFG][%s:%d] " fmt, __func__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

#define FAIL(ctx, result, fmt, ...) \
    do { \
        syslog(LOG_ALERT, "[CFG][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__); \
        VRT_fail(ctx, "[CFG][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__); \
        return result; \
    } while (0)

#define FAIL_WS(ctx, result) \
    FAIL(ctx, result, "Workspace overflow")

#endif
