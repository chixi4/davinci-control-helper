// <--- 修改点 1: 定义 UNICODE 宏，确保所有 Windows API 调用使用宽字符版本
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <winuser.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <psapi.h>
#include <algorithm>
#include <iomanip>
#include <conio.h>
#include <io.h>      // 新增：_setmode, _fileno
#include <fcntl.h>   // 新增：_O_U16TEXT
#include <locale>    // 新增：本地化（可选）

// 可调参数
const std::wstring LEFT_MOUSE_ID = L"\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
const std::wstring RIGHT_MOUSE_ID = L"\\?\\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
const std::wstring TARGET_PROCESS = L"Resolve.exe";
const int IDLE_TIMEOUT_MS = 20;
const double POLL_INTERVAL_SEC = 0.1;

// ===== 新增：控制台工具 =====
static void ClearConsoleScreen()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

    DWORD cells = csbi.dwSize.X * csbi.dwSize.Y;
    DWORD written = 0;
    COORD home{0, 0};

    FillConsoleOutputCharacterW(hOut, L' ', cells, home, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(hOut, home);
}

static void InitConsoleUnicode()
{
    // 将标准输入/输出切换到 UTF-16 文本模式，保证 wcout/wcin 正常显示/读取宽字符
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin),  _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    // 可选：把 C++ 宽流与全局区域设置绑定，避免本地化问题
    std::locale::global(std::locale(""));
    std::wcout.imbue(std::locale());
    std::wcin.imbue(std::locale());
    std::wcerr.imbue(std::locale());
}

// 鼠标灵敏度控制器类
class MouseSensitivityController {
private:
    int originalSpeed;
    bool settingsEnabled;

public:
    MouseSensitivityController() : originalSpeed(10), settingsEnabled(false) {
        originalSpeed = GetMouseSpeed();
    }

    ~MouseSensitivityController() {
        restoreOriginalSpeed();
    }

    int GetMouseSpeed() {
        int speed = 10;
        SystemParametersInfo(SPI_GETMOUSESPEED, 0, &speed, 0);
        return speed;
    }

    bool SetMouseSpeed(int speed) {
        // SPI_SETMOUSESPEED: pvParam 直接是整型值（1-20），通过指针大小的整数传入
        return SystemParametersInfo(
            SPI_SETMOUSESPEED,
            0,
            reinterpret_cast<PVOID>(static_cast<INT_PTR>(speed)),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
        ) != FALSE;
    }

    void restoreOriginalSpeed() {
        if (SetMouseSpeed(originalSpeed)) {
            std::wcout << L"已恢复原始鼠标速度 (" << originalSpeed << L")" << std::endl << std::flush;
        } else {
            std::wcout << L"警告: 无法恢复原始鼠标速度" << std::endl << std::flush;
        }
    }

    void showSettingsInterface() {
        ClearConsoleScreen();
        std::wcout << L"==========================================" << std::endl << std::flush;
        std::wcout << L"    双鼠标拖拽助手 v1.0 - 设置界面" << std::endl << std::flush;
        std::wcout << L"==========================================" << std::endl << std::endl << std::flush;

        int currentSpeed = GetMouseSpeed();
        double percentage = (double)currentSpeed / 20.0 * 100.0;

        std::wcout << L"当前鼠标速度: " << currentSpeed << L"/20 (" << std::fixed << std::setprecision(1) << percentage << L"%)" << std::endl << std::flush;
        std::wcout << L"原始速度: " << originalSpeed << L"/20" << std::endl << std::endl << std::flush;

        std::wcout << L"快捷设置 (Windows 1-20级别):" << std::endl << std::flush;
        std::wcout << L"[1] 速度 1   [2] 速度 2   [3] 速度 3" << std::endl << std::flush;
        std::wcout << L"[4] 速度 4   [5] 速度 5   [6] 速度 6" << std::endl << std::flush;
        std::wcout << L"[7] 速度 7   [8] 速度 8   [9] 速度 9" << std::endl << std::flush;
        std::wcout << L"[0] 速度 10 (默认)" << std::endl << std::endl << std::flush;

        std::wcout << L"精细控制:" << std::endl << std::flush;
        std::wcout << L"[+] 增加速度    [-] 减少速度" << std::endl << std::flush;
        std::wcout << L"[R] 恢复原始    [S] 退出设置" << std::endl << std::endl << std::flush;

        std::wcout << L"当前设置:" << std::endl << std::flush;
        if (currentSpeed <= 2) {
            std::wcout << L">> 非常慢 (适合精确操作)" << std::endl << std::flush;
            std::wcout << L">> 大约相当于 0.1x 效果！" << std::endl << std::flush;
        } else if (currentSpeed <= 5) {
            std::wcout << L">> 慢 (降低灵敏度)" << std::endl << std::flush;
        } else if (currentSpeed <= 10) {
            std::wcout << L">> 正常" << std::endl << std::flush;
        } else {
            std::wcout << L">> 快" << std::endl << std::flush;
        }

        std::wcout << std::endl << L"注意: 更改立即应用到整个系统！" << std::endl << std::flush;
        std::wcout << L"建议: 使用速度 1-2 获得 0.1x 效果" << std::endl << std::flush;
    }

