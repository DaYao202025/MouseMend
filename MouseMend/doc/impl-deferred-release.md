# 实施清单：把延迟松手的注入移出钩子回调

配套设计：`doc/design-deferred-release.md`（v2，§9.7 已实测载体可行，§10 为第一原则）
状态：**清单已定稿，自评见 §7；其中「重放按下」一项已于 2.3.0 删除（见下）**
日期：2026-11-25

> **2026-11-26 更新（2.3.0）：重放按下已删除。** 实机日志证明它是"按住不放"的
> 来源：扣留期间到来的按下是开关抖动，重放 `UP → DOWN` 等于替用户再按一次，
> 抖动若没有对应松开就永久按下。现在按下直接拦下、扣留保留、只补发一个 UP；
> `deferred_release_take` 已删除，`debounce_service_replay` 等保留但不可达。
> 另外扣留默认关闭（`Release Delay = 0`）。

---

## 1. 目标（可验证的验收标准）

| # | 验收标准 | 怎么验证 |
| :-- | :--- | :--- |
| A1 | **钩子回调栈内不再出现任何 `SendInput`** | 结构守门（§4.5）+ 代码审查 + 单测断言 |
| A2 | 智能拖拽行为对用户不变：拖动后松手被推迟，随后快速按下仍算一次新点击 | 单测覆盖判定字段 + 冒烟 |
| A3 | 任何路径都不会让按钮在系统里永久处于按下 | 单测覆盖"推迟→按下→松手"的全部时序 |
| A4 | 定时器不可用时**不推迟**，而不是推迟后补不回来 | 单测：`defer_available=false` 时不置 `isDeferringRelease` |
| A5 | 四配置零告警、全量单测通过、冒烟通过 | 构建脚本 |

---

## 2. 关键前提（已确认，不要再重新推导）

1. **钩子回调跑在 UI 线程上**（`mouse_hook.c:83` 由 UI 线程安装）。
2. **钩子与 `WM_TIMER` 处理在同一线程** → 两者**不会并发**。这使 v1 设计里那套跨线程握手变得不必要。
3. `SendInput` 从 UI 线程的定时器处理里调用**已被现有代码验证数月**（推迟松手的 UP 一直这么发）。
4. **模态循环期间 `WM_TIMER` 照常派发**（§9.7 实测，`TrackPopupMenu` 内 95/96 跳，`SendInput` 95/95 成功）。

---

## 3. 数据结构改动（`debouncer.h`）

`ButtonDebounceData` 追加（放在现有字段之后，避免影响既有初始化顺序）：

```c
bool pending_replay;        // 已吞下一次按下，等待定时器重放
bool replay_in_flight;      // 重放正在执行（仅同线程，作防御用）
bool release_after_replay;  // 重放执行期间收到真实 UP（防御用）
uint64_t replay_press_time; // 被吞下那次按下的时间戳（诊断用）
POINT replay_point;         // 被吞下那次按下的屏幕坐标
uint32_t replay_down_flags;
uint32_t replay_down_data;
```

`DebounceManager` 追加：

```c
bool defer_available;  // 定时器可用；false 时永不推迟
```

`DebounceDecision` 追加（**末尾**，不参与任何按偏移序列化）：

```c
bool replay;  // 这次按下被吞下并将在定时器里重放（不是"拦下误触"）
```

新增 API：

```c
void debounce_set_defer_available(DebounceManager *manager, bool available);
bool debounce_has_pending_work(DebounceManager *manager);   // 推迟中 或 待重放
void debounce_service_replay(DebounceManager *manager);     // 定时器里调用，执行重放
void debounce_drop_replays(DebounceManager *manager);       // 退出时丢掉未执行的重放
```

---

## 4. 代码改动逐处

### 4.1 `debouncer.c`：抽出按键映射

把 `debouncer.c:93-115` 的 switch 抽成两个函数，供 UP 与 DOWN 共用：

```c
static bool button_up_flags(int button, DWORD *flags, DWORD *data);
static bool button_down_flags(int button, DWORD *flags, DWORD *data);
```

两者都对滚轮返回 `false`（滚轮不参与推迟）。**调用方必须检查返回值**——这是 v1 §4.3 漏掉的一条。

### 4.2 `debouncer.c`：注入收敛到一个函数（结构守门）

```c
static bool inject_mouse(DWORD flags, DWORD data);        // 单事件
static bool inject_mouse_pair(const INPUT *inputs, int count);  // 有序多事件
```

两者开头：

