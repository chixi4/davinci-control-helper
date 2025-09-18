# DualSensManager

一个面向双鼠标的低延迟灵敏度管理小工具：识别左右手鼠标（HID），为左/右分配独立 Profile，并提供“移动即按压”的极低延迟链路。支持参数记忆与退出恢复默认。

## 特性
- 左/右手鼠标识别与绑定（RAWINPUT，基于句柄集合匹配）
- 移动即按压：左手移动立刻 `LEFTDOWN`，右手移动立刻 `LEFTUP`
- 低延迟实现：RAWINPUT 独立线程、优先级提升、缓冲复用
- 参数记忆：
  - 是否勾选“移动即按压”
  - 左/右手 HID 与显示名称
  - 左手灵敏度（left multiplier）
- 退出恢复：关闭程序时自动恢复驱动配置为启动快照（不留残效）

## 快速开始（二进制）
1. 下载 Release（或本地构建）
2. 放在同一目录运行：
   - `DualSensManager.exe`
   - `wrapper.dll`
   - `Newtonsoft.Json.dll`
3. GUI 中点击“检测右手/检测左手”，晃动对应鼠标完成绑定
4. 勾选“移动即按压”体验低延迟

## 构建
- 依赖：Windows + Visual Studio 2022（Desktop C++、.NET Desktop、C++/CLI）
- 命令（Developer Command Prompt）：
```
nuget.exe restore .\dual-sens-manager\dual-sens-manager.csproj
msbuild .\dual-sens-manager\dual-sens-manager.csproj /m /p:Configuration=Release /p:Platform=x64
```
- 产物：`x64\Release\DualSensManager.exe`

## 运行说明
- 第一次运行需“检测”绑定左右手，设置 left multiplier；程序会记忆这些参数
- 退出时程序会把驱动配置恢复到启动时快照
- 若 Rebuild 失败，请先退出正在运行的 `DualSensManager.exe`

## 许可与致谢
- 本工具包含基于 RAWINPUT/C++/CLI 的封装与改造；感谢社区相关项目的思路启发
- 如需补充第三方依赖许可（例如 Json.NET），请在本段追加

