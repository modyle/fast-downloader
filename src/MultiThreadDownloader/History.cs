using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MultiThreadDownloader
{
    /// <summary>
    /// Одна запись истории загрузок. Только C# 2.0.
    /// </summary>
    public class HistoryEntry
    {
        private DateTime _time;
        private string _url;
        private string _filePath;
        private long _size;
        private string _status;

        public HistoryEntry(DateTime time, string url, string filePath, long size, string status)
        {
            _time = time;
            _url = url;
            _filePath = filePath;
            _size = size;
            _status = status;
        }

        public DateTime Time
        {
            get { return _time; }
            set { _time = value; }
        }

        public string Url
        {
            get { return _url; }
            set { _url = value; }
        }

        public string FilePath
        {
            get { return _filePath; }
            set { _filePath = value; }
        }

        public long Size
        {
            get { return _size; }
            set { _size = value; }
        }

        public string Status
        {
            get { return _status; }
            set { _status = value; }
        }
    }

    /// <summary>
    /// Хранилище истории в history.csv (таб-разделитель, Base64 для строк,
    /// чтобы не ломаться на ';' и переводах строк). Формат совместим с .NET 2.0.
    /// </summary>
    public class HistoryManager
    {
        private List<HistoryEntry> _entries = new List<HistoryEntry>();
        private string _file;

        public HistoryManager(string file)
        {
            _file = file;
        }

        public List<HistoryEntry> Entries
        {
            get { return _entries; }
        }

        public void Add(HistoryEntry e)
        {
            _entries.Insert(0, e);
            // Держим не больше 500 записей
            while (_entries.Count > 500)
            {
                _entries.RemoveAt(_entries.Count - 1);
            }
        }

        public void Clear()
        {
            _entries.Clear();
        }

        public void Load()
        {
            _entries.Clear();
            if (_file == null || _file.Length == 0)
            {
                return;
            }
            if (!File.Exists(_file))
            {
                return;
            }
            try
            {
                using (StreamReader sr = new StreamReader(_file, Encoding.UTF8))
                {
                    string line;
                    while ((line = sr.ReadLine()) != null)
                    {
                        if (line.Length == 0)
                        {
                            continue;
                        }
                        string[] parts = line.Split('\t');
                        if (parts.Length < 5)
                        {
                            continue;
                        }
                        try
                        {
                            long ticks = long.Parse(parts[0]);
                            string url = Decode(parts[1]);
                            string path = Decode(parts[2]);
                            long size = long.Parse(parts[3]);
                            string status = Decode(parts[4]);
                            HistoryEntry e = new HistoryEntry(new DateTime(ticks), url, path, size, status);
                            _entries.Add(e);
                        }
                        catch
                        {
                            continue;
                        }
                    }
                }
            }
            catch
            {
            }
        }

        public void Save()
        {
            if (_file == null || _file.Length == 0)
            {
                return;
            }
            try
            {
                string dir = Path.GetDirectoryName(_file);
                if (dir != null && dir.Length > 0 && !Directory.Exists(dir))
                {
                    Directory.CreateDirectory(dir);
                }
                using (StreamWriter sw = new StreamWriter(_file, false, Encoding.UTF8))
                {
                    for (int i = 0; i < _entries.Count; i++)
                    {
                        HistoryEntry e = (HistoryEntry)_entries[i];
                        string line = e.Time.Ticks.ToString()
                            + "\t" + Encode(e.Url)
                            + "\t" + Encode(e.FilePath)
                            + "\t" + e.Size.ToString()
                            + "\t" + Encode(e.Status);
                        sw.WriteLine(line);
                    }
                }
            }
            catch
            {
            }
        }

        private static string Encode(string s)
        {
            if (s == null)
            {
                return "";
            }
            byte[] bytes = Encoding.UTF8.GetBytes(s);
            return Convert.ToBase64String(bytes);
        }

        private static string Decode(string s)
        {
            if (s == null || s.Length == 0)
            {
                return "";
            }
            try
            {
                byte[] bytes = Convert.FromBase64String(s);
                return Encoding.UTF8.GetString(bytes);
            }
            catch
            {
                return s;
            }
        }
    }

    /// <summary>
    /// Форматирование размеров/скоростей. Без string interpolation (C# 2.0).
    /// </summary>
    public static class FormatHelper
    {
        public static string FormatSize(long bytes)
        {
            if (bytes < 0)
            {
                return "?";
            }
            if (bytes < 1024)
            {
                return bytes.ToString() + " Б";
            }
            double kb = (double)bytes / 1024.0;
            if (kb < 1024)
            {
                return kb.ToString("F1") + " КБ";
            }
            double mb = kb / 1024.0;
            if (mb < 1024)
            {
                return mb.ToString("F2") + " МБ";
            }
            double gb = mb / 1024.0;
            return gb.ToString("F2") + " ГБ";
        }

        public static string FormatSpeed(double bytesPerSec)
        {
            if (bytesPerSec < 0)
            {
                bytesPerSec = 0;
            }
            if (bytesPerSec < 1024)
            {
                return ((long)bytesPerSec).ToString() + " Б/с";
            }
            double kb = bytesPerSec / 1024.0;
            if (kb < 1024)
            {
                return kb.ToString("F1") + " КБ/с";
            }
            double mb = kb / 1024.0;
            return mb.ToString("F2") + " МБ/с";
        }

        public static string FileNameFromUrl(string url)
        {
            if (url == null || url.Length == 0)
            {
                return "download.bin";
            }
            try
            {
                Uri u = new Uri(url);
                string path = u.AbsolutePath;
                int slash = path.LastIndexOf('/');
                string name = (slash >= 0) ? path.Substring(slash + 1) : path;
                // Убираем query-хвост, если AbsolutePath его не убрал
                int q = name.IndexOf('?');
                if (q >= 0)
                {
                    name = name.Substring(0, q);
                }
                name = name.Trim();
                if (name.Length == 0)
                {
                    return "download.bin";
                }
                // Чистим запрещённые символы
                char[] bad = Path.GetInvalidFileNameChars();
                for (int i = 0; i < bad.Length; i++)
                {
                    name = name.Replace(bad[i].ToString(), "_");
                }
                // Uri оставляет %20 и т.п. - декодируем аккуратно
                try
                {
                    // Uri.UnescapeDataString есть в .NET 2.0
                    name = Uri.UnescapeDataString(name);
                }
                catch
                {
                }
                if (name.Length == 0)
                {
                    return "download.bin";
                }
                return name;
            }
            catch
            {
                return "download.bin";
            }
        }
    }
}
