# 方案 v2：自学习阈值 + 上下文过滤（2026-11-26）

> 状态：**四轮评审后定稿，可实施**。v1 被两路独立评审判"不可实施"；v2 又经过三轮复核，
> 共闭合 12 项发现（P0×6、P1×2、P2×2、架构×2）。每一处修正都标注了评审编号，便于复核。
> 实施前必读 §3.0（R1–R5）与 §4.2。

## 0. 目标与边界

| 功能 | 解决的问题 | 明确不做 |
|---|---|---|
| 自学习阈值 | 每个键的抖动间隔因人而异，固定三档预设需要用户自己试 | 不改判定逻辑、不自动改用户显式选过的值、不在钩子里算分位数、不让钩子等待学习线程 |
| 上下文过滤 | 游戏/设计软件里不该去抖 | 不在钩子里 `OpenProcess`/`QueryFullProcessImageName`、不按窗口标题匹配、不吞事件、不自动写规则 |

**v1 被否决的根因**：采样量纲错、排除返回值写反、分位数脆弱、手势边界定义错误。v2 逐条修正。

## 1. 求证事实与契约基线

| # | 事实 | 证据 |
|---|---|---|
| F1 | 阈值判定用 `elapsed_ms`（上一任意事件→本次按下），不是 `press_gap_ms` | `debouncer.c:422-423` 判定；`:499` up 也更新 `previousTime`；`debouncer.h:120-128` 明写两者差异 |
| F2 | 被 `BLOCK_THRESHOLD` 拦下的 press 同时更新了 `elapsed_ms` 和 `press_gap_ms` | `debouncer.c:445-465`，`:450` 更新 pressGap，`:463` 更新 decision |
| F3 | `near_miss` 用 `elapsed_ms`，与 `press_gap_ms` 不同量纲 | `debouncer.c:573-579` |
| F4 | `return 1` = 吞事件，`CallNextHookEx` = 放行 | `main.c:328/332`，`mouse_hook.c:61-72` |
| F5 | `system_down` 只在引擎里更新 | `debouncer.c:548-559` |
| F6 | 无锁 SPSC 模式已有（trace 环） | `trace_log.c:28-33,74-90,139-155` |
| F7 | 生产版没有 `GetForegroundWindow` 轮询，没有 `window_is_ours` | `main.c:1443-1467`；`trace_log.c:61-70` 仅 trace 构建 |
| F8 | `WM_TIMER` 在鼠标持续移动时会被饿死 | `handover` §10 坑 1；契约 6 |
| F9 | `settings.c` 全量硬编码逐键读写，只有 DWORD helper | `settings.c:13-35,38-137,140-173` |
| F10 | 悬浮提示两行、`TRAY_TOOLTIP_MAX_CHARS=128` | `language.c:724-773`、`language.h:181-183` |
| F11 | 体检窗口右列无空位 | `MouseMend.rc:90-123` |
| F12 | 退出顺序：定时器→hang 线程→窗口→落盘→钩子→缓冲→trace 线程 | `main.c:830-919` |
| F13 | trace 只有单点时间，测不了回调耗时 | `trace_log.c:16-26` |
| F14 | 菜单 ID：全局到 `WM_USER+80`，阈值 `+1000` / 自定义 `+2000` | `menu_ids.h:32-69` |
| F15 | `AddButtonSubmenu` 只收 `DebounceManager` | `context_menu.c:38-40,134-135,373` |

### 契约（handover §3，六条）逐条对照

| 契约 | v2 是否遵守 |
|---|---|
| 1 不替用户按下 | 是，无任何注入 |
| 2 系统按着时不吞松手 | **是**（R4：旁路决策下沉到引擎内部，`isBlocked` / `system_down` 按实际放行结果更新） |
| 3 能推迟↔定时器 | 是，不碰 |
| 4 默认不推迟 | 是，不碰 |
| 5 钩子 O(1) 内存操作 | 是（修正 P2-9：不新增 `GetTickCount64`，用 `event.timestamp`） |
| 6 不用 WM_TIMER 做精确延迟 | 是（修正 P2-8：上下文轮询改专用后台线程，不挂 `TIMER_STATE`） |

