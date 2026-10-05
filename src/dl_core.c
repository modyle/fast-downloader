/* dl_core.c - многопоточный загрузчик.
 * Схема как в C#-версии: probe (HEAD + проверочный Range),
 * дальше либо один поток, либо очередь блоков + адаптивный контроллер.
 * Только http:// (без TLS). Весь текст ошибок - английский (ASCII),
 * чтобы не зависеть от кодировки исходников.
 */
#include "dl_core.h"
#include "dl_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define strncasecmp _strnicmp
#endif

#define DL_MEASURE_MS 1500
#define DL_GROW 1.05
#define DL_MIN_BLOCK (256 * 1024)
#define DL_BUF (64 * 1024)
#define DL_MAX_REDIRECTS 5

struct dl {
    char url[2048];
    char path[1024];
    int max_threads;
    int adaptive;
    dl_mutex_t mu;
    volatile int stop;
    dl_thread_t controller;
    int started;
    int sock_inited;

    int state;
    char error[DL_ERR_SIZE];
    long long total;
    long long downloaded;
    double speed;
    double best;
    int stag;
    int frozen;

    long long block_size;
    int block_count;
    int *queue;
    int qh;
    int qt;
    int qcap;
    int *retries;
    dl_thread_t workers[DL_MAX_THREADS];
    int alive;
};

/* ================= URL ================= */

static int parse_url(const char *url, char *host, int hostcap,
                     int *port, char *path, int pathcap)
{
    const char *p;
    const char *slash;
    const char *colon;
    size_t hlen;

    if (strncmp(url, "http://", 7) != 0) {
        return -1; /* https и остальное не поддерживаем */
    }
    p = url + 7;
    slash = strchr(p, '/');
    if (slash != NULL) {
        hlen = (size_t)(slash - p);
        strncpy(path, slash, (size_t)(pathcap - 1));
        path[pathcap - 1] = 0;
    } else {
        hlen = strlen(p);
        strncpy(path, "/", (size_t)(pathcap - 1));
        path[pathcap - 1] = 0;
    }
    colon = memchr(p, ':', hlen);
    if (colon != NULL) {
        size_t hpart = (size_t)(colon - p);
        if (hpart >= (size_t)hostcap) {
            hpart = (size_t)(hostcap - 1);
        }
        memcpy(host, p, hpart);
        host[hpart] = 0;
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) {
            return -1;
        }
    } else {
        if (hlen >= (size_t)hostcap) {
            hlen = (size_t)(hostcap - 1);
        }
        memcpy(host, p, hlen);
        host[hlen] = 0;
        *port = 80;
    }
    if (host[0] == 0) {
        return -1;
    }
    return 0;
}

/* ================= TCP ================= */

static dl_sock_t tcp_connect(const char *host, int port)
{
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    struct addrinfo *ai;
    char ports[16];
    dl_sock_t s = DL_SOCK_INVALID;
    int nfd = 0;

    sprintf(ports, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ports, &hints, &list) != 0) {
        return DL_SOCK_INVALID;
    }
    for (ai = list; ai != NULL; ai = ai->ai_next) {
        fd_set wf;
        fd_set ef;
        struct timeval tv;
        int sel;
        int err = 0;

        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == DL_SOCK_INVALID) {
            continue;
        }
#ifdef _WIN32
        {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
        }
        nfd = 0;
#else
        {
            int fl = fcntl(s, F_GETFL, 0);
            if (fl >= 0) {
                fcntl(s, F_SETFL, fl | O_NONBLOCK);
            }
        }
        nfd = (int)s + 1;
#endif
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                dl_sock_close(s);
                s = DL_SOCK_INVALID;
                continue;
            }
#else
            if (errno != EINPROGRESS) {
                dl_sock_close(s);
                s = DL_SOCK_INVALID;
                continue;
            }
#endif
        }
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        FD_ZERO(&ef);
        FD_SET(s, &ef);
        tv.tv_sec = 15;
        tv.tv_usec = 0;
        sel = select(nfd, NULL, &wf, &ef, &tv);
        if (sel <= 0) {
            dl_sock_close(s);
            s = DL_SOCK_INVALID;
            continue;
        }
        {
            socklen_t el = (socklen_t)sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
            if (err != 0) {
                dl_sock_close(s);
                s = DL_SOCK_INVALID;
                continue;
            }
        }
