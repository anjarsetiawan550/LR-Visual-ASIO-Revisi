using System;
using System.Linq;
using System.Threading;
using System.Windows;

namespace LRVisualASIO
{
    public partial class App : Application
    {
        Mutex _mutex;
        EventWaitHandle _showEvt;
        LRVisualASIO.MainWindow _win;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);
            ShutdownMode = ShutdownMode.OnExplicitShutdown;   // keluar hanya lewat menu tray "Keluar"

            bool background = e.Args.Any(a => a.Equals("--background", StringComparison.OrdinalIgnoreCase));
            bool created;
            _mutex = new Mutex(true, "LRVisualASIO_SingleInstance", out created);
            _showEvt = new EventWaitHandle(false, EventResetMode.AutoReset, "LRVisualASIO_ShowEvent");

            if (!created)                       // sudah berjalan di tray -> tampilkan jendela yang ada
            {
                if (!background) _showEvt.Set();
                Shutdown();
                return;
            }

            _win = new LRVisualASIO.MainWindow();
            var t = new Thread(() =>
            {
                while (true)
                {
                    _showEvt.WaitOne();
                    try { Dispatcher.Invoke(() => _win.ShowFromTray()); } catch { return; }
                }
            }) { IsBackground = true };
            t.Start();

            if (!background) _win.ShowFromTray();
        }
    }
}
