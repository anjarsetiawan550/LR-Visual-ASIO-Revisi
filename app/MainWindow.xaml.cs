using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO.MemoryMappedFiles;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using NAudio.CoreAudioApi;

namespace LRVisualASIO
{
    public class DeviceItem
    {
        public string Id { get; set; }
        public string Name { get; set; }
        public override string ToString() { return Name; }
    }

    public partial class MainWindow : Window
    {
        static readonly int[] Rates = { 44100, 48000, 88200, 96000, 176400, 192000 };
        static readonly int[] Buffers = { 32, 64, 128, 256, 512, 1024, 2048 };
        const string CfgKey = @"Software\LRVisual\ASIO";
        const string StatusMap = "LRVisualASIO_Status";

        int _sr = 48000, _buf = 128, _regBuf = -1, _pollTick;
        double _ratio = 1.0;
        const double MinW = 380, DefW = 640;
        const string GuiKey = @"Software\LRVisual\GUI";
        int _lastHb = -1;
        DateTime _lastHbTime = DateTime.MinValue;
        double _smIn, _smOut;
        bool? _lastActive;
        string _lastTip;
        bool _exiting;
        readonly DispatcherTimer _timer;

        System.Windows.Forms.NotifyIcon _tray;
        System.Windows.Forms.ToolStripMenuItem _miAuto;
        System.Drawing.Icon _icoIdle, _icoActive;

        public MainWindow()
        {
            InitializeComponent();
            BuildChips();
            LoadDevices();
            LoadConfig();
            UpdateBufInfo();
            InitScale();
            EnsureAutoStartDefault();
            SyncAuto();
            InitTray();

            Footer.Text = DriverRegistered()
                ? "Driver \"LR Visual ASIO\" terdaftar — pilih di DAW: Audio Device → ASIO → LR Visual ASIO."
                : "Driver belum terdaftar. Jalankan installer sebagai Administrator.";

            Activated += (s, e) => { RefreshDevices(); SyncAuto(); };
            Application.Current.SessionEnding += (s, e) => _exiting = true;

            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            _timer.Tick += (s, e) => Poll();
            _timer.Start();
            Poll();
        }

        // ------------------------------------------------------------ tray & latar belakang
        static System.Drawing.Icon LoadIcon(string name)
        {
            var sri = Application.GetResourceStream(new Uri("pack://application:,,,/Assets/" + name));
            using (var st = sri.Stream)
                return new System.Drawing.Icon(st, System.Windows.Forms.SystemInformation.SmallIconSize);
        }

        void InitTray()
        {
            _icoIdle = LoadIcon("icon.ico");
            _icoActive = LoadIcon("icon_active.ico");
            _tray = new System.Windows.Forms.NotifyIcon { Icon = _icoIdle, Text = "LR Visual ASIO — NONAKTIF", Visible = true };

            var menu = new System.Windows.Forms.ContextMenuStrip();
            menu.Items.Add("Buka LR Visual ASIO", null, (s, e) => ShowFromTray());
            _miAuto = new System.Windows.Forms.ToolStripMenuItem("Mulai otomatis dengan Windows") { CheckOnClick = true };
            _miAuto.Click += (s, e) => { SetAutoStart(_miAuto.Checked); SyncAuto(); };
            menu.Items.Add(_miAuto);
            menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
            menu.Items.Add("Keluar", null, (s, e) => ExitApp());
            menu.Opening += (s, e) => _miAuto.Checked = AutoStart.IsEnabled();
            _tray.ContextMenuStrip = menu;
            _tray.DoubleClick += (s, e) => ShowFromTray();
        }

        public void ShowFromTray()
        {
            if (!IsVisible) Show();
            if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
            Activate();
            Topmost = true; Topmost = false;
        }

        void ExitApp()
        {
            _exiting = true;
            if (_tray != null) { _tray.Visible = false; _tray.Dispose(); _tray = null; }
            Application.Current.Shutdown();
        }

        protected override void OnClosing(CancelEventArgs e)
        {
            if (!_exiting) { e.Cancel = true; Hide(); }   // tombol ✕ = sembunyikan ke tray, driver tetap dipantau
            base.OnClosing(e);
        }

