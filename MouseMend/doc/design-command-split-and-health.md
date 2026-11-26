# 方案：菜单收窄 + 命令分类 + 学习窄接口 + 能力健康自检

> 目标版本：2.4.0 开发线上继续（未发布）
> 状态：**已实施并验证完毕**。v1 被两路评审否决（13 项）→ v2 逐条修正 → 终审再抓 3 项 → v3 → 实施。
> 流程：求证 → 方案 v1 → 两路评审 → 修方案 v2 → 终审 → 修方案 v3 → 实施 → 自审。
> 实施结果与真机发现记在 `handover-2026-11-26.md` §18。

---

## 0. v1 被评审否决的问题与修正（先看这段）

| # | v1 的错误 | v2 的修正 |
|---|---|---|
| A1 | 菜单 3 列定为「键名 / 当前阈值 / 样本数」，**删掉了建议值** | 用户看这个菜单只问「该不该点采用」，需要的是**建议值**。改为 `键名 / 当前→建议 / 样本数` |
| A2 | `ID 覆盖率测试` 被当作分类器存在的核心理由 | 分类器按区间判断，任何落区间的 ID 都非 `NONE`，**该断言恒绿**。理由改为「收敛区间判定 + 可读性」，验证手段换成下面的 V2/V3 |
| A3 | 实施顺序 3.1 先行 | 菜单的阈值列来自 `debounce`，而 3.4 要删该字段。**改为 3.4 → 3.1 → 3.2/3.3 → 3.5** |
| A4 | `CAP_CONTEXT` 判据 `enabled = mode != ALL` | **语义反了**：`ALL` 是最严模式，不是关闭。且暂停期间会误报健康。改为三态 + `!is_paused` |
| A5 | `CAP_TRAY` 判据 `healthy = enabled` | 同义反复，不携带信息。**删除该能力**，托盘并入「钩子/主体」不单列 |
| A6 | `CAP_LOG` 判据 `log_queue_ready` | 该值启动后永不变化，是「永远绿灯」。改用 `log_queue_ready && log_directory[0] && !log_warning_shown` |
| A7 | `app_capabilities_snapshot` 未说放哪 | 放 `main.c` 则**测试工程链接不到**（不编译 main.c），§5 的自审项是空头支票。改为纯 POD 模块 `src/core/app_capabilities.c` + 入参结构，复用 `app_snapshot.c` 的先例 |
| A8 | 「体检窗口顶部加一行」 | `IDC_STATUS` 底边 y=19、`IDC_GROUP_TEST` 顶边 y=21，**只有 2 个对话框单位**。改为**追加到 `IDC_STATUS` 文本**（384 字符预算有余量），不动 rc |
| A9 | `return handled ? 0 : DefWindowProc(...)` | 不等价于原 `break`。**保留 `break`**，未认领路径与今天逐字节一致 |
| A10 | 「数 if 数量」作为验证方法 | 数不出 `return 0→false` 的写反。改为**机械 diff**（V2）+ **e2e 逐 ID**（V3） |
| A11 | 删 `STR_LEARNING_KEY_FMT`，称「唯一引用在 context_menu.c」 | 实为 **4 处引用**（`language.h` 枚举 + 两张表 + 调用点），且删了会留空槽让 `test_language_tables` 失败。改为**改内容不删槽位** |
| A12 | `unhandled menu id` 只在 `#ifndef NDEBUG` 打印 | e2e 跑 Release，用户也跑 Release —— 恰恰在最需要时是哑的。改为**无条件计数**并进能力状态 |
| A13 | 未提 `learning_service.h` 的 `#include "debouncer.h"` 与测试签名 | 移除后可能丢失 `MOUSE_BUTTON_COUNT` 的传递来源；`test_core.c:2167-2168` 两行**必须改**（v1 承诺「断言一条不改」需更正） |

---

## 1. 目标与约束

| 用户的原话 | 工程含义 |
|---|---|
| 一个功能出问题，不能影响其他功能 | **能力降级**：某模块失效时进程仍可用，且用户能看出哪块坏了 |
| 有些数据可以共用就共用 | **单一真源**：同一份数据只有一个写入者 |
| 做成一块块模块化的软件 | **窄接口**：模块之间只通过少量函数通信 |

界面要求：自学习菜单**最多 3 列，只放重要的**。

**硬约束**：

1. 不改 `debouncer.c` 的任何判定逻辑。
2. 不改变任何用户可见行为（除菜单布局）。
3. 每一步独立可回退。

