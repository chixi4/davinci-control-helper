# -*- coding: utf-8 -*-
import tkinter as tk
from tkinter import ttk, messagebox
import threading
import time
import ctypes
import ctypes.wintypes as wintypes
import win32api
import win32con
import win32gui
import win32process
import atexit
import json
import os
import sys

# ==================== 使用 SendInput API ====================
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
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

def send_mouse_event(flags, dx=0, dy=0, data=0):
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.ii.mi = MOUSEINPUT(dx, dy, data, flags, 0, ULONG_PTR(0))
    SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
# ==============================================================================

class Config:
    def __init__(self):
        self.config_file = "davinci_drag_config.json"
        self.load_config()
    
    def load_config(self):
        default_config = {
            "enabled": False,
            "left_mouse_id": r"\\?\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
            "right_mouse_id": r"\\?\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
            "drag_speed": 1.0,
            "idle_timeout": 20,
            "debug_log": False,
            "default_mouse_speed": 10,
            "speed_check_interval": 1.0,
            "target_process": "Resolve.exe"
        }
        
        try:
            if os.path.exists(self.config_file):
                with open(self.config_file, 'r', encoding='utf-8') as f:
                    loaded_config = json.load(f)
                    self.config = {**default_config, **loaded_config}
            else:
                self.config = default_config
        except Exception as e:
            print(f"加载配置失败: {e}")
            self.config = default_config
    
    def save_config(self):
        try:
            with open(self.config_file, 'w', encoding='utf-8') as f:
                json.dump(self.config, f, indent=2, ensure_ascii=False)
        except Exception as e:
            print(f"保存配置失败: {e}")

