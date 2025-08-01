# 文件名: show_my_mouse_id.py
# 功能: 移动鼠标时，在控制台打印出该鼠标的设备ID。

import ctypes
import ctypes.wintypes as wt
import win32gui, win32con, win32api
import sys

# --- Windows API 定义 ---
WM_INPUT = 0x00FF
RIM_TYPEMOUSE = 0
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIDI_DEVICENAME = 0x20000007

# --- ctypes 结构体定义 ---
class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]

class RAWMOUSE(ctypes.Structure):
    _fields_ = [("usFlags", wt.USHORT), ("ulButtons", wt.ULONG),
                ("usButtonFlags", wt.USHORT), ("usButtonData", wt.USHORT),
                ("ulRawButtons", wt.ULONG), ("lLastX", ctypes.c_long),
                ("lLastY", ctypes.c_long), ("ulExtraInformation", wt.ULONG)]

class _RAWINPUT_DATA(ctypes.Union):
    _fields_ = [("mouse", RAWMOUSE), ("dummy", wt.BYTE * 1)]

class RAWINPUT(ctypes.Structure):
    _fields_ = [("header", RAWINPUTHEADER), ("data", _RAWINPUT_DATA)]

class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# 用一个集合来存储已经打印过的设备ID，避免刷屏
printed_ids = set()

def get_device_name(hdev):
    """根据设备句柄获取设备ID字符串"""
    size = wt.UINT(0)
    user32.GetRawInputDeviceInfoW(hdev, RIDI_DEVICENAME, None, ctypes.byref(size))
    if size.value == 0:
        return None
    buf = ctypes.create_unicode_buffer(size.value)
    user32.GetRawInputDeviceInfoW(hdev, RIDI_DEVICENAME, buf, ctypes.byref(size))
    return buf.value

def wnd_proc(hwnd, msg, wp, lp):
    """窗口消息处理函数"""
    if msg == WM_INPUT:
        # 1. 获取原始输入数据的大小
        sz = wt.UINT(0)
        user32.GetRawInputData(lp, RID_INPUT, None, ctypes.byref(sz), ctypes.sizeof(RAWINPUTHEADER))
        
        # 2. 分配缓冲区并获取数据
        buf = ctypes.create_string_buffer(sz.value)
        user32.GetRawInputData(lp, RID_INPUT, buf, ctypes.byref(sz), ctypes.sizeof(RAWINPUTHEADER))
        
        # 3. 将缓冲区转换为 RAWINPUT 结构体
        rin = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
        
        # 4. 判断是否为鼠标输入，并且有实际移动
        if rin.header.dwType == RIM_TYPEMOUSE:
            if rin.data.mouse.lLastX != 0 or rin.data.mouse.lLastY != 0:
                device_name = get_device_name(rin.header.hDevice)
                # 5. 如果是新的设备ID，则打印并记录
                if device_name and device_name not in printed_ids:
                    print(f"检测到鼠标移动! ID: {device_name}\n")
                    printed_ids.add(device_name)
                    
    return win32gui.DefWindowProc(hwnd, msg, wp, lp)

def main():
    """主程序"""
    # 创建一个不可见的消息专用窗口
    wc = win32gui.WNDCLASS()
    wc.lpszClassName = 'MouseIdentifier'
    wc.lpfnWndProc = wnd_proc
    wc.hInstance = win32api.GetModuleHandle(None)
    atom = win32gui.RegisterClass(wc)
    hwnd = win32gui.CreateWindow(atom, '', 0, 0, 0, 0, 0, win32con.HWND_MESSAGE, 0, wc.hInstance, None)
    
    # 注册以接收原始鼠标输入
    dev = RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, hwnd) # UsagePage=1(Generic Desktop), Usage=2(Mouse)
    if not user32.RegisterRawInputDevices(ctypes.byref(dev), 1, ctypes.sizeof(dev)):
        print('注册 RawInput 失败，请检查程序权限或系统设置。')
        sys.exit(1)
        
    print("="*60)
    print("鼠标ID识别脚本正在运行...")
    print("请移动你的物理鼠标，其设备ID将会显示在下方。")
    print("你可以逐个移动你的设备（鼠标、触摸板等）来识别它们。")
    print("按 Ctrl+C 键退出程序。")
    print("="*60)

    try:
        # 开始消息循环，等待输入事件
        win32gui.PumpMessages()
    except KeyboardInterrupt:
        print("\n程序退出。")
    finally:
        # 清理资源
        win32gui.DestroyWindow(hwnd)
        win32gui.UnregisterClass(atom, wc.hInstance)

if __name__ == '__main__':
    main()