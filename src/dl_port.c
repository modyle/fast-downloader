/* dl_port.c - реализация OS-абстракции. */
#include "dl_port.h"

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#else
#  include <unistd.h>
#  include <sys/time.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#endif

/* ---------------- потоки ---------------- */

int dl_thread_create(dl_thread_t *out, dl_thread_fn_t fn, void *arg)
{
#ifdef _WIN32
    HANDLE h;
    h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (h == NULL) {
        return -1;
    }
    *out = h;
    return 0;
#else
    /* Нашим потокам хватает 1 МБ стека с запасом (глубоких фреймов
     * нет, большие буферы - в куче). По умолчанию pthread даёт 8 МБ
     * виртуалки на поток - на машине с 256 МБ это расточительно. */
    pthread_attr_t at;
    int rc;
    if (pthread_attr_init(&at) != 0) {
        return -1;
    }
    pthread_attr_setstacksize(&at, (size_t)(1024 * 1024));
    rc = pthread_create(out, &at, fn, arg);
    pthread_attr_destroy(&at);
    return rc;
#endif
}

void dl_thread_join(dl_thread_t t)
{
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

/* ---------------- мьютексы ---------------- */

int dl_mutex_init(dl_mutex_t *m)
{
#ifdef _WIN32
    InitializeCriticalSection(m);
    return 0;
#else
    return pthread_mutex_init(m, NULL);
#endif
}

void dl_mutex_lock(dl_mutex_t *m)
{
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}

void dl_mutex_unlock(dl_mutex_t *m)
{
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

void dl_mutex_destroy(dl_mutex_t *m)
{
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}

/* ---------------- сон ---------------- */

void dl_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

/* ---------------- часы ---------------- */

double dl_now(void)
{
#ifdef _WIN32
    return (double)GetTickCount() / 1000.0;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}

/* ---------------- сокеты ---------------- */

int dl_sock_init(void)
{
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        return -1;
    }
#endif
    return 0;
}

void dl_sock_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

void dl_sock_close(dl_sock_t s)
{
    if (s == DL_SOCK_INVALID) {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

/* ---------------- сырые файлы ---------------- */

dl_fd_t dl_fd_open_rw(const char *path)
{
#ifdef _WIN32
    int fd;
    fd = _open(path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY,
               _S_IREAD | _S_IWRITE);
    return fd;
#else
    return open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
#endif
}

dl_fd_t dl_fd_open_rw_existing(const char *path)
{
#ifdef _WIN32
    return _open(path, _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path, O_RDWR);
#endif
}

int dl_fd_seek64(dl_fd_t fd, long long off)
{
#ifdef _WIN32
    return (_lseeki64(fd, (__int64)off, SEEK_SET) < 0) ? -1 : 0;
#else
    return (lseek(fd, (off_t)off, SEEK_SET) < 0) ? -1 : 0;
#endif
}

int dl_fd_write_all(dl_fd_t fd, const char *buf, int len)
{
    int done = 0;
    while (done < len) {
#ifdef _WIN32
        int r = _write(fd, buf + done, (unsigned int)(len - done));
#else
        ssize_t r = write(fd, buf + done, (size_t)(len - done));
#endif
        if (r <= 0) {
            return -1;
        }
        done += r;
    }
    return 0;
}

int dl_fd_close(dl_fd_t fd)
{
    if (fd < 0) {
        return 0;
    }
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

int dl_fd_prealloc(dl_fd_t fd, long long size)
{
#ifdef _WIN32
    return _chsize_s(fd, (__int64)size);
#else
    return ftruncate(fd, (off_t)size);
#endif
}

long long dl_file_size(const char *path)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) {
        return -1;
    }
    return (long long)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long long)st.st_size;
#endif
}
