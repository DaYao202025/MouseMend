# 方案 v3：托盘菜单重构 + 白/黑名单模式（2026-11-26）

> 状态：**两轮评审后定稿，可实施**。v1/v2 各被评出 4 个 P0，本文逐条修正并标注来源
> （A=架构评审，C=正确性评审，N=v2 复核新增）。

## 0. 需求

1. 「开机自启」移进「高级」。
2. 原位置换成三个母菜单：自学习 / 白名单 / 黑名单，一级净增 2 行。
3. **白/黑互斥**，也可都不选（= 全部拦截）。
4. 白名单 = 只在名单内程序拦截；黑名单 = 名单内程序不拦截。
5. 「添加程序」打开 Windows exe 选择框；**本软件不能被添加**。
6. 记录**完整路径**；菜单显示已添加项。
7. 自学习可开关，并显示各键阈值与建议。
8. 同一程序不能同时在两个名单，重复时提示「已在 X 名单中」。
9. 设置窗口暂不加 UI，但数据模型与 UI 解耦。

## 1. 三轮评审的修正汇总

| 来源 | 问题 | v3 修法 |
|---|---|---|
| A-C-P0-1 | `SRWLOCK` 放进 `ContextPolicy` 会让它不可复制；`memset` 清零「恰好有效」是 ABI 巧合 | `ContextPolicy` 保持**纯 POD**；`SRWLOCK` 只放在 `ContextFilter` 顶层，并在 init 里显式 `InitializeSRWLock` |
| A-C-P0-2 | `static_assert` 在 C89 模式下编不过；`sizeof(ContextSnapshot)` 实际 296，断言必然失败 | 用负数组宽度的 `MM_STATIC_ASSERT`；断言改成 `offsetof(ContextFilter, policy) % 64 == 0` |
| A-C-P0-3 | `context_path_matches` 的 `file_name` 参数在 UI 添加时无来源，互斥仍可绕过 | 规定 `add_context_path` **只收完整路径**，内部用 `file_name_from_path` 提取；**用户接口永不产生通配条目**（只有迁移能） |
| A-N-P0-1 | `ExcludedExes` 与新的两名单**双源并存**，`IDM_EXCLUDE_CURRENT` 仍写旧键 | **整套删除**：`Preferences.excluded_exes`、`IDM_EXCLUDE_CURRENT`、`app_controller_set_excluded_exes`、`encode/decode_rules` 等；只保留一个只读的 legacy 读取函数供迁移 |
| A-N-P0-2 | `Preferences` 与 `policy` 两份名单，同步点无落点（`AppController` 不知道 `ContextFilter`） | **名单只存一份真源**：`ContextPolicy`；持久化独立成 `context_store.c`；`SaveSink` 里额外调 `context_store_save` |
| A-N-P0-3 | `#if` 守卫引用未定义的 `IDM_WHITELIST_ITEM_BASE`，预处理按 0 求值 → 守卫恒假 | 先在 `menu_ids.h` 定义宏，再用字面量写 `#error`；并补全段两两比较 |
| C-P1-1 | `WM_APP_ADD_EXE` 未分配 ID | `#define WM_APP_ADD_EXE (WM_APP + 13)` |
| C-P1-3 | 写侧未校验 `len_i < CONTEXT_PATH_CHARS`，超长路径被静默截断成另一个合法路径 | 写入前校验长度，超长返回失败码 |
| C-§5 笔误 | 「`bytes == 0` 视为畸形」与「空列表是 4 字节」自相矛盾 | 只保留 `bytes < 4` 或 `bytes % 2 != 0` 判畸形 |
| A-P1-4 | `ALL` 项在两个子菜单共用同一 ID，观感混淆 | 两个子菜单各用自己的 ID（`+85` / `+90`），分发到同一处理函数 |
| A-§4 | 删「高级」里的学习三项会破 e2e | 删**菜单项**，**保留 `WM_COMMAND` 分支与 ID `+81/+82`**（e2e 按 ID 直接 PostMessage） |
| A-§3 | `MF_CHECKED` 跨子菜单不构成 radio 组 | 每个子菜单内部用 `CheckMenuRadioItem`；「关闭」文案带上本名单名 |

