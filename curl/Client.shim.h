/*
 * Client.shim.h - the wrapper handle, the callback typedefs, and every typed
 * helper the map recipes call.
 *
 * CT sees `CurlEasy *` (a `pub type` alias for `struct ct_curl_easy`); the
 * struct itself is package-private. Wrapping `CURL *` instead of hosting
 * methods on it buys three things:
 *
 *   1. every method can return the same wrapper pointer, which is what makes
 *      the chain `"url".openCurl().setTimeout(60).perform()` a single C
 *      expression with no temporary values (see docs/design.md section 6);
 *   2. per-handle state (last CURLcode, ERRORBUFFER, the internal header
 *      slist, the default response-body sink, the POST body copy) has a home;
 *   3. the method hosts are package-private types, so `close` / `destroy`
 *      cannot collide with another package's methods on a shared C type.
 *
 * Every helper is a real function in Client.shim.c (not `static inline`), so
 * there is exactly one copy per program and the shim's own state is shared.
 * All of them return the receiver itself (`return c;`), never a copy and
 * never a new allocation.
 *
 * License: MIT (see LICENSE).
 */
#ifndef CURL_CT_SHIM_H
#define CURL_CT_SHIM_H

#include <curl/curl.h>

/* Write / header callback shapes. Spelled once so a trait can name them; the
   CT side refers to these typedef names (the same pattern as sqlite's
   `ct_sqlite_cb`). `userdata` is the only channel through which CT state
   reaches a callback. */
typedef size_t (*CurlWriteCb)(void *ptr, size_t size, size_t nmemb,
                              void *userdata);
typedef size_t (*CurlHeaderCb)(void *ptr, size_t size, size_t nmemb,
                               void *userdata);

typedef struct ct_curl_easy ct_curl_easy;

struct ct_curl_easy {
    CURL *h;                       /* NULL after close() */
    int last_code;                 /* most recent CURLcode */
    int init_error;                /* sticky: a setopt / allocation failed */
    int closed;                    /* close() ran */
    char errbuf[CURL_ERROR_SIZE];  /* CURLOPT_ERRORBUFFER, always NUL-terminated */
    struct curl_slist *headers;    /* internal header list, owned here */
    char *post_copy;               /* CURLOPT_POSTFIELDS copy, owned here */
    int post_len;
    char *body;                    /* default sink buffer, NUL-terminated */
    int body_len;
    int body_cap;
    int body_limit;                /* 0 = unlimited */
    int body_overflow;             /* sink aborted because of body_limit */
    CurlWriteCb write_cb;          /* 0 = use the default sink */
    void *write_ud;
    CurlHeaderCb header_cb;        /* 0 = ignore headers */
    void *header_ud;
};

/* ---- entry (host `char *`) -------------------------------------------- */

ct_curl_easy *ct_curl_open(const char *url);

/* ---- configuration (host `CurlEasy *`, all return the receiver) ------- */

ct_curl_easy *ct_curl_set_url(ct_curl_easy *c, const char *url);
ct_curl_easy *ct_curl_set_request(ct_curl_easy *c, const char *method);
ct_curl_easy *ct_curl_set_user_agent(ct_curl_easy *c, const char *ua);
ct_curl_easy *ct_curl_set_user_pwd(ct_curl_easy *c, const char *userpwd);
ct_curl_easy *ct_curl_set_follow(ct_curl_easy *c, int on);
ct_curl_easy *ct_curl_set_max_redirs(ct_curl_easy *c, int n);
ct_curl_easy *ct_curl_set_timeout(ct_curl_easy *c, int seconds);
ct_curl_easy *ct_curl_set_connect_timeout(ct_curl_easy *c, int seconds);
ct_curl_easy *ct_curl_set_verify_peer(ct_curl_easy *c, int on);
ct_curl_easy *ct_curl_set_verify_host(ct_curl_easy *c, int level);
ct_curl_easy *ct_curl_set_cainfo(ct_curl_easy *c, const char *path);
ct_curl_easy *ct_curl_add_header(ct_curl_easy *c, const char *name,
                                 const char *value);
ct_curl_easy *ct_curl_set_post(ct_curl_easy *c, const char *data, int len);
ct_curl_easy *ct_curl_set_body_limit(ct_curl_easy *c, int max_bytes);
ct_curl_easy *ct_curl_set_write_function(ct_curl_easy *c, CurlWriteCb cb,
                                         void *ud);
ct_curl_easy *ct_curl_set_header_function(ct_curl_easy *c, CurlHeaderCb cb,
                                          void *ud);

/* ---- execution (all return the receiver; close/destroy return void) --- */

ct_curl_easy *ct_curl_perform(ct_curl_easy *c);
ct_curl_easy *ct_curl_get(ct_curl_easy *c);
ct_curl_easy *ct_curl_post(ct_curl_easy *c, const char *data, int len);
ct_curl_easy *ct_curl_reset(ct_curl_easy *c);
ct_curl_easy *ct_curl_clear_body(ct_curl_easy *c);
void ct_curl_close(ct_curl_easy *c);
void ct_curl_destroy(ct_curl_easy *c);

/* ---- reading (terminators) -------------------------------------------- */

int ct_curl_status(ct_curl_easy *c);
const char *ct_curl_message(ct_curl_easy *c);
const char *ct_curl_error_text(ct_curl_easy *c);
int ct_curl_response_code(ct_curl_easy *c);
const char *ct_curl_effective_url(ct_curl_easy *c);
const char *ct_curl_content_type(ct_curl_easy *c);
double ct_curl_total_time(ct_curl_easy *c);
long long ct_curl_size_download(ct_curl_easy *c);
const char *ct_curl_body(ct_curl_easy *c);
int ct_curl_body_len(ct_curl_easy *c);

/* ---- namespace (no receiver) ------------------------------------------ */

int ct_curl_global_init(void);
void ct_curl_global_cleanup(void);
const char *ct_curl_version(void);
const char *ct_curl_strerror(int code);

#endif /* CURL_CT_SHIM_H */
