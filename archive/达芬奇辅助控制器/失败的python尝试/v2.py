# -*- coding: utf-8 -*-
import ctypes
import ctypes.wintypes as wintypes
import time
import win32gui
import win32api
import win32con
import atexit
import signal

# ==================== 可调参数 ====================

# 空闲超时 (秒): 在拖拽状态下，如果主鼠标（左手鼠标）停止移动超过这个时间，将自动抬起左键结束拖拽。
IDLE_TIMEOUT_SECONDS = 3.0

# 主循环休眠时间 (秒): 控制检查状态的频率，同时也影响CPU使用率。
# 较小的值响应更快，但CPU占用略高。0.01 对应每秒检查100次。
LOOP_SLEEP_INTERVAL = 0.01

# ==================== Windows API 定义 ====================

# 为 SendInput 定义的结构体和常量
if ctypes.sizeof(ctypes.c_void_p) == 8: ULONG_PTR = ctypes.c_uint64
else: ULONG_PTR = ctypes.c_uint32
class MOUSEINPUT(ctypes.Structure): _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]
class INPUT_Union(ctypes.Union): _fields_ = [("mi", MOUSEINPUT)]
class INPUT(ctypes.Structure): _fields_ = [("type", wintypes.DWORD), ("ii", INPUT_Union)]
INPUT_MOUSE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0, 0x0002, 0x0004
SendInput = ctypes.windll.user32.SendInput

# 为 Raw Input 定义的结构体和常量
class RAWINPUTDEVICE(ctypes.Structure): _fields_ = [("usUsagePage", wintypes.USHORT), ("usUsage", wintypes.USHORT), ("dwFlags", wintypes.DWORD), ("hwndTarget", wintypes.HWND)]
class RAWMOUSE(ctypes.Structure): _fields_ = [("usFlags", wintypes.USHORT), ("ulButtons", wintypes.ULONG), ("usButtonFlags", wintypes.USHORT), ("usButtonData", wintypes.USHORT), ("ulRawButtons", wintypes.ULONG), ("lLastX", wintypes.LONG), ("lLastY", wintypes.LONG), ("ulExtraInformation", wintypes.ULONG)]
class RAWINPUTHEADER(ctypes.Structure): _fields_ = [("dwType", wintypes.DWORD), ("dwSize", wintypes.DWORD), ("hDevice", wintypes.HANDLE), ("wParam", wintypes.WPARAM)]
class RAWINPUT_DATA(ctypes.Union): _fields_ = [("mouse", RAWMOUSE)]
class RAWINPUT(ctypes.Structure): _fields_ = [("header", RAWINPUTHEADER), ("data", RAWINPUT_DATA)]
WM_INPUT, RID_INPUT, RIDEV_INPUTSINK, RIM_TYPEMOUSE = 0x00FF, 0x10000003, 0x00000100, 0

# ==================== 核心功能类 ====================