## 2. 修正记录（v1 → v2）

| 评审发现 | v1 错在哪 | v2 怎么改 |
|---|---|---|
| **P0-1** | 写"排除命中 `return 1` = 放行" | 明确 `return 1` 是吞事件；**废弃"回调最前面旁路"**。最终做法见 R4：排除决策作为参数进引擎，引擎产出真实 `blocked`，回调按 `blocked` 决定 `return 1` 或 `CallNextHookEx`。**绝不吞事件** |
| **P0-2** | 用 `press_gap_ms` 建分布 | 改为采集 **`decision.elapsed_ms`**，且只在 `reason == BLOCK_THRESHOLD` 时采；与阈值判定完全同源。`press_gap_ms` 不参与学习 |
| **P0-3** | 8 样本 95 分位 = max，被离群点主导 | 置信门槛提到 **32**；分位数改用 **P80**（`ceil(0.8*32)=26`，单一离群点无法主导）；离群点先按 `min(gap, 4 * 当前阈值)` 截尾；再受 ±50% 相对限幅与全局 1..200 钳制 |
| **P1-4** | 早退绕过 `system_down` | **废弃本条**。由三轮 R4 取代：旁路决策下沉到引擎内部，所有状态按实际放行结果更新 |
| **P1-5** | 以为只加 setter 就落盘 | 强制清单：`Preferences` 加字段 + `settings_load` + `settings_save` + `app_controller` setter + e2e 恢复默认断言。排除规则用**单个编码 REG_SZ**（`name1;name2;...`），复用 `settings.c` 新加的 `read_string`/`write_string` helper |
| **P1-6** | "5% 钩子耗时"无法测 | 先给 trace 记录加进入/退出双 QPC + `duration_us` 字段，明确基线 = 未开启两功能时同一版 trace 版 1000 次事件的 P50/P99，再对比开启后 |
| **P2-7** | 生产版没有 `window_is_ours` | 在专用轮询线程里用 `GetWindowThreadProcessId` 拿 PID，与 `GetCurrentProcessId()` 比较，写进缓存槽的 `ours` 标志；钩子不判断 |
| **P2-8** | 轮询挂 `TIMER_STATE` 会被饿死 | 上下文轮询放进**专用后台线程**（与学习线程分开），2 秒一次 `WaitForSingleObject`，不受消息队列影响 |
| **P2-9** | 新增第二次 `GetTickCount64` | 暂停判断直接用 `event->timestamp`（钩子层已取过一次，传进回调） |
| 架构#2 | release 手势边界对双击/拖拽/滚轮都不成立 | "手势结束"改为**空闲超时**：钩子侧每次 press 写 `InterlockedIncrement64(&last_activity_seq)`；学习线程用 `event.timestamp` 的单调性 + 300 ms 空闲窗口判定手势结束，不依赖 release 事件。滚轮无 release 也能工作 |
| 架构#3 | `current_slot` 竞态导致错配 | 缓存槽**永不原地改**：双缓冲 `slots[2]`，UI 写备用槽 → `InterlockedExchange(&current, next)` 原子发布；钩子读一次索引后只读该槽 |
| 架构#4 | 两功能共用通道耦合 | **拆开**：学习 = 直方图计数器 + 学习线程；上下文 = 双缓冲快照 + 轮询线程。只共用"事件时间戳"与 `Interlocked` 模式，不共用线程/队列 |
| 架构#6 | 菜单信息行"天然拿到建议"不成立 | 明确要改 `AddButtonSubmenu` / `context_menu_create` / `context_menu_show` 三个签名 + `main.c` 调用点；新 ID 放 `WM_USER+81..+999`（不是 +100 段）；**仅一个全局"采用建议"入口**，不为每键加 ID，减少 UI 与 e2e 冲击 |

## 3. 模块设计

### 3.0 二轮复核新增的三条硬约束（P0，先于一切实现）