class MouseDetector:
    def __init__(self, callback=None, parent_window=None):
        self.callback = callback
        self.parent_window = parent_window
        self.running = False
        self.hwnd = None
        self.atom = None
        self.wc = None
        self.detected_ids = set()
        self.message_thread = None
        
        # Windows API 定义
        self.WM_INPUT = 0x00FF
        self.RIM_TYPEMOUSE = 0
        self.RID_INPUT = 0x10000003
        self.RIDEV_INPUTSINK = 0x00000100
        self.RIDI_DEVICENAME = 0x20000007
        
        self.setup_api_structures()
    
    def setup_api_structures(self):
        class RAWINPUTHEADER(ctypes.Structure):
            _fields_ = [("dwType", wintypes.DWORD), ("dwSize", wintypes.DWORD),
                       ("hDevice", wintypes.HANDLE), ("wParam", wintypes.WPARAM)]

        class RAWMOUSE(ctypes.Structure):
            _fields_ = [("usFlags", wintypes.USHORT), ("ulButtons", wintypes.ULONG),
                       ("usButtonFlags", wintypes.USHORT), ("usButtonData", wintypes.USHORT),
                       ("ulRawButtons", wintypes.ULONG), ("lLastX", ctypes.c_long),
                       ("lLastY", ctypes.c_long), ("ulExtraInformation", wintypes.ULONG)]

        class _RAWINPUT_DATA(ctypes.Union):
            _fields_ = [("mouse", RAWMOUSE), ("dummy", wintypes.BYTE * 1)]

        class RAWINPUT(ctypes.Structure):
            _fields_ = [("header", RAWINPUTHEADER), ("data", _RAWINPUT_DATA)]

        class RAWINPUTDEVICE(ctypes.Structure):
            _fields_ = [("usUsagePage", wintypes.USHORT), ("usUsage", wintypes.USHORT),
                       ("dwFlags", wintypes.DWORD), ("hwndTarget", wintypes.HWND)]
        
        self.RAWINPUTHEADER = RAWINPUTHEADER
        self.RAWMOUSE = RAWMOUSE
        self._RAWINPUT_DATA = _RAWINPUT_DATA
        self.RAWINPUT = RAWINPUT
        self.RAWINPUTDEVICE = RAWINPUTDEVICE
    
    def get_device_name(self, hdev):
        try:
            size = wintypes.UINT(0)
            result = ctypes.windll.user32.GetRawInputDeviceInfoW(hdev, self.RIDI_DEVICENAME, None, ctypes.byref(size))
            if size.value == 0 or result != 0:
                return None
            buf = ctypes.create_unicode_buffer(size.value)
            result = ctypes.windll.user32.GetRawInputDeviceInfoW(hdev, self.RIDI_DEVICENAME, buf, ctypes.byref(size))
            if result < 0:
                return None
            return buf.value
        except Exception as e:
            print(f"获取设备名称失败: {e}")
            return None
    
    def wnd_proc(self, hwnd, msg, wp, lp):
        try:
            if msg == self.WM_INPUT and self.running:
                sz = wintypes.UINT(0)
                result = ctypes.windll.user32.GetRawInputData(lp, self.RID_INPUT, None, ctypes.byref(sz), ctypes.sizeof(self.RAWINPUTHEADER))
                if result != 0 or sz.value == 0:
                    return win32gui.DefWindowProc(hwnd, msg, wp, lp)
                
                buf = ctypes.create_string_buffer(sz.value)
                result = ctypes.windll.user32.GetRawInputData(lp, self.RID_INPUT, buf, ctypes.byref(sz), ctypes.sizeof(self.RAWINPUTHEADER))
                if result != sz.value:
                    return win32gui.DefWindowProc(hwnd, msg, wp, lp)
                
                rin = ctypes.cast(buf, ctypes.POINTER(self.RAWINPUT)).contents
                
                if rin.header.dwType == self.RIM_TYPEMOUSE:
                    if rin.data.mouse.lLastX != 0 or rin.data.mouse.lLastY != 0:
                        device_name = self.get_device_name(rin.header.hDevice)
                        if device_name and device_name not in self.detected_ids:
                            self.detected_ids.add(device_name)
                            if self.callback and self.parent_window:
                                # 使用after方法确保在主线程中执行回调
                                self.parent_window.after(0, lambda: self.callback(device_name))
        except Exception as e:
            print(f"处理原始输入消息时出错: {e}")
                        
        return win32gui.DefWindowProc(hwnd, msg, wp, lp)
    
    def start_detection(self):
        if self.running:
            return False
        
        try:
            self.wc = win32gui.WNDCLASS()
            self.wc.lpszClassName = 'MouseDetector_' + str(id(self))
            self.wc.lpfnWndProc = self.wnd_proc
            self.wc.hInstance = win32api.GetModuleHandle(None)
            
            try:
                self.atom = win32gui.RegisterClass(self.wc)
            except Exception as e:
                print(f"注册窗口类失败: {e}")
                return False
                
            self.hwnd = win32gui.CreateWindow(
                self.atom, '', 0, 0, 0, 0, 0, 
                win32con.HWND_MESSAGE, 0, self.wc.hInstance, None
            )
            
            if not self.hwnd:
                print("创建消息窗口失败")
                return False
            
            dev = self.RAWINPUTDEVICE(1, 2, self.RIDEV_INPUTSINK, self.hwnd)
            if not ctypes.windll.user32.RegisterRawInputDevices(ctypes.byref(dev), 1, ctypes.sizeof(dev)):
                print("注册原始输入设备失败")
                return False
            
            self.running = True
            self.detected_ids.clear()
            
            # 启动消息循环线程
            self.message_thread = threading.Thread(target=self.message_loop, daemon=True)
            self.message_thread.start()
            
            return True
        except Exception as e:
            print(f"启动检测失败: {e}")
            self.cleanup()
            return False
    
    def stop_detection(self):
        if not self.running:
            return
        
        print("停止鼠标检测...")
        self.running = False
        self.cleanup()
    
    def cleanup(self):
        try:
            if self.hwnd:
                win32gui.DestroyWindow(self.hwnd)
                self.hwnd = None
        except Exception as e:
            print(f"销毁窗口失败: {e}")
        
        try:
            if self.atom and self.wc:
                win32gui.UnregisterClass(self.atom, self.wc.hInstance)
                self.atom = None
        except Exception as e:
            print(f"注销窗口类失败: {e}")
    
    def message_loop(self):
        try:
            while self.running:
                try:
                    win32gui.PumpWaitingMessages()
                    time.sleep(0.001)
                except Exception as e:
                    if self.running:  # 只有在仍在运行时才打印错误
                        print(f"消息循环错误: {e}")
                    break
        except Exception as e:
            print(f"消息循环异常: {e}")

