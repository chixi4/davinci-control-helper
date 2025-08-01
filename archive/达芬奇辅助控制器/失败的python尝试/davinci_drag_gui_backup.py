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

class Config:
    def __init__(self):
        self.config_file = "davinci_drag_config.json"
        self.load_config()
    
    def load_config(self):
        default_config = {
            "enabled": False,
            "left_mouse_id": r"\\?\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
            "right_mouse_id": r"\\?\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}",
            "drag_speed": 1,
            "idle_timeout": 20,
            "debug_log": False,
            "default_mouse_speed": 10,
            "speed_check_interval": 1.0,
            "target_process": "Resolve.exe",
            "direction_correction": True  # 新增：是否启用方向校正
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

class DavinciDragHelper:
    def __init__(self, config, gui_callback=None):
        self.config = config
        self.gui_callback = gui_callback  # GUI回调函数
        self.enabled = False
        self.running = False
        self.original_speed = None
        self.hwnd = None
        self.atom = None
        self.wc = None
        self.last_speed_check = 0
        self.speed_protection_interval = config.config['speed_check_interval']  # 使用配置的检查频率
        
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
        level = max(1, min(20, level))
        ctypes.windll.user32.SystemParametersInfoW(self.SPI_SETMOUSESPEED, 0, level, 0)
    
    def reset_to_normal_state(self):
        """强制重置到正常状态并恢复原始速度"""
        if self.current_state != self.State.NORMAL:
            print(f"强制重置状态: {self.current_state} -> NORMAL")
            self.current_state = self.State.NORMAL
            if self.original_speed:
                self.set_pointer_speed(self.original_speed)
                print(f"恢复原始鼠标速度: {self.original_speed}")
    
    def ensure_speed_protection(self):
        """确保鼠标速度保护机制正常工作"""
        current_time = time.time()
        if current_time - self.last_speed_check > self.speed_protection_interval:
            self.last_speed_check = current_time
            
            # 如果不在拖拽状态，确保速度是原始速度
            if self.current_state == self.State.NORMAL and self.original_speed:
                current_speed = self.get_pointer_speed()
                if current_speed != self.original_speed:
                    print(f"检测到速度异常 {current_speed}, 恢复到 {self.original_speed}")
                    self.set_pointer_speed(self.original_speed)
            
            # 检查是否离开了目标应用
            if self.current_state != self.State.NORMAL:
                current_process = self.get_active_process_name()
                if not current_process or not current_process.lower().endswith(self.config.config['target_process'].lower()):
                    print("检测到离开目标应用，重置状态")
                    self.reset_to_normal_state()
    
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
    
    def wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == self.WM_INPUT and self.enabled:
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
            
            # 通知GUI检测到鼠标移动（用于鼠标ID检测）
            if self.gui_callback and hasattr(self.gui_callback, 'detection_running') and self.gui_callback.detection_running:
                # 只有在有实际移动时才通知
                if ri.data.mouse.lLastX != 0 or ri.data.mouse.lLastY != 0:
                    try:
                        # 使用线程安全的方式通知GUI
                        import queue
                        if not hasattr(self.gui_callback, 'detection_queue'):
                            self.gui_callback.detection_queue = queue.Queue()
                        self.gui_callback.detection_queue.put(dev_name)
                    except:
                        pass
            
            is_left = self.config.config['left_mouse_id'].lower() in dev_name.lower()
            is_right = self.config.config['right_mouse_id'].lower() in dev_name.lower()
            
            if self.current_state == self.State.NORMAL and is_left:
                # 检查是否在目标进程中
                current_process = self.get_active_process_name()
                if not current_process or not current_process.lower().endswith(self.config.config['target_process'].lower()):
                    return 1  # 不在目标进程中，忽略拖拽
                
                self.set_pointer_speed(self.config.config['drag_speed'])
                self.current_state = self.State.DRAG
                self.last_left_move_time = time.time()
                self.cumulative_dx = self.cumulative_dy = 0.0
                win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
                return 0
            
            elif self.current_state == self.State.DRAG:
                if is_left:
                    self.last_left_move_time = time.time()
                    
                    # 获取原始移动增量
                    raw_dx = ri.data.mouse.lLastX
                    raw_dy = ri.data.mouse.lLastY
                    
                    # 如果有实际移动才处理
                    if raw_dx != 0 or raw_dy != 0:
                        # 检查是否启用方向校正
                        if self.config.config.get('direction_correction', True):
                            # 添加方向校正：确保垂直移动真正是垂直的
                            # 如果主要是垂直移动（abs(dy) > abs(dx) * 2），则强制dx为0
                            if abs(raw_dy) > abs(raw_dx) * 2:
                                corrected_dx = 0
                                corrected_dy = raw_dy
                            # 如果主要是水平移动（abs(dx) > abs(dy) * 2），则强制dy为0  
                            elif abs(raw_dx) > abs(raw_dy) * 2:
                                corrected_dx = raw_dx
                                corrected_dy = 0
                            else:
                                # 小角度移动保持原样
                                corrected_dx = raw_dx
                                corrected_dy = raw_dy
                        else:
                            # 不启用方向校正，直接使用原始值
                            corrected_dx = raw_dx
                            corrected_dy = raw_dy
                        
                        # 使用校正后的增量
                        win32api.mouse_event(win32con.MOUSEEVENTF_MOVE, corrected_dx, corrected_dy, 0, 0)
                    
                    return 0
                elif is_right:
                    return 0
            
            elif self.current_state == self.State.WAIT_CONFIRM:
                if is_left:
                    self.current_state = self.State.DRAG
                    self.last_left_move_time = time.time()
                    return 0
                elif is_right:
                    self.current_state = self.State.NORMAL
                    win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
                    self.set_pointer_speed(self.original_speed)
                    return 0
        
        return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)
    
    def state_monitor(self):
        """状态监控 - 改为定时器方式"""
        if not self.running:
            return
            
        try:
            # 确保速度保护机制工作
            self.ensure_speed_protection()
            
            if self.current_state == self.State.DRAG:
                if (time.time() - self.last_left_move_time) * 1000 > self.config.config['idle_timeout']:
                    self.current_state = self.State.WAIT_CONFIRM
        except Exception as e:
            print(f"状态监控异常: {e}")
            # 出现异常时重置状态
            try:
                self.reset_to_normal_state()
            except:
                pass
        
        # 继续监控
        if self.running and hasattr(self, 'gui_callback') and self.gui_callback:
            try:
                self.gui_callback.root.after(100, self.state_monitor)  # 每100ms检查一次
            except:
                pass
    
    def message_loop(self):
        """主消息循环 - 改为定时器方式"""
        if not self.running:
            return
            
        try:
            # 使用更安全的消息处理
            bRet = ctypes.windll.user32.PeekMessageW(None, None, 0, 0, 0)
            if bRet != 0:
                win32gui.PumpWaitingMessages()
        except Exception as e:
            print(f"主消息循环异常: {e}")
        
        # 继续消息循环
        if self.running and hasattr(self, 'gui_callback') and self.gui_callback:
            try:
                self.gui_callback.root.after(10, self.message_loop)  # 每10ms检查一次
            except:
                pass
    
    def enable(self):
        if self.enabled:
            return True
            
        try:
            # 首先强制恢复到默认速度，然后再使用它作为原始速度
            default_speed = self.config.config['default_mouse_speed']
            self.set_pointer_speed(default_speed)
            
            # 等待一下确保设置生效
            import time
            time.sleep(0.1)
            
            # 验证速度是否正确设置
            current_speed = self.get_pointer_speed()
            if current_speed != default_speed:
                print(f"警告: 设置速度为 {default_speed}, 但实际为 {current_speed}")
                # 再次尝试设置
                self.set_pointer_speed(default_speed)
                time.sleep(0.1)
                current_speed = self.get_pointer_speed()
            
            self.original_speed = current_speed
            print(f"启用时设置原始鼠标速度: {self.original_speed}")
            
            self.wc = win32gui.WNDCLASS()
            self.wc.lpfnWndProc = self.wnd_proc
            self.wc.lpszClassName = "DavinciDragHelper"
            self.wc.hInstance = win32api.GetModuleHandle(None)
            self.atom = win32gui.RegisterClass(self.wc)
            self.hwnd = win32gui.CreateWindow(
                self.atom, "DDH", 0, 0, 0, 0, 0, 
                win32con.HWND_MESSAGE, None, self.wc.hInstance, None
            )
            
            rid = self.RAWINPUTDEVICE(1, 2, self.RIDEV_INPUTSINK, self.hwnd)
            if not ctypes.windll.user32.RegisterRawInputDevices(
                ctypes.byref(rid), 1, ctypes.sizeof(self.RAWINPUTDEVICE)
            ):
                return False
            
            self.enabled = True
            self.running = True
            
            # 使用定时器替代线程
            if self.gui_callback:
                self.gui_callback.root.after(100, self.state_monitor)
                self.gui_callback.root.after(10, self.message_loop)
            
            return True
        except Exception as e:
            print(f"启用失败: {e}")
            return False
    
    def disable(self):
        if not self.enabled:
            return
        
        print("禁用达芬奇拖拽助手...")
        self.enabled = False
        self.running = False
        
        # 强制恢复原始速度
        if self.original_speed:
            try:
                self.set_pointer_speed(self.original_speed)
                print(f"恢复原始鼠标速度: {self.original_speed}")
            except Exception as e:
                print(f"恢复鼠标速度失败: {e}")
        
        # 重置状态
        self.current_state = self.State.NORMAL
        
        # 清理Windows资源
        if self.hwnd:
            try:
                win32gui.DestroyWindow(self.hwnd)
            except Exception as e:
                print(f"销毁窗口失败: {e}")
        
        if self.atom and self.wc:
            try:
                win32gui.UnregisterClass(self.atom, self.wc.hInstance)
            except Exception as e:
                print(f"注销窗口类失败: {e}")
        
        # 清理资源引用
        self.hwnd = None
        self.atom = None
        self.wc = None

