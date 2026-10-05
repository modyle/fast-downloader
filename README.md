# fast-downloader — ветка `c-port` (чистый C)

Та же идея, что в `main`, но **без .NET**: ядро на portable C (C99),
HTTP через сырые сокеты, потоки — Win32 API / pthreads через `#ifdef _WIN32`.

- **Windows**: оконный интерфейс на **чистом Win32 API**
  (`CreateWindowEx`, кнопки, прогресс-бар, ListView для истории).
- **Linux**: консольный интерфейс (диалог в терминале + прогресс-строка).
- Общий код (`dl_core.c`, `dl_hist.c`, `dl_port.c`) один на обе ОС,
  различия — только в `dl_port.*` и в `main_win32.c` / `main_linux.c`.

## HTTPS: встроенный BearSSL

TLS-стек **завендорен** (`third_party/bearssl`, MIT) — никаких внешних
зависимостей, работает даже на **Windows XP** (где системный SChannel
умеет максимум TLS 1.0 и современные сайты его отшивают):

- Клиент TLS 1.0–1.2 (сервер выберет 1.2 везде, где он есть), SNI,
  проверка цепочки **и** имени хоста.
- Якоря доверия: **бандл Mozilla** (`third_party/cacert.pem`, fallback
  для протухших систем) **плюс системное хранилище ОС**
  (Windows ROOT store через CryptoAPI / Linux CA-bundle) — второе важно
  для корпоративных MITM-прокси.
- Entropy: `RtlGenRandom` (advapi32, есть на XP) / `/dev/urandom`.
- `cacert.pem` ищется рядом с exe, в текущей папке и в `third_party/`;
  в релизные архивы кладётся рядом с бинарником.

Ограничения честно: нет TLS 1.3 (BearSSL его не умеет; таких
строго-1.3-only сайтов в дикой природе почти нет), нет клиентских
сертификатов.

## Возможности

- Скачивание в **до 32 потоков** через `Range`-запросы.
- **Адаптив**: старт с 2 потоков, замер скорости каждые 1.5 сек;
  растёт (>5%) — плюс поток; два замера без роста — добавление стоп.
- Без `Accept-Ranges` / с неизвестным размером — один поток.
- История в `history.csv` (формат совместим с C#-версией из `main`:
  тики .NET + base64), редиректы 301/302/303/307/308 (до 5 штук).

## Требования

### Запуск

- Windows XP+ (exe без зависимостей, нужен только интернет) или
  любой Linux x86_64 (один бинарник).
- Нужен именно **`http://`-URL**.

### Сборка локально

- **Linux**: `gcc` + `make` (обычно уже стоят).
- **Windows**: MinGW-w64 (`gcc`, `mingw32-make`) **или** MSVC (`cl`).

## Сборка

```bash
# Linux
make
./downloader
```

```bat
:: Windows, вариант 1 - MinGW (из cmd с MinGW в PATH)
mingw32-make
downloader.exe

:: Windows, вариант 2 - MSVC (из Developer Command Prompt)
build_msvc.bat
```

## Сборка в облаке GitHub

Workflow `.github/workflows/build.yml` (триггер — пуш в `c-port`):
`ubuntu-latest` собирает `downloader`, `windows-latest` (MinGW через MSYS2)
собирает `downloader.exe`. Готовые бинарники — в артефактах запуска
**Actions → build → downloader-linux / downloader-windows**.

Linux-сборка заодно прогоняет **end-to-end smoke-тесты**: скачивание 6 МБ
по HTTP (многопоток, сверка побайтово) и по HTTPS против локального сервера
с самоподписанным CA (настоящий TLS-хендшейк BearSSL + проверка цепочки,
сверка побайтово). Референсом рядом качает curl.

```bash
git checkout c-port
# ... правим код ...
git add -A && git commit -m "..." && git push origin c-port
```

## Использование

**Windows** — как обычное окно: URL → «Сохранить как» (`...`) →
потоки → галочка авторежима → «Скачать». Внизу прогресс, скорость,
потоки; история таблицей.

**Linux** — интерактив в терминале:

```text
URL файла: http://example.com/big.iso
Сохранить как [big.iso]:
Макс. потоков (1-32) [32]:
Авторежим (y/n) [y]:
 45.2%  58.1/128.0 MB  3.1 MB/s  потоки 5/32
```

## Диета памяти (замерено в CI: 20 МБ, 8 потоков)

- RSS пик: **~2.2 МБ** (exe+libc, якоря только для https, буферы 16 КБ).
- Виртуалка: **~8 МБ** (стеки потоков по 1 МБ, одна malloc-арена
  через `mallopt(M_ARENA_MAX, 1)`).
- Приёмы: ленивая загрузка TLS-якорей (для чистого http вообще
  без разбора CA — это было 96% всех инструкций CPU), буферы 16 КБ,
  история ужата до 100 записей.
- На машине с 256 МБ ОЗУ это меньше 1% памяти.

## Диета CPU (замерено в CI: 20 МБ, 8 потоков, callgrind)

- Инструкций на всё скачивание: **~1.5M** (было 95M до ленивых якорей,
  3.9M до CPU-диеты). CPU загрузки: ~2%.
- Приёмы: сырые `write` вместо `fwrite` (минус лишний `memcpy` через
  stdio), буферизированный reader строк (вместо syscall на байт),
  буфер и fd свои у каждого воркера (без malloc/open на блок),
  счётчик прогресса — одним сложением за блок.
- Оставшийся топ плоский: парсинг заголовков, `recv`/`write`,
  printf прогресса — жира больше нет, дальше только ядро ОС.

## Структура

```text
Makefile                  # Linux: downloader / Windows: downloader.exe
build_msvc.bat            # сборка через cl (best-effort, в CI не гоняется)
src/dl_core.h/.c          # ядро: HTTP(S), probe, очередь блоков, адаптив
src/dl_port.h/.c          # #ifdef _WIN32: потоки, мьютексы, сокеты, файлы
src/dl_hist.h/.c          # history.csv + base64 + формат размеров
src/tls_bear.h/.c         # HTTPS: BearSSL, якоря (бандл + система)
src/main_win32.c          # GUI Win32 API
src/main_linux.c          # консоль Linux
third_party/bearssl/      # вендоренный BearSSL (MIT)
third_party/cacert.pem    # корневые CA Mozilla (fallback)
```

## Лицензия

MIT — см. файл [LICENSE](LICENSE).
