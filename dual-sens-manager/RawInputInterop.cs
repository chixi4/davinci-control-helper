using System;
using System.Runtime.InteropServices;

namespace DualSensManager
{
    internal static class RawInputInterop
    {
        internal const int RIM_TYPEMOUSE = 0;
        internal const int WM_INPUT = 0x00FF;
        internal const int WM_INPUT_DEVICE_CHANGE = 0x00FE;
        private const int RID_INPUT = 0x10000003;
        private const uint RIDEV_INPUTSINK = 0x00000100;
        private const uint RIDEV_DEVNOTIFY = 0x00002000;

        [StructLayout(LayoutKind.Sequential)]
        internal struct RAWINPUTDEVICE
        {
            public ushort UsagePage;
            public ushort Usage;
            public uint Flags;
            public IntPtr Target;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct RAWINPUTHEADER
        {
            public uint Type;
            public uint Size;
            public IntPtr Device;
            public IntPtr WParam;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct RAWMOUSE
        {
            public ushort Flags;
            public uint Buttons;
            public ushort ButtonFlags;
            public ushort ButtonData;
            public uint RawButtons;
            public int LastX;
            public int LastY;
            public uint ExtraInformation;
        }

        [StructLayout(LayoutKind.Explicit)]
        private struct RAWINPUTDATA
        {
            [FieldOffset(0)]
            public RAWMOUSE Mouse;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct RAWINPUT
        {
            public RAWINPUTHEADER Header;
            public RAWINPUTDATA Data;
        }

        [DllImport("User32.dll", SetLastError = true)]
        private static extern bool RegisterRawInputDevices([In] RAWINPUTDEVICE[] pRawInputDevices, uint uiNumDevices, uint cbSize);

        [DllImport("User32.dll", SetLastError = true)]
        private static extern uint GetRawInputData(IntPtr hRawInput, uint uiCommand, IntPtr pData, ref uint pcbSize, uint cbSizeHeader);

        internal static bool RegisterForMouseMessages(IntPtr handle)
        {
            var rid = new RAWINPUTDEVICE
            {
                UsagePage = 0x01,
                Usage = 0x02,
                Flags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                Target = handle
            };

            return RegisterRawInputDevices(new[] { rid }, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)));
        }

        internal static bool TryReadMouseInput(IntPtr lParam, out IntPtr deviceHandle, out int deltaX, out int deltaY)
        {
            deviceHandle = IntPtr.Zero;
            deltaX = 0;
            deltaY = 0;

            uint size = 0;
            if (GetRawInputData(lParam, RID_INPUT, IntPtr.Zero, ref size, (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER))) == 0 && size > 0)
            {
                IntPtr buffer = Marshal.AllocHGlobal((int)size);
                try
                {
                    if (GetRawInputData(lParam, RID_INPUT, buffer, ref size, (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER))) == size)
                    {
                        var raw = Marshal.PtrToStructure<RAWINPUT>(buffer);
                        if (raw.Header.Type == RIM_TYPEMOUSE)
                        {
                            deviceHandle = raw.Header.Device;
                            deltaX = raw.Data.Mouse.LastX;
                            deltaY = raw.Data.Mouse.LastY;
                            return true;
                        }
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
            }

            return false;
        }
    }
}