| # | 二轮发现 | 强制解法 |
|---|---|---|
| R1 | "空闲 300 ms"没有时间戳来源，学习线程无法判定手势结束 | `ButtonSignal` 增加 `volatile LONG64 last_activity_tick`；钩子在**每个** press（`reason != IGNORE_*`）写 `InterlockedExchange64(&last, (LONG64)event.timestamp)`。学习线程用自己的 `GetTickCount64()` 判 `now - last >= 300`；轮询周期 100 ms。序号 `gesture_seq` 删除，不参与设计 |
| R2 | 旁路留下陈旧 `system_down` / `isBlocked`，后续 UP 可能被吞（违反契约 2） | **废弃本条**。R2 的"回调后改局部 blocked"被三轮 R4 否决；最终做法只有 R4 的"引擎内部 suppression = 完整 PASS 语义" |
| R3 | `overflow` 桶把 65 和 190 混一起，与 `min(gap, 4*current)` 截尾冲突 | 直方图固定 1..64 ms；`gap > 64` 直接把该样本记到桶 64（饱和），**取消 `overflow` 字段**。截尾规则改为 `bucket = clamp(gap, 1, 64)`，不再出现"4×当前阈值"这种与桶数不一致的规则；限幅仍按 ±50% 与 1..200 |
| R4（三轮，四轮补全） | R2 只在回调后把局部 `blocked=false`，引擎里 `isBlocked` / `system_down` 已按"拦截"写过 → 后续 UP 仍可能被吞 | **旁路决策下沉到引擎内部，且 suppression = 完整 PASS 语义**。给引擎加只读布尔 `suppress_block`（钩子侧从 `slot->excluded` / 暂停状态算）。引擎初始化后若 `suppress_block`，立即走统一放行路径：`reason = DEBOUNCE_REASON_PASS`、`should_block = false`、**不写 `isBlocked` / `blockedTime` / `blocks` / `deferArmedUntil` / `isDeferringRelease` / `deferred`**，但仍照常更新 `previousTime` / `lastDownTime` / `downPoint` / `pressGapMs` / `system_down`（与真实放行一致）。五类拦截点全部覆盖：wheel `:388`、DEFERRING `:405`、THRESHOLD `:428`、RELEASE `:494`、**DEFER_RELEASE `:537`（四轮补）**。不是"先拦再翻返回值"，而是从一开始就不进入拦截分支。API 兼容：保留 `debounce_process_event_ex(manager,event,decision)` 为 `suppress=false` 的 wrapper（25 处测试不改），新增 `debounce_process_event_ex_suppressed(..., bool suppress_block)` 供 `main.c:249` 调用 |
| R5（三轮） | `PauseUntil` 用 DWORD 存 64 位 tick，49.7 天回绕、重启后语义错误 | 暂停用 **`REG_SZ` 存绝对本地时间**或简化为**内存态 `volatile LONG64 pause_until`（不持久化）**；选后者：暂停 5 分钟是临时动作，退出即失效，符合直觉。`GetTickCount64()` 值只在进程内比较，无回绕/重启问题。持久化键 `PauseUntil` 取消 |

> R1 统一字段：所有时间戳用 `uint64_t` / `LONG64` 的 `GetTickCount64()` 值（ms），来源只有 `event.timestamp`（钩子）或 `GetTickCount64()`（学习线程）。`InterlockedExchange64` 是发布方式。

### 3.1 `src/core/signal_ring.c`（钩子写，学习线程读）

```c
#define LEARNING_BUCKET_COUNT 64   // 1..64 ms，1 ms 一桶；更大值饱和进 64
typedef struct {
    volatile LONG buckets[LEARNING_BUCKET_COUNT];
    volatile LONG samples;             // BLOCK_THRESHOLD 样本数
    volatile LONG64 last_activity_tick; // event.timestamp，主线程用
} ButtonSignal;
```

钩子侧：
- **每个 press**（`reason != IGNORE_*`）写 `InterlockedExchange64(&last_activity_tick, (LONG64)event.timestamp)`。
- `reason == BLOCK_THRESHOLD` 时：`gap = clamp(decision.elapsed_ms, 1, 64)`，`InterlockedIncrement(&samples)` 与 `&buckets[gap-1]`。
- O(1)、无锁、无分配、无 API 调用。关闭学习时整个分支不执行。

