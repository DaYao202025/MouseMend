# Changelog / 更新记录

## 2.2.2 — 2026-11-25

### 修复 / Fixed

- **左键"粘连"（这次的主要问题）** —— 智能拖拽原来用「按得久 **或** 移动超过
  5 像素」判断拖拽，两条只要中一条就把松手扣住 50 ms 再补发。**这 50 ms 里系统
  仍然认为左键是按下的**，于是手一动就会拖动窗口、拉长文字选区，一次普通单击被
  感受成长按。
  现在改成**两个条件都满足**才算拖拽（按得久 **而且** 移动过），普通点击立即放行，
  系统不会停留在"按键还按着"的状态。默认扣留时间同时从 50 ms 降到 **20 ms**，
  真实拖拽结束时的尾巴也缩短到几乎察觉不到；预设改为 20 / 40 / 80 ms。
  A release is only held back when the press was long **and** moved. It used to be
  "either one", which charged every ordinary click that drifted a few pixels with
  50 ms of the button staying down - the window kept following the cursor and the
  text kept selecting. The default hold is now 20 ms.
- **测试** —— 五处断言按新判据更新（长按不动、短按移动都不再算拖拽），并新增
  "长按且移动才算拖拽"的用例。这是**有意的行为变更**，不是把测试改到通过。

### 说明 / Notes

- 已装过的机器上保存的仍是旧值（`ReleaseDelay=50`），设置不会被静默改写：
  到 托盘 → 高级 → 释放延迟 选 20（或点 恢复默认）即可。
  Previously saved settings are left alone: pick 20 in the tray menu, or restore
  defaults.

### 验证 / Verified

Debug / Release × x64 / x86 全量构建零告警（含 `/analyze`）；单测 2713 条通过；
`e2e` 与 `diagnose` 各 0 failure。

---

## 2.2.1 — 2026-11-25

### 改进 / Changed

- **点击光环重画** —— 离屏双缓冲（原来直接画在窗口上，每帧都能看到"擦掉再画"的撕裂）、
  60 帧（原来 30）、半径缓出、笔宽由粗变细（原来恒定 3px，所以看起来是"点下去细圈、放大变粗圈"）、
  淡向背景色而不是纯白（高对比度主题下原来会"凭空消失"）。圆心加了一个字：左 / 中 / 右
  （英文 L / M / R），跟着一起淡入淡出 —— 只靠颜色区分按键，对色弱用户等于没有提示。
  The ring is redrawn: off-screen buffer, 60 fps, eased growth, a pen that thins as
  it travels, and a fade into the background. A single letter sits in the middle.
- **日志框重新可以复制** —— 每次写入后强制 `EM_SETSEL 0,0`，正在拖的选区每 100 毫秒被清一次。
- **按钮位置** —— 「版本说明」移到状态行、「判定说明」移到日志表头行，都是普通按钮，与左侧文字同一行、
  只比文字高一点，取代原来那个自绘圆点。
- **测试区字体** —— 自绘控件拿到的是系统默认字体，与周围标签不是一套，改为跟随对话框字体。
- **输入框布局** —— 「取消」右边缘与输入框对齐（原来超出 10 像素）。

### 修复 / Fixed

- **诊断包泄露身份** —— `report.txt` 里原来有 `exe`、`log_dir`、崩溃模块的完整路径，路径里带 Windows
  用户名。现在只写文件名和位置类别。包内说明的隐私段落同步改正。
  The report no longer carries a user name or a full path.
- **同秒导出互相覆盖** —— 文件名只到秒且 `CREATE_ALWAYS`，一秒内导出两次会互相覆盖，现在加数字后缀。
- **诊断包半截** —— 直接写最终文件名，崩在写一半会留下打不开的 zip，还会把保留的旧包挤掉。
  改为先写 `.part`，完整后再原子改名。
- **拦截日志写入** —— 每条都重建目录、查属性、开关文件，200 毫秒内最多 16 条全压在钩子线程上。
  改为整批一次打开。
- **看门狗冷却期仍在累积证据** —— 冷却一结束必然立刻再重装一次钩子。
- **只读日志框吞掉 Tab** —— 多行 EDIT 带 `WS_TABSTOP`，键盘用户按进去出不来。
- **自检报告构建时机** —— 原来在窗口显示前同步装探针钩子、写注册表，改为窗口出来后再说。

### 说明 / Notes

- 版本说明窗口改为「新的在上面」，并补上 2.2.0 与 2.2.1 —— 之前只写到 2.1.4，所以看起来和版本号对不上。

### 验证 / Verified

Debug / Release × x64 / x86 全量构建零告警（含 `/analyze`）；单测 2705 条通过；`e2e` 与 `diagnose`
各 0 failure；光环逐帧截图核对（左/中/右，中英文）；解开诊断包确认 `exe : MouseMend-x64.exe (other)`；
同一秒导出两次得到两个文件。

---

## 2.2.0 — 2026-11-25

### 新增 / Added

- **判定说明** —— 测试日志分组右上角加了一个圆形「!」，点开是判定列每个取值的完整解释：
  通过、通过+补发、程序模拟、推迟松手、四种拦截、此键未防护、滚轮无位移。窗口里以前
  只显示这些词，没有任何地方说明它们的意思。
  The verdict column is now explained: a round "!" beside the log header opens a
  glossary of every value it can show.
- **版本说明** —— 已拦截的误触分组右上角加了「版本说明」，列出 2.0.0 以来每个公开版本
  修了什么，并注明 2.1.0 / 2.1.1 是内部版本、没有发布。
  A "Versions" button lists what each published release fixed.

### 说明 / Notes

两个窗体共用一套模板：正文放在只读编辑框里，长文可以滚动、可以选中复制，跟随界面语言。

### 验证 / Verified

Debug 与 Release × x64 / x86 全量构建零告警（含 `/analyze`）；单测通过；`e2e` 与
`diagnose` 冒烟各 0 failure；两个窗体逐屏截图核对（中英文标题、换行、圆形按钮渲染）。

---

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