```c
// The hook callback is not allowed to inject: the injected event has to travel
// the hook chain again, and the chain head would be the thread that is already
// waiting inside SendInput. Refusing is deliberate - a lost release costs one
// click, a deadlock costs a reboot.
if (mouse_hook_in_callback())
{
    InterlockedIncrement(&g_injection_refused_in_hook);
    return false;
}
```

`mouse_hook.{c,h}` 新增：

```c
static volatile LONG g_callback_depth;
bool mouse_hook_in_callback(void);   // depth != 0
```

`LowLevelMouseProc` 里**两个返回点都要减**（`return result;` 与末尾 `CallNextHookEx`）。

### 4.3 `debouncer.c`：把"清状态"和"注入"拆开

- 新增 `static bool deferred_release_take(DebounceManager *, int button, DWORD *flags, DWORD *data)`：**只改状态**（清 `isDeferringRelease`），不注入。供钩子路径用。
- `release_deferred_button()` 保持不变，但内部改用 `button_up_flags()` + `inject_mouse()`。供**钩子外的五个调用点**用（§4.6 列出）。

**不写 `previousTime`**：`debouncer.c:91` 那次 `previousTime = GetTickCount64()` 删除。理由：钩子路径随后会被 `debouncer.c:324` 用事件时间戳覆盖；保留两处写入只会让基准漂移（v1 自评已发现）。

### 4.4 `debouncer.c`：`debounce_process_event_ex` 的钩子路径

替换 `debouncer.c:207-209`：

```c
bool replay_pending = false;
if (!event->is_injected && event->is_down)
    replay_pending = deferred_release_take(manager, event->button, &up_flags, &up_data);
```

拿到 `replay_pending` 后（此时仍不在临界区内，与现状一致）：

```c
if (replay_pending)
{
    EnterCriticalSection(&manager->cs);
    ButtonDebounceData *d = &manager->buttons[event->button];
    d->pending_replay = true;
    d->replay_in_flight = false;
    d->release_after_replay = false;
    d->replay_press_time = event->timestamp;
    d->replay_point = (POINT){ event->x, event->y };
    d->replay_down_flags = down_flags;
    d->replay_down_data = down_data;
    LeaveCriticalSection(&manager->cs);
}
```

后续判定逻辑**不动**：`flushed_on_press = replay_pending` 仍使这条按下豁免 bounce 判定（`debouncer.c:307`）、走 `PASS_FLUSHED`（`debouncer.c:319`）。所以 `data->blocks` 不增（与今天一致）。

**取消规则**：在按钮抬起分支（`debouncer.c:352` 之后、判定之前）加：

```c
// The user let go before the replay ran: the release is already on its way to
// the system, so replaying a press now would leave the button logically held.
if (data->pending_replay && !data->replay_in_flight)
    data->pending_replay = false;
```

**拒绝推迟**：`debouncer.c:376` 的条件加 `manager->defer_available`：

```c
if (!should_block && manager->use_hybrid_heuristic && manager->defer_available)
```

**返回值**：`_ex` 返回 `should_block || replay_pending`（语义 = "这次事件要不要吞下"）；`decision->blocked = should_block`（不变），`decision->replay = replay_pending`。

### 4.5 `debouncer.c`：新增三个函数

**`debounce_service_replay`**（只在 `WM_TIMER` 里调用）：

1. 临界区内找出 `pending_replay && !replay_in_flight` 的按键，拷贝参数，置 `replay_in_flight = true`，**释放锁**
2. 锁外：`SendInput` 两条 —— `UP`，然后 `DOWN`（`MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE`，坐标用 `replay_point` 归一化到虚拟屏幕 0..65535）
3. 再进临界区：若 `release_after_replay` → 再注入一次 `UP`；置 `pending_replay = false`，清 `replay_in_flight`

**为什么 DOWN 要带坐标**：`downPoint` 记的是被吞下那次按下的位置，若注入的 DOWN 落在别处，下次松手的 `dist_sq`（`debouncer.c:379-381`）会偏大，可能把一次普通点击误判成拖拽而再次推迟。

**`debounce_has_pending_work`**：任一按键 `isDeferringRelease || pending_replay`。

**`debounce_drop_replays`**：清掉所有 `pending_replay`（退出专用）。

### 4.6 `main.c`

