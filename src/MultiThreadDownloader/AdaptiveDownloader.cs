using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Threading;

namespace MultiThreadDownloader
{
    /// <summary>
    /// Многопоточный загрузчик с адаптивным числом потоков (максимум 32).
    /// Логика: стартуем с малого числа потоков, раз в интервал меряем
    /// суммарную скорость. Пока скорость растёт - добавляем потоки.
    /// Если скорость перестала расти (2 замера подряд без прироста) -
    /// перестаём прибавлять потоки и докачиваем на текущем числе.
    /// Только конструкции C# 2.0 / API .NET 2.0.
    /// </summary>
    public class AdaptiveDownloader
    {
        private const int MaxAllowedThreads = 32;
        private const int MinBlockSize = 256 * 1024; // 256 КБ
        private const int BufferSize = 64 * 1024;    // 64 КБ
        private const int MeasureIntervalMs = 1500;
        private const double GrowThreshold = 1.05; // +5% считаем ростом

        private string _url;
        private string _destPath;
        private int _maxThreads;
        private bool _adaptive;

        private object _sync = new object();
        private Queue<int> _pending = new Queue<int>();
        private Dictionary<int, int> _retries = new Dictionary<int, int>();
        private List<Thread> _workers = new List<Thread>();

        private volatile bool _stopRequested;
        private volatile bool _running;
        private ManualResetEvent _doneEvent = new ManualResetEvent(false);

        private long _totalBytes = -1;
        private long _downloadedBytes = 0;
        private double _speedBytesPerSec = 0;
        private double _bestSpeed = 0;
        private int _stagnation = 0;
        private bool _frozen = false; // true = больше не добавляем потоки

        private bool _completed = false;
        private bool _failed = false;
        private bool _canceled = false;
        private string _error = null;
        private bool _supportsRanges = false;

        private long _blockSize = 0;
        private int _blockCount = 0;

        static AdaptiveDownloader()
        {
            try
            {
                // Больше 2 соединений на хост (по умолчанию 2) - иначе
                // многопоточность упрётся в лимит ServicePoint.
                ServicePointManager.DefaultConnectionLimit = 32;
                ServicePointManager.Expect100Continue = false;
            }
            catch
            {
            }
            try
            {
                // Числовые значения TLS 1.1/1.2, чтобы старые рантаймы
                // могли качать с современных https-сайтов там, где
                // это поддерживается ОС. В .NET 2.0 этих имён в enum нет,
                // поэтому каст от int. Оборачиваем в try/catch.
                int tls11 = 768;
                int tls12 = 3072;
                int current = (int)ServicePointManager.SecurityProtocol;
                current = current | tls11 | tls12;
                ServicePointManager.SecurityProtocol = (SecurityProtocolType)current;
            }
            catch
            {
            }
        }

        public AdaptiveDownloader(string url, string destPath, int maxThreads, bool adaptive)
        {
            _url = url;
            _destPath = destPath;
            if (maxThreads < 1)
            {
                maxThreads = 1;
            }
            if (maxThreads > MaxAllowedThreads)
            {
                maxThreads = MaxAllowedThreads;
            }
            _maxThreads = maxThreads;
            _adaptive = adaptive;
        }

        public bool IsRunning
        {
            get { return _running; }
        }

        public bool IsCompleted
        {
            get { lock (_sync) { return _completed; } }
        }

        public bool IsFailed
        {
            get { lock (_sync) { return _failed; } }
        }

        public bool IsCanceled
        {
            get { lock (_sync) { return _canceled; } }
        }

        public string Error
        {
            get { lock (_sync) { return _error; } }
        }

        public long TotalBytes
        {
            get { lock (_sync) { return _totalBytes; } }
        }

        public long DownloadedBytes
        {
            get { lock (_sync) { return _downloadedBytes; } }
        }

        public double SpeedBytesPerSec
        {
            get { lock (_sync) { return _speedBytesPerSec; } }
        }

        public int LiveThreadCount
        {
            get { lock (_sync) { return CountAliveLocked(); } }
        }

        public int WaitingBlocks
        {
            get { lock (_sync) { return _pending.Count; } }
        }

        public bool AdaptiveFrozen
        {
            get { lock (_sync) { return _frozen; } }
        }