    bool handleSettingsInput(char key) {
        int newSpeed = GetMouseSpeed();

        switch (key) {
            case '1': newSpeed = 1; break;
            case '2': newSpeed = 2; break;
            case '3': newSpeed = 3; break;
            case '4': newSpeed = 4; break;
            case '5': newSpeed = 5; break;
            case '6': newSpeed = 6; break;
            case '7': newSpeed = 7; break;
            case '8': newSpeed = 8; break;
            case '9': newSpeed = 9; break;
            case '0': newSpeed = 10; break;

            case '+':
            case '=':
                newSpeed = std::min(20, GetMouseSpeed() + 1);
                break;

            case '-':
                newSpeed = std::max(1, GetMouseSpeed() - 1);
                break;

            case 'r':
            case 'R':
                newSpeed = originalSpeed;
                break;

            case 's':
            case 'S':
                settingsEnabled = false;
                return false;

            default:
                return true;
        }

        if (SetMouseSpeed(newSpeed)) {
            showSettingsInterface();
        } else {
            std::wcout << L"\n设置鼠标速度失败！请尝试以管理员权限运行。\n" << std::flush;
        }

        return true;
    }

    void toggleSettings() {
        settingsEnabled = !settingsEnabled;
        if (settingsEnabled) {
            showSettingsInterface();
        }
    }

    bool isSettingsEnabled() const {
        return settingsEnabled;
    }
};

class DragHelper {
public:
    bool running;

private:
    enum State {
        NORMAL = 1,
        DRAG = 2,
        WAIT_CONFIRM = 3
    };

    bool enabled;
    HWND hwnd;
    ATOM atom;
    WNDCLASS wc;
    State currentState;
    std::chrono::steady_clock::time_point lastLeftMoveTime;
    std::chrono::steady_clock::time_point lastPollTime;

public:
    DragHelper() : running(false), enabled(false), hwnd(nullptr), atom(0),
                   currentState(NORMAL) {
        ZeroMemory(&wc, sizeof(WNDCLASS));
        lastLeftMoveTime = std::chrono::steady_clock::now();
        lastPollTime = std::chrono::steady_clock::now();
    }

    ~DragHelper() {
        disable();
    }

    void sendMouseEvent(DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        SendInput(1, &input, sizeof(INPUT));
    }

    std::wstring getDeviceName(HANDLE hDevice) {
        UINT size = 0;
        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &size);
        if (size == 0) return L"";