---

## 2. 求证结论（v2 修正过的数字）

| # | 事实 | 证据 |
|---|---|---|
| F1 | `WM_COMMAND` 占 **305 行** | `main.c:1789` → `2092` |
| F2 | 内部结构是 **25 条 `if (menu_id == X)` + 5 条范围式 `if (menu_id >= X)` + 1 个无 `if` 外壳的裸块**（阈值预设/自定义，内含 2 个解码调用），共 **31 个入口** | `Select-String` 实测 25 / 5 / 2；v1 的「约 30 条」与 v2 的「32+5+1」都不准 |
| F3 | 其中 **3 条是「一个 if 管两个 ID」**：`IDM_CONTEXT_MODE_ALL_WHITE\|ALL_BLACK`、`IDM_EXCLUDE_CURRENT\|ONLY_CURRENT`、`IDM_CONTEXT_ADD_WHITE\|ADD_BLACK` | 切分时最易只搬一半 |
| F4 | `AppState` 定义在 `main.c:137`，实例 `static AppState g_app`；命令处理依赖 18 个 `main.c` 内 `static` 函数 | 但这些函数**依赖密度不均**：7 个只走 `g_app.controller`，5 个重依赖多字段 |
| F5 | `menu_id == 0` 是合法的**惰性信息行 ID**，被 `context_menu.c` 用 9 次 | 分类器不能把 0 当异常 |
| F6 | 体检窗口 `IDC_STATUS` 与下方 groupbox 只有 2 个对话框单位间隙 | `MouseMend.rc:90-96` |
| F7 | 测试工程**不编译 `main.c`** | `tests/MouseMendTests.vcxproj` 只列 `..\src\...` |
| F8 | `e2e.ps1:127-130` 已有 `Send-Command`，用 `PostMessage(WM_COMMAND)` 真实驱动命令 | 现成的分支覆盖手段，v1 完全没用上 |

### 2.1 判断：**不做物理拆分**（结论不变，理由修正）

`g_app` 是文件内单例，搬到新文件只有两条路：改成跨文件全局（耦合变隐蔽）或参数化 18 个函数（改动 1000+ 行）。

但 v1 漏了一个**零成本**的中间步骤：**`Handle*` 的签名写成 `(AppState *app, UINT menu_id)`，函数体仍在 `main.c`、调用处传 `&g_app`**。今天零风险，将来真要搬文件时只需 20 行调用改动。**这一步纳入 v2。**

---

## 3. 设计

### 3.1 菜单收窄（3 列，保留建议值）

```
自学习 ▸
  ☑ 启用自学习
  ──────────────────────
  左键   50→48   1240
  右键   50→50    380
  中键   —        12
  ──────────────────────
  采用全部学习建议
```

三列 = **键名 / 当前→建议 / 样本数**。

- 有建议：`50→48`（用户一眼知道点「采用」会改成 48）
- 无建议：`—`，后面仍显示样本数（用户知道是「12 个样本还不够」还是「0 个，没采到」）
- 学习**关闭**时：整块换成一行 `STR_LEARNING_OFF_HINT`（保持现状，不逐键列）

**文案处理（避开 A11 的空槽陷阱）**：

| ID | 操作 | 内容 |
|---|---|---|
| `STR_LEARNING_KEY_FMT` | **改内容，保留枚举槽位** | `"%s  %u→%u  %u"` / `"%s  %u→%u  %u"` |
| `STR_LEARNING_ROW_PENDING_FMT` | **新增** | `"%s  —  %u"` / 同 |
| `STR_LEARNING_OFF_HINT` | 保留 | 不变 |
| `STR_LEARNING_PROGRESS_FMT` | **删除**（被逐键行取代） | 需同步删 `language.h` 枚举 + 两张表三处 |

**关键实现点（评审 M4）**：现有的 `bool any` 同时控制「是否显示进度行」和「采用按钮是否置灰」。逐键插入 PENDING 行时**绝不能**把 `any` 置 true，否则「采用」从置灰变成可点，点了却什么都不做（`main.c:1995` 会跳过 `suggested_ms == 0`）。

**两行的占位符个数必须与参数个数严格对应**（终审 P0-2 提到的风险，这里写死避免实现时错位）：

