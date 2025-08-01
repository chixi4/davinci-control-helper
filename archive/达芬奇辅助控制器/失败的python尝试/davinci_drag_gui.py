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
            "speed_check_interval": 1.0
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
    def __init__(self, config):
        self.config = config
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
            
            # 检查是否离开了目标应用 - 移除目标进程检查
            # if self.current_state != self.State.NORMAL:
            #     ap = self.get_active_process_name()
            #     if not ap or not ap.endswith(self.config.config['target_process']):
            #         print("检测到离开目标应用，重置状态")
            #         self.reset_to_normal_state()
    
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
            
            is_left = self.config.config['left_mouse_id'].lower() in dev_name.lower()
            is_right = self.config.config['right_mouse_id'].lower() in dev_name.lower()
            
            if self.current_state == self.State.NORMAL and is_left:
                # 移除目标进程检查，直接启用拖拽
                self.set_pointer_speed(self.config.config['drag_speed'])
                self.current_state = self.State.DRAG
                self.last_left_move_time = time.time()
                self.cumulative_dx = self.cumulative_dy = 0.0
                win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
                return 0
            
            elif self.current_state == self.State.DRAG:
                if is_left:
                    self.last_left_move_time = time.time()
                    self.cumulative_dx += ri.data.mouse.lLastX
                    self.cumulative_dy += ri.data.mouse.lLastY
                    dx, dy = int(self.cumulative_dx), int(self.cumulative_dy)
                    if dx or dy:
                        win32api.mouse_event(win32con.MOUSEEVENTF_MOVE, dx, dy, 0, 0)
                        self.cumulative_dx -= dx
                        self.cumulative_dy -= dy
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
        while self.running:
            try:
                # 确保速度保护机制工作
                self.ensure_speed_protection()
                
                if self.current_state == self.State.DRAG:
                    if (time.time() - self.last_left_move_time) * 1000 > self.config.config['idle_timeout']:
                        self.current_state = self.State.WAIT_CONFIRM
                time.sleep(0.01)
            except Exception as e:
                print(f"状态监控异常: {e}")
                # 出现异常时重置状态
                self.reset_to_normal_state()
    
    def message_loop(self):
        while self.running:
            try:
                win32gui.PumpWaitingMessages()
                time.sleep(0.001)
            except:
                break
    
    def enable(self):
        if self.enabled:
            return True
            
        try:
            # 使用配置的默认速度而不是系统当前值，避免被异常状态污染
            self.original_speed = self.config.config['default_mouse_speed']
            
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
            
            self.monitor_thread = threading.Thread(target=self.state_monitor, daemon=True)
            self.monitor_thread.start()
            
            self.message_thread = threading.Thread(target=self.message_loop, daemon=True)
            self.message_thread.start()
            
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

class DavinciDragGUI:
    def __init__(self):
        self.config = Config()
        self.helper = DavinciDragHelper(self.config)
        
        self.root = tk.Tk()
        self.root.title("达芬奇双鼠标拖拽助手")
        self.root.geometry("500x600")
        self.root.resizable(False, False)
        
        atexit.register(self.cleanup)
        
        self.create_widgets()
        
        if self.config.config['enabled']:
            self.enabled_var.set(True)
            self.toggle_helper()
    
    def create_widgets(self):
        main_frame = ttk.Frame(self.root, padding="15")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
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
        button_frame.grid(row=4, column=0, columnspan=2, pady=(10, 0))
        
        save_btn = ttk.Button(button_frame, text="保存配置", command=self.save_config)
        save_btn.grid(row=0, column=0, padx=(0, 10))
        
        reset_btn = ttk.Button(button_frame, text="重置配置", command=self.reset_config)
        reset_btn.grid(row=0, column=1, padx=(0, 10))
        
        # 添加紧急恢复按钮
        emergency_btn = ttk.Button(
            button_frame, 
            text="紧急恢复鼠标速度", 
            command=self.emergency_restore_speed
        )
        emergency_btn.grid(row=0, column=2, padx=(0, 10))
        
        help_btn = ttk.Button(button_frame, text="帮助", command=self.show_help)
        help_btn.grid(row=0, column=3)
        
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

鼠标ID获取：
运行 find_mouse_id.py 获取您的鼠标设备ID

功能改进：
• 增加了鼠标速度保护机制，每秒自动检查和恢复
• 添加了离开目标应用时的自动状态重置
• 新增"紧急恢复鼠标速度"按钮，解决速度卡死问题

注意事项：
• 需要两个独立的鼠标设备
• 如果启用失败，可能需要管理员权限
• 仅在目标程序激活时生效
• 如遇到鼠标速度异常，点击"紧急恢复"按钮
"""
        messagebox.showinfo("帮助", help_text)
    
    def cleanup(self):
        if hasattr(self, 'helper'):
            self.helper.disable()
    
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