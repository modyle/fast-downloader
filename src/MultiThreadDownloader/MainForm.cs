using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace MultiThreadDownloader
{
    /// <summary>
    /// Главное окно: прогресс текущей загрузки + история.
    /// Только C# 2.0 / WinForms .NET 2.0, без designer-файла.
    /// </summary>
    public class MainForm : Form
    {
        private Label lblUrl;
        private TextBox txtUrl;
        private Label lblSave;
        private TextBox txtSave;
        private Button btnBrowse;
        private Label lblThreads;
        private NumericUpDown numThreads;
        private CheckBox chkAdaptive;
        private Button btnStart;
        private Button btnStop;
        private ProgressBar progress;
        private Label lblStatus;
        private Label lblSpeed;
        private Label lblThreadInfo;
        private Label lblHistory;
        private ListView lvHistory;
        private Button btnClearHistory;
        private Button btnOpenFolder;
        private Timer timer;

        private AdaptiveDownloader _dl;
        private HistoryManager _history;
        private string _historyFile;
        private string _currentUrl = "";
        private string _currentPath = "";

        public MainForm()
        {
            _historyFile = Path.Combine(Application.StartupPath, "history.csv");
            _history = new HistoryManager(_historyFile);
            _history.Load();

            BuildUi();
            RefreshHistoryView();
        }

        private void BuildUi()
        {
            this.Text = "Многопоточный загрузчик (до 32 потоков, адаптивный)";
            this.ClientSize = new Size(720, 540);
            this.StartPosition = FormStartPosition.CenterScreen;
            this.MinimumSize = new Size(640, 480);

            lblUrl = new Label();
            lblUrl.Text = "URL файла:";
            lblUrl.Location = new Point(12, 12);
            lblUrl.Size = new Size(80, 16);
            this.Controls.Add(lblUrl);

            txtUrl = new TextBox();
            txtUrl.Location = new Point(12, 30);
            txtUrl.Size = new Size(696, 20);
            txtUrl.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            this.Controls.Add(txtUrl);

            lblSave = new Label();
            lblSave.Text = "Сохранить как:";
            lblSave.Location = new Point(12, 58);
            lblSave.Size = new Size(120, 16);
            this.Controls.Add(lblSave);

            txtSave = new TextBox();
            txtSave.Location = new Point(12, 76);
            txtSave.Size = new Size(616, 20);
            txtSave.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            this.Controls.Add(txtSave);

            btnBrowse = new Button();
            btnBrowse.Text = "...";
            btnBrowse.Location = new Point(634, 74);
            btnBrowse.Size = new Size(74, 23);
            btnBrowse.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            btnBrowse.Click += new EventHandler(BtnBrowse_Click);
            this.Controls.Add(btnBrowse);

            lblThreads = new Label();
            lblThreads.Text = "Макс. потоков (1-32):";
            lblThreads.Location = new Point(12, 106);
            lblThreads.Size = new Size(140, 16);
            this.Controls.Add(lblThreads);

            numThreads = new NumericUpDown();
            numThreads.Location = new Point(152, 104);
            numThreads.Size = new Size(60, 20);
            numThreads.Minimum = 1;
            numThreads.Maximum = 32;
            numThreads.Value = 32;
            this.Controls.Add(numThreads);

            chkAdaptive = new CheckBox();
            chkAdaptive.Text = "Авторежим: добавлять потоки, пока растёт скорость";
            chkAdaptive.Location = new Point(224, 105);
            chkAdaptive.Size = new Size(400, 18);
            chkAdaptive.Checked = true;
            this.Controls.Add(chkAdaptive);

            btnStart = new Button();
            btnStart.Text = "Скачать";
            btnStart.Location = new Point(12, 132);
            btnStart.Size = new Size(120, 28);
            btnStart.Click += new EventHandler(BtnStart_Click);
            this.Controls.Add(btnStart);

            btnStop = new Button();
            btnStop.Text = "Стоп";
            btnStop.Location = new Point(140, 132);
            btnStop.Size = new Size(120, 28);
            btnStop.Enabled = false;
            btnStop.Click += new EventHandler(BtnStop_Click);
            this.Controls.Add(btnStop);

            progress = new ProgressBar();
            progress.Location = new Point(12, 170);
            progress.Size = new Size(696, 22);
            progress.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            progress.Minimum = 0;
            progress.Maximum = 1000;
            progress.Value = 0;
            this.Controls.Add(progress);

            lblStatus = new Label();
            lblStatus.Text = "Готов.";
            lblStatus.Location = new Point(12, 198);
            lblStatus.Size = new Size(696, 16);
            lblStatus.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            this.Controls.Add(lblStatus);

            lblSpeed = new Label();
            lblSpeed.Text = "Скорость: —";
            lblSpeed.Location = new Point(12, 216);
            lblSpeed.Size = new Size(340, 16);
            this.Controls.Add(lblSpeed);

            lblThreadInfo = new Label();
            lblThreadInfo.Text = "Потоки: —";
            lblThreadInfo.Location = new Point(360, 216);
            lblThreadInfo.Size = new Size(348, 16);
            lblThreadInfo.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            this.Controls.Add(lblThreadInfo);

            lblHistory = new Label();
            lblHistory.Text = "История загрузок:";
            lblHistory.Location = new Point(12, 242);
            lblHistory.Size = new Size(200, 16);
            this.Controls.Add(lblHistory);

            lvHistory = new ListView();
            lvHistory.Location = new Point(12, 260);
            lvHistory.Size = new Size(696, 200);
            lvHistory.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            lvHistory.View = View.Details;
            lvHistory.FullRowSelect = true;
            lvHistory.GridLines = true;
            lvHistory.Columns.Add("Дата", 120);
            lvHistory.Columns.Add("Файл", 180);
            lvHistory.Columns.Add("Размер", 90);
            lvHistory.Columns.Add("Статус", 130);
            lvHistory.Columns.Add("URL", 170);
            this.Controls.Add(lvHistory);

            btnClearHistory = new Button();
            btnClearHistory.Text = "Очистить историю";
            btnClearHistory.Location = new Point(12, 468);
            btnClearHistory.Size = new Size(150, 26);
            btnClearHistory.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnClearHistory.Click += new EventHandler(BtnClearHistory_Click);
            this.Controls.Add(btnClearHistory);

            btnOpenFolder = new Button();
            btnOpenFolder.Text = "Открыть папку загрузок";
            btnOpenFolder.Location = new Point(170, 468);
            btnOpenFolder.Size = new Size(170, 26);
            btnOpenFolder.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnOpenFolder.Click += new EventHandler(BtnOpenFolder_Click);
            this.Controls.Add(btnOpenFolder);

            timer = new Timer();
            timer.Interval = 500;
            timer.Tick += new EventHandler(Timer_Tick);

            this.FormClosing += new FormClosingEventHandler(MainForm_FormClosing);
        }

        private void BtnBrowse_Click(object sender, EventArgs e)
        {
            using (SaveFileDialog dlg = new SaveFileDialog())
            {
                dlg.Title = "Куда сохранить файл";
                string guess = FormatHelper.FileNameFromUrl(txtUrl.Text.Trim());
                dlg.FileName = guess;
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    txtSave.Text = dlg.FileName;
                }
            }
        }

        private void BtnStart_Click(object sender, EventArgs e)
        {
            if (_dl != null && _dl.IsRunning)
            {
                return;
            }
            string url = txtUrl.Text.Trim();
            if (url.Length == 0 || (!url.StartsWith("http://") && !url.StartsWith("https://")))
            {
                MessageBox.Show(this, "Введите корректный URL (http:// или https://).",
                    "Проверка URL", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string dest = txtSave.Text.Trim();
            if (dest.Length == 0)
            {
                using (SaveFileDialog dlg = new SaveFileDialog())
                {
                    dlg.Title = "Куда сохранить файл";
                    dlg.FileName = FormatHelper.FileNameFromUrl(url);
                    if (dlg.ShowDialog(this) != DialogResult.OK)
                    {
                        return;
                    }
                    dest = dlg.FileName;
                    txtSave.Text = dest;
                }
            }

            int maxT = (int)numThreads.Value;
            if (maxT < 1)
            {
                maxT = 1;
            }
            if (maxT > 32)
            {
                maxT = 32;
            }
            bool adaptive = chkAdaptive.Checked;

            _currentUrl = url;
            _currentPath = dest;

            _dl = new AdaptiveDownloader(url, dest, maxT, adaptive);
            progress.Value = 0;
            lblStatus.Text = "Подключение...";
            lblSpeed.Text = "Скорость: —";
            lblThreadInfo.Text = "Потоки: —";
            btnStart.Enabled = false;
            btnStop.Enabled = true;
            numThreads.Enabled = false;
            chkAdaptive.Enabled = false;
            _dl.Start();
            timer.Start();
        }

        private void BtnStop_Click(object sender, EventArgs e)
        {
            if (_dl != null)
            {
                _dl.Stop();
                lblStatus.Text = "Остановка...";
                btnStop.Enabled = false;
            }
        }

        private void Timer_Tick(object sender, EventArgs e)
        {
            if (_dl == null)
            {
                return;
            }
            long total = _dl.TotalBytes;
            long done = _dl.DownloadedBytes;
            double speed = _dl.SpeedBytesPerSec;
            int threads = 0;
            int waiting = 0;
            bool frozen = false;
            try
            {
                threads = _dl.LiveThreadCount;
                waiting = _dl.WaitingBlocks;
                frozen = _dl.AdaptiveFrozen;
            }
            catch
            {
            }

            if (total > 0)
            {
                double pct = (double)done * 100.0 / (double)total;
                int v = (int)((double)done * 1000.0 / (double)total);
                if (v < 0)
                {
                    v = 0;
                }
                if (v > 1000)
                {
                    v = 1000;
                }
                progress.Value = v;
                lblStatus.Text = "Скачано " + FormatHelper.FormatSize(done)
                    + " из " + FormatHelper.FormatSize(total)
                    + " (" + pct.ToString("F1") + "%)";
            }
            else
            {
                lblStatus.Text = "Скачано " + FormatHelper.FormatSize(done)
                    + " (размер определяется...)";
            }
            lblSpeed.Text = "Скорость: " + FormatHelper.FormatSpeed(speed);

            string tinfo = "Потоки: " + threads.ToString() + " / макс " + ((int)numThreads.Value).ToString();
            if (waiting > 0)
            {
                tinfo += " | блоков в очереди: " + waiting.ToString();
            }
            if (chkAdaptive.Checked)
            {
                if (frozen)
                {
                    tinfo += " | рост остановлен (скорость не растёт)";
                }
                else
                {
                    tinfo += " | адаптивный подбор...";
                }
            }
            lblThreadInfo.Text = tinfo;

            if (_dl.IsCompleted)
            {
                timer.Stop();
                FinishOk(total);
            }
            else if (_dl.IsFailed)
            {
                timer.Stop();
                FinishFail(_dl.Error);
            }
            else if (_dl.IsCanceled)
            {
                timer.Stop();
                FinishCancel(done);
            }
        }

        private void FinishOk(long total)
        {
            btnStart.Enabled = true;
            btnStop.Enabled = false;
            numThreads.Enabled = true;
            chkAdaptive.Enabled = true;
            progress.Value = 1000;
            lblStatus.Text = "Готово: " + _currentPath;
            lblSpeed.Text = "Скорость: —";
            HistoryEntry en = new HistoryEntry(DateTime.Now, _currentUrl, _currentPath, total, "OK");
            _history.Add(en);
            _history.Save();
            RefreshHistoryView();
            _dl = null;
        }

        private void FinishFail(string error)
        {
            btnStart.Enabled = true;
            btnStop.Enabled = false;
            numThreads.Enabled = true;
            chkAdaptive.Enabled = true;
            lblStatus.Text = "Ошибка: " + error;
            string st = "Ошибка";
            if (error != null && error.Length > 0)
            {
                st = "Ошибка: " + error;
            }
            HistoryEntry en = new HistoryEntry(DateTime.Now, _currentUrl, _currentPath, 0, st);
            _history.Add(en);
            _history.Save();
            RefreshHistoryView();
            MessageBox.Show(this, "Не удалось скачать файл.\n" + error,
                "Ошибка загрузки", MessageBoxButtons.OK, MessageBoxIcon.Error);
            _dl = null;
        }

        private void FinishCancel(long done)
        {
            btnStart.Enabled = true;
            btnStop.Enabled = false;
            numThreads.Enabled = true;
            chkAdaptive.Enabled = true;
            lblStatus.Text = "Остановлено пользователем.";
            HistoryEntry en = new HistoryEntry(DateTime.Now, _currentUrl, _currentPath, done, "Отмена");
            _history.Add(en);
            _history.Save();
            RefreshHistoryView();
            _dl = null;
        }

        private void RefreshHistoryView()
        {
            lvHistory.BeginUpdate();
            lvHistory.Items.Clear();
            for (int i = 0; i < _history.Entries.Count; i++)
            {
                HistoryEntry en = (HistoryEntry)_history.Entries[i];
                string[] cols = new string[5];
                cols[0] = en.Time.ToString("yyyy-MM-dd HH:mm:ss");
                string fp = en.FilePath;
                try
                {
                    fp = Path.GetFileName(en.FilePath);
                    if (fp == null || fp.Length == 0)
                    {
                        fp = en.FilePath;
                    }
                }
                catch
                {
                    fp = en.FilePath;
                }
                cols[1] = fp;
                cols[2] = FormatHelper.FormatSize(en.Size);
                cols[3] = en.Status;
                cols[4] = en.Url;
                ListViewItem item = new ListViewItem(cols);
                lvHistory.Items.Add(item);
            }
            lvHistory.EndUpdate();
        }

        private void BtnClearHistory_Click(object sender, EventArgs e)
        {
            _history.Clear();
            _history.Save();
            RefreshHistoryView();
        }

        private void BtnOpenFolder_Click(object sender, EventArgs e)
        {
            try
            {
                string dir = null;
                if (txtSave.Text.Trim().Length > 0)
                {
                    try
                    {
                        dir = Path.GetDirectoryName(Path.GetFullPath(txtSave.Text.Trim()));
                    }
                    catch
                    {
                        dir = null;
                    }
                }
                if (dir == null || dir.Length == 0 || !Directory.Exists(dir))
                {
                    dir = Application.StartupPath;
                }
                System.Diagnostics.Process.Start(dir);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Не удалось открыть папку.\n" + ex.Message,
                    "Папка", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        private void MainForm_FormClosing(object sender, FormClosingEventArgs e)
        {
            try
            {
                if (_dl != null && _dl.IsRunning)
                {
                    _dl.Stop();
                }
                _history.Save();
            }
            catch
            {
            }
        }
    }
}
