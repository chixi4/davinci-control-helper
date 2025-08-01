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

# -------- SendInput 结构与常量（替代 mouse_event，仅用于注入按键） --------
# 参考：Microsoft Learn: mouse_event 已被 SendInput 取代；使用 MOUSEEVENTF_LEFTDOWN/LEFTUP。
# https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mouse_event
# https://learn.microsoft.com/en-us/windows/win32/inputdev/mouse-input-functions

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
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010

def send_mouse_event(flags, dx=0, dy=0, data=0):
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.ii.mi = MOUSEINPUT(dx, dy, data, flags, 0, ULONG_PTR(0))
    if SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT)) != 1:
        # 失败时不抛异常，避免影响流程；必要时可打印 GetLastError
        pass

# -------- 配置 --------
class Config:
    def __init__(self):
        self.config_file = "davinci_drag_config.json"
        self.load_config()
    
    def load_config(self):
        # 注意：drag_speed/默认鼠标速度已不再用于“改系统速度”
        default_config = {
            "enabled": False,
            "left_mouse_id": r"\\?\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
            "right_mouse_id": r"\\?\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
            "drag_speed": 1,                # <已停用：不再改系统速度，仅保留 UI
            "idle_timeout": 20,             # 毫秒
            "debug_log": False,
            "default_mouse_speed": 10,      # <已停用：不再自动改系统速度
            "speed_check_interval": 1.0,    # <已简化为状态轮询周期
            "target_process": "Resolve.exe"
        }
        try:
            if os.path.exists(self.config_file):
                with open(self.config_file, 'r', encoding='utf-8') as f:
                    loaded = json.load(f)
                    self.config = {**default_config, **loaded}
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