        public void Start()
        {
            lock (_sync)
            {
                if (_running)
                {
                    return;
                }
                _running = true;
                _stopRequested = false;
                _doneEvent.Reset();
            }
            Thread controller = new Thread(new ThreadStart(Run));
            controller.IsBackground = true;
            controller.Name = "DownloaderController";
            controller.Start();
        }

        public void Stop()
        {
            _stopRequested = true;
        }

        public void WaitDone()
        {
            _doneEvent.WaitOne();
        }

        private void Run()
        {
            try
            {
                string dir = null;
                try
                {
                    dir = Path.GetDirectoryName(_destPath);
                }
                catch
                {
                    dir = null;
                }
                if (dir != null && dir.Length > 0 && !Directory.Exists(dir))
                {
                    Directory.CreateDirectory(dir);
                }

                long total;
                bool ranges;
                Probe(_url, out total, out ranges);

                lock (_sync)
                {
                    _totalBytes = total;
                    _supportsRanges = ranges;
                }

                if (_stopRequested)
                {
                    MarkCanceled();
                    return;
                }

                // Однопоточно, если размер неизвестен, сервер не умеет
                // Range, файл маленький, выбран 1 поток или файл больше 2 ГБ
                // (int-перегрузки AddRange в чистом .NET 2.0).
                if (total <= 0 || !ranges || total < 1024 * 1024 || _maxThreads == 1 || total > 2147483647L)
                {
                    DownloadSingle();
                }
                else
                {
                    DownloadMulti(total);
                }
            }
            catch (Exception ex)
            {
                MarkFailed(ex.Message);
            }
            finally
            {
                lock (_sync)
                {
                    _running = false;
                }
                try
                {
                    _doneEvent.Set();
                }
                catch
                {
                }
            }
        }

        private static void Probe(string url, out long total, out bool acceptRanges)
        {
            total = -1;
            acceptRanges = false;

            // 1) Пробуем HEAD
            try
            {
                HttpWebRequest head = (HttpWebRequest)WebRequest.Create(url);
                head.Method = "HEAD";
                head.UserAgent = "MultiThreadDownloader/1.0";
                head.Timeout = 20000;
                head.ReadWriteTimeout = 20000;
                head.AllowAutoRedirect = true;
                using (HttpWebResponse resp = (HttpWebResponse)head.GetResponse())
                {
                    total = resp.ContentLength; // -1 если неизвестен
                    string ar = resp.Headers["Accept-Ranges"];
                    if (ar != null && ar.Trim().ToLowerInvariant() == "bytes")
                    {
                        acceptRanges = true;
                    }
                    else if (total > 0)
                    {
                        // Ряд серверов не шлёт заголовок, но Range держит.
                        // Точную проверку сделаем отдельным запросом ниже.
                    }
                }
            }
            catch
            {
                total = -1;
                acceptRanges = false;
            }

            // 2) Проверочный запрос bytes=0-0: точно узнаём поддержку Range.
            try
            {
                HttpWebRequest test = (HttpWebRequest)WebRequest.Create(url);
                test.Method = "GET";
                test.AddRange(0, 0);
                test.UserAgent = "MultiThreadDownloader/1.0";
                test.Timeout = 20000;
                test.ReadWriteTimeout = 20000;
                test.AllowAutoRedirect = true;
                using (HttpWebResponse resp = (HttpWebResponse)test.GetResponse())
                {
                    if (resp.StatusCode == HttpStatusCode.PartialContent)
                    {
                        acceptRanges = true;
                        // Content-Range: bytes 0-0/12345 -> вытащим полный размер
                        string cr = resp.Headers["Content-Range"];
                        if (cr != null)
                        {
                            int slash = cr.LastIndexOf('/');
                            if (slash >= 0 && slash + 1 < cr.Length)
                            {
                                string totalStr = cr.Substring(slash + 1).Trim();
                                long parsed;
                                if (long.TryParse(totalStr, out parsed) && parsed > 0)
                                {
                                    total = parsed;
                                }
                            }
                        }
                        if (total <= 0)
                        {
                            // Иногда полный размер не отдали - берём HEAD-значение
                            if (resp.ContentLength > 1)
                            {
                                // ContentLength тут = 1 (один байт), не подходит
                            }
                        }
                    }
                    else
                    {
                        // Сервер проигнорировал Range и отдал 200
                        acceptRanges = false;
                    }
                }
            }
            catch
            {
                // Оставляем то, что узнали из HEAD
            }
        }

