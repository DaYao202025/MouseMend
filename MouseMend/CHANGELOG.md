# Changelog / 更新记录

## 2.1.4 — 2026-11-25

### 修复 / Fixed

- **松手送不出去时不再静默** —— 当前窗口以管理员身份运行时，Windows 不允许普通权限的
  程序向它发送输入，`SendInput` 返回 0，那一次松手就丢了。以前没有任何地方记录，
  用户只觉得"这次拖拽结束得有点晚"，报告里也一切正常。现在会计数、写进诊断报告
  （`inject_fail`），并在**每次运行只提示一次**。
  A release that never reaches the system is now counted, reported and mentioned
  once. Until now it was indistinguishable from the program doing nothing wrong.

- **测试里的一条假通过** —— `stuck_key.ps1` 用 `SendInput` 造输入，而产品刻意忽略注入
  事件，所以它测的那条路径**根本没有产品参与**，断言永远成立。改为验证一条真实且
  重要的性质：**注入的输入必须原样通过**（否则远程桌面用户的所有点击都会被当成本地
  点击去抖，鼠标会变粘）。延迟松手本身改由单元测试覆盖 —— 注入出口可替换，测试直接
  断言 `UP → DOWN(原坐标)` 的顺序与坐标，**不再真的点桌面**。

### 变更 / Changed

- 单元测试现在完全不产生真实输入（以前会真的发几个松手事件）。

### 验证 / Verified

Debug 与 Release × x64 / x86 全量构建零告警（含 `/analyze`）；2621 条断言通过；
`e2e` 与 `diagnose` 冒烟各 0 failure。

---

## 2.1.3 — 2026-11-25

### 修复 / Fixed

- **卡死（重要）** —— `SendInput` 原来是在低级鼠标钩子的回调里被调用的。低级钩子跑在
  安装它的那个线程上，而注入的事件又要再过一遍钩子链，链头正是这个线程自己 —— 自等待，
  永不返回。表现是：**全系统鼠标卡滞两三秒**（`LowLevelHooksTimeout` 到期摘掉钩子），
  之后**只有这个程序点不动**，进程结束不掉，只能注销或重启。触发条件是"松手被推迟时
  又来了一次按下"，在绘图、设计类软件里是很常见的操作。
  现在钩子回调只改状态，注入交给一直存在的推迟松手定时器（同线程，但在回调之外）。
  Injection no longer happens inside the low-level hook callback, which deadlocked
  the calling thread against itself and could only be cleared by signing out.

### 新增 / Added

- **卡死报告** —— 以前卡死什么都不留（走不到崩溃处理器）。现在有看门狗线程盯着窗口
  心跳，安静超过 10 秒就自动生成 `MouseMend-hang-<时间>.zip`（minidump + 文字报告），
  下次启动用托盘气泡告知路径。**它救不了卡死的进程，只是让下次有证据。**
  A watcher thread now writes a hang bundle, so a frozen process leaves evidence
  behind for the first time.

### 变更 / Changed

- 自检提示里的"发给客服"改为"发给作者"（英文 `support` → `the author`）。
- 手动导出的诊断包改名为 `MouseMend-diagnostics-<时间>.zip`（原 `MouseMend-support-`）。

### 验证 / Verified

Debug 与 Release × x64 / x86 全量构建零告警（含 `/analyze`）；2594 条断言通过；
`e2e` 与 `diagnose` 冒烟各 0 failure；看门狗经"只挂起 UI 线程"实测，15 秒内产出报告。

---

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