class DavinciDragGUI:
    def __init__(self):
        self.config = Config()
        self.helper = DavinciDragHelper(self.config, gui_callback=self)  # 传递self作为回调
        
        self.root = tk.Tk()
        self.root.title("达芬奇双鼠标拖拽助手")
        self.root.geometry("500x630")  # 增加高度以适应增加的边距
        self.root.resizable(False, False)
        
        atexit.register(self.cleanup)
        # 注册信号处理器以处理意外退出
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
        
        # 配置主框架的列权重
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        
        title_label = ttk.Label(
            main_frame, 
            text="达芬奇双鼠标拖拽助手", 
            font=("Microsoft YaHei", 16, "bold")
        )
        title_label.grid(row=0, column=0, columnspan=2, pady=(0, 20))
        
        control_frame = ttk.LabelFrame(main_frame, text="控制", padding="10")
        control_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 15))
        
        self.enabled_var = tk.BooleanVar(value=self.config.config['enabled'])
        enable_check = ttk.Checkbutton(
            control_frame,
            text="启用达芬奇双鼠标拖拽助手",
            variable=self.enabled_var,
            command=self.toggle_helper
        )
        enable_check.grid(row=0, column=0, sticky=tk.W, pady=(0, 10))
        
        self.status_var = tk.StringVar(value="程序已启动")
        status_label = ttk.Label(control_frame, textvariable=self.status_var, foreground="blue")
        status_label.grid(row=1, column=0, sticky=tk.W)
        
        config_frame = ttk.LabelFrame(main_frame, text="参数设置", padding="10")
        config_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 15))
        
        ttk.Label(config_frame, text="拖拽速度:").grid(row=0, column=0, sticky=tk.W, pady=(0, 5))
        self.speed_var = tk.IntVar(value=self.config.config['drag_speed'])
        speed_scale = ttk.Scale(
            config_frame,
            from_=1,
            to=20,
            orient=tk.HORIZONTAL,
            variable=self.speed_var,
            command=self.update_config
        )
        speed_scale.grid(row=0, column=1, sticky=(tk.W, tk.E), padx=(10, 0), pady=(0, 5))
        
        # 可输入小数的输入框
        self.speed_entry_var = tk.StringVar(value=str(float(self.config.config['drag_speed'])))
        speed_entry = ttk.Entry(config_frame, textvariable=self.speed_entry_var, width=8)
        speed_entry.grid(row=0, column=2, padx=(10, 0), pady=(0, 5))
        speed_entry.bind('<KeyRelease>', self.update_speed_from_entry)
        speed_entry.bind('<FocusOut>', self.validate_speed_entry)
        
        ttk.Label(config_frame, text="空闲超时 (ms):").grid(row=1, column=0, sticky=tk.W, pady=(5, 5))
        self.timeout_var = tk.IntVar(value=self.config.config['idle_timeout'])
        timeout_spin = ttk.Spinbox(
            config_frame,
            from_=10,
            to=1000,
            textvariable=self.timeout_var,
            width=15,
            command=self.update_config
        )
        timeout_spin.grid(row=1, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        timeout_spin.bind('<KeyRelease>', self.update_config)
        
        # 添加默认鼠标速度设置
        ttk.Label(config_frame, text="默认鼠标速度:").grid(row=2, column=0, sticky=tk.W, pady=(5, 5))
        self.default_speed_var = tk.IntVar(value=self.config.config['default_mouse_speed'])
        default_speed_spin = ttk.Spinbox(
            config_frame,
            from_=1,
            to=20,
            textvariable=self.default_speed_var,
            width=15,
            command=self.update_config
        )
        default_speed_spin.grid(row=2, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        default_speed_spin.bind('<KeyRelease>', self.update_config)
        
        # 添加速度检查频率设置
        ttk.Label(config_frame, text="速度检查频率 (秒):").grid(row=3, column=0, sticky=tk.W, pady=(5, 5))
        self.check_interval_var = tk.DoubleVar(value=self.config.config['speed_check_interval'])
        check_interval_spin = ttk.Spinbox(
            config_frame,
            from_=0.1,
            to=10.0,
            increment=0.1,
            textvariable=self.check_interval_var,
            width=15,
            command=self.update_config
        )
        check_interval_spin.grid(row=3, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        check_interval_spin.bind('<KeyRelease>', self.update_config)
        
        # 添加方向校正设置
        ttk.Label(config_frame, text="方向校正:").grid(row=4, column=0, sticky=tk.W, pady=(5, 5))
        self.direction_correction_var = tk.BooleanVar(value=self.config.config.get('direction_correction', True))
        direction_check = ttk.Checkbutton(
            config_frame,
            text="启用垂直/水平拖拽方向校正",
            variable=self.direction_correction_var,
            command=self.update_config
        )
        direction_check.grid(row=4, column=1, columnspan=2, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        
        mouse_frame = ttk.LabelFrame(main_frame, text="鼠标设备 ID", padding="10")
        mouse_frame.grid(row=4, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 15))
        
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
        button_frame.grid(row=5, column=0, columnspan=2, pady=(15, 20), sticky=(tk.W, tk.E))
        
        # 配置按钮框架的列权重，使按钮均匀分布
        for i in range(4):
            button_frame.columnconfigure(i, weight=1)
        
        # 创建一行四个按钮的布局
        save_btn = ttk.Button(button_frame, text="保存配置", command=self.save_config)
        save_btn.grid(row=0, column=0, padx=5, pady=10, sticky=(tk.W, tk.E))
        
        reset_btn = ttk.Button(button_frame, text="重置配置", command=self.reset_config)
        reset_btn.grid(row=0, column=1, padx=5, pady=10, sticky=(tk.W, tk.E))
        
        emergency_btn = ttk.Button(
            button_frame, 
            text="紧急恢复鼠标速度", 
            command=self.emergency_restore_speed
        )
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
                messagebox.showwarning(
                    "启用失败", 
                    "无法注册原始输入设备。\n\n可能的解决方案：\n1. 以管理员权限运行程序\n2. 检查鼠标设备ID是否正确\n3. 确保鼠标设备已连接"
                )
        else:
            self.helper.disable()
            self.status_var.set("达芬奇拖拽助手已禁用")
    
    def update_config(self, *args):
        self.config.config['drag_speed'] = self.speed_var.get()
        self.config.config['idle_timeout'] = self.timeout_var.get()
        self.config.config['default_mouse_speed'] = self.default_speed_var.get()
        self.config.config['speed_check_interval'] = self.check_interval_var.get()
        self.config.config['direction_correction'] = self.direction_correction_var.get()
        
        # 同步更新输入框显示
        self.speed_entry_var.set(str(float(self.speed_var.get())))
        
        # 如果助手正在运行，更新检查间隔
        if hasattr(self, 'helper') and self.helper.enabled:
            self.helper.speed_protection_interval = self.config.config['speed_check_interval']
            # 更新默认速度（如果当前不在拖拽状态）
            if self.helper.current_state == self.helper.State.NORMAL:
                self.helper.original_speed = self.config.config['default_mouse_speed']
    
    def update_speed_from_entry(self, *args):
        """从输入框更新速度值"""
        try:
            value = float(self.speed_entry_var.get())
            if 0.1 <= value <= 20.0:
                # 更新滑块（滑块只支持整数，所以四舍五入）
                self.speed_var.set(int(round(value)))
                # 实际的拖拽速度使用小数值
                self.config.config['drag_speed'] = value
        except ValueError:
            pass  # 输入无效时忽略
    
    def validate_speed_entry(self, *args):
        """验证输入框的速度值"""
        try:
            value = float(self.speed_entry_var.get())
            if value < 0.1:
                value = 0.1
            elif value > 20.0:
                value = 20.0
            self.speed_entry_var.set(str(value))
            self.speed_var.set(int(round(value)))
            self.config.config['drag_speed'] = value
        except ValueError:
            # 输入无效时恢复为当前配置值
            self.speed_entry_var.set(str(float(self.config.config['drag_speed'])))
    
    def update_mouse_id(self, side, value):
        if side == 'left':
            self.config.config['left_mouse_id'] = value
        else:
            self.config.config['right_mouse_id'] = value
    
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
            self.speed_var.set(int(self.config.config['drag_speed']))
            self.speed_entry_var.set(str(float(self.config.config['drag_speed'])))
            self.timeout_var.set(self.config.config['idle_timeout'])
            self.default_speed_var.set(self.config.config['default_mouse_speed'])
            self.check_interval_var.set(self.config.config['speed_check_interval'])
            self.direction_correction_var.set(self.config.config.get('direction_correction', True))
            
            self.status_var.set("配置已重置")
    
    def emergency_restore_speed(self):
        """紧急恢复鼠标速度到系统默认值"""
        try:
            # 强制重置助手状态
            if hasattr(self, 'helper'):
                self.helper.reset_to_normal_state()
            
            # 设置鼠标速度为系统默认值 (通常是10)
            default_speed = 10
            ctypes.windll.user32.SystemParametersInfoW(0x0071, 0, default_speed, 0)
            
            messagebox.showinfo("恢复成功", f"已将鼠标速度恢复为默认值 ({default_speed})")
            self.status_var.set("✓ 鼠标速度已紧急恢复")
            
        except Exception as e:
            messagebox.showerror("恢复失败", f"无法恢复鼠标速度: {e}")
    
    def show_help(self):
        # 创建帮助窗口
        help_window = tk.Toplevel(self.root)
        help_window.title("帮助 - 达芬奇双鼠标拖拽助手")
        help_window.geometry("700x600")
        help_window.resizable(True, True)
        
        # 创建选项卡
        notebook = ttk.Notebook(help_window)
        notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # 使用说明选项卡
        help_frame = ttk.Frame(notebook)
        notebook.add(help_frame, text="使用说明")
        
        help_text = """达芬奇双鼠标拖拽助手 v1.1

使用说明：
1. 勾选"启用"开关激活拖拽助手
2. 在达芬奇软件中，用左手鼠标开始拖拽
3. 用右手鼠标确认结束拖拽操作

参数说明：
• 拖拽速度：拖拽时的鼠标速度（支持小数，如1.5）
• 空闲超时：拖拽停止多久后进入确认模式
• 默认鼠标速度：正常状态下的鼠标速度（用于恢复）
• 速度检查频率：检查和恢复鼠标速度的频率（秒）

功能改进：
• 增加了鼠标速度保护机制，每秒自动检查和恢复
• 添加了离开目标应用时的自动状态重置
• 新增"紧急恢复鼠标速度"按钮，解决速度卡死问题
• 程序退出时自动恢复鼠标速度

注意事项：
• 需要两个独立的鼠标设备
• 如果启用失败，可能需要管理员权限
• 仅在达芬奇软件中生效
• 如遇到鼠标速度异常，点击"紧急恢复"按钮
"""
        
        help_text_widget = tk.Text(help_frame, wrap=tk.WORD, padx=10, pady=10)
        help_text_widget.insert('1.0', help_text)
        help_text_widget.config(state=tk.DISABLED)
        
        help_scrollbar = ttk.Scrollbar(help_frame, orient=tk.VERTICAL, command=help_text_widget.yview)
        help_text_widget.config(yscrollcommand=help_scrollbar.set)
        
        help_text_widget.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        help_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # 鼠标ID检测选项卡
        mouse_id_frame = ttk.Frame(notebook)
        notebook.add(mouse_id_frame, text="获取鼠标ID")
        
        # 说明文本
        instruction_frame = ttk.LabelFrame(mouse_id_frame, text="检测说明", padding="10")
        instruction_frame.pack(fill=tk.X, padx=10, pady=(10, 5))
        
        instruction_text = """1. 点击"开始检测"按钮
2. 移动您要配置的鼠标
3. 点击对应的"设为左手鼠标"或"设为右手鼠标"按钮
4. 重复步骤2-3配置另一个鼠标
5. 检测完成后点击"应用配置"保存设置"""
        
        instruction_label = ttk.Label(instruction_frame, text=instruction_text, justify=tk.LEFT)
        instruction_label.pack()
        
        # 控制按钮框架
        control_frame = ttk.LabelFrame(mouse_id_frame, text="检测控制", padding="10")
        control_frame.pack(fill=tk.X, padx=10, pady=5)
        
        # 按钮布局
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
        
        clear_btn = ttk.Button(
            button_frame, 
            text="清空结果", 
            command=self.clear_detection_results
        )
        clear_btn.pack(side=tk.LEFT, padx=5)
        
        apply_btn = ttk.Button(
            button_frame, 
            text="应用配置", 
            command=self.apply_mouse_config
        )
        apply_btn.pack(side=tk.RIGHT)
        
        # 当前检测状态
        status_frame = ttk.Frame(control_frame)
        status_frame.pack(fill=tk.X, pady=(10, 0))
        
        ttk.Label(status_frame, text="状态:").pack(side=tk.LEFT)
        self.detection_status_var = tk.StringVar(value="未开始")
        self.detection_status_label = ttk.Label(
            status_frame, 
            textvariable=self.detection_status_var,
            foreground="blue"
        )
        self.detection_status_label.pack(side=tk.LEFT, padx=(5, 0))
        
        # 检测结果区域
        result_frame = ttk.LabelFrame(mouse_id_frame, text="检测结果", padding="10")
        result_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        
        # 当前检测到的鼠标
        current_mouse_frame = ttk.LabelFrame(result_frame, text="当前检测到的鼠标", padding="10")
        current_mouse_frame.pack(fill=tk.X, pady=(0, 10))
        
        self.current_mouse_var = tk.StringVar(value="移动鼠标以检测...")
        current_mouse_label = ttk.Label(
            current_mouse_frame, 
            textvariable=self.current_mouse_var,
            wraplength=500,
            justify=tk.LEFT
        )
        current_mouse_label.pack(anchor=tk.W)
        
        # 按钮框架
        mouse_buttons_frame = ttk.Frame(current_mouse_frame)
        mouse_buttons_frame.pack(fill=tk.X, pady=(10, 0))
        
        self.set_left_btn = ttk.Button(
            mouse_buttons_frame, 
            text="设为左手鼠标", 
            command=self.set_as_left_mouse,
            state=tk.DISABLED
        )
        self.set_left_btn.pack(side=tk.LEFT, padx=(0, 5))
        
        self.set_right_btn = ttk.Button(
            mouse_buttons_frame, 
            text="设为右手鼠标", 
            command=self.set_as_right_mouse,
            state=tk.DISABLED
        )
        self.set_right_btn.pack(side=tk.LEFT)
        
        # 已配置的鼠标
        config_frame = ttk.LabelFrame(result_frame, text="已配置的鼠标", padding="10")
        config_frame.pack(fill=tk.BOTH, expand=True)
        
        # 左手鼠标
        left_frame = ttk.Frame(config_frame)
        left_frame.pack(fill=tk.X, pady=(0, 5))
        
        ttk.Label(left_frame, text="左手鼠标:", font=("Microsoft YaHei", 9, "bold")).pack(anchor=tk.W)
        self.left_mouse_config_var = tk.StringVar(value="未配置")
        self.left_mouse_config_label = ttk.Label(
            left_frame, 
            textvariable=self.left_mouse_config_var,
            wraplength=500,
            justify=tk.LEFT,
            foreground="green"
        )
        self.left_mouse_config_label.pack(anchor=tk.W, padx=(10, 0))
        
        # 右手鼠标
        right_frame = ttk.Frame(config_frame)
        right_frame.pack(fill=tk.X)
        
        ttk.Label(right_frame, text="右手鼠标:", font=("Microsoft YaHei", 9, "bold")).pack(anchor=tk.W)
        self.right_mouse_config_var = tk.StringVar(value="未配置")
        self.right_mouse_config_label = ttk.Label(
            right_frame, 
            textvariable=self.right_mouse_config_var,
            wraplength=500,
            justify=tk.LEFT,
            foreground="green"
        )
        self.right_mouse_config_label.pack(anchor=tk.W, padx=(10, 0))
        
        # 保存检测窗口和控件的引用
        self.mouse_detection_window = help_window
        self.detect_btn = detect_btn
        self.clear_btn = clear_btn
        self.current_detected_mouse = None
        
        # 初始化显示当前配置
        self.update_mouse_config_display()
        
        # 窗口关闭时停止检测
        help_window.protocol("WM_DELETE_WINDOW", lambda: (
            self.stop_mouse_detection() if hasattr(self, 'detection_running') and self.detection_running else None,
            help_window.destroy()
        ))
    
    def start_mouse_detection(self):
        """开始鼠标ID检测"""
        if self.detection_running:
            return
            
        self.detection_running = True
        self.current_detected_mouse = None
        
        # 初始化检测队列
        import queue
        self.detection_queue = queue.Queue()
        
        # 重置UI状态
        self.detection_status_var.set("检测中... 请移动鼠标")
        self.current_mouse_var.set("移动鼠标以检测...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        
        # 启动队列处理
        self.process_detection_queue()
        
        try:
            # 使用现有的helper来检测鼠标移动
            if not hasattr(self.helper, 'enabled') or not self.helper.enabled:
                # 临时启用helper来进行检测
                self.temp_detection_mode = True
                if not self.helper.enable():
                    raise Exception("无法启用鼠标检测")
            
        except Exception as e:
            self.detection_running = False
            self.detection_status_var.set(f"启动失败: {e}")
    
    def process_detection_queue(self):
        """处理检测队列中的鼠标事件"""
        if not self.detection_running:
            return
            
        try:
            # 非阻塞方式检查队列
            import queue
            while not self.detection_queue.empty():
                try:
                    device_name = self.detection_queue.get_nowait()
                    self.on_mouse_detected(device_name)
                except queue.Empty:
                    break
        except Exception as e:
            print(f"处理检测队列异常: {e}")
        
        # 继续处理队列
        if self.detection_running:
            self.root.after(50, self.process_detection_queue)  # 每50ms检查一次队列
    
    def on_mouse_detected(self, device_name):
        """当检测到鼠标移动时调用"""
        if not self.detection_running:
            return
            
        self.current_detected_mouse = device_name
        
        # 显示简化的设备名称
        short_name = self.get_short_device_name(device_name)
        self.current_mouse_var.set(f"检测到: {short_name}")
        
        # 启用配置按钮
        self.set_left_btn.config(state=tk.NORMAL)
        self.set_right_btn.config(state=tk.NORMAL)
        
        self.detection_status_var.set("检测成功！选择鼠标用途")
    
    def get_short_device_name(self, device_name):
        """获取设备的简化名称"""
        if not device_name:
            return "未知设备"
        
        # 提取关键信息
        if "VID" in device_name and "PID" in device_name:
            import re
            vid_match = re.search(r'VID[_&]([0-9A-F]{4})', device_name, re.IGNORECASE)
            pid_match = re.search(r'PID[_&]([0-9A-F]{4})', device_name, re.IGNORECASE)
            
            if vid_match and pid_match:
                return f"鼠标设备 (VID:{vid_match.group(1)}, PID:{pid_match.group(1)})"
        
        # 如果是蓝牙HID设备
        if "00001812-0000-1000-8000-00805f9b34fb" in device_name:
            return "蓝牙鼠标设备"
        
        return "鼠标设备"
    
    def set_as_left_mouse(self):
        """设置为左手鼠标"""
        if not self.current_detected_mouse:
            return
            
        self.config.config['left_mouse_id'] = self.current_detected_mouse
        self.update_mouse_config_display()
        
        self.detection_status_var.set("左手鼠标已配置")
        self.current_mouse_var.set("继续移动其他鼠标以配置...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.current_detected_mouse = None
    
    def set_as_right_mouse(self):
        """设置为右手鼠标"""
        if not self.current_detected_mouse:
            return
            
        self.config.config['right_mouse_id'] = self.current_detected_mouse
        self.update_mouse_config_display()
        
        self.detection_status_var.set("右手鼠标已配置")
        self.current_mouse_var.set("继续移动其他鼠标以配置...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.current_detected_mouse = None
    
    def update_mouse_config_display(self):
        """更新鼠标配置显示"""
        # 显示左手鼠标配置
        left_id = self.config.config.get('left_mouse_id', '')
        if left_id:
            short_name = self.get_short_device_name(left_id)
            self.left_mouse_config_var.set(f"已配置: {short_name}")
        else:
            self.left_mouse_config_var.set("未配置")
        
        # 显示右手鼠标配置
        right_id = self.config.config.get('right_mouse_id', '')
        if right_id:
            short_name = self.get_short_device_name(right_id)
            self.right_mouse_config_var.set(f"已配置: {short_name}")
        else:
            self.right_mouse_config_var.set("未配置")
    
    def clear_detection_results(self):
        """清空检测结果"""
        self.current_detected_mouse = None
        self.current_mouse_var.set("移动鼠标以检测...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.detection_status_var.set("已清空")
    
    def apply_mouse_config(self):
        """应用鼠标配置"""
        # 更新主界面的鼠标ID输入框
        try:
            # 获取主界面的Text控件引用
            for widget in self.root.winfo_children():
                if isinstance(widget, ttk.Frame):
                    for child in widget.winfo_children():
                        if isinstance(child, ttk.LabelFrame) and "鼠标设备 ID" in child.cget('text'):
                            text_widgets = []
                            for subchild in child.winfo_children():
                                if isinstance(subchild, tk.Text):
                                    text_widgets.append(subchild)
                            
                            # 更新左手鼠标ID
                            if len(text_widgets) >= 1:
                                left_text = text_widgets[0]
                                left_text.delete('1.0', tk.END)
                                left_text.insert('1.0', self.config.config.get('left_mouse_id', ''))
                            
                            # 更新右手鼠标ID
                            if len(text_widgets) >= 2:
                                right_text = text_widgets[1]
                                right_text.delete('1.0', tk.END)
                                right_text.insert('1.0', self.config.config.get('right_mouse_id', ''))
                            break
        except Exception as e:
            print(f"更新主界面失败: {e}")
        
        # 保存配置
        self.config.save_config()
        
        # 显示成功消息
        from tkinter import messagebox
        messagebox.showinfo("配置应用", "鼠标配置已应用并保存！\n请重新启用助手以使新配置生效。")
        
        self.detection_status_var.set("配置已应用")
    
    def stop_mouse_detection(self):
        """停止鼠标ID检测"""
        if not self.detection_running:
            return
            
        self.detection_running = False
        
        # 清理检测队列
        if hasattr(self, 'detection_queue'):
            try:
                import queue
                while not self.detection_queue.empty():
                    self.detection_queue.get_nowait()
            except:
                pass
        
        # 重置UI状态
        self.detection_status_var.set("检测已停止")
        self.current_mouse_var.set("移动鼠标以检测...")
        self.set_left_btn.config(state=tk.DISABLED)
        self.set_right_btn.config(state=tk.DISABLED)
        self.current_detected_mouse = None
        
        # 如果是临时启用的helper，考虑是否需要禁用
        if hasattr(self, 'temp_detection_mode') and self.temp_detection_mode:
            # 检查主程序是否需要helper运行
            if not self.enabled_var.get():
                self.helper.disable()
            self.temp_detection_mode = False
    
    def cleanup(self):
        """清理资源并恢复系统默认鼠标速度"""
        print("开始清理程序资源...")
        
        # 停止鼠标检测
        if hasattr(self, 'detection_running') and self.detection_running:
            self.stop_mouse_detection()
        
        if hasattr(self, 'helper'):
            # 如果helper存在，强制重置状态并禁用
            try:
                self.helper.reset_to_normal_state()
                self.helper.disable()
                print("Helper已禁用")
            except Exception as e:
                print(f"禁用helper失败: {e}")
            
        # 多次尝试恢复鼠标速度，确保成功
        for attempt in range(5):  # 增加尝试次数到5次
            try:
                # 使用配置中的默认速度，如果没有则使用系统默认10
                target_speed = getattr(self.config, 'config', {}).get('default_mouse_speed', 10)
                
                # 第一次尝试使用配置的速度，后续尝试用系统默认值
                if attempt >= 2:
                    target_speed = 10
                    
                ctypes.windll.user32.SystemParametersInfoW(0x0071, 0, target_speed, 0)
                
                # 等待设置生效
                import time
                time.sleep(0.1)
                
                # 验证是否设置成功
                current_speed = ctypes.c_int()
                ctypes.windll.user32.SystemParametersInfoW(0x0070, 0, ctypes.byref(current_speed), 0)
                
                if current_speed.value == target_speed:
                    print(f"退出时成功恢复鼠标速度: {target_speed}")
                    break
                else:
                    print(f"尝试 {attempt + 1}: 设置速度为 {target_speed}, 实际为 {current_speed.value}")
                    
            except Exception as e:
                print(f"尝试 {attempt + 1} 恢复鼠标速度失败: {e}")
                
            # 如果前面失败，最后一次使用硬编码的系统默认值并强制多次设置
            if attempt == 4:
                try:
                    for i in range(3):
                        ctypes.windll.user32.SystemParametersInfoW(0x0071, 0, 10, 0)
                        time.sleep(0.05)
                    print("使用硬编码默认值10强制恢复鼠标速度")
                except:
                    print("所有恢复鼠标速度的尝试都失败了")
        
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