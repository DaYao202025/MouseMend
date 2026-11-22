# MouseMend

<div align="center">

![Version](https://img.shields.io/badge/version-2.0.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey.svg)
![Language](https://img.shields.io/badge/language-C-informational.svg)
![Toolset](https://img.shields.io/badge/toolset-MSVC%20v145-purple.svg)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen.svg)

<br>

**Stop False Clicks — Mend Your Mouse in Software**<br>
**鼠标误触不用换硬件 —— 让软件把按键修回来**

<br>

[English](#-english) | [中文](#-中文) | [构建](#-build--构建) | [测试](#-tests--测试)

</div>

---

<a name="-english"></a>

## English

### What is MouseMend?

A mechanical mouse switch wears out long before the rest of the mouse does. The
contacts start to bounce: one physical press is reported as two, a wheel scroll
jumps back one notch, a drag lets go halfway across the screen.

MouseMend sits in the notification area and filters the mouse event stream in
software, so the worn switch stops mattering. No driver, no kernel component, no
network access, no installation — a single executable.

It is **not** a driver and it cannot repair hardware. It makes the *reported*
presses match the *intended* presses.

### The problem it solves

| The symptom | What actually happens | What MouseMend does |
| :--- | :--- | :--- |
| **Double clicks on a single press** | Contact bounce: two `DOWN` events a few milliseconds apart | Suppresses the second press and its release, so exactly one click reaches the target |
| **Wheel jumps backwards** | A worn encoder emits one event with the opposite sign | Drops the reversal when it arrives within the debounce window of the previous scroll |
| **Files drop mid-drag** | Bounce interrupts a held button during a drag | Holds the release back briefly, then re-injects it (Smart Drag) |

### Key features

* **Full coverage of seven axes** — left, right, middle, `X1` (back), `X2`
  (forward), vertical wheel and horizontal wheel. Each one can be enabled,
  disabled and tuned separately.
* **Per-button thresholds** — three presets (Game 40 ms / Office 50 ms /
  Strict 60 ms) plus a custom value per button.
* **Smart Drag (Hybrid Heuristic)** — distinguishes a click from a drag by
  checking how long the button was held and how far the cursor moved. A release
  that follows a real drag is delayed and then re-injected, so a bouncing switch
  cannot break the drag. Both the hold time and the distance are configurable.
* **Check-up window** — a read-only window that shows what the engine is
  actually deciding, click by click: time, button, the gap that was measured,
  the hold time and the verdict. It also lists the false clicks that were
  blocked, with the evidence the engine used, and a self-check report.
* **Near misses** — presses that *passed* but only just cleared the threshold are
  counted and shown. "0 blocked (7 near misses)" means the switch is already
  failing and the threshold is still saving you.
* **Tray hover at a glance** — the tooltip shows the version, the protection
  state and how many false clicks have been blocked this session.
* **Optional balloon on block** — off by default, rate limited to one every
  20 seconds, silent while a full-screen application or a presentation is
  running.
* **Optional blocked-click history** — off by default, one file per day, written
  only when something is actually blocked.
* **Hook watchdog** — a low-level mouse hook can be removed by the system
  without any notice. The watchdog notices the silence, reinstalls the hook and
  re-samples its baseline, once per 30 seconds at most.
* **Crash dumps** — if the process ever dies, a minidump is written next to the
  log directory so the crash can be diagnosed instead of guessed at.

### How the decision is made

The engine compares **the previous event of the same button → this press**. On a
normal click sequence that is exactly "release of the last click → this press",
which is the same rule other software debouncers use. The gap is measured before
any timestamp is updated, so the number the window shows is the number the
decision used — the log and the verdict can never disagree.

A blocked press is paired with its release, and that release is swallowed too: the
target application must see a complete click or none at all, never a press whose
release never arrives.

Two safety valves exist on purpose:

* A blocked press expects its release immediately. If the pairing release never
  arrives (a lost event, a button switched off mid-press), the block expires
  after 2 seconds and the release is delivered normally. Without that timeout a
  single lost event would leave the button stuck inside the system.
* Disabling a button, disabling Smart Drag, resetting statistics or exiting all
  flush any deferred release first. A pending release is never dropped together
  with the state that created it.

### The check-up window

The window is the main window of the application: it opens on every start, and
closing it **hides it into the tray** instead of destroying it. The log, the
counters and the scroll positions survive, so reopening it continues the same
session rather than starting a new one.

It is strictly read-only. It cannot change a threshold, it does not write the
registry, and it never decides anything on its own: every verdict it prints comes
from the engine.

* **Test area** — clicks inside the marked area are logged. Clicks elsewhere are
  ignored, so working in other windows does not pollute the log.
* **One click, one row** — the press opens the row, its release fills in the hold
  time. A click is never shown as two rows the user has to pair up mentally.
* **Filter** — *Auto* shows every button. Untick it to watch a single button.
* **Newest first** — the most recent click is on top.
* **Live language switch** — changing the language from the tray menu repaints
  the open window immediately.

<p align="center">
  <img src="https://cdn.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/checkup-zh.png" width="430" alt="Check-up window, Chinese UI">
  <img src="https://cdn.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/checkup-en.png" width="430" alt="Check-up window, English UI">
  <br>
  <img src="https://cdn.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/tray-menu.png" width="230" alt="Tray menu">
  <br>
  <sub>检查窗口（中 / 英）与托盘右键菜单 · check-up window and tray menu</sub>
</p>

### Tray behaviour

| Action | Result |
| :--- | :--- |
| Right click | Context menu |
| Single left click | Nothing (deliberate: a stray click must not open a window) |
| Double left click | Show the check-up window |
| Keyboard select (`NIN_KEYSELECT`) | Show the check-up window |
| Clicking the balloon | Show the check-up window |

### Command line

```
MouseMend-x64.exe --version     Print the version and exit
MouseMend-x64.exe --diagnose    Print a read-only self check and exit
```

`--diagnose` reports whether the hook installs, which buttons are protected, the
current profile, whether settings are writable, the autostart state and an
overall verdict. It never modifies anything. The report is ASCII on purpose so
it survives being piped through a console with a different code page.

### Where settings live

Everything is stored per user, in `HKCU\Software\MouseMend`. Nothing is written
system-wide and no administrator rights are required — the executable runs as
`asInvoker`.

Changes made from the menu are written immediately. Continuous changes (dragging
a value) are coalesced and written 400 ms after the last change: registry writes
on the thread that owns the mouse hook are exactly the kind of stall that makes
Windows drop the hook.

### Requirements

* Windows 10 or Windows 11, x64 or x86.
* No runtime, no installer, no dependencies. The executable is self-contained.
* Administrator rights are **not** required.

### Known limits

* **Hardware cannot be repaired.** A switch that bounces badly will still bounce;
  MouseMend only hides the consequences.
* **Elevated windows are invisible to it.** Windows does not deliver input from
  a higher integrity level to a normal process (UIPI), so clicks inside an
  administrator window are neither seen nor filtered. The watchdog may report a
  hook recovery in that situation — that is the expected behaviour.
* **Some security software blocks low-level hooks.** If the hook cannot be
  installed at all, `--diagnose` says so. MouseMend does not fight for it.
* **It only filters the mouse.** Keyboard chatter is out of scope.

### Privacy

No network access, no telemetry, no analytics, no update check. The only files it
ever writes are its own settings in the registry, an optional blocked-click log
and an optional crash dump. Nothing leaves the machine.

---

<a name="-中文"></a>

## 中文

### MouseMend 是什么

机械鼠标的微动开关，往往比鼠标本体先坏。触点开始弹跳之后：按一下变成两下、
滚轮往回跳一格、拖拽拖到一半松手。

MouseMend 常驻托盘，在软件层过滤鼠标事件流 —— 让磨损的开关不再影响使用。
没有驱动、没有内核组件、不联网、不用安装，就是一个可执行文件。

它**不是驱动**，也修不好硬件。它做的是让**系统收到的**按键与**你想按的**按键
一致。

### 它解决什么问题

| 你遇到的现象 | 实际发生的事 | MouseMend 的处理 |
| :--- | :--- | :--- |
| **按一下变两下** | 触点弹跳：相隔几毫秒的两次按下 | 吞掉第二次按下和它的松手，目标程序只收到一次完整点击 |
| **滚轮往回跳** | 磨损的编码器发出一个符号相反的事件 | 若这次反向出现在去抖窗口内，直接丢弃 |
| **拖拽中途断开** | 按住期间触点弹跳，打断了拖拽 | 先把松手扣住一小会儿，再补发（智能拖拽） |

### 主要功能

* **七个轴全覆盖** —— 左键、右键、中键、`X1`（后退）、`X2`（前进）、垂直滚轮、
  水平滚轮。每个轴都可以单独启用、单独调阈值。
* **逐键阈值** —— 三档预设（游戏 40 ms / 办公 50 ms / 严格 60 ms），也可以为
  每个键单独填自定义值。
* **智能拖拽（混合启发式）** —— 靠"按住时长 + 光标位移"区分点击和拖拽。真实拖拽
  之后的松手会被扣住一小段再补发，弹跳就打断不了拖拽。按住时长和位移距离都可调。
* **鼠标体检窗口** —— 只读窗口，逐次点击告诉你引擎到底怎么判的：时间、按键、
  实测间隔、按住时长、判定结果。同时列出被拦下的误触、引擎当时用的证据，以及
  一份自检报告。
* **临界点击（near miss）** —— 那些**放行了**、但只是勉强过线的按下会被单独计数并
  显示。"共拦截 0 次（临界 7 次）"的意思是：鼠标已经在坏了，只是阈值还在替你挡。
* **悬浮即看** —— 鼠标停在托盘图标上就能看到版本、防护状态、本会话已拦截次数。
* **拦截气泡提示（默认关）** —— 全屏程序或演示模式下自动静默，最快 20 秒一次。
* **拦截历史（默认关）** —— 一天一个文件，只有真的拦到东西才写盘。
* **看门狗** —— 低级鼠标钩子可能被系统静默摘掉。看门狗会发现"没有输入"这件事，
  重装钩子并重采样基线，最快 30 秒一次。
* **崩溃转储** —— 万一进程真的挂了，会在日志目录旁写一个 minidump，让崩溃可以
  被查清，而不是靠猜。

### 判定是怎么做的

引擎比较的是**同一个按键的上一次事件 → 这次按下**。在普通点击序列上，它就等于
"上次点击的松手 → 这次按下"，与其他软件去抖工具的口径一致。间隔在任何时间戳被
更新之前测量，所以窗口里显示的数字就是判定用的数字 —— 日志与结论不可能互相矛盾。

被拦下的按下会和它的松手配对，松手也一起吞掉：目标程序要么收到一次完整点击，
要么什么也收不到，绝不会收到一个永远等不到松手的按下。

有两个安全阀是刻意留的：

* 被拦的按下，期待它的松手紧随其后。如果配对的松手一直没来（事件丢失、按键中途
  关开），这次拦截在 2 秒后过期，松手正常放行。没有这个超时，一次丢事件就会让
  系统里的按键永久卡住。
* 关闭某个按键、关闭智能拖拽、清空统计、退出程序，都会先把待补发的松手冲出去。
  待补发的松手绝不会跟着创建它的状态一起被丢掉。

### 鼠标体检窗口

这个窗口就是程序的主窗体：每次启动都会打开，点关闭**只是收进托盘**，不销毁。
日志、计数、滚动位置全都留着，再打开还是同一个会话，而不是从头开始。

它严格只读：改不了阈值、不写注册表、也从不自己下结论 —— 它打印的每一条判定都来自
引擎。

* **测试区** —— 只有标记区域内的点击才进日志，在别的窗口工作不会污染记录。
* **一次点击一行** —— 按下开行，松手回填按住时长。一次点击不会拆成两行让人自己配对。
* **筛选** —— `自动` 显示全部按键；取消勾选后只看单个按键。
* **最新在最上面** —— 刚发生的事不需要滚动就能看到。
* **语言热切换** —— 从托盘菜单改语言，开着的窗口立刻重绘。

### 托盘操作

| 操作 | 结果 |
| :--- | :--- |
| 右键 | 弹出菜单 |
| 左键单击 | 什么都不做（故意的：误点不该弹出窗口） |
| 左键双击 | 打开体检窗口 |
| 键盘选中（`NIN_KEYSELECT`） | 打开体检窗口 |
| 点击气泡 | 打开体检窗口 |

### 命令行

```
MouseMend-x64.exe --version     打印版本后退出
MouseMend-x64.exe --diagnose    打印只读自检报告后退出
```

`--diagnose` 会报告钩子是否装得上、哪些按键在防护中、当前配置、设置是否可写、
开机自启状态，以及一个总体结论。它不修改任何东西。报告刻意保持纯 ASCII，
这样即使被管道传给代码页不同的控制台也不会变成乱码。

### 设置存在哪

全部按用户存于 `HKCU\Software\MouseMend`。不写系统级位置，不需要管理员权限 ——
可执行文件的清单是 `asInvoker`。

菜单里的改动立即落盘。连续输入（拖动数值）会合并，在最后一次改动 400 ms 后写一次：
在拥有鼠标钩子的那个线程上做注册表写入，正是会让 Windows 摘掉钩子的那种停顿。

### 运行要求

* Windows 10 或 Windows 11，x64 或 x86。
* 无运行时、无安装程序、无第三方依赖，单个可执行文件。
* **不需要**管理员权限。

### 已知限制

* **修不好硬件。** 弹跳严重的开关还是会弹，MouseMend 只是把后果挡住。
* **看不见提权窗口。** Windows 不会把更高完整性级别窗口的输入投递给普通进程
  （UIPI），所以管理员窗口里的点击既看不到也过滤不了。这种情况下看门狗可能报告
  一次钩子恢复 —— 那是预期行为。
* **部分安全软件会拦截低级钩子。** 如果钩子根本装不上，`--diagnose` 会直说。
  MouseMend 不会去抢。
* **只管鼠标。** 键盘连击不在范围内。

### 隐私

不联网、不上报、不统计、不检查更新。它只会写三类东西：注册表里的自身设置、
可选的拦截日志、可选的崩溃转储。任何数据都不会离开本机。

---

<a name="-build--构建"></a>

## Build / 构建

Visual Studio 2022 或更新版本（MSVC，`/std:c11` 不强制，代码按 C89 友好的写法组织），
Windows SDK 10。

```bat
msbuild MouseMend.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:RunCodeAnalysis=true
msbuild MouseMend.sln /t:Build   /p:Configuration=Release /p:Platform=x86
```

> The solution platform is named `x86`, not `Win32`. `/p:Platform=Win32` fails with
> MSB4126. This is a naming detail of the solution file, not an error.
>
> 解决方案里的平台名是 `x86`，不是 `Win32`；传 `/p:Platform=Win32` 会报 MSB4126。

Output / 产物：

```
bin\x64\Release\MouseMend-x64.exe
bin\x86\Release\MouseMend-x86.exe
```

The build is expected to be warning free, including the static analyser
(`/analyze`). The four configurations (Debug/Release × x64/x86) are all expected
to compile cleanly.

构建应当零告警，包括静态分析（`/analyze`）。四个配置（Debug/Release × x64/x86）
都应编译干净。

### Source layout / 目录结构

```
main.c                      window procedure, tray, timers, application wiring
menu_ids.h                  single source of truth for menu command ids
version.h                   product identity and version (single source)
MouseMend.rc                resources; Chinese strings rely on /c65001

src/core/
  mouse_hook.c              WH_MOUSE_LL install / uninstall / probe
  debouncer.c               the decision state machine (the actual product)
  config.c                  presets, ranges, button table
  settings.c                registry load / save
  app_controller.c          the only writer; mutations only touch memory
  app_snapshot.c            builds the read-only view for the window
  notify_policy.c           balloon throttling (pure functions)
  block_log.c               queued daily log files
  event_trace.c             128 entry ring the hook writes into
  hook_watchdog.c           detection of a silently removed hook

src/ui/
  settings_window.c         the check-up window (dialog, timers, controls)
  log_format.c              pure text rendering of engine output
  context_menu.c            tray menu construction
  tray_icon.c               Shell_NotifyIcon wrapper
  language.c                every user-visible string, both languages
  input_box.c               small modal prompt for a custom value
  about_window.c            the about dialog
  ripple_view.c             the click ripples in the test area
  window_layout.c           shared monitor-aware centering

src/utils/
  autostart.c               HKCU Run entry
  diagnostics.c             --version / --diagnose
  crash_dump.c              minidump writer (dbghelp loaded at runtime)
  logger.c                  debug-only file log

tests/                      console test suite, no window required
```

### Design rules that matter / 关键设计约束

These are not style preferences. Each one exists because breaking it caused a real
bug.

这些不是风格偏好，每一条都是踩过坑之后定下来的。

1. **The hook callback only touches memory.** No registry, no file, no
   `PostMessage`, no `Shell_NotifyIcon`. Anything slow is turned into a flag and
   handled by the 200 ms timer. A low-level hook that overruns its budget is
   removed by Windows without a message.
2. **Every settings change goes through `app_controller`.** It is the only writer.
   Setters touch memory and mark the state dirty; persistence is debounced.
3. **A deferred release is never dropped by clearing a flag.** It is injected with
   `SendInput` first, then the flag is cleared. Every path that disables Smart
   Drag, disables a button, resets statistics or shuts down does this.
4. **A blocked press expires after 2 seconds.** If its release never arrives, the
   block must not survive, or the button stays stuck in the system.
5. **The window never decides.** Every verdict comes from `DebounceDecision`.
6. **Timestamps are only subtracted through `elapsed_since()`.** `GetTickCount64`
   is 64-bit, but one overflow helper is cheaper than one wrap-around bug.

---

<a name="-tests--测试"></a>

## Tests / 测试

### Unit tests / 单元测试

```bat
tests\bin\x64\Release\MouseMendTests.exe
tests\bin\Win32\Release\MouseMendTests.exe
```

A console program with no window and no message loop. It covers the menu id
codec, the language tables, the debounce state machine, the Smart Drag paths,
the persistence controller, the log file naming, the watchdog, the ripple maths
and the log formatting.

一个不需要窗口和消息循环的控制台程序。覆盖：菜单 ID 编解码、语言表、去抖状态机、
智能拖拽各条路径、持久化控制器、日志文件命名、看门狗、涟漪几何、日志格式化。

Both architectures must be run. A failure that shows up on only one of them is
almost always uninitialised memory, not an architecture-specific difference.

两个架构都必须跑。只在某一个上失败，几乎总是未初始化内存，而不是架构差异。

### Smoke tests / 冒烟测试

```powershell
pwsh -File tests\smoke\e2e.ps1          # start, menu, presets, language, exit
pwsh -File tests\smoke\diagnose.ps1     # --version and --diagnose
pwsh -File tests\smoke\stuck_key.ps1 -Force   # needs an interactive desktop
```

The first two export `HKCU\Software\MouseMend` before they start and restore it
afterwards, so running them does not change your settings. `stuck_key.ps1` drives
a real deferred release and checks that no button is left pressed; it is skipped
when the session is locked.

前两个脚本会先导出、结束时还原 `HKCU\Software\MouseMend`，不会动你的实际设置。
`stuck_key.ps1` 会真实触发一次延迟松手并确认没有按键卡住；锁屏时会自动跳过。

---

<a name="-credits--致谢"></a>

## Credits / 致谢

MouseMend started from an idea that already had a working implementation: the
[original project](https://gitee.com/Matreshka15/MouseFix). The debounce rule —
comparing a press against the previous event of the same button — comes from
there, and so does the general shape of the problem. Everything else in this
repository was written for MouseMend: the seven-axis model, the per-button
thresholds, Smart Drag, the check-up window, the hook watchdog, the crash dumps
and the layout.

MouseMend 的想法起点就是上面链接的原项目，"上一次事件 → 这次按下"这条去抖口径
源自那里，要解决的问题也源自那里。除此之外的实现都是为 MouseMend 写的：
七个轴的模型、逐键阈值、智能拖拽、鼠标体检窗口、钩子看门狗、崩溃转储与界面布局。

Released under the MIT licence — see [LICENSE](LICENSE).<br>
基于 MIT 许可证发布，见 [LICENSE](LICENSE)。

---

<div align="center">

**MouseMend 2.0.0** — MIT licensed, built with plain C and the Win32 API.<br>
**MouseMend 2.0.0** —— MIT 许可证，纯 C 与 Win32 API 实现。

</div>
