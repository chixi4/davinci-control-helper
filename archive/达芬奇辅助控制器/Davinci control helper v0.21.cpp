// 双鼠标拖拽助手 v2.0 - 优化解决延迟问题版本
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

// =================================================================================
// 核心参数配置 (请根据你的设备ID和需求修改)
// =================================================================================
// !!! 重要：请用你自己的鼠标ID替换下面的示例ID !!!
// 你可以使用 Device Manager (设备管理器) -> 鼠标和其他指针设备 -> 你的鼠标 -> 详细信息 -> 设备实例路径
const std::wstring LEFT_MOUSE_ID = L"\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02047d_PID&80d4_REV&6701_d659ebc655ec#9&23d231c9&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"; // 左手鼠标ID
const std::wstring RIGHT_MOUSE_ID = L"\\?\\HID#VID_1532&PID_00B4&MI_00#7&1a4c5aa2&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"; // 右手鼠标ID
const std::wstring TARGET_PROCESS = L"Resolve.exe"; // 目标应用程序名
const int IDLE_TIMEOUT_MS = 300;     // 拖拽时多久不移动算作暂停 (毫秒)
const int DRAG_SPEED = 2;            // 拖拽时的鼠标速度 (1-20)
const double POLL_INTERVAL_SEC = 0.05; // 主循环轮询间隔 (秒)
// =================================================================================


// 控制台工具
namespace ConsoleUtil {
    void ClearScreen() {
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

    void InitUnicode() {
        _setmode(_fileno(stdout), _O_U16TEXT);
        std::locale::global(std::locale(""));
        std::wcout.imbue(std::locale());
    }

    enum Color {
        GREEN = 10, YELLOW = 14, RED = 12, CYAN = 11, WHITE = 15, GRAY = 8
    };

    void SetColor(int color) {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
    }
}

// 智能鼠标速度控制器
class SmartMouseController {
private:
    int originalSpeed;
    int dragSpeed;
    bool isDragSpeedActive;

public:
    SmartMouseController(int dragSpeedValue) 
        : dragSpeed(dragSpeedValue), isDragSpeedActive(false) {
        SystemParametersInfo(SPI_GETMOUSESPEED, 0, &originalSpeed, 0);
    }

    ~SmartMouseController() {
        // 确保退出时恢复原始速度
        if (isDragSpeedActive) {
            SetMouseSpeed(originalSpeed, false);
        }
    }

    bool SetMouseSpeed(int speed, bool isTemporary) {
        UINT flags = SPIF_SENDCHANGE;
        // FIX: 移除了 SPIF_UPDATEINIFILE，避免写入磁盘造成延迟，只在当前会话生效
        if (!isTemporary) {
            flags |= SPIF_UPDATEINIFILE; 
        }
        return SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(speed)), flags);
    }

    void activateDragSpeed() {
        if (!isDragSpeedActive) {
            if (SetMouseSpeed(dragSpeed, true)) {
                isDragSpeedActive = true;
            }
        }
    }

    void restoreNormalSpeed() {
        if (isDragSpeedActive) {
            if (SetMouseSpeed(originalSpeed, true)) {
                isDragSpeedActive = false;
            }
        }
    }
    
    void setDragSpeed(int speed) {
        if (speed >= 1 && speed <= 20) {
            dragSpeed = speed;
            if (isDragSpeedActive) {
                SetMouseSpeed(dragSpeed, true); // 实时更新当前拖拽速度
            }
        }
    }

    bool isInDragMode() const { return isDragSpeedActive; }
    int getCurrentDragSpeed() const { return dragSpeed; }
    int getOriginalSpeed() const { return originalSpeed; }
};

// 优化的拖拽助手
class OptimizedDragHelper {
public:
    enum class State { NORMAL, DRAG, WAIT_CONFIRM };

    bool running;

private:
    State currentState;
    State previousState; // 用于检测状态变化
    HWND hwnd;
    SmartMouseController& mouseController;
    std::chrono::steady_clock::time_point lastLeftMoveTime;
    std::chrono::steady_clock::time_point stateStartTime;

public:
    OptimizedDragHelper(SmartMouseController& controller) 
        : running(true), currentState(State::NORMAL), previousState(State::NORMAL), 
          hwnd(nullptr), mouseController(controller) {
        lastLeftMoveTime = std::chrono::steady_clock::now();
        stateStartTime = lastLeftMoveTime;
    }

    ~OptimizedDragHelper() {
        disable();
    }

