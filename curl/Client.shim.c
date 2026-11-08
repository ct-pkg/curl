/*
 * Client.shim.c - the single non-inline copy of every typed helper.
 *
 * The helpers fall into four groups: the constructor, the configuration
 * setters, the executors, and the readers. The setters and executors all
 * `return c;` - that identity is the whole contract behind chaining: the CT
 * call site expands to a nested C call such as
 *
 *   ct_curl_perform(ct_curl_set_follow(ct_curl_set_timeout(
 *       ct_curl_open("url"), 60), 1))
 *
 * with one heap object flowing through every step, so there is no temporary
 * and nothing to dangle.
 *
 * License: MIT (see LICENSE).
 */
#include "Client.shim.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- internal helpers ------------------------------------------------- */

/* The default sink: grow `body` by doubling, keep it NUL-terminated, and
   enforce `body_limit`. Exceeding the limit aborts the transfer with
   CURLE_WRITE_ERROR rather than silently truncating. */
static size_t ct_curl_default_write(void *ptr, size_t size, size_t nmemb,
                                    void *ud)
{
    ct_curl_easy *c = (ct_curl_easy *)ud;
    size_t n = size * nmemb;
    int need;
    char *grown;
    int cap;

    if (!c || n == 0) return 0;
    if (n > (size_t)INT_MAX) {
        c->body_overflow = 1;
        return 0;
    }
    if (c->body_limit > 0 && c->body_len + (int)n > c->body_limit) {
        c->body_overflow = 1;
        return 0;
    }
    if (c->body_len > INT_MAX - (int)n - 1) {
        c->body_overflow = 1;
        return 0;
    }
    need = c->body_len + (int)n + 1;
    if (need > c->body_cap) {
        cap = c->body_cap ? c->body_cap : 256;
        while (cap < need) cap *= 2;
        grown = (char *)realloc(c->body, (size_t)cap);
        if (!grown) {
            c->body_overflow = 1;
            return 0;
        }
        c->body = grown;
        c->body_cap = cap;
    }
    memcpy(c->body + c->body_len, ptr, n);
    c->body_len += (int)n;
    c->body[c->body_len] = '\0';
    return n;
}

/* A NULL, closed, or uninitialised handle cannot take options: the call is a
   no-op that leaves `init_error` set, and the next perform() reports
   CURLE_FAILED_INIT instead of pretending to succeed. */
static int ct_curl_ready(ct_curl_easy *c)
{
    if (!c) return 0;
    if (c->closed || !c->h) {
        c->init_error = 1;
        return 0;
    }
    return 1;
}

/* Record a failed setopt. Sticky: perform() refuses to run afterwards. */
static void ct_curl_note(ct_curl_easy *c, CURLcode rc)
{
    if (rc != CURLE_OK) {
        c->last_code = (int)rc;
        c->init_error = 1;
    }
}

/* ---- entry ------------------------------------------------------------ */

ct_curl_easy *ct_curl_open(const char *url)
{
    ct_curl_easy *c = (ct_curl_easy *)calloc(1, sizeof *c);

    if (!c) return 0;
    c->h = curl_easy_init();
    if (!c->h) {
        free(c);
        return 0;
    }
    curl_easy_setopt(c->h, CURLOPT_ERRORBUFFER, c->errbuf);
    curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, ct_curl_default_write);
    curl_easy_setopt(c->h, CURLOPT_WRITEDATA, c);
    if (url && url[0]) ct_curl_set_url(c, url);
    return c;
}

/* ---- configuration ---------------------------------------------------- */

ct_curl_easy *ct_curl_set_url(ct_curl_easy *c, const char *url)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_URL, url ? url : ""));
    return c;
}

ct_curl_easy *ct_curl_set_request(ct_curl_easy *c, const char *method)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_CUSTOMREQUEST,
                                     method ? method : ""));
    return c;
}

ct_curl_easy *ct_curl_set_user_agent(ct_curl_easy *c, const char *ua)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_USERAGENT, ua ? ua : ""));
    return c;
}

ct_curl_easy *ct_curl_set_user_pwd(ct_curl_easy *c, const char *userpwd)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_USERPWD,
                                     userpwd ? userpwd : ""));
    return c;
}

ct_curl_easy *ct_curl_set_follow(ct_curl_easy *c, int on)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_FOLLOWLOCATION,
                                     on ? 1L : 0L));
    return c;
}

ct_curl_easy *ct_curl_set_max_redirs(ct_curl_easy *c, int n)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_MAXREDIRS, (long)n));
    return c;
}

ct_curl_easy *ct_curl_set_timeout(ct_curl_easy *c, int seconds)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_TIMEOUT, (long)seconds));
    return c;
}