# -------- 核心助手 --------
class DavinciDragHelper:
    def __init__(self, config, gui_callback=None):
        self.config = config
        self.gui_callback = gui_callback
        self.enabled = False
        self.running = False
        self.hwnd = None
        self.atom = None
        self.wc = None
        self.last_speed_check = 0
        self.poll_interval = config.config['speed_check_interval']  # 用作状态轮询

        # RAW Input / 消息常量
        self.WM_INPUT = 0x00FF
        self.RID_INPUT = 0x10000003
        self.RIDEV_INPUTSINK = 0x00000100  # 后台也能收到 WM_INPUT
        self.RIDEV_NOLEGACY = 0x00000030   # 避免本窗口收到传统鼠标消息（不影响系统移动指针）
        self.RIM_TYPEMOUSE = 0

        # 状态机
        self.State = type('State', (), {'NORMAL': 1, 'DRAG': 2, 'WAIT_CONFIRM': 3})()
        self.current_state = self.State.NORMAL
        self.last_left_move_time = 0

        # 结构体定义
        self.setup_api_structures()
    
    def setup_api_structures(self):
        class RAWINPUTDEVICE(ctypes.Structure):
            _fields_ = [("usUsagePage", wintypes.USHORT),
                        ("usUsage", wintypes.USHORT),
                        ("dwFlags", wintypes.DWORD),
                        ("hwndTarget", wintypes.HWND)]
        
        class RAWMOUSE(ctypes.Structure):
            _fields_ = [("usFlags", wintypes.USHORT),
                        ("ulButtons", wintypes.ULONG),
                        ("usButtonFlags", wintypes.USHORT),
                        ("usButtonData", wintypes.USHORT),
                        ("ulRawButtons", wintypes.ULONG),
                        ("lLastX", wintypes.LONG),
                        ("lLastY", wintypes.LONG),
                        ("ulExtraInformation", wintypes.ULONG)]
        
        class RAWINPUTHEADER(ctypes.Structure):
            _fields_ = [("dwType", wintypes.DWORD),
                        ("dwSize", wintypes.DWORD),
                        ("hDevice", wintypes.HANDLE),
                        ("wParam", wintypes.WPARAM)]
        
        class RAWINPUT_DATA(ctypes.Union):
            _fields_ = [("mouse", RAWMOUSE)]
        
        class RAWINPUT(ctypes.Structure):
            _fields_ = [("header", RAWINPUTHEADER),
                        ("data", RAWINPUT_DATA)]
        
        self.RAWINPUTDEVICE = RAWINPUTDEVICE
        self.RAWMOUSE = RAWMOUSE
        self.RAWINPUTHEADER = RAWINPUTHEADER
        self.RAWINPUT_DATA = RAWINPUT_DATA
        self.RAWINPUT = RAWINPUT

    # --------- 实用方法 ---------
    def get_device_name(self, hDevice):
        cb = wintypes.UINT()
        ctypes.windll.user32.GetRawInputDeviceInfoW(hDevice, 0x20000007, None, ctypes.byref(cb))
        if cb.value == 0:
            return None
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

    def ensure_state_guard(self):
        """轮询：离开目标应用/超时，收尾并复位。"""
        current_time = time.time()
        if current_time - self.last_speed_check > self.poll_interval:
            self.last_speed_check = current_time

            # 离开目标进程就强制收尾
            if self.current_state != self.State.NORMAL:
                current_process = self.get_active_process_name()
                if not current_process or not current_process.lower().endswith(self.config.config['target_process'].lower()):
                    self.reset_to_normal_state()

            # 拖拽空闲超时 -> 等待确认
            if self.current_state == self.State.DRAG:
                if (time.time() - self.last_left_move_time) * 1000 > self.config.config['idle_timeout']:
                    self.current_state = self.State.WAIT_CONFIRM

    def reset_to_normal_state(self):
        """强制回到 NORMAL，并确保左键已抬起"""
        if self.current_state != self.State.NORMAL:
            # 保险起见，抬起一次左键
            send_mouse_event(MOUSEEVENTF_LEFTUP)
            self.current_state = self.State.NORMAL

    # --------- 窗口过程 ---------
    def wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == self.WM_INPUT and self.enabled:
            # 读取 RAWINPUT
            sz = wintypes.UINT()
            ctypes.windll.user32.GetRawInputData(lparam, self.RID_INPUT, None, ctypes.byref(sz), ctypes.sizeof(self.RAWINPUTHEADER))
            buf = ctypes.create_string_buffer(sz.value)
            ctypes.windll.user32.GetRawInputData(lparam, self.RID_INPUT, buf, ctypes.byref(sz), ctypes.sizeof(self.RAWINPUTHEADER))
            ri = ctypes.cast(buf, ctypes.POINTER(self.RAWINPUT)).contents

            if ri.header.dwType != self.RIM_TYPEMOUSE:
                return 1

            dev_name = self.get_device_name(ri.header.hDevice)
            if not dev_name:
                return 1

            # 若在“获取鼠标ID”界面，透传检测结果
            if self.gui_callback and hasattr(self.gui_callback, 'detection_running') and self.gui_callback.detection_running:
                if ri.data.mouse.lLastX != 0 or ri.data.mouse.lLastY != 0:
                    try:
                        import queue
                        if not hasattr(self.gui_callback, 'detection_queue'):
                            self.gui_callback.detection_queue = queue.Queue()
                        self.gui_callback.detection_queue.put(dev_name)
                    except:
                        pass

            is_left = self.config.config['left_mouse_id'].lower() in dev_name.lower()
            is_right = self.config.config['right_mouse_id'].lower() in dev_name.lower()

            # NORMAL：左手一动 -> 进入拖拽并按下左键（不重放移动）
            if self.current_state == self.State.NORMAL and is_left:
                current_process = self.get_active_process_name()
                if not current_process or not current_process.lower().endswith(self.config.config['target_process'].lower()):
                    return 1
                self.current_state = self.State.DRAG
                self.last_left_move_time = time.time()
                send_mouse_event(MOUSEEVENTF_LEFTDOWN)
                return 0

            # DRAG：只更新时间/不注入移动；右手移动不做处理
            elif self.current_state == self.State.DRAG:
                if is_left:
                    self.last_left_move_time = time.time()
                    return 0
                elif is_right:
                    # 忽略右手移动，避免干扰（RAW 无法“阻止”系统输入，这里只是不处理）
                    return 0

            # WAIT_CONFIRM：左手再动 -> 恢复 DRAG；右手动 -> 结束拖拽
            elif self.current_state == self.State.WAIT_CONFIRM:
                if is_left:
                    self.current_state = self.State.DRAG
                    self.last_left_move_time = time.time()
                    return 0
                elif is_right:
                    self.current_state = self.State.NORMAL
                    send_mouse_event(MOUSEEVENTF_LEFTUP)
                    return 0

        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

    # --------- 定时器循环 ---------
    def state_monitor(self):
        if not self.running: return
        try:
            self.ensure_state_guard()
        except Exception as e:
            print(f"状态监控异常: {e}")
            try: self.reset_to_normal_state()
            except: pass
        if self.running and hasattr(self, 'gui_callback') and self.gui_callback:
            try: self.gui_callback.root.after(100, self.state_monitor)
            except: pass

    def message_loop(self):
        if not self.running: return
        try:
            bRet = ctypes.windll.user32.PeekMessageW(None, None, 0, 0, 0)
            if bRet != 0:
                win32gui.PumpWaitingMessages()
        except Exception as e:
            print(f"主消息循环异常: {e}")
        if self.running and hasattr(self, 'gui_callback') and self.gui_callback:
            try: self.gui_callback.root.after(10, self.message_loop)
            except: pass

    # --------- 启用/禁用 ---------
    def enable(self):
        if self.enabled: return True
        try:
            # 注册消息窗口
            self.wc = win32gui.WNDCLASS()
            self.wc.lpfnWndProc = self.wnd_proc
            self.wc.lpszClassName = "DavinciDragHelper"
            self.wc.hInstance = win32api.GetModuleHandle(None)
            self.atom = win32gui.RegisterClass(self.wc)
            self.hwnd = win32gui.CreateWindow(
                self.atom, "DDH", 0, 0, 0, 0, 0,
                win32con.HWND_MESSAGE, None, self.wc.hInstance, None
            )

            # 注册 RAW：鼠标（UsagePage=1, Usage=2），后台接收 + 不给本窗口发传统鼠标消息
            rid = self.RAWINPUTDEVICE(1, 2, self.RIDEV_INPUTSINK | self.RIDEV_NOLEGACY, self.hwnd)
            if not ctypes.windll.user32.RegisterRawInputDevices(
                ctypes.byref(rid), 1, ctypes.sizeof(self.RAWINPUTDEVICE)
            ):
                return False  # 失败可能需要管理员权限

            self.enabled = True
            self.running = True

            if self.gui_callback:
                self.gui_callback.root.after(100, self.state_monitor)
                self.gui_callback.root.after(10, self.message_loop)

            return True
        except Exception as e:
            print(f"启用失败: {e}")
            return False

    def disable(self):
        if not self.enabled: return
        print("禁用达芬奇拖拽助手...")
        # 收尾：确保左键抬起
        try: send_mouse_event(MOUSEEVENTF_LEFTUP)
        except: pass

        self.enabled = False
        self.running = False
        self.current_state = self.State.NORMAL

        if self.hwnd:
            try: win32gui.DestroyWindow(self.hwnd)
            except Exception as e: print(f"销毁窗口失败: {e}")
        if self.atom and self.wc:
            try: win32gui.UnregisterClass(self.atom, self.wc.hInstance)
            except Exception as e: print(f"注销窗口类失败: {e}")
        self.hwnd, self.atom, self.wc = None, None, None

