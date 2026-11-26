# 推送到云端 GitHub —— 运维手册

> 这份文档解决一个问题：**在这台机器上 `git push` 用不了**，但仓库必须能持续更新到 GitHub。
> 里面记的是已经实际跑通的步骤、踩过的坑，以及下次怎么做能一次成功。

---

## 一、现状（2026-11-22 建立）

| 项目 | 内容 |
|---|---|
| 仓库地址 | https://github.com/DaYao202025/MouseMend |
| 可见性 | Public |
| 默认分支 | `main` |
| 许可证 | MIT（GitHub 已识别） |
| 远程提交数 | **2**：`ab157c9` 根提交（发布基线）+ `01fed38`（补根 README 与本文档） |
| 远程文件数 | 75 |
| 本地仓库根 | `E:\TEST\Mouse`（项目在 `MouseMend\` 子目录） |
| 本地分支 | 只有 `main`，含完整开发历史 |

**远程历史为什么从一条开始**：这个项目源自另一个仓库，本地历史里有原作者和另一位贡献者的姓名与邮箱。公开发布时按需求压平成一个根提交，那些身份信息就不会出现在 GitHub 上。之后每次发布在**已有的 head 之上追加**，历史正常生长，但永远不会接回本地那条老历史。

---

## 二、核心限制：`git push` 在这台机器上不通

实测（2026-11-22）：

| 目标 | 结果 |
|---|---|
| `github.com:443`（git 传输走这个域名） | **连接被重置 / 21 秒超时** |
| `api.github.com:443`（REST API） | **200 正常** |

所以：

- ❌ `git push` → `fatal: unable to access ... Recv failure: Connection was reset`
- ❌ `git ls-remote https://github.com/...` → 同样失败
- ✅ `api.github.com` 的所有调用正常

**结论：发布必须走 REST API，不能走 git 协议。** 不要浪费时间重试 `git push`，也不要去配代理（用户明确不开代理）。

顺带一个容易误判的点：`git ls-remote` 对**不存在的仓库**返回 404。**这个返回值不能用来判断有没有推送权限**（GitHub 对"不存在"和"无权限"都回 404）。要判断权限只能查 token 的 scopes。

---

## 三、日常发布：三条命令

前提：**工作区干净**（`git status` 无改动）。脚本会检查这一点，因为它发布的是磁盘上的内容。

```powershell
# 1. 在 main 上正常提交（本地历史照常增长）
cd E:\TEST\Mouse
git add -A -- ':!*.png' ':!*.txt'
git commit -m "你的提交说明"

# 2. 发布到云端
pwsh -File MouseMend\doc\push-to-github.ps1 -Message "v2.0.1: ..."

# 3. 想先看会传哪些文件而不真正发布
pwsh -File MouseMend\doc\push-to-github.ps1 -Message "..." -DryRun
```

脚本会自动完成：读凭证 → 收集已跟踪文件 → 逐个上传 → 建树 → 建提交 → 移动分支 → 回读校验。**结束时会把"远程文件数 / 期望文件数"和差异打出来**，看到 `remote matches the working tree exactly` 才算成功。

### 术语约定

| 用户说 | 含义 | 做什么 |
|---|---|---|
| **推送到云端 Git / 云端 GitHub** | 发到 GitHub | 跑上面第 2 步 |
| **推送到本地 Git / 提交一下** | 只在本地提交 | 只跑第 1 步，不动远程 |

---

## 四、脚本内部做了什么

GitHub 的 Git Data API 是"手动挡"的：它不接收文件，只接收**对象**。

```
1. 凭证          git credential fill        → 取 password= 那一行（绝不打印）
2. 文件清单      git ls-files               → .gitignore 自动决定发布范围
3. 行尾校验      git hash-object vs index   → 保证磁盘内容 == git 对象内容
4. 上传文件      POST /git/blobs            → 每个文件一个 base64 blob，得到 sha
5. 建目录树      POST /git/trees            → 自底向上，每个目录一次
6. 建提交        POST /git/commits          → tree + parents
7. 移动分支      PATCH /git/refs/heads/main → 或首次用 POST /git/refs
8. 回读校验      GET  /git/trees/main?recursive=1 → 比对文件集
```

几个必须知道的**硬约束**：

- **`POST /git/trees` 的 `path` 不能包含 `/`**。必须按目录逐层建树，把子树的 sha 当作 `mode: 040000, type: tree` 的条目挂上去。一次性传 `src/core/foo.c` 会被拒绝。
- **空仓库调不通 Git Data API**。一个提交都没有的仓库，`POST /git/blobs` 直接返回 **409 Conflict**（"Git Repository is empty"）。详见下一节的坑 2。
- **`PATCH /git/refs/heads/<branch>` 带 `force: false`** 是快进更新；首次建分支要用 `POST /git/refs`。

