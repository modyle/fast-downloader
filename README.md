# MultiThreadDownloader

Простой многопоточный загрузчик файлов под **.NET Framework 2.0**, написанный строго на
конструкциях **C# 2.0** (без `var`, LINQ, `async/await`, лямбд — только `Thread`,
`HttpWebRequest`, `lock`). Запускается даже на старых Windows с установленным .NET 2.0.

## Что умеет

- Качание файла в **несколько потоков (максимум 32)** через HTTP `Range`-запросы.
- **Адаптивный режим** (включён по умолчанию):
  стартует с 2 потоков, раз в 1.5 сек замеряет суммарную скорость.
  Пока скорость растёт (прирост > 5%) — добавляет по одному потоку.
  Если скорость **перестала расти 2 замера подряд** — больше потоки не добавляет
  и докачивает на текущем числе («перестаёт прибавлять потоки»).
- Если сервер не поддерживает `Range` или размер неизвестен — тихо качает в 1 поток.
- Простой WinForms UI: поле URL, выбор файла, прогресс текущей загрузки
  (проценты, скорость, число потоков, очередь блоков), **история загрузок**
  (дата, файл, размер, статус, URL), хранение в `history.csv` рядом с exe.
- Ручной режим: снимите галочку «Авторежим» — будет сразу N потоков.

## Как запустить (вариант «скачать на другом компе»)

1. Залейте этот проект на GitHub (см. ниже).
2. Откройте вкладку **Actions → build → последний успешный запуск**,
   скачайте артефакт **MultiThreadDownloader-net20**.
3. Распакуйте на любом Windows с .NET Framework 2.0+ и запустите
   `MultiThreadDownloader.exe`. Установка не нужна.

## Как залить на GitHub

```powershell
cd MultiThreadDownloader
git init
git add .
git commit -m "Initial commit: multithread downloader net20"
git branch -M main
git remote add origin https://github.com/<ваш-логин>/MultiThreadDownloader.git
git push -u origin main
```

После пуша сборка стартует сама (`.github/workflows/build.yml`, Windows-обработчик
с .NET 8 SDK, таргет `net20` через пакет `Microsoft.NETFramework.ReferenceAssemblies`).
Готовый exe забирайте из артефактов, как описано выше.

## Локальная сборка

Нужен .NET SDK 8+ (Windows):

```powershell
dotnet build MultiThreadDownloader.sln -c Release
```

Результат: `src/MultiThreadDownloader/bin/Release/net20/MultiThreadDownloader.exe`.

## Структура

- `src/MultiThreadDownloader/AdaptiveDownloader.cs` — ядро: очередь блоков,
  воркеры на `Thread`, контроллер адаптивного числа потоков.
- `src/MultiThreadDownloader/History.cs` — история (`history.csv`) + форматирование.
- `src/MultiThreadDownloader/MainForm.cs` — окно (прогресс + история), весь UI в коде.
- `src/MultiThreadDownloader/Program.cs` — точка входа.
- `src/MultiThreadDownloader/MultiThreadDownloader.csproj` — SDK-проект,
  `<TargetFramework>net20</TargetFramework>`, `<LangVersion>ISO-2</LangVersion>`.

## Ограничения честно

- «.NET Framework 2.5» не существует — проект таргетит **.NET 2.0**
  (результат тот же: работает на старом рантайме, язык ограничен C# 2.0).
- Старый `HttpWebRequest` на очень старых ОС может не договориться по TLS 1.2
  с некоторыми https-сайтами (в коде пробуем включить TLS 1.1/1.2 числовым кастом,
  но на Windows XP без обновлений часть сайтов не откроется — это ограничение
  платформы, а не баг).
- Многопоточность ускоряет только если сервер отдаёт `Accept-Ranges: bytes`
  и не режет скорость на соединение. Иначе — честный однопоток.

Лицензия: MIT.
