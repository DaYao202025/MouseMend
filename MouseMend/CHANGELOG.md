# Changelog / 更新记录

## 2.5.0 — 2026-11-28

> 另外两项属于内部改善，用户看不到，故不单列：`language.c` 的版本说明表加了
> 「发版必须同步」的注释；状态灯的缝隙改为「视觉上分开、命中归左边块」。

### 新增 / Added

- **状态灯改成单击展开**：以前鼠标滑过哪块就展开哪块。实际用起来"追不上"——块的左边缘
  是前面所有块的宽度之和，鼠标一靠近，目标就被推着走；暂停时倒计时还会让宽度跟着数字
  抖，指针下面的块会自己换。现在鼠标滑过只把那一块的字符点亮，**点一下才展开**，再点
  一下收回总览；悬停提示会写清点下去会发生什么。暂停期间的剩余时间按最长格式预留宽度，
  整段倒计时里这条色块不会动一个像素。**点别的控件、或把窗口收进托盘，都会自动回到总览**，
  不用再点回去；只有点进三个文字窗格和测试区不算（那还是"在看同一件事"）。
  The status strip opens on click instead of on hover. Hovering only brightens the
  character, so the row never moves under the pointer; the tip says what a click
  will do, and the paused countdown keeps a reserved width so nothing shifts while
  it counts. It folds back to the verdict when you click another control or put the
  window away, so it never stays open by accident.
- **暂停过滤可以选择时长**：原来是写死的 5 分钟——想立刻试一个设置时嫌久，看一段过场
  动画时嫌短。现在托盘菜单的「暂停过滤」是一个子菜单：暂停时顶部有「恢复过滤」，下面是
  5 / 10 / 30 分钟和「自定义…」。自定义范围 **1–720 分钟**（最长 12 小时），走与「设置
  拦截时长」同一个输入框——空串、小数、带字母、越界值都拒绝。
  暂停时窗口顶部第一个色块变琥珀色并显示 `已暂停 · 剩余 4:32`，逐秒减少。
  Filtering pause now takes a duration: 5 / 10 / 30 minutes or a custom value
  from 1 to 720, instead of a hard-coded five. The verdict block turns amber and
  counts down while paused.
- **诊断包补五行运行时状态**：`context_paused` / `pause_left_ms` / `advice_count` /
  `foreground` / `caps`。`--diagnose` 是启动开关，在引擎起来之前执行，这类信息它结构性地
  拿不到——所以「配了为什么没生效」以前只能靠猜。
  The exported bundle now carries the live state that `--diagnose` cannot see.
- **测试日志可以只看一个按键了**：筛选从「自动 + 左/中/右」变成「全部 + 左/中/右/侧一/侧二」，
  六个开关排成一行。**选「中」也能看到滚轮**——滚轮就是中键压下去换了个方向，以前选了
  "中键"却看不见任何滚动，是最别扭的一处。判定引擎没变，中键仍然把三个键分开算，
  这只是显示层的合并。六个开关没有让窗口变宽：实测英文最宽的一档合计 217 DLU、可用 230。
  The log can be filtered by any of the five buttons, and "middle" now includes the
  wheel - it is the same physical control. The row of six fits without widening the
  window.
- **拦截记录能看出是哪个程序了**：每行在时间后面多一列程序名
  （`14:32:01.234  chrome.exe  left  pressed  threshold=50ms`），取不到时写 `-`，
  五列始终对齐。文件名第一次可能出现中文，所以新建的文件带 UTF-8 BOM，Excel 不乱码。
  顺手把「历史目录」改叫「拦截记录」、「清空记录」改叫「清空显示」——**它只清窗口，
  不删磁盘上的文件**，这一点现在按钮和悬停提示都会说明。诊断包的最近事件表也补了
  同一列，以后发过来的报告能直接看出是在哪个软件里点的。
  The blocked-clicks log now names the program each click was in, and the exported
  report gained the same column. "Clear log" became "Clear view", because the files
  on disk were never deleted and the label promised more than it did.
