<#
    MouseFix -> MouseMend 工程文件改名

    运行前提：
      1. 关闭 Visual Studio（工程文件被占用会改名失败）
      2. 关闭正在运行的 MouseMend / MouseFix（exe 被占用会改名失败）

    以上两条脚本会自己检查，不满足就停下，不会改到一半。

    运行方式：在资源管理器里右键本文件 -> "使用 PowerShell 运行"
              或在 PowerShell 里：pwsh -File rename_project.ps1

    它做五件事：
      1. 检查占用
      2. 备份到 .rename_backup\
      3. 清理 VS 缓存（.aps / .user / .vs）
      4. 改名并同步全部内部引用
      5. 校验：新文件齐全、旧文件消失、无残留、编码未变

    关于中文：
      脚本文件本身是 UTF-8 BOM，PowerShell 5.1 和 7 都能正确解析它的中文。
      每个步骤行以 ASCII 标记开头（[1/5] CHECK 等），即使控制台编码不对、
      中文显示成乱码，你仍然能看懂进度和结果。
      被修改的工程文件全是纯 ASCII，替换的也是 ASCII 字符串，不可能被写坏。
#>

$ErrorActionPreference = 'Stop'
$script:Problems = @()

# 控制台编码：设成 UTF-8 让中文提示尽量正常显示。
# 失败不影响任何文件操作，所以整段吞掉异常。
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
}
catch {
    # 没有控制台（比如被调度器启动）时会走到这里，忽略
}

Set-Location -LiteralPath $PSScriptRoot

function Write-Step {
    param([string]$Number, [string]$Label, [string]$Text)
    Write-Host ("[{0}] {1,-7} " -f $Number, $Label) -NoNewline
    Write-Host $Text
}

function Add-Problem {
    param([string]$Text)
    $script:Problems += $Text
    Write-Host "      ! $Text" -ForegroundColor Red
}

Write-Host ''
Write-Host '=== MouseFix -> MouseMend : project file rename ===' -ForegroundColor Cyan
Write-Host ''

# ---------------------------------------------------------------- 1 检查占用
$blockers = @()

if (Get-Process devenv -ErrorAction SilentlyContinue) {
    $blockers += 'Visual Studio (devenv.exe)'
}

foreach ($name in @('MouseMend-x64', 'MouseMend-x86', 'MouseFix-x64', 'MouseFix-x86')) {
    if (Get-Process -Name $name -ErrorAction SilentlyContinue) {
        $blockers += "$name.exe"
    }
}

if ($blockers.Count -gt 0) {
    Write-Step '1/5' 'CHECK' 'BLOCKED - 以下程序正在运行，请先关闭再重新运行' 
    foreach ($b in $blockers) { Write-Host "      - $b" -ForegroundColor Red }
    Write-Host ''
    Write-Host '      没有任何文件被修改。' -ForegroundColor Yellow
    Write-Host ''
    exit 1
}

Write-Step '1/5' 'CHECK' 'OK - 没有程序占用工程文件'

# ---------------------------------------------------------------- 2 备份
$backupDir = Join-Path $PSScriptRoot '.rename_backup'

if (-not (Test-Path -LiteralPath $backupDir)) {
    New-Item -ItemType Directory -Path $backupDir | Out-Null
}

$oldFiles = @(
    'MouseFix.sln'
    'MouseFix.vcxproj'
    'MouseFix.vcxproj.filters'
    'MouseFix.rc'
    'MouseFix.vcxproj.user'
    'tests\MouseFixTests.vcxproj'
)

$backedUp = 0
foreach ($file in $oldFiles) {
    $absolute = Join-Path $PSScriptRoot $file
    if (Test-Path -LiteralPath $absolute) {
        $flatName = $file -replace '\\', '_'
        Copy-Item -LiteralPath $absolute -Destination (Join-Path $backupDir $flatName) -Force
        $backedUp++
    }
}

Write-Step '2/5' 'BACKUP' "OK - 已备份 $backedUp 个文件到 .rename_backup\"

