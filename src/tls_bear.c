/* tls_bear.c - HTTPS на встроенном BearSSL.
 * Якоря: Mozilla-бандл (fallback для протухших систем, напр. WinXP)
 * плюс системное хранилище ОС (Windows ROOT store / Linux CA bundle).
 */
#include "tls_bear.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl_ssl.h"
#include "bearssl_x509.h"
#include "bearssl_rand.h"
#include "bearssl_hash.h"

#ifdef _WIN32
#  include <wincrypt.h>
#  include <ntsecapi.h>
#endif

/* ---------------- base64 (только для PEM) ---------------- */

static int b64v(char c)
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

/* Декодирует base64 с пропусками пробелов. Возвращает длину или -1. */
static int b64_decode(const char *in, unsigned char *out, int outcap)
{
    int vals[4];
    int nv = 0;
    int o = 0;
    int pad = 0;
    const char *p = in;
    for (;;) {
        char c = *p++;
        int v;
        if (c == 0) {
            break;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '=') {
            v = 0;
            pad++;
        } else {
            v = b64v(c);
            if (v < 0) {
                return -1;
            }
        }
        vals[nv++] = v;
        if (nv == 4) {
            unsigned int t;
            if (o + 3 > outcap) {
                return -1;
            }
            t = ((unsigned int)vals[0] << 18) |
                ((unsigned int)vals[1] << 12) |
                ((unsigned int)vals[2] << 6) |
                (unsigned int)vals[3];
            out[o++] = (unsigned char)((t >> 16) & 255);
            if (pad < 2) {
                out[o++] = (unsigned char)((t >> 8) & 255);
            }
            if (pad < 1) {
                out[o++] = (unsigned char)(t & 255);
            }
            nv = 0;
            pad = 0;
        }
    }
    if (nv != 0) {
        return -1;
    }
    return o;
}

/* ---------------- якоря ---------------- */

typedef struct {
    br_x509_trust_anchor *tas;
    size_t num;
    size_t cap;
} ta_list_t;

static ta_list_t g_tas = { NULL, 0, 0 };

typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
} blob_t;

