# 发布手册：从改完代码到 GitHub Release

> 这份文档是为「下次自己或别人接手时不再踩同样的坑」写的。
> 2.3.0 和 2.4.1 两次发布都踩过下面的坑，每条都注明了现象和原因。

---

## 0. 先理解一件事：这台机器不能用 `git push`

`github.com` 的 **git 传输被网络屏蔽**，但 `api.github.com` 正常。

所以本项目的发布**不走 `git push`**，而是走：

```
doc/push-to-github.ps1
```

它做的事是：把**工作区里的每一个被 git 跟踪的文件**逐个上传成一个 blob，
在服务端拼出 tree 和 commit，然后把 `refs/heads/main` 移过去。

**推论（很重要）**：

- 脚本读的是 **git 索引 + 磁盘**，不是你的本地提交历史。
- 所以远端的提交是服务端新拼的，**sha 和本地任何一个提交都不相同**。
- 因此**本地 tag 永远对不上远端**，必须单独修改远端 tag（见第七步）。

---

## 1. 环境前提

### 1.1 凭据

脚本第一件事是取 GitHub 令牌：

```powershell
$raw = "protocol=https`nhost=github.com`n`n" | & git credential fill 2>$null
$token = ($raw | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
```

**这条命令内联在命令行里跑，会返回空令牌。写成 `.ps1` 文件跑才正常。**

- 2.3.0 踩过，2.4.1 又踩了一次（以为凭据丢了，其实只是调用方式不对）。
- 令牌来自 Visual Studio 的 `git credential manager`（`credential.helper = manager`）。
- 如果确实没有：打开 Visual Studio 登录一次 GitHub，或 `git credential approve` 手工写入。

### 1.2 网络

- `api.github.com` 和 `uploads.github.com` 可通。
- `github.com`（HTTPS git 传输）不可通 —— 所以 `git push` 必然失败，不要试。

---

## 2. 完整流程

按顺序做，**不要跳步**。

### 第一步：升版本号（5 个地方，漏一个就会被测试挡住）

| 文件 | 改什么 |
|---|---|
| `MouseMend/version.h` | `APP_VERSION_PATCH`、`APP_VERSION_STRING`、`APP_VERSION_STRING_W` **三处** |
| `MouseMend/MouseMend.rc` | **不用改**，它用宏，编译时自动取版本 |
| `MouseMend/CHANGELOG.md` | 顶部新增 `## x.y.z — 日期` 段 |
| `MouseMend/README.md` | 两处：徽章 `version-x.y.z`、页脚两行 |
| `README.md`（仓库根） | 徽章；**`Latest release` 那一行先不要改**（见第七步） |
| `MouseMend/src/ui/language.c` | `STR_VERSION_NOTES_TEXT` 中英各加一条 |

> **为什么根 README 的 Latest release 先不改**：先改会指向一个还不存在的 tag，页面上是 404。
> 发布成功后再改，然后**再发布一次**（见第七步）。

> **`tests/test_core.c` 里有一条版本一致性断言**：`APP_VERSION_STRING` 和 `_W` 拼写不一致会直接挂。

### 第二步：构建与验证（四条腿，缺一不可）

```powershell
# 1. 四配置 Rebuild，带静态分析
msbuild MouseMend.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64  /p:RunCodeAnalysis=true /v:minimal
msbuild MouseMend.sln /t:Rebuild /p:Configuration=Release /p:Platform=x86  /p:RunCodeAnalysis=true /v:minimal
msbuild MouseMend.sln /t:Rebuild /p:Configuration=Debug   /p:Platform=x64  /p:RunCodeAnalysis=true /v:minimal
msbuild MouseMend.sln /t:Rebuild /p:Configuration=Debug   /p:Platform=x86  /p:RunCodeAnalysis=true /v:minimal
```

要求 **0 error 0 warning**。

```powershell
# 2. 单测：两个平台都要跑
& 'tests\bin\x64\Release\MouseMendTests.exe'
& 'tests\bin\Win32\Release\MouseMendTests.exe'     # x86 的产物目录叫 Win32，不叫 x86
```

```powershell
# 3. 格式串审计（改了任何 STR_*_FMT 都必须跑）
python tests\tools\audit_printf.py
```

```powershell
# 4. 三个冒烟测试
pwsh -File tests\smoke\e2e.ps1
pwsh -File tests\smoke\diagnose.ps1
pwsh -File tests\smoke\stuck_key.ps1 -Force      # 会话锁定时会 SKIP，重试一次即可
```

最后确认 exe 的资源版本真的变了：

```powershell
(Get-Item 'MouseMend\bin\x64\Release\MouseMend-x64.exe').VersionInfo | Select ProductVersion,FileVersion
```

### 第三步：提交代码

```powershell
cd E:\TEST\Mouse
git add <改动过的文件>            # 精确列出，不要 git add -A
git commit -m "Release x.y.z: 一句话" -m "详细说明……"
```

**建议**：commit message 用「一句话标题 + 若干段正文」，说明**为什么改**而不是改了什么。
发布脚本的 `-Message` 可以复用同一句标题。

### 第四步：提交 exe（可选，但每次都被问）

```powershell
git add -f MouseMend/bin/x64/Release/MouseMend-x64.exe
git add -f MouseMend/bin/x86/Release/MouseMend-x86.exe
git commit -m "Ship the x.y.z executables"
```

- `.gitignore` 里有 `bin/` 和 `*.exe`，所以**必须 `-f`**。
- 目的：让「发布出去的源码」和「用户下载的 exe」在历史里能对上号。

### 第五步：撤出 exe（**必须做，否则第六步一定失败**）

```powershell
git rm --cached MouseMend/bin/x64/Release/MouseMend-x64.exe
git rm --cached MouseMend/bin/x86/Release/MouseMend-x86.exe
git commit -m "Stop tracking the executables"
```

- `--cached` 只动索引，**磁盘上的 exe 还在**，后面上传附件要用。
- 发布脚本第 117-133 行会检查索引：

```powershell
if (($path -match '\.(jpg|jpeg|gif|bmp|exe|pfx|txt)$') -or ($path -match '(^|/)bin/')) {
    throw 'unwanted files are tracked'
}
```

只要 `.exe` 或 `bin/` 还在索引里，**整个发布直接中止**。

### 第六步：发布代码

先干跑，看清计划：

```powershell
cd MouseMend
pwsh -File doc\push-to-github.ps1 -Message "Release x.y.z: ..." -DryRun
```

输出里会看到根 tree 的 sha 和 `parents: <远端当前 main>`。
**确认 parent 是远端 main**（不是别的分支）再正式跑：

```powershell
pwsh -File doc\push-to-github.ps1 -Message "Release x.y.z: ..."
```

成功的标志是最后三行：

```
refs/heads/main -> <新 sha>
  blobs on remote: N
  expected       : N
  remote matches the working tree exactly
```

**把那个新 sha 记下来**，下一步要用。

### 第七步：改落地页，再发布一次

```powershell
# 根 README.md：
# Latest release / 最新版本：**[vX.Y.Z](https://github.com/.../releases/tag/vX.Y.Z)**

git add README.md
git commit -m "Point the landing page at x.y.z"
pwsh -File doc\push-to-github.ps1 -Message "Point the landing page at x.y.z"
```

> 这一步很容易被忘掉，忘了的结果是：Release 页是新的，首页还写着上个版本。
> 2.3.0 和 2.4.1 都是**发布完才发现**，然后补了一次。

### 第八步：把远端 tag 指过去（**最容易漏的一步**）

本地打的 tag 指向本地提交，而远端那个提交根本不存在。

```powershell
# 取远端 main 的 sha，然后强制改 tag
PATCH https://api.github.com/repos/<owner>/<repo>/git/refs/tags/vX.Y.Z
{ "sha": "<远端 main sha>", "force": true }
```

正确做法（PowerShell）：

```powershell
$main = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/git/ref/heads/main" -Headers $headers
Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/git/refs/tags/v$ver" -Method Patch `
    -Headers $headers -ContentType 'application/json' `
    -Body (@{ sha = $main.object.sha; force = $true } | ConvertTo-Json -Compress)
```

如果 tag 还不存在，用 `POST /git/refs` 新建。

### 第九步：建 Release 并上传 exe

```powershell
# 建 Release（tag_name 必须是已经存在的 tag）
POST https://api.github.com/repos/<owner>/<repo>/releases
{
  "tag_name": "vX.Y.Z",
  "name": "MouseMend X.Y.Z",
  "target_commitish": "main",
  "body": "<发布说明，可复用 CHANGELOG 段落>"
}
```

上传附件（**注意是 `uploads.github.com`，不是 `api.github.com`**）：

```powershell
POST https://uploads.github.com/repos/<owner>/<repo>/releases/<release_id>/assets?name=MouseMend-x64.exe
Content-Type: application/octet-stream
Body: <exe 的原始字节>
```

用 `Invoke-RestMethod -InFile <exe 路径>`，不要手动读成字符串。

### 第十步：核对

```powershell
$main = GET /repos/<owner>/<repo>/git/ref/heads/main
$tag  = GET /repos/<owner>/<repo>/git/ref/tags/vX.Y.Z
$rel  = GET /repos/<owner>/<repo>/releases/tags/vX.Y.Z
```

要求三条同时成立：

1. `main.object.sha == tag.object.sha`
2. `release.draft == false`、`prerelease == false`
3. `release.assets` 里两个 exe，大小与本地一致

---

## 3. 每个坑的「现象 → 原因 → 处理」

| 现象 | 原因 | 处理 |
|---|---|---|
| 脚本报 `No GitHub credential stored` | `git credential fill` 内联在命令行里跑，拿不到令牌 | 把取令牌的代码写进 `.ps1` 文件再跑 |
| 脚本报 `unwanted files are tracked` | `.exe` 或 `bin/` 还在 git 索引里 | 先 `git rm --cached` 再发布 |
| Release 页是新的，首页还是旧版本 | 根 README 的 `Latest release` 没改，或改了没再发布 | 改 README → 再跑一次脚本 |
| tag 指向一个远端不存在的提交 | 发布脚本在服务端拼 commit，本地提交不会上传 | `PATCH /git/refs/tags/<tag>` 指向远端 main sha |
| `git push` 卡住 / 超时 | 这台机器的 github.com git 传输被屏蔽 | 永远用 `push-to-github.ps1`，不要试 push |
| 附件上传 401 | 用了 `api.github.com` 而不是 `uploads.github.com` | 换域名，`Content-Type: application/octet-stream` |
| 单测版本一致性断言挂 | `version.h` 里宽窄两个字符串只改了一个 | 三处一起改 |
| x86 单测找不到文件 | 产物在 `tests\bin\Win32\Release\` | 路径写 Win32 |
| `stuck_key.ps1` 输出 SKIP | 会话锁定，不是失败 | 解锁后重跑一次 |

---

## 4. 发布前检查清单

复制到 issue 或笔记里逐条打勾：

```
[ ] version.h 三处（MAJOR/MINOR/PATCH、STRING、STRING_W）
[ ] CHANGELOG 新增段
[ ] MouseMend/README.md 徽章 + 页脚（两行）
[ ] 版本说明文案中英各一条
[ ] 四配置 Rebuild：0 error 0 warning
[ ] 单测 x64：N/0
[ ] 单测 x86：N/0
[ ] audit_printf：0 问题
[ ] e2e / diagnose / stuck_key：各 0 failure
[ ] exe ProductVersion 已是新版本
[ ] 提交代码
[ ] exe 入库提交
[ ] exe 撤出索引
[ ] DryRun 通过，parent 是远端 main
[ ] 正式发布，118 个 blob 一致
[ ] 根 README 改 Latest release
[ ] 再发布一次
[ ] PATCH tag 到远端 main
[ ] 建 Release + 上传两个 exe
[ ] 核对 main == tag、release 非 draft、两个 asset 大小正确
```

---

## 5. 命令速查

```powershell
# 当前状态
git --no-pager status --short
git --no-pager log --oneline -5
git tag --points-at HEAD

# 远端状态（脚本里的 API 地址）
GET https://api.github.com/repos/DaYao202025/MouseMend/git/ref/heads/main
GET https://api.github.com/repos/DaYao202025/MouseMend/releases/tags/vX.Y.Z

# 发布
cd MouseMend
pwsh -File doc\push-to-github.ps1 -Message "..." -DryRun
pwsh -File doc\push-to-github.ps1 -Message "..."
```

---

## 6. 为什么不把 exe 直接放进代码树

三个理由，按重要性排：

1. **发布脚本会拒绝**。索引里有 `.exe` 或 `bin/`，脚本直接 `throw`，整个发布中止。
2. **仓库会被撑大，且删不掉**。二进制进了历史就永久占空间，`git rm` 也只是加一个删除提交。
3. **分发方式不对**。用户要的是「下载最新版」，Release 附件正好，而且带下载计数。

所以最终形态是：**历史里留一次（证明构建产物与源码同源），索引里撤掉，实际分发走 Release 附件**。

---

## 7. 我自己补的两条建议

**一、发布前一定先 `-DryRun`。**
它会打印根 tree 的 sha 和 `parents:`。看一眼 parent 是不是远端 main，
能提前发现「本地分支和远端分叉」这类问题，比发布到一半失败好得多。

**二、把发布后的核对当成流程的一部分，不要当成"顺手"。**
`main == tag` 这一条最容易漏，而漏了的后果是：用户点 tag 看到的是旧代码，
Release 里的 exe 却是新的 —— 这种不一致极难从界面上发现。
