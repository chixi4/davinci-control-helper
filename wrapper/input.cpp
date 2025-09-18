#include "input.h"

using namespace System;
using namespace System::Collections::Generic;
using namespace System::Runtime::InteropServices;

#include <vector>

// 先声明 Win32 回调，供窗口类注册时使用
LRESULT CALLBACK RawInputSource_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

[StructLayout(LayoutKind::Sequential, CharSet = CharSet::Unicode)]
public ref struct RawInputDevice {
    System::IntPtr handle;

    [MarshalAs(UnmanagedType::ByValTStr, SizeConst = MAX_NAME_LEN)]
    System::String^ name;

    [MarshalAs(UnmanagedType::ByValTStr, SizeConst = MAX_DEV_ID_LEN)]
    System::String^ id;
};

// 独立线程 RAW INPUT 事件源，避免 UI 消息泵干扰
public ref class RawInputSource
{
private:
    System::Threading::Thread^ worker;
    volatile bool running = false;
    IntPtr hwnd = IntPtr::Zero;

public:
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_INPUT)
        {
            // 复用缓冲，WndProc 专用线程，使用静态缓冲安全
            static std::vector<BYTE> s_buf;
            if (s_buf.empty()) s_buf.resize(256);

            UINT cap = (UINT)s_buf.size();
            UINT read = cap;
            UINT ret = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, s_buf.data(), &read, sizeof(RAWINPUTHEADER));

            if (ret == (UINT)-1 || read > cap)
            {
                // 缓冲不足或失败：查询所需大小并重试
                UINT need = 0;
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &need, sizeof(RAWINPUTHEADER)) != 0 || need == 0)
                {
                    return 0;
                }
                if (s_buf.size() < need) s_buf.resize(need);
                read = need;
                ret = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, s_buf.data(), &read, sizeof(RAWINPUTHEADER));
                if (ret == (UINT)-1 || read == 0) return 0;
            }

            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(s_buf.data());
            if (raw->header.dwType == RIM_TYPEMOUSE)
            {
                int dx = raw->data.mouse.lLastX;
                int dy = raw->data.mouse.lLastY;
                if (dx != 0 || dy != 0)
                {
                    DeviceMoved(System::IntPtr(raw->header.hDevice), dx, dy);
                }
            }
            return 0;
        }
        else if (msg == WM_INPUT_DEVICE_CHANGE)
        {
            DeviceListChanged();
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

private:
    void MessageLoop()
    {
        // 提升优先级以减少调度抖动（谨慎使用极端等级）
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &RawInputSource_WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RawInputSourceMessageWindow";
        RegisterClassExW(&wc);

        HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hWnd)
        {
            return;
        }

        auto gch = System::Runtime::InteropServices::GCHandle::Alloc(this);
        auto ip = System::Runtime::InteropServices::GCHandle::ToIntPtr(gch);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)ip.ToPointer());

        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
        rid.hwndTarget = hWnd;
        RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));

        hwnd = IntPtr(hWnd);
        running = true;

        MSG msg;
        while (running)
        {
            BOOL ok = GetMessageW(&msg, nullptr, 0, 0);
            if (ok <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        RAWINPUTDEVICE ridRemove = {};
        ridRemove.usUsagePage = 0x01;
        ridRemove.usUsage = 0x02;
        ridRemove.dwFlags = RIDEV_REMOVE;
        ridRemove.hwndTarget = hWnd;
        RegisterRawInputDevices(&ridRemove, 1, sizeof(RAWINPUTDEVICE));

        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        gch.Free();
        DestroyWindow(hWnd);
        hwnd = IntPtr::Zero;
    }

public:
    event System::Action<IntPtr, int, int>^ DeviceMoved;
    event System::Action^ DeviceListChanged;

    void Start()
    {
        if (worker != nullptr) return;
        worker = gcnew System::Threading::Thread(gcnew System::Threading::ThreadStart(this, &RawInputSource::MessageLoop));
        worker->IsBackground = true;
        worker->Name = "RawInputSourceThread";
        worker->Start();
    }

    void Stop()
    {
        running = false;
        if (hwnd != IntPtr::Zero)
        {
            PostMessageW(static_cast<HWND>(hwnd.ToPointer()), WM_QUIT, 0, 0);
        }
        if (worker != nullptr)
        {
            worker->Join();
            worker = nullptr;
        }
    }

    ~RawInputSource()
    {
        Stop();
    }
};

// Win32 回调（自由函数），转发到 RawInputSource 实例
static LRESULT CALLBACK RawInputSource_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LONG_PTR data = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (data == 0)
    {
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    auto ip = System::IntPtr((void*)data);
    auto gch = System::Runtime::InteropServices::GCHandle::FromIntPtr(ip);
    RawInputSource^ self = static_cast<RawInputSource^>(gch.Target);
    return self->HandleMessage(hWnd, msg, wParam, lParam);
}

static int CompareByID(RawInputDevice^ x, RawInputDevice^ y)
{
    return String::Compare(x->id, y->id);
}

public ref struct MultiHandleDevice {
    System::String^ name;
    System::String^ id;
    List<System::IntPtr>^ handles;

    // Returned list represents the current connected raw input devices,
    // where each device has a distinct device id
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/install/device-ids
    static IList<MultiHandleDevice^>^ GetList()
    {
        return ListMaker::MakeList()->AsReadOnly();
    }

    ref class ListMaker {
        List<RawInputDevice^>^ devices = gcnew List<RawInputDevice^>();

        delegate void NativeDevHandler(rawinput_device&);

        void Add(rawinput_device& dev)
        {
            devices->Add(Marshal::PtrToStructure<RawInputDevice^>(IntPtr(&dev)));
        }

        ListMaker() {}
    public:
        static List<MultiHandleDevice^>^ MakeList()
        {
            auto maker = gcnew ListMaker();
            NativeDevHandler^ del = gcnew NativeDevHandler(maker, &Add);
            GCHandle gch = GCHandle::Alloc(del);
            auto fp = static_cast<void (*)(rawinput_device&)>(
                Marshal::GetFunctionPointerForDelegate(del).ToPointer());
            rawinput_foreach(fp);
            gch.Free();

            auto ret = gcnew List<MultiHandleDevice^>();
            auto count = maker->devices->Count;
            auto first = 0;
            auto last = 0;

            if (count > 0) {
                maker->devices->Sort(gcnew Comparison<RawInputDevice^>(&CompareByID));
                while (++last != count) {
                    if (!String::Equals(maker->devices[first]->id, maker->devices[last]->id)) {
                        auto range = maker->devices->GetRange(first, last - first);
                        ret->Add(gcnew MultiHandleDevice(range));
                        first = last;
                    }
                }
                auto range = maker->devices->GetRange(first, last - first);
                ret->Add(gcnew MultiHandleDevice(range));
            }

            return ret;
        }
    };

private:
    MultiHandleDevice(IEnumerable<RawInputDevice^>^ seq)
    {
        auto it = seq->GetEnumerator();
        if (it->MoveNext()) {
            name = it->Current->name;
            id = it->Current->id;
            handles = gcnew List<IntPtr>();
            do handles->Add(it->Current->handle); while (it->MoveNext());
        }
    }
};