- **窗口可以置顶了**：鼠标移到状态灯右边那块空白上会浮出一行「右键试试」，右键就有
  「窗口置顶」。勾上之后窗口一直压在其他窗口上面，方便一边看日志一边操作别的软件；
  它是窗口状态，不是设置，退出程序后不会保留。**这块空白平时什么都不画，窗口宽度没有变。**
  Hover the empty space right of the status strip and it says "Right-click here";
  right-click gives you "Always on top". Nothing is drawn there otherwise, and the
  window is not any wider.
- **底排多了一个「设置」**：设置窗体还在做，点它会在状态栏说明这一点，不会弹框挡住你。
  The Settings button is a placeholder and says so in the status bar.
- **右下角的「自检」换成了实时日志**：以前那块文本是诊断包的一份副本，导诊断包就能拿到，
  摆在窗口里只是占地方。现在这里是**真实时日志**：你在任何一个程序里的点击都会进来，
  按程序分段（`—— chrome.exe ——`），每行写时间、哪个键、与上一次的间隔、按住多久、
  以及引擎的判定——**和上面两块表是同一套列**。按住时间由按下与松开两个真实时间戳相减，
  松开没来就写 `>2000`（下界，不是估计值）。测试区上方那块仍然只记测试区里的点击，
  两者分工不同。**「刷新」按钮随之取消**（它只刷新那块自检）；「清空记录」会把实时日志
  一起清空，等于从头开始。
  The self check pane is now a live log of clicks in every program, grouped by
  program, with the same five columns as the panes above it. The refresh button is
  gone with the pane it served.
- **测试区会给侧键和滚轮画圈了**：以前侧键和滚轮按下去**什么标记都没有**（空白灰圈），
  滚轮甚至根本不画圈。现在侧一/侧二各有自己的颜色和文字，上下滚动分别显示「上」「下」——
  方向来自事件本身的 delta，因为上滚和下滚是同一个按键。横向滚轮显示「横」，
  不给它左右箭头：那会和左右键的标记撞。
  The test area now marks side buttons and the wheel; a scroll says which way it went.

## 2.4.2 — 2026-11-27

### 新增 / Added

- **窗口底部新增状态栏**：默认高度的原生状态栏，两格。第一格回答一个此前在界面上
  完全看不到的问题——**当前前台程序正在被拦截还是被排除**
  （`已拦截：chrome.exe` / `已排除：chrome.exe` / `本程序窗口` / `等待前台程序`）。
  第二格暂时留空，用来放事件提示；后续新增的窗体共用同一套布局。
  对话框高度从 299 DLU 增加到 311 DLU，**现有控件一个坐标都没有动**——挤压布局要
  重排状态条、图表、两个文字窗格和四个按钮，那才是真正容易做坏的地方。
  A native status bar at the bottom of the check-up window. Its first column
  reports the one decision that had no visible answer before: whether the
  program in front is being intercepted or exempted. The dialog grew by 12 DLU
  and no existing control moved.
- **状态栏第二格接入消息通道**：钩子被系统丢弃后自动恢复、配置写盘失败这两件事，
  会在这里留一行文字（8 秒 / 10 秒后自动消失），气泡仍然只在第一次出现。同一句话
  30 秒内重复不刷新，所以看门狗反复触发也不会一直闪。
  消息存在应用层而不是窗口里：动作发生在 `main.c`，那时体检窗口可能根本没开；任何
  打开的窗口在下次刷新时自己取走。也**不播报「已保存」**——滑块每拖一次就保存一次，
  那样状态栏会变成滚动字幕。
  A notice channel in the second status bar column. Hook recoveries and failed
  saves leave a line that expires on its own, throttled so a watchdog that
  trips repeatedly does not blink.