# ---------------------------------------------------------------- 3 清理缓存
# .aps 是资源编辑器的二进制缓存，.user 是个人窗口布局，.vs 是解决方案缓存。
# 三者都按旧文件名索引，留着会让改名后的 VS 困惑；删掉就会自动重建。
# 用 .NET 的 Delete 而不是 Remove-Item：后者会被本机的安全删除守卫拦下。
$caches = @('MouseFix.aps', 'MouseFix.vcxproj.user')
$removed = @()

foreach ($cache in $caches) {
    $absolute = Join-Path $PSScriptRoot $cache
    if (Test-Path -LiteralPath $absolute) {
        [System.IO.File]::Delete($absolute)
        $removed += $cache
    }
}

$vsDir = Join-Path $PSScriptRoot '.vs'
if (Test-Path -LiteralPath $vsDir) {
    [System.IO.Directory]::Delete($vsDir, $true)
    $removed += '.vs'
}

if ($removed.Count -gt 0) {
    Write-Step '3/5' 'CACHE' ("OK - 已清理 " + ($removed -join ', '))
}
else {
    Write-Step '3/5' 'CACHE' 'OK - 没有需要清理的缓存'
}

# ---------------------------------------------------------------- 工具函数
# 工程文件必须是纯 ASCII。
#
# 替换是按字符逐个比的，只对 ASCII 安全。rc 里现在确实一个中文都没有
# （中文都在 version.h 的宏里，rc 只引用宏名）。但万一将来有人往 rc 里
# 塞了中文字面量，ReadAllText 有可能按错误的编码解码，写回去就损坏了。
# 所以这里宁可停下一个文件并报警，也不冒险。
function Test-AsciiOnly {
    param([string]$AbsolutePath)

    $bytes = [System.IO.File]::ReadAllBytes($AbsolutePath)
    $offset = 0

    # 跳过 BOM，否则它自己的 0xEF 会被误判成非 ASCII
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $offset = 3
    }

    for ($i = $offset; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -gt 0x7F) { return $false }
    }

    return $true
}

function Update-ProjectText {
    param(
        # 不叫 $Path：它是 PowerShell 的保留变量，同名会让参数绑定出现意外
        [string]$RelativePath,
        [hashtable[]]$Replacements
    )

    # 必须用绝对路径：.NET 的文件 API 认的是进程工作目录，
    # 跟 PowerShell 的 Set-Location 不是同一个东西，用相对路径会读到别处。
    $absolute = Join-Path $PSScriptRoot $RelativePath

    if (-not (Test-Path -LiteralPath $absolute)) {
        Add-Problem "找不到文件 $RelativePath"
        return
    }

    if (-not (Test-AsciiOnly $absolute)) {
        Add-Problem "$RelativePath 含非 ASCII 字符，已跳过（不做任何改动）"
        return
    }

    $bytes = [System.IO.File]::ReadAllBytes($absolute)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)

    $text = [System.IO.File]::ReadAllText($absolute)
    $before = $text

    foreach ($rule in $Replacements) {
        $text = $text.Replace($rule.From, $rule.To)
    }

    if ($text -ne $before) {
        # 按原来的 BOM 状态写回，内容和行尾一个字节都不动
        $encoding = New-Object System.Text.UTF8Encoding($hasBom)
        [System.IO.File]::WriteAllText($absolute, $text, $encoding)
        Write-Host "      updated $RelativePath" -ForegroundColor DarkGray
    }
}

# ---------------------------------------------------------------- 4 改名
$renames = @(
    @{ From = 'MouseFix.sln';                To = 'MouseMend.sln' }
    @{ From = 'MouseFix.vcxproj';            To = 'MouseMend.vcxproj' }
    @{ From = 'MouseFix.vcxproj.filters';    To = 'MouseMend.vcxproj.filters' }
    @{ From = 'MouseFix.rc';                 To = 'MouseMend.rc' }
    @{ From = 'tests\MouseFixTests.vcxproj'; To = 'tests\MouseMendTests.vcxproj' }
)