| 位置 | 改动 |
| :--- | :--- |
| `main.c:196-211` `EnsureDeferredReleaseTimer` | **函数删除**，换 `EnsureSmartDragTimer()`：定时器未武装就 `SetTimer`；失败则 `debounce_set_defer_available(false)` 并记一次诊断。**只在物理按下时调用**，因此钩内永不再有 SendInput |
| `main.c:261` | `EnsureDeferredReleaseTimer()` → 仅当 `event->is_down && !event->is_injected` 时调用 `EnsureSmartDragTimer()` |
| `main.c:263` `if (blocked)` | 内层加 `if (!app->decision.replay)`，让重放不写拦截日志、不弹气泡、不更新证据 |
| `main.c:1343` `WM_TIMER` | 加 `debounce_service_replay()`；结束条件改用 `debounce_has_pending_work()` |
| `main.c:813` 退出 | 在 flush 之前加 `debounce_drop_replays()`；`debounce_flush_deferred_releases()` 保持（同步注入，此时不在钩内） |

**退出顺序保持不变**：停定时器 → 丢重放 → flush 推迟（同步 UP）→ 移除托盘 → 拔钩子 → cleanup。因为不再有线程，v1 里那套"线程先死还是临界区先死"的问题不存在。

### 4.7 钩子外仍然同步注入的五个点（**都不要改**）

`debounce_cleanup`(52)、`debounce_set_hybrid_heuristic`(456)、`debounce_set_monitored`(516)、`debounce_set_all_monitored`(543)、`debounce_reset_statistics`(698)、`debounce_check_deferred_releases`(755) / `flush`(765)。

**共 8 个调用点**（v1 写 6 个是错的）。全部在消息循环里执行，不在钩子栈上。**新增任何调用点都必须先证明它不在钩子栈上，并把这条证明写成代码注释。**

---

## 5. 测试清单

**单测（`tests/test_core.c` / `test_smart_drag.c`）**

| # | 断言 | 说明 |
| :-- | :--- | :--- |
| T1 | 无推迟时 `deferred_release_take` 返回 false 且不改状态 | 基线 |
| T2 | 推迟中收到按下 → 返回 `replay=true`、`blocked=false`、`reason=PASS_FLUSHED`、`blocks` 不变、`pending_replay=true` | 本次核心 |
| T3 | 推迟中收到按下 → 再收到真实 UP → `pending_replay` 变 false（取消） | 防卡键 |
| T4 | `defer_available=false` 时永不置 `isDeferringRelease` | A4 |
| T5 | `debounce_has_pending_work`：推迟中/待重放都为 true，都不为 false | 定时器不会被提前关掉 |
| T6 | `button_up_flags` / `button_down_flags`：7 个按键全覆盖，X1/X2 的 `mouseData` 正确，滚轮返回 false | 防映射漂移 |
| T7 | `previousTime` 在"推迟→按下"之后等于按下的事件时间戳（不是 `GetTickCount64`） | 防基准漂移 |
| T8 | **既有测试 `test_smart_drag.c:114-117` 的期望需要更新**：那条路径的返回值由 false 变 true（事件现在被吞下并重放），必须显式说明这是有意的行为变更 | 回归风险 |

**冒烟**：`tests/smoke/e2e.ps1` / `diagnose.ps1` 必须仍然 0 failure。

**已知局限**：现有冒烟用 `SendInput` 造输入，而产品忽略注入事件（`plan.md:287-290`），所以**冒烟对本次改动几乎没有覆盖**。真正的覆盖靠 T2/T3 这类状态机单测。

---

## 6. 不动的东西（明确列出，避免顺手改）

- 去抖判据、阈值、拖拽距离、推迟时长
- `block_log` / 通知 / 证据的判定口径
- `hook_watchdog` 的检测逻辑
- 钩子外那 8 个注入调用点
- 语言资源（重放沿用 `PASS_FLUSHED` 的"通过+补发"文案，对用户语义不变）

---

## 7. 自评：这份清单有没有漏

### 7.1 已确认覆盖的失败路径

| 情形 | 行为 | 是否卡键 |
| :--- | :--- | :--- |
| 定时器起不来 | 永不推迟，UP 原样放行 | 否 |
| 重放排队后又收到真实 UP | 取消重放，UP 放行 | 否 |
| 重放执行期间收到真实 UP | 吞下并在重放后补一个 UP（防御分支） | 否 |
| 重放时 `SendInput` 失败 | 丢弃并计数，`pending_replay` 清掉 | 是（按钮卡住，但下次点击自愈） |
| 程序退出时仍有待重放 | `debounce_drop_replays` + flush 同步补 UP | 否 |
| 关闭监控 / 清零统计 / 关混合模式 | 走原有同步 flush 路径，不受重放影响 | 否 |
| 钩子重装（`RecoverHook`） | 待办：重装后应清掉 `pending_replay` | 是（风险低） |

### 7.2 自评发现的问题（已在清单里修掉）