        std::vector<wchar_t> buffer(size);
        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, buffer.data(), &size);
        return std::wstring(buffer.data());
    }

    std::wstring getActiveProcessName() {
        HWND fghwnd = GetForegroundWindow();
        if (!fghwnd) return L"";

        DWORD pid = 0;
        GetWindowThreadProcessId(fghwnd, &pid);
        if (!pid) return L"";

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) return L"";

        wchar_t processPath[MAX_PATH]{};
        if (GetModuleFileNameExW(hProcess, nullptr, processPath, MAX_PATH)) {
            CloseHandle(hProcess);
            return std::wstring(processPath);
        }

        CloseHandle(hProcess);
        return L"";
    }

    void resetToNormal() {
        if (currentState != NORMAL) {
            sendMouseEvent(MOUSEEVENTF_LEFTUP);
            currentState = NORMAL;
            std::wcout << L"状态重置为 NORMAL" << std::endl << std::flush;
        }
    }

    void ensureStateGuard() {
        auto currentTime = std::chrono::steady_clock::now();
        auto pollDuration = std::chrono::duration<double>(currentTime - lastPollTime).count();

        if (pollDuration > POLL_INTERVAL_SEC) {
            lastPollTime = currentTime;

            // 离开目标进程时复位
            if (currentState != NORMAL) {
                std::wstring currentProcess = getActiveProcessName();
                if (currentProcess.empty() ||
                    currentProcess.find(TARGET_PROCESS) == std::wstring::npos) {
                    std::wcout << L"离开目标进程，重置状态" << std::endl << std::flush;
                    resetToNormal();
                }
            }

            // 拖拽超时检查
            if (currentState == DRAG) {
                auto dragDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - lastLeftMoveTime).count();
                if (dragDuration > IDLE_TIMEOUT_MS) {
                    currentState = WAIT_CONFIRM;
                    std::wcout << L"拖拽空闲超时，进入等待确认状态" << std::endl << std::flush;
                }
            }
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        DragHelper* pThis = nullptr;

        if (uMsg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<DragHelper*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        } else {
            pThis = reinterpret_cast<DragHelper*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pThis) {
            return pThis->wndProc(hwnd, uMsg, wParam, lParam);
        }

        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_INPUT && enabled) {
            // 读取RAW输入数据
            UINT dwSize = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                            nullptr, &dwSize, sizeof(RAWINPUTHEADER));

            std::vector<BYTE> buffer(dwSize);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                                buffer.data(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
                return 0;
            }

            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
            if (raw->header.dwType != RIM_TYPEMOUSE) {
                return 0;
            }

            std::wstring devName = getDeviceName(raw->header.hDevice);
            if (devName.empty()) {
                return 0;
            }

            // 转换为小写进行比较
            std::transform(devName.begin(), devName.end(), devName.begin(), ::towlower);
            std::wstring leftMouseIdLower = LEFT_MOUSE_ID;
            std::wstring rightMouseIdLower = RIGHT_MOUSE_ID;
            std::transform(leftMouseIdLower.begin(), leftMouseIdLower.end(), leftMouseIdLower.begin(), ::towlower);
            std::transform(rightMouseIdLower.begin(), rightMouseIdLower.end(), rightMouseIdLower.begin(), ::towlower);

            bool isLeft = devName.find(leftMouseIdLower) != std::wstring::npos;
            bool isRight = devName.find(rightMouseIdLower) != std::wstring::npos;

            // 状态机处理
            if (currentState == NORMAL && isLeft) {
                // 检查是否在目标进程中
                std::wstring currentProcess = getActiveProcessName();
                if (currentProcess.empty() ||
                    currentProcess.find(TARGET_PROCESS) == std::wstring::npos) {
                    return 0;
                }

                currentState = DRAG;
                lastLeftMoveTime = std::chrono::steady_clock::now();
                sendMouseEvent(MOUSEEVENTF_LEFTDOWN);
                std::wcout << L"左鼠标移动，开始拖拽" << std::endl << std::flush;
                return 0;
            }
            else if (currentState == DRAG) {
                if (isLeft) {
                    lastLeftMoveTime = std::chrono::steady_clock::now();
                    return 0;
                }
                else if (isRight) {
                    return 0;
                }
            }
            else if (currentState == WAIT_CONFIRM) {
                if (isLeft) {
                    currentState = DRAG;
                    lastLeftMoveTime = std::chrono::steady_clock::now();
                    std::wcout << L"左鼠标继续移动，恢复拖拽" << std::endl << std::flush;
                    return 0;
                }
                else if (isRight) {
                    currentState = NORMAL;
                    sendMouseEvent(MOUSEEVENTF_LEFTUP);
                    std::wcout << L"右鼠标移动，结束拖拽" << std::endl << std::flush;
                    return 0;
                }
            }
        }

        return DefWindowProc(hwnd, msg, wparam, lparam);
    }

    bool enable() {
        if (enabled) return true;

        try {
            // 创建消息窗口
            wc.style         = CS_NOCLOSE;
            wc.lpfnWndProc   = WindowProc;
            wc.hInstance     = GetModuleHandle(nullptr);
            wc.lpszClassName = L"MinimalDragHelper";

            atom = RegisterClass(&wc);
            if (!atom) {
                std::wcout << L"注册窗口类失败" << std::endl << std::flush;
                return false;
            }

            hwnd = CreateWindow(
                reinterpret_cast<LPCWSTR>(atom), L"MDH", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, wc.hInstance, this
            );

            if (!hwnd) {
                std::wcout << L"创建窗口失败" << std::endl << std::flush;
                return false;
            }

            // 注册RAW输入设备（鼠标）
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = 0x01; // Generic desktop controls
            rid.usUsage     = 0x02; // Mouse
            rid.dwFlags     = RIDEV_INPUTSINK | RIDEV_NOLEGACY; // 后台接收 + 不生成 WM_MOUSE
            rid.hwndTarget  = hwnd;

            if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE))) {
                std::wcout << L"注册RAW输入设备失败" << std::endl << std::flush;
                return false;
            }

            enabled = true;
            running = true;
            std::wcout << L"双鼠标拖拽助手已启用" << std::endl << std::flush;
            return true;

        } catch (const std::exception& e) {
            std::wcout << L"启用失败: " << e.what() << std::endl << std::flush;
            return false;
        }
    }

    void disable() {
        if (!enabled) return;

        std::wcout << L"禁用双鼠标拖拽助手" << std::endl << std::flush;
        enabled = false;
        running = false;

        // 确保左键抬起
        try {
            sendMouseEvent(MOUSEEVENTF_LEFTUP);
        } catch (...) {}

        currentState = NORMAL;

        // 清理资源
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }

        if (atom && wc.hInstance) {
            UnregisterClass(reinterpret_cast<LPCWSTR>(atom), wc.hInstance);
            atom = 0;
        }
    }
};