ct_curl_easy *ct_curl_set_connect_timeout(ct_curl_easy *c, int seconds)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_CONNECTTIMEOUT,
                                     (long)seconds));
    return c;
}

ct_curl_easy *ct_curl_set_verify_peer(ct_curl_easy *c, int on)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYPEER,
                                     on ? 1L : 0L));
    return c;
}

ct_curl_easy *ct_curl_set_verify_host(ct_curl_easy *c, int level)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYHOST,
                                     (long)level));
    return c;
}

ct_curl_easy *ct_curl_set_cainfo(ct_curl_easy *c, const char *path)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_CAINFO, path ? path : ""));
    return c;
}

ct_curl_easy *ct_curl_add_header(ct_curl_easy *c, const char *name,
                                 const char *value)
{
    size_t need;
    char *line;
    struct curl_slist *grown;

    if (!ct_curl_ready(c)) return c;
    if (!name) name = "";
    if (!value) value = "";
    need = strlen(name) + strlen(value) + 3;
    line = (char *)malloc(need);
    if (!line) {
        c->init_error = 1;
        return c;
    }
    snprintf(line, need, "%s: %s", name, value);
    grown = curl_slist_append(c->headers, line);
    free(line);
    if (!grown) {
        c->init_error = 1;
        return c;
    }
    c->headers = grown;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, c->headers));
    return c;
}

ct_curl_easy *ct_curl_set_post(ct_curl_easy *c, const char *data, int len)
{
    char *copy;

    if (!ct_curl_ready(c)) return c;
    if (len < 0) len = data ? (int)strlen(data) : 0;
    if (len > 0 && !data) {
        c->init_error = 1;
        return c;
    }
    copy = (char *)malloc((size_t)len + 1);
    if (!copy) {
        c->init_error = 1;
        return c;
    }
    if (len > 0) memcpy(copy, data, (size_t)len);
    copy[len] = '\0';
    free(c->post_copy);
    c->post_copy = copy;
    c->post_len = len;
    /* Size first, fields second: with an explicit size libcurl never calls
       strlen on a body that may contain NUL. The copy stays alive until the
       next setPostFields / close, because libcurl does not copy the data. */
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_POSTFIELDSIZE, (long)len));
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_POSTFIELDS, c->post_copy));
    return c;
}

ct_curl_easy *ct_curl_set_body_limit(ct_curl_easy *c, int max_bytes)
{
    if (!c) return c;
    c->body_limit = max_bytes < 0 ? 0 : max_bytes;
    return c;
}

ct_curl_easy *ct_curl_set_write_function(ct_curl_easy *c, CurlWriteCb cb,
                                         void *ud)
{
    if (!ct_curl_ready(c)) return c;
    c->write_cb = cb;
    c->write_ud = ud;
    if (cb) {
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, cb));
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_WRITEDATA, ud));
    } else {
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION,
                                         ct_curl_default_write));
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_WRITEDATA, c));
    }
    return c;
}

ct_curl_easy *ct_curl_set_header_function(ct_curl_easy *c, CurlHeaderCb cb,
                                          void *ud)
{
    if (!ct_curl_ready(c)) return c;
    c->header_cb = cb;
    c->header_ud = ud;
    if (cb) {
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_HEADERFUNCTION, cb));
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_HEADERDATA, ud));
    } else {
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_HEADERFUNCTION,
                                         (curl_write_callback)0));
        ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_HEADERDATA,
                                         (void *)0));
    }
    return c;
}

/* ---- execution -------------------------------------------------------- */

ct_curl_easy *ct_curl_perform(ct_curl_easy *c)
{
    if (!c) return c;
    if (c->closed || !c->h || c->init_error) {
        c->last_code = (int)CURLE_FAILED_INIT;
        return c;
    }
    c->last_code = (int)curl_easy_perform(c->h);
    return c;
}

ct_curl_easy *ct_curl_get(ct_curl_easy *c)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_note(c, curl_easy_setopt(c->h, CURLOPT_HTTPGET, 1L));
    if (c->init_error) return c;
    return ct_curl_perform(c);
}

ct_curl_easy *ct_curl_post(ct_curl_easy *c, const char *data, int len)
{
    if (!ct_curl_ready(c)) return c;
    ct_curl_set_post(c, data, len);
    if (c->init_error) return c;
    return ct_curl_perform(c);
}

/* Reset every option, keep the connection cache, and re-install the pieces
   that are not options at all (error buffer, default sink) or that belong to
   the wrapper (the header slist, the POST copy, the body buffer). */
