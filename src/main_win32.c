/* main_win32.c - интерфейс на чистом Win32 API (без MFC/WTL).
 * Только ANSI-вызовы (*A). Русские строки лежат в исходнике как UTF-8
 * и переводятся в ACP через ru() - так не важно, в какой кодировке
 * компилятор прочитал файл.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "dl_core.h"
#include "dl_hist.h"
#include "dl_port.h"
#include "tls_bear.h"

#define IDC_URL        101
#define IDC_PATH       102
#define IDC_BROWSE     103
#define IDC_THREADS    104
#define IDC_ADAPTIVE   105
#define IDC_START      106
#define IDC_STOP       107
#define IDC_PROG       108
#define IDC_STATUS     109
#define IDC_SPEED      110
#define IDC_TINFO      111
#define IDC_HIST       112
#define IDC_CLEAR      113
#define IDC_OPENFOLDER 114
#define IDT_POLL       1

static HINSTANCE g_hInst;
static HWND hUrl, hPath, hThreads, hAdaptive, hStart, hStop;
static HWND hProg, hStatus, hSpeed, hTInfo, hHist;
static HFONT g_font;

static downloader_t *g_dl = NULL;
static hist_t g_hist;
static char g_csv[MAX_PATH] = "";
static char g_url[2048] = "";
static char g_path[1024] = "";
static int g_max = 32;

/* UTF-8 -> ACP (на русской Windows это CP1251). */
static char RU_BUF[8][2048];
static int ru_i = 0;
static const char *ru(const char *s)
{
    WCHAR w[2048];
    char *o;
    int n;
    if (s == NULL) {
        return "";
    }
    o = RU_BUF[ru_i];
    ru_i = (ru_i + 1) % 8;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 2048);
    if (n <= 0) {
        strncpy(o, s, 2047);
        o[2047] = 0;
        return o;
    }
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, o, 2048, NULL, NULL) <= 0) {
        strncpy(o, s, 2047);
        o[2047] = 0;
    }
    return o;
}

static HWND mk(HWND parent, const char *cls, const char *text, DWORD style,
               int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, (text != NULL) ? ru(text) : NULL,
                             WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             g_hInst, NULL);
    if (c != NULL && g_font != NULL) {
        SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    return c;
}

static const char *base_name(const char *p)
{
    const char *b1 = strrchr(p, '\\');
    const char *b2 = strrchr(p, '/');
    const char *b = (b1 > b2) ? b1 : b2;
    return (b != NULL) ? b + 1 : p;
}

static void refresh_history(HWND hwnd)
{
    int i;
    (void)hwnd;
    ListView_DeleteAllItems(hHist);
    for (i = 0; i < g_hist.count; i++) {
        LVITEMA it;
        char dt[32];
        char sz[32];
        hist_entry_t *e = &g_hist.items[i];
        int row;
        memset(&it, 0, sizeof(it));
        hist_ticks_to_str(e->ticks, dt, (int)sizeof(dt));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = (LPSTR)dt;
        row = ListView_InsertItem(hHist, &it);
        if (row < 0) {
            continue;
        }
        ListView_SetItemText(hHist, row, 1, (LPSTR)ru(base_name(e->file)));
        fmt_size(e->size, sz, (int)sizeof(sz));
        ListView_SetItemText(hHist, row, 2, (LPSTR)sz);
        ListView_SetItemText(hHist, row, 3, (LPSTR)ru(e->status));
        ListView_SetItemText(hHist, row, 4, (LPSTR)e->url);
    }
}