## 2. 语义：三选一枚举

```c
typedef enum
{
    CONTEXT_MODE_ALL = 0,
    CONTEXT_MODE_WHITELIST,
    CONTEXT_MODE_BLACKLIST,
    CONTEXT_MODE_COUNT
} ContextMode;
```

| 模式 | 命中白名单 | 命中黑名单 | 其他程序 |
|---|---|---|---|
| `ALL` | —— | —— | 拦截 |
| `WHITELIST` | 拦截 | 拦截（防御性定义，记录 WARN） | 不拦截 |
| `BLACKLIST` | 不拦截 | 不拦截 | 拦截 |

- 读取时 `value < CONTEXT_MODE_COUNT` 才采纳，否则回落 `ALL` 并覆盖写回 0。
- 判定函数 `default:` 按 `ALL`（宁可多拦，不可漏拦）。
- **模式切换只改 `policy` 并让轮询线程重算快照，绝不调用 `debounce_reset`**。

## 3. 匹配：唯一函数 + 唯一键

```c
// rule 形式：
//   "C:\Games\game.exe"  完整路径（用户添加的唯一形式）
//   "*\game.exe"         旧版迁移而来，只比文件名
bool context_path_matches(const wchar_t *rule, const wchar_t *full_path, const wchar_t *file_name);
```

- `rule` 以 `*\` 开头 → `_wcsicmp(rule + 2, file_name)`
- 否则 → `_wcsicmp(rule, full_path)`；失败再比 `GetLongPathNameW(full_path)`

**添加路径时**（`add_context_path` 只接受完整路径）：

```c
wchar_t name[CONTEXT_NAME_CHARS];
file_name_from_path(full_path, name, ARRAYSIZE(name));   // 从完整路径提取
```

五步检查，**一次持锁完成**：

1. `full_path` 等于本进程 exe（`GetModuleFileNameW` 只取一次缓存）→ 拒绝。
2. 当前名单 `context_path_matches(rule, full_path, name)` 任一命中 → 「已在该名单中」。
3. 另一名单同样命中 → 「已在白名单/黑名单中，请先移除」。
4. 数量 ≥ 16 → 「名单已满」。
5. 加入。

> 这样 `*\game.exe` 与 `D:\Game\game.exe` 不可能共存：第 2/3 步用的 `name` 让通配规则能命中路径条目。

**用户接口永不产生通配条目**；通配只由迁移内部函数写入，`add_context_path` 入口拒绝以 `*` 开头的路径。

## 4. 数据结构

```c
#define CONTEXT_LIST_MAX 16
#define CONTEXT_PATH_CHARS 1024

typedef struct
{
    wchar_t paths[CONTEXT_LIST_MAX][CONTEXT_PATH_CHARS];
    uint32_t count;
} ContextPathList;

// Hot path: the hook reads only these fields.
typedef struct
{
    ContextSlot slots[2];
    volatile LONG current;
    volatile LONG64 pause_until;
} ContextSnapshot;

// Pure POD, no lock inside: it can be memcpy'd, memset'd and stored anywhere.
typedef struct
{
    ContextMode mode;
    ContextPathList whitelist;
    ContextPathList blacklist;
    wchar_t recent_path[CONTEXT_PATH_CHARS];
    wchar_t recent_name[CONTEXT_NAME_CHARS];
} ContextPolicy;

