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
#include <string>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cerrno>
#include <cstdlib>

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
wchar_t g_registeredDevicePath[512];          // 已注册设备的 HID 路径
std::string g_registeredHardwareId;           // RawAccel 格式的硬件ID
double g_currentSensitivity = 1.0;            // 当前灵敏度倍数

const DWORD STOP_THRESHOLD_MS = 150;  // 停止移动的阈值（毫秒）
const char* SETTINGS_FILE = "settings.json";
const char* SENS_PROFILE_NAME = "sens_registered_mouse";

// ========== 函数声明 ==========
void MouseLeftDown();
void MouseLeftUp();
void DecodeExtraInfo(ULONG extraInfo, short* rawX, short* rawY);
void SetCursorVisible(bool visible);
void GetDeviceHidPath(HANDLE device, wchar_t* path, size_t pathSize);
std::string DevicePathToHardwareId(const wchar_t* devicePath);
std::string WideToAnsi(const std::wstring& ws);
bool ReadFileContent(const char* path, std::string& content);
bool WriteFileContent(const char* path, const std::string& content);
bool UpdateSettingsForDevice(const std::string& hardwareId, double sensitivity, std::string& errorMsg);
bool RunWriterExe();
void HandleSensitivityInput();
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

// 在devices数组中添加或更新设备映射
bool AddOrUpdateDeviceMapping(std::string& content, const std::string& hardwareId, std::string& errorMsg) {
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
    if (!ReadFileContent(SETTINGS_FILE, content)) {
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
    if (!WriteFileContent(SETTINGS_FILE, content)) {
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
    std::string settingsPath = dir + "settings.json";

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
    if (g_registeredDevice == NULL) {
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
    printf("  L         - Set sensitivity for registered mouse (0.001x - 100x)\n");
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

            // 灵敏度调整键（L）- 在非注册模式下可用
            if ((ch == 'l' || ch == 'L') && !g_registrationMode) {
                HandleSensitivityInput();
                continue;
            }

            // 注册模式下的按键处理
            if (g_registrationMode) {
                if ((ch == 'y' || ch == 'Y') && g_pendingDevice != NULL) {
                    // 确认注册当前检测到的设备
                    g_registeredDevice = g_pendingDevice;
                    g_registrationMode = false;

                    // 保存设备路径并转换为硬件ID
                    if (g_pendingDevicePath[0] == L'\0') {
                        GetDeviceHidPath(g_pendingDevice, g_pendingDevicePath, sizeof(g_pendingDevicePath)/sizeof(wchar_t));
                    }
                    wcscpy_s(g_registeredDevicePath, g_pendingDevicePath);
                    g_registeredHardwareId = DevicePathToHardwareId(g_registeredDevicePath);

                    printf("\n[OK] Mouse registered: 0x%p\n", g_registeredDevice);
                    if (g_registeredDevicePath[0] != L'\0') {
                        wprintf(L"[PATH] %ls\n", g_registeredDevicePath);
                    }
                    if (!g_registeredHardwareId.empty()) {
                        printf("[HWID] %s\n", g_registeredHardwareId.c_str());
                    } else {
                        printf("[WARN] Could not extract hardware ID from device path.\n");
                    }
                    printf("[OK] Monitoring started. Press L to adjust sensitivity.\n\n");
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