- **学习建议生成后不再只能靠右键菜单发现**：建议此前只在托盘菜单里被动展示，用户不
  右键就永远不知道有东西可采纳。现在体检窗口刷新时统计一次「可采纳建议数」，数量
  **发生变化**时在状态栏留一行（`有 2 项阈值建议可在托盘菜单采纳`，10 秒），同一数量
  不重复。
  **只统计菜单实际显示的三个键**：X1/X2 的采样和建议是真实的，但菜单不渲染、采用时
  也会跳过它们，数进去等于让用户去找一个不存在的东西。
  Learning advice is counted on each refresh and announced when the count
  changes, so a user who never opens the tray menu still learns there is
  something to adopt. Side buttons are excluded on purpose.
- **名单项只显示文件名，同名不同路径分不出来**：点哪个删除只能靠猜。现在显示完整
  路径，过长时**在中间**省略（`C:\Users\…\bin\app.exe`）。
  **不能在末尾省略**——菜单本来就是从右边裁剪，而文件名正好在右边，那样会把唯一
  能区分的信息裁掉。
  Tray list entries now show the full path, shortened in the middle when long,
  so two rules with the same file name are distinguishable.

### 修复 / Fixed

- **认不出的前台程序会继承上一个程序的判定**：`OpenProcess` 失败时（提权进程、受保护
  进程）上下文线程保留了上一次发布的槽，于是新程序被套用了旧程序的结论——两个方向
  都错，而且界面上没有任何提示。现在发布 `pid = 0` 且「不豁免」，过滤器保持拦截状态。
  代价是这类程序即使被用户手动排除也会被拦；影响面很小（它们几乎没有鼠标交互需求），
  方向与项目其他部分一致：宁可多拦，不可漏拦。
  When the foreground program could not be identified, the previous program's
  verdict was silently applied to it. The slot is now published as "not exempt"
  with pid 0, which keeps the filter armed.

### 验证 / Verified

四配置 Rebuild（含 `/analyze`）0 error 0 warning；单测 x64 / x86 **3402 / 0**；
`audit_printf.py` **PROBLEMS: none**；`e2e` / `diagnose` 各 0 failure；
`stuck_key` 同日真跑通过（`released latency 1 ms`），发版当时因会话锁定 SKIP，如实记录。
真机截图确认状态栏贴底、高 22 px、第一格显示 `已拦截：msedge.exe`，且现有布局未被挤动。

## 2.4.1 — 2026-11-26

### 变更 / Changed

- **状态行重做为「状态灯」色块条**：一个自绘控件里画五个无边框直角色块——主状态
  「鼠标防护中 / 异常」，后面是 `钩` / `学` / `名` / `志`。五个块各有自己的色相
  （绿 / 蓝 / 紫 / 橙 / 青），底色始终全亮度，收起时只显示一个 35% 白的单字；
  鼠标移上去 100ms 线性向右展开成完整文字，其余保持正方形，移开后回到第一个块展开。
  收起是瞬时的，所以展开过程中只有右边界在动。
  The status row is now a strip of five square status-light blocks, one per
  module, each with its own hue. The block under the pointer opens strictly to
  the right in 100 ms, and the first block stays open at rest.

### 修复 / Fixed

- **自学习会把正常的快双击学成「该拦」**：原来把所有 64ms 内的按下都计入直方图，
  一次 55ms 的正常双击（当前阈值 50ms，本来放行）会把建议推到 56，采纳后那批双击
  开始被吞。现在只采**确实被拦下**的按下——用户丢了点击，才是需要调整的证据。
  Self-learning now samples only presses that were actually blocked. Sampling
  every fast press could advise raising the threshold until a working double
  click started being swallowed.
- **「采用建议」可以绕过 ±50% 限幅**：建议是提前算好缓存的，用户随后手动改阈值不会
  触发重算，于是一条按 100ms 算出的建议能被套到 5ms 的阈值上。现在采用时按当前阈值
  重新限幅。
  The ±50% guard is re-applied at adoption time against the threshold in force
  now, not the one that was in force when the advice was computed.
- **「采用」会悄悄改掉菜单里看不见的 X1 / X2 侧键**：现在只作用于菜单显示的三个键。
- **建议等于当前阈值时仍然可点**：点下去什么都不变，样本却被清零，还提示「已采用」。
  这种情况现在不再产生建议。