---

## 五、踩过的坑（按踩到的顺序）

### 坑 1：`git push` 直接失败

```
fatal: unable to access 'https://github.com/DaYao202025/MouseMend.git/':
Recv failure: Connection was reset
```

不是权限问题，也不是凭证问题——`gh` 未安装、`GIT_TERMINAL_PROMPT=0` 也不影响结论。**是域名不通**。

**正确做法**：直接走 API。不要试 `HTTP/1.1`、不要试 `ssh://`（22 端口同样不通）、不要配代理。

### 坑 2：空仓库不能上传 blob（409 Conflict）

第一次用 API 往全新空仓库传文件：

```
POST /repos/.../git/blobs  →  409  "Git Repository is empty."
```

GitHub 要求仓库**至少有一个提交**，Git Data API 才肯工作。

**解法（首次建仓时用过一次）**：

1. 先用 Contents API 建一个临时文件把仓库"点亮"：
   `PUT /repos/{owner}/{repo}/contents/.release-seed`，body 带 `message` 和 base64 的 `content`
2. 之后正常走 blob → tree → commit
3. 提交时 **`parents: []`**（根提交）
4. `PATCH /git/refs/heads/main` 带 `force: true`，把分支指到新的根提交

那个临时提交变成**不可达对象**，不显示在历史里。最终 `GET /commits` 只会看到 1 条。

**现在仓库非空，不需要再做这一步。** 只有当仓库被彻底清空重建时才需要。

### 坑 3：临时提交会污染历史

上面第 1 步建的种子提交如果不处理，历史就是 2 条。必须靠第 4 步的 `force: true` 把它顶掉。**验证方法**：`GET /commits` 返回 1 条且 `parents` 为空。

### 坑 4：仓库根没有 README，GitHub 首页一片空白

项目结构是"仓库根 + `MouseMend\` 子目录"，完整文档在 `MouseMend\README.md`。但 **GitHub 只渲染仓库根目录的 `README.md`**，子目录里的不会被当成首页内容。

**解法**：仓库根单独放一份精简 README，用相对链接指向子目录的完整文档：

```markdown
[**Full documentation / 完整文档**](MouseMend/README.md)
```

**维护规则**：根 README 只放徽章、双语简介、目录结构和构建命令；详细内容一律写在 `MouseMend/README.md`，避免两处重复维护。

### 坑 5：`Invoke-RestMethod` 解 base64 内容会失败

`GET /contents/{path}` 返回的 `content` 是**带换行**的 base64。直接 `[Convert]::FromBase64String($content)` 会抛异常。

**解法**：要么先 `-replace "\`n", ''`，要么干脆用 raw 媒体类型：

```powershell
$headers['Accept'] = 'application/vnd.github.raw'
$text = Invoke-RestMethod "$api/contents/MouseMend/README.md" -Headers $headers
```

### 坑 6：`core.autocrlf=true` 会让"发布内容"和"仓库内容"不一致

本机 `git config core.autocrlf` 是 `true`，`.gitattributes` 又是 `* text=auto`，
所以：提交时 CRLF 被规范化成 LF 存进仓库，**checkout 时又变回 CRLF 写进磁盘**。

脚本读的是**磁盘字节**，所以某次 checkout 之后，发布的源文件就是 CRLF 版本，
而仓库里存的是 LF —— 两者内容一致、行尾不同。

脚本的处理方式：

- 校验用 `git hash-object --path=<path>`，**应用和 `git add` 相同的属性**再比哈希，
  这样 CRLF 的工作副本仍会算出 LF 的 blob 哈希，不会被误报成"文件不一致"。
  （如果直接用 `git hash-object -- <file>`，会把 CRLF 算成新哈希，然后误判）
- 如果检测到磁盘上是 CRLF，会打 `note: N file(s) have CRLF on disk` 提醒。

**想彻底避免**：

```powershell
cd E:\TEST\Mouse
git config core.autocrlf false
git rm --cached -r . ; git reset --hard
```

项目约定是 LF（`.c`/`.h`/`.md`/`.ps1` 全部无 BOM + LF）。这不影响编译，只影响"发布出去的样子"。

### 坑 7：推送成功后网页还是旧的

推完立刻刷新网页，可能仍然看到"1 Commit"、根目录没有 `README.md`——**这不是失败，是 CDN 缓存**。

同一个 URL 用命令行抓取，拿到的可能是几分钟前的快照。判断推送是否真的成功，**只信 API**：