        private void DownloadSingle()
        {
            HttpWebRequest req = (HttpWebRequest)WebRequest.Create(_url);
            req.Method = "GET";
            req.UserAgent = "MultiThreadDownloader/1.0";
            req.Timeout = 30000;
            req.ReadWriteTimeout = 30000;
            req.AllowAutoRedirect = true;

            using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
            {
                long total = resp.ContentLength;
                lock (_sync)
                {
                    _totalBytes = total;
                }

                using (Stream src = resp.GetResponseStream())
                {
                    using (FileStream dst = new FileStream(_destPath, FileMode.Create, FileAccess.Write, FileShare.Read))
                    {
                        byte[] buf = new byte[BufferSize];
                        long lastReport = 0;
                        DateTime start = DateTime.Now;
                        DateTime lastTime = start;

                        while (true)
                        {
                            if (_stopRequested)
                            {
                                MarkCanceled();
                                return;
                            }
                            int read = src.Read(buf, 0, buf.Length);
                            if (read <= 0)
                            {
                                break;
                            }
                            dst.Write(buf, 0, read);
                            lock (_sync)
                            {
                                _downloadedBytes += read;
                            }

                            DateTime now = DateTime.Now;
                            double elapsed = (now - lastTime).TotalSeconds;
                            if (elapsed >= 1.0)
                            {
                                long cur;
                                lock (_sync)
                                {
                                    cur = _downloadedBytes;
                                }
                                double inst = (double)(cur - lastReport) / elapsed;
                                lock (_sync)
                                {
                                    _speedBytesPerSec = inst;
                                    if (inst > _bestSpeed)
                                    {
                                        _bestSpeed = inst;
                                    }
                                }
                                lastReport = cur;
                                lastTime = now;
                            }
                        }
                    }
                }
            }

            if (_stopRequested)
            {
                MarkCanceled();
            }
            else
            {
                MarkCompleted();
            }
        }

