# 审计后续整改方案（v2.4.1 之后）

> 来源：第三方评审 + 本地核实。本文按「求证 → 方案 → 自评 → 预期 → 修订 → 实施 → 审计」推进。

---

## 一、求证：现在的代码到底是什么样

| # | 疑点 | 核实结果 |
|---|---|---|
| 1 | 诊断报告缺学习/名单 | **属实**。`diagnostics.c` 现有 17 个字段（exe / language / profile / protective_buttons / hybrid / release_delay_ms / drag_distance_px / notify_blocks / logging / log_dir / hook_install / status / hover_text_chars / settings_writable / autostart / hook_error / result），无 learning、无 context、无 pause。`support_bundle.c` 同样没有。 |
| 2 | 加报告键会不会让 `diagnose.ps1` 失败 | **不会**。`diagnose.ps1:51-56` 是「白名单里的键必须存在」，**多出来的键不报错**。所以新增键只会「没人测」，不会「测挂」。附带结论：早先「刻意不加」的顾虑不成立。 |
| 3 | `diagnostics_build` 能不能拿到运行时状态 | **不能**。它自己 `debounce_init` + `settings_load`，是从注册表重建的一份独立配置（`diagnostics.c:121-134`），与 `main.c` 的 `g_app` 无关。所以它**只能报告配置，不能报告「线程是否在跑」「当前前台是谁」**。 |
| 4 | `context_store` 的静态缓冲 | **属实**。`context_store.c:11` 的 `g_raw` 约 16K 宽字符，读/写 `MULTI_SZ` 共用，注释声明只在 UI 线程用，但没有显式标注非线程安全。 |
| 5 | 注册表键名是否集中 | **没有**。普通设置在 `settings.c`，名单在 `context_store.c`，两份各写各的键名，没有对照表。 |
| 6 | 状态条宽度会不会随 DPI 变 | **不会**。`status_strip_measure` 只在 `refresh_capabilities` 的状态变化分支里调用，DPI 变化不触发。当前机器 100% 缩放，所以没暴露。 |

---

## 二、方案

### A1 给 `g_raw` 加线程约束标注

在 `context_store.c` 的静态缓冲上方写明：非线程安全，只允许 UI 线程使用，若将来跨线程调用必须改为调用方传入缓冲。

**改动**：3 行注释，零逻辑。

### A2 注册表键名集中成清单

在 `settings.h` 与 `context_store.h` 头部各列一张「键名 → 类型 → 默认值 → 归属模块」的表，并互相注明「另一处还有哪些键」。

**改动**：纯注释。

### A3 诊断报告补 4 个**配置类**字段

新增：

```
learning_enabled=on|off
context_mode=all|whitelist|blacklist
context_rules=白名单数/黑名单数
context_migrated=yes|no
```

**实现方式**：在 `diagnostics_build` 内直接 `context_store_load(&policy)`（只读注册表），配合已有的 `prefs.learning_enabled`。**不改函数签名**。

### A4 状态条宽度随 DPI 重测

`refresh_live` 里记录上次 DPI，变化时调一次 `status_strip_measure`。

**改动**：约 8 行。

---

## 三、自评：这些方案的问题在哪

**A3 的局限（必须先说清）**：

它只能回答「配置开没开」，**不能回答「开了但没工作」**。

- 「名单没生效」→ 能看出 mode 和规则数，**看不出轮询线程是否启动**
- 「学习没开」→ 能看到，**看不出线程是否卡死**

要回答后者，必须把 `CapabilityState[4]` 和前台程序名从 `main.c` 传进来，那要改 `diagnostics_build` 签名、`main.c` 两处调用、`settings_window.c`、`support_bundle.c` 和 `diagnose.ps1`——**跨 5 个文件**。

**所以 A3 只解决一半盲区，不能宣传成「盲区已消除」。**

**A4 的问题**：

