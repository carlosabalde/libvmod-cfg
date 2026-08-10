#ifndef CFG_HELPERS_H_INCLUDED
#define CFG_HELPERS_H_INCLUDED

#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>

typedef struct vmod_state {
    unsigned refs;
    struct {
        void *lua;
    } libs;
    struct {
        struct vsc_seg *vsc_seg;
        struct VSC_lck *script;
    } locks;
    struct {
        unsigned syslog_enabled;
        unsigned stderr_enabled;
    } log;
} vmod_state_t;

extern vmod_state_t vmod_state;

// Both 'syslog()' and 'fprintf()' serialize threads. Each grabs a process-wide
// lock (glibc's internal lock and stdio's FILE lock, respectively) across its
// syscall. Therefore, do NOT use this macro in hot paths. Syslog (which doesn't
// matter much in containers) and stderr (which is gated by a pipe consumed by
// the Varnish management process) logging should be rare, especially when
// handling requests.
//
// Alternative: enable/disable syslog and/or stderr logging using the env var
// (see VMOD event function), or adjust this macro to limit syslog and stderr to
// non-request stuff (no VXID cases: initializations, helper threads, etc.).
// Better for performance, but not ideal for visibility.
#define LOG(ctx, level, fmt, ...) \
    do { \
        long _tst = (long) time(NULL); \
        \
        if (vmod_state.log.syslog_enabled) { \
            syslog(level, "[CFG][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__); \
        } \
        \
        if (vmod_state.log.stderr_enabled) { \
            fprintf(stderr, "[CFG][%ld][%d][%s:%d] " fmt "\n", _tst, level, __func__, __LINE__, ##__VA_ARGS__); \
        } \
        \
        unsigned _slt = ((level) <= LOG_ERR) ? SLT_VCL_Error : ((level) < LOG_DEBUG) ? SLT_VCL_Log : SLT_Debug; \
        if ((ctx) != NULL && (ctx)->vsl != NULL) { \
            VSLb((ctx)->vsl, _slt, "[CFG][%ld][%s:%d] " fmt, _tst, __func__, __LINE__, ##__VA_ARGS__); \
        } else { \
            VSL(_slt, 0, "[CFG][%ld][%s:%d] " fmt, _tst, __func__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define FAIL(ctx, result, fmt, ...) \
    do { \
        LOG(ctx, LOG_ALERT, fmt, ##__VA_ARGS__); \
        VRT_fail(ctx, "[CFG][%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__); \
        return result; \
    } while (0)

#define FAIL_WS(ctx, result) \
    FAIL(ctx, result, "Workspace overflow")

#define FAIL_INSTANCE(ctx, result) \
    FAIL(ctx, result, "Failed to create instance")

#endif