static void blob_append(blob_t *b, const void *data, size_t len)
{
    if (b->len + len > b->cap) {
        size_t nc = (b->cap == 0) ? 256 : b->cap * 2;
        unsigned char *nb;
        while (nc < b->len + len) {
            nc *= 2;
        }
        nb = (unsigned char *)realloc(b->buf, nc);
        if (nb == NULL) {
            return;
        }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

static void dn_append_cb(void *ctx, const void *buf, size_t len)
{
    blob_append((blob_t *)ctx, buf, len);
}

static unsigned char *memdup(const void *p, size_t n)
{
    unsigned char *o;
    if (n == 0) {
        n = 1;
    }
    o = (unsigned char *)malloc(n);
    if (o != NULL && p != NULL) {
        memcpy(o, p, n);
    }
    return o;
}

/* DER-сертификат -> trust anchor. 0 = ok. */
static int der_to_anchor(const unsigned char *der, size_t derlen)
{
    br_x509_decoder_context dc;
    br_x509_pkey *pk;
    br_x509_trust_anchor ta;
    blob_t dn = { NULL, 0, 0 };

    memset(&ta, 0, sizeof(ta));
    br_x509_decoder_init(&dc, dn_append_cb, &dn);
    br_x509_decoder_push(&dc, der, derlen);
    pk = br_x509_decoder_get_pkey(&dc);
    if (pk == NULL || dn.buf == NULL) {
        if (dn.buf != NULL) {
            free(dn.buf);
        }
        return -1;
    }
    ta.dn.data = dn.buf;
    ta.dn.len = dn.len;
    ta.flags = 0;
    if (br_x509_decoder_isCA(&dc)) {
        ta.flags |= BR_X509_TA_CA;
    }
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta.pkey.key_type = BR_KEYTYPE_RSA;
        ta.pkey.key.rsa.n = memdup(pk->key.rsa.n, pk->key.rsa.nlen);
        ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta.pkey.key.rsa.e = memdup(pk->key.rsa.e, pk->key.rsa.elen);
        ta.pkey.key.rsa.elen = pk->key.rsa.elen;
        if (ta.pkey.key.rsa.n == NULL || ta.pkey.key.rsa.e == NULL) {
            free(ta.dn.data);
            free(ta.pkey.key.rsa.n);
            free(ta.pkey.key.rsa.e);
            return -1;
        }
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta.pkey.key_type = BR_KEYTYPE_EC;
        ta.pkey.key.ec.curve = pk->key.ec.curve;
        ta.pkey.key.ec.q = memdup(pk->key.ec.q, pk->key.ec.qlen);
        ta.pkey.key.ec.qlen = pk->key.ec.qlen;
        if (ta.pkey.key.ec.q == NULL) {
            free(ta.dn.data);
            return -1;
        }
    } else {
        free(ta.dn.data);
        return -1;
    }
    if (g_tas.num >= g_tas.cap) {
        size_t nc = (g_tas.cap == 0) ? 64 : g_tas.cap * 2;
        br_x509_trust_anchor *nt;
        nt = (br_x509_trust_anchor *)realloc(g_tas.tas,
                                             nc * sizeof(*nt));
        if (nt == NULL) {
            free(ta.dn.data);
            if (ta.pkey.key_type == BR_KEYTYPE_RSA) {
                free(ta.pkey.key.rsa.n);
                free(ta.pkey.key.rsa.e);
            } else {
                free(ta.pkey.key.ec.q);
            }
            return -1;
        }
        g_tas.tas = nt;
        g_tas.cap = nc;
    }
    g_tas.tas[g_tas.num++] = ta;
    return 0;
}

/* Загрузить все CERTIFICATE-блоки из PEM-текста. Возвращает число. */
static int load_pem_anchors(const char *pem, size_t pemlen)
{
    int count = 0;
    const char *p = pem;
    const char *end = pem + pemlen;
    const char *begin_mark = "-----BEGIN CERTIFICATE-----";
    const char *end_mark = "-----END CERTIFICATE-----";
    while (p < end) {
        const char *b;
        const char *e;
        const char *body;
        size_t bodylen;
        unsigned char *der;
        int derlen;
        b = strstr(p, begin_mark);
        if (b == NULL || b >= end) {
            break;
        }
        body = b + strlen(begin_mark);
        e = strstr(body, end_mark);
        if (e == NULL || e > end) {
            break;
        }
        bodylen = (size_t)(e - body);
        /* base64-декодеру нужна NUL-строка - копируем тело блока */
        {
            char *b64;
            b64 = (char *)malloc(bodylen + 1);
            if (b64 == NULL) {
                break;
            }
            memcpy(b64, body, bodylen);
            b64[bodylen] = 0;
            der = (unsigned char *)malloc(bodylen + 4);
            if (der != NULL) {
                derlen = b64_decode(b64, der, (int)(bodylen + 4));
                if (derlen > 0) {
                    if (der_to_anchor(der, (size_t)derlen) == 0) {
                        count++;
                    }
                }
                free(der);
            }
            free(b64);
        }
        p = e + strlen(end_mark);
    }
    return count;
}

static int load_pem_file(const char *path)
{
    FILE *f;
    long n;
    char *buf;
    size_t got;
    int count = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    count = load_pem_anchors(buf, got);
    free(buf);
    return count;
}

#ifdef _WIN32
static int load_system_store(void)
{
    HCERTSTORE h;
    PCCERT_CONTEXT prev = NULL;
    PCCERT_CONTEXT cur;
    int count = 0;
    h = CertOpenSystemStoreA(0, "ROOT");
    if (h == NULL) {
        return 0;
    }
    for (;;) {
        cur = CertEnumCertificatesInStore(h, prev);
        if (cur == NULL) {
            break;
        }
        /* CertEnum сам освобождает prev, последний освободим после цикла */
        if (der_to_anchor(cur->pbCertEncoded,
                          (size_t)cur->cbCertEncoded) == 0) {
            count++;
        }
        prev = cur;
    }
    if (prev != NULL) {
        CertFreeCertificateContext(prev);
    }
    CertCloseStore(h, 0);
    return count;
}
#else
static int load_system_store(void)
{
    static const char *paths[] = {
        "/etc/ssl/certs/ca-certificates.crt", /* Debian/Ubuntu */
        "/etc/pki/tls/certs/ca-bundle.crt",   /* RHEL/Fedora */
        "/etc/ssl/cert.pem",                   /* Alpine/BSD */
        "/etc/ssl/certs/ca-bundle.crt",        /* SUSE */
        NULL
    };
    int i;
    for (i = 0; paths[i] != NULL; i++) {
        int n = load_pem_file(paths[i]);
        if (n > 0) {
            return n;
        }
    }
    return 0;
}
#endif

int tls_global_init(const char *bundle_path, char *err, int errcap)
{
    static int done = 0;
    int from_bundle = 0;
    int from_system = 0;
    if (done) {
        return 0; /* якоря уже загружены, повторно не надо */
    }
    if (bundle_path == NULL) {
        bundle_path = tls_find_bundle();
    }
    if (bundle_path != NULL) {
        from_bundle = load_pem_file(bundle_path);
    }
    from_system = load_system_store();
    if (g_tas.num == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, (size_t)errcap,
                     "no trust anchors (bundle=%d system=%d)",
                     from_bundle, from_system);
            err[errcap - 1] = 0;
        }
        return -1;
    }
    done = 1;
    return 0;
}

