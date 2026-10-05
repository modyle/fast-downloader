/* dl_port.h - изоляция различий Win32 / POSIX.
 * Весь остальной код пользуется только этими типами и функциями
 * и не знает, под чем компилируется.
 */
#ifndef DL_PORT_H
#define DL_PORT_H

#include <stdio.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET dl_sock_t;
#  define DL_SOCK_INVALID INVALID_SOCKET
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <strings.h>
   typedef int dl_sock_t;
#  define DL_SOCK_INVALID (-1)
#endif

#ifdef _WIN32
typedef HANDLE dl_thread_t;
typedef CRITICAL_SECTION dl_mutex_t;
#else
typedef pthread_t dl_thread_t;
typedef pthread_mutex_t dl_mutex_t;
#endif

/* --- потоки --- */
#ifdef _WIN32
typedef DWORD (WINAPI *dl_thread_fn_t)(void *arg);
#else
typedef void *(*dl_thread_fn_t)(void *arg);
#endif

int dl_thread_create(dl_thread_t *out, dl_thread_fn_t fn, void *arg);
void dl_thread_join(dl_thread_t t);

/* --- мьютексы --- */
int dl_mutex_init(dl_mutex_t *m);
void dl_mutex_lock(dl_mutex_t *m);
void dl_mutex_unlock(dl_mutex_t *m);
void dl_mutex_destroy(dl_mutex_t *m);

/* --- сон --- */
void dl_sleep_ms(int ms);

/* --- монотонные часы, секунды (для замеров скорости) --- */
double dl_now(void);

/* --- сокеты --- */
int dl_sock_init(void);      /* WSAStartup / nop, 0 = ok */
void dl_sock_cleanup(void);
void dl_sock_close(dl_sock_t s);

/* --- файлы с 64-битными смещениями (сырые fd, без stdio) --- */
typedef int dl_fd_t;
#define DL_FD_INVALID (-1)

dl_fd_t dl_fd_open_rw(const char *path); /* создать/обрезать, -1 = fail */
dl_fd_t dl_fd_open_rw_existing(const char *path); /* открыть сущ., -1 = fail */
int dl_fd_seek64(dl_fd_t fd, long long off);
int dl_fd_write_all(dl_fd_t fd, const char *buf, int len); /* 0/-1 */
int dl_fd_close(dl_fd_t fd);
int dl_fd_prealloc(dl_fd_t fd, long long size); /* 0 = ok */
long long dl_file_size(const char *path); /* -1 = нет файла */

#endif /* DL_PORT_H */
