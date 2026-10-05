/* main_linux.c - консольный интерфейс для Linux.
 * Сборка: make (gcc -pthread). Русский текст в UTF-8.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dl_core.h"
#include "dl_hist.h"
#include "dl_port.h"

static void read_line(const char *prompt, char *out, int outcap,
                      const char *def)
{
    printf("%s", prompt);
    if (def != NULL && def[0] != 0) {
        printf(" [%s]", def);
    }
    printf(": ");
    fflush(stdout);
    if (fgets(out, outcap, stdin) == NULL) {
        out[0] = 0;
    }
    {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
            out[--n] = 0;
        }
    }
    if (out[0] == 0 && def != NULL) {
        strncpy(out, def, (size_t)(outcap - 1));
        out[outcap - 1] = 0;
    }
}

static void show_history(hist_t *h)
{
    int i;
    printf("\n--- История загрузок (%d) ---\n", h->count);
    for (i = 0; i < h->count; i++) {
        char dt[32];
        char sz[32];
        hist_entry_t *e = &h->items[i];
        hist_ticks_to_str(e->ticks, dt, (int)sizeof(dt));
        fmt_size(e->size, sz, (int)sizeof(sz));
        printf("%s | %s | %s | %s\n  %s\n", dt, e->file, sz, e->status,
               e->url);
    }
    printf("----------------------------\n");
}

static const char *state_str(int st)
{
    if (st == DL_OK) {
        return "OK";
    }
    if (st == DL_FAIL) {
        return "FAIL";
    }
    if (st == DL_CANCEL) {
        return "CANCEL";
    }
    return "RUN";
}

int main(int argc, char **argv)
{
    hist_t hist;
    char url[2048] = "";
    char path[1024] = "";
    char tmp[256] = "";
    char guess[1024];
    char sdone[32];
    char stotal[32];
    char sspd[32];
    int max_threads = 32;
    int adaptive = 1;
    downloader_t *d;
    dl_status_t st;

    (void)argc;
    (void)argv;

    printf("fast-downloader (C, Linux). Только http:// (без TLS).\n");
    hist_load(&hist, "history.csv");
    if (hist.count > 0) {
        show_history(&hist);
    }

    read_line("URL файла", url, (int)sizeof(url), NULL);
    if (strncmp(url, "http://", 7) != 0) {
        printf("Нужен URL вида http://... (https в C-версии не поддерживается).\n");
        return 1;
    }
    file_name_from_url(url, guess, (int)sizeof(guess));
    read_line("Сохранить как", path, (int)sizeof(path), guess);
    read_line("Макс. потоков (1-32)", tmp, (int)sizeof(tmp), "32");
    max_threads = atoi(tmp);
    if (max_threads < 1) {
        max_threads = 1;
    }
    if (max_threads > 32) {
        max_threads = 32;
    }
    read_line("Авторежим (y/n)", tmp, (int)sizeof(tmp), "y");
    adaptive = (tmp[0] == 'n' || tmp[0] == 'N') ? 0 : 1;

    d = dl_create(url, path, max_threads, adaptive);
    if (d == NULL) {
        printf("Не создался загрузчик.\n");
        return 1;
    }
    if (dl_start(d) != 0) {
        printf("Не запустился.\n");
        dl_free(d);
        return 1;
    }

    for (;;) {
        dl_get_status(d, &st);
        if (st.state != DL_RUNNING) {
            break;
        }
        fmt_size(st.downloaded, sdone, (int)sizeof(sdone));
        if (st.total > 0) {
            double pct = (double)st.downloaded * 100.0 / (double)st.total;
            fmt_size(st.total, stotal, (int)sizeof(stotal));
            fmt_speed(st.speed, sspd, (int)sizeof(sspd));
            printf("\r%5.1f%%  %s / %s  %s  потоки %d/%d%s   ",
                   pct, sdone, stotal, sspd,
                   st.live_threads, max_threads,
                   st.frozen ? " [рост остановлен]" : "");
        } else {
            fmt_speed(st.speed, sspd, (int)sizeof(sspd));
            printf("\r%s  %s  потоки %d/%d   ",
                   sdone, sspd, st.live_threads, max_threads);
        }
        fflush(stdout);
        dl_sleep_ms(300);
    }
    printf("\nГотово: %s", state_str(st.state));
    if (st.state == DL_FAIL) {
        printf(" (%s)", st.error);
    }
    printf("\n");

    if (st.state == DL_OK) {
        hist_add(&hist, url, path, st.total, "OK");
    } else if (st.state == DL_FAIL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Error: %s", st.error);
        hist_add(&hist, url, path, 0, msg);
    } else {
        hist_add(&hist, url, path, st.downloaded, "Cancel");
    }
    hist_save(&hist, "history.csv");
    show_history(&hist);

    dl_free(d);
    return (st.state == DL_OK) ? 0 : 1;
}