class DavinciDragHelper:
    def __init__(self, config):
        self.config = config
        self.enabled = False
        self.running = False
        self.hwnd = None
        self.atom = None
        self.wc = None
        
        self.original_speed = None
        self.last_speed_check = 0
        self.speed_protection_interval = config.config['speed_check_interval']
        
        self.WM_INPUT = 0x00FF
        self.RID_INPUT = 0x10000003
        self.RIDEV_INPUTSINK = 0x00000100
        self.RIM_TYPEMOUSE = 0
        self.SPI_GETMOUSESPEED = 0x0070
        self.SPI_SETMOUSESPEED = 0x0071
        
        self.State = type('State', (), {'NORMAL': 1, 'DRAG': 2, 'WAIT_CONFIRM': 3})()
        self.current_state = self.State.NORMAL
        self.last_left_move_time = 0
        
        self.cumulative_dx = 0.0
        self.cumulative_dy = 0.0
        
        self.setup_api_structures()
    
    def setup_api_structures(self):
        class RAWINPUTDEVICE(ctypes.Structure):
            _fields_ = [("usUsagePage", wintypes.USHORT), ("usUsage", wintypes.USHORT),
                       ("dwFlags", wintypes.DWORD), ("hwndTarget", wintypes.HWND)]
        class RAWMOUSE(ctypes.Structure):
            _fields_ = [("usFlags", wintypes.USHORT), ("ulButtons", wintypes.ULONG),
                       ("usButtonFlags", wintypes.USHORT), ("usButtonData", wintypes.USHORT),
                       ("ulRawButtons", wintypes.ULONG), ("lLastX", wintypes.LONG),
                       ("lLastY", wintypes.LONG), ("ulExtraInformation", wintypes.ULONG)]
        class RAWINPUTHEADER(ctypes.Structure):
            _fields_ = [("dwType", wintypes.DWORD), ("dwSize", wintypes.DWORD),
                       ("hDevice", wintypes.HANDLE), ("wParam", wintypes.WPARAM)]
        class RAWINPUT_DATA(ctypes.Union):
            _fields_ = [("mouse", RAWMOUSE)]
        class RAWINPUT(ctypes.Structure):
            _fields_ = [("header", RAWINPUTHEADER), ("data", RAWINPUT_DATA)]
        self.RAWINPUTDEVICE = RAWINPUTDEVICE
        self.RAWMOUSE = RAWMOUSE
        self.RAWINPUTHEADER = RAWINPUTHEADER
        self.RAWINPUT_DATA = RAWINPUT_DATA
        self.RAWINPUT = RAWINPUT

    def get_pointer_speed(self):
        val = ctypes.c_int()
        ctypes.windll.user32.SystemParametersInfoW(self.SPI_GETMOUSESPEED, 0, ctypes.byref(val), 0)
        return val.value

    def set_pointer_speed(self, level):
        level = int(round(max(1, min(20, level))))
        ctypes.windll.user32.SystemParametersInfoW(self.SPI_SETMOUSESPEED, 0, level, 0)

    def reset_to_normal_state(self):
        if self.current_state != self.State.NORMAL:
            print(f"强制重置状态: {self.current_state} -> NORMAL")
            self.current_state = self.State.NORMAL
            if self.original_speed:
                self.set_pointer_speed(self.original_speed)
                print(f"恢复原始鼠标速度: {self.original_speed}")
            send_mouse_event(MOUSEEVENTF_LEFTUP)

    def ensure_speed_protection(self):
        current_time = time.time()
        if current_time - self.last_speed_check > self.speed_protection_interval:
            self.last_speed_check = current_time
            
            if self.current_state == self.State.NORMAL and self.original_speed:
                current_speed = self.get_pointer_speed()
                if current_speed != self.original_speed:
                    print(f"检测到速度异常 {current_speed}, 恢复到 {self.original_speed}")
                    self.set_pointer_speed(self.original_speed)
            
            if self.current_state != self.State.NORMAL:
                ap = self.get_active_process_name()
                target_proc = self.config.config.get('target_process', '')
                if not ap or not target_proc or not ap.lower().endswith(target_proc.lower()):
                    print("检测到离开目标应用，重置状态")
                    self.reset_to_normal_state()
    
    def get_device_name(self, hDevice):
        cb = wintypes.UINT()
        ctypes.windll.user32.GetRawInputDeviceInfoW(hDevice, 0x20000007, None, ctypes.byref(cb))
        if cb.value == 0: return None
        buf = ctypes.create_unicode_buffer(cb.value)
        ctypes.windll.user32.GetRawInputDeviceInfoW(hDevice, 0x20000007, buf, ctypes.byref(cb))
        return buf.value
    
    def get_active_process_name(self):
        try:
            hwnd = win32gui.GetForegroundWindow()
            if not hwnd: return None
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
            if not pid: return None
            h = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, 0, pid)
            name = win32process.GetModuleFileNameEx(h, 0)
            win32api.CloseHandle(h)
            return name
        except:
            return None
            
    def wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == self.WM_INPUT and self.enabled:
            sz = wintypes.UINT()
            ctypes.windll.user32.GetRawInputData(lparam, self.RID_INPUT, None, ctypes.byref(sz), ctypes.sizeof(self.RAWINPUTHEADER))
            buf = ctypes.create_string_buffer(sz.value)
            ctypes.windll.user32.GetRawInputData(lparam, self.RID_INPUT, buf, ctypes.byref(sz), ctypes.sizeof(self.RAWINPUTHEADER))
            ri = ctypes.cast(buf, ctypes.POINTER(self.RAWINPUT)).contents
            
            if ri.header.dwType != self.RIM_TYPEMOUSE: return 1
            dev_name = self.get_device_name(ri.header.hDevice)
            if not dev_name: return 1
            
            is_left = self.config.config['left_mouse_id'].lower() in dev_name.lower()
            is_right = self.config.config['right_mouse_id'].lower() in dev_name.lower()
            
            if self.current_state == self.State.NORMAL and is_left:
                ap = self.get_active_process_name()
                target_proc = self.config.config.get('target_process', '')
                if not ap or not target_proc or not ap.lower().endswith(target_proc.lower()): return 1

                print("左手鼠标移动，进入 DRAG 状态")
                self.set_pointer_speed(self.config.config['drag_speed'])
                self.current_state = self.State.DRAG
                self.last_left_move_time = time.time()
                self.cumulative_dx = self.cumulative_dy = 0.0
                send_mouse_event(MOUSEEVENTF_LEFTDOWN)
                return 0
            
            elif self.current_state == self.State.DRAG:
                if is_left:
                    self.last_left_move_time = time.time()
                    self.cumulative_dx += ri.data.mouse.lLastX
                    self.cumulative_dy += ri.data.mouse.lLastY
                    dx, dy = int(self.cumulative_dx), int(self.cumulative_dy)
                    if dx or dy:
                        send_mouse_event(MOUSEEVENTF_MOVE, dx, dy)
                        self.cumulative_dx -= dx
                        self.cumulative_dy -= dy
                    return 0
                elif is_right:
                    print("右手移动，结束拖拽，恢复 NORMAL 状态")
                    self.reset_to_normal_state()
                    return 0
            
            elif self.current_state == self.State.WAIT_CONFIRM:
                if is_left:
                    print("左手再次移动，恢复 DRAG 状态")
                    self.current_state = self.State.DRAG
                    self.last_left_move_time = time.time()
                    return 0
                elif is_right:
                    print("右手移动，确认结束拖拽，恢复 NORMAL 状态")
                    self.reset_to_normal_state()
                    return 0
        
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

    def state_monitor(self):
        while self.running:
            try:
                self.ensure_speed_protection()
                if self.current_state == self.State.DRAG:
                    if (time.time() - self.last_left_move_time) * 1000 > self.config.config['idle_timeout']:
                        print("拖拽超时，进入 WAIT_CONFIRM 状态")
                        self.current_state = self.State.WAIT_CONFIRM
                time.sleep(0.01)
            except Exception as e:
                print(f"状态监控异常: {e}")
                self.reset_to_normal_state()
    
    def message_loop(self):
        while self.running:
            try:
                win32gui.PumpWaitingMessages()
                time.sleep(0.001)
            except: break
    
    def enable(self):
        if self.enabled: return True
        try:
            self.original_speed = self.config.config['default_mouse_speed']
            current_sys_speed = self.get_pointer_speed()
            if current_sys_speed != self.original_speed:
                print(f"系统当前速度 {current_sys_speed} 与配置的默认速度 {self.original_speed} 不符，将以配置为准。")
            
            self.wc = win32gui.WNDCLASS()
            self.wc.lpfnWndProc = self.wnd_proc
            self.wc.lpszClassName = "DavinciDragHelper"
            self.wc.hInstance = win32api.GetModuleHandle(None)
            self.atom = win32gui.RegisterClass(self.wc)
            self.hwnd = win32gui.CreateWindow(self.atom, "DDH", 0, 0, 0, 0, 0, win32con.HWND_MESSAGE, None, self.wc.hInstance, None)
            
            rid = self.RAWINPUTDEVICE(1, 2, self.RIDEV_INPUTSINK, self.hwnd)
            if not ctypes.windll.user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(self.RAWINPUTDEVICE)): return False
            
            self.enabled = True
            self.running = True
            
            self.monitor_thread = threading.Thread(target=self.state_monitor, daemon=True)
            self.monitor_thread.start()
            
            self.message_thread = threading.Thread(target=self.message_loop, daemon=True)
            self.message_thread.start()
            return True
        except Exception as e:
            print(f"启用失败: {e}")
            return False
    
    def disable(self):
        if not self.enabled: return
        print("禁用达芬奇拖拽助手...")
        self.enabled = False
        self.running = False
        self.reset_to_normal_state()
        if self.hwnd:
            try: win32gui.DestroyWindow(self.hwnd)
            except Exception as e: print(f"销毁窗口失败: {e}")
        if self.atom and self.wc:
            try: win32gui.UnregisterClass(self.atom, self.wc.hInstance)
            except Exception as e: print(f"注销窗口类失败: {e}")