当前机器 100% 缩放，`WM_DPICHANGED` 也不存在，所以**这段代码永远不会执行**。它是纯粹为将来 manifest 升级铺路，收益为零、成本为零、风险为零。

**如果本轮目标是「可验证的改进」，A4 不具备可验证性。**

**A1 / A2 的问题**：

纯注释，无运行时影响，也不需要测试。

---

## 四、预期（验收条件）

| 项 | 预期 |
|---|---|
| 编译 | 四配置 Rebuild 含 `/analyze`，**0 error 0 warning** |
| 单测 | **3336 / 0，数字不变**（本轮不改任何判定逻辑） |
| `--diagnose` | 输出多 4 行，**全 ASCII**，不破坏 `diagnose.ps1` 的 17 个既有键 |
| `diagnose.ps1` | 全部 PASS，且新增 4 个键也纳入断言 |
| `e2e` / `stuck_key` | 各 0 failure（不受影响） |
| 报告体积 | 增长不超过 200 字节 |

---

## 五、修订：根据自评调整

| 项 | 原计划 | 修订后 | 理由 |
|---|---|---|---|
| A1 | 做 | **做** | 零风险 |
| A2 | 做 | **做** | 零风险 |
| A3 | 做 | **做，但限定为配置类** | 运行时状态跨 5 文件，单独一轮 |
| A4 | 做 | **降级为「同时补 DPI 常量换算」** | 只加 measure 重测而不改 `GAP/PAD/SLACK`，升级 manifest 后边距仍然错；两件一起做才有意义，而常量换算会改变当前视觉，**必须真机对比**，不适合和注释混在一轮 |

**最终本轮范围：A1 + A2 + A3。** A4 与 Per-Monitor V2 一起，等新窗体那一轮做。

---

## 六、实施记录

| 项 | 实际改动 |
|---|---|
| A1 | `context_store.c` 的 `g_raw` 注释从 2 行扩到 8 行：写明非线程安全、为什么今天安全、将来要改成传缓冲 |
| A2 | `settings.h` 加 8 个键的表格；`context_store.h` 加 4 个键 + 旧键 `ExcludedExes` |
| A3 | 新增 `context_store_inspect()`（不触发迁移的只读读取）；`diagnostics.c` 输出三行；`diagnose.ps1` 白名单同步 |
| A4 | **未做**，并入 Per-Monitor V2 那一轮 |

**实施中的两处偏离原方案**：

1. **取消 `context_migrated` 字段**。它只反映一次性标记，对排查没有价值，而 `inspect`
   要为此多读一个值。实际输出三行，不是方案里的四行。
2. **`ContextPolicy` 改成函数内 `static`**。第一版放在栈上，`/analyze` 立刻报
   `C6262: 函数使用堆叠的 72432 字节`。改完归零。
   **这是「先跑分析再提交」救回来的一次**——如果只看单测，这条警告会被带到发布。

## 七、审计记录

| 检查项 | 结果 |
|---|---|
| 四配置 Rebuild 含 `/analyze` | **0 error 0 warning** |
| 单测 x64 / x86 | **3336 / 0**，与改动前完全一致（本轮没碰判定逻辑） |
| `--diagnose` 输出 | 新增三行，全 ASCII |
| `diagnose.ps1` | 0 failure，且 **`settings unchanged by diagnose = True`** |
| `e2e` | 0 failure |
| `stuck_key` | **SKIP（会话锁定）**，不是 failure；重试仍 SKIP |
| `audit_printf.py` | 0 问题 |
| `context_store_inspect` 调用方 | 只有 `diagnostics.c` 一处 |

**审计发现的遗留**：

- `stuck_key` 本轮没真正跑过。它测的是引擎粘键行为，而本轮没有改动
  `debouncer.c` / `mouse_hook.c`，风险为零，但**下次发布前必须补跑一次**（解锁后）。
- 诊断报告仍然只能回答「配置」，不能回答「线程是否在跑」。报告里的注释已写明，
  避免下次误以为盲区已经消除。
