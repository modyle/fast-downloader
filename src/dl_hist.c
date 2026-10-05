/* dl_hist.c - история + base64 + тики + форматирование. */
#include "dl_hist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- base64 ---------------- */

static const char B64E[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, int len,
                       char *out, int outcap)
{
    int o = 0;
    int i;
    for (i = 0; i < len; i += 3) {
        unsigned int a = in[i];
        unsigned int b = (i + 1 < len) ? in[i + 1] : 0;
        unsigned int c = (i + 2 < len) ? in[i + 2] : 0;
        unsigned int v = (a << 16) | (b << 8) | c;
        if (o + 4 >= outcap) {
            break;
        }
        out[o++] = B64E[(v >> 18) & 63];
        out[o++] = B64E[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64E[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64E[v & 63] : '=';
    }
    out[o] = 0;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* 0 = ok */
static int b64_decode(const char *in, unsigned char *out, int outcap,
                      int *outlen)
{
    int o = 0;
    int n = (int)strlen(in);
    int i;
    for (i = 0; i + 3 < n + 1; i += 4) {
        int a = b64_val(in[i]);
        int b = b64_val(in[i + 1]);
        int c = (in[i + 2] == '=') ? 0 : b64_val(in[i + 2]);
        int d = (in[i + 3] == '=') ? 0 : b64_val(in[i + 3]);
        unsigned int v;
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return -1;
        }
        v = ((unsigned int)a << 18) | ((unsigned int)b << 12) |
            ((unsigned int)c << 6) | (unsigned int)d;
        if (o + 3 >= outcap) {
            return -1;
        }
        out[o++] = (unsigned char)((v >> 16) & 255);
        if (in[i + 2] != '=') {
            out[o++] = (unsigned char)((v >> 8) & 255);
        }
        if (in[i + 3] != '=') {
            out[o++] = (unsigned char)(v & 255);
        }
    }
    out[o] = 0;
    if (outlen != NULL) {
        *outlen = o;
    }
    return 0;
}

static void b64_encode_str(const char *s, char *out, int outcap)
{
    if (s == NULL) {
        s = "";
    }
    b64_encode((const unsigned char *)s, (int)strlen(s), out, outcap);
}

static void b64_decode_str(const char *s, char *out, int outcap)
{
    unsigned char *tmp;
    int len = (int)strlen(s);
    if (len == 0) {
        out[0] = 0;
        return;
    }
    tmp = (unsigned char *)malloc((size_t)(len + 8));
    if (tmp == NULL) {
        strncpy(out, s, (size_t)(outcap - 1));
        out[outcap - 1] = 0;
        return;
    }
    if (b64_decode(s, tmp, len + 8, NULL) != 0) {
        strncpy(out, s, (size_t)(outcap - 1));
        out[outcap - 1] = 0;
    } else {
        strncpy(out, (const char *)tmp, (size_t)(outcap - 1));
        out[outcap - 1] = 0;
    }
    free(tmp);
}

/* ---------------- тики ---------------- */

#define TICKS_AT_UNIX_EPOCH 621355968000000000LL

long long hist_now_ticks(void)
{
    return (long long)time(NULL) * 10000000LL + TICKS_AT_UNIX_EPOCH;
}

void hist_ticks_to_str(long long ticks, char *out, int outcap)
{
    time_t t;
    struct tm *lt;
    t = (time_t)((ticks - TICKS_AT_UNIX_EPOCH) / 10000000LL);
    if (t < 0) {
        t = 0;
    }
    lt = localtime(&t);
    if (lt == NULL) {
        strncpy(out, "?", (size_t)(outcap - 1));
        out[outcap - 1] = 0;
        return;
    }
    strftime(out, (size_t)outcap, "%Y-%m-%d %H:%M:%S", lt);
}

/* ---------------- история ---------------- */

void hist_clear(hist_t *h)
{
    h->count = 0;
}

void hist_add(hist_t *h, const char *url, const char *file,
              long long size, const char *status)
{
    int i;
    hist_entry_t *e;
    if (h->count >= HIST_MAX) {
        h->count = HIST_MAX - 1;
    }
    for (i = h->count; i > 0; i--) {
        h->items[i] = h->items[i - 1];
    }
    e = &h->items[0];
    e->ticks = hist_now_ticks();
    strncpy(e->url, (url != NULL) ? url : "", sizeof(e->url) - 1);
    e->url[sizeof(e->url) - 1] = 0;
    strncpy(e->file, (file != NULL) ? file : "", sizeof(e->file) - 1);
    e->file[sizeof(e->file) - 1] = 0;
    e->size = size;
    strncpy(e->status, (status != NULL) ? status : "", sizeof(e->status) - 1);
    e->status[sizeof(e->status) - 1] = 0;
    if (h->count < HIST_MAX) {
        h->count++;
    }
}

void hist_save(hist_t *h, const char *csv_path)
{
    FILE *f;
    int i;
    f = fopen(csv_path, "w");
    if (f == NULL) {
        return;
    }
    for (i = 0; i < h->count; i++) {
        char ub[2800];
        char fb[1400];
        char sb[360];
        hist_entry_t *e = &h->items[i];
        b64_encode_str(e->url, ub, (int)sizeof(ub));
        b64_encode_str(e->file, fb, (int)sizeof(fb));
        b64_encode_str(e->status, sb, (int)sizeof(sb));
        fprintf(f, "%lld\t%s\t%s\t%lld\t%s\n",
                e->ticks, ub, fb, e->size, sb);
    }
    fclose(f);
}

void hist_load(hist_t *h, const char *csv_path)
{
    FILE *f;
    char line[8192];
    h->count = 0;
    f = fopen(csv_path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char *p[5];
        int n = 0;
        char *s = line;
        hist_entry_t *e;
        /* режем по \t (последнее поле - до \n) */
        p[0] = s;
        while (*s != 0 && n < 4) {
            if (*s == '\t') {
                *s = 0;
                n++;
                p[n] = s + 1;
            }
            s++;
        }
        if (n != 4) {
            continue;
        }
        {
            char *nl = strchr(p[4], '\n');
            if (nl != NULL) {
                *nl = 0;
            }
            nl = strchr(p[4], '\r');
            if (nl != NULL) {
                *nl = 0;
            }
        }
        if (h->count >= HIST_MAX) {
            break;
        }
        e = &h->items[h->count];
        e->ticks = atoll(p[0]);
        b64_decode_str(p[1], e->url, (int)sizeof(e->url));
        b64_decode_str(p[2], e->file, (int)sizeof(e->file));
        e->size = atoll(p[3]);
        b64_decode_str(p[4], e->status, (int)sizeof(e->status));
        h->count++;
    }
    fclose(f);
}

/* ---------------- формат ---------------- */

void fmt_size(long long bytes, char *out, int outcap)
{
    if (bytes < 0) {
        strncpy(out, "?", (size_t)(outcap - 1));
    } else if (bytes < 1024) {
        snprintf(out, (size_t)outcap, "%lld B", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, (size_t)outcap, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        snprintf(out, (size_t)outcap, "%.2f MB",
                 (double)bytes / 1048576.0);
    } else {
        snprintf(out, (size_t)outcap, "%.2f GB",
                 (double)bytes / 1073741824.0);
    }
    out[outcap - 1] = 0;
}

void fmt_speed(double bps, char *out, int outcap)
{
    char tmp[64];
    if (bps < 0) {
        bps = 0;
    }
    fmt_size((long long)bps, tmp, (int)sizeof(tmp));
    snprintf(out, (size_t)outcap, "%s/s", tmp);
    out[outcap - 1] = 0;
}

void file_name_from_url(const char *url, char *out, int outcap)
{
    const char *p;
    const char *slash;
    const char *q;
    size_t n;
    size_t i;
    if (url == NULL || url[0] == 0) {
        strncpy(out, "download.bin", (size_t)(outcap - 1));
        out[outcap - 1] = 0;
        return;
    }
    p = strstr(url, "://");
    p = (p != NULL) ? p + 3 : url;
    slash = strrchr(p, '/');
    p = (slash != NULL) ? slash + 1 : p;
    q = strchr(p, '?');
    n = (q != NULL) ? (size_t)(q - p) : strlen(p);
    if (n == 0 || n >= (size_t)outcap) {
        n = (n >= (size_t)outcap) ? (size_t)(outcap - 1) : n;
        if (n == 0) {
            strncpy(out, "download.bin", (size_t)(outcap - 1));
            out[outcap - 1] = 0;
            return;
        }
    }
    memcpy(out, p, n);
    out[n] = 0;
    /* чистим запрещённые для имён символы */
    for (i = 0; i < n; i++) {
        if (out[i] == '<' || out[i] == '>' || out[i] == ':' ||
            out[i] == '"' || out[i] == '|' || out[i] == '*' ||
            out[i] == '?' || (unsigned char)out[i] < 32) {
            out[i] = '_';
        }
    }
}
