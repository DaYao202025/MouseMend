# MouseMend

<div align="center">

![Version](https://img.shields.io/badge/version-2.4.1-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey.svg)
![Language](https://img.shields.io/badge/language-C-informational.svg)
![Toolset](https://img.shields.io/badge/toolset-MSVC%20v145-purple.svg)
![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)

<br>

**Stop False Clicks — Mend Your Mouse in Software**<br>
**鼠标误触不用换硬件 —— 让软件把按键修回来**

<br>

[English](#english) · [中文](#中文) · [**Full documentation / 完整文档**](MouseMend/README.md)

</div>

---

## Download / 下载

Latest release / 最新版本：**[v2.4.1](https://github.com/DaYao202025/MouseMend/releases/tag/v2.4.1)**

- `MouseMend-x64.exe` —— 64 位 Windows 用这个 / for 64-bit Windows
- `MouseMend-x86.exe` —— 仅 32 位 Windows / 32-bit only

The executables live on the release page, not in the tree. / 可执行文件只作为 Release 附件，不进代码树。

---

## Screenshots / 界面

<!-- Images are served through jsDelivr rather than by relative path. GitHub
     serves relative README images from raw.githubusercontent.com, which does
     not resolve on networks where that host is blocked, so the pictures were
     showing up as broken alt text for exactly the readers this project is
     published for. jsDelivr serves the same bytes out of the same commit.
     The fastly. host is used rather than the canonical cdn. one because cdn.
     answers every request with a connection reset on that same network, which
     would have put the pictures back to broken alt text. gcore. and testingcf.
     serve identical bytes if fastly. ever goes the same way.
     If the repository is renamed or moved, every URL below has to follow. -->
<table>
  <tr>
    <td align="center" width="50%">
      <img src="https://fastly.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/checkup-zh.png" width="430" alt="鼠标体检窗口（中文）"><br>
      <sub>鼠标体检 · check-up window —— 中文</sub>
    </td>
    <td align="center" width="50%">
      <img src="https://fastly.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/checkup-en.png" width="430" alt="Mouse check-up window (English)"><br>
      <sub>check-up window —— English</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="https://fastly.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/tray-menu-zh.png" width="300" alt="托盘菜单与高级子菜单（中文）"><br>
      <sub>托盘菜单 / 高级子菜单 —— 中文</sub>
    </td>
    <td align="center" width="50%">
      <img src="https://fastly.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/tray-menu-en.png" width="300" alt="Tray menu and Advanced submenu (English)"><br>
      <sub>tray menu / Advanced —— English</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="https://fastly.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/about-zh.png" width="250" alt="关于窗口（中文）"><br>
      <sub>关于窗口 —— 中文</sub>
    </td>
    <td align="center" width="50%">
      <img src="https://fastly.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/about-en.png" width="250" alt="About window (English)"><br>
      <sub>about window —— English</sub>
    </td>
  </tr>
</table>

The check-up window is the main window: it opens on every start, and closing it
puts it back into the tray without losing the session. It is strictly read-only,
so what it shows cannot change what the engine does.

鼠标体检窗口就是主窗体：每次启动都会打开，关闭只是收进托盘、会话不丢。
它严格只读 —— 看到的内容不会反过来改变引擎的行为。

---

## English

A mechanical mouse switch wears out long before the rest of the mouse does. The
contacts start to bounce: one physical press is reported as two, a wheel scroll
jumps back one notch, a drag lets go halfway across the screen.

MouseMend sits in the notification area and filters the mouse event stream in
software, so the worn switch stops mattering. No driver, no kernel component, no
network access, no installation — a single executable.

What it does:

* **Seven axes, tuned separately** — left, right, middle, `X1`, `X2`, vertical
  wheel, horizontal wheel. Each can be enabled, disabled and tuned on its own.
* **Per-button thresholds** — three presets (Game 40 ms / Office 50 ms / Strict
  60 ms) plus a custom value per button.
* **Smart Drag** — only a press that was held long **and** moved counts as a
  drag. Its release is held back briefly and then re-injected, so a bouncing
  switch cannot break the drag; a plain click is released immediately, so nothing
  feels stuck to the pointer.
* **Check-up window** — read-only. Shows every verdict the engine made, click by
  click, plus the *near misses*: presses that passed but only just cleared the
  threshold. "0 blocked, 7 near misses" means the switch is already failing.
* **Quiet by design** — a balloon on block is off by default and rate limited to
  one every 20 seconds; a tray hover shows the state at a glance.

It is **not** a driver and it cannot repair hardware. It makes the *reported*
presses match the *intended* presses.

**→ Read the [full documentation](MouseMend/README.md)** for the decision rules,
the design constraints, the known limits and the build instructions.

## 中文

机械鼠标的微动开关，往往比鼠标本体先坏。触点开始弹跳之后：按一下变成两下、
滚轮往回跳一格、拖拽拖到一半松手。

MouseMend 常驻托盘，在软件层过滤鼠标事件流，让磨损的开关不再影响使用。
没有驱动、没有内核组件、不联网、不用安装，就是一个可执行文件。

主要功能：

* **七个轴单独调** —— 左键、右键、中键、`X1`、`X2`、垂直滚轮、水平滚轮，
  每个都能单独启用、单独调阈值。
* **逐键阈值** —— 三档预设（游戏 40 ms / 办公 50 ms / 严格 60 ms），
  也可以为每个键单独填自定义值。
* **智能拖拽** —— 只有**按得久而且移动过**才算拖拽。真实拖拽的松手会先扣住一小会儿
  再补发，弹跳打断不了拖拽；普通点击一律立即放行，不会有"粘连"感。
* **鼠标体检窗口** —— 只读。逐次点击告诉你引擎怎么判的，还有**临界点击**：
  那些放行了、但只是勉强过线的按下。"拦截 0 次、临界 7 次"说明鼠标已经在坏。
* **不打扰** —— 拦截气泡默认关闭，最快 20 秒一次；鼠标停在托盘图标上就能看状态。

它**不是驱动**，也修不好硬件。它做的是让**系统收到的**按键与**你想按的**按键一致。

**→ 读[完整文档](MouseMend/README.md)**，里面有判定规则、设计约束、已知限制和构建说明。

## Repository layout / 目录结构

```
MouseMend/            the project itself / 项目本体
  src/core/           hook, debounce engine, settings, watchdog
  src/ui/             check-up window, tray, menus, text rendering
  src/utils/          autostart, diagnostics, crash dumps, logging
  tests/              console test suite + PowerShell smoke tests
  doc/                design notes and the release runbook
LICENSE               MIT
sign_release.ps1      optional self-signed code signing helper
```

## Build / 构建

Requires Visual Studio 2022 or newer with the Windows SDK.

```bat
msbuild MouseMend\MouseMend.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
msbuild MouseMend\MouseMend.sln /t:Rebuild /p:Configuration=Release /p:Platform=x86
```

Output: `MouseMend\bin\x64\Release\MouseMend-x64.exe` and
`MouseMend\bin\x86\Release\MouseMend-x86.exe`.

> The solution platform is named `x86`, not `Win32`; `/p:Platform=Win32` fails
> with MSB4126.

## Tests / 测试

```bat
MouseMend\tests\bin\x64\Release\MouseMendTests.exe
MouseMend\tests\bin\Win32\Release\MouseMendTests.exe
```

2432 checks, no window and no message loop required. Two PowerShell smoke suites
live in `MouseMend\tests\smoke`.

## Licence / 许可证

MIT — see [LICENSE](LICENSE).<br>
基于 MIT 许可证发布，见 [LICENSE](LICENSE)。
