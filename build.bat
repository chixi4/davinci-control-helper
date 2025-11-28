@echo off
REM Mouse Monitor 编译脚本
REM 支持 MSVC 和 MinGW (g++)

echo === Compiling Mouse Monitor ===
echo.

REM 检查 g++ (MinGW)
where g++ >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo Found MinGW g++, compiling...
    g++ -O2 -Wall -o mouse_monitor.exe mouse_monitor.cpp -luser32 -static
    goto :check_result
)

REM 尝试使用 Visual Studio 2022
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    echo Found VS2022, compiling...
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    cl /EHsc /O2 /W3 mouse_monitor.cpp /link user32.lib /out:mouse_monitor.exe
    del mouse_monitor.obj 2>nul
    goto :check_result
)

REM 尝试使用 Visual Studio 2019
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    echo Found VS2019, compiling...
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    cl /EHsc /O2 /W3 mouse_monitor.cpp /link user32.lib /out:mouse_monitor.exe
    del mouse_monitor.obj 2>nul
    goto :check_result
)

echo [ERROR] No compiler found!
echo Please install MinGW (g++) or Visual Studio.
pause
exit /b 1

:check_result
if exist mouse_monitor.exe (
    echo.
    echo [SUCCESS] mouse_monitor.exe created!
    echo.
    echo Before running, make sure to:
    echo   1. Enable setExtraInfo in settings.json:
    echo      "Device settings": {
    echo          "setExtraInfo": true,
    echo          ...
    echo      }
    echo   2. Restart rawaccel.exe to apply settings
    echo.
) else (
    echo.
    echo [ERROR] Compilation failed!
)

pause