ct_curl_easy *ct_curl_reset(ct_curl_easy *c)
{
    if (!c) return c;
    if (c->h && !c->closed) {
        curl_easy_reset(c->h);
        curl_easy_setopt(c->h, CURLOPT_ERRORBUFFER, c->errbuf);
        curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, ct_curl_default_write);
        curl_easy_setopt(c->h, CURLOPT_WRITEDATA, c);
    }
    if (c->headers) {
        curl_slist_free_all(c->headers);
        c->headers = 0;
    }
    free(c->post_copy);
    c->post_copy = 0;
    c->post_len = 0;
    if (c->body) c->body[0] = '\0';
    c->body_len = 0;
    c->body_overflow = 0;
    c->body_limit = 0;
    c->write_cb = 0;
    c->write_ud = 0;
    c->header_cb = 0;
    c->header_ud = 0;
    c->last_code = 0;
    c->init_error = 0;
    return c;
}

ct_curl_easy *ct_curl_clear_body(ct_curl_easy *c)
{
    if (!c) return c;
    if (c->body) c->body[0] = '\0';
    c->body_len = 0;
    c->body_overflow = 0;
    return c;
}

/* Idempotent: a NULL or already-closed handle is a no-op. The wrapper and the
   body buffer survive so they can still be read. */
void ct_curl_close(ct_curl_easy *c)
{
    if (!c) return;
    if (c->closed) return;
    if (c->h) {
        curl_easy_cleanup(c->h);
        c->h = 0;
    }
    if (c->headers) {
        /* The slist must outlive the easy handle, so this runs after
           curl_easy_cleanup - never before. */
        curl_slist_free_all(c->headers);
        c->headers = 0;
    }
    free(c->post_copy);
    c->post_copy = 0;
    c->post_len = 0;
    c->closed = 1;
}

/* Destroys the wrapper itself. Call it exactly once; close() is the
   repeatable one. */
void ct_curl_destroy(ct_curl_easy *c)
{
    if (!c) return;
    if (!c->closed) ct_curl_close(c);
    free(c->body);
    free(c);
}

/* ---- reading ---------------------------------------------------------- */

int ct_curl_status(ct_curl_easy *c)
{
    return c ? c->last_code : (int)CURLE_FAILED_INIT;
}

const char *ct_curl_message(ct_curl_easy *c)
{
    if (!c) return curl_easy_strerror(CURLE_FAILED_INIT);
    return curl_easy_strerror((CURLcode)c->last_code);
}

const char *ct_curl_error_text(ct_curl_easy *c)
{
    if (!c) return "";
    return c->errbuf;
}

int ct_curl_response_code(ct_curl_easy *c)
{
    long v = 0;

    if (!c || !c->h || c->closed) return 0;
    if (curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &v) != CURLE_OK) {
        return 0;
    }
    return (int)v;
}

const char *ct_curl_effective_url(ct_curl_easy *c)
{
    char *v = 0;

    if (!c || !c->h || c->closed) return "";
    if (curl_easy_getinfo(c->h, CURLINFO_EFFECTIVE_URL, &v) != CURLE_OK) {
        return "";
    }
    return v ? v : "";
}

const char *ct_curl_content_type(ct_curl_easy *c)
{
    char *v = 0;

    if (!c || !c->h || c->closed) return "";
    if (curl_easy_getinfo(c->h, CURLINFO_CONTENT_TYPE, &v) != CURLE_OK) {
        return "";
    }
    return v ? v : "";
}

double ct_curl_total_time(ct_curl_easy *c)
{
    double v = 0.0;

    if (!c || !c->h || c->closed) return 0.0;
    if (curl_easy_getinfo(c->h, CURLINFO_TOTAL_TIME, &v) != CURLE_OK) {
        return 0.0;
    }
    return v;
}

long long ct_curl_size_download(ct_curl_easy *c)
{
    curl_off_t v = 0;

    if (!c || !c->h || c->closed) return 0;
    if (curl_easy_getinfo(c->h, CURLINFO_SIZE_DOWNLOAD_T, &v) != CURLE_OK) {
        return 0;
    }
    return (long long)v;
}

const char *ct_curl_body(ct_curl_easy *c)
{
    if (!c || !c->body) return "";
    return c->body;
}

int ct_curl_body_len(ct_curl_easy *c)
{
    return c ? c->body_len : 0;
}

/* ---- namespace -------------------------------------------------------- */

int ct_curl_global_init(void)
{
    return (int)curl_global_init(CURL_GLOBAL_DEFAULT);
}

void ct_curl_global_cleanup(void)
{
    curl_global_cleanup();
}

const char *ct_curl_version(void)
{
    return curl_version();
}

const char *ct_curl_strerror(int code)
{
    return curl_easy_strerror((CURLcode)code);
}
