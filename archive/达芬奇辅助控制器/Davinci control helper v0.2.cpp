// 双鼠标拖拽助手 v2.0 - 优化版本
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
#include <io.h>
#include <fcntl.h>
#include <locale>

// 可调参数
const std::wstring LEFT_MOUSE_ID = L"\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
const std::wstring RIGHT_MOUSE_ID = L"\\?\\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
const std::wstring TARGET_PROCESS = L"Resolve.exe";
const int IDLE_TIMEOUT_MS = 20;
const double POLL_INTERVAL_SEC = 0.1;
const int DRAG_SPEED = 2;  // 拖拽时的鼠标速度 (1-20)

// 控制台工具
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
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin),  _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    std::locale::global(std::locale(""));
    std::wcout.imbue(std::locale());
    std::wcin.imbue(std::locale());
    std::wcerr.imbue(std::locale());
}

// 设置控制台颜色
void SetConsoleColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

// 颜色常量
const int COLOR_GREEN = 10;
const int COLOR_YELLOW = 14;
const int COLOR_RED = 12;
const int COLOR_CYAN = 11;
const int COLOR_WHITE = 15;
const int COLOR_GRAY = 8;

// 智能鼠标速度控制器 - 拖拽时临时减速
class SmartMouseController {
private:
    int originalSpeed;
    int dragSpeed;
    bool isDragSpeedActive;
    std::chrono::steady_clock::time_point lastSpeedChange;

public:
    SmartMouseController(int dragSpeedValue = DRAG_SPEED) 
        : originalSpeed(10), dragSpeed(dragSpeedValue), isDragSpeedActive(false) {
        originalSpeed = GetMouseSpeed();
        lastSpeedChange = std::chrono::steady_clock::now();
    }

    ~SmartMouseController() {
        restoreNormalSpeed();
    }

    int GetMouseSpeed() {
        int speed = 10;
        SystemParametersInfo(SPI_GETMOUSESPEED, 0, &speed, 0);
        return speed;
    }

    bool SetMouseSpeed(int speed) {
        return SystemParametersInfo(
            SPI_SETMOUSESPEED,
            0,
            reinterpret_cast<PVOID>(static_cast<INT_PTR>(speed)),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
        ) != FALSE;
    }

    // 开始拖拽时减慢速度
    void activateDragSpeed() {
        if (!isDragSpeedActive) {
            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastChange = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpeedChange).count();
            
            // 防止频繁切换，至少间隔100ms
            if (timeSinceLastChange > 100) {
                if (SetMouseSpeed(dragSpeed)) {
                    isDragSpeedActive = true;
                    lastSpeedChange = now;
                    SetConsoleColor(COLOR_CYAN);
                    std::wcout << L"🐌 拖拽模式已激活 - 鼠标速度已降低至 " << dragSpeed << L"/20" << std::endl;
                    SetConsoleColor(COLOR_WHITE);
                    std::wcout.flush();
                }
            }
        }
    }

    // 结束拖拽时恢复正常速度
    void restoreNormalSpeed() {
        if (isDragSpeedActive) {
            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastChange = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpeedChange).count();
            
            // 防止频繁切换，至少间隔100ms
            if (timeSinceLastChange > 100) {
                if (SetMouseSpeed(originalSpeed)) {
                    isDragSpeedActive = false;
                    lastSpeedChange = now;
                    SetConsoleColor(COLOR_GREEN);
                    std::wcout << L"⚡ 正常模式已恢复 - 鼠标速度恢复至 " << originalSpeed << L"/20" << std::endl;
                    SetConsoleColor(COLOR_WHITE);
                    std::wcout.flush();
                }
            }
        }
    }

    // 手动设置拖拽速度
    void setDragSpeed(int speed) {
        if (speed >= 1 && speed <= 20) {
            dragSpeed = speed;
            if (isDragSpeedActive) {
                SetMouseSpeed(dragSpeed);
                SetConsoleColor(COLOR_YELLOW);
                std::wcout << L"🔧 拖拽速度已调整为 " << dragSpeed << L"/20" << std::endl;
                SetConsoleColor(COLOR_WHITE);
                std::wcout.flush();
            }
        }
    }

    bool isInDragMode() const {
        return isDragSpeedActive;
    }

    int getCurrentDragSpeed() const {
        return dragSpeed;
    }

    int getOriginalSpeed() const {
        return originalSpeed;
    }

    void showSpeedStatus() {
        int currentSpeed = GetMouseSpeed();
        if (isDragSpeedActive) {
            SetConsoleColor(COLOR_CYAN);
            std::wcout << L"🐌 当前模式: 拖拽模式 (速度: " << currentSpeed << L"/20)" << std::endl;
        } else {
            SetConsoleColor(COLOR_GREEN);
            std::wcout << L"⚡ 当前模式: 正常模式 (速度: " << currentSpeed << L"/20)" << std::endl;
        }
        SetConsoleColor(COLOR_WHITE);
        std::wcout.flush();
    }
};

