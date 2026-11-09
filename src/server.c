/*
 * Dellgado.invest - minimal static file server.
 *
 * Pure C17 + POSIX (sockets, pthreads). No frameworks, no TLS (the hosting
 * platform terminates HTTPS in front of this process).
 *
 * Environment:
 *   PORT                 TCP port to listen on (default 8080)
 *   DOCROOT              directory with static files (default "public")
 *   REQUEST_TIMEOUT_SEC  seconds allowed to receive request headers (default 10)
 */
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI (realpath) */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_HEADER_BYTES   8192
#define MAX_CONNECTIONS    256
#define SEND_TIMEOUT_SEC   30
#define SHUTDOWN_GRACE_SEC 10
#define FILE_CHUNK         65536
#define THREAD_STACK_SIZE  (512 * 1024)

static const char SECURITY_HEADERS[] =
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: strict-origin-when-cross-origin\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'self'; "
    "script-src 'self' 'unsafe-inline'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src https://fonts.gstatic.com; "
    "img-src 'self' data:\r\n";

static const char PAGE_404[] =
    "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Not found - Dellgado.invest</title>"
    "<style>body{margin:0;min-height:100vh;display:grid;place-items:center;"
    "background:#B6DFED;color:#002941;font-family:system-ui,sans-serif;"
    "text-align:center;padding:16px}"
    "@media (prefers-color-scheme:dark){body{background:#002941;color:#B6DFED}}"
    "h1{font-size:4rem;margin:0}a{color:inherit}</style></head>"
    "<body><main><h1>404</h1><p>This page does not exist.</p>"
    "<p><a href=\"/\">Back to Dellgado.invest</a></p></main></body></html>\n";

/* ------------------------------------------------------------------------ */
/* Global state                                                             */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;
static char *g_docroot = NULL;      /* realpath of document root */
static size_t g_docroot_len = 0;
static int g_request_timeout = 10;

static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_conn_cv = PTHREAD_COND_INITIALIZER;
static int g_active = 0;

struct conn {
    int fd;
    char ip[INET6_ADDRSTRLEN];
};

struct request {
    char method[16];
    char target[MAX_HEADER_BYTES];   /* raw request-target, for the log */
    char forwarded[64];              /* first X-Forwarded-For entry */
};

struct result {
    int status;
    long long bytes;                 /* body bytes sent */
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return def;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || *end != '\0' || n < lo || n > hi) {
        fprintf(stderr, "invalid %s=%s, using %d\n", name, v, def);
        return def;
    }
    return (int)n;
}

/* Write everything; returns 0 on success, -1 on error. */
static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

static const char *mime_type(const char *path)
{
    static const struct { const char *ext; const char *type; } map[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "mjs",  "text/javascript; charset=utf-8" },
        { "json", "application/json" },
        { "webmanifest", "application/manifest+json" },
        { "txt",  "text/plain; charset=utf-8" },
        { "xml",  "application/xml" },
        { "svg",  "image/svg+xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "webp", "image/webp" },
        { "avif", "image/avif" },
        { "ico",  "image/x-icon" },
        { "woff", "font/woff" },
        { "woff2", "font/woff2" },
        { "pdf",  "application/pdf" },
        { "mp4",  "video/mp4" },
        { "webm", "video/webm" },
    };
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (dot == NULL || (slash != NULL && dot < slash))
        return "application/octet-stream";
    dot++;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcasecmp(dot, map[i].ext) == 0)
            return map[i].type;
    return "application/octet-stream";
}

/* Send status line + common headers. extra may be NULL. */
static int send_head(int fd, int status, const char *ctype, long long length,
                     const char *extra)
{
    char hdr[2048];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: dellgado\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: close\r\n"
                     "%s%s\r\n",
                     status, status_text(status), ctype, length,
                     SECURITY_HEADERS, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof hdr)
        return -1;
    return send_all(fd, hdr, (size_t)n);
}

/* Send a small in-memory response. */
static void send_simple(int fd, bool head_only, int status, const char *ctype,
                        const char *body, const char *extra, struct result *res)
{
    size_t len = strlen(body);
    res->status = status;
    res->bytes = 0;
    if (send_head(fd, status, ctype, (long long)len, extra) != 0)
        return;
    if (!head_only && send_all(fd, body, len) == 0)
        res->bytes = (long long)len;
}