// 全局指针，用于在Ctrl+C处理程序中访问helper
DragHelper* g_pHelper = nullptr;
MouseSensitivityController* g_pMouseController = nullptr;

BOOL WINAPI ConsoleHandler(DWORD CEvent)
{
    switch(CEvent)
    {
    case CTRL_C_EVENT:
        std::wcout << L"收到中断信号" << std::endl << std::flush;
        if(g_pHelper) {
            g_pHelper->disable();
        }
        if(g_pMouseController) {
            g_pMouseController->restoreOriginalSpeed();
        }
        break;
    }
    return TRUE;
}

// <<<--- 这里是修改后的 main 函数 ---<<<
int main() {
    // 确保控制台窗口可见
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow != NULL) {
        ShowWindow(consoleWindow, SW_SHOW);
        SetForegroundWindow(consoleWindow);
    }

    // 关键修改：初始化控制台宽字符输出
    InitConsoleUnicode();

    // 设置控制台标题
    SetConsoleTitleW(L"双鼠标拖拽助手 v1.0");

    std::wcout << L"启动程序..." << std::endl << std::flush;

    std::wcout << L"=== 双鼠标拖拽助手 v1.0 ===" << std::endl << std::flush;
    std::wcout << L"目标进程: " << TARGET_PROCESS << std::endl << std::flush;
    std::wcout << L"空闲超时: " << IDLE_TIMEOUT_MS << L"ms" << std::endl << std::flush;
    std::wcout << L"轮询间隔: " << POLL_INTERVAL_SEC << L"s" << std::endl << std::flush;
    std::wcout << L"按 [M] 键进入鼠标设置界面" << std::endl << std::flush;
    std::wcout << L"按 Ctrl+C 退出" << std::endl << std::flush;

    std::wcout << L"正在创建DragHelper..." << std::endl << std::flush;
    DragHelper helper;

    std::wcout << L"正在创建MouseSensitivityController..." << std::endl << std::flush;
    MouseSensitivityController mouseController;

    g_pHelper = &helper;
    g_pMouseController = &mouseController;

    std::wcout << L"正在启用DragHelper..." << std::endl << std::flush;
    if (!helper.enable()) {
        std::wcout << L"启用失败，可能需要管理员权限" << std::endl << std::flush;
        std::wcout << L"请右键点击程序，选择“以管理员身份运行”" << std::endl << std::flush;
        std::wcout << L"按任意键退出..." << std::endl << std::flush;
        _getch();
        return 1;
    }

    std::wcout << L"正在设置Ctrl+C处理..." << std::endl << std::flush;
    if (SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE) == FALSE) {
        std::wcout << L"无法安装控制台处理程序" << std::endl << std::flush;
        std::wcout << L"程序仍可正常运行，但Ctrl+C可能无法正确清理" << std::endl << std::flush;
    }

    std::wcout << L"程序启动成功！进入主循环..." << std::endl << std::flush;

    try {
        // 主循环：同时处理拖拽和设置界面
        while (helper.running) {
            // 处理Windows消息
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            // 状态监控
            helper.ensureStateGuard();

            // 处理键盘输入
            if (_kbhit()) {
                char key = _getch();

                if (mouseController.isSettingsEnabled()) {
                    // 在设置界面中处理键盘输入
                    if (!mouseController.handleSettingsInput(key)) {
                        // 退出设置界面
                        ClearConsoleScreen();
                        std::wcout << L"=== 双鼠标拖拽助手 v1.0 ===" << std::endl << std::flush;
                        std::wcout << L"已退出设置界面，恢复拖拽功能" << std::endl << std::flush;
                        std::wcout << L"按 [M] 键进入鼠标设置界面" << std::endl << std::flush;
                        std::wcout << L"按 Ctrl+C 退出" << std::endl << std::flush;
                    }
                } else {
                    // 在主界面中处理键盘输入
                    if (key == 'm' || key == 'M') {
                        mouseController.toggleSettings();
                    } else if (key == 'q' || key == 'Q') {
                        // 添加 Q 键退出选项
                        std::wcout << L"用户请求退出程序..." << std::endl << std::flush;
                        helper.running = false;
                        break;
                    }
                }
            }

            // 短暂休眠避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

    } catch (const std::exception& e) {
        std::wcout << L"运行异常: " << e.what() << std::endl << std::flush;
    }

    std::wcout << L"程序正在退出..." << std::endl << std::flush;
    return 0;
}