class OptimizedDragHelper {
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
    std::chrono::steady_clock::time_point stateStartTime;
    SmartMouseController* mouseController;
    int dragDuration;

public:
    OptimizedDragHelper(SmartMouseController* controller) 
        : running(false), enabled(false), hwnd(nullptr), atom(0),
          currentState(NORMAL), mouseController(controller), dragDuration(0) {
        ZeroMemory(&wc, sizeof(WNDCLASS));
        lastLeftMoveTime = std::chrono::steady_clock::now();
        lastPollTime = std::chrono::steady_clock::now();
        stateStartTime = std::chrono::steady_clock::now();
    }

    ~OptimizedDragHelper() {
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
            stateStartTime = std::chrono::steady_clock::now();
            
            // 恢复正常鼠标速度
            if (mouseController) {
                mouseController->restoreNormalSpeed();
            }
            
            SetConsoleColor(COLOR_GRAY);
            std::wcout << L"🔄 状态已重置为正常模式" << std::endl;
            SetConsoleColor(COLOR_WHITE);
            std::wcout.flush();
        }
    }

    void showStatusBar() {
        // 清除当前行并显示状态栏
        std::wcout << L"\r";
        
        switch (currentState) {
            case NORMAL:
                SetConsoleColor(COLOR_GREEN);
                std::wcout << L"⭕ 就绪";
                break;
            case DRAG:
                SetConsoleColor(COLOR_CYAN);
                std::wcout << L"🖱️ 拖拽中 (" << dragDuration << L"ms)";
                break;
            case WAIT_CONFIRM:
                SetConsoleColor(COLOR_YELLOW);
                std::wcout << L"⏳ 等待确认";
                break;
        }
        
        SetConsoleColor(COLOR_WHITE);
        std::wcout << L" | 鼠标: ";
        
        if (mouseController && mouseController->isInDragMode()) {
            SetConsoleColor(COLOR_CYAN);
            std::wcout << L"减速模式";
        } else {
            SetConsoleColor(COLOR_GREEN);
            std::wcout << L"正常模式";
        }
        
        SetConsoleColor(COLOR_WHITE);
        std::wcout << L"                    "; // 清除行尾
        std::wcout.flush();
    }

    void ensureStateGuard() {
        auto currentTime = std::chrono::steady_clock::now();
        auto pollDuration = std::chrono::duration<double>(currentTime - lastPollTime).count();

        if (pollDuration > POLL_INTERVAL_SEC) {
            lastPollTime = currentTime;

            // 更新拖拽时长
            if (currentState == DRAG) {
                dragDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - stateStartTime).count();
            }

            // 更新状态栏
            showStatusBar();

            // 离开目标进程时复位
            if (currentState != NORMAL) {
                std::wstring currentProcess = getActiveProcessName();
                if (currentProcess.empty() ||
                    currentProcess.find(TARGET_PROCESS) == std::wstring::npos) {
                    std::wcout << std::endl;
                    SetConsoleColor(COLOR_YELLOW);
                    std::wcout << L"⚠️ 已离开目标程序，状态重置" << std::endl;
                    SetConsoleColor(COLOR_WHITE);
                    resetToNormal();
                }
            }

            // 拖拽超时检查
            if (currentState == DRAG) {
                auto dragTimeSinceLastMove = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - lastLeftMoveTime).count();
                if (dragTimeSinceLastMove > IDLE_TIMEOUT_MS) {
                    currentState = WAIT_CONFIRM;
                    stateStartTime = currentTime;
                    std::wcout << std::endl;
                    SetConsoleColor(COLOR_YELLOW);
                    std::wcout << L"⏳ 拖拽暂停，移动右鼠标结束或左鼠标继续" << std::endl;
                    SetConsoleColor(COLOR_WHITE);
                }
            }
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        OptimizedDragHelper* pThis = nullptr;

        if (uMsg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<OptimizedDragHelper*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        } else {
            pThis = reinterpret_cast<OptimizedDragHelper*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (pThis) {
            return pThis->wndProc(hwnd, uMsg, wParam, lParam);
        }

        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_INPUT && enabled) {
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

            // 检查是否有实际的鼠标移动
            if (raw->data.mouse.lLastX == 0 && raw->data.mouse.lLastY == 0) {
                return 0; // 没有移动，忽略此事件
            }

            std::transform(devName.begin(), devName.end(), devName.begin(), ::towlower);
            std::wstring leftMouseIdLower = LEFT_MOUSE_ID;
            std::wstring rightMouseIdLower = RIGHT_MOUSE_ID;
            std::transform(leftMouseIdLower.begin(), leftMouseIdLower.end(), leftMouseIdLower.begin(), ::towlower);
            std::transform(rightMouseIdLower.begin(), rightMouseIdLower.end(), rightMouseIdLower.begin(), ::towlower);

            bool isLeft = devName.find(leftMouseIdLower) != std::wstring::npos;
            bool isRight = devName.find(rightMouseIdLower) != std::wstring::npos;

            // 状态机处理
            if (currentState == NORMAL && isLeft) {
                std::wstring currentProcess = getActiveProcessName();
                if (currentProcess.empty() ||
                    currentProcess.find(TARGET_PROCESS) == std::wstring::npos) {
                    return 0;
                }

                currentState = DRAG;
                stateStartTime = std::chrono::steady_clock::now();
                lastLeftMoveTime = stateStartTime;
                dragDuration = 0;
                
                sendMouseEvent(MOUSEEVENTF_LEFTDOWN);
                
                // 激活拖拽减速模式
                if (mouseController) {
                    mouseController->activateDragSpeed();
                }
                
                std::wcout << std::endl;
                SetConsoleColor(COLOR_CYAN);
                std::wcout << L"🚀 开始拖拽 - 鼠标已减速" << std::endl;
                SetConsoleColor(COLOR_WHITE);
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
                    stateStartTime = std::chrono::steady_clock::now();
                    lastLeftMoveTime = stateStartTime;
                    std::wcout << std::endl;
                    SetConsoleColor(COLOR_CYAN);
                    std::wcout << L"▶️ 继续拖拽" << std::endl;
                    SetConsoleColor(COLOR_WHITE);
                    return 0;
                }
                else if (isRight) {
                    currentState = NORMAL;
                    stateStartTime = std::chrono::steady_clock::now();
                    sendMouseEvent(MOUSEEVENTF_LEFTUP);
                    
                    // 恢复正常鼠标速度
                    if (mouseController) {
                        mouseController->restoreNormalSpeed();
                    }
                    
                    std::wcout << std::endl;
                    SetConsoleColor(COLOR_GREEN);
                    std::wcout << L"✅ 拖拽结束 - 鼠标速度已恢复" << std::endl;
                    SetConsoleColor(COLOR_WHITE);
                    return 0;
                }
            }
        }

        return DefWindowProc(hwnd, msg, wparam, lparam);
    }

    bool enable() {
        if (enabled) return true;

        try {
            wc.style         = CS_NOCLOSE;
            wc.lpfnWndProc   = WindowProc;
            wc.hInstance     = GetModuleHandle(nullptr);
            wc.lpszClassName = L"OptimizedDragHelper";

            atom = RegisterClass(&wc);
            if (!atom) {
                SetConsoleColor(COLOR_RED);
                std::wcout << L"❌ 注册窗口类失败" << std::endl;
                SetConsoleColor(COLOR_WHITE);
                return false;
            }

            hwnd = CreateWindow(
                reinterpret_cast<LPCWSTR>(atom), L"ODH", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, wc.hInstance, this
            );

            if (!hwnd) {
                SetConsoleColor(COLOR_RED);
                std::wcout << L"❌ 创建窗口失败" << std::endl;
                SetConsoleColor(COLOR_WHITE);
                return false;
            }

            RAWINPUTDEVICE rid{};
            rid.usUsagePage = 0x01;
            rid.usUsage     = 0x02;
            rid.dwFlags     = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
            rid.hwndTarget  = hwnd;

            if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE))) {
                SetConsoleColor(COLOR_RED);
                std::wcout << L"❌ 注册RAW输入设备失败" << std::endl;
                SetConsoleColor(COLOR_WHITE);
                return false;
            }

            enabled = true;
            running = true;
            SetConsoleColor(COLOR_GREEN);
            std::wcout << L"✅ 双鼠标拖拽助手已启用" << std::endl;
            SetConsoleColor(COLOR_WHITE);
            return true;

        } catch (const std::exception& e) {
            SetConsoleColor(COLOR_RED);
            std::wcout << L"❌ 启用失败: " << e.what() << std::endl;
            SetConsoleColor(COLOR_WHITE);
            return false;
        }
    }

    void disable() {
        if (!enabled) return;

        SetConsoleColor(COLOR_YELLOW);
        std::wcout << std::endl << L"🔄 正在关闭双鼠标拖拽助手..." << std::endl;
        SetConsoleColor(COLOR_WHITE);
        
        enabled = false;
        running = false;

        try {
            sendMouseEvent(MOUSEEVENTF_LEFTUP);
        } catch (...) {}

        currentState = NORMAL;

        // 恢复鼠标速度
        if (mouseController) {
            mouseController->restoreNormalSpeed();
        }

        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }

        if (atom && wc.hInstance) {
            UnregisterClass(reinterpret_cast<LPCWSTR>(atom), wc.hInstance);
            atom = 0;
        }
        
        SetConsoleColor(COLOR_GREEN);
        std::wcout << L"✅ 已安全关闭" << std::endl;
        SetConsoleColor(COLOR_WHITE);
    }
};