```c
/* 有建议：4 个占位符，4 个参数 */
StringCchPrintfW(line, ARRAYSIZE(line), lang_str(STR_LEARNING_KEY_FMT),
                 lang_button_short_name(button), current_ms, suggested_ms, samples);

/* 无建议：2 个占位符，2 个参数。不显示当前阈值——子菜单标题里已经有了 */
StringCchPrintfW(line, ARRAYSIZE(line), lang_str(STR_LEARNING_ROW_PENDING_FMT),
                 lang_button_short_name(button), samples);
```

`STR_LEARNING_ROW_PENDING_FMT` 是 `"%s  —  %u"`，**只有 2 个占位符**，不要传 3 个参数。
`test_language_tables` 只校验两表占位符**签名**一致（个数 + 类型序列），不校验内容，
所以英文表用 `->`、中文表用 `→` 都可以；两表各自内部保持一致即可。

### 3.2 命令分类器（纯逻辑，理由已修正）

`src/core/command_classify.{c,h}`：

```c
typedef enum
{
    COMMAND_GROUP_NONE = 0,   // 只有 menu_id == 0 与真正未知的 ID
    COMMAND_GROUP_MENU,
    COMMAND_GROUP_THRESHOLD,
    COMMAND_GROUP_LIST,
    COMMAND_GROUP_LEARNING
} CommandGroup;

CommandGroup command_classify(UINT menu_id);
```

**它的价值**：把散落在 305 行中段的 5 条区间判定（`IDM_RELEASE_DELAY_BASE + n`、`IDM_DRAG_DISTANCE_BASE + n`、`IDM_TOGGLE_BASE + n`、`IDM_CONTEXT_LIST_*`、阈值预设）收敛到一个可读文件，让四个 `Handle*` 变成平铺的 `if (id == X)`。

**它不解决的问题**（v1 吹错了）：挡不住「分类对了但执行漏写」。那由 V2/V3 保证。

**测试断言要具体到组别**，不是 `!= NONE`：

```c
CHECK(command_classify(IDM_CONTEXT_MODE_ALL_BLACK) == COMMAND_GROUP_LIST);
CHECK(command_classify(IDM_THRESHOLD_PRESET_ID(0, 0)) == COMMAND_GROUP_THRESHOLD);
CHECK(command_classify(IDM_CONTEXT_LIST_BLACK + 15) == COMMAND_GROUP_LIST);
CHECK(command_classify(0) == COMMAND_GROUP_NONE);       // 惰性行
CHECK(command_classify(WM_USER + 4999) == COMMAND_GROUP_NONE);
```

**区间边界必须逐字符对齐**（评审 S3c）：阈值分类若写成 `>= IDM_THRESHOLD_BASE` 而不带上界，全部名单 ID 会落进 THRESHOLD 组，且因解码函数自带上下界而静默失败。

### 3.3 `main.c` 内切成四个函数

```c
static bool HandleMenuCommand(AppState *app, UINT menu_id);
static bool HandleThresholdCommand(AppState *app, UINT menu_id);
static bool HandleListCommand(AppState *app, UINT menu_id);
static bool HandleLearningCommand(AppState *app, UINT menu_id);
```

`WM_COMMAND`：

```c
case WM_COMMAND:
{
    UINT menu_id = LOWORD(wParam);
    bool handled = false;

    switch (command_classify(menu_id))
    {
    case COMMAND_GROUP_MENU:      handled = HandleMenuCommand(&g_app, menu_id);      break;
    case COMMAND_GROUP_THRESHOLD: handled = HandleThresholdCommand(&g_app, menu_id); break;
    case COMMAND_GROUP_LIST:      handled = HandleListCommand(&g_app, menu_id);      break;
    case COMMAND_GROUP_LEARNING:  handled = HandleLearningCommand(&g_app, menu_id);  break;
    default:
        if (menu_id != 0)   // 0 是惰性行，不是错误
            app_capabilities_note_unhandled(&g_app.caps, menu_id);
        break;
    }

    (void)handled;   // 保留 break，未认领路径与今天逐字节一致
    break;
}
```

**两个必须遵守的规则**：

1. **命中即返回 `true`**，包括「认领了但什么也没做」的情况（例如 `if (!snapshot.recent_path[0]) return true;`）。返回 `false` 会让它落到 `DefWindowProc`，属于行为变更。
2. **每个分支内部代码原样搬运**，只把 `return 0` 改成 `return true`，`break` 改成 `return true` 视情况。**特别是 `main.c:1962-1963` 那三行「自动切模式」必须原样搬**，它是 §16.10-16 契约。

### 3.4 学习模块窄接口

