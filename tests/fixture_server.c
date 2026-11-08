/*
 * fixture_server.c - the loopback HTTP fixture used by tests/run.sh.
 *
 * Usage:  fixture_server <portfile>
 *
 * Binds 127.0.0.1:0, writes the chosen port to <portfile>, then accepts
 * connections forever. Each connection is handled in a forked child, so the
 * /slow endpoint (which sleeps) cannot block the others, and an aborted
 * transfer (the timeout test) only kills its own child.
 *
 * Routes:
 *   GET  /hello     200 "hello, curl\n"
 *   GET  /redirect  302 Location: /hello
 *   GET  /missing   404 "not found\n"
 *   GET  /slow      sleeps 3s, then 200 "hello, curl\n"
 *   GET  /large     200, 100000 'x' bytes
 *   ANY  /echo      200, echoes the request body
 *   GET  /header    200, the X-Test request header value
 *   GET  /ua        200, the User-Agent request header value
 *   GET  /auth      200 "ok" for user:pass, else 401 "denied"
 *   anything else   404
 *
 * License: MIT (see LICENSE).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQUEST 8192
#define MAX_BODY (1 << 20)
#define LARGE_BYTES 100000

static void send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *reason, const char *ctype,
                    const char *body, size_t blen)
{
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, ctype, (unsigned long)blen);
    if (n > 0) send_all(fd, head, (size_t)n);
    if (blen > 0) send_all(fd, body, blen);
}

static void copy_trim(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    while (*src == ' ' || *src == '\t') src++;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) i--;
    dst[i] = '\0';
}

static void handle(int fd)
{
    char req[MAX_REQUEST];
    size_t used = 0;
    char *hdr_end = 0;
    char method[16];
    char path[256];
    char xtest[256];
    char auth[256];
    char ua[256];
    long clen = 0;
    char *p;
    char *body;
    size_t body_off;
    size_t have;
    size_t got;

    method[0] = '\0';
    path[0] = '\0';
    xtest[0] = '\0';
    auth[0] = '\0';
    ua[0] = '\0';

    for (;;) {
        ssize_t r;

        if (used + 1 >= sizeof req) break;
        r = read(fd, req + used, sizeof req - 1 - used);
        if (r <= 0) return;
        used += (size_t)r;
        req[used] = '\0';
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) break;
    }
    if (!hdr_end) return;

    /* request line: METHOD path HTTP/1.1 */
    p = strstr(req, "\r\n");
    if (!p) return;
    *p = '\0';
    {
        char *hdr_start = p + 2;
        char *q;
        char *sp1 = strchr(req, ' ');
        if (sp1) {
            size_t mlen = (size_t)(sp1 - req);
            char *sp2;

            if (mlen >= sizeof method) mlen = sizeof method - 1;
            memcpy(method, req, mlen);
            method[mlen] = '\0';
            sp2 = strchr(sp1 + 1, ' ');
            if (sp2) *sp2 = '\0';
            copy_trim(path, sizeof path, sp1 + 1);
        }

        /* headers, between hdr_start and hdr_end */
        q = hdr_start;
        while (q < hdr_end) {
            char *e = strstr(q, "\r\n");

            if (!e || e > hdr_end) break;
            *e = '\0';
            if (strncasecmp(q, "Content-Length:", 15) == 0) {
                clen = atol(q + 15);
            } else if (strncasecmp(q, "X-Test:", 7) == 0) {
                copy_trim(xtest, sizeof xtest, q + 7);
            } else if (strncasecmp(q, "Authorization:", 14) == 0) {
                copy_trim(auth, sizeof auth, q + 14);
            } else if (strncasecmp(q, "User-Agent:", 11) == 0) {
                copy_trim(ua, sizeof ua, q + 11);
            }
            q = e + 2;
        }
    }

    if (clen < 0 || clen > MAX_BODY) clen = 0;
    body = (char *)malloc((size_t)clen + 1);
    if (!body) return;
    body_off = (size_t)(hdr_end + 4 - req);
    have = used > body_off ? used - body_off : 0;
    if (have > (size_t)clen) have = (size_t)clen;
    memcpy(body, req + body_off, have);
    got = have;
    while (got < (size_t)clen) {
        ssize_t r = read(fd, body + got, (size_t)clen - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    body[got] = '\0';

    if (strcmp(path, "/hello") == 0) {
        respond(fd, 200, "OK", "text/plain", "hello, curl\n", 12);
    } else if (strcmp(path, "/redirect") == 0) {
        static const char *r = "HTTP/1.1 302 Found\r\n"
                               "Location: /hello\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
        send_all(fd, r, strlen(r));
    } else if (strcmp(path, "/missing") == 0) {
        respond(fd, 404, "Not Found", "text/plain", "not found\n", 10);
    } else if (strcmp(path, "/slow") == 0) {
        sleep(3);
        respond(fd, 200, "OK", "text/plain", "hello, curl\n", 12);
    } else if (strcmp(path, "/large") == 0) {
        char *big = (char *)malloc(LARGE_BYTES);
        if (big) {
            memset(big, 'x', LARGE_BYTES);
            respond(fd, 200, "OK", "text/plain", big, LARGE_BYTES);
            free(big);
        } else {
            respond(fd, 500, "Internal Server Error", "text/plain", "", 0);
        }
    } else if (strcmp(path, "/echo") == 0) {
        respond(fd, 200, "OK", "text/plain", body, got);
    } else if (strcmp(path, "/header") == 0) {
        respond(fd, 200, "OK", "text/plain", xtest, strlen(xtest));
    } else if (strcmp(path, "/ua") == 0) {
        respond(fd, 200, "OK", "text/plain", ua, strlen(ua));
    } else if (strcmp(path, "/auth") == 0) {
        if (strcmp(auth, "Basic dXNlcjpwYXNz") == 0) {
            respond(fd, 200, "OK", "text/plain", "ok", 2);
        } else {
            respond(fd, 401, "Unauthorized", "text/plain", "denied", 6);
        }
    } else {
        respond(fd, 404, "Not Found", "text/plain", "not found\n", 10);
    }
    free(body);
}

int main(int argc, char **argv)
{
    int listen_fd;
    struct sockaddr_in addr;
    socklen_t alen;
    FILE *f;
    int on = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <portfile>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 16) != 0) {
        perror("listen");
        return 1;
    }
    alen = sizeof addr;
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        perror("getsockname");
        return 1;
    }
    f = fopen(argv[1], "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "%d\n", (int)ntohs(addr.sin_port));
    fclose(f);

    for (;;) {
        int fd = accept(listen_fd, 0, 0);
        pid_t pid;

        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pid = fork();
        if (pid == 0) {
            close(listen_fd);
            handle(fd);
            close(fd);
            _exit(0);
        }
        close(fd);
    }
    close(listen_fd);
    return 0;
}