        private void DownloadMulti(long total)
        {
            // Размер блока: не меньше 256 КБ, всего не больше ~512 блоков,
            // чтобы очередь была гранулярной для work-stealing.
            long blockSize = total / 512;
            if (blockSize < MinBlockSize)
            {
                blockSize = MinBlockSize;
            }
            int blockCount = (int)((total + blockSize - 1) / blockSize);

            lock (_sync)
            {
                _blockSize = blockSize;
                _blockCount = blockCount;
                _pending.Clear();
                _retries.Clear();
                for (int i = 0; i < blockCount; i++)
                {
                    _pending.Enqueue(i);
                }
                _downloadedBytes = 0;
                _speedBytesPerSec = 0;
                _bestSpeed = 0;
                _stagnation = 0;
                _frozen = false;
            }

            // Предсоздаём файл нужного размера
            using (FileStream pre = new FileStream(_destPath, FileMode.Create, FileAccess.Write, FileShare.Write))
            {
                pre.SetLength(total);
            }

            int initial = 1;
            if (!_adaptive)
            {
                initial = _maxThreads;
                if (initial > blockCount)
                {
                    initial = blockCount;
                }
            }
            else
            {
                // В адаптивном режиме стартуем с 2 потоков (если блоков >= 2),
                // чтобы сразу было видно эффект параллелизма.
                if (blockCount >= 2 && _maxThreads >= 2)
                {
                    initial = 2;
                }
                else
                {
                    initial = 1;
                }
            }

            lock (_sync)
            {
                _workers.Clear();
            }
            for (int i = 0; i < initial; i++)
            {
                StartOneWorkerLocked();
            }

            long prevDownloaded = 0;
            DateTime prevTime = DateTime.Now;

            while (true)
            {
                Thread.Sleep(MeasureIntervalMs);

                if (_stopRequested)
                {
                    JoinWorkers();
                    MarkCanceled();
                    return;
                }

                long cur;
                int alive;
                int waiting;
                lock (_sync)
                {
                    cur = _downloadedBytes;
                    alive = CountAliveLocked();
                    waiting = _pending.Count;
                }

                DateTime now = DateTime.Now;
                double elapsed = (now - prevTime).TotalSeconds;
                if (elapsed <= 0)
                {
                    elapsed = 1.0;
                }
                double speed = (double)(cur - prevDownloaded) / elapsed;
                if (speed < 0)
                {
                    speed = 0;
                }

                lock (_sync)
                {
                    _speedBytesPerSec = speed;
                    if (speed > _bestSpeed * GrowThreshold)
                    {
                        _bestSpeed = speed;
                        _stagnation = 0;
                    }
                    else
                    {
                        // Не растём (первый замер после старта не считаем
                        // застоем, если best ещё нулевой).
                        if (_bestSpeed > 0)
                        {
                            _stagnation++;
                        }
                        if (_stagnation >= 2)
                        {
                            _frozen = true;
                        }
                    }
                }

                // Решение о добавлении потока (вне lock чтения, под lock записи):
                bool add = false;
                lock (_sync)
                {
                    if (_adaptive && !_frozen && waiting > 0)
                    {
                        int a = CountAliveLocked();
                        if (a < _maxThreads)
                        {
                            if (_bestSpeed == 0 || speed >= _bestSpeed * 0.98 || _stagnation == 0)
                            {
                                // Скорость держится/растёт - добавляем
                                add = true;
                            }
                            else
                            {
                                // Скорость просела, но это может быть шум сети.
                                // Даём второй шанс (stagnation), заморозка
                                // наступит на следующем тике через _frozen.
                                if (_stagnation == 0)
                                {
                                    add = true;
                                }
                            }
                        }
                    }
                }
                if (add)
                {
                    lock (_sync)
                    {
                        int a2 = CountAliveLocked();
                        if (a2 < _maxThreads && _pending.Count > 0 && !_frozen)
                        {
                            StartOneWorkerLocked();
                        }
                    }
                }

                prevDownloaded = cur;
                prevTime = now;

                // Готовность: всё скачано
                if (cur >= total)
                {
                    JoinWorkers();
                    if (_stopRequested)
                    {
                        MarkCanceled();
                    }
                    else
                    {
                        MarkCompleted();
                    }
                    return;
                }

                // Готовность: очередь пуста и все воркеры завершились
                lock (_sync)
                {
                    alive = CountAliveLocked();
                    waiting = _pending.Count;
                }
                if (waiting == 0 && alive == 0)
                {
                    // Проверим реальный размер файла
                    try
                    {
                        FileInfo fi = new FileInfo(_destPath);
                        if (fi.Exists && fi.Length >= total)
                        {
                            lock (_sync)
                            {
                                _downloadedBytes = total;
                            }
                            MarkCompleted();
                        }
                        else
                        {
                            MarkFailed("Загрузка incomplete: сервер отдал не все байты.");
                        }
                    }
                    catch (Exception ex)
                    {
                        MarkFailed(ex.Message);
                    }
                    return;
                }
            }
        }

        private void StartOneWorkerLocked()
        {
            Thread t = new Thread(new ThreadStart(WorkerLoop));
            t.IsBackground = true;
            t.Name = "DownloaderWorker";
            _workers.Add(t);
            t.Start();
        }

        private int CountAliveLocked()
        {
            int n = 0;
            for (int i = 0; i < _workers.Count; i++)
            {
                try
                {
                    if (_workers[i] != null && _workers[i].IsAlive)
                    {
                        n++;
                    }
                }
                catch
                {
                }
            }
            return n;
        }

        private void JoinWorkers()
        {
            List<Thread> copy;
            lock (_sync)
            {
                copy = new List<Thread>(_workers);
            }
            for (int i = 0; i < copy.Count; i++)
            {
                try
                {
                    if (copy[i] != null && copy[i].IsAlive)
                    {
                        copy[i].Join(5000);
                    }
                }
                catch
                {
                }
            }
        }