// 显示帮助界面
void showHelpInterface() {
    ClearConsoleScreen();
    
    SetConsoleColor(COLOR_CYAN);
    std::wcout << L"╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::wcout << L"║                双鼠标拖拽助手 v2.0 - 帮助                ║" << std::endl;
    std::wcout << L"╚══════════════════════════════════════════════════════════╝" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << std::endl;

    SetConsoleColor(COLOR_YELLOW);
    std::wcout << L"💡 功能说明:" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << L"   • 当您在 " << TARGET_PROCESS << L" 中移动左鼠标时，程序会自动开始拖拽" << std::endl;
    std::wcout << L"   • 拖拽开始时，鼠标速度会自动降低到 " << DRAG_SPEED << L"/20，便于精确操作" << std::endl;
    std::wcout << L"   • 移动右鼠标可以结束拖拽，鼠标速度自动恢复正常" << std::endl;
    std::wcout << L"   • 拖拽暂停时，左鼠标继续拖拽，右鼠标结束拖拽" << std::endl << std::endl;

    SetConsoleColor(COLOR_YELLOW);
    std::wcout << L"⌨️ 键盘控制:" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << L"   [H] 显示此帮助界面     [S] 查看鼠标速度状态" << std::endl;
    std::wcout << L"   [1-9] 设置拖拽速度     [Q] 退出程序" << std::endl;
    std::wcout << L"   [Ctrl+C] 安全退出      [任意键] 返回主界面" << std::endl << std::endl;

    SetConsoleColor(COLOR_YELLOW);
    std::wcout << L"📊 状态指示:" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << L"   ⭕ 就绪 - 等待在目标程序中开始拖拽" << std::endl;
    std::wcout << L"   🖱️ 拖拽中 - 正在进行拖拽操作（鼠标已减速）" << std::endl;
    std::wcout << L"   ⏳ 等待确认 - 拖拽暂停，等待继续或结束" << std::endl << std::endl;

    SetConsoleColor(COLOR_YELLOW);
    std::wcout << L"⚙️ 当前设置:" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << L"   目标程序: " << TARGET_PROCESS << std::endl;
    std::wcout << L"   拖拽速度: " << DRAG_SPEED << L"/20" << std::endl;
    std::wcout << L"   空闲超时: " << IDLE_TIMEOUT_MS << L"ms" << std::endl << std::endl;

    SetConsoleColor(COLOR_GRAY);
    std::wcout << L"按任意键返回主界面..." << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout.flush();
}

