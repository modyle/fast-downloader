using System;
using System.Windows.Forms;

namespace MultiThreadDownloader
{
    /// <summary>
    /// Точка входа. Только C# 2.0.
    /// </summary>
    public static class Program
    {
        [STAThread]
        public static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            MainForm f = new MainForm();
            Application.Run(f);
        }
    }
}
