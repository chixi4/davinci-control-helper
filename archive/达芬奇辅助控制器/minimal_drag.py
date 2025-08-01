# -*- coding: utf-8 -*-
"""
精简双鼠标拖拽助手
左鼠标移动：按下左键开始拖拽
右鼠标移动：抬起左键结束拖拽
"""
import ctypes
import ctypes.wintypes as wintypes
import time
import win32api
import win32con
import win32gui
import win32process

# === 可调参数 ===
LEFT_MOUSE_ID = r"\?\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"
RIGHT_MOUSE_ID = r"\?\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"
TARGET_PROCESS = "Resolve.exe"
IDLE_TIMEOUT_MS = 20  # 拖拽空闲超时毫秒
POLL_INTERVAL_SEC = 0.1  # 状态轮询间隔秒

# === SendInput 结构定义 ===
if ctypes.sizeof(ctypes.c_void_p) == 8:
    ULONG_PTR = ctypes.c_uint64
else:
    ULONG_PTR = ctypes.c_uint32

class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]

class INPUT_Union(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT)]

class INPUT(ctypes.Structure):
    _fields_ = [
        ("type", wintypes.DWORD),
        ("ii", INPUT_Union),
    ]

SendInput = ctypes.windll.user32.SendInput
INPUT_MOUSE = 0
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

def send_mouse_event(flags):
    """发送鼠标事件"""
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.ii.mi = MOUSEINPUT(0, 0, 0, flags, 0, ULONG_PTR(0))
    SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

# === RAW Input 结构定义 ===
class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [
        ("usUsagePage", wintypes.USHORT),
        ("usUsage", wintypes.USHORT),
        ("dwFlags", wintypes.DWORD),
        ("hwndTarget", wintypes.HWND)
    ]

class RAWMOUSE(ctypes.Structure):
    _fields_ = [
        ("usFlags", wintypes.USHORT),
        ("ulButtons", wintypes.ULONG),
        ("usButtonFlags", wintypes.USHORT),
        ("usButtonData", wintypes.USHORT),
        ("ulRawButtons", wintypes.ULONG),
        ("lLastX", wintypes.LONG),
        ("lLastY", wintypes.LONG),
        ("ulExtraInformation", wintypes.ULONG)
    ]

class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [
        ("dwType", wintypes.DWORD),
        ("dwSize", wintypes.DWORD),
        ("hDevice", wintypes.HANDLE),
        ("wParam", wintypes.WPARAM)
    ]

class RAWINPUT_DATA(ctypes.Union):
    _fields_ = [("mouse", RAWMOUSE)]

class RAWINPUT(ctypes.Structure):
    _fields_ = [
        ("header", RAWINPUTHEADER),
        ("data", RAWINPUT_DATA)
    ]