        private void WorkerLoop()
        {
            while (true)
            {
                if (_stopRequested)
                {
                    return;
                }
                int idx = -1;
                lock (_sync)
                {
                    if (_pending.Count > 0)
                    {
                        idx = (int)_pending.Dequeue();
                    }
                    else
                    {
                        return; // работы больше нет
                    }
                }

                bool ok = false;
                try
                {
                    ok = DownloadBlock(idx);
                }
                catch
                {
                    ok = false;
                }

                if (_stopRequested)
                {
                    return;
                }

                if (!ok)
                {
                    int tries = 0;
                    lock (_sync)
                    {
                        if (_retries.ContainsKey(idx))
                        {
                            tries = (int)_retries[idx];
                        }
                        tries++;
                        _retries[idx] = tries;
                        // Возвращаем блок в очередь, максимум 5 попыток
                        if (tries <= 5)
                        {
                            _pending.Enqueue(idx);
                        }
                        else
                        {
                            _failed = true;
                            _error = "Не удалось скачать часть файла после 5 попыток (блок " + idx.ToString() + ").";
                        }
                    }
                    lock (_sync)
                    {
                        if (_failed)
                        {
                            _stopRequested = true;
                            return;
                        }
                    }
                    Thread.Sleep(500);
                }
            }
        }

        /// <summary>
        /// Выставляет Range-заголовок для long-границ.
        /// В .NET 2.0 у AddRange есть только int-перегрузки, а long
        /// появились позже. Поэтому сначала пробуем long-вариант через
        /// рефлексию (сработает, когда exe запущен на .NET 4.x),
        /// иначе - int-вариант (файлы до 2 ГБ).
        /// </summary>
        private static void AddRangeLong(HttpWebRequest req, long from, long to)
        {
            try
            {
                Type t = typeof(HttpWebRequest);
                Type[] sig = new Type[2];
                sig[0] = typeof(long);
                sig[1] = typeof(long);
                System.Reflection.MethodInfo m = t.GetMethod("AddRange", sig);
                if (m != null)
                {
                    object[] args = new object[2];
                    args[0] = from;
                    args[1] = to;
                    m.Invoke(req, args);
                    return;
                }
            }
            catch
            {
            }
            if (from > 2147483647L || to > 2147483647L)
            {
                throw new Exception("Файл больше 2 ГБ: многопоточный режим требует .NET 4.x в качестве рантайма.");
            }
            req.AddRange((int)from, (int)to);
        }

        private bool DownloadBlock(int index)
        {
            long total;
            long bsize;
            lock (_sync)
            {
                total = _totalBytes;
                bsize = _blockSize;
            }
            long start = (long)index * bsize;
            long end = start + bsize - 1;
            if (end >= total)
            {
                end = total - 1;
            }
            if (start >= total)
            {
                return true;
            }

            HttpWebRequest req = (HttpWebRequest)WebRequest.Create(_url);
            req.Method = "GET";
            AddRangeLong(req, start, end);
            req.UserAgent = "MultiThreadDownloader/1.0";
            req.Timeout = 30000;
            req.ReadWriteTimeout = 30000;
            req.AllowAutoRedirect = true;

            using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
            {
                if (resp.StatusCode != HttpStatusCode.PartialContent)
                {
                    return false;
                }
                using (Stream src = resp.GetResponseStream())
                {
                    using (FileStream dst = new FileStream(_destPath, FileMode.Open, FileAccess.Write, FileShare.Write))
                    {
                        dst.Seek(start, SeekOrigin.Begin);
                        byte[] buf = new byte[BufferSize];
                        long want = end - start + 1;
                        long got = 0;
                        while (got < want)
                        {
                            if (_stopRequested)
                            {
                                return true; // выходим без повторной постановки
                            }
                            int toRead = buf.Length;
                            long left = want - got;
                            if (left < toRead)
                            {
                                toRead = (int)left;
                            }
                            int read = src.Read(buf, 0, toRead);
                            if (read <= 0)
                            {
                                break;
                            }
                            dst.Write(buf, 0, read);
                            got += read;
                            lock (_sync)
                            {
                                _downloadedBytes += read;
                            }
                        }
                        if (got < want)
                        {
                            return false; // оборвалось - повторим блок
                        }
                        return true;
                    }
                }
            }
        }

        private void MarkCompleted()
        {
            lock (_sync)
            {
                _completed = true;
                _failed = false;
                _canceled = false;
                if (_totalBytes > 0)
                {
                    _downloadedBytes = _totalBytes;
                }
                _running = false;
            }
        }

        private void MarkFailed(string message)
        {
            lock (_sync)
            {
                _failed = true;
                _completed = false;
                _canceled = false;
                _error = message;
                _running = false;
            }
        }

        private void MarkCanceled()
        {
            lock (_sync)
            {
                _canceled = true;
                _completed = false;
                _failed = false;
                _running = false;
            }
        }
    }
}