#ifdef _WIN32
        {
            u_long nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
        }
        {
            DWORD t = 30000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&t, (int)sizeof(t));
        }
#else
        {
            int fl = fcntl(s, F_GETFL, 0);
            if (fl >= 0) {
                fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
            }
        }
        {
            struct timeval t;
            t.tv_sec = 30;
            t.tv_usec = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
        }
#endif
        break;
    }
    freeaddrinfo(list);
    return s;
}

static int send_all(dl_sock_t s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r <= 0) {
            return -1;
        }
        sent += r;
    }
    return 0;
}

/* строка без \r\n, 0..n-1 символов, -1 при обрыве */
static int recv_line(dl_sock_t s, char *buf, int cap)
{
    int n = 0;
    while (n < cap - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) {
            if (n == 0) {
                return -1;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            buf[n++] = c;
        }
    }
    buf[n] = 0;
    return n;
}

/* ================= HTTP ================= */

typedef struct {
    dl_sock_t sock;
    int status;
    long long length;   /* -1 = нет/чанки */
    int chunked;
    int accept_ranges;
    long long range_total; /* из Content-Range, -1 */
} http_resp_t;

static int read_response(dl_sock_t s, http_resp_t *r, char *location, int loccap)
{
    char line[4096];
    int i;

    if (recv_line(s, line, (int)sizeof(line)) < 0) {
        return -1;
    }
    if (strncmp(line, "HTTP/", 5) != 0) {
        return -1;
    }
    r->status = atoi(line + 9);
    r->length = -1;
    r->chunked = 0;
    r->accept_ranges = 0;
    r->range_total = -1;
    if (location != NULL && loccap > 0) {
        location[0] = 0;
    }
    for (i = 0; i < 200; i++) {
        if (recv_line(s, line, (int)sizeof(line)) < 0) {
            return -1;
        }
        if (line[0] == 0) {
            break; /* конец заголовков */
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            r->length = atoll(line + 15);
        } else if (strncasecmp(line, "Accept-Ranges:", 14) == 0) {
            if (strstr(line + 14, "bytes") != NULL) {
                r->accept_ranges = 1;
            }
        } else if (strncasecmp(line, "Content-Range:", 14) == 0) {
            long long a = 0;
            long long b = 0;
            long long t = -1;
            /* бывает звёздочка вместо размера, тогда sscanf вернёт 2 */
            if (sscanf(line + 14, "bytes %lld-%lld/%lld", &a, &b, &t) >= 2) {
                r->range_total = t;
            }
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strstr(line + 18, "chunked") != NULL) {
                r->chunked = 1;
            }
        } else if (strncasecmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            if (location != NULL && loccap > 0) {
                strncpy(location, v, (size_t)(loccap - 1));
                location[loccap - 1] = 0;
            }
        }
    }
    return 0;
}

/* собрать абсолютный URL из Location (абсолютный или path-only) */
static void resolve_url(const char *base, const char *loc,
                        char *out, int outcap)
{
    if (strncmp(loc, "http://", 7) == 0) {
        strncpy(out, loc, (size_t)(outcap - 1));
        out[outcap - 1] = 0;
        return;
    }
    if (loc[0] == '/') {
        /* scheme://host[:port] + path */
        const char *p = strstr(base, "://");
        size_t n;
        p = (p != NULL) ? p + 3 : base;
        {
            const char *slash = strchr(p, '/');
            n = (slash != NULL) ? (size_t)(slash - base) : strlen(base);
        }
        if (n >= (size_t)(outcap - 1)) {
            n = (size_t)(outcap - 2);
        }
        memcpy(out, base, n);
        out[n] = 0;
        strncat(out, loc, (size_t)(outcap - (int)n - 1));
        return;
    }
    /* относительный без слэша - не умеем, отдаём как есть */
    strncpy(out, loc, (size_t)(outcap - 1));
    out[outcap - 1] = 0;
}

