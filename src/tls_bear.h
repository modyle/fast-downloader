/* tls_bear.h - HTTPS через встроенный BearSSL.
 * Работает везде, где есть сокеты: WinXP+ (без зависимости от SChannel),
 * Linux. Проверяет цепочку + имя хоста (SNI).
 */
#ifndef TLS_BEAR_H
#define TLS_BEAR_H

#include "dl_port.h"

/* Одноразовая загрузка якорей доверия:
 *   1) PEM-бандл (Mozilla, может быть NULL),
 *   2) системное хранилище ОС (Windows ROOT / Linux CA bundle).
 * 0 = ok, -1 = нет ни одного якоря (err заполнен). */
int tls_global_init(const char *bundle_path, char *err, int errcap);

/* Поиск cacert.pem (рядом с exe, в cwd, в third_party/).
 * Возвращает путь или NULL. */
const char *tls_find_bundle(void);

typedef struct tls_conn tls_conn_t;

/* TLS-хендшейк поверх уже соединённого сокета, SNI = host.
 * NULL = fail (err заполнен). Сокет при неудаче остаётся у вызывающего. */
tls_conn_t *tls_client_open(dl_sock_t sock, const char *host,
                            char *err, int errcap);

/* Чтение: 1+ байт или -1. Запись всего буфера: 0 / -1. */
int tls_read(tls_conn_t *c, char *buf, int len);
int tls_write_all(tls_conn_t *c, const char *buf, int len);

/* Код последней ошибки движка (для диагностики). */
int tls_error_code(tls_conn_t *c);

/* Закрыть TLS и нижележащий сокет, освободить всё. */
void tls_close(tls_conn_t *c);

#endif /* TLS_BEAR_H */