- **迁移来的旧排除规则重启后消失**：迁移只改了内存、没有落盘，而「已迁移」标记已经
  写死，用户下次启动名单和模式双双丢失。现在迁移后立即保存。
  Migrated rules were written to memory only; they are now persisted immediately.
- **白名单为空时切到白名单模式会静默关闭全部过滤**：现在先弹确认，说明后果。
- **名单已满仍可点「添加程序…」**：现在满 16 个时该项直接置灰，不用先找 exe 再被告知。
- **名单子菜单的单选勾按「只要不是全部程序」显示**：白名单生效时，黑名单子菜单标题
  写着「未启用」，里面的单选却是选中的。现在按本名单是否为当前模式显示。
- **添加成功的提示不带程序名**：现在提示「已把 chrome.exe 添加到黑名单」。
- **添加规则后 400ms 才落盘**：这个防抖是给滑块拖动用的，菜单动作丢了不划算。
  现在与删除一致，立即写盘。

### 验证 / Verified

四配置 Rebuild（含 `/analyze`）0 error 0 warning；单测 x64 / x86 **3336 / 0**；
`audit_printf.py` 全部调用点 0 问题；`e2e` / `diagnose` / `stuck_key` 各 0 failure。

## 2.4.0 — 2026-11-26

### 新增 / Added

- **自学习阈值（默认关闭，只给建议不自动修改）** —— 打开后，程序统计每个按键
  「被阈值判为抖动」时的真实间隔，用 P80 分位数给出建议值。建议要用户从托盘菜单里
  点「采用全部学习建议」才会写进阈值，且限制在当前值的 ±50% 以内、1–200 ms。
  采样与判定同源（都用 `elapsed_ms`），所以在钩子里只做两次原子自增，不查盘、不加锁。
  **It never changes a threshold on its own; suggestions need one explicit click.**
- **排除指定程序 / 临时暂停** —— 托盘 → 高级 → 「排除当前程序」把最近一次的前台程序
  加入排除表，之后在该程序里完全不去抖；规则写进注册表，重启后保留。
  「暂停过滤 5 分钟」是临时动作，退出即失效。前台程序名由一条 2 秒轮询的后台线程解析
  （`OpenProcess` 只在后台线程），钩子里只读一个原子整数。
- 托盘菜单新增学习建议与样本数显示；「排除当前程序」与「暂停过滤」同样在高级子菜单。

### 变更 / Changed

- 引擎新增 `debounce_process_event_ex_suppressed()`：被排除/暂停时事件仍然完整走一遍判定，
  只是所有拦截都变成放行，`system_down` / `isBlocked` 与实际送达系统的事件保持一致。
  原 `debounce_process_event_ex()` 行为不变。
- **托盘一级菜单重排**：「开机自启」移进「高级」，原位置换成三个母菜单
  **自学习 / 白名单 / 黑名单**，一级只多两行。三个子菜单里都能看到当前状态与已添加的程序。
- **白名单与黑名单互斥**（三选一）：全部拦截 / 只在白名单程序里拦截 / 黑名单程序不拦截。
  用单一枚举而不是两个开关，从数据结构上排除「两个都开」的矛盾状态。
  Whitelist and blacklist are mutually exclusive modes; a single enum makes the
  contradictory "both on" state impossible to represent.
- **添加程序走系统文件选择框**，记录完整路径；本程序自身不能被添加；同一程序不能同时
  出现在两个名单；名单上限 16 个。
- **系统进程不会被误加**：`csrss.exe` / `lsass.exe` / `winlogon.exe` 这类永远不会出现在前台的
  进程会被直接拒绝并说明原因；`explorer.exe` / `ApplicationFrameHost.exe` 等外壳与 UWP 宿主
  允许添加，但会先提示「一条规则影响一整片场景」。
  Processes that can never own a foreground window are refused; shell and UWP
  hosts are allowed after a warning about how much one rule covers.
