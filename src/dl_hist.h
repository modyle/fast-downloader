/* dl_hist.h - история загрузок.
 * Формат history.csv совместим с C#-версией:
 * ticks \t b64(url) \t b64(file) \t size \t b64(status)
 * ticks = .NET DateTime.Ticks (100нс от 0001-01-01).
 */
#ifndef DL_HIST_H
#define DL_HIST_H

#define HIST_MAX 500

typedef struct {
    long long ticks;
    char url[2048];
    char file[1024];
    long long size;
    char status[256];
} hist_entry_t;

typedef struct {
    hist_entry_t items[HIST_MAX];
    int count;
} hist_t;

void hist_load(hist_t *h, const char *csv_path);
void hist_save(hist_t *h, const char *csv_path);
void hist_add(hist_t *h, const char *url, const char *file,
              long long size, const char *status);
void hist_clear(hist_t *h);

/* тики <-> читаемая дата */
long long hist_now_ticks(void);
void hist_ticks_to_str(long long ticks, char *out, int outcap);

/* форматирование размеров */
void fmt_size(long long bytes, char *out, int outcap);
void fmt_speed(double bps, char *out, int outcap);

/* имя файла из URL */
void file_name_from_url(const char *url, char *out, int outcap);

#endif /* DL_HIST_H */