```powershell
$head = (Invoke-RestMethod "https://api.github.com/repos/DaYao202025/MouseMend/git/ref/heads/main" -Headers $h).object.sha
$tree = Invoke-RestMethod "https://api.github.com/repos/DaYao202025/MouseMend/git/trees/$head`?recursive=1" -Headers $h
($tree.tree | Where-Object { $_.type -eq 'blob' }).Count
```

如果这里显示 75 个文件、并且 `README.md` 在列表里，那就是成功了。网页通常几分钟后跟上，实在不行强制刷新（`Ctrl+F5`）。

**不要因为网页没变就重复推送**——脚本是幂等的（每次重建整棵 tree），重复推只会多一个空提交。

### 坑 9：README 里的图片显示成破图

现象：仓库里的 PNG 明明存在、大小也对，网页上却只显示 alt 文字（"鼠标体检窗口（中文）"之类），三个都破。

**先分清两种原因，不要急着改文件：**

1. **文件真的坏了** —— 用 API 取回来跟本地比对字节：
   ```powershell
   $h = @{ Authorization = "Bearer $token"; 'User-Agent' = 'x'; 'Accept' = 'application/vnd.github.raw' }
   $remote = Invoke-WebRequest "https://api.github.com/repos/DaYao202025/MouseMend/contents/MouseMend/doc/screenshots/checkup-zh.png" -Headers $h
   ```
   比 SHA-256 + 看前 4 字节是不是 `89 50 4E 47`。本项目实测：**三张全部与本地一致**，文件没问题。

2. **文件没坏，是取不到** —— GitHub 对 README 里的**相对路径**图片，是从 `raw.githubusercontent.com` 提供的。这个域名在部分网络下 **DNS 直接解析失败**（本机实测 `DNS FAILED`，而 `github.com` / `api.github.com` / `camo.githubusercontent.com` / `cdn.jsdelivr.net` 都能解析）。于是页面能打开、图全是破的。

**对策（本项目采用）**：README 里的 `<img>` 改用 jsDelivr 直链，它从同一个提交里取同样的字节：

```
https://cdn.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/checkup-zh.png
```

实测三张都在 jsDelivr 上返回 200 且 SHA-256 与本地一致，本机直接可访问。

**这个选择的代价，记清楚：**
- 写死了仓库名 `DaYao202025/MouseMend`。**仓库改名或转移后，这两个 README 里各 6 处 URL 必须同步改**，否则图全破。
- 依赖第三方 CDN。jsDelivr 对 `@main` 有约 12 小时缓存，换了截图要等缓存过期（或换个文件名）。
- 别人 fork 出去，图仍然指向本仓库。

**选哪个 host 也有讲究（2026-11-23 实测）：**

| host | 结果 |
| :--- | :--- |
| `cdn.jsdelivr.net` | **连接被重置**，连试 5 次全失败（TLS 握手阶段 `Recv failure: Connection was reset`） |
| `fastly.jsdelivr.net` | 200，6 张图 SHA-256 全部与仓库一致 ← **当前采用** |
| `gcore.jsdelivr.net` | 200，6 张全部一致 |
| `testingcf.jsdelivr.net` | 200，但其中一张还返回旧缓存 |

所以 README 里用的是 **`fastly.jsdelivr.net`**，不是官方文档默认的 `cdn.`。**如果哪天 fastly 也被重置，改成 `gcore.` 或 `testingcf.` 即可**（三者内容相同，只是边缘节点不同）。

**换图之后必须清缓存**，否则 `cdn`/`testingcf` 会继续返回旧字节：

```powershell
Invoke-WebRequest "https://purge.jsdelivr.net/gh/DaYao202025/MouseMend@main/MouseMend/doc/screenshots/checkup-zh.png" -UseBasicParsing
```

实测：purge 之后 `fastly` 与 `gcore` 立即变成新字节，`testingcf` 仍滞后。**核验方式只能是逐个 host 取回来比 SHA-256**，不能只看 HTTP 200。

**如果哪天不需要照顾受限网络了**，改回相对路径即可（`MouseMend/doc/screenshots/xxx.png` / `doc/screenshots/xxx.png`）。

### 坑 8：代码页垃圾输出

`powershell.exe` 5.1 的管道按 ANSI 解码，中文会变乱码。**统一用 `pwsh`**（PowerShell 7），并且把脚本写成文件用 `-File` 调用，不要用 `-Command`（`$` 会被吞掉）。

---

## 六、注意事项清单

**发布前**

- [ ] `git status` 干净。脚本会拒绝发布脏工作区，因为它读的是磁盘内容。
- [ ] 没有误加文件。`git add -A` 要配 `':!*.png' ':!*.txt'` 排除两个杂物（`QQ20261121-200950.png`、`新建文本文档.txt`）。脚本会二次拦截 `png/jpg/txt/exe/pfx/.vs/bin`。
- [ ] **`MouseMend\.codebuddy\` 永远不能发布**。里面有工作记忆，含个人邮箱等信息。已由 `.gitignore` 排除，`git ls-files` 不会包含它。
- [ ] 改过 `.gitattributes` 或换行设置后，先跑 `-DryRun`，看行尾校验有没有报警。

**发布中**

- [ ] 每个 blob 都是**独立的一次 HTTP 请求**，76 个文件就是 76 次。网络慢的时候会跑一两分钟，属正常。
- [ ] 脚本对 5xx 和 429 自动重试（间隔 2/4/6 秒），对 4xx 立即失败——4xx 重试没有意义。
- [ ] token 只在内存里，**不要 echo、不要写进文件、不要提交**。

**发布后**

- [ ] 看脚本最后输出的 `remote matches the working tree exactly`。有 `MISSING` 或 `EXTRA` 就是没对齐。
- [ ] 到网页确认：首页 README 有内容、About 描述在、Topics 在、`1 Commit`（或递增）。
- [ ] 如果改了许可证或新增依赖，同步更新根 README 与 `MouseMend/README.md`。

**绝对不要做**

- ❌ 不要 `git push origin main` 去推那个指向原作者的 `origin`（推不上去，且没必要）。
- ❌ 不要为了"能 push"去改 git 远程为 `ssh://`——22 端口一样不通。
- ❌ 不要把本地 `main` 的完整历史推上去。远程历史必须从那个根提交开始，一旦推了带原作者的历史，就再也收不回来了。
- ❌ 不要在 `-Message` 里写敏感信息（提交信息会永久公开）。