`last_activity_tick` 是 R1 的时间戳来源；学习线程 100 ms 轮询，`now - last >= 300` 才判定手势结束并重算。

### 3.2 `src/core/learning.c`（纯逻辑，可单测）

- 样本门槛 **32**；不足则 `suggested_ms = 0`（不建议）。
- 离群点处理：桶 64 已饱和吸收所有 ≥64 ms 样本，不再另做截尾（R3）。
- 统计量 **P80**：累计 ≥ `ceil(0.8 * samples)` 的最小上界 + 1 ms。
- 限幅：`[max(1, current*0.5), min(200, current*1.5)]`，且全局 `THRESHOLD_MIN/MAX_VALUE`。
- 空闲 300 ms 判定手势结束后才重算；发布用双缓冲 `LearningAdvice advice[2]` + `InterlockedExchange(&active, next)`。
- UI 拿到的永远是完整一份。

### 3.3 `src/core/context_filter.c`（纯逻辑 + UI 线程轮询）

```c
typedef struct {
    LONG  pid;        // 0 = 未知
    LONG  excluded;   // 0/1
    LONG  ours;       // 是否本进程窗口
    wchar_t name[64]; // 仅轮询线程写
} ContextSlot;

typedef struct {
    ContextSlot slots[2];              // 双缓冲，不原地改已发布槽
    volatile LONG current;             // 0/1，原子发布
    volatile LONG64 pause_until;       // 临时暂停，用 event.timestamp 比较
} ContextFilter;
```

