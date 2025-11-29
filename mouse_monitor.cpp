/*
 * Mouse Monitor - 纯用户态版本
 *
 * 功能：
 * - 通过 Raw Input API 读取鼠标移动数据
 * - 从 ExtraInformation 字段解码原始移动量（需要 rawaccel 启用 setExtraInfo）
 * - 支持注册特定鼠标，只响应该鼠标的移动
 * - Caps Lock 开启/关闭自动左键功能
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

// ========== 全局变量 ==========
HWND g_hWnd = NULL;
bool g_running = true;
bool g_featureEnabled = false;
bool g_isMouseDown = false;
DWORD g_lastMoveTime = 0;
LONG g_moveCount = 0;
short g_lastRawX = 0;
short g_lastRawY = 0;
bool g_extraInfoValid = false;  // ExtraInformation 是否有效

// 设备注册相关
volatile HANDLE g_registeredDevice = NULL;    // 已注册的鼠标设备句柄
volatile HANDLE g_pendingDevice = NULL;       // 待确认的设备句柄
volatile bool g_registrationMode = true;      // 是否处于注册模式
wchar_t g_pendingDevicePath[512];             // 待确认设备的 HID 路径

const DWORD STOP_THRESHOLD_MS = 150;  // 停止移动的阈值（毫秒）

// ========== 函数声明 ==========
void MouseLeftDown();
void MouseLeftUp();
void DecodeExtraInfo(ULONG extraInfo, short* rawX, short* rawY);
void SetCursorVisible(bool visible);
void GetDeviceHidPath(HANDLE device, wchar_t* path, size_t pathSize);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
DWORD WINAPI MessageLoopThread(LPVOID lpParam);

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

// 窗口过程 - 处理 WM_INPUT 消息
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));

        if (size > 0) {
            BYTE* buffer = new BYTE[size];

            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size) {
                RAWINPUT* raw = (RAWINPUT*)buffer;

                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    HANDLE deviceHandle = raw->header.hDevice;

                    // 注册模式：检测移动的鼠标并显示设备信息
                    if (g_registrationMode) {
                        // 只有当移动量不为零时才触发检测
                        bool hasMovement = (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0);
                        if (hasMovement && deviceHandle != g_pendingDevice) {
                            g_pendingDevice = deviceHandle;
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
                    // 正常模式：只响应已注册的设备
                    else if (g_registeredDevice != NULL && deviceHandle != g_registeredDevice) {
                        // 忽略未注册设备的移动，不做任何处理
                    }
                    else {
                        // 检查是否是相对移动（不是绝对定位）
                        bool isRelative = !(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE);

                        if (isRelative) {
                            // 加速处理后的移动量
                            LONG accelX = raw->data.mouse.lLastX;
                            LONG accelY = raw->data.mouse.lLastY;

                            // 从 ExtraInformation 解码原始移动量
                            ULONG extraInfo = raw->data.mouse.ulExtraInformation;
                            short rawX = 0, rawY = 0;
                            DecodeExtraInfo(extraInfo, &rawX, &rawY);

                            // 检测 ExtraInformation 是否有效
                            // 如果 extraInfo 非零且与加速后数据不同，说明 setExtraInfo 已启用
                            if (extraInfo != 0 && (rawX != 0 || rawY != 0)) {
                                g_extraInfoValid = true;
                                g_lastRawX = rawX;
                                g_lastRawY = rawY;
                            }

                            // 检测是否有移动（使用原始数据或加速后数据）
                            bool hasMoved = false;
                            if (g_extraInfoValid) {
                                hasMoved = (rawX != 0 || rawY != 0);
                            } else {
                                hasMoved = (accelX != 0 || accelY != 0);
                            }

                            if (hasMoved) {
                                g_moveCount++;
                                g_lastMoveTime = GetTickCount();

                                // 显示移动信息
                                if (g_extraInfoValid) {
                                    printf("\r[RAW] X: %+5d  Y: %+5d  | Accel: (%+5ld, %+5ld) | Count: %ld  | %s    ",
                                           rawX, rawY, accelX, accelY, g_moveCount,
                                           g_featureEnabled ? "ON " : "OFF");
                                } else {
                                    printf("\r[ACCEL] X: %+5ld  Y: %+5ld  | Count: %ld  | %s  (setExtraInfo disabled)    ",
                                           accelX, accelY, g_moveCount,
                                           g_featureEnabled ? "ON " : "OFF");
                                }
                                fflush(stdout);

                                // 自动左键功能
                                if (g_featureEnabled && !g_isMouseDown) {
                                    MouseLeftDown();
                                    g_isMouseDown = true;
                                }
                            }
                        }
                    }
                }
            }
            delete[] buffer;
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
        printf("[ERROR] RegisterClass failed: %lu\n", GetLastError());
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
        printf("[ERROR] CreateWindow failed: %lu\n", GetLastError());
        return 1;
    }

    // 注册 Raw Input 设备（鼠标）
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;  // Generic Desktop
    rid.usUsage = 0x02;      // Mouse
    rid.dwFlags = RIDEV_INPUTSINK;  // 即使窗口不在前台也接收输入
    rid.hwndTarget = g_hWnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        printf("[ERROR] RegisterRawInputDevices failed: %lu\n", GetLastError());
        return 1;
    }

    printf("[OK] Raw Input registered\n");

    // 消息循环
    MSG msg;
    while (g_running && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DestroyWindow(g_hWnd);
    return 0;
}

int main() {
    // 隐藏控制台光标，解决闪烁问题
    SetCursorVisible(false);

    printf("=== Mouse Monitor (Pure User-Mode) ===\n");
    printf("\n");
    printf("This tool reads mouse movement via Raw Input API.\n");
    printf("For raw (unaccelerated) data, enable 'setExtraInfo' in settings.json\n");
    printf("\n");
    printf("Controls:\n");
    printf("  Y / N     - Register or skip mouse device (in registration mode)\n");
    printf("  Caps Lock - Toggle auto-click feature\n");
    printf("  Q         - Quit\n");
    printf("\n");

    // 启动消息循环线程
    HANDLE hThread = CreateThread(NULL, 0, MessageLoopThread, NULL, 0, NULL);
    if (!hThread) {
        printf("[ERROR] Failed to create message thread: %lu\n", GetLastError());
        SetCursorVisible(true);
        return 1;
    }

    // 等待窗口创建完成
    Sleep(100);

    if (!g_hWnd) {
        printf("[ERROR] Window not created\n");
        WaitForSingleObject(hThread, 1000);
        CloseHandle(hThread);
        SetCursorVisible(true);
        return 1;
    }

    printf("[REGISTER] Move the mouse you want to register...\n\n");

    // 主循环
    bool lastCapsState = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

    while (g_running) {
        // 检查按键
        if (_kbhit()) {
            char ch = _getch();

            // 退出键
            if (ch == 'q' || ch == 'Q') {
                if (g_isMouseDown) {
                    MouseLeftUp();
                }
                g_running = false;
                break;
            }

            // 注册模式下的按键处理
            if (g_registrationMode) {
                if ((ch == 'y' || ch == 'Y') && g_pendingDevice != NULL) {
                    // 确认注册当前检测到的设备
                    g_registeredDevice = g_pendingDevice;
                    g_registrationMode = false;
                    printf("\n[OK] Mouse registered: 0x%p\n", g_registeredDevice);
                    printf("[OK] Monitoring started. Only this mouse will trigger auto-click.\n\n");
                    fflush(stdout);
                } else if (ch == 'n' || ch == 'N') {
                    // 跳过当前设备，继续检测
                    g_pendingDevice = NULL;
                    g_pendingDevicePath[0] = L'\0';
                    printf("\n[REGISTER] Skipped. Move another mouse...\n\n");
                    fflush(stdout);
                }
                continue;
            }
        }

        // 注册模式下只处理按键，跳过其他逻辑
        if (g_registrationMode) {
            Sleep(10);
            continue;
        }

        // 检测 Caps Lock 状态变化
        bool currentCapsState = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        if (currentCapsState != lastCapsState) {
            lastCapsState = currentCapsState;
            g_featureEnabled = currentCapsState;

            if (g_featureEnabled) {
                printf("\n[AUTO-CLICK] ENABLED\n");
            } else {
                if (g_isMouseDown) {
                    MouseLeftUp();
                    g_isMouseDown = false;
                }
                printf("\n[AUTO-CLICK] DISABLED\n");
            }
        }

        // 检查是否需要松开鼠标（停止移动超过阈值）
        if (g_featureEnabled && g_isMouseDown) {
            DWORD currentTime = GetTickCount();
            if (currentTime - g_lastMoveTime > STOP_THRESHOLD_MS) {
                MouseLeftUp();
                g_isMouseDown = false;
            }
        }

        Sleep(1);
    }

    // 停止消息循环
    if (g_hWnd) {
        PostMessage(g_hWnd, WM_QUIT, 0, 0);
    }
    WaitForSingleObject(hThread, 1000);
    CloseHandle(hThread);

    // 恢复光标可见性
    SetCursorVisible(true);

    printf("\n\nMonitor stopped.\n");
    return 0;
}
