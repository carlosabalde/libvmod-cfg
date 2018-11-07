#ifndef CFG_REMOTE_H_INCLUDED
#define CFG_REMOTE_H_INCLUDED

typedef struct remote {
    unsigned magic;
    #define REMOTE_MAGIC 0x9774a43f

    struct {
        const char *raw;
        const char *parsed;
    } location;
    const char *backup;
    unsigned period;
    struct {
        unsigned connection_timeout;
        unsigned transfer_timeout;
        unsigned ssl_verify_peer;
        unsigned ssl_verify_host;
        const char *ssl_cafile;
        const char *ssl_capath;
        const char *proxy;
    } curl;
    char *(*read)(VRT_CTX, struct remote *);

    struct {
        unsigned version;
        time_t tst;
        pthread_mutex_t mutex;
        unsigned reloading;
    } state;
} remote_t;

remote_t *new_remote(
    const char *location, unsigned period, unsigned curl_connection_timeout,
    unsigned curl_transfer_timeout, unsigned curl_ssl_verify_peer,
    unsigned curl_ssl_verify_host, const char *curl_ssl_cafile,
    const char *curl_ssl_capath, const char *curl_proxy);
void free_remote(remote_t *remote);

unsigned check_remote(
    VRT_CTX, remote_t *remote, unsigned force,
    unsigned (*callback)(VRT_CTX, void *, char *), void *ptr);

#endif