- **专用轮询线程**（2 s，`BELOW_NORMAL`，事件 + 3 s 等待停止）：`GetForegroundWindow` → `GetWindowThreadProcessId` → PID 变化才查规则；缓存 miss 时 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW`；取最后一段比规则。
- 失败时**保持上一次发布**（不写 PID 0，不改 excluded）。
- 本进程窗口 `ours=1` 强制 `excluded=0`。
- 钩子侧（R4）：在调用引擎**之前**只读一个 `LONG`：`slot = &slots[InterlockedCompareExchange(&current, -1, -1)]`；`suppress_block = (slot->excluded || event.timestamp < pause_until)`。把 `suppress_block` 传给 `debounce_process_event_ex_suppressed()`；引擎从一开始走完整 PASS 路径，所有状态按真实放行更新。回调拿到的 `blocked` 自洽，不做事后改写。
- 当 `suppress_block == true`，钩子侧**不采集学习样本**（避免污染分布），但仍照常走 `event_trace` / `event_history` 记录，方便自测版确认"排除生效且事件已放行"。
- 槽发布顺序固定：轮询线程先写备用槽全部字段 → `MemoryBarrier()` → `InterlockedExchange(&current, next)`；已发布槽在下一次切换前不得改写。

### 3.4 UI 与设置

- 新设置键：`LearningEnabled`（DWORD，默认 0 = opt-in）、`ExcludedExes`（REG_SZ 编码字符串，默认空）。**`PauseUntil` 不持久化**（R5：内存态 `volatile LONG64`，退出即失效）。
- 两处持久化同改：`settings.h` / `settings_load` / `settings_save`；`app_controller` 加 setter；e2e 恢复默认断言补两个键。
- 托盘菜单：一个全局「**采用全部学习建议**」项 + 一个「暂停 5 分钟」项 + 一个「排除当前程序」开关项；全部放 ID `WM_USER+81..`，不动既有 ID。名称明确"全部"，消除二轮复核 #4 的歧义。
- 菜单信息行（ID 0）显示：`学习：左 NN / 右 NN / 中 NN ms（各 N 样本）` / `当前程序：已排除`；用现有 inert 标签行模式，不用 `MF_GRAYED`。不逐键加 ID，避免 UI 与 e2e 爆炸。
- **悬浮提示保持两行**，不改；不新增报告键（避免 `diagnose.ps1` 白名单漏检）。

### 3.5 线程与退出

- 两个新线程：`learning_thread`（默认不启动，`LearningEnabled` 打开才启）与 `context_thread`（常驻，但规则空时几乎不做事）。
- 停止位置（修正 F12）：`KillTimer` 后、`hang_watchdog_stop()` **之前**依次停两个新线程，各 3 s 等待；此时 hang watchdog 仍在监控覆盖内。
- 学习线程停时钩子可能还在写直方图 —— 环形计数器是静态的，停线程只是不再读，安全。

## 4. 实施与自审计划

### 4.1 实施顺序（小步可回退）

1. 地基：先只加 `signal_ring.c` + 单测（不接线），跑四配置 + 单测。
2. 学习：`learning.c` + 单测 + 设置键 + 菜单显示；**默认关闭**，不应用。
3. 上下文：`context_filter.c` + 单测 + 轮询线程 + 旁路；规则默认空。
4. trace 加 duration 字段，实测钩子开销。阈值：P50 ≤ 基线 5% **且** P99 绝对增量 ≤ 50 µs。不达标时**只禁用上下文过滤的钩子侧读取**（保留研究与设置，用编译期开关 `CONTEXT_FILTER_HOOK_READ` 关掉），学习功能独立保留；不需要删除整套设置/菜单/线程。
5. 文档三处 + 版本号（另议）。

### 4.2 必须注意

- 绝不在钩子里 `OpenProcess` / `QueryFullProcessImageName` / `GetTickCount64`（新增）/ `SendInput` / 锁 / 分配。
- **不存在回调层旁路**：`main.c` 只有 `if (blocked) return 1; else CallNextHookEx;`，"不拦"通过引擎的 `suppress_block` 实现（R4）。
- **suppress 不看 `system_down`**：排除/暂停期间同样必须完整走引擎的公共写入路径，只是不拦截（终检 P0-A）。
- **suppress 实现为五处拦截点门控**，不是函数开头早退（终检 P0-B）：只在 wheel `:388`、DEFERRING `:405`、THRESHOLD `:428`、RELEASE `:494`、DEFER_RELEASE `:537` 的判定条件上加 `!suppress_block`，并圈住各自的 `blocks/isBlocked/blockedTime/deferArmedUntil/isDeferringRelease/deferStartTime/deferUpPoint` 写入；其余公共写入（`wheelDirection`、rhythm、`downTime`、`previousTime`、`downPoint`、`system_down`、`near_miss`、`decision`）一律照常执行。
- 已发布槽永不原地改。
- 新设置键必须三处持久化 + e2e 断言；不能只加 setter。

### 4.3 自审清单

1. 四配置 Rebuild 含 `/analyze` 0 error 0 warning；单测 x64/x86 全绿。
2. 新单测覆盖：量纲一致（用 `elapsed_ms`）、32 样本门槛、P80 截尾、±50% 限幅、空闲手势边界、双缓冲发布、PID 0、ours 强制、暂停用事件时间戳、system_down 优先、规则空表。
3. `audit_printf.py` 0 问题；语言双表齐全。
4. 磁盘/vcxproj/filters 三方一致（顺手补 `trace_log` 缺口）。
5. `e2e` / `diagnose` / `stuck_key` 全绿，既有断言数值未改。
6. trace 双 QPC 实测：开启两功能后 P50/P99 钩子耗时相对基线 ≤ 5%。
7. 正式版不含 trace 字符串。
8. 真实鼠标验收：开启学习→建议可见→手动采用→双击正常；排除当前程序→该程序不再拦截→切回来恢复；系统按着时切程序→不出现粘键。

## 5. 明确的风险

- 专用后台线程 2 s 轮询有最长 2 s 的生效延迟，用户需知道；菜单里显示"最多 2 秒生效"。
- 学习只给建议、不自动应用，价值依赖用户手动点一次；这是为安全付出的代价，不打算在 v1 自动应用。
- 排除规则用 REG_SZ 编码，用户无法直接编辑；第一版只提供"排除当前程序"按钮。