---

## 七、故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `No GitHub credential stored` | 凭证被清掉了 | 打开 Visual Studio 重新登录 GitHub；或 `git credential approve` 手动写入 |
| `Working tree is not clean` | 有未提交改动 | 先 commit；或确认改动就该发布后再跑 |
| `line-ending mismatch` | `.gitattributes` 会改写行尾 | 先 `git add --renormalize .` 再提交，避免发布的源码和编译过的不一致 |
| `refusing to publish` | 有 png/txt/exe 被跟踪 | `git rm --cached` 掉再发布 |
| `409 Git Repository is empty` | 仓库被清空了 | 按坑 2 的步骤，先用 Contents API 建种子提交 |
| `401 / 403` | token 过期或权限不足 | 查 `GET /user` 返回的 `X-OAuth-Scopes` 是否含 `repo`；重新登录 VS |
| `422` 且提到 tree | `path` 里带了 `/` | 必须按目录逐层建树 |
| 远程文件数与本地不一致 | 上次没跑完 | 重跑脚本即可，它是幂等的（每次都重建完整 tree） |
| 网页看不到 README | 根目录没有 `README.md` | 见坑 4 |

---

## 八、账号与权限

- 发布使用 Visual Studio 写入的 GitHub 凭据，**本仓库不保存任何令牌**。
- 具体条目名、令牌类型、权限范围**不写进文档**。这些对正常发布没有帮助，
  但会告诉别人「去哪里找、找到之后能做什么」。早先版本记过这些内容，
  已在本仓库的历史里，因此**凭据应当按已暴露处理并轮换**（见下）。
- 需要确认凭据还有效时，只看调用是否成功，**不要把令牌或 scopes 写进任何文件**：

```powershell
$raw = "protocol=https`nhost=github.com`n`n" | git credential fill
$token = ($raw | Where-Object { $_ -match '^password=' }) -replace '^password=',''
if (-not $token) { 'no credential' }
else {
  (Invoke-WebRequest 'https://api.github.com/user' `
    -Headers @{ Authorization = "Bearer $token"; 'User-Agent' = 'check' }).StatusCode
}
```

- **怀疑凭据泄露时，去 GitHub 设置里吊销并重新授权**，不要只改文档：
  文档能改，已经发布出去的历史改不了。
- **`gh` CLI 未安装**，也不需要安装——脚本只用 `git` + `Invoke-RestMethod`。

---

## 九、想换别的发布方式时

如果哪天网络能直连 `github.com` 了，可以回到最普通的方式：

```powershell
git remote add cloud https://github.com/DaYao202025/MouseMend.git
git push cloud main
```

但要注意：**本地 `main` 的历史和远程的根提交不是同一条线**，直接推会被拒绝（non-fast-forward）。届时要么继续用 API 追加，要么接受把本地历史整体覆盖上去（那会把原作者信息带上去，**不建议**）。

**因此推荐长期沿用 API 方式**，它与网络状况无关，也天然保证远程历史从根提交开始。