class DragHelper:
    """双鼠标拖拽助手核心类"""
    
    # 常量
    WM_INPUT = 0x00FF
    RID_INPUT = 0x10000003
    RIDEV_INPUTSINK = 0x00000100
    RIDEV_NOLEGACY = 0x00000030
    RIM_TYPEMOUSE = 0
    
    # 状态
    NORMAL = 1
    DRAG = 2
    WAIT_CONFIRM = 3
    
    def __init__(self):
        self.enabled = False
        self.running = False
        self.hwnd = None
        self.atom = None
        self.wc = None
        self.current_state = self.NORMAL
        self.last_left_move_time = 0
        self.last_poll_time = 0
    
    def get_device_name(self, hDevice):
        """获取设备名称"""
        cb = wintypes.UINT()
        ctypes.windll.user32.GetRawInputDeviceInfoW(hDevice, 0x20000007, None, ctypes.byref(cb))
        if cb.value == 0:
            return None
        buf = ctypes.create_unicode_buffer(cb.value)
        ctypes.windll.user32.GetRawInputDeviceInfoW(hDevice, 0x20000007, buf, ctypes.byref(cb))
        return buf.value
    
    def get_active_process_name(self):
        """获取当前活动进程名"""
        try:
            hwnd = win32gui.GetForegroundWindow()
            if not hwnd:
                return None
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
            if not pid:
                return None
            h = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, 0, pid)
            name = win32process.GetModuleFileNameEx(h, 0)
            win32api.CloseHandle(h)
            return name
        except:
            return None
    
    def reset_to_normal(self):
        """重置到正常状态"""
        if self.current_state != self.NORMAL:
            send_mouse_event(MOUSEEVENTF_LEFTUP)
            self.current_state = self.NORMAL
            print("状态重置为NORMAL")
    
    def ensure_state_guard(self):
        """状态监控"""
        current_time = time.time()
        if current_time - self.last_poll_time > POLL_INTERVAL_SEC:
            self.last_poll_time = current_time
            
            # 离开目标进程时复位
            if self.current_state != self.NORMAL:
                current_process = self.get_active_process_name()
                if not current_process or not current_process.lower().endswith(TARGET_PROCESS.lower()):
                    print(f"离开目标进程，重置状态")
                    self.reset_to_normal()
            
            # 拖拽超时检查
            if self.current_state == self.DRAG:
                if (time.time() - self.last_left_move_time) * 1000 > IDLE_TIMEOUT_MS:
                    self.current_state = self.WAIT_CONFIRM
                    print("拖拽空闲超时，进入等待确认状态")
    
    def wnd_proc(self, hwnd, msg, wparam, lparam):
        """窗口消息处理"""
        if msg == self.WM_INPUT and self.enabled:
            # 读取RAW输入数据
            sz = wintypes.UINT()
            ctypes.windll.user32.GetRawInputData(lparam, self.RID_INPUT, None, ctypes.byref(sz), ctypes.sizeof(RAWINPUTHEADER))
            buf = ctypes.create_string_buffer(sz.value)
            ctypes.windll.user32.GetRawInputData(lparam, self.RID_INPUT, buf, ctypes.byref(sz), ctypes.sizeof(RAWINPUTHEADER))
            ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
            
            if ri.header.dwType != self.RIM_TYPEMOUSE:
                return 1
            
            dev_name = self.get_device_name(ri.header.hDevice)
            if not dev_name:
                return 1
            
            is_left = LEFT_MOUSE_ID.lower() in dev_name.lower()
            is_right = RIGHT_MOUSE_ID.lower() in dev_name.lower()
            
            # 状态机处理
            if self.current_state == self.NORMAL and is_left:
                # 检查是否在目标进程中
                current_process = self.get_active_process_name()
                if not current_process or not current_process.lower().endswith(TARGET_PROCESS.lower()):
                    return 1
                
                self.current_state = self.DRAG
                self.last_left_move_time = time.time()
                send_mouse_event(MOUSEEVENTF_LEFTDOWN)
                print("左鼠标移动，开始拖拽")
                return 0
            
            elif self.current_state == self.DRAG:
                if is_left:
                    self.last_left_move_time = time.time()
                    return 0
                elif is_right:
                    return 0
            
            elif self.current_state == self.WAIT_CONFIRM:
                if is_left:
                    self.current_state = self.DRAG
                    self.last_left_move_time = time.time()
                    print("左鼠标继续移动，恢复拖拽")
                    return 0
                elif is_right:
                    self.current_state = self.NORMAL
                    send_mouse_event(MOUSEEVENTF_LEFTUP)
                    print("右鼠标移动，结束拖拽")
                    return 0
        
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)
    
    def enable(self):
        """启用拖拽助手"""
        if self.enabled:
            return True
        
        try:
            # 创建消息窗口
            self.wc = win32gui.WNDCLASS()
            self.wc.lpfnWndProc = self.wnd_proc
            self.wc.lpszClassName = "MinimalDragHelper"
            self.wc.hInstance = win32api.GetModuleHandle(None)
            self.atom = win32gui.RegisterClass(self.wc)
            self.hwnd = win32gui.CreateWindow(
                self.atom, "MDH", 0, 0, 0, 0, 0,
                win32con.HWND_MESSAGE, None, self.wc.hInstance, None
            )
            
            # 注册RAW输入设备
            rid = RAWINPUTDEVICE(1, 2, self.RIDEV_INPUTSINK | self.RIDEV_NOLEGACY, self.hwnd)
            if not ctypes.windll.user32.RegisterRawInputDevices(
                ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE)
            ):
                print("注册RAW输入设备失败")
                return False
            
            self.enabled = True
            self.running = True
            print("双鼠标拖拽助手已启用")
            return True
            
        except Exception as e:
            print(f"启用失败: {e}")
            return False
    
    def disable(self):
        """禁用拖拽助手"""
        if not self.enabled:
            return
        
        print("禁用双鼠标拖拽助手")
        self.enabled = False
        self.running = False
        
        # 确保左键抬起
        try:
            send_mouse_event(MOUSEEVENTF_LEFTUP)
        except:
            pass
        
        self.current_state = self.NORMAL
        
        # 清理资源
        if self.hwnd:
            try:
                win32gui.DestroyWindow(self.hwnd)
            except:
                pass
        if self.atom and self.wc:
            try:
                win32gui.UnregisterClass(self.atom, self.wc.hInstance)
            except:
                pass
        
        self.hwnd = self.atom = self.wc = None
    
    def run_loop(self):
        """主循环"""
        try:
            while self.running:
                # 处理Windows消息
                win32gui.PumpWaitingMessages()
                
                # 状态监控
                self.ensure_state_guard()
                
                # 短暂休眠避免CPU占用过高
                time.sleep(0.01)
                
        except KeyboardInterrupt:
            print("收到中断信号")
        finally:
            self.disable()

def main():
    """主函数"""
    print("=== 精简双鼠标拖拽助手 ===")
    print(f"目标进程: {TARGET_PROCESS}")
    print(f"空闲超时: {IDLE_TIMEOUT_MS}ms")
    print(f"轮询间隔: {POLL_INTERVAL_SEC}s")
    print("按 Ctrl+C 退出")
    
    helper = DragHelper()
    
    if not helper.enable():
        print("启用失败，可能需要管理员权限")
        return
    
    try:
        helper.run_loop()
    except Exception as e:
        print(f"运行异常: {e}")
    finally:
        helper.disable()

if __name__ == "__main__":
    main()