        // ------------------------------------------------------------ auto start
        void EnsureAutoStartDefault()
        {
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(CfgKey))
                {
                    if (Convert.ToInt32(k.GetValue("AutoStartInit", 0)) == 0)
                    {
                        AutoStart.Enable();                      // default: nyala saat pertama kali dijalankan
                        k.SetValue("AutoStartInit", 1, RegistryValueKind.DWord);
                    }
                    else AutoStart.RefreshPath();
                }
            }
            catch { }
        }

        static void SetAutoStart(bool on)
        {
            try { if (on) AutoStart.Enable(); else AutoStart.Disable(); } catch { }
        }

        void SyncAuto()
        {
            bool on = AutoStart.IsEnabled();
            ChkAuto.IsChecked = on;
            if (_miAuto != null) _miAuto.Checked = on;
        }

        void Auto_Click(object sender, RoutedEventArgs e)
        {
            SetAutoStart(ChkAuto.IsChecked == true);
            SyncAuto();
        }

        // ------------------------------------------------------------ UI builders
        void BuildChips()
        {
            foreach (var r in Rates)
            {
                var rb = new RadioButton
                {
                    Content = (r / 1000.0).ToString("0.0##") + " kHz",
                    Tag = r,
                    GroupName = "sr",
                    Style = (Style)FindResource("Chip")
                };
                rb.Checked += (s, e) => { _sr = (int)((RadioButton)s).Tag; UpdateBufInfo(); };
                RatePanel.Children.Add(rb);
            }
            foreach (var b in Buffers)
            {
                var rb = new RadioButton
                {
                    Content = b.ToString(),
                    Tag = b,
                    GroupName = "buf",
                    Style = (Style)FindResource("Chip")
                };
                rb.Checked += (s, e) => { _buf = (int)((RadioButton)s).Tag; UpdateBufInfo(); };
                BufPanel.Children.Add(rb);
            }
        }

        void UpdateBufInfo()
        {
            if (BufInfo == null) return;
            BufInfo.Text = $"{_buf} samples  •  {_buf * 1000.0 / _sr:0.00} ms per buffer";
        }

        void LoadDevices()
        {
            FillCombo(CmbOut, DataFlow.Render, "");
            FillCombo(CmbIn, DataFlow.Capture, "");
        }

        void Refresh_Click(object sender, RoutedEventArgs e) => RefreshDevices();

        // muat ulang daftar perangkat tanpa kehilangan pilihan yang sedang aktif
        void RefreshDevices()
        {
            if (CmbOut.IsDropDownOpen || CmbIn.IsDropDownOpen) return;
            var o = (CmbOut.SelectedItem as DeviceItem)?.Id ?? "";
            var i = (CmbIn.SelectedItem as DeviceItem)?.Id ?? "";
            FillCombo(CmbOut, DataFlow.Render, o);
            FillCombo(CmbIn, DataFlow.Capture, i);
        }

        static void FillCombo(ComboBox cb, DataFlow flow, string selectId)
        {
            var items = new List<DeviceItem> { new DeviceItem { Id = "", Name = "Default Windows" } };
            try
            {
                using (var en = new MMDeviceEnumerator())
                    foreach (var d in en.EnumerateAudioEndPoints(flow, DeviceState.Active))
                        items.Add(new DeviceItem { Id = d.ID, Name = d.FriendlyName });
            }
            catch { }
            cb.ItemsSource = items;
            var idx = items.FindIndex(d => d.Id == selectId);
            cb.SelectedIndex = idx >= 0 ? idx : 0;
        }

        // ------------------------------------------------------------ konfigurasi
        void LoadConfig()
        {
            int sr = 48000, buf = 128, safe = 0; string outId = "", inId = "";
            using (var k = Registry.CurrentUser.OpenSubKey(CfgKey))
            {
                if (k != null)
                {
                    sr = Convert.ToInt32(k.GetValue("SampleRate", 48000));
                    buf = Convert.ToInt32(k.GetValue("BufferSize", 128));
                    safe = Convert.ToInt32(k.GetValue("SafeMode", 0));
                    outId = (k.GetValue("OutputDevice", "") as string) ?? "";
                    inId = (k.GetValue("InputDevice", "") as string) ?? "";
                }
            }
            if (!Rates.Contains(sr)) sr = 48000;
            if (!Buffers.Contains(buf)) buf = 128;
            _regBuf = buf;

            foreach (RadioButton rb in RatePanel.Children) rb.IsChecked = (int)rb.Tag == sr;
            foreach (RadioButton rb in BufPanel.Children) rb.IsChecked = (int)rb.Tag == buf;
            ChkSafe.IsChecked = safe != 0;
            Select(CmbOut, outId);
            Select(CmbIn, inId);
        }

        static void Select(ComboBox cb, string id)
        {
            var list = (List<DeviceItem>)cb.ItemsSource;
            var idx = list.FindIndex(d => d.Id == id);
            cb.SelectedIndex = idx >= 0 ? idx : 0;
        }

        void Apply_Click(object sender, RoutedEventArgs e)
        {
            bool bufChanged = _buf != _regBuf;
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(CfgKey))
                {
                    k.SetValue("SampleRate", _sr, RegistryValueKind.DWord);
                    k.SetValue("BufferSize", _buf, RegistryValueKind.DWord);
                    if (bufChanged) k.SetValue("BufferForce", 1, RegistryValueKind.DWord);   // DAW dipaksa memakai buffer baru saat memuat ulang driver
                    k.SetValue("SafeMode", ChkSafe.IsChecked == true ? 1 : 0, RegistryValueKind.DWord);
                    k.SetValue("OutputDevice", (CmbOut.SelectedItem as DeviceItem)?.Id ?? "", RegistryValueKind.String);
                    k.SetValue("InputDevice", (CmbIn.SelectedItem as DeviceItem)?.Id ?? "", RegistryValueKind.String);
                }
                _regBuf = _buf;
                Note.Text = "Tersimpan. Jika DAW sedang memakai driver, DAW diminta memuat ulang dan akan memakai buffer ini; jika tidak, berlaku saat driver dibuka berikutnya.";
            }
            catch (Exception ex)
            {
                Note.Text = "Gagal menyimpan: " + ex.Message;
            }
        }

        static bool DriverRegistered()
        {
            try { using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\ASIO\LR Visual ASIO")) return k != null; }
            catch { return false; }
        }

        // ------------------------------------------------------------ status realtime dari driver
        void Poll()
        {
            try { PollCore(); if (++_pollTick % 5 == 0) SyncBufFromRegistry(); } catch { }
        }

        // DAW mengubah buffer -> driver menulis registry -> chip buffer di aplikasi ikut berubah
        void SyncBufFromRegistry()
        {
            int buf;
            using (var k = Registry.CurrentUser.OpenSubKey(CfgKey))
            {
                if (k == null) return;
                buf = Convert.ToInt32(k.GetValue("BufferSize", _regBuf));
            }
            if (buf == _regBuf) return;
            _regBuf = buf;
            if (!Buffers.Contains(buf)) return;
            foreach (RadioButton rb in BufPanel.Children) rb.IsChecked = (int)rb.Tag == buf;
            Note.Text = "Buffer diubah dari DAW: " + buf + " samples.";
        }

        static double Frac(int peak)
        {
            if (peak <= 0) return 0;
            double db = 20.0 * Math.Log10(peak / 32767.0);
            return Math.Max(0, Math.Min(1, (db + 60.0) / 60.0));
        }

        void PollCore()
        {
            int[] v = null;
            try
            {
                using (var m = MemoryMappedFile.OpenExisting(StatusMap, MemoryMappedFileRights.Read))
                using (var a = m.CreateViewAccessor(0, 128, MemoryMappedFileAccess.Read))
                {
                    v = new int[17];
                    for (int i = 0; i < 17; i++) v[i] = a.ReadInt32(i * 4);
                }
            }
            catch { v = null; }

            bool loaded = false, active = false;
            if (v != null)
            {
                if (v[9] != _lastHb) { _lastHb = v[9]; _lastHbTime = DateTime.UtcNow; }
                bool fresh = (DateTime.UtcNow - _lastHbTime).TotalSeconds < 2.0;
                loaded = v[0] == 1 && fresh;
                active = loaded && v[1] == 1;
            }
            else _lastHb = -1;

            // status utama
            if (_lastActive != active)
            {
                var c = active ? Color.FromRgb(0x00, 0xE5, 0xA0) : Color.FromRgb(0xFF, 0x4D, 0x6D);
                Dot.Fill = new SolidColorBrush(c);
                DotGlow.Color = c;
                Pill.BorderBrush = new SolidColorBrush(c);
                Pill.Background = new SolidColorBrush(active ? Color.FromRgb(0x0F, 0x2A, 0x2A) : Color.FromRgb(0x2A, 0x1A, 0x2E));
                StatusText.Text = active ? "AKTIF" : "NONAKTIF";
                if (_tray != null) _tray.Icon = active ? _icoActive : _icoIdle;
                _lastActive = active;
            }
            StatusSub.Text = active ? "Streaming ke DAW" : loaded ? "Terhubung ke DAW (siaga)" : "Menunggu DAW";

            string tip;
            if (loaded)
            {
                int sr = v[2];
                ValRate.Text = (sr / 1000.0).ToString("0.0##") + " kHz";
                ValBuf.Text = v[3] + " smp";
                ValIn.Text = Lat(v[4], sr);
                ValOut.Text = Lat(v[5], sr);
                ValMode.Text = v[14] == 1 ? "Shared (fallback)" : v[8] == 1 ? "Safe (Shared)" : "Exclusive";
                ValRtt.Text = Lat(v[4] + v[5], sr);
                ValDev.Text = v[15] > 0 ? $"{v[15] * 1000.0 / sr:0.00} ms" : "—";
                if (v[14] == 1)
                {
                    ModeWarn.Text = "Mode Exclusive gagal sehingga memakai Shared (latensi lebih tinggi). Di Windows: Sound → perangkat → Properties → Advanced → centang \"Allow applications to take exclusive control\", dan tutup aplikasi lain yang memakai perangkat itu.";
                    ModeWarn.Visibility = Visibility.Visible;
                }
                else if (v[15] > v[3] && v[3] > 0)
                {
                    ModeWarn.Text = "Perangkat memakai buffer minimum " + v[15] + " smp (lebih besar dari buffer ASIO " + v[3] + "). Itu batas dari driver perangkat; buffer ASIO lebih kecil tidak menurunkan latensi.";
                    ModeWarn.Visibility = Visibility.Visible;
                }
                else ModeWarn.Visibility = Visibility.Collapsed;
                ValCh.Text = v[6] + " / " + v[7];
                tip = "LR Visual ASIO — " + (active ? "AKTIF" : "siaga") + " • " + (sr / 1000.0).ToString("0.0##") + " kHz • " + v[3] + " smp";
            }
            else
            {
                ValRate.Text = ValBuf.Text = ValIn.Text = ValOut.Text = ValMode.Text = ValCh.Text = ValRtt.Text = ValDev.Text = "—";
                ModeWarn.Visibility = Visibility.Collapsed;
                tip = "LR Visual ASIO — NONAKTIF";
            }
            if (_tray != null && tip != _lastTip) { _tray.Text = tip.Length > 120 ? tip.Substring(0, 120) : tip; _lastTip = tip; }

            // meter level (hanya jika jendela terlihat)
            if (IsVisible)
            {
                double fi = active && v != null ? Frac(v[11]) : 0, fo = active && v != null ? Frac(v[12]) : 0;
                _smIn = Math.Max(fi, _smIn * 0.82);
                _smOut = Math.Max(fo, _smOut * 0.82);
                MeterIn.Width = _smIn * MeterInTrack.ActualWidth;
                MeterOut.Width = _smOut * MeterOutTrack.ActualWidth;
                InWarn.Visibility = (active && v != null && v[6] > 0 && v[13] == 0) ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        static string Lat(int smp, int sr) =>
            sr > 0 ? $"{smp * 1000.0 / sr:0.00} ms" : "—";

        // ------------------------------------------------------------ ukuran jendela (proporsional)
        void InitScale()
        {
            RootCard.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var d = RootCard.DesiredSize;
            _ratio = d.Width > 0 ? d.Height / d.Width : 1.0;
            double w = DefW;
            try { using (var k = Registry.CurrentUser.OpenSubKey(GuiKey)) if (k != null) w = Convert.ToDouble(k.GetValue("Width", DefW)); } catch { }
            ApplyWidth(w);
        }

        void ApplyWidth(double w)
        {
            double maxW = Math.Min(SystemParameters.WorkArea.Width, SystemParameters.WorkArea.Height / _ratio);
            w = Math.Max(MinW, Math.Min(w, maxW));
            Width = w; Height = w * _ratio;
        }

        void GripR_Delta(object s, DragDeltaEventArgs e) => ApplyWidth(Width + e.HorizontalChange);
        void GripB_Delta(object s, DragDeltaEventArgs e) => ApplyWidth((Height + e.VerticalChange) / _ratio);
        void GripC_Delta(object s, DragDeltaEventArgs e)
        {
            double dw = e.HorizontalChange, dh = e.VerticalChange / _ratio;
            ApplyWidth(Width + (Math.Abs(dw) > Math.Abs(dh) ? dw : dh));
        }
        void Grip_Done(object s, DragCompletedEventArgs e)
        {
            try { using (var k = Registry.CurrentUser.CreateSubKey(GuiKey)) k.SetValue("Width", (int)Width, RegistryValueKind.DWord); } catch { }
        }

        // ------------------------------------------------------------ jendela
        void Header_MouseDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }
        void Min_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
        void Close_Click(object sender, RoutedEventArgs e) => Close();   // -> OnClosing: sembunyi ke tray
    }
}
