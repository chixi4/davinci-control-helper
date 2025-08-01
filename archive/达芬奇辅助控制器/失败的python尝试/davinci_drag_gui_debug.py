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
            "direction_correction": True,
            "correction_threshold": 2.0,  # 新增：校正阈值，越小校正越严格
            "debug_mode": False  # 新增：调试模式
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
                        corrected_dx = raw_dx
                        corrected_dy = raw_dy
                        
                        # 调试模式：显示原始值
                        if self.config.config.get('debug_mode', False):
                            print(f"原始移动: dx={raw_dx}, dy={raw_dy}")
                        
                        # 检查是否启用方向校正
                        if self.config.config.get('direction_correction', True):
                            threshold = self.config.config.get('correction_threshold', 2.0)
                            
                            # 更严格的方向校正算法
                            abs_dx = abs(raw_dx)
                            abs_dy = abs(raw_dy)
                            
                            if abs_dy > abs_dx * threshold:
                                # 主要是垂直移动，强制水平为0
                                corrected_dx = 0
                                corrected_dy = raw_dy
                                if self.config.config.get('debug_mode', False):
                                    print(f"垂直校正: {raw_dx},{raw_dy} -> {corrected_dx},{corrected_dy}")
                            elif abs_dx > abs_dy * threshold:
                                # 主要是水平移动，强制垂直为0
                                corrected_dx = raw_dx
                                corrected_dy = 0
                                if self.config.config.get('debug_mode', False):
                                    print(f"水平校正: {raw_dx},{raw_dy} -> {corrected_dx},{corrected_dy}")
                            # 如果是对角线移动（两个方向都有显著移动），保持原样
                        
                        # 使用校正后的增量
                        win32api.mouse_event(win32con.MOUSEEVENTF_MOVE, corrected_dx, corrected_dy, 0, 0)
                        
                        # 调试模式：显示最终值
                        if self.config.config.get('debug_mode', False):
                            print(f"最终移动: dx={corrected_dx}, dy={corrected_dy}")
                    
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
        self.root.geometry("500x800")  # 再次增加高度
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
            text="达芬奇双鼠标拖拽助手 - 调试版", 
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
        
        # 添加校正阈值设置
        ttk.Label(config_frame, text="校正阈值:").grid(row=5, column=0, sticky=tk.W, pady=(5, 5))
        self.correction_threshold_var = tk.DoubleVar(value=self.config.config.get('correction_threshold', 2.0))
        threshold_spin = ttk.Spinbox(
            config_frame,
            from_=1.0,
            to=5.0,
            increment=0.1,
            textvariable=self.correction_threshold_var,
            width=15,
            command=self.update_config
        )
        threshold_spin.grid(row=5, column=1, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        threshold_spin.bind('<KeyRelease>', self.update_config)
        
        # 添加调试模式
        ttk.Label(config_frame, text="调试模式:").grid(row=6, column=0, sticky=tk.W, pady=(5, 5))
        self.debug_mode_var = tk.BooleanVar(value=self.config.config.get('debug_mode', False))
        debug_check = ttk.Checkbutton(
            config_frame,
            text="启用调试输出 (控制台显示移动数据)",
            variable=self.debug_mode_var,
            command=self.update_config
        )
        debug_check.grid(row=6, column=1, columnspan=2, sticky=tk.W, padx=(10, 0), pady=(5, 5))
        
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
        self.config.config['correction_threshold'] = self.correction_threshold_var.get()
        self.config.config['debug_mode'] = self.debug_mode_var.get()
        
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
            self.correction_threshold_var.set(self.config.config.get('correction_threshold', 2.0))
            self.debug_mode_var.set(self.config.config.get('debug_mode', False))
            
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
        help_text = """达芬奇双鼠标拖拽助手 - 调试版

新增功能：
• 校正阈值：调整方向校正的敏感度（1.0=最严格，5.0=最宽松）
• 调试模式：在控制台显示鼠标移动的原始数据和校正结果

使用建议：
1. 启用调试模式，观察控制台输出的鼠标移动数据
2. 如果发现垂直移动时有水平偏移，可以调低校正阈值到1.0-1.5
3. 如果校正太严格影响斜角拖拽，可以调高阈值到3.0-5.0

调试步骤：
1. 勾选"启用调试输出"
2. 启用拖拽助手
3. 在达芬奇中测试垂直拖拽
4. 观察控制台输出的移动数据
5. 根据数据调整校正阈值
"""
        messagebox.showinfo("帮助", help_text)
    
    def cleanup(self):
        """清理资源并恢复系统默认鼠标速度"""
        print("开始清理程序资源...")
        
        if hasattr(self, 'helper'):
            # 如果helper存在，强制重置状态并禁用
            try:
                self.helper.reset_to_normal_state()
                self.helper.disable()
                print("Helper已禁用")
            except Exception as e:
                print(f"禁用helper失败: {e}")
            
        # 多次尝试恢复鼠标速度，确保成功
        for attempt in range(5):
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