class DavinciDragGUI:
    def __init__(self):
        self.config = Config()
        self.helper = DavinciDragHelper(self.config)
        self.mouse_detector = None
        self.detection_active = False
        
        self.root = tk.Tk()
        self.root.title("达芬奇双鼠标拖拽助手 v3")
        self.root.resizable(False, False)
        
        atexit.register(self.cleanup)
        
        self.create_widgets()
        
        if self.config.config.get('enabled', False):
            self.enabled_var.set(True)
            self.toggle_helper()
    
    def create_widgets(self):
        main_frame = ttk.Frame(self.root, padding="15")
        main_frame.grid(row=0, column=0, sticky="nsew")
        
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        title_label = ttk.Label(main_frame, text="达芬奇双鼠标拖拽助手 v3", font=("Microsoft YaHei", 16, "bold"))
        title_label.grid(row=0, column=0, columnspan=2, pady=(0, 20))
        
        control_frame = ttk.LabelFrame(main_frame, text="控制", padding="10")
        control_frame.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(0, 15))
        
        self.enabled_var = tk.BooleanVar(value=self.config.config.get('enabled', False))
        enable_check = ttk.Checkbutton(control_frame, text="启用达芬奇双鼠标拖拽助手", variable=self.enabled_var, command=self.toggle_helper)
        enable_check.grid(row=0, column=0, sticky="w", pady=(0, 10))
        
        self.status_var = tk.StringVar(value="程序已启动")
        status_label = ttk.Label(control_frame, textvariable=self.status_var, foreground="blue")
        status_label.grid(row=1, column=0, sticky="w")
        
        config_frame = ttk.LabelFrame(main_frame, text="参数设置", padding="10")
        config_frame.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(0, 15))
        
        # 参数设置区域
        ttk.Label(config_frame, text="拖拽速度:").grid(row=0, column=0, sticky="w", pady=5, padx=5)
        self.speed_var = tk.StringVar(value=f"{self.config.config['drag_speed']:.1f}")
        speed_spin = ttk.Spinbox(
            config_frame, from_=1.0, to=20.0, increment=0.1, format="%.1f",
            textvariable=self.speed_var, width=15, command=self.update_config)
        speed_spin.grid(row=0, column=1, sticky="w", padx=5)
        speed_spin.bind('<KeyRelease>', lambda e: self.update_config())

        ttk.Label(config_frame, text="空闲超时 (ms):").grid(row=1, column=0, sticky="w", pady=5, padx=5)
        self.timeout_var = tk.IntVar(value=self.config.config['idle_timeout'])
        timeout_spin = ttk.Spinbox(
            config_frame, from_=10, to=1000, textvariable=self.timeout_var, width=15, command=self.update_config)
        timeout_spin.grid(row=1, column=1, sticky="w", padx=5)
        timeout_spin.bind('<KeyRelease>', lambda e: self.update_config())
        
        ttk.Label(config_frame, text="默认鼠标速度:").grid(row=2, column=0, sticky="w", pady=5, padx=5)
        self.default_speed_var = tk.IntVar(value=self.config.config['default_mouse_speed'])
        default_speed_spin = ttk.Spinbox(
            config_frame, from_=1, to=20, textvariable=self.default_speed_var, width=15, command=self.update_config)
        default_speed_spin.grid(row=2, column=1, sticky="w", padx=5)
        default_speed_spin.bind('<KeyRelease>', lambda e: self.update_config())
        
        ttk.Label(config_frame, text="速度检查 (秒):").grid(row=3, column=0, sticky="w", pady=5, padx=5)
        self.check_interval_var = tk.DoubleVar(value=self.config.config['speed_check_interval'])
        check_interval_spin = ttk.Spinbox(
            config_frame, from_=0.1, to=10.0, increment=0.1, format="%.1f",
            textvariable=self.check_interval_var, width=15, command=self.update_config)
        check_interval_spin.grid(row=3, column=1, sticky="w", padx=5)
        check_interval_spin.bind('<KeyRelease>', lambda e: self.update_config())
        
        # 检测鼠标ID按钮（放在鼠标ID框的上面）
        self.detect_btn = ttk.Button(main_frame, text="开始检测鼠标ID", command=self.detect_mouse_id)
        self.detect_btn.grid(row=3, column=0, columnspan=2, pady=(0, 10), sticky="ew")
        
        # 鼠标ID区域
        mouse_frame = ttk.LabelFrame(main_frame, text="鼠标设备 ID", padding="10")
        mouse_frame.grid(row=4, column=0, columnspan=2, sticky="ew", pady=(0, 15))
        
        # 左手鼠标ID区域
        left_id_frame = ttk.Frame(mouse_frame)
        left_id_frame.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 5))
        left_id_frame.columnconfigure(0, weight=1)
        
        ttk.Label(left_id_frame, text="左手鼠标 ID:").grid(row=0, column=0, sticky="w")
        left_apply_btn = ttk.Button(left_id_frame, text="应用", command=lambda: self.apply_mouse_id('left'))
        left_apply_btn.grid(row=0, column=1, sticky="e", padx=(10, 0))
        
        self.left_entry = tk.Text(mouse_frame, height=3, width=50, wrap=tk.WORD)
        self.left_entry.insert('1.0', self.config.config['left_mouse_id'])
        self.left_entry.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(0, 10))
        self.left_entry.bind('<KeyRelease>', lambda e: self.update_mouse_id('left', self.left_entry.get('1.0', tk.END).strip()))
        
        # 右手鼠标ID区域
        right_id_frame = ttk.Frame(mouse_frame)
        right_id_frame.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(0, 5))
        right_id_frame.columnconfigure(0, weight=1)
        
        ttk.Label(right_id_frame, text="右手鼠标 ID:").grid(row=0, column=0, sticky="w")
        right_apply_btn = ttk.Button(right_id_frame, text="应用", command=lambda: self.apply_mouse_id('right'))
        right_apply_btn.grid(row=0, column=1, sticky="e", padx=(10, 0))
        
        self.right_entry = tk.Text(mouse_frame, height=3, width=50, wrap=tk.WORD)
        self.right_entry.insert('1.0', self.config.config['right_mouse_id'])
        self.right_entry.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(0, 10))
        self.right_entry.bind('<KeyRelease>', lambda e: self.update_mouse_id('right', self.right_entry.get('1.0', tk.END).strip()))
        
        # --- 按钮布局优化 ---
        button_container = ttk.Frame(main_frame)
        button_container.grid(row=5, column=0, columnspan=2, pady=(10, 0), sticky="ew")

        buttons_info = [
            ("保存配置", self.save_config),
            ("重置配置", self.reset_config),
            ("帮助", self.show_help)
        ]

        # 配置容器的列权重，使按钮均匀分布
        for i in range(len(buttons_info)):
            button_container.columnconfigure(i, weight=1)

        for i, (text, command) in enumerate(buttons_info):
            btn = ttk.Button(button_container, text=text, command=command)
            # 使用grid布局，并设置sticky='ew'让按钮水平填充
            btn.grid(row=0, column=i, padx=5, sticky="ew")
        
        main_frame.columnconfigure(1, weight=1)
        mouse_frame.columnconfigure(0, weight=1)

    def detect_mouse_id(self):
        if self.detection_active:
            # 停止检测
            self.stop_detection()
        else:
            # 开始检测
            self.start_detection()
    
    def start_detection(self):
        try:
            if self.mouse_detector:
                self.mouse_detector.stop_detection()
                self.mouse_detector = None
            
            def on_mouse_detected(device_id):
                self.show_detected_mouse(device_id)
            
            self.mouse_detector = MouseDetector(callback=on_mouse_detected, parent_window=self.root)
            
            if self.mouse_detector.start_detection():
                self.detection_active = True
                self.detect_btn.config(text="停止检测")
                messagebox.showinfo("开始检测", "鼠标ID检测已开始！\n\n请移动您的鼠标设备，检测到的ID将会显示在弹窗中。")
            else:
                messagebox.showerror("检测失败", "无法启动鼠标检测，可能需要管理员权限")
                self.mouse_detector = None
        except Exception as e:
            messagebox.showerror("检测失败", f"启动检测时出错: {e}")
            self.mouse_detector = None
    
    def stop_detection(self):
        try:
            self.detection_active = False
            self.detect_btn.config(text="开始检测鼠标ID")
            
            if self.mouse_detector:
                self.mouse_detector.stop_detection()
                self.mouse_detector = None
            
            messagebox.showinfo("停止检测", "鼠标ID检测已停止")
        except Exception as e:
            print(f"停止检测时出错: {e}")
    
    def show_detected_mouse(self, device_id):
        try:
            # 创建一个对话框显示检测到的鼠标ID
            dialog = tk.Toplevel(self.root)
            dialog.title("检测到鼠标")
            dialog.geometry("600x300")
            dialog.resizable(True, True)
            dialog.transient(self.root)  # 设置为主窗口的子窗口
            dialog.grab_set()  # 模态对话框
            
            ttk.Label(dialog, text="检测到鼠标设备:", font=("Microsoft YaHei", 12, "bold")).pack(pady=10)
            
            text_widget = tk.Text(dialog, height=8, width=70, wrap=tk.WORD)
            text_widget.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
            text_widget.insert('1.0', device_id)
            text_widget.config(state=tk.DISABLED)  # 只读
            
            btn_frame = ttk.Frame(dialog)
            btn_frame.pack(pady=10)
            
            ttk.Button(btn_frame, text="设为左手鼠标", 
                      command=lambda: self.set_mouse_id('left', device_id, dialog)).pack(side=tk.LEFT, padx=5)
            ttk.Button(btn_frame, text="设为右手鼠标", 
                      command=lambda: self.set_mouse_id('right', device_id, dialog)).pack(side=tk.LEFT, padx=5)
            ttk.Button(btn_frame, text="关闭", command=dialog.destroy).pack(side=tk.LEFT, padx=5)
            
            # 居中显示对话框
            dialog.update_idletasks()
            x = (dialog.winfo_screenwidth() // 2) - (dialog.winfo_width() // 2)
            y = (dialog.winfo_screenheight() // 2) - (dialog.winfo_height() // 2)
            dialog.geometry(f"+{x}+{y}")
            
        except Exception as e:
            print(f"显示检测结果时出错: {e}")
            messagebox.showerror("显示错误", f"显示检测结果时出错: {e}")
    
    def set_mouse_id(self, side, device_id, dialog):
        try:
            if side == 'left':
                self.left_entry.delete('1.0', tk.END)
                self.left_entry.insert('1.0', device_id)
                self.config.config['left_mouse_id'] = device_id
            else:
                self.right_entry.delete('1.0', tk.END)
                self.right_entry.insert('1.0', device_id)
                self.config.config['right_mouse_id'] = device_id
            
            messagebox.showinfo("设置成功", f"已将此设备设置为{'左' if side == 'left' else '右'}手鼠标ID")
            dialog.destroy()
        except Exception as e:
            messagebox.showerror("设置失败", f"设置鼠标ID时出错: {e}")
    
    def apply_mouse_id(self, side):
        try:
            if side == 'left':
                device_id = self.left_entry.get('1.0', tk.END).strip()
                self.config.config['left_mouse_id'] = device_id
                messagebox.showinfo("应用成功", "左手鼠标ID已应用到配置")
            else:
                device_id = self.right_entry.get('1.0', tk.END).strip()
                self.config.config['right_mouse_id'] = device_id
                messagebox.showinfo("应用成功", "右手鼠标ID已应用到配置")
        except Exception as e:
            messagebox.showerror("应用失败", f"应用鼠标ID时出错: {e}")

    def toggle_helper(self):
        enabled = self.enabled_var.get()
        self.config.config['enabled'] = enabled
        
        if enabled:
            if self.helper.enable():
                self.status_var.set("✓ 达芬奇拖拽助手已启用")
            else:
                self.status_var.set("✗ 启用失败，可能需要管理员权限")
                self.enabled_var.set(False)
                messagebox.showwarning("启用失败", "无法注册原始输入设备。\n\n解决方案：\n1. 以管理员权限运行程序\n2. 检查鼠标设备ID是否正确")
        else:
            self.helper.disable()
            self.status_var.set("达芬奇拖拽助手已禁用")
    
    def update_config(self):
        try:
            self.config.config['drag_speed'] = float(self.speed_var.get())
            self.config.config['idle_timeout'] = self.timeout_var.get()
            self.config.config['default_mouse_speed'] = self.default_speed_var.get()
            self.config.config['speed_check_interval'] = self.check_interval_var.get()
        except (ValueError, tk.TclError):
            pass

        if hasattr(self, 'helper') and self.helper.enabled:
            self.helper.speed_protection_interval = self.config.config['speed_check_interval']
            if self.helper.current_state == self.helper.State.NORMAL:
                self.helper.original_speed = self.config.config['default_mouse_speed']

    def update_mouse_id(self, side, value):
        if side == 'left': self.config.config['left_mouse_id'] = value
        else: self.config.config['right_mouse_id'] = value
    
    def save_config(self):
        self.update_config()
        self.config.save_config()
        messagebox.showinfo("保存成功", "配置已保存")
    
    def reset_config(self):
        if messagebox.askyesno("确认重置", "确定要重置所有配置吗？"):
            self.helper.disable()
            if os.path.exists(self.config.config_file):
                os.remove(self.config.config_file)
            self.config.load_config()
            
            self.enabled_var.set(self.config.config['enabled'])
            self.speed_var.set(f"{self.config.config['drag_speed']:.1f}")
            self.timeout_var.set(self.config.config['idle_timeout'])
            self.default_speed_var.set(self.config.config['default_mouse_speed'])
            self.check_interval_var.set(self.config.config['speed_check_interval'])
            
            # 更新文本框内容
            self.left_entry.delete('1.0', tk.END)
            self.left_entry.insert('1.0', self.config.config['left_mouse_id'])
            self.right_entry.delete('1.0', tk.END)
            self.right_entry.insert('1.0', self.config.config['right_mouse_id'])
            
            self.status_var.set("配置已重置")
    
    def show_help(self):
        help_text = """达芬奇双鼠标拖拽助手 v3 (速度控制模式)

使用说明：
1. 勾选"启用"开关激活拖拽助手。
2. 在达芬奇软件中(Resolve.exe)，用【左手鼠标】开始移动，即可进入拖拽模式。
3. 此时系统鼠标速度会变为您设置的"拖拽速度"，继续使用【左手鼠标】进行拖拽操作。
4. 【右手鼠标】现在用于结束拖拽。移动一下右手鼠标，即可退出拖拽模式，系统鼠标速度将恢复。
5. 如果左手鼠标在拖拽中停止移动超过"空闲超时"，会进入等待确认状态。此时移动左手可继续拖拽，移动右手则结束拖拽。

新功能 v3：
• 检测鼠标ID：点击"开始检测鼠标ID"按钮后移动鼠标设备，自动检测并可直接设置为左手或右手鼠标
• 应用按钮：快速应用当前输入框中的鼠标ID设置
• 修复了检测功能的稳定性问题

核心逻辑：
• 本模式通过【修改系统指针速度】来实现拖拽。
• 左手鼠标：用于启动、进行和维持拖拽。
• 右手鼠标：仅用于确认结束拖拽。

参数说明：
• 拖拽速度：进入拖拽模式后的系统鼠标速度（1.0-20.0）。
• 空闲超时：拖拽停止多久后进入等待确认模式。
• 默认鼠标速度：程序恢复时应将系统速度设置回的值。
• 速度检查：后台检查并强制恢复速度的频率。
"""
        messagebox.showinfo("帮助", help_text)
    
    def cleanup(self):
        try:
            if hasattr(self, 'mouse_detector') and self.mouse_detector:
                self.mouse_detector.stop_detection()
            if hasattr(self, 'helper'):
                self.helper.disable()
        except Exception as e:
            print(f"清理资源时出错: {e}")
    
    def run(self):
        try:
            self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
            self.root.mainloop()
        except KeyboardInterrupt:
            self.cleanup()
    
    def on_closing(self):
        self.cleanup()
        self.root.destroy()

def main():
    try:
        app = DavinciDragGUI()
        app.run()
    except Exception as e:
        messagebox.showerror("启动失败", f"程序启动失败: {e}")

if __name__ == "__main__":
    main()