/* ---------------- поиск бандла ---------------- */

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static char g_bundle_path[4096];

const char *tls_find_bundle(void)
{
    if (file_exists("./cacert.pem")) {
        return "./cacert.pem";
    }
    if (file_exists("./third_party/cacert.pem")) {
        return "./third_party/cacert.pem";
    }
#ifdef _WIN32
    {
        char dir[4096];
        char *bs;
        DWORD m = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
        if (m > 0 && m < (DWORD)sizeof(dir)) {
            bs = strrchr(dir, '\\');
            if (bs == NULL) {
                bs = strrchr(dir, '/');
            }
            if (bs != NULL) {
                *bs = 0;
                snprintf(g_bundle_path, sizeof(g_bundle_path),
                         "%s\\cacert.pem", dir);
                g_bundle_path[sizeof(g_bundle_path) - 1] = 0;
                if (file_exists(g_bundle_path)) {
                    return g_bundle_path;
                }
            }
        }
    }
#else
    {
        /* рядом с бинарником через /proc/self/exe */
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            char *bs;
            exe[n] = 0;
            bs = strrchr(exe, '/');
            if (bs != NULL) {
                *bs = 0;
                snprintf(g_bundle_path, sizeof(g_bundle_path),
                         "%s/cacert.pem", exe);
                g_bundle_path[sizeof(g_bundle_path) - 1] = 0;
                if (file_exists(g_bundle_path)) {
                    return g_bundle_path;
                }
                snprintf(g_bundle_path, sizeof(g_bundle_path),
                         "%s/third_party/cacert.pem", exe);
                g_bundle_path[sizeof(g_bundle_path) - 1] = 0;
                if (file_exists(g_bundle_path)) {
                    return g_bundle_path;
                }
            }
        }
    }
#endif
    return NULL;
}

/* ---------------- entropy ---------------- */

static int os_random(unsigned char *out, size_t len)
{
#ifdef _WIN32
    /* RtlGenRandom (advapi32), есть ещё на XP */
    if (!SystemFunction036(out, (ULONG)len)) {
        return -1;
    }
    return 0;
#else
    FILE *f;
    size_t got = 0;
    f = fopen("/dev/urandom", "rb");
    if (f == NULL) {
        return -1;
    }
    while (got < len) {
        size_t r = fread(out + got, 1, len - got, f);
        if (r == 0) {
            fclose(f);
            return -1;
        }
        got += r;
    }
    fclose(f);
    return 0;
#endif
}

/* ---------------- соединение ---------------- */

struct tls_conn {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char *iobuf;
    dl_sock_t sock;
};

static int low_read(void *ctx, unsigned char *data, size_t len)
{
    dl_sock_t s = *(dl_sock_t *)ctx;
    int want = (len > 16384) ? 16384 : (int)len;
    int r = recv(s, (char *)data, want, 0);
    if (r <= 0) {
        return -1;
    }
    return r;
}