1. **退出时若存在待重放，系统会停在按下状态** → 加了 `debounce_drop_replays` + 退出前补 UP。v1 完全没提这一点。
2. **`MOUSEEVENTF_MOVE` 的归一化基准**：`MSLLHOOKSTRUCT.pt` 是虚拟屏幕坐标，归一化必须用 `SM_XVIRTUALSCREEN/SM_CXVIRTUALSCREEN`，不是 0..width。清单已写明。
3. **`EnsureDeferredReleaseTimer` 删除后，谁负责关定时器**：改为由 `WM_TIMER` 用 `debounce_has_pending_work` 判定。清单已写明，否则定时器会被提前关掉，重放永不执行。
4. **`decision.replay` 与 `blocked` 的关系**：明确 `blocked` 仍表示"拦下误触"（用于计数/日志），`_ex` 返回值表示"是否吞下"。两者不能混用，否则拦截数会虚高。

### 7.3 仍未解决（**已知、接受、不在本次范围**）

1. `hook_watchdog` 在钩子被 Windows 摘掉后会重装钩子（`main.c:915`），重装时未清 `pending_replay`——概率极低（重放存活期 < 20 ms）。**记为待办**。
2. 重放时 `SendInput` 失败会导致按钮卡到下次点击。这是"能放弃就别等"的代价，按 §10 的原则**接受**，只记账不重试。
3. 冒烟对本次改动覆盖很低（见 §5 末）。
4. 挂起看门狗（§9.8 第 1 条）不在本次范围，作为下一步。

### 7.4 与验收标准对照

| 标准 | 是否达成 | 依据 |
| :--- | :--- | :--- |
| A1 钩内无 SendInput | 是 | 唯一钩内注入点改为 `deferred_release_take`（纯状态）；`EnsureDeferredReleaseTimer` 已删；结构守门兜底 |
| A2 行为不变 | 是 | 判定字段逐项对照（`blocks`/`previousTime`/`downTime`/`reason` 均不变），仅"按下由放行改为吞下后重放" |
| A3 不卡键 | 是（除 `SendInput` 失败这一已知例外） | §7.1 逐条 |
| A4 定时器不可用不推迟 | 是 | `defer_available` |
| A5 构建与测试 | 待实施后验证 | — |

---

## 8. 实施记录（2026-11-25 已完成）

按本清单实施，改动文件：`src/core/mouse_hook.{c,h}`、`src/core/debouncer.{c,h}`、`main.c`、`tests/test_smart_drag.c`、`tests/test_core.c`。

**实施过程中与清单不同的四处**（都是动手时才发现的问题）：

1. **`debounce_drop_replays` 必须顺带把松手还回去。**
   清单原写"退出时丢掉未执行的重放"。实际推演发现：`deferred_release_take` 已经把 `isDeferringRelease` 清掉了，所以退出时 `flush_deferred_releases` 根本不会为它发 UP —— 进程退出后按钮会**永久停在按下状态**。改为：丢重放，但为每个待重放按键注入一次对应的 UP。

2. **先算按下标志，再取走推迟。**
   清单的顺序是"先 take 再算 flags"。若 `button_down_flags` 失败，UP 已经被取走却没人补发 → 丢一次松手。改为先算 flags，两个条件都满足才 take。

3. **`EnsureSmartDragTimer` 需要两个前置守卫。**
   没有 `debounce_ready` 时不能碰引擎；没有 `hWnd` 时 `SetTimer(NULL,...)` 会创建线程定时器，而线程定时器的 `WM_TIMER` 没有窗口接收 → 释放永远回不去。两处都加了判断。

4. **两处既有断言是有意改掉的**，不是"修到过"：
   - `test_smart_drag.c:115`：推迟中收到的按下，返回值由 `false`（放行）改为 `true`（吞下并重放）
   - `test_core.c:935`：同上
   两处都补了 `decision.replay` / `pending_replay` / `has_pending_work` 的断言，并写明这是刻意的语义变更。

**验证结果**：Debug/Release × x64/x86 四配置 **0 错误 0 告警**（含 `/analyze`）；单测 **2594 checks / 0 failures**（原 2578，+16）；`e2e.ps1` 与 `diagnose.ps1` 均 **0 failure**。

**仍未验证的部分（诚实记录）**：重放的**实际注入**没有被自动化覆盖。产品忽略 `is_injected` 事件（`debouncer.c:235`），所以用 `SendInput` 造输入驱动不了这条路径；单测只覆盖到"状态机把重放排好队"，注入本身只能靠真实鼠标的拖动操作验证。这是 §5 末已记录的老问题。
