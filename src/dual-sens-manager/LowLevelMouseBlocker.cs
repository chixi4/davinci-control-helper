using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace DualSensManager
{
    internal sealed class LowLevelMouseBlocker : IDisposable
    {
        private const int WH_MOUSE_LL = 14;
        private const uint LLMHF_INJECTED = 0x00000001;

        private readonly Func<bool> shouldBlock;
        private readonly HookProc hookProc;
        private readonly object gate = new object();
        private IntPtr hookHandle = IntPtr.Zero;

        public LowLevelMouseBlocker(Func<bool> shouldBlock)
        {
            this.shouldBlock = shouldBlock ?? (() => false);
            hookProc = HookCallback;
        }

        public bool Start()
        {
            lock (gate)
            {
                if (hookHandle != IntPtr.Zero)
                {
                    return true;
                }

                IntPtr moduleHandle = IntPtr.Zero;
                try
                {
                    using (var currentProcess = Process.GetCurrentProcess())
                    using (var currentModule = currentProcess.MainModule)
                    {
                        moduleHandle = GetModuleHandle(currentModule.ModuleName);
                    }
                }
                catch
                {
                    moduleHandle = GetModuleHandle(null);
                }

                hookHandle = SetWindowsHookEx(WH_MOUSE_LL, hookProc, moduleHandle, 0);
                return hookHandle != IntPtr.Zero;
            }
        }

        public void Stop()
        {
            lock (gate)
            {
                if (hookHandle == IntPtr.Zero)
                {
                    return;
                }

                UnhookWindowsHookEx(hookHandle);
                hookHandle = IntPtr.Zero;
            }
        }

        public void Dispose()
        {
            Stop();
            GC.SuppressFinalize(this);
        }

        private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0 && shouldBlock())
            {
                var data = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
                if ((data.flags & LLMHF_INJECTED) == 0)
                {
                    return new IntPtr(1);
                }
            }

            return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
        }

        private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential)]
        private struct MSLLHOOKSTRUCT
        {
            public POINT pt;
            public uint mouseData;
            public uint flags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int X;
            public int Y;
        }

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        private static extern IntPtr SetWindowsHookEx(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool UnhookWindowsHookEx(IntPtr hhk);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string lpModuleName);
    }
}