- 旧版本的「排除列表」（`ExcludedExes`，纯文件名）在首次启动时**自动迁移**为黑名单规则
  （`*\explorer.exe` 形式），原有排除行为保持不变；迁移只执行一次，旧键保留以便回退。
- 顺带清理：补上 `trace_log.c/.h` 在工程 filters 里的历史缺口。

### 重构 / Refactored

- **`WM_COMMAND` 从 305 行一条链拆成四个处理函数**（菜单 / 按键参数 / 名单 / 学习），
  由一个新增的纯逻辑分类器按 ID 空间分组。区间判定原先散在这条链的中段，现在集中在一处，
  且每条区间都带上界——不带上界会把所有更大的 ID 吸进同一组，然后被解码函数静默拒绝。
  新增菜单项从此只改一个函数，不再需要在 305 行里找位置。
  `WM_COMMAND` was a single 305-line chain covering four unrelated features; it is now
  four handlers behind a pure classifier.
- **自学习子菜单收窄为 3 列**：`键名 / 当前→建议 / 样本数`。去掉单位文字与保持时长，
  但**保留建议值本身**——打开这个子菜单只问一件事「该不该点采用」，没有建议值就无法判断。
- **自学习模块不再认识引擎**：启动时收一个阈值回调，不再持有 `DebounceManager *`。
  引擎改字段名时学习模块不用动，也不会因为调了加锁接口而与钩子线程抢锁。
- **新增能力健康自检**：钩子 / 学习 / 名单 / 日志四项各判「关闭 / 正常 / 尚未观测 / 停摆」，
  原先只有一个「防护正常」，模块在背后静默死掉时照样显示正常——这一行就是为了让那种状态
  变成可见事实。
- **状态行改为「状态灯」色块条**：一个 `SS_OWNERDRAW` 控件里画五个无边框直角色块，
  最左是主状态，右边依次是 `钩` / `学` / `名` / `志`。高度与右侧「版本说明」按钮一致
  （`y=5 h=11`），没有圆角也没有描边。
  - **静止**：五个正方形。底色只有 20% 亮度（灯灭），方块里一个字用 45% 白——淡到不
    抢眼，但看得出是哪一块。
  - **悬停**：那一块宽度与亮度一起渐变，展开到放得下完整文字（`钩子 正常`），其余四块
    收回正方形；移开全部收起。
  - **动画**：宽度和亮度都是「每帧走完剩余距离的 14%」，不是定时插值。指针快速划过时
    从当前位置改道，既不重启动画也不抖动；2 px 的块间隙不属于任何人，扫过时自然收起。
  - 主块文字随状态变：`鼠标防护中`（绿）或 `鼠标防护异常`（红）。
  - 五个块放在**一个控件**里而不是五个：动画每帧都改宽度，五个窗口就是每帧五次
    `SetWindowPos`，而它们之间一像素的偏差看起来就是「永远对不齐」。
  - **实现坑**：`SS_OWNERDRAW` 的静态控件默认对鼠标透明（`WM_NCHITTEST` 返回
    `HTTRANSPARENT`），不加 `SS_NOTIFY` 永远收不到 `WM_MOUSEMOVE`，所有块会一直不动。
  The status row is a strip of five square status-light blocks in a single
  owner-draw control: dim lamp-like squares at rest, and the block under the
  pointer fades brighter and grows until its caption fits. Width and brightness
  approach their target by a fixed fraction per frame, so a pointer sweeping
  across the strip re-targets mid-animation instead of restarting.
- **修复状态行每 400 ms 闪一下**：这一行原来无条件 `SetDlgItemTextW`，即使文字没变也让静态
  控件擦除并重绘整行。现在只在文本真正变化时写入。这是那个"隔几秒闪一下"的根因。
  The status line was rewritten every 400 ms whether or not it had changed,
  which made the static control repaint itself four times a second.

### 修复 / Fixed