    void start() {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"OptimizedDragHelperClass";
        
        if (!RegisterClass(&wc)) {
            ConsoleUtil::SetColor(ConsoleUtil::RED);
            std::wcout << L"❌ 注册窗口类失败！" << std::endl;
            running = false;
            return;
        }

        hwnd = CreateWindow(wc.lpszClassName, L"ODH Message Window", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!hwnd) {
            ConsoleUtil::SetColor(ConsoleUtil::RED);
            std::wcout << L"❌ 创建消息窗口失败！" << std::endl;
            running = false;
            return;
        }

        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01; // Generic Desktop
        rid.usUsage = 0x02;     // Mouse
        rid.dwFlags = RIDEV_INPUTSINK; // 接收后台输入
        rid.hwndTarget = hwnd;

        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            ConsoleUtil::SetColor(ConsoleUtil::RED);
            std::wcout << L"❌ 注册原始输入设备失败！可能需要管理员权限。" << std::endl;
            running = false;
            return;
        }
        ConsoleUtil::SetColor(ConsoleUtil::GREEN);
        std::wcout << L"✅ 拖拽助手已成功启动并监听鼠标事件。" << std::endl;
        ConsoleUtil::SetColor(ConsoleUtil::WHITE);
    }

    void disable() {
        running = false;
        if (mouseController.isInDragMode()) {
            sendMouseEvent(MOUSEEVENTF_LEFTUP);
            mouseController.restoreNormalSpeed();
        }
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        UnregisterClass(L"OptimizedDragHelperClass", GetModuleHandle(nullptr));
    }
    
    // 主循环中调用的状态守护和UI更新函数
    void update() {
        auto now = std::chrono::steady_clock::now();

        // 状态变化时打印日志
        if (currentState != previousState) {
            handleStateChange(now);
            previousState = currentState;
        }

        // 状态守护逻辑
        if (currentState == State::DRAG) {
            auto idle_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLeftMoveTime).count();
            if (idle_duration > IDLE_TIMEOUT_MS) {
                currentState = State::WAIT_CONFIRM;
            }
        }
        
        // 如果不在拖拽状态，检查是否离开了目标窗口
        if (currentState != State::NORMAL) {
            if (!isTargetProcessActive()) {
                 currentState = State::NORMAL;
            }
        }
        
        // 更新状态栏UI
        printStatusBar(now);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_CREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreate->lpCreateParams));
            return 0;
        }
        
        OptimizedDragHelper* pThis = reinterpret_cast<OptimizedDragHelper*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (pThis && uMsg == WM_INPUT) {
            return pThis->handleRawInput(lParam);
        }
        
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    LRESULT handleRawInput(LPARAM lParam) {
        UINT size;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        std::vector<BYTE> buffer(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
            return 0;
        }

        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
        if (raw->header.dwType != RIM_TYPEMOUSE) return 0;

        // FIX: 不再过滤 lLastX/Y 为0的事件，这是解决问题的关键
        // 现在我们只关心事件来自哪个鼠标

        std::wstring deviceName = getDeviceName(raw->header.hDevice);
        if (deviceName.empty()) return 0;
        
        std::transform(deviceName.begin(), deviceName.end(), deviceName.begin(), ::towlower);
        
        static const std::wstring leftMouseIdLower = []{ std::wstring s = LEFT_MOUSE_ID; std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; }();
        static const std::wstring rightMouseIdLower = []{ std::wstring s = RIGHT_MOUSE_ID; std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; }();

        bool isLeftMouse = deviceName.find(leftMouseIdLower) != std::wstring::npos;
        bool isRightMouse = deviceName.find(rightMouseIdLower) != std::wstring::npos;

        // 状态机核心逻辑
        switch (currentState) {
            case State::NORMAL:
                if (isLeftMouse && isTargetProcessActive()) {
                    currentState = State::DRAG;
                }
                break;
            case State::DRAG:
                if (isLeftMouse) {
                    lastLeftMoveTime = std::chrono::steady_clock::now();
                }
                // 在拖拽中，右鼠标移动不产生任何效果，避免误触
                break;
            case State::WAIT_CONFIRM:
                if (isLeftMouse) {
                    currentState = State::DRAG; // 继续拖拽
                } else if (isRightMouse) {
                    currentState = State::NORMAL; // 结束拖拽
                }
                break;
        }
        return 0;
    }

    void handleStateChange(const std::chrono::steady_clock::time_point& now) {
        std::wcout << L"\r" << std::wstring(80, L' ') << L"\r"; // 清除状态栏行

        if (currentState == State::DRAG && previousState == State::NORMAL) {
            // 从正常进入拖拽
            stateStartTime = now;
            lastLeftMoveTime = now;
            sendMouseEvent(MOUSEEVENTF_LEFTDOWN);
            mouseController.activateDragSpeed();
            ConsoleUtil::SetColor(ConsoleUtil::CYAN);
            std::wcout << L"🚀 开始拖拽... (鼠标已减速至 " << mouseController.getCurrentDragSpeed() << "/20)" << std::endl;
        } else if (currentState == State::DRAG && previousState == State::WAIT_CONFIRM) {
            // 从暂停恢复拖拽
            lastLeftMoveTime = now;
            ConsoleUtil::SetColor(ConsoleUtil::CYAN);
            std::wcout << L"▶️ 继续拖拽..." << std::endl;
        } else if (currentState == State::WAIT_CONFIRM) {
            // 进入暂停
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - stateStartTime).count();
            ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
            std::wcout << L"⏳ 拖拽暂停 (已持续 " << duration << L"ms)。移动[右鼠标]结束，或移动[左鼠标]继续。" << std::endl;
        } else if (currentState == State::NORMAL) {
            // 恢复正常
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - stateStartTime).count();
            sendMouseEvent(MOUSEEVENTF_LEFTUP);
            mouseController.restoreNormalSpeed();
            ConsoleUtil::SetColor(ConsoleUtil::GREEN);
            std::wcout << L"✅ 拖拽结束 (总时长 " << duration << "ms)。鼠标速度已恢复。" << std::endl;
        }
        ConsoleUtil::SetColor(ConsoleUtil::WHITE);
    }
    
    void printStatusBar(const std::chrono::steady_clock::time_point& now) {
        std::wcout << L"\r"; // 光标到行首
        ConsoleUtil::SetColor(ConsoleUtil::WHITE);

        switch(currentState) {
            case State::NORMAL:
                ConsoleUtil::SetColor(ConsoleUtil::GREEN);
                std::wcout << L"⭕ 就绪";
                break;
            case State::DRAG: {
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - stateStartTime).count();
                ConsoleUtil::SetColor(ConsoleUtil::CYAN);
                std::wcout << L"🖱️ 拖拽中 (" << duration << L"ms)";
                break;
            }
            case State::WAIT_CONFIRM:
                ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
                std::wcout << L"⏳ 等待确认";
                break;
        }
        
        ConsoleUtil::SetColor(ConsoleUtil::WHITE);
        std::wcout << L" | 鼠标: ";
        if (mouseController.isInDragMode()) {
            ConsoleUtil::SetColor(ConsoleUtil::CYAN);
            std::wcout << L"减速模式 (" << mouseController.getCurrentDragSpeed() << L"/20)";
        } else {
            ConsoleUtil::SetColor(ConsoleUtil::GREEN);
            std::wcout << L"正常模式 (" << mouseController.getOriginalSpeed() << L"/20)";
        }

        std::wcout << std::wstring(15, L' '); // 填充空白，清除行尾残留
        std::wcout.flush();
    }

    void sendMouseEvent(DWORD flags) {
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        SendInput(1, &input, sizeof(INPUT));
    }
    
    bool isTargetProcessActive() {
        HWND foreground_hwnd = GetForegroundWindow();
        if (!foreground_hwnd) return false;
        
        DWORD pid;
        GetWindowThreadProcessId(foreground_hwnd, &pid);
        if (pid == 0) return false;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) return false;

        wchar_t processPath[MAX_PATH];
        bool result = false;
        if (GetModuleFileNameExW(hProcess, nullptr, processPath, MAX_PATH)) {
            std::wstring processName(processPath);
            size_t last_slash = processName.find_last_of(L"\\/");
            if (last_slash != std::wstring::npos) {
                if (_wcsicmp(processName.substr(last_slash + 1).c_str(), TARGET_PROCESS.c_str()) == 0) {
                    result = true;
                }
            }
        }
        CloseHandle(hProcess);
        return result;
    }

    std::wstring getDeviceName(HANDLE hDevice) {
        UINT size = 0;
        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &size);
        if (size == 0) return L"";

        std::vector<wchar_t> buffer(size);
        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, buffer.data(), &size);
        return std::wstring(buffer.data());
    }
};