static void send_error(int fd, bool head_only, int status, struct result *res)
{
    if (status == 404) {
        send_simple(fd, head_only, 404, "text/html; charset=utf-8", PAGE_404,
                    "Cache-Control: no-cache\r\n", res);
        return;
    }
    char body[128];
    snprintf(body, sizeof body, "%d %s\n", status, status_text(status));
    send_simple(fd, head_only, status, "text/plain; charset=utf-8", body,
                status == 405 ? "Allow: GET, HEAD\r\n" : NULL, res);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Turn a request-target into a safe, decoded URL path.
 * Returns 0 on success, otherwise an HTTP status code to answer with.
 */
static int decode_path(const char *target, char *out, size_t outsz)
{
    const char *p = target;

    /* absolute-form (RFC 9112 3.2.2): strip scheme and authority */
    if (strncasecmp(p, "http://", 7) == 0 || strncasecmp(p, "https://", 8) == 0) {
        p = strstr(p, "://") + 3;
        p = strchr(p, '/');
        if (p == NULL)
            p = "/";
    }
    if (*p != '/')
        return 400;

    size_t o = 0;
    for (; *p != '\0' && *p != '?' && *p != '#'; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int hi = hexval((unsigned char)p[1]);
            int lo = hi < 0 ? -1 : hexval((unsigned char)p[2]);
            if (hi < 0 || lo < 0)
                return 400;
            c = hi * 16 + lo;
            p += 2;
        }
        if (c == 0 || c < 0x20 || c == 0x7f || c == '\\')
            return 400;
        if (o + 1 >= outsz)
            return 414;
        out[o++] = (char)c;
    }
    out[o] = '\0';

    /* Reject "." / ".." segments and hidden files (".git", ".env", ...). */
    for (const char *s = out; *s != '\0'; ) {
        if (*s == '/') {
            s++;
            continue;
        }
        const char *e = strchr(s, '/');
        size_t seglen = e ? (size_t)(e - s) : strlen(s);
        if (s[0] == '.') {
            if ((seglen == 1) || (seglen == 2 && s[1] == '.'))
                return 400;
            return 404;
        }
        s += seglen;
    }
    return 0;
}

/* realpath() + containment check. Returns malloc'd path or NULL. */
static char *resolve_inside_docroot(const char *candidate)
{
    char *real = realpath(candidate, NULL);
    if (real == NULL)
        return NULL;
    if (strncmp(real, g_docroot, g_docroot_len) != 0 ||
        (real[g_docroot_len] != '/' && real[g_docroot_len] != '\0')) {
        free(real);
        return NULL;
    }
    return real;
}

static void serve_file(int fd, bool head_only, const char *urlpath,
                       struct result *res)
{
    char candidate[PATH_MAX];
    size_t ulen = strlen(urlpath);
    const char *suffix = (ulen > 0 && urlpath[ulen - 1] == '/') ? "index.html" : "";
    int n = snprintf(candidate, sizeof candidate, "%s%s%s", g_docroot, urlpath, suffix);
    if (n < 0 || (size_t)n >= sizeof candidate) {
        send_error(fd, head_only, 414, res);
        return;
    }

    char *real = resolve_inside_docroot(candidate);
    if (real == NULL) {
        send_error(fd, head_only, 404, res);
        return;
    }

    struct stat st;
    if (stat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* directory without trailing slash: serve its index.html */
        n = snprintf(candidate, sizeof candidate, "%s/index.html", real);
        free(real);
        real = NULL;
        if (n > 0 && (size_t)n < sizeof candidate)
            real = resolve_inside_docroot(candidate);
        if (real == NULL) {
            send_error(fd, head_only, 404, res);
            return;
        }
    }

    int file = open(real, O_RDONLY);
    if (file < 0) {
        send_error(fd, head_only, errno == EACCES ? 403 : 404, res);
        free(real);
        return;
    }
    if (fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(file);
        free(real);
        send_error(fd, head_only, 404, res);
        return;
    }

    const char *ctype = mime_type(real);
    const char *cache = strncmp(ctype, "text/html", 9) == 0
                            ? "Cache-Control: no-cache\r\n"
                            : "Cache-Control: public, max-age=3600\r\n";
    free(real);

    res->status = 200;
    res->bytes = 0;
    if (send_head(fd, 200, ctype, (long long)st.st_size, cache) != 0 || head_only) {
        close(file);
        return;
    }

