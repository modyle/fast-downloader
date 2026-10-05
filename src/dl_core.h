/* dl_core.h - многопоточный загрузчик с адаптивным числом потоков.
 * HTTP через сырые сокеты (только http://, без TLS - см. README).
 */
#ifndef DL_CORE_H
#define DL_CORE_H

#define DL_MAX_THREADS 32
#define DL_ERR_SIZE 256

#define DL_RUNNING 0
#define DL_OK 1
#define DL_FAIL 2
#define DL_CANCEL 3

typedef struct {
    long long total;      /* -1 = неизвестен */
    long long downloaded;
    double speed;         /* байт/сек */
    int live_threads;
    int waiting_blocks;
    int frozen;           /* 1 = перестали добавлять потоки */
    int state;            /* DL_RUNNING / DL_OK / DL_FAIL / DL_CANCEL */
    char error[DL_ERR_SIZE];
} dl_status_t;

typedef struct dl downloader_t;

downloader_t *dl_create(const char *url, const char *path,
                        int max_threads, int adaptive);
int dl_start(downloader_t *d);   /* 0 = запущен */
void dl_stop(downloader_t *d);
void dl_wait(downloader_t *d);
void dl_get_status(downloader_t *d, dl_status_t *out);
void dl_free(downloader_t *d);

#endif /* DL_CORE_H */