class DualMouseDrag:
    def __init__(self):
        # 状态定义
        self.STATE_NORMAL = "NORMAL"
        self.STATE_DRAGGING = "DRAGGING"

        # 内部状态变量
        self.current_state = self.STATE_NORMAL
        self.dragging_mouse_handle = None
        self.last_move_time = 0
        self.release_requested = False  # [核心改进] 释放请求标志
        self._running = False

        # Win32 窗口相关
        self.hwnd, self.wc, self.atom = None, None, None
        
        # 确保在程序退出时执行清理
        atexit.register(self.disable)
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        print(f"\nReceived signal {signum}, shutting down...")
        # atexit 会处理 disable，这里直接退出即可
        exit(0)

    def _send_mouse_event(self, flags):
        inp = INPUT(type=INPUT_MOUSE, ii=INPUT_Union(mi=MOUSEINPUT(dwFlags=flags)))
        SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

    def reset_to_normal_state(self):
        """重置状态机，确保左键抬起，并清除所有状态标志。"""
        if self.current_state == self.STATE_DRAGGING:
            print("Resetting state to NORMAL, releasing left mouse button.")
            self._send_mouse_event(MOUSEEVENTF_LEFTUP)
        self.current_state = self.STATE_NORMAL
        self.dragging_mouse_handle = None
        self.release_requested = False

    def wnd_proc(self, hwnd, msg, wparam, lparam):
        """窗口消息处理函数，现在只负责更新状态，不直接执行动作。"""
        if msg == WM_INPUT:
            sz = wintypes.UINT()
            ctypes.windll.user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(sz), ctypes.sizeof(RAWINPUTHEADER))
            buf = ctypes.create_string_buffer(sz.value)
            ctypes.windll.user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(sz), ctypes.sizeof(RAWINPUTHEADER))
            ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents

            if ri.header.dwType != RIM_TYPEMOUSE or (ri.data.mouse.lLastX == 0 and ri.data.mouse.lLastY == 0):
                return 1

            device_handle = ri.header.hDevice

            # 状态：正常 -> 任意鼠标移动，开始拖拽
            if self.current_state == self.STATE_NORMAL:
                print(f"Drag started by mouse [handle: {device_handle}]")
                self.dragging_mouse_handle = device_handle
                self.current_state = self.STATE_DRAGGING
                self.last_move_time = time.time()
                self._send_mouse_event(MOUSEEVENTF_LEFTDOWN)

            # 状态：拖拽中
            elif self.current_state == self.STATE_DRAGGING:
                # 如果是主拖拽鼠标在移动，更新时间戳
                if device_handle == self.dragging_mouse_handle:
                    self.last_move_time = time.time()
                # 如果是另一个鼠标在移动，设置释放请求标志
                else:
                    # [核心改进] 只设置标志，不立即操作
                    if not self.release_requested:
                        print(f"Release requested by other mouse [handle: {device_handle}]")
                        self.release_requested = True
        
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

    def enable(self):
        """创建消息窗口并注册原始输入设备。"""
        if self._running: return
            
        print("Enabling dual mouse drag helper...")
        self.wc = win32gui.WNDCLASS()
        self.wc.lpfnWndProc = self.wnd_proc
        self.wc.lpszClassName = "DualMouseDragHelper"
        self.wc.hInstance = win32api.GetModuleHandle(None)
        
        try:
            self.atom = win32gui.RegisterClass(self.wc)
            self.hwnd = win32gui.CreateWindow(self.atom, "DDH_MESSAGE_ONLY", 0, 0, 0, 0, 0,
                                              win32con.HWND_MESSAGE, None, self.wc.hInstance, None)
        except win32gui.error as e:
            print(f"Error creating message window: {e}")
            return False

        rid = RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, self.hwnd)
        if not ctypes.windll.user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE)):
            print("Failed to register raw input device. Try running as administrator.")
            return False

        self._running = True
        print("Helper enabled successfully. Ready to use.")
        print("How to use: Move one mouse to press and hold left-click. Move a different mouse to release.")
        return True

    def disable(self):
        """清理资源，注销设备和窗口。"""
        if not self._running: return

        print("Disabling helper and cleaning up...")
        self._running = False
        self.reset_to_normal_state()

        rid = RAWINPUTDEVICE(1, 2, 0, None)
        ctypes.windll.user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE))
        
        if self.hwnd and win32gui.IsWindow(self.hwnd): win32gui.DestroyWindow(self.hwnd)
        if self.atom and self.wc: win32gui.UnregisterClass(self.atom, self.wc.hInstance)
        
        self.hwnd, self.atom, self.wc = None, None, None
        print("Cleanup complete.")

    def run(self):
        """启动助手并进入主循环，由主循环负责执行状态变更。"""
        if not self.enable():
            return

        print("Main loop started. Press Ctrl+C to exit.")
        while self._running:
            try:
                win32gui.PumpWaitingMessages()

                # [核心改进] 主循环检查状态标志并执行操作
                if self.release_requested:
                    self.reset_to_normal_state()
                
                # 检查空闲超时
                elif self.current_state == self.STATE_DRAGGING and (time.time() - self.last_move_time) > IDLE_TIMEOUT_SECONDS:
                    print(f"Drag timed out after {IDLE_TIMEOUT_SECONDS} seconds.")
                    self.reset_to_normal_state()

                time.sleep(LOOP_SLEEP_INTERVAL)
            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"An error occurred in the main loop: {e}")
                break
        
        self.disable()


if __name__ == "__main__":
    helper = DualMouseDrag()
    helper.run()