static void update_ui(HWND hwnd)
{
    dl_status_t st;
    char tmp[2048];
    char sdone[32];
    char stotal[32];
    char sspd[32];

    if (g_dl == NULL) {
        return;
    }
    dl_get_status(g_dl, &st);
    fmt_size(st.downloaded, sdone, (int)sizeof(sdone));
    fmt_speed(st.speed, sspd, (int)sizeof(sspd));
    if (st.total > 0) {
        double pct = (double)st.downloaded * 100.0 / (double)st.total;
        int pos = (int)((double)st.downloaded * 1000.0 / (double)st.total);
        if (pos < 0) {
            pos = 0;
        }
        if (pos > 1000) {
            pos = 1000;
        }
        SendMessageA(hProg, PBM_SETPOS, (WPARAM)pos, 0);
        fmt_size(st.total, stotal, (int)sizeof(stotal));
        sprintf(tmp, "Скачано %s из %s (%.1f%%)", sdone, stotal, pct);
        SetWindowTextA(hStatus, ru(tmp));
    } else {
        sprintf(tmp, "Скачано %s (размер определяется...)", sdone);
        SetWindowTextA(hStatus, ru(tmp));
    }
    sprintf(tmp, "Скорость: %s", sspd);
    SetWindowTextA(hSpeed, ru(tmp));
    sprintf(tmp, "Потоки: %d / макс %d", st.live_threads, g_max);
    if (st.waiting_blocks > 0) {
        char more[64];
        sprintf(more, " | блоков в очереди: %d", st.waiting_blocks);
        strncat(tmp, more, sizeof(tmp) - strlen(tmp) - 1);
    }
    if (st.frozen) {
        strncat(tmp, " | рост остановлен (скорость не растёт)",
                sizeof(tmp) - strlen(tmp) - 1);
    }
    SetWindowTextA(hTInfo, ru(tmp));

    if (st.state == DL_RUNNING) {
        return;
    }
    KillTimer(hwnd, IDT_POLL);
    EnableWindow(hStart, TRUE);
    EnableWindow(hStop, FALSE);
    if (st.state == DL_OK) {
        SendMessageA(hProg, PBM_SETPOS, (WPARAM)1000, 0);
        sprintf(tmp, "Готово: %s", g_path);
        SetWindowTextA(hStatus, ru(tmp));
        hist_add(&g_hist, g_url, g_path, st.total, "OK");
    } else if (st.state == DL_FAIL) {
        sprintf(tmp, "Ошибка: %s", st.error);
        SetWindowTextA(hStatus, ru(tmp));
        {
            char msg[600];
            char stt[300];
            sprintf(stt, "Ошибка: %s", st.error);
            sprintf(msg, "Не удалось скачать файл.\n%s", st.error);
            hist_add(&g_hist, g_url, g_path, 0, stt);
            MessageBoxA(hwnd, ru(msg), ru("Ошибка загрузки"),
                        MB_OK | MB_ICONERROR);
        }
    } else {
        SetWindowTextA(hStatus, ru("Остановлено пользователем."));
        hist_add(&g_hist, g_url, g_path, st.downloaded, "Отмена");
    }
    hist_save(&g_hist, g_csv);
    refresh_history(hwnd);
    dl_free(g_dl);
    g_dl = NULL;
}

static void on_browse(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = "";
    char guess[1024];
    char url[2048];
    GetWindowTextA(hUrl, url, (int)sizeof(url));
    file_name_from_url(url, guess, (int)sizeof(guess));
    strncpy(file, guess, sizeof(file) - 1);
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)sizeof(file);
    ofn.lpstrFilter = "All files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = ru("Куда сохранить файл");
    if (GetSaveFileNameA(&ofn)) {
        SetWindowTextA(hPath, file);
    }
}

