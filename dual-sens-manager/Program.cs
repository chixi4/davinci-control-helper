using System;
using System.Windows.Forms;
using System.Threading;
using System.Runtime.InteropServices;

namespace DualSensManager
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            using (var singleMutex = new Mutex(initiallyOwned: true, name: "Global/DualSensManager_SingleInstance", out bool createdNew))
            {
                if (!createdNew)
                {
                    TryActivateExistingWindow();
                    return;
                }

                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm());
            }
        }

        private static void TryActivateExistingWindow()
        {
            // 通过窗口标题尝试找到已运行实例并激活到前台
            const string title = "双鼠标灵敏度管理";
            IntPtr hWnd = FindWindowW(null, title);
            if (hWnd != IntPtr.Zero)
            {
                if (IsIconic(hWnd))
                {
                    ShowWindow(hWnd, SW_RESTORE);
                }
                SetForegroundWindow(hWnd);
                return;
            }

            MessageBox.Show("程序已在运行。", title, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private const int SW_RESTORE = 9;

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool IsIconic(IntPtr hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    }
}
