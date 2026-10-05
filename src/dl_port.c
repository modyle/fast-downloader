/* dl_port.c - реализация OS-абстракции. */
#include "dl_port.h"

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#  include <sys/time.h>
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
    return pthread_create(out, NULL, fn, arg);
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

/* ---------------- 64-битные файлы ---------------- */

int dl_fseek64(FILE *f, long long off, int whence)
{
#ifdef _WIN32
    return _fseeki64(f, (__int64)off, whence);
#else
    return fseeko(f, (off_t)off, whence);
#endif
}

int dl_prealloc(FILE *f, long long size)
{
#ifdef _WIN32
    if (dl_fseek64(f, size - 1, SEEK_SET) != 0) {
        return -1;
    }
    if (fputc(0, f) == EOF) {
        return -1;
    }
    fflush(f);
    return 0;
#else
    if (ftruncate(fileno(f), (off_t)size) != 0) {
        return -1;
    }
    return 0;
#endif
}