    char buf[FILE_CHUNK];
    for (;;) {
        ssize_t r = read(file, buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        if (send_all(fd, buf, (size_t)r) != 0)
            break;
        res->bytes += r;
    }
    close(file);
}

/*
 * Read request headers into buf (NUL-terminated).
 * Returns header length, 0 if the client closed without sending anything,
 * or -status on error (-408, -431, -400).
 */
static int read_headers(int fd, char *buf, size_t bufsz)
{
    size_t len = 0;
    double deadline = now_ms() + g_request_timeout * 1000.0;

    for (;;) {
        double left = deadline - now_ms();
        if (left <= 0)
            return len == 0 ? 0 : -408;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)left + 1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return len == 0 ? 0 : -400;
        }
        if (pr == 0)
            return -408;

        if (len >= bufsz - 1)
            return -431;
        ssize_t n = recv(fd, buf + len, bufsz - 1 - len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -408;
            return len == 0 ? 0 : -400;
        }
        if (n == 0)
            return len == 0 ? 0 : -400;
        if (memchr(buf + len, '\0', (size_t)n) != NULL)
            return -400;
        len += (size_t)n;
        buf[len] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL || strstr(buf, "\n\n") != NULL)
            return (int)len;
        if (len >= bufsz - 1)
            return -431;
    }
}

/* Copy src into dst, replacing anything non-printable, for safe logging. */
static void sanitize(char *dst, size_t dstsz, const char *src, size_t maxin)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && i < maxin && o + 1 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '?';
    }
    dst[o] = '\0';
}

/* Parse request line and the few headers we care about. 0 or HTTP status. */
static int parse_request(char *buf, struct request *req)
{
    char *line_end = strpbrk(buf, "\r\n");
    if (line_end == NULL)
        return 400;
    *line_end = '\0';

    char *sp1 = strchr(buf, ' ');
    if (sp1 == NULL)
        return 400;
    char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL || strchr(sp2 + 1, ' ') != NULL)
        return 400;
    *sp1 = '\0';
    *sp2 = '\0';
    const char *method = buf, *target = sp1 + 1, *version = sp2 + 1;

    size_t mlen = strlen(method);
    if (mlen == 0 || mlen >= sizeof req->method)
        return 400;
    for (size_t i = 0; i < mlen; i++)
        if (method[i] < 'A' || method[i] > 'Z')
            return 400;
    memcpy(req->method, method, mlen + 1);
    snprintf(req->target, sizeof req->target, "%s", target);

    if (strncmp(version, "HTTP/", 5) != 0)
        return 400;
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        return 505;
    if (*target == '\0')
        return 400;

    /* Headers: only X-Forwarded-For is read (for the access log). */
    char *p = line_end + 1;
    while (*p != '\0') {
        if (*p == '\n') { p++; continue; }
        char *eol = strpbrk(p, "\r\n");
        if (eol == NULL)
            break;
        *eol = '\0';
        if (strncasecmp(p, "X-Forwarded-For:", 16) == 0) {
            const char *v = p + 16;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vl = strcspn(v, ", \t");
            sanitize(req->forwarded, sizeof req->forwarded, v, vl);
        }
        p = eol + 1;
    }
    return 0;
}

static void log_request(const struct conn *c, const struct request *req,
                        const struct result *res, double ms)
{
    char target[512];
    sanitize(target, sizeof target, req->target[0] ? req->target : "-",
             sizeof target - 1);

    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stdout, "%s %s \"%s %s\" %d %lld %.1fms\n", ts,
            req->forwarded[0] ? req->forwarded : c->ip,
            req->method[0] ? req->method : "-", target,
            res->status, res->bytes, ms);
}