// 全局指针，用于 Ctrl+C 处理
OptimizedDragHelper* g_pHelper = nullptr;

// Ctrl+C 退出处理函数
BOOL WINAPI ConsoleHandler(DWORD CEvent) {
    if (CEvent == CTRL_C_EVENT) {
        ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
        std::wcout << std::endl << L"🛑 收到退出信号，正在安全关闭..." << std::endl;
        if (g_pHelper) {
            g_pHelper->disable();
        }
    }
    return TRUE;
}

void showHelp() {
    ConsoleUtil::ClearScreen();
    ConsoleUtil::SetColor(ConsoleUtil::CYAN);
    std::wcout << L"╔════════════════════════════════════════════════════╗\n"
                  L"║              双鼠标拖拽助手 v2.0 - 帮助            ║\n"
                  L"╚════════════════════════════════════════════════════╝\n\n";
    ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
    std::wcout << L"💡 功能说明:\n";
    ConsoleUtil::SetColor(ConsoleUtil::WHITE);
    std::wcout << L"   - 在目标程序 (" << TARGET_PROCESS << L") 激活时，移动[左鼠标]自动开始拖拽。\n"
                  L"   - 拖拽时鼠标速度会降低，方便精细操作。\n"
                  L"   - 拖拽中暂停移动一段时间后，会进入[等待确认]状态。\n"
                  L"   - 在[等待确认]时，移动[左鼠标]继续拖拽，移动[右鼠标]结束拖拽。\n\n";
    ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
    std::wcout << L"⌨️ 快捷键:\n";
    ConsoleUtil::SetColor(ConsoleUtil::WHITE);
    std::wcout << L"   [H] - 显示此帮助界面\n"
                  L"   [Q] - 退出程序\n"
                  L"   [1-9] - 实时调整拖拽时的鼠标速度 (1最慢, 9较快)\n"
                  L"   [Ctrl+C] - 安全退出程序\n\n";
    ConsoleUtil::SetColor(ConsoleUtil::GRAY);
    std::wcout << L"按任意键返回主界面..." << std::endl;
    _getch();
}