static int low_write(void *ctx, const unsigned char *data, size_t len)
{
    dl_sock_t s = *(dl_sock_t *)ctx;
    size_t sent = 0;
    while (sent < len) {
        size_t left = len - sent;
        int chunk = (left > 16384) ? 16384 : (int)left;
        int r = send(s, (const char *)data + sent, chunk, 0);
        if (r <= 0) {
            return -1;
        }
        sent += (size_t)r;
    }
    return (int)len;
}

static void tls_err_text(int code, char *out, int outcap)
{
    const char *what = NULL;
    if (code == 54) {
        what = "certificate expired";
    } else if (code == 56) {
        what = "server name mismatch (SNI/SAN)";
    } else if (code == 62) {
        what = "issuer not trusted (no anchor)";
    } else if (code >= 32 && code < 64) {
        what = "certificate rejected by validator";
    } else if (code == BR_ERR_BAD_VERSION) {
        what = "server requires newer TLS than 1.2";
    }
    if (what != NULL && code != BR_ERR_BAD_VERSION) {
        snprintf(out, (size_t)outcap, "%s (x509 code %d)", what, code);
    } else if (what != NULL) {
        snprintf(out, (size_t)outcap, "%s (code %d)", what, code);
    } else {
        snprintf(out, (size_t)outcap, "tls handshake failed (code %d)",
                 code);
    }
    out[outcap - 1] = 0;
}

tls_conn_t *tls_client_open(dl_sock_t sock, const char *host,
                            char *err, int errcap)
{
    tls_conn_t *c;
    unsigned char seed[32];

    if (g_tas.num == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, (size_t)errcap, "tls not initialised");
            err[errcap - 1] = 0;
        }
        return NULL;
    }
    c = (tls_conn_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->iobuf = (unsigned char *)malloc(BR_SSL_BUFSIZE_BIDI);
    if (c->iobuf == NULL) {
        free(c);
        return NULL;
    }
    c->sock = sock;
    br_ssl_client_init_full(&c->sc, &c->xc, g_tas.tas, g_tas.num);
    /* Порядок важен: сначала буфер (reset внутри дёргает
     * set_buffer(NULL), который лишь сохраняет уже заданный),
     * потом reset. reset возвращает НЕНОЛЬ при успехе. */
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    if (br_ssl_client_reset(&c->sc, host, 0) == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, (size_t)errcap, "tls reset failed (code %d)",
                     br_ssl_engine_last_error(&c->sc.eng));
            err[errcap - 1] = 0;
        }
        free(c->iobuf);
        free(c);
        return NULL;
    }
    if (os_random(seed, sizeof(seed)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, (size_t)errcap, "no system entropy");
            err[errcap - 1] = 0;
        }
        free(c->iobuf);
        free(c);
        return NULL;
    }
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
    br_sslio_init(&c->ioc, &c->sc.eng, low_read, &c->sock,
                  low_write, &c->sock);
    /* Хендшейк ленивый: реально пойдёт при первой записи.
     * Проверяем раннюю ошибку движка (напр. кривой SNI): */
    if (br_ssl_engine_current_state(&c->sc.eng) == BR_SSL_CLOSED) {
        int code = br_ssl_engine_last_error(&c->sc.eng);
        if (err != NULL && errcap > 0) {
            tls_err_text(code, err, errcap);
        }
        free(c->iobuf);
        free(c);
        return NULL;
    }
    return c;
}

int tls_read(tls_conn_t *c, char *buf, int len)
{
    int r;
    if (len <= 0) {
        return 0;
    }
    r = br_sslio_read(&c->ioc, buf, (size_t)len);
    return r;
}

int tls_write_all(tls_conn_t *c, const char *buf, int len)
{
    if (br_sslio_write_all(&c->ioc, buf, (size_t)len) != 0) {
        return -1;
    }
    /* Записи могут остаться в буфере движка - проталкиваем в сеть.
     * (В примере BearSSL после записей всегда br_sslio_flush.) */
    if (br_sslio_flush(&c->ioc) != 0) {
        return -1;
    }
    return 0;
}

int tls_error_code(tls_conn_t *c)
{
    return br_ssl_engine_last_error(&c->sc.eng);
}

void tls_close(tls_conn_t *c)
{
    if (c == NULL) {
        return;
    }
    br_sslio_close(&c->ioc);
    dl_sock_close(c->sock);
    if (c->iobuf != NULL) {
        free(c->iobuf);
    }
    free(c);
}