```c
typedef uint32_t (*LearningThresholdFn)(int button, void *user);

bool learning_service_start(LearningService *service, SignalRing *ring,
                            LearningThresholdFn threshold_fn, void *user);
```

**必须处理的四件事**（评审 M1）：
1. 线程里 `threshold_fn` 为 `NULL` 时短路（对应今天 `!service->debounce` 的兜底）。
2. 回调**不得加锁**——直接读 `debounce->buttons[i].thresholdMs`（与今天一致）。若实现成调 `debounce_is_any_monitored` 那种加锁接口，学习线程会每 100 ms 抢 7 次全局锁。
3. `learning_service.h` 的 `#include "debouncer.h"` 移除后，`MOUSE_BUTTON_COUNT` 可能失去传递来源 → **保留 `#include "mouse_hook.h"`** 并验证编译。
4. `tests/test_core.c:2167-2168` 两处调用**必须改签名**；`main.c:767` 与 `main.c:1883` 两处也要改。

### 3.5 能力健康自检（纯 POD 模块，三态）

`src/core/app_capabilities.{c,h}`（参照 `app_snapshot.c` 的先例）：

```c
typedef enum
{
    CAP_HOOK = 0,
    CAP_LEARNING,
    CAP_CONTEXT,
    CAP_LOG,
    CAP_COUNT          // 托盘不单列：healthy = enabled 是同义反复
} AppCapability;

typedef enum
{
    CAP_STATE_DISABLED = 0,  // 用户没开
    CAP_STATE_OK,            // 开着且在工作
    CAP_STATE_UNKNOWN,       // 开着但还没观测到有效输出
    CAP_STATE_STALLED        // 曾经工作过，现在停了
} CapabilityState;

typedef struct
{
    bool hook_ready;
    bool hook_installed;
    bool hook_healthy;        // 来自 hook_watchdog_is_healthy()

    bool learning_enabled;
    bool learning_started;
    uint32_t learning_samples;   // 最近一次观测到的样本总数

    bool context_active;         // mode != ALL
    bool context_started;
    bool context_paused;
    bool context_observed;       // 是否成功 publish 过

    bool log_enabled;
    bool log_ready;
    bool log_directory_resolved; // log_directory[0] != L'\0'
} CapabilityInputs;

void app_capabilities_evaluate(const CapabilityInputs *in,
                               CapabilityState out[CAP_COUNT]);
```

**判据（修正 A4/A5/A6）**：

| 能力 | DISABLED | OK | UNKNOWN | STALLED |
|---|---|---|---|---|
| 钩子 | — | `ready && installed && healthy` | — | 否则 |
| 学习 | `!enabled` | `enabled && started && samples > 0` | `enabled && started && samples == 0` | `enabled && !started` |
| 名单 | `!active` | `active && started && !paused && observed` | `active && started && !observed` | `active && (!started \|\| paused)` |
| 日志 | `!enabled` | `enabled && ready && directory_resolved` | `enabled && ready && !directory_resolved` | `enabled && !ready` |

「暂停」归入 `STALLED` 是有意的：暂停期间名单确实不生效，用户按「名单不生效」来查时应看到这一条。

**显示方式（修正 A8）**：追加到 `IDC_STATUS` 的文本，不动 rc。例：

```
状态：保护中 · 已拦截 128 次        钩子✓ 学习✓ 名单— 日志✓
```

`SETTINGS_STATUS_SIZE` 是 384，`log_format_status` 现在只输出一两句，余量充足。文本拼接放进 `log_format.c`，那里已有 `test_core.c:1527` 的单测模式。

**未认领 ID 计数（修正 A12）**：`AppState` 加 `uint32_t caps.unhandled_menu_ids`，无条件递增（不加 `#ifndef NDEBUG`），并进上面那一行显示。Release 用户报「这个菜单点了没反应」时，这个计数器是唯一线索。

**语义边界**（终审 P0-1）：它统计的是「收到了 `WM_COMMAND` 但没有任何分组认领」，是**运行时诊断线索**，
**不是自动化验收标准**。自动化测试若枚举 ID 空间逐条发送，必然触发大量「本来就没定义的区间」，
计数非零是正常的——V3 因此改用副作用断言，不读这个计数。

---

## 4. 明确不做

| 不做 | 理由 |
|---|---|
| 物理拆文件 | 见 §2.1 |
| `CAP_TRAY` | `healthy = enabled` 无信息量 |
| 逐键进度汇总行 | 被逐键行取代 |
| 改 `debouncer.c` | 2.3.0 的核心资产 |
| 自动应用阈值 | 属于后续阶段 |