static int http_open(const char *url, const char *method,
                     int use_range, long long rfrom, long long rto,
                     http_resp_t *out)
{
    char cur[2048];
    char host[256];
    char path[2048];
    char req[8192];
    char location[2048];
    int redir;
    int port;

    strncpy(cur, url, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    for (redir = 0; redir < DL_MAX_REDIRECTS; redir++) {
        dl_sock_t s;
        int n;
        http_resp_t r;

        if (parse_url(cur, host, (int)sizeof(host),
                      &port, path, (int)sizeof(path)) != 0) {
            return -1;
        }
        s = tcp_connect(host, port);
        if (s == DL_SOCK_INVALID) {
            return -1;
        }
        if (use_range) {
            n = snprintf(req, sizeof(req),
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: fast-downloader-c/1.0\r\n"
                         "Range: bytes=%lld-%lld\r\n"
                         "Connection: close\r\n\r\n",
                         method, path, host, rfrom, rto);
        } else {
            n = snprintf(req, sizeof(req),
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: fast-downloader-c/1.0\r\n"
                         "Connection: close\r\n\r\n",
                         method, path, host);
        }
        if (n <= 0 || n >= (int)sizeof(req)) {
            dl_sock_close(s);
            return -1;
        }
        if (send_all(s, req, n) != 0) {
            dl_sock_close(s);
            return -1;
        }
        r.sock = s;
        if (read_response(s, &r, location, (int)sizeof(location)) != 0) {
            dl_sock_close(s);
            return -1;
        }
        if ((r.status == 301 || r.status == 302 || r.status == 303 ||
             r.status == 307 || r.status == 308) && location[0] != 0) {
            dl_sock_close(s);
            resolve_url(cur, location, cur, (int)sizeof(cur));
            continue;
        }
        *out = r;
        return 0;
    }
    return -1; /* слишком много редиректов */
}

static void http_close(http_resp_t *r)
{
    dl_sock_close(r->sock);
    r->sock = DL_SOCK_INVALID;
}

/* ================= probe ================= */

static void probe_url(downloader_t *d, long long *total, int *ranges)
{
    http_resp_t r;

    *total = -1;
    *ranges = 0;
    if (http_open(d->url, "HEAD", 0, 0, 0, &r) == 0) {
        *total = r.length;
        if (r.accept_ranges) {
            *ranges = 1;
        }
        http_close(&r);
    }
    if (http_open(d->url, "GET", 1, 0, 0, &r) == 0) {
        if (r.status == 206) {
            *ranges = 1;
            if (r.range_total > 0) {
                *total = r.range_total;
            }
        } else {
            *ranges = 0;
        }
        http_close(&r);
    }
}

/* ================= состояние ================= */

static void set_fail(downloader_t *d, const char *msg)
{
    dl_mutex_lock(&d->mu);
    d->state = DL_FAIL;
    strncpy(d->error, msg, sizeof(d->error) - 1);
    d->error[sizeof(d->error) - 1] = 0;
    dl_mutex_unlock(&d->mu);
}

static void set_done(downloader_t *d, int st)
{
    dl_mutex_lock(&d->mu);
    d->state = st;
    if (st == DL_OK && d->total > 0) {
        d->downloaded = d->total;
    }
    dl_mutex_unlock(&d->mu);
}

static long long file_len(const char *path)
{
    FILE *f;
    long long n;
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    n = (long long)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    n = (long long)ftello(f);
#endif
    fclose(f);
    return n;
}

/* ================= один поток ================= */

/* чтение chunked-тела в файл, 0 = ok */
static int read_chunked(dl_sock_t s, FILE *f, downloader_t *d,
                        long long *got)
{
    char line[128];
    char *buf;

    buf = (char *)malloc(DL_BUF);
    if (buf == NULL) {
        return -1;
    }
    for (;;) {
        long chunk;
        long left;
        if (d->stop) {
            free(buf);
            return -2; /* стоп */
        }
        if (recv_line(s, line, (int)sizeof(line)) < 0) {
            free(buf);
            return -1;
        }
        chunk = strtol(line, NULL, 16);
        if (chunk < 0) {
            free(buf);
            return -1;
        }
        if (chunk == 0) {
            /* трейлеры до пустой строки */
            for (;;) {
                if (recv_line(s, line, (int)sizeof(line)) < 0) {
                    break;
                }
                if (line[0] == 0) {
                    break;
                }
            }
            free(buf);
            return 0;
        }
        left = chunk;
        while (left > 0) {
            int want = (left > DL_BUF) ? DL_BUF : (int)left;
            int r = recv(s, buf, want, 0);
            if (r <= 0) {
                free(buf);
                return -1;
            }
            fwrite(buf, 1, (size_t)r, f);
            left -= r;
            dl_mutex_lock(&d->mu);
            d->downloaded += r;
            dl_mutex_unlock(&d->mu);
            if (got != NULL) {
                *got += r;
            }
        }
        /* CRLF после чанка */
        {
            char crlf[2];
            int r = recv(s, crlf, 2, 0);
            (void)r;
        }
    }
}

static void download_single(downloader_t *d)
{
    http_resp_t r;
    FILE *f;
    char *buf;
    double start;
    double last;
    long long last_dl = 0;

    if (http_open(d->url, "GET", 0, 0, 0, &r) != 0) {
        set_fail(d, "connect/request failed");
        return;
    }
    if (r.status != 200) {
        http_close(&r);
        set_fail(d, "server did not return 200");
        return;
    }
    dl_mutex_lock(&d->mu);
    d->total = r.length;
    dl_mutex_unlock(&d->mu);

    f = fopen(d->path, "wb");
    if (f == NULL) {
        http_close(&r);
        set_fail(d, "cannot create file");
        return;
    }
    buf = (char *)malloc(DL_BUF);
    if (buf == NULL) {
        fclose(f);
        http_close(&r);
        set_fail(d, "out of memory");
        return;
    }
    start = dl_now();
    last = start;
    if (r.chunked) {
        int rc = read_chunked(r.sock, f, d, NULL);
        free(buf);
        fclose(f);
        http_close(&r);
        if (d->stop) {
            set_done(d, DL_CANCEL);
        } else if (rc == 0) {
            set_done(d, DL_OK);
        } else {
            set_fail(d, "chunked body broken");
        }
        return;
    }
    if (r.length >= 0) {
        long long left = r.length;
        while (left > 0) {
            int want = (left > DL_BUF) ? DL_BUF : (int)left;
            int got;
            double now;
            if (d->stop) {
                break;
            }
            got = recv(r.sock, buf, want, 0);
            if (got <= 0) {
                break;
            }
            fwrite(buf, 1, (size_t)got, f);
            left -= got;
            dl_mutex_lock(&d->mu);
            d->downloaded += got;
            dl_mutex_unlock(&d->mu);
            now = dl_now();
            if (now - last >= 1.0) {
                long long cur;
                dl_mutex_lock(&d->mu);
                cur = d->downloaded;
                d->speed = (double)(cur - last_dl) / (now - last);
                if (d->speed > d->best) {
                    d->best = d->speed;
                }
                dl_mutex_unlock(&d->mu);
                last_dl = cur;
                last = now;
            }
        }
        free(buf);
        fclose(f);
        http_close(&r);
        if (d->stop) {
            set_done(d, DL_CANCEL);
        } else if (left == 0) {
            set_done(d, DL_OK);
        } else {
            set_fail(d, "connection cut (short body)");
        }
        return;
    }
    /* длина неизвестна - читаем до закрытия */
    for (;;) {
        int got;
        double now;
        if (d->stop) {
            break;
        }
        got = recv(r.sock, buf, DL_BUF, 0);
        if (got <= 0) {
            break;
        }
        fwrite(buf, 1, (size_t)got, f);
        dl_mutex_lock(&d->mu);
        d->downloaded += got;
        dl_mutex_unlock(&d->mu);
        now = dl_now();
        if (now - last >= 1.0) {
            long long cur;
            dl_mutex_lock(&d->mu);
            cur = d->downloaded;
            d->speed = (double)(cur - last_dl) / (now - last);
            if (d->speed > d->best) {
                d->best = d->speed;
            }
            dl_mutex_unlock(&d->mu);
            last_dl = cur;
            last = now;
        }
    }
    free(buf);
    fclose(f);
    http_close(&r);
    if (d->stop) {
        set_done(d, DL_CANCEL);
    } else {
        set_done(d, DL_OK);
    }
}

/* ================= блоки ================= */

static int download_block(downloader_t *d, int index)
{
    long long total;
    long long bsize;
    long long start;
    long long end;
    long long want;
    long long got = 0;
    http_resp_t r;
    FILE *f;
    char *buf;
    int ok = 0;

    dl_mutex_lock(&d->mu);
    total = d->total;
    bsize = d->block_size;
    dl_mutex_unlock(&d->mu);

    start = (long long)index * bsize;
    end = start + bsize - 1;
    if (end >= total) {
        end = total - 1;
    }
    if (start >= total) {
        return 1;
    }
    want = end - start + 1;

    if (http_open(d->url, "GET", 1, start, end, &r) != 0) {
        return 0;
    }
    if (r.status != 206) {
        http_close(&r);
        return 0;
    }
    f = fopen(d->path, "r+b");
    if (f == NULL) {
        http_close(&r);
        return 0;
    }
    if (dl_fseek64(f, start, SEEK_SET) != 0) {
        fclose(f);
        http_close(&r);
        return 0;
    }
    buf = (char *)malloc(DL_BUF);
    if (buf == NULL) {
        fclose(f);
        http_close(&r);
        return 0;
    }
    while (got < want) {
        long long left = want - got;
        int want_now = (left > DL_BUF) ? DL_BUF : (int)left;
        int n;
        if (d->stop) {
            ok = 1; /* выходим без перепостановки */
            break;
        }
        n = recv(r.sock, buf, want_now, 0);
        if (n <= 0) {
            break;
        }
        fwrite(buf, 1, (size_t)n, f);
        got += n;
        dl_mutex_lock(&d->mu);
        d->downloaded += n;
        dl_mutex_unlock(&d->mu);
    }
    if (got >= want) {
        ok = 1;
    }
    free(buf);
    fclose(f);
    http_close(&r);
    return ok;
}

#ifdef _WIN32
static DWORD WINAPI dl_worker(void *arg)
#else
static void *dl_worker(void *arg)
#endif
{
    downloader_t *d = (downloader_t *)arg;
    for (;;) {
        int idx = -1;
        int ok;
        if (d->stop) {
            break;
        }
        dl_mutex_lock(&d->mu);
        if (d->qh < d->qt) {
            idx = d->queue[d->qh++];
        }
        dl_mutex_unlock(&d->mu);
        if (idx < 0) {
            break; /* работы нет */
        }
        ok = download_block(d, idx);
        if (d->stop) {
            break;
        }
        if (!ok) {
            int fatal = 0;
            dl_mutex_lock(&d->mu);
            d->retries[idx]++;
            if (d->retries[idx] > 5) {
                d->state = DL_FAIL;
                strncpy(d->error, "block failed after 5 retries",
                        sizeof(d->error) - 1);
                d->error[sizeof(d->error) - 1] = 0;
                d->stop = 1;
                fatal = 1;
            } else if (d->qt < d->qcap) {
                d->queue[d->qt++] = idx;
            } else {
                d->state = DL_FAIL;
                strncpy(d->error, "retry queue overflow",
                        sizeof(d->error) - 1);
                d->error[sizeof(d->error) - 1] = 0;
                d->stop = 1;
                fatal = 1;
            }
            dl_mutex_unlock(&d->mu);
            if (fatal) {
                break;
            }
            dl_sleep_ms(500);
        }
    }
    dl_mutex_lock(&d->mu);
    d->alive--;
    dl_mutex_unlock(&d->mu);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void spawn_worker(downloader_t *d)
{
    dl_thread_t t;
    if (dl_thread_create(&t, dl_worker, d) == 0) {
        dl_mutex_lock(&d->mu);
        d->alive++;
        dl_mutex_unlock(&d->mu);
        /* fire-and-forget: завершение отслеживаем счётчиком alive.
         * Хэндл/поток открепляем сразу, чтобы ничего не текло. */
#ifdef _WIN32
        CloseHandle(t);
#else
        pthread_detach(t);
#endif
    }
}

/* ================= multi ================= */

static void download_multi(downloader_t *d, long long total)
{
    long long block_size = total / 512;
    int block_count;
    int initial;
    int i;
    double prev_t;
    long long prev_dl = 0;
    FILE *pre;

    if (block_size < DL_MIN_BLOCK) {
        block_size = DL_MIN_BLOCK;
    }
    block_count = (int)((total + block_size - 1) / block_size);

    d->queue = (int *)malloc(sizeof(int) * (size_t)(block_count * 2 + 16));
    d->retries = (int *)calloc((size_t)block_count, sizeof(int));
    if (d->queue == NULL || d->retries == NULL) {
        set_fail(d, "out of memory");
        return;
    }
    d->qcap = block_count * 2 + 16;
    dl_mutex_lock(&d->mu);
    d->block_size = block_size;
    d->block_count = block_count;
    d->qh = 0;
    d->qt = 0;
    for (i = 0; i < block_count; i++) {
        d->queue[d->qt++] = i;
    }
    d->downloaded = 0;
    d->speed = 0;
    d->best = 0;
    d->stag = 0;
    d->frozen = 0;
    d->alive = 0;
    dl_mutex_unlock(&d->mu);

    pre = fopen(d->path, "w+b");
    if (pre == NULL) {
        set_fail(d, "cannot create file");
        return;
    }
    if (dl_prealloc(pre, total) != 0) {
        fclose(pre);
        set_fail(d, "cannot preallocate file");
        return;
    }
    fclose(pre);

    if (!d->adaptive) {
        initial = (d->max_threads < block_count) ? d->max_threads : block_count;
    } else {
        initial = (block_count >= 2 && d->max_threads >= 2) ? 2 : 1;
    }
    for (i = 0; i < initial; i++) {
        spawn_worker(d);
    }

    prev_t = dl_now();
    for (;;) {
        long long cur;
        int alive;
        int waiting;
        double now;
        double elapsed;
        double speed;

        dl_sleep_ms(DL_MEASURE_MS);
        if (d->stop) {
            break;
        }
        dl_mutex_lock(&d->mu);
        cur = d->downloaded;
        alive = d->alive;
        waiting = d->qt - d->qh;
        dl_mutex_unlock(&d->mu);

        now = dl_now();
        elapsed = now - prev_t;
        if (elapsed <= 0) {
            elapsed = 1.0;
        }
        speed = (double)(cur - prev_dl) / elapsed;
        if (speed < 0) {
            speed = 0;
        }
        dl_mutex_lock(&d->mu);
        d->speed = speed;
        if (speed > d->best * DL_GROW) {
            d->best = speed;
            d->stag = 0;
        } else {
            if (d->best > 0) {
                d->stag++;
            }
            if (d->stag >= 2) {
                d->frozen = 1;
            }
        }
        dl_mutex_unlock(&d->mu);

        /* добавить поток, если скорость держится/растёт */
        if (d->adaptive) {
            int frozen;
            int stag;
            double best;
            dl_mutex_lock(&d->mu);
            frozen = d->frozen;
            stag = d->stag;
            best = d->best;
            alive = d->alive;
            waiting = d->qt - d->qh;
            dl_mutex_unlock(&d->mu);
            if (!frozen && waiting > 0 && alive < d->max_threads) {
                if (best == 0 || speed >= best * 0.98 || stag == 0) {
                    spawn_worker(d);
                }
            }
        }

        prev_dl = cur;
        prev_t = now;

        if (cur >= total) {
            break;
        }
        dl_mutex_lock(&d->mu);
        alive = d->alive;
        waiting = d->qt - d->qh;
        dl_mutex_unlock(&d->mu);
        if (waiting == 0 && alive == 0) {
            break;
        }
    }

    /* ждём воркеров (по счётчику, без хэндлов) */
    for (i = 0; i < 600; i++) {
        int alive;
        if (d->stop && d->state == DL_RUNNING) {
            break;
        }
        dl_mutex_lock(&d->mu);
        alive = d->alive;
        dl_mutex_unlock(&d->mu);
        if (alive == 0) {
            break;
        }
        dl_sleep_ms(100);
    }

    if (d->stop) {
        dl_mutex_lock(&d->mu);
        if (d->state == DL_RUNNING) {
            d->state = DL_CANCEL;
        }
        dl_mutex_unlock(&d->mu);
        return;
    }
    {
        long long cur;
        dl_mutex_lock(&d->mu);
        cur = d->downloaded;
        dl_mutex_unlock(&d->mu);
        if (cur >= total) {
            set_done(d, DL_OK);
            return;
        }
    }
    if (file_len(d->path) >= total) {
        set_done(d, DL_OK);
    } else {
        set_fail(d, "incomplete: server gave fewer bytes");
    }
}

/* ================= контроллер ================= */

#ifdef _WIN32
static DWORD WINAPI dl_controller(void *arg)
#else
static void *dl_controller(void *arg)
#endif
{
    downloader_t *d = (downloader_t *)arg;
    long long total = -1;
    int ranges = 0;

    probe_url(d, &total, &ranges);
    dl_mutex_lock(&d->mu);
    d->total = total;
    dl_mutex_unlock(&d->mu);

    if (d->stop) {
        set_done(d, DL_CANCEL);
    } else if (strncmp(d->url, "http://", 7) != 0) {
        set_fail(d, "only http:// supported (C port has no TLS)");
    } else if (total <= 0 || !ranges || total < 1024 * 1024 ||
               d->max_threads == 1) {
        download_single(d);
    } else {
        download_multi(d, total);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================= API ================= */

downloader_t *dl_create(const char *url, const char *path,
                        int max_threads, int adaptive)
{
    downloader_t *d;
    d = (downloader_t *)calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    strncpy(d->url, url, sizeof(d->url) - 1);
    strncpy(d->path, path, sizeof(d->path) - 1);
    if (max_threads < 1) {
        max_threads = 1;
    }
    if (max_threads > DL_MAX_THREADS) {
        max_threads = DL_MAX_THREADS;
    }
    d->max_threads = max_threads;
    d->adaptive = adaptive ? 1 : 0;
    dl_mutex_init(&d->mu);
    d->state = DL_RUNNING;
    d->total = -1;
    if (dl_sock_init() != 0) {
        dl_mutex_destroy(&d->mu);
        free(d);
        return NULL;
    }
    d->sock_inited = 1;
    return d;
}

int dl_start(downloader_t *d)
{
    if (d->started) {
        return 0;
    }
    d->started = 1;
    return dl_thread_create(&d->controller, dl_controller, d);
}

void dl_stop(downloader_t *d)
{
    d->stop = 1;
}

void dl_wait(downloader_t *d)
{
    if (d->started) {
        dl_thread_join(d->controller);
        d->started = 0;
    }
}

void dl_get_status(downloader_t *d, dl_status_t *out)
{
    dl_mutex_lock(&d->mu);
    out->total = d->total;
    out->downloaded = d->downloaded;
    out->speed = d->speed;
    out->frozen = d->frozen;
    out->state = d->state;
    out->waiting_blocks = d->qt - d->qh;
    out->live_threads = d->alive;
    strncpy(out->error, d->error, sizeof(out->error) - 1);
    out->error[sizeof(out->error) - 1] = 0;
    dl_mutex_unlock(&d->mu);
}

void dl_free(downloader_t *d)
{
    if (d == NULL) {
        return;
    }
    dl_wait(d);
    if (d->sock_inited) {
        dl_sock_cleanup();
    }
    dl_mutex_destroy(&d->mu);
    if (d->queue != NULL) {
        free(d->queue);
    }
    if (d->retries != NULL) {
        free(d->retries);
    }
    free(d);
}