typedef struct
{
    ContextSnapshot snap;
    SRWLOCK lock; // the only lock, initialised explicitly in context_filter_init
    __declspec(align(64)) ContextPolicy policy;
} ContextFilter;
```

```c
// .c file, C89 compatible (tag must be unique within the TU):
#define MM_STATIC_ASSERT(cond, tag) typedef char mm_sa_##tag[(cond) ? 1 : -1]
MM_STATIC_ASSERT(sizeof(ContextSnapshot) <= 384, snapshot_budget);
```

- 用 `__declspec(align(64))` 修饰 policy 成员，编译器会插入 padding 让它从新的 cache line 开始。
  **不再断言 `offsetof(...) % 64 == 0`**：align 已强制成立，该断言恒真、没有防护价值（终审结论）。
  改成断言快照不膨胀（`<= 384`，当前 296），防止将来加字段后跨太多行。
- `context_filter_init`：`memset(filter, 0, sizeof(*filter))` 之后**显式** `InitializeSRWLock(&filter->lock)`，不依赖「0 值恰好有效」这条 ABI 细节。
- 钩子：`context_filter_should_suppress(&filter->snap, ts)`，**只读 `slots[current].excluded`，头文件写明「此函数内不得出现 `AcquireSRWLock*`」**。
- 体积：`ContextPolicy` ≈ 2 × 16 × 1024 × 2 = 65 KB，`g_app` 静态，可接受。

## 5. 单一真源与持久化

**名单只存在 `ContextPolicy` 里**，`Preferences` 不复制一份（避免两份漂移）。

新增 `src/core/context_store.c/.h`：

```c
bool context_store_load(ContextPolicy *policy);   // 读三个键 + 迁移
bool context_store_save(const ContextPolicy *policy); // 写三个键
bool context_store_read_legacy(wchar_t *buffer, size_t count); // 只读旧 ExcludedExes
```

- `context_store_save` 只被 `main.c` 的 `SaveSink` 调用：`settings_save(...) && context_store_save(&g_app.context_filter.policy)`。
- `AppController` 新增 `ContextPolicy *policy` 字段与 `app_controller_attach_context(controller, policy)`；三个 setter 操作 policy 后 `mark_dirty` + `notify`。
- **`settings_save` 签名不变**（只传指针，零拷贝）；名单不进 `Preferences`，不会踩 C6262。

| 键 | 类型 | 默认 |
|---|---|---|
| `ContextMode` | REG_DWORD | 0 |
| `WhitelistExes` | REG_MULTI_SZ | 空 |
| `BlacklistExes` | REG_MULTI_SZ | 空 |
| `ContextMigrated` | REG_DWORD | 0 |
| `ExcludedExes`（旧） | REG_SZ | 只读，供迁移 |

`REG_MULTI_SZ`：
- **写**：`cbData = (Σ(len_i + 1) + 1) * 2`；N=0 → `cbData = 4`，两个 NUL；写入前校验每条 **`0 < len_i < CONTEXT_PATH_CHARS`**。
  **空条目必须拒绝**（终审 P0-C）：读侧遇到第一个空串就停止计数，写侧若允许 `len_i == 0`，
  读回时会在该条目处截断，**其后所有名单项静默丢失**。
- **读**：`RegQueryValueExW` 失败 → 空表；`type != REG_MULTI_SZ` → 空表；`bytes < 4` 或 `bytes % 2 != 0` → 畸形，空表；遇到第一个空串立即停止计数。

## 6. 迁移（一次性）

1. 读 `ContextMigrated`，为 1 → 跳过。
2. `BlacklistExes` 非空 → 已迁移，写 `ContextMigrated=1`，跳过。
3. 否则读旧 `ExcludedExes`：每条转成 `*\文件名`，逐条校验，坏条目丢弃并计数；写进 `BlacklistExes`；`ContextMode` 若为 `ALL` 改为 `BLACKLIST`；写 `ContextMigrated=1`。
4. 旧 `ExcludedExes` 保留不删（回退旧版可读），新版本不再写它。

菜单里迁移条目显示 `game.exe（旧版规则）`，名单顶部显示 `已从旧版迁移 N 条规则`（N>0 时）。

## 7. 菜单

一级（自启移走后）：

```
[状态] [统计]
──────────────
[7 个按键子菜单]
──────────────
[禁用/启用全部]
[智能拖拽] ✓
[自学习模式 ▸]
[白名单模式 ▸ (3)]     ← 模式为 ALL 时显示 [白名单模式（未启用） (3)]
[黑名单模式 ▸ (2)]
[清除统计]
──────────────
[预设 ▸] [高级 ▸] [语言 ▸]
──────────────
[关于] [设置] [退出]
```

```
自学习模式 ▸                       白名单模式 ▸                      黑名单模式 ▸
  ✓ 启用自学习                       ● 关闭 — 白名单不生效             ● 关闭 — 黑名单不生效
  ─────────                            ○ 只在白名单程序里拦截            ○ 黑名单程序不拦截
  左键 50 → 建议 25                  ─────────                         ─────────
  右键 50 → 样本不足                   game.exe（旧版规则）              chrome.exe
  ─────────                            design.exe                        ─────────
  采用全部建议                        ─────────                         添加程序…
                                      添加程序…                         最多 2 秒生效
                                     最多 2 秒生效