static void handle_connection(struct conn *c)
{
    double start = now_ms();
    char buf[MAX_HEADER_BYTES + 1];
    struct request req;
    struct result res = { 0, 0 };
    memset(&req, 0, sizeof req);

    int n = read_headers(c->fd, buf, sizeof buf);
    if (n == 0)
        return; /* client connected and left: nothing to log */
    if (n < 0) {
        send_error(c->fd, false, -n, &res);
        log_request(c, &req, &res, now_ms() - start);
        return;
    }

    int err = parse_request(buf, &req);
    bool head_only = strcmp(req.method, "HEAD") == 0;
    if (err == 0 && !head_only && strcmp(req.method, "GET") != 0)
        err = 405;

    char path[MAX_HEADER_BYTES];
    if (err == 0)
        err = decode_path(req.target, path, sizeof path);

    if (err != 0)
        send_error(c->fd, head_only, err, &res);
    else if (strcmp(path, "/healthz") == 0)
        send_simple(c->fd, head_only, 200, "text/plain; charset=utf-8", "ok",
                    "Cache-Control: no-store\r\n", &res);
    else
        serve_file(c->fd, head_only, path, &res);

    log_request(c, &req, &res, now_ms() - start);
}

static void *connection_thread(void *arg)
{
    struct conn *c = arg;
    handle_connection(c);

    /* Graceful close: stop sending, drain briefly so the client gets
       everything instead of a reset. */
    shutdown(c->fd, SHUT_WR);
    char drain[512];
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    for (int i = 0; i < 8 && poll(&pfd, 1, 200) > 0; i++)
        if (recv(c->fd, drain, sizeof drain, 0) <= 0)
            break;
    close(c->fd);
    free(c);

    pthread_mutex_lock(&g_conn_mu);
    g_active--;
    pthread_cond_broadcast(&g_conn_cv);
    pthread_mutex_unlock(&g_conn_mu);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */

static int open_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: let poll() return EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int port = env_int("PORT", 8080, 1, 65535);
    g_request_timeout = env_int("REQUEST_TIMEOUT_SEC", 10, 1, 300);
    const char *docroot = getenv("DOCROOT");
    if (docroot == NULL || *docroot == '\0')
        docroot = "public";

    g_docroot = realpath(docroot, NULL);
    struct stat st;
    if (g_docroot == NULL || stat(g_docroot, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "document root '%s' is not a directory\n", docroot);
        free(g_docroot);
        return 1;
    }
    g_docroot_len = strlen(g_docroot);

    int lfd = open_listener(port);
    if (lfd < 0) {
        free(g_docroot);
        return 1;
    }
    printf("listening on 0.0.0.0:%d, serving %s\n", port, g_docroot);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);

    struct timeval rcv = { .tv_sec = g_request_timeout, .tv_usec = 0 };
    struct timeval snd = { .tv_sec = SEND_TIMEOUT_SEC, .tv_usec = 0 };

    while (!g_stop) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0)
            continue; /* timeout or EINTR: re-check g_stop */

        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                struct timespec pause = { 0, 50 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            continue;
        }
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);

        pthread_mutex_lock(&g_conn_mu);
        bool full = g_active >= MAX_CONNECTIONS;
        if (!full)
            g_active++;
        pthread_mutex_unlock(&g_conn_mu);
        if (full) {
            struct result r;
            send_error(cfd, false, 503, &r);
            close(cfd);
            continue;
        }

        struct conn *c = calloc(1, sizeof *c);
        if (c == NULL) {
            close(cfd);
            pthread_mutex_lock(&g_conn_mu);
            g_active--;
            pthread_mutex_unlock(&g_conn_mu);
            continue;
        }
        c->fd = cfd;
        if (peer.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr,
                      c->ip, sizeof c->ip);
        else if (peer.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr,
                      c->ip, sizeof c->ip);
        else
            snprintf(c->ip, sizeof c->ip, "-");

        pthread_t tid;
        if (pthread_create(&tid, &attr, connection_thread, c) != 0) {
            close(cfd);
            free(c);
            pthread_mutex_lock(&g_conn_mu);
            g_active--;
            pthread_mutex_unlock(&g_conn_mu);
        }
    }

    printf("shutdown requested, closing listener\n");
    close(lfd);

    /* Let in-flight requests finish, but not forever. */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += SHUTDOWN_GRACE_SEC;
    pthread_mutex_lock(&g_conn_mu);
    while (g_active > 0) {
        if (pthread_cond_timedwait(&g_conn_cv, &g_conn_mu, &until) == ETIMEDOUT)
            break;
    }
    int left = g_active;
    pthread_mutex_unlock(&g_conn_mu);

    pthread_attr_destroy(&attr);
    printf("stopped (%d connection(s) still open)\n", left);
    if (left == 0)
        free(g_docroot); /* still in use by any remaining threads otherwise */
    return 0;
}