int main() {
    ConsoleUtil::InitUnicode();
    SetConsoleTitleW(L"双鼠标拖拽助手 v2.0 - 优化版");
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    ConsoleUtil::ClearScreen();
    ConsoleUtil::SetColor(ConsoleUtil::CYAN);
    std::wcout << L"╔════════════════════════════════════════════════════╗\n"
                  L"║       双鼠标拖拽助手 v2.0 - by AI & C++            ║\n"
                  L"╚════════════════════════════════════════════════════╝\n\n";
    
    if (LEFT_MOUSE_ID.find(L"XXXX") != std::string::npos || RIGHT_MOUSE_ID.find(L"YYYY") != std::string::npos) {
        ConsoleUtil::SetColor(ConsoleUtil::RED);
        std::wcout << L"错误：请在代码中设置你自己的鼠标ID！\n"
                      L"你可以在设备管理器中找到鼠标的'设备实例路径'。\n" << std::endl;
        ConsoleUtil::SetColor(ConsoleUtil::GRAY);
        std::wcout << L"按任意键退出..." << std::endl;
        _getch();
        return 1;
    }

    ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
    std::wcout << L"正在初始化...\n";
    ConsoleUtil::SetColor(ConsoleUtil::WHITE);

    SmartMouseController mouseController(DRAG_SPEED);
    OptimizedDragHelper helper(mouseController);
    g_pHelper = &helper;

    helper.start();
    
    if (!helper.running) {
        ConsoleUtil::SetColor(ConsoleUtil::GRAY);
        std::wcout << L"\n按任意键退出..." << std::endl;
        _getch();
        return 1;
    }

    std::wcout << L"\n提示: 按 [H] 键可以查看详细帮助和快捷键。\n" << std::endl;

    // 主循环
    while (helper.running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        helper.update();

        if (_kbhit()) {
            char key = _getch();
            std::wcout << L"\r" << std::wstring(80, L' ') << L"\r"; // 清除状态栏行
            if (key >= '1' && key <= '9') {
                int newSpeed = key - '0';
                mouseController.setDragSpeed(newSpeed);
                ConsoleUtil::SetColor(ConsoleUtil::YELLOW);
                std::wcout << L"🔧 拖拽速度已调整为 " << newSpeed << "/20" << std::endl;
            } else if (key == 'h' || key == 'H') {
                showHelp();
                ConsoleUtil::ClearScreen();
            } else if (key == 'q' || key == 'Q') {
                helper.disable();
            }
        }

        std::this_thread::sleep_for(std::chrono::duration<double>(POLL_INTERVAL_SEC));
    }
    
    ConsoleUtil::SetColor(ConsoleUtil::GREEN);
    std::wcout << L"\n程序已安全退出。感谢使用！" << std::endl;
    ConsoleUtil::SetColor(ConsoleUtil::WHITE);

    return 0;
}