// 全局变量
OptimizedDragHelper* g_pHelper = nullptr;
SmartMouseController* g_pMouseController = nullptr;

BOOL WINAPI ConsoleHandler(DWORD CEvent)
{
    switch(CEvent)
    {
    case CTRL_C_EVENT:
        SetConsoleColor(COLOR_YELLOW);
        std::wcout << std::endl << L"🛑 收到退出信号，正在安全关闭..." << std::endl;
        SetConsoleColor(COLOR_WHITE);
        if(g_pHelper) {
            g_pHelper->disable();
        }
        if(g_pMouseController) {
            g_pMouseController->restoreNormalSpeed();
        }
        break;
    }
    return TRUE;
}

int main() {
    // 设置控制台
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow != NULL) {
        ShowWindow(consoleWindow, SW_SHOW);
        SetForegroundWindow(consoleWindow);
    }

    InitConsoleUnicode();
    SetConsoleTitleW(L"双鼠标拖拽助手 v2.0 - 智能减速版");

    // 欢迎界面
    ClearConsoleScreen();
    SetConsoleColor(COLOR_CYAN);
    std::wcout << L"╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::wcout << L"║                双鼠标拖拽助手 v2.0                       ║" << std::endl;
    std::wcout << L"║                   智能减速版                             ║" << std::endl;
    std::wcout << L"╚══════════════════════════════════════════════════════════╝" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << std::endl;

    SetConsoleColor(COLOR_GREEN);
    std::wcout << L"🚀 正在启动程序..." << std::endl;
    SetConsoleColor(COLOR_WHITE);

    // 创建智能鼠标控制器
    SmartMouseController mouseController(DRAG_SPEED);
    g_pMouseController = &mouseController;

    SetConsoleColor(COLOR_GREEN);
    std::wcout << L"✅ 智能鼠标控制器已就绪" << std::endl;
    SetConsoleColor(COLOR_WHITE);

    // 创建拖拽助手
    OptimizedDragHelper helper(&mouseController);
    g_pHelper = &helper;

    SetConsoleColor(COLOR_GREEN);
    std::wcout << L"✅ 拖拽助手已创建" << std::endl;
    SetConsoleColor(COLOR_WHITE);

    // 启用功能
    if (!helper.enable()) {
        SetConsoleColor(COLOR_RED);
        std::wcout << L"❌ 启用失败！可能需要管理员权限" << std::endl;
        std::wcout << L"请右键点击程序，选择\"以管理员身份运行\"" << std::endl;
        SetConsoleColor(COLOR_WHITE);
        std::wcout << L"按任意键退出..." << std::endl;
        _getch();
        return 1;
    }

    // 设置控制台处理程序
    if (SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE) == FALSE) {
        SetConsoleColor(COLOR_YELLOW);
        std::wcout << L"⚠️ 无法安装控制台处理程序，Ctrl+C可能无法正确清理" << std::endl;
        SetConsoleColor(COLOR_WHITE);
    }

    // 显示初始状态
    std::wcout << std::endl;
    SetConsoleColor(COLOR_YELLOW);
    std::wcout << L"📋 程序已就绪！使用说明:" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    std::wcout << L"   • 在 " << TARGET_PROCESS << L" 中移动左鼠标开始拖拽" << std::endl;
    std::wcout << L"   • 拖拽时鼠标会自动减速至 " << DRAG_SPEED << L"/20" << std::endl;
    std::wcout << L"   • 移动右鼠标结束拖拽并恢复正常速度" << std::endl;
    std::wcout << L"   • 按 [H] 查看详细帮助，[Q] 退出程序" << std::endl << std::endl;

    mouseController.showSpeedStatus();
    std::wcout << std::endl;

    try {
        // 主循环
        while (helper.running) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            helper.ensureStateGuard();

            // 处理键盘输入
            if (_kbhit()) {
                char key = _getch();
                std::wcout << std::endl;

                switch (key) {
                    case 'h':
                    case 'H':
                        showHelpInterface();
                        _getch();
                        ClearConsoleScreen();
                        SetConsoleColor(COLOR_GREEN);
                        std::wcout << L"✅ 已返回主界面" << std::endl;
                        SetConsoleColor(COLOR_WHITE);
                        mouseController.showSpeedStatus();
                        break;

                    case 's':
                    case 'S':
                        mouseController.showSpeedStatus();
                        break;

                    case '1': case '2': case '3': case '4': case '5':
                    case '6': case '7': case '8': case '9':
                        {
                            int newSpeed = key - '0';
                            mouseController.setDragSpeed(newSpeed);
                        }
                        break;

                    case 'q':
                    case 'Q':
                        SetConsoleColor(COLOR_YELLOW);
                        std::wcout << L"🚪 用户请求退出程序..." << std::endl;
                        SetConsoleColor(COLOR_WHITE);
                        helper.running = false;
                        break;

                    default:
                        SetConsoleColor(COLOR_GRAY);
                        std::wcout << L"💡 按 [H] 查看帮助，[Q] 退出程序" << std::endl;
                        SetConsoleColor(COLOR_WHITE);
                        break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

    } catch (const std::exception& e) {
        SetConsoleColor(COLOR_RED);
        std::wcout << std::endl << L"❌ 运行异常: " << e.what() << std::endl;
        SetConsoleColor(COLOR_WHITE);
    }

    SetConsoleColor(COLOR_GREEN);
    std::wcout << std::endl << L"👋 程序已安全退出，感谢使用！" << std::endl;
    SetConsoleColor(COLOR_WHITE);
    return 0;
}