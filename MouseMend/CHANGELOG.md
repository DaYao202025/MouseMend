# Changelog / 更新记录

## 2.1.2 — 2026-11-23

> 2.1.0 与 2.1.1 是内部阶段版本，没有发布；2.1.2 是 2.0.0 之后的第一个发布版。
> 2.1.0 and 2.1.1 were internal steps and were never published. 2.1.2 is the
> first public build after 2.0.0.

### 修复 / Fixed

- **闪退（重要）** —— 拦截证据那行把毫秒数当成字符串指针去读（格式串写了 `%s`，
  调用处传的是整数）。任何一次被拦下的点击都可能让程序直接退出，2.0.0 起存在。
  反馈中"连点几十下就崩""看到拦截数据后点刷新就崩"都是它。
  A duration was passed to a `%s` conversion, so printf read the millisecond
  count as a string pointer. Any blocked click could crash the program.
- **崩溃转储不完整** —— `dbghelp.dll` 改为从 `System32` 绝对路径加载。之前会被
  其他软件带进进程的副本顶替，实测只记录到 1 个线程。
  The minidump writer now loads dbghelp from System32 by full path; a foreign
  copy had been winning, and the dump held a single thread.
- **窗口跑到屏幕外** —— 当屏幕工作区比窗口小时（高缩放 + 小屏），主窗口居中会算出
  负坐标，标题栏被推到屏幕上方、抓不回来。改为贴左上。
  A window larger than the work area is now pinned to the top left instead of
  being centred off screen.

### 新增 / Added

- **崩溃报告包** —— 崩溃时在程序目录生成单个 `MouseMend-crash-<时间>.zip`，
  内含 minidump、最近 96 条判定、当时的全部设置和异常信息。同目录不可写时依次退到
  桌面 / `%LOCALAPPDATA%` / `%TEMP%`。下次启动会用托盘气泡提示报告在哪。
  托盘菜单「高级 → 导出诊断报告」可随时手动导出同一份报告。
  One zip per crash, containing the dump, the last 96 decisions, the settings
  and the exception. Also available on demand from the tray menu.
- **窗口标题带版本号** —— `MouseMend v2.1.2 —— 鼠标体检修复工具`，以后升版本只改
  `version.h`。
- **格式串静态检查** —— `tests/tools/audit_printf.py`，逐个调用点核对格式串与实参
  类型。上面那个闪退就是它能抓、而编译器和单元测试抓不到的类型。

### 验证 / Verified

Debug 与 Release × x64 / x86 全量构建零告警（含 `/analyze`）；2578 条断言通过
（两个架构各跑一遍）；`e2e` 与 `diagnose` 冒烟全绿。

Debug and Release on x64 and x86 build with no warnings including `/analyze`;
2578 assertions pass on both architectures; both smoke suites are green.

## 2.0.0 — 2026-11-22

首个 MouseMend 版本：更名为 MouseMend、全新的鼠标体检窗口、逐键阈值、智能拖拽、
钩子看门狗、可选的拦截历史与气泡提示。

First MouseMend release: rebrand, the check-up window, per-button thresholds,
Smart Drag, the hook watchdog, optional history and balloons.