- **打开托盘右键菜单会闪退** —— 自学习默认关闭，服务从未启动，但菜单仍去读取它，
  在一个从未初始化的临界区上进入，访问冲突（`0xC0000005`, `ntdll`）。
  现在服务区分「从未初始化」与「已停止」：临界区只初始化一次、永不删除，
  未启动时读取直接返回空建议。退出与恢复默认路径同样加固。
  Opening the tray menu could crash when self-learning had never been enabled;
  the service is now safe to read, stop and reset before it was ever started.
- **体检窗口开着时，点「添加程序」像没反应** —— 文件对话框确实弹了，但它和体检窗口
  同属隐藏主窗口的子级，被压在体检窗口下面。现在所有对话框优先以可见窗口为 owner，
  对话框居中在体检窗口上，并把体检窗口置为模态。
  The file dialog used to open *behind* the check-up window because both were owned
  by the same hidden tray window; dialogs now prefer the visible window as owner.
- **改了名单却要切换窗口才生效** —— 前台轮询按 pid 短路，同一个窗口内编辑名单不会
  重新判定，用户加完程序回到原窗口发现完全没变。现在按「pid + 策略版本号」判断，
  任何名单或模式改动都会立即作废缓存；轮询周期也从 2 秒收紧到 300 毫秒。
  Editing a list took effect only after switching to another window and back; the
  polling thread now also watches a policy revision, so changes apply immediately.
- **「采用全部学习建议」在样本分散时点了没反应** —— 可用状态按三个键的样本**总和**
  判断，总和够而单个键都不够时，入口可点却没有任何建议可加。改为「至少一个键有建议」。
  The adopt entry was enabled by the total sample count instead of per-button
  readiness, so it could be clickable while every button was still below the minimum.
- **子菜单的「(未启用)」只在全部程序模式下才出现** —— 白名单生效时，黑名单子菜单
  看起来仍然启用。现在按「该模式是否就是当前模式」标注。
  A submenu was marked inactive only in "all programs" mode, so the blacklist looked
  active while the whitelist was the one in force.
- **自学习一直显示「样本不足」** —— 三个问题叠在一起：
  ① 采样只记「已经被阈值拦下」的按下。抖动间隔比当前阈值长时（例如开关抖 60 ms、
     阈值 50 ms）永远学不到，而这种情况恰恰最需要建议。
  ② 学习线程等「空闲 300 ms」才重算。用户连续抖动测试时这个空闲永远不会到来，
     样本在涨但菜单一直不刷新，看起来就是零。
  ③ 学习关闭时也显示「样本不足」，让人以为正在等样本。
  现在采样改用固定的 64 ms 窗口，与当前阈值无关；改成样本一增长就重算；
  关闭时显示「开启后开始采样」，开启时显示「样本：左 N / 右 N / 中 N（需 32）」。
  `GetTickCount64` 的粒度约 15.6 ms，间隔 0（同一时钟刻内的两次按下）是最确定的
  抖动，现在也计入样本。
  Self-learning kept reporting "not enough samples": it only recorded presses the
  threshold had already rejected (so chatter wider than the threshold could never be
  learned), it waited for a 300 ms quiet window that continuous testing never
  produced, and it said "not enough samples" even when sampling was off.

### 新增 / Added

- **一键规则当前程序**：名单子菜单里直接出现「不拦截 chrome.exe」/「只对 chrome.exe 生效」，
  不用再去文件对话框里找 exe。若当前是「全部程序」模式，点击后会自动切到对应模式——
  否则配置出来的规则不会生效。
  One-click rule for the program that was just in front; the mode follows the click.
- **空白名单会明说后果**：白名单模式且名单为空时，子菜单显示「名单为空：所有程序都不拦截」。
- **暂停状态可见可撤销**：暂停后菜单项变成「恢复过滤（已暂停）」，再点一次立即恢复。
- **采用学习建议后给出反馈**：托盘气泡显示「已采用学习建议：左 40 ms, 右 55 ms」，
  同时清空产生这些建议的样本，避免同一建议反复出现。
