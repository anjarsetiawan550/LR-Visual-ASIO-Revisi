using System;
using Microsoft.Win32;

namespace LRVisualASIO
{
    // Auto start lewat HKCU\...\Run -> otomatis muncul di Task Manager > Startup apps (bisa di-Disable di sana).
    static class AutoStart
    {
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string ApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
        const string ValueName = "LR Visual ASIO";

        static string Command => "\"" + Environment.ProcessPath + "\" --background";

        public static bool IsEnabled()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey))
                    if (k == null || k.GetValue(ValueName) == null) return false;
                using (var a = Registry.CurrentUser.OpenSubKey(ApprovedKey))
                {
                    var b = a?.GetValue(ValueName) as byte[];
                    if (b != null && b.Length > 0 && (b[0] & 1) == 1) return false;   // ganjil (03) = dimatikan di Task Manager
                }
                return true;
            }
            catch { return false; }
        }

        public static void Enable()
        {
            using (var k = Registry.CurrentUser.CreateSubKey(RunKey)) k.SetValue(ValueName, Command);
            using (var a = Registry.CurrentUser.CreateSubKey(ApprovedKey))
                a.SetValue(ValueName, new byte[] { 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, RegistryValueKind.Binary);
        }

        public static void Disable()
        {
            using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true)) k?.DeleteValue(ValueName, false);
            using (var a = Registry.CurrentUser.OpenSubKey(ApprovedKey, true)) a?.DeleteValue(ValueName, false);
        }

        // jaga path tetap benar kalau aplikasi dipindah/diinstal ulang (tanpa mengubah status enable/disable)
        public static void RefreshPath()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true))
                    if (k?.GetValue(ValueName) is string s && s != Command) k.SetValue(ValueName, Command);
            }
            catch { }
        }
    }
}