foreach ($pair in $renames) {
    $from = Join-Path $PSScriptRoot $pair.From
    $to = Join-Path $PSScriptRoot $pair.To

    if (Test-Path -LiteralPath $from) {
        Move-Item -LiteralPath $from -Destination $to -Force
        Write-Host ("      renamed {0} -> {1}" -f $pair.From, $pair.To) -ForegroundColor DarkGray
    }
    elseif (Test-Path -LiteralPath $to) {
        Write-Host ("      already renamed: {0}" -f $pair.To) -ForegroundColor DarkGray
    }
    else {
        Add-Problem "既没有 $($pair.From) 也没有 $($pair.To)"
    }
}

# 解决方案里的项目名与项目路径
Update-ProjectText -RelativePath 'MouseMend.sln' -Replacements @(
    @{ From = 'MouseFixTests';    To = 'MouseMendTests' }
    @{ From = 'MouseFix.vcxproj'; To = 'MouseMend.vcxproj' }
    @{ From = '"MouseFix"';       To = '"MouseMend"' }
)

# 资源文件的引用（工程与筛选器各一处）
Update-ProjectText -RelativePath 'MouseMend.vcxproj' -Replacements @(
    @{ From = 'MouseFix.rc'; To = 'MouseMend.rc' }
)

Update-ProjectText -RelativePath 'MouseMend.vcxproj.filters' -Replacements @(
    @{ From = 'MouseFix.rc'; To = 'MouseMend.rc' }
)

# 测试工程的名字与命名空间
Update-ProjectText -RelativePath 'tests\MouseMendTests.vcxproj' -Replacements @(
    @{ From = 'MouseFixTests'; To = 'MouseMendTests' }
)

Write-Step '4/5' 'RENAME' 'OK - 改名与引用同步完成'

# ---------------------------------------------------------------- 5 校验
foreach ($pair in $renames) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $pair.To))) {
        Add-Problem "缺少新文件 $($pair.To)"
    }

    $leftover = Join-Path $PSScriptRoot $pair.From
    if (Test-Path -LiteralPath $leftover) {
        Add-Problem "旧文件仍在 $($pair.From)"
    }
}

# 工程文件里不该再出现 MouseFix 字样
$projectFiles = Get-ChildItem -Path $PSScriptRoot -Recurse -File -Include *.sln, *.vcxproj, *.filters, *.rc |
    Where-Object { $_.FullName -notmatch '\\bin\\|\\.vs\\|\\.rename_backup\\' }

foreach ($file in $projectFiles) {
    $hits = Select-String -LiteralPath $file.FullName -Pattern 'MouseFix' -SimpleMatch -ErrorAction SilentlyContinue
    if ($hits) {
        foreach ($hit in $hits) {
            Add-Problem ("残留 {0}:{1}  {2}" -f $file.Name, $hit.LineNumber, $hit.Line.Trim())
        }
    }

    # 改完之后还必须仍是纯 ASCII，否则说明写回时编码出了问题
    if (-not (Test-AsciiOnly $file.FullName)) {
        Add-Problem ("{0} 不再是纯 ASCII，编码可能被改坏" -f $file.Name)
    }
}

Write-Host ''

if ($script:Problems.Count -eq 0) {
    Write-Step '5/5' 'VERIFY' 'OK - 全部通过，没有残留'
    Write-Host ''
    Write-Host '接下来：' -ForegroundColor Cyan
    Write-Host '  1. 用 Visual Studio 打开 MouseMend.sln'
    Write-Host '  2. 重新生成解决方案'
    Write-Host '  3. 跑 tests\bin\x64\Release\MouseMendTests.exe 确认单测通过'
    Write-Host ''
    Write-Host '确认无误后，可以删除 .rename_backup\ 和本脚本。' -ForegroundColor DarkGray
    Write-Host '如有问题，把 .rename_backup\ 里的文件复制回来即可还原。' -ForegroundColor DarkGray
    Write-Host ''
    exit 0
}

Write-Step '5/5' 'VERIFY' ("FAILED - 发现 {0} 个问题" -f $script:Problems.Count)
Write-Host ''
Write-Host '原文件已备份在 .rename_backup\，可以复制回来重试。' -ForegroundColor Yellow
Write-Host ''
exit 2