```

- 每个子菜单内部用 `CheckMenuRadioItem` 表示「关闭 / 本名单生效」二选一。
- 「关闭」项：白名单子菜单 `IDM_CONTEXT_MODE_ALL_WHITE(+85)`，黑名单子菜单 `IDM_CONTEXT_MODE_ALL_BLACK(+90)`，都分发到同一个「设为 ALL」处理。
- 名单项点击 = 移除（确认框显示完整路径）；空名单显示「尚未添加程序」（ID 0）。
- 「添加程序…」只 `PostMessage(WM_APP_ADD_EXE, mode)`，文件框在菜单关闭后弹出。
  **必须是二段式**（终审 P0-B）：`WM_APP_ADD_EXE` 的处理函数首行再 `PostMessage(WM_APP + 14)`（或 `SetTimer` 一次），
  第二段才调 `GetOpenFileNameW`。否则它会与 `context_menu_show` 在 `TrackPopupMenu` 返回后发的 `WM_NULL` 竞争，
  文件框内部的消息泵会吃掉托盘的下一次点击，表现为「第二次右键没反应」。
- 「高级」新增「开机自启」，删除原有学习三项（ID `+81/+82` 的 `WM_COMMAND` 分支**保留**）。

## 8. 命令 ID

| 用途 | ID |
|---|---|
| 白名单「关闭」 | `WM_USER + 85` |
| 模式 WHITELIST | `WM_USER + 86` |
| 模式 BLACKLIST | `WM_USER + 87` |
| 白名单添加 | `WM_USER + 88` |
| 黑名单添加 | `WM_USER + 89` |
| 黑名单「关闭」 | `WM_USER + 90` |
| 白名单列表项 | `WM_USER + 3000 + i` |
| 黑名单列表项 | `WM_USER + 3100 + i` |
| 文件框请求 | `WM_APP + 13` |

`menu_ids.h` 守卫（宏必须先定义）：

```c
#if (IDM_CONTEXT_LIST_WHITE + CONTEXT_LIST_MAX) > IDM_CONTEXT_LIST_BLACK
#error "context list ID ranges overlap"
#endif
#if (IDM_CONTEXT_LIST_BLACK + CONTEXT_LIST_MAX) > (WM_USER + 4000)
#error "context list range is too wide"
#endif
#if (WM_USER + 90) >= IDM_THRESHOLD_BASE
#error "context mode IDs collide with the threshold range"
#endif
```

`test_menu_id_uniqueness` 改为**全段两两比较**（把 `+10..+19/+20/+22/+23/+30..+36/+41/+50/+51/+60..+63/+70..+74/+80..+90/+1000..+1079/+2000..+2007/+3000..+3015/+3100..+3115` 全部展开后排序查重）。

## 9. 实施步骤

1. **纯逻辑**：`context_path_matches`、`ContextPathList` 增删查、`context_mode_suppresses`、`file_name_from_path`（从 `context_service.c` 移到可复用位置）；单测。
2. **持久化**：`context_store.c`（MULTI_SZ 读写 + 迁移 + legacy 只读）；删 `ExcludedExes` 体系。
3. **控制器**：`attach_context` + 三个 setter；`SaveSink` 增加 `context_store_save`。
4. **菜单**：自启搬高级、删高级学习三项、三个母菜单、列表渲染与旧版标注、`PostMessage` 添加、移除确认。
   **`RestoreDefaults` 必须同时重置 policy**（终审 P0-A）：`mode = ALL`、清空两名单、
   `context_store_save` 写回空表；否则 e2e 的「恢复默认后 `ContextMode == 0`」必然失败。
5. **轮询**：`context_service` 保留完整路径，用 `context_path_matches` + `context_mode_suppresses` 计算 suppress。
6. **自审**：见 §11。

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 互斥被绕过 | 查重与匹配共用 `context_path_matches`；添加路径时提取 `file_name`；用户接口不产生通配 |
| 锁清零崩溃 | `SRWLOCK` 不放 policy 内；init 里显式初始化；policy 纯 POD |
| false sharing | `__declspec(align(64))` + `offsetof` 断言 |
| e2e 被文件框阻塞 | 文件框走 `WM_APP_ADD_EXE`，e2e 从不发该消息 |
| 删菜单项破 e2e | 只删菜单项，保留 `+81/+82` 的 `WM_COMMAND` 分支 |
| 超长路径截断 | 写侧校验 `len_i < CONTEXT_PATH_CHARS` |
| 8.3 短名 | 先比原样，再比 `GetLongPathNameW` |
| 名单满 16 | 明确提示 |
| 本软件被添加 | 按路径比较本进程 exe；注册表手写属已知限制 |
| 模式切换破坏 `system_down` | 只改 policy，绝不 `debounce_reset` |

## 11. 自审清单

1. 四配置 Rebuild 含 `/analyze` 0 error 0 warning。
2. 单测 x64/x86 全绿；覆盖：模式逐格（含 `WHITELIST×黑名单`）、通配与路径交叉互斥、上限 16、空表、非法模式值（-1/3/0x7FFFFFFF）、**`suppress=true` 时事件仍走 `debounce_process_event_ex_suppressed`**、`test_context_filter_unstarted`（全零结构体上调用各读接口）、`publish` 与 `suppress` 并发压力、MULTI_SZ 编解码（空/1条/恰好16条/畸形长度）。
3. `audit_printf.py` 0 问题；语言双表齐全。
4. 四个工程登记一致。
5. `e2e` / `diagnose` / `stuck_key -Force` 全绿；新增断言：`ContextMode` 默认 0、恢复默认后 0。
6. **真机**：三模式各切换一次验证拦截；添加/移除各一次；选本软件被拒；通配与路径交叉被拒；重启后保留；旧 `ExcludedExes` 迁移后仍生效；清空名单后重启不回弹；旧版 exe 回退后仍能读到旧键。
7. 回归上一轮崩溃场景：学习关闭/开启 + 打开菜单。
8. 1366×768 下三个子菜单不翻转；菜单项数量断言（`GetMenuItemCount` + 关键 `GetMenuString`）。
9. **grep 断言：`context_filter.c` 的 `should_suppress` 函数体内不出现 `AcquireSRWLock`**。
10. trace 版钩子耗时：P50 ≤ 基线 5%，P99 增量 ≤ 50 µs。

## 12. 待确认

1. 名单上限 16、路径 1024 字符是否够？
2. 旧 `ExcludedExes` 保留不删（回退旧版可读），可以吗？
3. 迁移条目显示「（旧版规则）」、删除后消失，可以吗？
