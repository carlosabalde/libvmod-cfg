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
            VSL(_slt, NO_VXID, "[CFG][%ld][%s:%d] " fmt, _tst, __func__, __LINE__, ##__VA_ARGS__); \
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

// Appends 'value' to the synthetic response body being built by 'vcl_synth' /
// 'vcl_backend_error'. This used to be implemented writing into the
// 'synth_body' VSB exposed through 'ctx->specific', but since the introduction
// of the special-purpose synth storage engine that VSB is ignored whenever the
// response is backed by that engine, which is the new default: bodies must be
// assigned through 'VRT_l_resp_body()' / 'VRT_l_beresp_body()' instead, just
// like 'set (be)resp.body = ...' does.
//
// BEWARE: on the 'vcl_synth' side the synth storage engine keeps *references*
// to the body constituents and only reads them during delivery. Therefore
// 'value' must remain valid and immutable until the response has been sent:
// workspace-allocated strings and VCL_STRINGs are fine; heap memory that may
// be freed or mutated by other threads (e.g., on a remote reload) is not.
//
// Because of this new behavior, callers now stage 'value' on the workspace,
// which caps synthetic bodies at the free workspace size. The old
// not-limited-by-workspace behavior could be restored by staging on the heap
// instead: build the body in a private 'VSB_new_auto()', hang it off a
// PRIV_TASK with a free callback, and pass 'VSB_data()' here. Client task privs
// are only released at request teardown, after 'cnt_transmit()' has delivered
// the body, so a task-owned heap buffer satisfies the until-delivery lifetime
// required by the synth storage engine; on the 'vcl_backend_error' side
// 'VRT_l_(be)resp_body()' copies the string right away, so it works there too.
void append_response_body(VRT_CTX, const char *value);

#endif
