/*
 * Mouse Monitor - 纯用户态版本
 *
 * 功能：
 * - 通过 Raw Input API 读取鼠标移动数据
 * - 从 ExtraInformation 字段解码原始移动量（需要 rawaccel 启用 setExtraInfo）
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

const DWORD STOP_THRESHOLD_MS = 150;  // 停止移动的阈值（毫秒）

// ========== 函数声明 ==========
void MouseLeftDown();
void MouseLeftUp();
void DecodeExtraInfo(ULONG extraInfo, short* rawX, short* rawY);
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
    printf("=== Mouse Monitor (Pure User-Mode) ===\n");
    printf("\n");
    printf("This tool reads mouse movement via Raw Input API.\n");
    printf("For raw (unaccelerated) data, enable 'setExtraInfo' in settings.json\n");
    printf("\n");
    printf("Controls:\n");
    printf("  Caps Lock - Toggle auto-click feature\n");
    printf("  Q         - Quit\n");
    printf("\n");

    // 启动消息循环线程
    HANDLE hThread = CreateThread(NULL, 0, MessageLoopThread, NULL, 0, NULL);
    if (!hThread) {
        printf("[ERROR] Failed to create message thread: %lu\n", GetLastError());
        return 1;
    }

    // 等待窗口创建完成
    Sleep(100);

    if (!g_hWnd) {
        printf("[ERROR] Window not created\n");
        WaitForSingleObject(hThread, 1000);
        CloseHandle(hThread);
        return 1;
    }

    printf("[OK] Monitoring started...\n\n");

    // 主循环
    bool lastCapsState = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

    while (g_running) {
        // 检查退出键
        if (_kbhit()) {
            char ch = _getch();
            if (ch == 'q' || ch == 'Q') {
                if (g_isMouseDown) {
                    MouseLeftUp();
                }
                g_running = false;
                break;
            }
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

    printf("\n\nMonitor stopped.\n");
    return 0;
}