- **合成建议行显示样本数**：`左 50 → 40 ms（32 样本）`，用户能判断可信度。

### 验证 / Verified

Debug / Release × x64 / x86 全量构建零告警（含 `/analyze`）；单测 **3327** 条通过
（x64 与 x86 各一次）；`audit_printf.py` 164 个调用点无问题；`e2e` / `diagnose` /
`stuck_key` 各 0 failure。正式版 exe 仍不含 `MouseMend-trace` 字符串。
真机截图验证三种状态：五个收起方块、主块展开「鼠标防护中」、钩子块展开「钩子 正常」，
其余四块保持正方形。

## 2.3.0 — 2026-11-26

### 修复 / Fixed

- **"按键粘住鼠标"的真正原因（这次的主问题）** —— 智能拖拽扣住一次松手之后，
  如果紧接着又来一个"按下"，旧代码会把它吞掉，再连同松手一起补发 **UP + DOWN**。
  对抖动的开关来说，那个"按下"就是抖动本身；一旦它后面没有对应的松开，系统就被
  **重新按住**，鼠标一动窗口就跟着走，直到再点一次才解开。问题机器上的日志证实了
  这一点：每一次扣留后面都跟着一对注入的 UP + DOWN。
  现在**重放已删除**：扣留期间到来的按下直接拦下，定时器只补发一个 UP，
  没有任何路径会替用户按下按键。
- **被扣住的松手可能永远丢失** —— 补发定时器空闲时会把自已关掉，却没有同时告诉
  引擎"不能再扣留"。任何超过 15 ms 的长按拖拽都会踩中：松手被吞、没人补发、
  左键永久按下。现在定时器自停会一并关闭扣留能力，松手判定之前也会重新武装定时器。
- **普通鼠标的松手不再被延迟** —— 扣留延迟默认从 20 ms 改为 **0（关闭）**。实测：
  一颗在松手瞬间抖动的开关，每次拖拽都要多跟 22–79 ms（补发走 `WM_TIMER`，
  鼠标持续移动时会被饿住）；而扣留对"抖动只发生在松手瞬间"的开关买不到任何东西。
  A release is never held back unless the user asks for it: the default is now 0,
  the replay of the press is gone, and a held release can no longer be lost.

### 变更 / Changed

- 菜单：松手延迟新增 **关闭** 档（关闭 / 20 / 40 / 80）。
- 迁移：注册表里的 `50` 与 `20` 都按"旧默认值"处理，落到 0；`40` / `80` 保持不变。

### 新增 / Added

- **自测版独立成子项目** `tools/MouseMendTrace`：与正式版编译**同一套源码**，只多一个
  `MOUSEMEND_TRACE` 宏，输出到 `tools/bin/`。正式版里不含任何追踪代码（已用字符串
  检查守住：`bin` 里搜不到 `MouseMend-trace`）。**自测版只用于内部复现，不对外发布。**
  记录内容：每个按键事件与判定、鼠标轨迹（10 ms 采样，带前台窗口矩形与捕获窗口）、
  我们自己的每一次注入、钩子与定时器的生命周期。

### 验证 / Verified

Debug / Release × x64 / x86 全量构建零告警（含 `/analyze`）；单测 2756 条通过；
`e2e` 与 `diagnose` 各 0 failure。自测版在问题机器上复核：44 个按键事件全部放行、
0 次扣留、0 次注入，松手后窗口不再跟随。

---

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

- **旧的 50 ms 会被一次性迁到 20 ms。** 2.2.2 之前每次保存都会把 `ReleaseDelay`
  写进注册表，所以存着 50 的机器基本都是"旧默认值"，不是用户的主动选择；而它正是
  拖完窗口还要多跟 50 ms 的原因。启动时读到 50 就按新默认处理，40 / 80 保持不动
  （那两个只能是用户自己选的）。改在菜单里改，随时可以再改回去。
  A stored 50 ms is migrated to 20 ms once: the old build wrote the default on
  every save, so 50 says nothing about intent. 40 and 80 are left alone.

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