# -------- GUI --------
class DavinciDragGUI:
    def __init__(self):
        self.config = Config()
        self.helper = DavinciDragHelper(self.config, gui_callback=self)
        self.root = tk.Tk()
        self.root.title("达芬奇双鼠标拖拽助手")
        self.root.geometry("500x680")
        self.root.resizable(False, False)

        atexit.register(self.cleanup)
        import signal
        def signal_handler(signum, frame):
            print(f"接收到信号 {signum}，开始清理...")
            self.cleanup()
            sys.exit(0)
        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)

        self.create_widgets()
        if self.config.config['enabled']:
            self.enabled_var.set(True)
            self.toggle_helper()
    
    def create_widgets(self):
        main_frame = ttk.Frame(self.root, padding="15")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        
        title_label = ttk.Label(main_frame, text="达芬奇双鼠标拖拽助手", font=("Microsoft YaHei", 16, "bold"))
        title_label.grid(row=0, column=0, columnspan=2, pady=(0, 20))
        
        control_frame = ttk.LabelFrame(main_frame, text="控制", padding="10")
        control_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 15))
        self.enabled_var = tk.BooleanVar(value=self.config.config['enabled'])
        enable_check = ttk.Checkbutton(control_frame, text="启用达芬奇双鼠标拖拽助手", variable=self.enabled_var, command=self.toggle_helper)
        enable_check.grid(row=0, column=0, sticky=tk.W, pady=(0, 10))
        self.status_var = tk.StringVar(value="程序已启动")
        status_label = ttk.Label(control_frame, textvariable=self.status_var, foreground="blue")
        status_label.grid(row=1, column=0, sticky=tk.W)
        
        config_frame = ttk.LabelFrame(main_frame, text="参数设置", padding="10")
        config_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 15))
        
        ttk.Label(config_frame, text="拖拽速度（已停用）:").grid(row=0, column=0, sticky=tk.W, pady=(0, 5))
        self.speed_var = tk.IntVar(value=self.config.config['drag_speed'])
        speed_scale = ttk.Scale(config_frame, from_=1, to=20, orient=tk.HORIZONTAL, variable=self.speed_var, command=self.update_config, state="disabled")
        speed_scale.grid(row=0, column=1, sticky=(tk.W, tk.E), padx=(10, 0), pady=(0, 5))
        self.speed_entry_var = tk.StringVar(value=str(float(self.config.config['drag_speed'])))
        speed_entry = ttk.Entry(config_frame, textvariable=self.speed_entry_var, width=8, state="disabled")
        speed_entry.grid(row=0, column=2, padx=(10, 0), pady=(0, 5))
        
        ttk.Label(config_frame, text="空闲超时 (ms):").grid(row=1, column=0, sticky=tk.W, pady=(5, 5))
        self.timeout_var = tk.IntVar(value=self.config.config['idle_timeout'])
        timeout_spin = ttk.Spinbox(config_frame, from_=10, to=1000, textvariable=self.timeout_var, width=15, command=self.update_config)
        timeout_spin.grid(row=1, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        timeout_spin.bind('<KeyRelease>', self.update_config)
        
        ttk.Label(config_frame, text="默认鼠标速度（已停用）:").grid(row=2, column=0, sticky=tk.W, pady=(5, 5))
        self.default_speed_var = tk.IntVar(value=self.config.config['default_mouse_speed'])
        default_speed_spin = ttk.Spinbox(config_frame, from_=1, to=20, textvariable=self.default_speed_var, width=15, command=self.update_config, state="disabled")
        default_speed_spin.grid(row=2, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        
        ttk.Label(config_frame, text="状态检查频率 (秒):").grid(row=3, column=0, sticky=tk.W, pady=(5, 5))
        self.check_interval_var = tk.DoubleVar(value=self.config.config['speed_check_interval'])
        check_interval_spin = ttk.Spinbox(config_frame, from_=0.1, to=10.0, increment=0.1, textvariable=self.check_interval_var, width=15, command=self.update_config)
        check_interval_spin.grid(row=3, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        check_interval_spin.bind('<KeyRelease>', self.update_config)
        
        mouse_frame = ttk.LabelFrame(main_frame, text="鼠标设备 ID", padding="10")
        mouse_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 15))
        
        ttk.Label(mouse_frame, text="左手鼠标 ID:").grid(row=0, column=0, sticky=tk.W, pady=(0, 5))
        self.left_id_var = tk.StringVar(value=self.config.config['left_mouse_id'])
        left_entry = tk.Text(mouse_frame, height=3, width=50, wrap=tk.WORD)
        left_entry.insert('1.0', self.left_id_var.get())
        left_entry.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        left_entry.bind('<KeyRelease>', lambda e: self.update_mouse_id('left', left_entry.get('1.0', tk.END).strip()))
        
        ttk.Label(mouse_frame, text="右手鼠标 ID:").grid(row=2, column=0, sticky=tk.W, pady=(0, 5))
        self.right_id_var = tk.StringVar(value=self.config.config['right_mouse_id'])
        right_entry = tk.Text(mouse_frame, height=3, width=50, wrap=tk.WORD)
        right_entry.insert('1.0', self.right_id_var.get())
        right_entry.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        right_entry.bind('<KeyRelease>', lambda e: self.update_mouse_id('right', right_entry.get('1.0', tk.END).strip()))
        
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=4, column=0, columnspan=2, pady=(15, 20), sticky=(tk.W, tk.E))
        for i in range(4): button_frame.columnconfigure(i, weight=1)
        save_btn = ttk.Button(button_frame, text="保存配置", command=self.save_config)
        save_btn.grid(row=0, column=0, padx=5, pady=10, sticky=(tk.W, tk.E))
        reset_btn = ttk.Button(button_frame, text="重置配置", command=self.reset_config)
        reset_btn.grid(row=0, column=1, padx=5, pady=10, sticky=(tk.W, tk.E))
        emergency_btn = ttk.Button(button_frame, text="紧急恢复鼠标速度（可选）", command=self.emergency_restore_speed)
        emergency_btn.grid(row=0, column=2, padx=5, pady=10, sticky=(tk.W, tk.E))
        help_btn = ttk.Button(button_frame, text="帮助", command=self.show_help)
        help_btn.grid(row=0, column=3, padx=5, pady=10, sticky=(tk.W, tk.E))
        
        main_frame.columnconfigure(1, weight=1)
        config_frame.columnconfigure(1, weight=1)
        mouse_frame.columnconfigure(0, weight=1)
    
    def toggle_helper(self):
        enabled = self.enabled_var.get()
        self.config.config['enabled'] = enabled
        if enabled:
            if self.helper.enable():
                self.status_var.set("✓ 达芬奇拖拽助手已启用")
            else:
                self.status_var.set("✗ 启用失败，可能需要管理员权限")
                self.enabled_var.set(False)
                messagebox.showwarning("启用失败", "无法注册原始输入设备。\n\n可能的解决方案：\n1. 以管理员权限运行程序\n2. 检查鼠标设备ID是否正确\n3. 确保鼠标设备已连接")
        else:
            self.helper.disable()
            self.status_var.set("达芬奇拖拽助手已禁用")
    
    def update_config(self, *args):
        self.config.config['idle_timeout'] = self.timeout_var.get()
        self.config.config['speed_check_interval'] = self.check_interval_var.get()
        if hasattr(self, 'helper') and self.helper.enabled:
            self.helper.poll_interval = self.config.config['speed_check_interval']
    
    def update_mouse_id(self, side, value):
        if side == 'left': self.config.config['left_mouse_id'] = value
        else: self.config.config['right_mouse_id'] = value
    
    def save_config(self):
        self.config.save_config()
        messagebox.showinfo("保存成功", "配置已保存")
    
    def reset_config(self):
        if messagebox.askyesno("确认重置", "确定要重置所有配置吗？"):
            self.helper.disable()
            if os.path.exists(self.config.config_file):
                os.remove(self.config.config_file)
            self.config.load_config()
            self.enabled_var.set(self.config.config['enabled'])
            self.timeout_var.set(self.config.config['idle_timeout'])
            self.check_interval_var.set(self.config.config['speed_check_interval'])
            self.status_var.set("配置已重置")
    
    def emergency_restore_speed(self):
        # 可选：手动恢复系统指针速度，非必需；仅当你觉得系统速度异常时点击
        try:
            default_speed = 10
            ctypes.windll.user32.SystemParametersInfoW(0x0071, 0, default_speed, 0)  # SPI_SETMOUSESPEED
            messagebox.showinfo("恢复成功", f"已将鼠标速度恢复为默认值 ({default_speed})")
            self.status_var.set("✓ 鼠标速度已紧急恢复")
        except Exception as e:
            messagebox.showerror("恢复失败", f"无法恢复鼠标速度: {e}")
    
    # --- 以下“获取鼠标ID”的 UI 与逻辑保持原样 ---
    def show_help(self):
        help_window = tk.Toplevel(self.root)
        help_window.title("帮助 - 达芬奇双鼠标拖拽助手")
        help_window.geometry("700x600")
        help_window.resizable(True, True)
        
        notebook = ttk.Notebook(help_window)
        notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        help_frame = ttk.Frame(notebook)
        notebook.add(help_frame, text="使用说明")
        
        help_text = """达芬奇双鼠标拖拽助手 v1.3（精简修正）

使用说明：
1. 勾选"启用"开关激活拖拽助手
2. 在达芬奇软件中，用左手鼠标开始拖拽（移动即可触发）
3. 用右手鼠标确认结束拖拽（或等待超时自动进入确认，再次移动左手可继续）

重要说明：
• 拖拽期间不再重放移动，只托管按键，避免路径叠加导致的“斜移”
• 不再自动修改系统的指针速度，减少不一致

参数说明：
• 空闲超时：拖拽停止多久后进入确认模式
• 状态检查频率：后台轮询检查状态的频率（秒）

注意事项：
• 需要两个独立的鼠标设备
• 如启用失败，可能需要管理员权限
• 仅在达芬奇软件（Resolve.exe）前台时生效
"""
        help_text_widget = tk.Text(help_frame, wrap=tk.WORD, padx=10, pady=10)
        help_text_widget.insert('1.0', help_text)
        help_text_widget.config(state=tk.DISABLED)
        help_scrollbar = ttk.Scrollbar(help_frame, orient=tk.VERTICAL, command=help_text_widget.yview)
        help_text_widget.config(yscrollcommand=help_scrollbar.set)
        help_text_widget.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        help_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        mouse_id_frame = ttk.Frame(notebook)
        notebook.add(mouse_id_frame, text="获取鼠标ID")
        instruction_frame = ttk.LabelFrame(mouse_id_frame, text="检测说明", padding="10")
        instruction_frame.pack(fill=tk.X, padx=10, pady=(10, 5))
        instruction_text = """1. 点击"开始检测"按钮\n2. 移动您要配置的鼠标\n3. 点击对应的"设为左手鼠标"或"设为右手鼠标"按钮\n4. 重复步骤2-3配置另一个鼠标\n5. 检测完成后点击"应用配置"保存设置"""
        instruction_label = ttk.Label(instruction_frame, text=instruction_text, justify=tk.LEFT)
        instruction_label.pack()
        control_frame = ttk.LabelFrame(mouse_id_frame, text="检测控制", padding="10")
        control_frame.pack(fill=tk.X, padx=10, pady=5)
        button_frame = ttk.Frame(control_frame)
        button_frame.pack(fill=tk.X)
        self.detection_running = False
        def toggle_detection():
            if not self.detection_running:
                self.start_mouse_detection()
                detect_btn.config(text="停止检测", state=tk.NORMAL)
                clear_btn.config(state=tk.DISABLED)
            else:
                self.stop_mouse_detection()
                detect_btn.config(text="开始检测", state=tk.NORMAL)
                clear_btn.config(state=tk.NORMAL)
        detect_btn = ttk.Button(button_frame, text="开始检测", command=toggle_detection)
        detect_btn.pack(side=tk.LEFT, padx=(0, 5))
        clear_btn = ttk.Button(button_frame, text="清空结果", command=self.clear_detection_results)
        clear_btn.pack(side=tk.LEFT, padx=5)
        apply_btn = ttk.Button(button_frame, text="应用配置", command=self.apply_mouse_config)
        apply_btn.pack(side=tk.RIGHT)
        status_frame = ttk.Frame(control_frame)
        status_frame.pack(fill=tk.X, pady=(10, 0))
        ttk.Label(status_frame, text="状态:").pack(side=tk.LEFT)
        self.detection_status_var = tk.StringVar(value="未开始")
        self.detection_status_label = ttk.Label(status_frame, textvariable=self.detection_status_var, foreground="blue")
        self.detection_status_label.pack(side=tk.LEFT, padx=(5, 0))
        result_frame = ttk.LabelFrame(mouse_id_frame, text="检测结果", padding="10")
        result_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        current_mouse_frame = ttk.LabelFrame(result_frame, text="当前检测到的鼠标", padding="10")
        current_mouse_frame.pack(fill=tk.X, pady=(0, 10))
        self.current_mouse_var = tk.StringVar(value="移动鼠标以检测...")
        current_mouse_label = ttk.Label(current_mouse_frame, textvariable=self.current_mouse_var, wraplength=500, justify=tk.LEFT)
        current_mouse_label.pack(anchor=tk.W)
        mouse_buttons_frame = ttk.Frame(current_mouse_frame)
        mouse_buttons_frame.pack(fill=tk.X, pady=(10, 0))
        self.set_left_btn = ttk.Button(mouse_buttons_frame, text="设为左手鼠标", command=self.set_as_left_mouse, state=tk.DISABLED)
        self.set_left_btn.pack(side=tk.LEFT, padx=(0, 5))
        self.set_right_btn = ttk.Button(mouse_buttons_frame, text="设为右手鼠标", command=self.set_as_right_mouse, state=tk.DISABLED)
        self.set_right_btn.pack(side=tk.LEFT)
        config_frame = ttk.LabelFrame(result_frame, text="已配置的鼠标", padding="10")
        config_frame.pack(fill=tk.BOTH, expand=True)
        left_frame = ttk.Frame(config_frame)
        left_frame.pack(fill=tk.X, pady=(0, 5))
        ttk.Label(left_frame, text="左手鼠标:", font=("Microsoft YaHei", 9, "bold")).pack(anchor=tk.W)
        self.left_mouse_config_var = tk.StringVar(value="未配置")
        self.left_mouse_config_label = ttk.Label(left_frame, textvariable=self.left_mouse_config_var, wraplength=500, justify=tk.LEFT, foreground="green")
        self.left_mouse_config_label.pack(anchor=tk.W, padx=(10, 0))
        right_frame = ttk.Frame(config_frame)
        right_frame.pack(fill=tk.X)
        ttk.Label(right_frame, text="右手鼠标:", font=("Microsoft YaHei", 9, "bold")).pack(anchor=tk.W)
        self.right_mouse_config_var = tk.StringVar(value="未配置")
        self.right_mouse_config_label = ttk.Label(right_frame, textvariable=self.right_mouse_config_var, wraplength=500, justify=tk.LEFT, foreground="green")
        self.right_mouse_config_label.pack(anchor=tk.W, padx=(10, 0))
        self.mouse_detection_window = help_window
        self.detect_btn = detect_btn
        self.clear_btn = clear_btn
        self.current_detected_mouse = None
        self.update_mouse_config_display()
        help_window.protocol("WM_DELETE_WINDOW", lambda: (self.stop_mouse_detection() if hasattr(self, 'detection_running') and self.detection_running else None, help_window.destroy()))
    
    def start_mouse_detection(self):
        if self.detection_running: return
        self.detection_running = True
        self.current_detected_mouse = None
        import queue
        self.detection_queue = queue.Queue()
        self.detection_status_var.set("检测中... 请移动鼠标")
        self.current_mouse_var.set("移动鼠标以检测...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.process_detection_queue()
        try:
            if not hasattr(self.helper, 'enabled') or not self.helper.enabled:
                self.temp_detection_mode = True
                if not self.helper.enable(): raise Exception("无法启用鼠标检测")
        except Exception as e:
            self.detection_running = False
            self.detection_status_var.set(f"启动失败: {e}")
    
    def process_detection_queue(self):
        if not self.detection_running: return
        try:
            import queue
            while not self.detection_queue.empty():
                try:
                    device_name = self.detection_queue.get_nowait()
                    self.on_mouse_detected(device_name)
                except queue.Empty: break
        except Exception as e:
            print(f"处理检测队列异常: {e}")
        if self.detection_running: self.root.after(50, self.process_detection_queue)
    
    def on_mouse_detected(self, device_name):
        if not self.detection_running: return
        self.current_detected_mouse = device_name
        short_name = self.get_short_device_name(device_name)
        self.current_mouse_var.set(f"检测到: {short_name}")
        self.set_left_btn.config(state=tk.NORMAL)
        self.set_right_btn.config(state=tk.NORMAL)
        self.detection_status_var.set("检测成功！选择鼠标用途")
    
    def get_short_device_name(self, device_name):
        if not device_name: return "未知设备"
        if "VID" in device_name and "PID" in device_name:
            import re
            vid_match = re.search(r'VID[_&]([0-9A-F]{4})', device_name, re.IGNORECASE)
            pid_match = re.search(r'PID[_&]([0-9A-F]{4})', device_name, re.IGNORECASE)
            if vid_match and pid_match: return f"鼠标设备 (VID:{vid_match.group(1)}, PID:{pid_match.group(1)})"
        if "00001812-0000-1000-8000-00805f9b34fb" in device_name: return "蓝牙鼠标设备"
        return "鼠标设备"
    
    def set_as_left_mouse(self):
        if not self.current_detected_mouse: return
        self.config.config['left_mouse_id'] = self.current_detected_mouse
        self.update_mouse_config_display()
        self.detection_status_var.set("左手鼠标已配置")
        self.current_mouse_var.set("继续移动其他鼠标以配置...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.current_detected_mouse = None
    
    def set_as_right_mouse(self):
        if not self.current_detected_mouse: return
        self.config.config['right_mouse_id'] = self.current_detected_mouse
        self.update_mouse_config_display()
        self.detection_status_var.set("右手鼠标已配置")
        self.current_mouse_var.set("继续移动其他鼠标以配置...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.current_detected_mouse = None
    
    def update_mouse_config_display(self):
        left_id = self.config.config.get('left_mouse_id', '')
        self.left_mouse_config_var.set(f"已配置: {self.get_short_device_name(left_id)}" if left_id else "未配置")
        right_id = self.config.config.get('right_mouse_id', '')
        self.right_mouse_config_var.set(f"已配置: {self.get_short_device_name(right_id)}" if right_id else "未配置")
    
    def clear_detection_results(self):
        self.current_detected_mouse = None
        self.current_mouse_var.set("移动鼠标以检测...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.detection_status_var.set("已清空")
    
    def apply_mouse_config(self):
        try:
            for widget in self.root.winfo_children():
                if isinstance(widget, ttk.Frame):
                    for child in widget.winfo_children():
                        if isinstance(child, ttk.LabelFrame) and "鼠标设备 ID" in child.cget('text'):
                            text_widgets = [subchild for subchild in child.winfo_children() if isinstance(subchild, tk.Text)]
                            if len(text_widgets) >= 1:
                                text_widgets[0].delete('1.0', tk.END)
                                text_widgets[0].insert('1.0', self.config.config.get('left_mouse_id', ''))
                            if len(text_widgets) >= 2:
                                text_widgets[1].delete('1.0', tk.END)
                                text_widgets[1].insert('1.0', self.config.config.get('right_mouse_id', ''))
                            break
        except Exception as e:
            print(f"更新主界面失败: {e}")
        self.config.save_config()
        from tkinter import messagebox
        messagebox.showinfo("配置应用", "鼠标配置已应用并保存！\n请重新启用助手以使新配置生效。")
        self.detection_status_var.set("配置已应用")
    
    def stop_mouse_detection(self):
        if not self.detection_running: return
        self.detection_running = False
        if hasattr(self, 'detection_queue'):
            try:
                import queue
                while not self.detection_queue.empty(): self.detection_queue.get_nowait()
            except: pass
        self.detection_status_var.set("检测已停止")
        self.current_mouse_var.set("移动鼠标以检测...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.current_detected_mouse = None
        if hasattr(self, 'temp_detection_mode') and self.temp_detection_mode:
            if not self.enabled_var.get(): self.helper.disable()
            self.temp_detection_mode = False
    
    def cleanup(self):
        print("开始清理程序资源...")
        if hasattr(self, 'detection_running') and self.detection_running:
            self.stop_mouse_detection()
        if hasattr(self, 'helper'):
            try:
                # 保证 LEFTUP
                send_mouse_event(MOUSEEVENTF_LEFTUP)
            except: pass
            try:
                self.helper.disable()
                print("Helper已禁用")
            except Exception as e:
                print(f"禁用helper失败: {e}")
        print("程序清理完成")
    
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