static void on_start(HWND hwnd)
{
    char tbuf[64];
    char file[MAX_PATH];
    int adaptive;

    if (g_dl != NULL) {
        return;
    }
    GetWindowTextA(hUrl, g_url, (int)sizeof(g_url));
    if (strncmp(g_url, "http://", 7) != 0 &&
        strncmp(g_url, "https://", 8) != 0) {
        MessageBoxA(hwnd,
                    ru("Введите корректный URL (http:// или https://)."),
                    ru("Проверка URL"), MB_OK | MB_ICONWARNING);
        return;
    }
    GetWindowTextA(hPath, g_path, (int)sizeof(g_path));
    if (g_path[0] == 0) {
        OPENFILENAMEA ofn;
        char guess[1024];
        file_name_from_url(g_url, guess, (int)sizeof(guess));
        strncpy(file, guess, sizeof(file) - 1);
        file[sizeof(file) - 1] = 0;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = file;
        ofn.nMaxFile = (DWORD)sizeof(file);
        ofn.lpstrFilter = "All files\0*.*\0";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrTitle = ru("Куда сохранить файл");
        if (!GetSaveFileNameA(&ofn)) {
            return;
        }
        strncpy(g_path, file, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = 0;
        SetWindowTextA(hPath, g_path);
    }
    GetWindowTextA(hThreads, tbuf, (int)sizeof(tbuf));
    g_max = atoi(tbuf);
    if (g_max < 1) {
        g_max = 1;
    }
    if (g_max > 32) {
        g_max = 32;
    }
    adaptive = (IsDlgButtonChecked(hwnd, IDC_ADAPTIVE) == BST_CHECKED);

    /* TLS-якоря - лениво и только для https (разбор ~300 CA
     * при старте - это 96% всего CPU и ~400 КБ памяти ни за что). */
    if (strncmp(g_url, "https://", 8) == 0) {
        char terr[256];
        if (tls_global_init(NULL, terr, (int)sizeof(terr)) != 0) {
            char msg[512];
            sprintf(msg, "TLS не инициализировался: %s", terr);
            MessageBoxA(hwnd, ru(msg),
                        ru("TLS"), MB_OK | MB_ICONERROR);
            return;
        }
    }

    g_dl = dl_create(g_url, g_path, g_max, adaptive);
    if (g_dl == NULL || dl_start(g_dl) != 0) {
        if (g_dl != NULL) {
            dl_free(g_dl);
            g_dl = NULL;
        }
        MessageBoxA(hwnd, ru("Не запустился загрузчик."),
                    ru("Ошибка"), MB_OK | MB_ICONERROR);
        return;
    }
    SendMessageA(hProg, PBM_SETPOS, (WPARAM)0, 0);
    SetWindowTextA(hStatus, ru("Подключение..."));
    EnableWindow(hStart, FALSE);
    EnableWindow(hStop, TRUE);
    SetTimer(hwnd, IDT_POLL, 500, NULL);
}

static void on_open_folder(HWND hwnd)
{
    char dir[MAX_PATH];
    char *b;
    GetWindowTextA(hPath, dir, (int)sizeof(dir));
    b = (char *)base_name(dir);
    if (b != dir && b[-1] != 0) {
        /* отрезаем имя файла */
        char tmp[MAX_PATH];
        size_t n = (size_t)(b - dir);
        if (n >= sizeof(tmp)) {
            n = sizeof(tmp) - 1;
        }
        memcpy(tmp, dir, n);
        tmp[n] = 0;
        strncpy(dir, tmp, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
    } else {
        /* пути нет - папка с exe */
        GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
        b = (char *)base_name(dir);
        if (b != dir) {
            b[-1] = 0;
        }
    }
    {
        DWORD attr = GetFileAttributesA(dir);
        if (attr == (DWORD)0xFFFFFFFF || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
            b = (char *)base_name(dir);
            if (b != dir) {
                b[-1] = 0;
            }
        }
    }
    if ((INT_PTR)ShellExecuteA(hwnd, "open", ru(dir),
                               NULL, NULL, SW_SHOWNORMAL) <= 32) {
        MessageBoxA(hwnd, ru("Не удалось открыть папку."),
                    ru("Папка"), MB_OK | MB_ICONWARNING);
    }
}

static void build_ui(HWND hwnd)
{
    LVCOLUMNA col;
    const char *titles[5];
    int widths[5];
    int i;

    g_font = GetStockObject(DEFAULT_GUI_FONT);

    mk(hwnd, "STATIC", "URL файла:", SS_LEFT, 12, 12, 80, 16, 0);
    hUrl = mk(hwnd, "EDIT", NULL, WS_BORDER | ES_AUTOHSCROLL,
              12, 30, 560, 22, IDC_URL);
    mk(hwnd, "STATIC", "Сохранить как:", SS_LEFT, 12, 58, 120, 16, 0);
    hPath = mk(hwnd, "EDIT", NULL, WS_BORDER | ES_AUTOHSCROLL,
               12, 76, 480, 22, IDC_PATH);
    mk(hwnd, "BUTTON", "...", BS_PUSHBUTTON, 498, 74, 74, 25, IDC_BROWSE);

    mk(hwnd, "STATIC", "Макс. потоков (1-32):", SS_LEFT,
       12, 106, 150, 16, 0);
    hThreads = mk(hwnd, "EDIT", "32", WS_BORDER | ES_NUMBER,
                  162, 103, 50, 22, IDC_THREADS);
    hAdaptive = mk(hwnd, "BUTTON",
                   "Авторежим: добавлять потоки, пока растёт скорость",
                   BS_AUTOCHECKBOX, 224, 104, 348, 20, IDC_ADAPTIVE);
    CheckDlgButton(hwnd, IDC_ADAPTIVE, BST_CHECKED);

    hStart = mk(hwnd, "BUTTON", "Скачать", BS_PUSHBUTTON,
                12, 132, 120, 28, IDC_START);
    hStop = mk(hwnd, "BUTTON", "Стоп", BS_PUSHBUTTON,
               140, 132, 120, 28, IDC_STOP);
    EnableWindow(hStop, FALSE);

    hProg = mk(hwnd, PROGRESS_CLASSA, NULL, 0, 12, 170, 560, 22, IDC_PROG);
    SendMessageA(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    hStatus = mk(hwnd, "STATIC", "Готов.", SS_LEFT, 12, 198, 560, 16,
                 IDC_STATUS);
    hSpeed = mk(hwnd, "STATIC", "Скорость: —", SS_LEFT, 12, 216, 280, 16,
                IDC_SPEED);
    hTInfo = mk(hwnd, "STATIC", "Потоки: —", SS_LEFT, 300, 216, 272, 16,
                IDC_TINFO);

    mk(hwnd, "STATIC", "История загрузок:", SS_LEFT, 12, 242, 200, 16, 0);
    hHist = mk(hwnd, WC_LISTVIEWA, NULL,
               WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
               12, 260, 560, 180, IDC_HIST);
    ListView_SetExtendedListViewStyle(hHist,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    titles[0] = "Дата";
    titles[1] = "Файл";
    titles[2] = "Размер";
    titles[3] = "Статус";
    titles[4] = "URL";
    widths[0] = 130;
    widths[1] = 140;
    widths[2] = 80;
    widths[3] = 110;
    widths[4] = 100;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < 5; i++) {
        col.pszText = (LPSTR)ru(titles[i]);
        col.cx = widths[i];
        ListView_InsertColumn(hHist, i, &col);
    }

    mk(hwnd, "BUTTON", "Очистить историю", BS_PUSHBUTTON,
       12, 448, 150, 26, IDC_CLEAR);
    mk(hwnd, "BUTTON", "Открыть папку загрузок", BS_PUSHBUTTON,
       170, 448, 180, 26, IDC_OPENFOLDER);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        build_ui(hwnd);
        GetModuleFileNameA(NULL, g_csv, (DWORD)sizeof(g_csv));
        {
            char *b = (char *)base_name(g_csv);
            if (b != g_csv) {
                b[-1] = 0;
            }
            strncat(g_csv, "\\history.csv",
                    sizeof(g_csv) - strlen(g_csv) - 1);
        }
        hist_load(&g_hist, g_csv);
        refresh_history(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:
            on_browse(hwnd);
            return 0;
        case IDC_START:
            on_start(hwnd);
            return 0;
        case IDC_STOP:
            if (g_dl != NULL) {
                dl_stop(g_dl);
                SetWindowTextA(hStatus, ru("Остановка..."));
                EnableWindow(hStop, FALSE);
            }
            return 0;
        case IDC_CLEAR:
            hist_clear(&g_hist);
            hist_save(&g_hist, g_csv);
            refresh_history(hwnd);
            return 0;
        case IDC_OPENFOLDER:
            on_open_folder(hwnd);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_POLL) {
            update_ui(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (g_dl != NULL) {
            dl_stop(g_dl);
        }
        hist_save(&g_hist, g_csv);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG m;
    INITCOMMONCONTROLSEX ic;

    (void)hPrev;
    (void)cmd;
    g_hInst = hInst;

    ic.dwSize = sizeof(ic);
    ic.dwICC = ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&ic);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "FastDownloaderC";
    if (!RegisterClassA(&wc)) {
        return 1;
    }
    hwnd = CreateWindowExA(0, "FastDownloaderC",
                           ru("Многопоточный загрузчик (C, Win32, до 32 потоков)"),
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 600, 520,
                           NULL, NULL, hInst, NULL);
    if (hwnd == NULL) {
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}