---

## 5. 验证（v2 全部换过）

| 编号 | 方法 | 通过标准 |
|---|---|---|
| **V1** | 单测：分类器断言**具体组别** + 边界 | 每组至少 3 个 ID，含区间首尾与跨区间陷阱 |
| **V2** | **机械 diff**：`git diff` 后核对四个 `Handle*` 与原 305 行，除注释外只允许 `return 0` → `return true` 与缩进差异 | 人工逐行看完，允许的差异类型只有两种 |
| **V3** | **e2e 副作用断言**：对每个**有注册表副作用**的 ID 发送 `WM_COMMAND`，读注册表断言它确实变了（现有 `e2e.ps1` 已有这套 `Check` 模式）；对其余安全 ID 只断言进程存活、无残留 `#32770` 弹窗 | 每个有副作用的 ID 都被单独断言；无 ID 导致崩溃或卡框 |
| **V4** | 单测：`app_capabilities_evaluate` 在零初始化入参上不崩、全 `DISABLED` | 通过 |
| **V5** | 单测：`learning_service` 用假 provider 启动，确认线程取到 provider 的值 | 通过 |
| **V6** | 真机：菜单截图测量像素宽度 | 不超过 **240 px**（现有最宽项 `采用全部学习建议` 约 150 px，留出余量） |
| **V7** | 完整回归：四配置 + `/analyze`、x64/x86 单测、`e2e` / `diagnose` / `stuck_key` | 0 error / 0 warning / 0 failure |

**V3 的安全 ID 跳过表**（这些 ID 有副作用，不能盲发）：

`IDM_EXIT`（退出）、`IDM_ABOUT`（模态）、`IDM_OPEN_SETTINGS`（开窗）、`IDM_RESTORE_DEFAULTS`（确认框）、`IDM_EXPORT_SUPPORT`（写文件 + 弹框）、`IDM_OPEN_LOG_FOLDER`（开资源管理器）、`IDM_CONTEXT_ADD_WHITE` / `ADD_BLACK`（弹文件框）。

其余（开关类、阈值类、名单模式类、学习开关）都可以发，e2e 结尾恢复设置。

**基线**：当前单测 **3148 / 0**。v2 需更正 v1 的「断言一条不改」为「除 `learning_service_start` 签名适配那 2 行外，断言不改」。

---

## 6. 风险与回退

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 切分漏掉一条分支（尤其 3 条双 ID 合并） | 中 | 某菜单项失效 | V2 机械 diff + V3 逐 ID |
| `return 0` 误写成 `return false` | 中 | 落到 `DefWindowProc`，行为变更 | V2 只允许 `return 0`→`return true` |
| 分类器区间判定与解码函数不一致 | 中 | 分类对了但执行静默失败 | V1 断言组别 + 边界 ID |
| 窄接口回调加锁 | 低 | 学习线程每 100 ms 抢锁 | 方案钉死「不得加锁」；代码注释重复一遍 |
| 能力自检读未初始化字段 | 中 | 崩溃（有前科） | 全部经 `CapabilityInputs` 传值，纯 POD，V4 零初始化测试 |
| 删 `STR_LEARNING_PROGRESS_FMT` 漏一处 | 低 | 编译失败 | 三处同删，编译即验 |

**回退**：四项独立提交，单项 `git revert` 不影响其他。

---

## 7. 实施顺序（按 A3 修正）

| 步 | 内容 | 为什么在这个位置 |
|---|---|---|
| 1 | **学习窄接口（3.4）** | 唯一会改其他模块签名的一项，先做；后面全是加法 |
| 2 | **菜单收窄（3.1）** | 依赖步 1 定下「阈值从哪来」 |
| 3 | **分类器 + 四函数切分（3.2/3.3）** | 独立提交，V2/V3 验证 |
| 4 | **能力自检（3.5）** | 放最后，因为它的显示要用到前面稳定的状态 |

**每步之间跑一次完整回归**，不攒着测。

**新文件登记（漏了就是 LNK2019）**：
`src/core/command_classify.{c,h}` 与 `src/core/app_capabilities.{c,h}` 各需登记 **4 处**：
`MouseMend.vcxproj`、`MouseMend.vcxproj.filters`、`tests/MouseMendTests.vcxproj`、`MouseMend.vcxproj.filters` 对应项。
（`tools/MouseMendTrace` 只在它直接引用这些符号时才需要。）
