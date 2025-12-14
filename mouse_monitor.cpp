/*
 * Mouse Monitor - 纯用户态版本
 *
 * 功能：
 * - 通过 Raw Input API 读取鼠标移动数据
 * - 从 ExtraInformation 字段解码原始移动量（需要 rawaccel 启用 setExtraInfo）
 * - 支持注册特定鼠标，只响应该鼠标的移动
 * - 按 P 键开启/关闭自动左键功能（切换）
 * - 双击 Caps Lock 执行完整重置（回到注册模式）
 * - 检测到鼠标移动时自动按下左键，停止移动时松开
 *
 * 编译：
 *   cl /EHsc /O2 mouse_monitor.cpp /link user32.lib /out:mouse_monitor.exe
 *
 * 使用前提：
 *   需要在 settings.json 中添加 "setExtraInfo": true
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <conio.h>
#include <string>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

// ========== 全局变量 ==========
HWND g_hWnd = NULL;

// 状态机定义
enum class LockState { IDLE, LOCKED, UNLOCKABLE };

// 线程安全的原子变量
std::atomic<bool> g_running(true);
std::atomic<bool> g_ipcMode(false);
std::atomic<bool> g_powerEnabled(false);
std::atomic<bool> g_featureEnabled(false);
std::atomic<bool> g_isMouseDown(false);
std::atomic<DWORD> g_lastRegisteredMoveTime(0);  // 注册鼠标最后移动时间
std::atomic<DWORD> g_cooldownUntil(0);           // 冷却期结束时间
std::atomic<LockState> g_lockState(LockState::IDLE);
std::atomic<bool> g_otherMouseActive(false);     // 其他鼠标是否活跃

LONG g_moveCount = 0;
short g_lastRawX = 0;
short g_lastRawY = 0;
std::atomic<bool> g_extraInfoValid(false);  // ExtraInformation 是否有效

// 设备注册相关
std::atomic<HANDLE> g_registeredDevice(nullptr);
std::atomic<HANDLE> g_pendingDevice(nullptr);
std::atomic<bool> g_registrationMode(true);
wchar_t g_pendingDevicePath[512];
wchar_t g_registeredDevicePath[512];
std::string g_registeredHardwareId;
double g_currentSensitivity = 1.0;
std::string g_settingsPath;
std::string g_statePath;

// IPC queues (used in --ipc mode)
std::mutex g_cmdMutex;
std::queue<std::string> g_cmdQueue;
std::mutex g_evtMutex;
std::queue<std::string> g_evtQueue;

// Registration scan accumulator (IPC mode auto-register)
std::mutex g_scanMutex;
std::unordered_map<HANDLE, float> g_scanAccum;
float g_scanTotalAccum = 0.0f;
std::atomic<HANDLE> g_lastScanEmitDevice(nullptr);
std::atomic<DWORD> g_lastScanEmitTick(0);

// 低级鼠标钩子
HHOOK g_mouseHook = NULL;
std::atomic<bool> g_blockingMouse(false);  // 是否正在阻止鼠标移动

// 常量
const DWORD STOP_TO_UNLOCK_MS = 50;     // 停止后进入 UNLOCKABLE 的阈值
const LONG DEADZONE_THRESHOLD = 3;      // 其他鼠标死区阈值 |dx|+|dy|
const char* SETTINGS_FILE = "settings.json";
const char* SENS_PROFILE_NAME = "sens_registered_mouse";

// ========== 函数声明 ==========
void MouseLeftDown();
void MouseLeftUp();
void QueueEvent(const std::string& line);
void FlushEvents();
void StartIpcStdinThread();
void ProcessIpcCommands();
void HandleIpcCommand(const std::string& line);
bool ApplySensitivityMultiplier(double multiplier, std::string& errorMsg);
bool RestoreDefaultSensitivity(std::string& errorMsg);
std::string TrimString(const std::string& s);
std::string NormalizeIpcLine(const std::string& line);
void DecodeExtraInfo(ULONG extraInfo, short* rawX, short* rawY);
void SetCursorVisible(bool visible);
void GetDeviceHidPath(HANDLE device, wchar_t* path, size_t pathSize);
std::string DevicePathToHardwareId(const wchar_t* devicePath);
std::string WideToAnsi(const std::wstring& ws);
bool ReadFileContent(const char* path, std::string& content);
bool WriteFileContent(const char* path, const std::string& content);
bool SaveLastRegisteredHardwareId(const std::string& hardwareId);
bool LoadLastRegisteredHardwareId(std::string& hardwareId);
void ClearLastRegisteredHardwareId();
bool TryRestoreLastRegisteredMouse();
bool UpdateSettingsForDevice(const std::string& hardwareId, double sensitivity, std::string& errorMsg);
bool RunWriterExe();
void HandleSensitivityInput();
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
DWORD WINAPI MessageLoopThread(LPVOID lpParam);

// 新增函数声明
bool IsOtherMouseMovementSignificant(LONG dx, LONG dy);
void MoveCursorBy(LONG dx, LONG dy);
DWORD GetCooldownDuration();
void EnterLockedState();
void EnterUnlockableState();
void ReleaseToIdle();
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
bool InstallMouseHook();
void UninstallMouseHook();
void FailsafeCleanup();
void PerformFullReset();
bool RemoveOldSensDeviceMappings(std::string& content, const std::string& currentHardwareId);

// 模拟鼠标左键按下
void MouseLeftDown() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

// 模拟鼠标左键抬起
void MouseLeftUp() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void QueueEvent(const std::string& line) {
    if (!g_ipcMode.load()) return;
    std::lock_guard<std::mutex> lock(g_evtMutex);
    g_evtQueue.push(line);
}

void FlushEvents() {
    if (!g_ipcMode.load()) return;

    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(g_evtMutex);
        std::swap(local, g_evtQueue);
    }

    while (!local.empty()) {
        printf("%s\n", local.front().c_str());
        local.pop();
    }
    fflush(stdout);
}

static std::string ToUpperAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

void StartIpcStdinThread() {
    if (!g_ipcMode.load()) return;

    std::thread([]() {
        std::string line;
        while (g_running.load() && std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lock(g_cmdMutex);
            g_cmdQueue.push(line);
        }
    }).detach();
}

void ProcessIpcCommands() {
    if (!g_ipcMode.load()) return;

    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        std::swap(local, g_cmdQueue);
    }

    while (!local.empty()) {
        HandleIpcCommand(local.front());
        local.pop();
    }
}

bool ApplySensitivityMultiplier(double multiplier, std::string& errorMsg) {
    if (g_registeredDevice.load() == NULL) {
        errorMsg = "no mouse registered";
        return false;
    }
    if (g_registeredHardwareId.empty()) {
        errorMsg = "hardware id not available";
        return false;
    }

    std::string updateErr;
    if (!UpdateSettingsForDevice(g_registeredHardwareId, multiplier, updateErr)) {
        errorMsg = updateErr;
        return false;
    }

    if (!RunWriterExe()) {
        errorMsg = "writer.exe failed";
        return false;
    }

    g_currentSensitivity = multiplier;
    return true;
}

bool RestoreDefaultSensitivity(std::string& errorMsg) {
    if (g_settingsPath.empty()) {
        errorMsg = "settings path not set";
        return false;
    }

    std::string content;
    if (!ReadFileContent(g_settingsPath.c_str(), content)) {
        errorMsg = "failed to read settings.json";
        return false;
    }

    if (!RemoveOldSensDeviceMappings(content, std::string())) {
        errorMsg = "failed to clear device mappings";
        return false;
    }

    if (!WriteFileContent(g_settingsPath.c_str(), content)) {
        errorMsg = "failed to write settings.json";
        return false;
    }

    if (!RunWriterExe()) {
        errorMsg = "writer.exe failed";
        return false;
    }

    return true;
}

void HandleIpcCommand(const std::string& line) {
    std::string trimmed = TrimString(NormalizeIpcLine(line));
    if (trimmed.empty()) return;

    std::istringstream iss(trimmed);
    std::string cmd;
    iss >> cmd;
    cmd = ToUpperAscii(cmd);

    if (cmd == "PING") {
        QueueEvent("EVT PONG");
        return;
    }

    if (cmd == "QUIT") {
        QueueEvent("EVT EXITING");
        FlushEvents();
        FailsafeCleanup();
        QueueEvent("EVT EXITED");
        FlushEvents();
        g_running.store(false);
        return;
    }

    if (cmd == "RESET") {
        PerformFullReset();
        return;
    }

    if (cmd == "POWER") {
        std::string arg;
        iss >> arg;
        arg = ToUpperAscii(arg);

        if (arg == "ON") {
            g_powerEnabled.store(true);
            QueueEvent("EVT POWER ON");

            if (!g_registeredHardwareId.empty()) {
                std::string err;
                if (!ApplySensitivityMultiplier(g_currentSensitivity, err)) {
                    QueueEvent(std::string("EVT NOTIFY ERR:") + err);
                }
            } else {
                QueueEvent("EVT NOTIFY ERR:NO MOUSE REGISTERED");
            }

            QueueEvent("EVT POWER_APPLIED ON");
            return;
        }

        if (arg == "OFF") {
            g_powerEnabled.store(false);
            g_featureEnabled.store(false);
            ReleaseToIdle();
            QueueEvent("EVT POWER OFF");
            QueueEvent("EVT FEATURE OFF");

            std::string err;
            if (!RestoreDefaultSensitivity(err)) {
                QueueEvent(std::string("EVT NOTIFY ERR:") + err);
            }

            QueueEvent("EVT POWER_APPLIED OFF");
            return;
        }

        QueueEvent("EVT NOTIFY ERR:INVALID PARAMETER");
        return;
    }

    if (cmd == "FEATURE") {
        std::string arg;
        iss >> arg;
        arg = ToUpperAscii(arg);

        if (arg == "ON") {
            if (!g_powerEnabled.load()) {
                QueueEvent("EVT NOTIFY ERR:POWER OFF");
                return;
            }
            g_featureEnabled.store(true);
            QueueEvent("EVT FEATURE ON");
            return;
        }

        if (arg == "OFF") {
            g_featureEnabled.store(false);
            ReleaseToIdle();
            QueueEvent("EVT FEATURE OFF");
            return;
        }

        QueueEvent("EVT NOTIFY ERR:INVALID PARAMETER");
        return;
    }

    if (cmd == "SET_SENS") {
        double value = 0.0;
        if (!(iss >> value)) {
            QueueEvent("EVT NOTIFY ERR:INVALID PARAMETER");
            return;
        }

        if (value < 0.001) value = 0.001;
        if (value > 100.0) value = 100.0;
        g_currentSensitivity = value;

        if (g_powerEnabled.load() && !g_registeredHardwareId.empty()) {
            std::string err;
            if (!ApplySensitivityMultiplier(value, err)) {
                QueueEvent(std::string("EVT NOTIFY ERR:") + err);
            }
        }

        {
            char buf[64] = {0};
            snprintf(buf, sizeof(buf), "EVT SENS_APPLIED %.3f", value);
            QueueEvent(buf);
        }
        return;
    }

    QueueEvent("EVT NOTIFY ERR:UNKNOWN COMMAND");
}

// ========== 状态机辅助函数 ==========

// 判断其他鼠标的移动是否超过死区（用于过滤抖动）
bool IsOtherMouseMovementSignificant(LONG dx, LONG dy) {
    return (std::abs(dx) + std::abs(dy)) >= DEADZONE_THRESHOLD;
}

// 手动移动光标（用于注册鼠标控制光标）
void MoveCursorBy(LONG dx, LONG dy) {
    if (dx == 0 && dy == 0) return;

    POINT pt;
    if (!GetCursorPos(&pt)) return;

    pt.x += dx;
    pt.y += dy;
    SetCursorPos(pt.x, pt.y);
}

// 获取冷却期时长（使用系统双击时间）
DWORD GetCooldownDuration() {
    DWORD cooldown = GetDoubleClickTime();
    if (cooldown == 0) cooldown = 500;
    return cooldown;
}

// 进入 LOCKED 状态：按下左键，开始阻止其他鼠标
void EnterLockedState() {
    bool wasDown = g_isMouseDown.exchange(true);
    if (!wasDown) {
        MouseLeftDown();
        QueueEvent("EVT FIRING ON");
    }
    g_lastRegisteredMoveTime.store(GetTickCount());
    g_lockState.store(LockState::LOCKED);
    g_blockingMouse.store(true);
}

// 进入 UNLOCKABLE 状态：等待其他鼠标移动来触发释放
void EnterUnlockableState() {
    if (g_lockState.load() == LockState::LOCKED) {
        g_lockState.store(LockState::UNLOCKABLE);
    }
}

// 释放到 IDLE 状态：抬起左键，停止阻止，设置冷却期
void ReleaseToIdle() {
    bool wasDown = g_isMouseDown.exchange(false);
    if (wasDown) {
        MouseLeftUp();
        QueueEvent("EVT FIRING OFF");
    }
    g_lockState.store(LockState::IDLE);
    g_blockingMouse.store(false);
    g_cooldownUntil.store(GetTickCount() + GetCooldownDuration());
}

// 安全清理：确保程序退出时不会留下按住的左键
void FailsafeCleanup() {
    static std::atomic<bool> s_cleanupRan{false};
    if (s_cleanupRan.exchange(true)) {
        return;
    }

    bool wasDown = g_isMouseDown.exchange(false);
    if (wasDown) {
        MouseLeftUp();
        QueueEvent("EVT FIRING OFF");
    }
    g_blockingMouse.store(false);
    g_lockState.store(LockState::IDLE);
    UninstallMouseHook();

    // 退出时恢复鼠标灵敏度：清理 settings.json 中的设备映射
    std::string content;
    if (ReadFileContent(g_settingsPath.c_str(), content)) {
        if (RemoveOldSensDeviceMappings(content, std::string())) {
            if (WriteFileContent(g_settingsPath.c_str(), content)) {
                if (!g_ipcMode.load()) {
                    printf("\n[EXIT] Restored mouse sensitivity (cleared device mappings)\n");
                }
                RunWriterExe();  // 应用配置
            }
        }
    }
}

// 双击 Caps Lock 触发的完整重置：
// - 恢复灵敏度为 1.0（如有已注册设备则同步 settings.json 并尝试 writer.exe 应用）
// - 关闭自动按左键功能并释放按键
// - 取消注册设备，回到注册模式
void PerformFullReset() {
    if (!g_ipcMode.load()) {
        printf("\n[RESET] Full reset triggered (Caps Lock double-press)\n");
        fflush(stdout);
    }

    if (g_ipcMode.load()) {
        g_powerEnabled.store(false);
    }

    std::string hardwareId = g_registeredHardwareId; // 先拷贝，后面会清空注册信息

    // 关闭自动按键功能并确保释放
    g_featureEnabled.store(false);
    ReleaseToIdle();

    // 恢复灵敏为 1.0（程序内状态）
    g_currentSensitivity = 1.0;

    // 清理 settings.json 中映射到 sens_registered_mouse 的设备，避免残留映射影响其他鼠标
    {
        std::string content;
        if (ReadFileContent(g_settingsPath.c_str(), content)) {
            if (RemoveOldSensDeviceMappings(content, std::string())) {
                if (!WriteFileContent(g_settingsPath.c_str(), content)) {
                    printf("[RESET] [WARN] Failed to write settings.json while clearing device mappings.\n");
                } else {
                    printf("[RESET] Cleared device mappings for profile: %s\n", SENS_PROFILE_NAME);
                    printf("[RESET] Running writer.exe to apply configuration...\n");
                    if (!RunWriterExe()) {
                        printf("[RESET] [WARN] writer.exe may have failed. Check if RawAccel is running.\n");
                    }
                }
            } else {
                printf("[RESET] [WARN] Failed to clear device mappings (devices array parse failed).\n");
            }
        } else {
            printf("[RESET] [WARN] Failed to read settings.json while clearing device mappings.\n");
        }
        fflush(stdout);
    }

    // 取消注册并回到注册模式
    g_registeredDevice.store(NULL);
    g_pendingDevice.store(NULL);
    g_registrationMode.store(true);
    g_pendingDevicePath[0] = L'\0';
    g_registeredDevicePath[0] = L'\0';
    g_registeredHardwareId.clear();
    ClearLastRegisteredHardwareId();

    // 重置状态机/统计相关状态
    g_lastRegisteredMoveTime.store(0);
    g_cooldownUntil.store(0);
    g_lockState.store(LockState::IDLE);
    g_blockingMouse.store(false);
    g_otherMouseActive.store(false);
    g_extraInfoValid.store(false);
    g_moveCount = 0;
    g_lastRawX = 0;
    g_lastRawY = 0;

    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_scanAccum.clear();
        g_scanTotalAccum = 0.0f;
        g_lastScanEmitDevice.store(nullptr);
        g_lastScanEmitTick.store(0);
    }
    QueueEvent("EVT SCAN_PROGRESS 0.0");
    QueueEvent("EVT SENS_APPLIED 1.0");
    QueueEvent("EVT RESET");
    QueueEvent("EVT POWER OFF");
    QueueEvent("EVT FEATURE OFF");

    if (!g_ipcMode.load()) {
        printf("[REGISTER] Move the mouse you want to register...\n\n");
        fflush(stdout);
    }
}

// ========== 低级鼠标钩子 ==========

// 低级鼠标钩子回调：阻止物理鼠标移动，放行注入事件
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    // 只处理鼠标移动事件
    if (wParam != WM_MOUSEMOVE) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    // 如果没有启用阻止，直接放行
    if (!g_blockingMouse.load()) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (!info) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    // 放行注入的事件（包括我们的 SetCursorPos）
    if (info->flags & LLMHF_INJECTED) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    // 阻止物理鼠标的移动
    return 1;
}

// 安装低级鼠标钩子
bool InstallMouseHook() {
    if (g_mouseHook != NULL) return true;
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
    return g_mouseHook != NULL;
}

// 卸载低级鼠标钩子
void UninstallMouseHook() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }
}

// 解码 ExtraInformation 获取原始移动量
// rawaccel 驱动将原始 X,Y 编码为: 低16位=X, 高16位=Y
void DecodeExtraInfo(ULONG extraInfo, short* rawX, short* rawY) {
    *rawX = (short)(extraInfo & 0xFFFF);
    *rawY = (short)((extraInfo >> 16) & 0xFFFF);
}

// 设置控制台光标可见性
void SetCursorVisible(bool visible) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) return;

    CONSOLE_CURSOR_INFO cursorInfo;
    if (GetConsoleCursorInfo(hConsole, &cursorInfo)) {
        cursorInfo.bVisible = visible;
        SetConsoleCursorInfo(hConsole, &cursorInfo);
    }
}

// 获取设备的 HID 路径
void GetDeviceHidPath(HANDLE device, wchar_t* path, size_t pathSize) {
    path[0] = L'\0';
    UINT size = 0;

    // 首先获取需要的缓冲区大小
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, NULL, &size);
    if (size == 0 || size > pathSize) return;

    // 获取设备名称（HID 路径）
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path, &size);
}

// ========== 灵敏度调整相关函数 ==========

// 宽字符转ANSI
std::string WideToAnsi(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int required = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    if (required <= 0) return std::string();
    std::string result(static_cast<size_t>(required) - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, &result[0], required, NULL, NULL);
    return result;
}

// 将 RawInput 的设备路径转换为 RawAccel 驱动使用的硬件ID格式
// 输入: \\?\HID#VID_1532&PID_0067&MI_00#8&12345678&0&0000#{...GUID...}
// 输出: HID\VID_1532&PID_0067&MI_00
std::string DevicePathToHardwareId(const wchar_t* devicePath) {
    if (!devicePath || devicePath[0] == L'\0') return std::string();

    std::wstring path(devicePath);

    // 跳过 \\?\ 或 \\??\ 前缀
    const std::wstring prefixWin32 = L"\\\\?\\";
    const std::wstring prefixNt = L"\\\\??\\";
    size_t start = 0;
    if (path.compare(0, prefixWin32.size(), prefixWin32) == 0) {
        start = prefixWin32.size();
    } else if (path.compare(0, prefixNt.size(), prefixNt) == 0) {
        start = prefixNt.size();
    }

    // 找到第一个 # (VID_...&PID_...&MI_... 之前的分隔符)
    size_t firstHash = path.find(L'#', start);
    if (firstHash == std::wstring::npos) return std::string();

    // 找到第二个 # (实例ID之前的分隔符)
    size_t secondHash = path.find(L'#', firstHash + 1);
    if (secondHash == std::wstring::npos) return std::string();

    // 提取 HID#VID_...&PID_...&MI_... 部分
    std::wstring segment = path.substr(start, secondHash - start);

    // 将第一个 # 替换为 \ (HID#... -> HID\...)
    for (size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] == L'#') {
            segment[i] = L'\\';
            break;
        }
    }

    return WideToAnsi(segment);
}

// 读取文件内容
bool ReadFileContent(const char* path, std::string& content) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    content = ss.str();
    return true;
}

// 写入文件内容
bool WriteFileContent(const char* path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file << content;
    return true;
}

// 去除字符串首尾空白
std::string TrimString(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

bool SaveLastRegisteredHardwareId(const std::string& hardwareId) {
    if (hardwareId.empty() || g_statePath.empty()) return false;
    return WriteFileContent(g_statePath.c_str(), hardwareId + "\n");
}

bool LoadLastRegisteredHardwareId(std::string& hardwareId) {
    if (g_statePath.empty()) return false;
    std::string content;
    if (!ReadFileContent(g_statePath.c_str(), content)) return false;
    hardwareId = TrimString(content);
    return !hardwareId.empty();
}

void ClearLastRegisteredHardwareId() {
    if (g_statePath.empty()) return;
    DeleteFileA(g_statePath.c_str());
}

bool TryRestoreLastRegisteredMouse() {
    std::string hardwareId;
    if (!LoadLastRegisteredHardwareId(hardwareId)) return false;

    UINT count = 0;
    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) {
        return false;
    }

    std::vector<RAWINPUTDEVICELIST> list(static_cast<size_t>(count));
    UINT got = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == static_cast<UINT>(-1)) {
        return false;
    }

    for (UINT i = 0; i < got; ++i) {
        HANDLE device = list[i].hDevice;
        if (!device) continue;

        wchar_t path[512] = {0};
        GetDeviceHidPath(device, path, sizeof(path) / sizeof(wchar_t));
        if (path[0] == L'\0') continue;

        std::string id = DevicePathToHardwareId(path);
        if (id.empty()) continue;
        if (id != hardwareId) continue;

        g_registeredDevice.store(device);
        wcscpy_s(g_registeredDevicePath, path);
        g_registeredHardwareId = id;
        g_registrationMode.store(false);
        return true;
    }

    return false;
}

// Normalize stdin lines in IPC mode:
// - PowerShell may pipe UTF-16LE/BE strings to native executables.
// - Some tools may include UTF-8 BOM.
std::string NormalizeIpcLine(const std::string& line) {
    std::string s = line;

    // Strip UTF-8 BOM if present.
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s = s.substr(3);
    }

    const bool hasUtf16Bom =
        s.size() >= 2 &&
        ((static_cast<unsigned char>(s[0]) == 0xFF && static_cast<unsigned char>(s[1]) == 0xFE) ||
         (static_cast<unsigned char>(s[0]) == 0xFE && static_cast<unsigned char>(s[1]) == 0xFF));

    const bool hasNul = s.find('\0') != std::string::npos;

    if (hasUtf16Bom || hasNul) {
        bool littleEndian = true;
        size_t start = 0;

        if (hasUtf16Bom) {
            littleEndian = (static_cast<unsigned char>(s[0]) == 0xFF);
            start = 2;
        } else {
            // Heuristic: UTF-16LE ASCII tends to have NULs at odd indices; UTF-16BE at even indices.
            size_t zerosEven = 0;
            size_t zerosOdd = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\0') {
                    if ((i & 1) == 0) zerosEven++;
                    else zerosOdd++;
                }
            }
            littleEndian = zerosOdd >= zerosEven;
        }

        std::string decoded;
        decoded.reserve(s.size() / 2);
        for (size_t i = start; i + 1 < s.size(); i += 2) {
            const unsigned char b0 = static_cast<unsigned char>(s[i]);
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned short u = littleEndian ? static_cast<unsigned short>(b0 | (b1 << 8))
                                                  : static_cast<unsigned short>(b1 | (b0 << 8));

            if (u == 0) continue;
            if (u <= 0x7F) {
                decoded.push_back(static_cast<char>(u));
                continue;
            }
            if (u == 0xFEFF) continue; // BOM codepoint
            decoded.push_back('?');
        }

        s.swap(decoded);
    }

    // Defensive: strip any remaining NULs.
    std::string filtered;
    filtered.reserve(s.size());
    for (char c : s) {
        if (c != '\0') filtered.push_back(c);
    }
    return filtered;
}

// 在JSON中查找数组的范围 [arrayStart, arrayEnd]
bool FindJsonArrayRange(const std::string& content, const std::string& key, size_t& arrayStart, size_t& arrayEnd) {
    std::string token = "\"" + key + "\"";
    size_t pos = content.find(token);
    if (pos == std::string::npos) return false;

    size_t bracket = content.find('[', pos);
    if (bracket == std::string::npos) return false;

    int depth = 0;
    for (size_t i = bracket; i < content.size(); ++i) {
        if (content[i] == '[') depth++;
        else if (content[i] == ']') {
            depth--;
            if (depth == 0) {
                arrayStart = bracket;
                arrayEnd = i;
                return true;
            }
        }
    }
    return false;
}

// 在JSON中查找对象的范围 {objStart, objEnd}
bool FindNextJsonObject(const std::string& content, size_t searchStart, size_t boundary, size_t& objStart, size_t& objEnd) {
    size_t brace = content.find('{', searchStart);
    if (brace == std::string::npos || brace > boundary) return false;

    int depth = 0;
    for (size_t i = brace; i <= boundary && i < content.size(); ++i) {
        if (content[i] == '{') depth++;
        else if (content[i] == '}') {
            depth--;
            if (depth == 0) {
                objStart = brace;
                objEnd = i;
                return true;
            }
        }
    }
    return false;
}

// 从JSON对象中提取字符串字段值
bool ExtractJsonStringField(const std::string& obj, const std::string& field, std::string& value) {
    std::string key = "\"" + field + "\"";
    size_t pos = obj.find(key);
    if (pos == std::string::npos) return false;

    pos = obj.find(':', pos);
    if (pos == std::string::npos) return false;

    pos = obj.find('"', pos);
    if (pos == std::string::npos) return false;

    size_t end = obj.find('"', pos + 1);
    if (end == std::string::npos) return false;

    value = obj.substr(pos + 1, end - pos - 1);
    return true;
}

// 替换JSON中的数字字段值
bool ReplaceJsonNumberField(std::string& content, const std::string& field, double value) {
    std::string key = "\"" + field + "\"";
    size_t pos = content.find(key);
    if (pos == std::string::npos) return false;

    pos = content.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;

    // 跳过空白
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) pos++;

    // 找到数字的结束位置
    size_t end = pos;
    while (end < content.size() &&
           (std::isdigit(static_cast<unsigned char>(content[end])) ||
            content[end] == '-' || content[end] == '+' ||
            content[end] == '.' || content[end] == 'e' || content[end] == 'E')) {
        end++;
    }

    // 格式化新的数字值
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(1);
    ss << value;

    content.replace(pos, end - pos, ss.str());
    return true;
}

// 复制profile并修改Output DPI
bool CreateOrUpdateSensProfile(std::string& content, double outputDpi, std::string& errorMsg) {
    size_t arrStart, arrEnd;
    if (!FindJsonArrayRange(content, "profiles", arrStart, arrEnd)) {
        errorMsg = "profiles array not found";
        return false;
    }

    // 遍历所有profile，查找模板和是否已存在目标profile
    size_t search = arrStart;
    std::string templateProfile;
    bool profileExists = false;
    size_t existingProfileStart = 0, existingProfileEnd = 0;

    while (true) {
        size_t objStart, objEnd;
        if (!FindNextJsonObject(content, search, arrEnd, objStart, objEnd)) break;

        std::string obj = content.substr(objStart, objEnd - objStart + 1);
        std::string name;
        ExtractJsonStringField(obj, "name", name);

        if (name == SENS_PROFILE_NAME) {
            profileExists = true;
            existingProfileStart = objStart;
            existingProfileEnd = objEnd;
        }

        // 使用第一个profile作为模板
        if (templateProfile.empty()) {
            templateProfile = obj;
        }

        search = objEnd + 1;
    }

    if (profileExists) {
        // 更新已存在的profile
        std::string existingObj = content.substr(existingProfileStart, existingProfileEnd - existingProfileStart + 1);
        if (!ReplaceJsonNumberField(existingObj, "Output DPI", outputDpi)) {
            errorMsg = "failed to update Output DPI in existing profile";
            return false;
        }
        content.replace(existingProfileStart, existingProfileEnd - existingProfileStart + 1, existingObj);
    } else {
        // 创建新的profile
        if (templateProfile.empty()) {
            errorMsg = "no profile template found";
            return false;
        }

        std::string newProfile = templateProfile;

        // 替换name字段
        std::string oldName;
        ExtractJsonStringField(newProfile, "name", oldName);
        size_t namePos = newProfile.find("\"name\"");
        if (namePos != std::string::npos) {
            size_t valueStart = newProfile.find('"', namePos + 6);
            size_t valueEnd = newProfile.find('"', valueStart + 1);
            if (valueStart != std::string::npos && valueEnd != std::string::npos) {
                newProfile.replace(valueStart + 1, valueEnd - valueStart - 1, SENS_PROFILE_NAME);
            }
        }

        // 替换Output DPI
        if (!ReplaceJsonNumberField(newProfile, "Output DPI", outputDpi)) {
            errorMsg = "failed to set Output DPI in new profile";
            return false;
        }

        // 插入新profile
        std::string insertion = ",\n    " + newProfile;

        // 需要重新计算arrEnd，因为content可能已经被修改
        if (!FindJsonArrayRange(content, "profiles", arrStart, arrEnd)) {
            errorMsg = "profiles array not found after modification";
            return false;
        }

        // 找到最后一个}的位置
        size_t insertPos = content.rfind('}', arrEnd);
        if (insertPos != std::string::npos && insertPos > arrStart) {
            content.insert(insertPos + 1, insertion);
        }
    }

    return true;
}

// 删除 devices 数组中映射到 sens_registered_mouse 的旧设备：
// - 若 currentHardwareId 非空：删除所有 profile==sens_registered_mouse 且 id!=currentHardwareId 的设备；
//   同时若存在重复 currentHardwareId 条目，仅保留第一个。
// - 若 currentHardwareId 为空：删除所有 profile==sens_registered_mouse 的设备。
bool RemoveOldSensDeviceMappings(std::string& content, const std::string& currentHardwareId) {
    size_t arrStart = 0, arrEnd = 0;
    if (!FindJsonArrayRange(content, "devices", arrStart, arrEnd)) {
        // 没有 devices 数组，视为无需清理
        return true;
    }

    // 转义 currentHardwareId 以匹配 settings.json 中的 id 字段（反斜杠在 JSON 中被转义为 \\\\）
    std::string escapedCurrentId;
    if (!currentHardwareId.empty()) {
        for (size_t i = 0; i < currentHardwareId.size(); ++i) {
            if (currentHardwareId[i] == '\\') {
                escapedCurrentId += "\\\\";
            } else {
                escapedCurrentId += currentHardwareId[i];
            }
        }
    }

    struct EraseRange { size_t start; size_t end; };
    std::vector<EraseRange> removals;

    bool keptCurrent = false;
    size_t search = arrStart;
    while (true) {
        size_t objStart = 0, objEnd = 0;
        if (!FindNextJsonObject(content, search, arrEnd, objStart, objEnd)) break;

        std::string obj = content.substr(objStart, objEnd - objStart + 1);
        std::string profile;
        if (!ExtractJsonStringField(obj, "profile", profile) || profile != SENS_PROFILE_NAME) {
            search = objEnd + 1;
            continue;
        }

        std::string devId;
        ExtractJsonStringField(obj, "id", devId);

        bool shouldRemove = false;
        if (currentHardwareId.empty()) {
            shouldRemove = true;
        } else if (devId != escapedCurrentId) {
            shouldRemove = true;
        } else {
            // devId == escapedCurrentId
            if (keptCurrent) {
                // 同一设备的重复条目，仅保留第一个
                shouldRemove = true;
            } else {
                keptCurrent = true;
            }
        }

        if (shouldRemove) {
            size_t removeStart = objStart;
            size_t removeEnd = objEnd;

            // 优先吞掉"前置逗号 + 空白"，否则吞掉"后置逗号 + 空白"
            while (removeStart > (arrStart + 1) &&
                   std::isspace(static_cast<unsigned char>(content[removeStart - 1]))) {
                removeStart--;
            }

            if (removeStart > (arrStart + 1) && content[removeStart - 1] == ',') {
                removeStart--; // 吞掉前置逗号
                while (removeStart > (arrStart + 1) &&
                       std::isspace(static_cast<unsigned char>(content[removeStart - 1]))) {
                    removeStart--;
                }
            } else {
                size_t pos = removeEnd + 1;
                while (pos < content.size() &&
                       std::isspace(static_cast<unsigned char>(content[pos]))) {
                    pos++;
                }
                if (pos < content.size() && pos <= arrEnd && content[pos] == ',') {
                    removeEnd = pos; // 吞掉后置逗号
                    size_t pos2 = removeEnd + 1;
                    while (pos2 < content.size() &&
                           std::isspace(static_cast<unsigned char>(content[pos2]))) {
                        pos2++;
                    }
                    if (pos2 > removeEnd + 1) {
                        removeEnd = pos2 - 1; // 也吞掉逗号后的空白，避免留下多余空行/缩进
                    }
                }
            }

            removals.push_back({removeStart, removeEnd});
        }

        search = objEnd + 1;
    }

    for (auto it = removals.rbegin(); it != removals.rend(); ++it) {
        if (it->end >= it->start && it->end < content.size()) {
            content.erase(it->start, it->end - it->start + 1);
        }
    }

    return true;
}

// 在devices数组中添加或更新设备映射
bool AddOrUpdateDeviceMapping(std::string& content, const std::string& hardwareId, std::string& errorMsg) {
    // 先清理旧映射，避免多个设备共享同一 sens_registered_mouse profile 导致"调一个全都变"
    if (!RemoveOldSensDeviceMappings(content, hardwareId)) {
        errorMsg = "failed to remove old sens_registered_mouse device mappings";
        return false;
    }
    size_t arrStart, arrEnd;
    if (!FindJsonArrayRange(content, "devices", arrStart, arrEnd)) {
        errorMsg = "devices array not found";
        return false;
    }

    // 先转义反斜杠用于JSON匹配和写入
    std::string escapedId;
    for (size_t i = 0; i < hardwareId.size(); ++i) {
        if (hardwareId[i] == '\\') {
            escapedId += "\\\\";
        } else {
            escapedId += hardwareId[i];
        }
    }

    // 检查devices数组是否为空
    std::string arrContent = content.substr(arrStart, arrEnd - arrStart + 1);
    bool isEmpty = (arrContent.find('{') == std::string::npos);

    // 遍历现有设备，检查是否已存在
    size_t search = arrStart;
    bool deviceExists = false;
    size_t existingDevStart = 0, existingDevEnd = 0;
    size_t lastObjEnd = 0;

    while (!isEmpty) {
        size_t objStart, objEnd;
        if (!FindNextJsonObject(content, search, arrEnd, objStart, objEnd)) break;
        lastObjEnd = objEnd;

        std::string obj = content.substr(objStart, objEnd - objStart + 1);
        std::string devId;
        ExtractJsonStringField(obj, "id", devId);

        // JSON中的反斜杠已被转义，使用escapedId比较
        if (devId == escapedId) {
            deviceExists = true;
            existingDevStart = objStart;
            existingDevEnd = objEnd;
            break;
        }

        search = objEnd + 1;
    }

    // 构建设备配置JSON
    std::string deviceJson =
        "{\n"
        "      \"name\": \"Registered Mouse\",\n"
        "      \"profile\": \"" + std::string(SENS_PROFILE_NAME) + "\",\n"
        "      \"id\": \"" + escapedId + "\",\n"
        "      \"config\": {\n"
        "        \"disable\": false,\n"
        "        \"setExtraInfo\": true,\n"
        "        \"Use constant time interval based on polling rate\": false,\n"
        "        \"DPI (normalizes input speed unit: counts/ms -> in/s)\": 0,\n"
        "        \"Polling rate Hz (keep at 0 for automatic adjustment)\": 0\n"
        "      }\n"
        "    }";

    if (deviceExists) {
        // 替换现有设备配置
        content.replace(existingDevStart, existingDevEnd - existingDevStart + 1, deviceJson);
    } else {
        // 需要重新获取数组范围
        if (!FindJsonArrayRange(content, "devices", arrStart, arrEnd)) {
            errorMsg = "devices array not found";
            return false;
        }

        if (isEmpty) {
            // 数组为空，直接插入
            std::string insertion = "\n    " + deviceJson + "\n  ";
            content.insert(arrStart + 1, insertion);
        } else {
            // 数组非空，在最后一个对象后插入
            // 重新查找最后一个对象
            search = arrStart;
            lastObjEnd = 0;
            while (true) {
                size_t objStart, objEnd;
                if (!FindNextJsonObject(content, search, arrEnd, objStart, objEnd)) break;
                lastObjEnd = objEnd;
                search = objEnd + 1;
            }

            if (lastObjEnd > 0) {
                std::string insertion = ",\n    " + deviceJson;
                content.insert(lastObjEnd + 1, insertion);
            }
        }
    }

    return true;
}

// 更新settings.json为指定设备设置灵敏度
bool UpdateSettingsForDevice(const std::string& hardwareId, double sensitivity, std::string& errorMsg) {
    std::string content;
    if (!ReadFileContent(g_settingsPath.c_str(), content)) {
        errorMsg = "failed to read settings.json";
        return false;
    }

    // 限制灵敏度范围
    double clamped = sensitivity;
    if (clamped < 0.001) clamped = 0.001;
    if (clamped > 100.0) clamped = 100.0;

    // 计算Output DPI (灵敏度 * 1000)
    double outputDpi = clamped * 1000.0;

    // 创建或更新灵敏度profile
    if (!CreateOrUpdateSensProfile(content, outputDpi, errorMsg)) {
        return false;
    }

    // 添加或更新设备映射
    if (!AddOrUpdateDeviceMapping(content, hardwareId, errorMsg)) {
        return false;
    }

    // 写回文件
    if (!WriteFileContent(g_settingsPath.c_str(), content)) {
        errorMsg = "failed to write settings.json";
        return false;
    }

    return true;
}

// 运行writer.exe应用配置
bool RunWriterExe() {
    char modulePath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, modulePath, MAX_PATH)) {
        return false;
    }

    // 获取程序所在目录
    std::string path(modulePath);
    size_t slash = path.find_last_of("\\/");
    std::string dir = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    std::string writerPath = dir + "writer.exe";
    std::string settingsPath = g_settingsPath.empty() ? (dir + SETTINGS_FILE) : g_settingsPath;

    // 构建命令行: writer.exe <settings file path>
    std::string cmdLine = "\"" + writerPath + "\" \"" + settingsPath + "\"";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // 创建进程
    if (!CreateProcessA(NULL, const_cast<char*>(cmdLine.c_str()),
                        NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    // 等待进程完成（最多5秒）
    WaitForSingleObject(pi.hProcess, 5000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

// 处理灵敏度输入
void HandleSensitivityInput() {
    if (g_registeredDevice.load() == NULL) {
        printf("\n[WARN] No mouse registered. Please register a mouse first.\n");
        return;
    }

    if (g_registeredHardwareId.empty()) {
        printf("\n[WARN] Hardware ID not available for registered device.\n");
        return;
    }

    SetCursorVisible(true);
    printf("\n============================================\n");
    printf("[SENS] Current sensitivity: %.3fx\n", g_currentSensitivity);
    printf("[SENS] Enter new multiplier (0.001 - 100), or 'r' to reset to 1.0\n");
    printf("[SENS] Input: ");
    fflush(stdout);

    // 读取用户输入
    char inputBuf[64] = {0};
    if (fgets(inputBuf, sizeof(inputBuf), stdin) == NULL) {
        SetCursorVisible(false);
        printf("[SENS] Input cancelled.\n");
        return;
    }

    std::string input = TrimString(std::string(inputBuf));
    SetCursorVisible(false);

    if (input.empty()) {
        printf("[SENS] Input cancelled.\n");
        return;
    }

    double multiplier = 1.0;

    if (input == "r" || input == "R") {
        multiplier = 1.0;
        printf("[SENS] Resetting to 1.0x\n");
    } else {
        errno = 0;
        char* endPtr = NULL;
        multiplier = strtod(input.c_str(), &endPtr);

        if (endPtr == input.c_str() || *endPtr != '\0' || errno == ERANGE) {
            printf("[ERROR] Invalid input: %s\n", input.c_str());
            return;
        }

        if (multiplier < 0.001 || multiplier > 100.0) {
            printf("[WARN] Value clamped to valid range (0.001 - 100)\n");
            if (multiplier < 0.001) multiplier = 0.001;
            if (multiplier > 100.0) multiplier = 100.0;
        }
    }

    printf("[SENS] Applying %.3fx sensitivity for device: %s\n", multiplier, g_registeredHardwareId.c_str());

    std::string errorMsg;
    if (!UpdateSettingsForDevice(g_registeredHardwareId, multiplier, errorMsg)) {
        printf("[ERROR] Failed to update settings: %s\n", errorMsg.c_str());
        return;
    }

    printf("[SENS] Running writer.exe to apply configuration...\n");
    if (!RunWriterExe()) {
        printf("[WARN] writer.exe may have failed. Check if RawAccel is running.\n");
    } else {
        printf("[SENS] Configuration applied successfully!\n");
    }

    g_currentSensitivity = multiplier;
    printf("[SENS] New sensitivity: %.3fx (Output DPI: %.1f)\n", multiplier, multiplier * 1000.0);
    printf("============================================\n\n");
}

// 窗口过程 - 处理 WM_INPUT 消息
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        // PERF(P0): 消除每包 new/delete（高频 WM_INPUT 下会引入堆锁竞争/抖动）
        // 预期改进：1000Hz 输入下显著降低 jitter，减少 CPU/堆分配开销峰值
        thread_local std::vector<BYTE> buffer;
        if (buffer.size() < sizeof(RAWINPUT)) {
            buffer.resize(sizeof(RAWINPUT));
        }

        UINT size = static_cast<UINT>(buffer.size());
        UINT copied = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
        if (copied == static_cast<UINT>(-1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(size);
            copied = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
        }

        if (copied == size && size > 0) {
            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());

            if (raw->header.dwType == RIM_TYPEMOUSE) {
                HANDLE deviceHandle = raw->header.hDevice;
                bool isRegistrationMode = g_registrationMode.load();

                // 注册模式：检测移动的鼠标并显示设备信息
                if (isRegistrationMode) {
                    bool hasMovement = (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0);
                    bool isRelative = !(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE);
                    if (!hasMovement || !isRelative) {
                        return 0;
                    }

                    if (g_ipcMode.load()) {
                        const LONG dx = raw->data.mouse.lLastX;
                        const LONG dy = raw->data.mouse.lLastY;
                        const float delta = static_cast<float>(std::abs(dx) + std::abs(dy));
                        if (delta <= 0.0f) {
                            return 0;
                        }

                        const float kScanThreshold = 2000.0f;
                        const DWORD kScanEmitIntervalMs = 10;
                        float totalProgress = 0.0f;
                        HANDLE winnerDevice = deviceHandle;

                        {
                            std::lock_guard<std::mutex> lock(g_scanMutex);
                            float& acc = g_scanAccum[deviceHandle];
                            acc += delta;
                            g_scanTotalAccum += delta;

                            totalProgress = (g_scanTotalAccum / kScanThreshold) * 100.0f;
                            if (totalProgress > 100.0f) totalProgress = 100.0f;

                            if (totalProgress >= 100.0f) {
                                float bestAcc = acc;
                                winnerDevice = deviceHandle;
                                for (const auto& kv : g_scanAccum) {
                                    if (kv.second > bestAcc) {
                                        bestAcc = kv.second;
                                        winnerDevice = kv.first;
                                    }
                                }
                            }
                        }

                        const DWORD now = GetTickCount();
                        DWORD lastEmit = g_lastScanEmitTick.load();
                        HANDLE lastDev = g_lastScanEmitDevice.load();
                        const bool deviceChanged = (lastDev != deviceHandle);

                        bool shouldEmit = (totalProgress >= 100.0f || lastEmit == 0 || deviceChanged ||
                                           (now - lastEmit) >= kScanEmitIntervalMs);

                        if (shouldEmit) {
                            g_lastScanEmitTick.store(now);
                            g_lastScanEmitDevice.store(deviceHandle);
                            char buf[64] = {0};
                            snprintf(buf, sizeof(buf), "EVT SCAN_PROGRESS %.2f", totalProgress);
                            QueueEvent(buf);

                            // 立即 flush：确保首次/换设备时 UI 立刻看到反馈（避免“要晃两下”）
                            if ((lastEmit == 0 || deviceChanged) && totalProgress > 0.0f) {
                                FlushEvents();
                            }
                        }

                        if (totalProgress >= 100.0f) {
                            bool expected = true;
                            if (!g_registrationMode.compare_exchange_strong(expected, false)) {
                                return 0;
                            }

                            g_registeredDevice.store(winnerDevice);
                            GetDeviceHidPath(winnerDevice, g_registeredDevicePath, sizeof(g_registeredDevicePath)/sizeof(wchar_t));
                            g_registeredHardwareId = DevicePathToHardwareId(g_registeredDevicePath);

                            if (!g_registeredHardwareId.empty() && !g_settingsPath.empty()) {
                                std::string content;
                                if (ReadFileContent(g_settingsPath.c_str(), content)) {
                                    if (RemoveOldSensDeviceMappings(content, g_registeredHardwareId)) {
                                        WriteFileContent(g_settingsPath.c_str(), content);
                                    }
                                }
                            }

                            if (!g_registeredHardwareId.empty()) {
                                SaveLastRegisteredHardwareId(g_registeredHardwareId);
                                QueueEvent(std::string("EVT REGISTERED ") + g_registeredHardwareId);
                            } else {
                                QueueEvent("EVT NOTIFY ERR:HWID NOT FOUND");
                                QueueEvent("EVT REGISTERED ");
                            }
                        }
                    } else {
                        if (deviceHandle != g_pendingDevice.load()) {
                            g_pendingDevice.store(deviceHandle);
                            GetDeviceHidPath(deviceHandle, g_pendingDevicePath, sizeof(g_pendingDevicePath)/sizeof(wchar_t));

                            printf("\r                                                                              \r");
                            printf("[DETECT] Device: 0x%p\n", deviceHandle);
                            if (g_pendingDevicePath[0] != L'\0') {
                                wprintf(L"         Path: %ls\n", g_pendingDevicePath);
                            }
                            printf("         Press Y to register this mouse, N to skip\n");
                            fflush(stdout);
                        }
                    }
                }
                // 正常模式
                else {
                    HANDLE registeredDevice = g_registeredDevice.load();
                    bool isRelative = !(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE);

                    // 其他鼠标的移动
                    if (registeredDevice != NULL && deviceHandle != registeredDevice) {
                        if (isRelative) {
                            LONG otherX = raw->data.mouse.lLastX;
                            LONG otherY = raw->data.mouse.lLastY;

                            // 记录其他鼠标是否活跃（超过死区）
                            if (IsOtherMouseMovementSignificant(otherX, otherY)) {
                                g_otherMouseActive.store(true);

                                // 在 UNLOCKABLE 状态下，其他鼠标移动触发释放
                                if (g_lockState.load() == LockState::UNLOCKABLE) {
                                    ReleaseToIdle();
                                }
                            }
                        }
                    }
                    // 注册鼠标的移动
                    else if (registeredDevice != NULL && deviceHandle == registeredDevice) {
                        if (isRelative) {
                            LONG accelX = raw->data.mouse.lLastX;
                            LONG accelY = raw->data.mouse.lLastY;

                            // 从 ExtraInformation 解码原始移动量
                            ULONG extraInfo = raw->data.mouse.ulExtraInformation;
                            short rawX = 0, rawY = 0;
                            DecodeExtraInfo(extraInfo, &rawX, &rawY);

                            // 更新 extraInfoValid 状态（修复永不重置的 bug）
                            if (extraInfo != 0 && (rawX != 0 || rawY != 0)) {
                                g_extraInfoValid.store(true);
                                g_lastRawX = rawX;
                                g_lastRawY = rawY;
                            } else if (extraInfo == 0) {
                                g_extraInfoValid.store(false);
                            }

                            // 判断是否有实际移动
                            bool extraInfoValid = g_extraInfoValid.load();
                            bool hasMoved = extraInfoValid ? (rawX != 0 || rawY != 0) : (accelX != 0 || accelY != 0);

                            if (hasMoved) {
                                g_moveCount++;
                                DWORD now = GetTickCount();
                                bool featureEnabled = g_featureEnabled.load() && g_powerEnabled.load();
                                LockState currentState = g_lockState.load();
                                DWORD cooldownUntil = g_cooldownUntil.load();

                                // PERF(P0): 限频控制台输出，避免每包 printf/fflush 造成阻塞和抖动
                                // 预期改进：将控制台 I/O 从 500/1000Hz 降到 10Hz（100ms），显著降低 WM_INPUT 处理时间波动
                                static DWORD s_lastPrintTick = 0;
                                const DWORD kPrintIntervalMs = 100;
                                bool shouldPrint = (s_lastPrintTick == 0) || ((DWORD)(now - s_lastPrintTick) >= kPrintIntervalMs);
                                if (shouldPrint) s_lastPrintTick = now;

                                if (shouldPrint && !g_ipcMode.load()) {
                                    // 显示移动信息
                                    const char* stateStr = "IDLE";
                                    if (currentState == LockState::LOCKED) stateStr = "LOCK";
                                    else if (currentState == LockState::UNLOCKABLE) stateStr = "UNLK";

                                    if (extraInfoValid) {
                                        printf("\r[RAW] X:%+4d Y:%+4d | Accel:(%+4ld,%+4ld) | %s | %s    ",
                                               rawX, rawY, accelX, accelY,
                                               featureEnabled ? "ON " : "OFF", stateStr);
                                    } else {
                                        printf("\r[ACCEL] X:%+4ld Y:%+4ld | %s | %s (no extraInfo)    ",
                                               accelX, accelY,
                                               featureEnabled ? "ON " : "OFF", stateStr);
                                    }
                                    fflush(stdout);
                                }

                                // 状态机逻辑
                                if (featureEnabled) {
                                    if (currentState == LockState::IDLE) {
                                        // 检查冷却期
                                        if (now >= cooldownUntil) {
                                            EnterLockedState();
                                        }
                                    } else if (currentState == LockState::LOCKED || currentState == LockState::UNLOCKABLE) {
                                        // 注册鼠标继续移动，保持/回到 LOCKED 状态
                                        g_lastRegisteredMoveTime.store(now);
                                        if (currentState == LockState::UNLOCKABLE) {
                                            g_lockState.store(LockState::LOCKED);
                                        }
                                    }

                                    // 手动移动光标（使用加速后的数据）
                                    MoveCursorBy(accelX, accelY);
                                }
                            }
                        }
                    }
                }
            }
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 消息循环线程
DWORD WINAPI MessageLoopThread(LPVOID lpParam) {
    // 创建隐藏窗口类
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RawInputMouseMonitor";

    if (!RegisterClassA(&wc)) {
        if (g_ipcMode.load()) {
            QueueEvent("EVT NOTIFY FS:OFFLINE");
            QueueEvent("EVT NOTIFY ERR:REGISTER CLASS FAILED");
        } else {
            printf("[ERROR] RegisterClass failed: %lu\n", GetLastError());
        }
        return 1;
    }

    // 创建消息窗口（不可见）
    g_hWnd = CreateWindowA(
        wc.lpszClassName,
        "Mouse Monitor",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,  // 消息窗口，不显示
        NULL,
        wc.hInstance,
        NULL
    );

    if (!g_hWnd) {
        if (g_ipcMode.load()) {
            QueueEvent("EVT NOTIFY FS:OFFLINE");
            QueueEvent("EVT NOTIFY ERR:CREATE WINDOW FAILED");
        } else {
            printf("[ERROR] CreateWindow failed: %lu\n", GetLastError());
        }
        return 1;
    }

    // 注册 Raw Input 设备（鼠标）
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;  // Generic Desktop
    rid.usUsage = 0x02;      // Mouse
    rid.dwFlags = RIDEV_INPUTSINK;  // 即使窗口不在前台也接收输入
    rid.hwndTarget = g_hWnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        if (g_ipcMode.load()) {
            QueueEvent("EVT NOTIFY FS:OFFLINE");
            QueueEvent("EVT NOTIFY ERR:REGISTER RAW INPUT FAILED");
        } else {
            printf("[ERROR] RegisterRawInputDevices failed: %lu\n", GetLastError());
        }
        return 1;
    }

    if (g_ipcMode.load()) {
        QueueEvent("EVT INPUT_READY");
        FlushEvents();
    }

    if (!g_ipcMode.load()) {
        printf("[OK] Raw Input registered\n");
    }

    // 安装低级鼠标钩子
    if (!InstallMouseHook()) {
        if (g_ipcMode.load()) {
            QueueEvent("EVT NOTIFY ERR:MOUSE HOOK FAILED");
        } else {
            printf("[WARN] Failed to install mouse hook: %lu (feature will work without blocking)\n", GetLastError());
        }
    } else {
        if (!g_ipcMode.load()) {
            printf("[OK] Low-level mouse hook installed\n");
        }
    }

    // 消息循环
    MSG msg;
    while (g_running.load() && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UninstallMouseHook();
    DestroyWindow(g_hWnd);
    return 0;
}

int main(int argc, char** argv) {
    // Default settings path: next to this executable.
    {
        char modulePath[MAX_PATH] = {0};
        if (GetModuleFileNameA(NULL, modulePath, MAX_PATH)) {
            std::string exePath(modulePath);
            size_t slash = exePath.find_last_of("\\/");
            std::string dir = (slash == std::string::npos) ? std::string() : exePath.substr(0, slash + 1);
            g_settingsPath = dir + SETTINGS_FILE;
        } else {
            g_settingsPath = SETTINGS_FILE;
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--ipc") {
            g_ipcMode.store(true);
            continue;
        }
        if (arg == "--settings" && (i + 1) < argc) {
            g_settingsPath = argv[++i];
            continue;
        }
    }

    // State file lives next to settings.json (portable).
    {
        std::string dir;
        size_t slash = g_settingsPath.find_last_of("\\/");
        if (slash != std::string::npos) {
            dir = g_settingsPath.substr(0, slash + 1);
        }
        g_statePath = dir + "registered_mouse.txt";
    }

    const bool restored = TryRestoreLastRegisteredMouse();

    // CLI mode has no separate "power" toggle; keep it enabled for existing behavior.
    if (!g_ipcMode.load()) {
        g_powerEnabled.store(true);
    } else {
        StartIpcStdinThread();
        QueueEvent("EVT READY");
        if (restored && !g_registeredHardwareId.empty()) {
            QueueEvent("EVT SCAN_PROGRESS 100.0");
            QueueEvent(std::string("EVT REGISTERED ") + g_registeredHardwareId);
        } else {
            QueueEvent("EVT SCAN_PROGRESS 0.0");
        }
        FlushEvents();
    }

    // 隐藏控制台光标，解决闪烁问题
    if (!g_ipcMode.load()) {
        SetCursorVisible(false);

        printf("=== Mouse Monitor (Pure User-Mode) ===\n");
    printf("\n");
    printf("This tool reads mouse movement via Raw Input API.\n");
    printf("For raw (unaccelerated) data, enable 'setExtraInfo' in settings.json\n");
    printf("\n");
    printf("Controls:\n");
    printf("  Y / N     - Register or skip mouse device (in registration mode)\n");
    printf("  L         - Set sensitivity for registered mouse (0.001x - 100x)\n");
    printf("  P         - Toggle auto-click feature\n");
    printf("  Caps Lock - Double-press to full reset\n");
    printf("  Q         - Quit\n");
    printf("\n");
    }

    // 启动消息循环线程
    HANDLE hThread = CreateThread(NULL, 0, MessageLoopThread, NULL, 0, NULL);
    if (!hThread) {
        if (g_ipcMode.load()) {
            QueueEvent("EVT NOTIFY FS:OFFLINE");
            QueueEvent("EVT NOTIFY ERR:MESSAGE THREAD FAILED");
            FlushEvents();
            return 1;
        } else {
            printf("[ERROR] Failed to create message thread: %lu\n", GetLastError());
            SetCursorVisible(true);
            return 1;
        }
    }

    // 等待窗口创建完成
    Sleep(100);

    if (!g_hWnd) {
        if (g_ipcMode.load()) {
            QueueEvent("EVT NOTIFY FS:OFFLINE");
            QueueEvent("EVT NOTIFY ERR:WINDOW NOT CREATED");
            FlushEvents();
        } else {
            printf("[ERROR] Window not created\n");
        }
        WaitForSingleObject(hThread, 1000);
        CloseHandle(hThread);
        if (!g_ipcMode.load()) {
            SetCursorVisible(true);
        }
        return 1;
    }

    if (!g_ipcMode.load() && g_registrationMode.load()) {
        printf("[REGISTER] Move the mouse you want to register...\n\n");
    }

    // 主循环
    g_featureEnabled.store(false);

    while (g_running.load()) {
        if (g_ipcMode.load()) {
            ProcessIpcCommands();
        }

        // 检查按键
        if (!g_ipcMode.load() && _kbhit()) {
            char ch = _getch();

            // 退出键
            if (ch == 'q' || ch == 'Q') {
                FailsafeCleanup();
                g_running.store(false);
                break;
            }

            // 灵敏度调整键（L）- 在非注册模式下可用
            if ((ch == 'l' || ch == 'L') && !g_registrationMode.load()) {
                HandleSensitivityInput();
                continue;
            }

            // 自动按左键功能开关（P）
            if (ch == 'p' || ch == 'P') {
                bool enabled = !g_featureEnabled.load();
                g_featureEnabled.store(enabled);
                if (!enabled) {
                    ReleaseToIdle();
                }
                printf("\n[AUTO-CLICK] %s\n", enabled ? "ENABLED" : "DISABLED");
                fflush(stdout);
                continue;
            }

            // 注册模式下的按键处理
            if (g_registrationMode.load()) {
                HANDLE pending = g_pendingDevice.load();
                if ((ch == 'y' || ch == 'Y') && pending != NULL) {
                    // 确认注册当前检测到的设备
                    g_registeredDevice.store(pending);
                    g_registrationMode.store(false);

                    // 保存设备路径并转换为硬件ID
                    if (g_pendingDevicePath[0] == L'\0') {
                        GetDeviceHidPath(pending, g_pendingDevicePath, sizeof(g_pendingDevicePath)/sizeof(wchar_t));
                    }
                    wcscpy_s(g_registeredDevicePath, g_pendingDevicePath);
                    g_registeredHardwareId = DevicePathToHardwareId(g_registeredDevicePath);
                    if (!g_registeredHardwareId.empty()) {
                        SaveLastRegisteredHardwareId(g_registeredHardwareId);
                    }

                    // 注册新鼠标时：清理 settings.json 中所有其他映射到 sens_registered_mouse 的设备，
                    // 只保留当前注册设备（若当前设备此前不存在映射，则清理后将不再保留任何旧映射）。
                    if (!g_registeredHardwareId.empty()) {
                        std::string content;
                        if (ReadFileContent(g_settingsPath.c_str(), content)) {
                            if (RemoveOldSensDeviceMappings(content, g_registeredHardwareId)) {
                                WriteFileContent(g_settingsPath.c_str(), content);
                            }
                        }
                    }

                    printf("\n[OK] Mouse registered: 0x%p\n", pending);
                    if (g_registeredDevicePath[0] != L'\0') {
                        wprintf(L"[PATH] %ls\n", g_registeredDevicePath);
                    }
                    if (!g_registeredHardwareId.empty()) {
                        printf("[HWID] %s\n", g_registeredHardwareId.c_str());
                    } else {
                        printf("[WARN] Could not extract hardware ID from device path.\n");
                    }
                    printf("[OK] Monitoring started. Press P to toggle auto-click, L to adjust sensitivity.\n\n");
                    fflush(stdout);
                } else if (ch == 'n' || ch == 'N') {
                    // 跳过当前设备，继续检测
                    g_pendingDevice.store(NULL);
                    g_pendingDevicePath[0] = L'\0';
                    printf("\n[REGISTER] Skipped. Move another mouse...\n\n");
                    fflush(stdout);
                }
                continue;
            }
        }

        // 双击 Caps Lock 检测（500ms 时间窗）
        // GetAsyncKeyState 低位：自上次调用后是否按下过该键（边沿事件）
        static DWORD s_lastCapsPressTick = 0;
        if (GetAsyncKeyState(VK_CAPITAL) & 0x0001) {
            DWORD now = GetTickCount();
            const DWORD kCapsDoublePressWindowMs = 500;
            if (s_lastCapsPressTick != 0 && (DWORD)(now - s_lastCapsPressTick) <= kCapsDoublePressWindowMs) {
                s_lastCapsPressTick = 0;
                PerformFullReset();
                continue;
            }
            s_lastCapsPressTick = now;
        }

        // 注册模式下只处理按键，跳过其他逻辑
        FlushEvents();
        if (g_registrationMode.load()) {
            Sleep(10);
            continue;
        }

        // 状态机：检查是否需要从 LOCKED 转换到 UNLOCKABLE
        LockState currentState = g_lockState.load();
        if (currentState == LockState::LOCKED) {
            DWORD lastMove = g_lastRegisteredMoveTime.load();
            DWORD now = GetTickCount();
            if (lastMove != 0 && (now - lastMove) >= STOP_TO_UNLOCK_MS) {
                EnterUnlockableState();
            }
        }

        // 重置其他鼠标活跃标志（用于下一轮检测）
        g_otherMouseActive.store(false);

        Sleep(1);
    }

    // 确保清理
    FailsafeCleanup();
    FlushEvents();

    // 停止消息循环
    if (g_hWnd) {
        PostMessage(g_hWnd, WM_QUIT, 0, 0);
    }
    WaitForSingleObject(hThread, 1000);
    CloseHandle(hThread);

    if (!g_ipcMode.load()) {
        // 恢复光标可见性
        SetCursorVisible(true);
        printf("\n\nMonitor stopped.\n");
    }
    return 0;
}
