# Push the current tree to GitHub through the REST API
#
# Why this exists: on this network `github.com` (the git transport) is blocked
# while `api.github.com` answers normally, so `git push` cannot be used at all.
# This script builds the commit on the server side instead: one blob per file,
# one tree per directory, then a commit, then the branch is moved.
#
# The published history is deliberately short. The first release was a single
# root commit so that nothing from the upstream project shows up on GitHub;
# every run after that appends one commit on top, so the repository gains
# history but never regains the old one.
#
# Usage:
#   pwsh -File doc\push-to-github.ps1 -Message "Fix the wheel reversal edge case"
#   pwsh -File doc\push-to-github.ps1 -Message "..." -DryRun
#
# Requires: a GitHub credential stored for github.com (the one Visual Studio
# writes), and a clean working tree.

param(
    [string]$Repo = 'DaYao202025/MouseMend',
    [string]$Branch = 'main',
    [string]$Message = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$env:GIT_TERMINAL_PROMPT = '0'
$env:GCM_INTERACTIVE = 'never'

$repoRoot = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Set-Location $repoRoot

$api = "https://api.github.com/repos/$Repo"

# ---------------------------------------------------------------- credential
# The secret is read into a variable and never printed.
$raw = "protocol=https`nhost=github.com`n`n" | & git credential fill 2>$null
$token = ($raw | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
if (-not $token) {
    throw 'No GitHub credential stored. Open Visual Studio and sign in, or run: git credential approve'
}
$headers = @{
    'Authorization' = "Bearer $token"
    'User-Agent'    = 'MouseMend-release'
    'Accept'        = 'application/vnd.github+json'
}

function Send-Json {
    param([string]$Uri, $Object, [string]$Method = 'Post', [int]$Retries = 4)

    if ($DryRun) {
        Write-Host ("  [dry-run] {0}" -f $Uri) -ForegroundColor DarkGray
        # 40 hex characters, the shape of a real object id, so downstream code
        # that slices it behaves the same as in a real run
        $fake = ([guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')).Substring(0, 40)
        return @{ sha = $fake; tree = @{ sha = $fake }; ref = 'refs/heads/dry'; object = @{ sha = $fake } }
    }

    $json = $Object | ConvertTo-Json -Depth 12 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)

    for ($i = 1; $i -le $Retries; $i++) {
        try {
            return Invoke-RestMethod -Uri $Uri -Method $Method -Body $bytes `
                -ContentType 'application/json; charset=utf-8' -Headers $headers -TimeoutSec 120
        }
        catch {
            $code = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
            # 4xx other than 429 will not get better by retrying
            if (($code -ge 400 -and $code -lt 500 -and $code -ne 429) -or $i -eq $Retries) {
                throw ("{0} {1} failed (status {2}): {3}" -f $Method, $Uri, $code, $_.Exception.Message)
            }
            Write-Host ("  retry {0}/{1} after status {2}" -f $i, $Retries, $code) -ForegroundColor Yellow
            Start-Sleep -Seconds (2 * $i)
        }
    }
}

# ---------------------------------------------------------------- preflight
Write-Host '=== preflight ===' -ForegroundColor Cyan

# Only tracked files matter: an untracked file is never published, and this
# repository deliberately keeps a couple of screenshots lying around untracked.
$dirty = & git status --porcelain --untracked-files=no
if ($dirty) {
    Write-Host $dirty
    throw 'Working tree has uncommitted changes to tracked files. Commit them first: the script publishes what is on disk.'
}

$untracked = @(& git status --porcelain --untracked-files=normal | Where-Object { $_ -match '^\?\?' })
if ($untracked.Count -gt 0) {
    Write-Host ("note: {0} untracked file(s) will not be published" -f $untracked.Count) -ForegroundColor DarkGray
}

if (-not $Message) {
    # %B is the whole message. Using %s here silently dropped the body of every
    # commit that had one, which is how two commits ended up on the remote with
    # a subject and nothing else.
    #
    # git prints one line per element, so the output has to be joined back with
    # newlines: passing the array straight into the JSON payload collapsed the
    # whole message into a single 491 character line.
    $Message = ((& git log -1 --format='%B') -join "`n").Trim()
    Write-Host 'no -Message given, reusing the full message of the last local commit'
}
else {
    $localSubject = ((& git log -1 --format=%s) -join "`n").Trim()
    $localBody = ((& git log -1 --format='%B') -join "`n").Trim()
    if ($Message.Trim() -eq $localSubject -and $localBody.Length -gt $localSubject.Length) {
        Write-Host 'warning: -Message is only the subject, but the local commit has a body.' -ForegroundColor Yellow
        Write-Host '         pass no -Message at all to keep the full text.' -ForegroundColor Yellow
    }
}

# The file list comes from git, so .gitignore decides what is published and
# the two scratch files never travel.
$paths = @(& git ls-files)
Write-Host ("tracked files: {0}" -f $paths.Count)

# Screenshots are published on purpose; everything else that looks like a
# scratch file or a build artefact is not.
$forbidden = $paths | Where-Object {
    ($_ -match '\.(jpg|jpeg|gif|bmp|exe|pfx|txt)$') -or
    ($_ -match '\.png$' -and $_ -notmatch '^MouseMend/doc/screenshots/') -or
    ($_ -match '(^|/)\.vs/') -or
    ($_ -match '(^|/)bin/')
}
if ($forbidden) {
    Write-Host 'refusing to publish:' -ForegroundColor Red
    $forbidden | ForEach-Object { Write-Host ("  {0}" -f $_) }
    throw 'unwanted files are tracked'
}

# A blob hash that differs from the file on disk means the working copy holds
# something the repository does not, which would publish source nobody compiled.
#
# -Path applies the same attributes as `git add` (this repository sets
# `* text=auto` with core.autocrlf=true), so a file that was checked out with
# CRLF still hashes to the LF blob and is not reported as a false mismatch.
$mismatch = 0
foreach ($p in $paths) {
    $wt = (& git hash-object "--path=$p" -- $p).Trim()
    $blob = ((& git ls-files -s -- $p) -split '\s+')[1]
    if ($wt -ne $blob) {
        Write-Host ("  differs from the index: {0}" -f $p) -ForegroundColor Red
        $mismatch++
    }
}
if ($mismatch -gt 0) {
    throw "$mismatch file(s) differ from the index; commit them before publishing"
}
Write-Host 'working tree matches the index'

# Report what the bytes on disk look like, so a CRLF working copy is visible
# instead of silently publishing different line endings from the ones in git.
$crlfFiles = @()
foreach ($p in $paths) {
    $full = Join-Path $repoRoot ($p -replace '/', '\')
    $bytes = [IO.File]::ReadAllBytes($full)
    for ($i = 0; $i -lt [Math]::Min($bytes.Length - 1, 4096); $i++) {
        if ($bytes[$i] -eq 13 -and $bytes[$i + 1] -eq 10) { $crlfFiles += $p; break }
    }
}
if ($crlfFiles.Count -gt 0) {
    Write-Host ("note: {0} file(s) have CRLF on disk and will be published that way" -f $crlfFiles.Count) -ForegroundColor Yellow
    $crlfFiles | Select-Object -First 5 | ForEach-Object { Write-Host ("  {0}" -f $_) -ForegroundColor DarkGray }
    Write-Host '  run `git config core.autocrlf false` and re-checkout to keep the tree LF'
}

# ---------------------------------------------------------------- remote head
$parent = $null
try {
    $ref = Invoke-RestMethod "$api/git/ref/heads/$Branch" -Headers $headers -TimeoutSec 60
    $parent = $ref.object.sha
    Write-Host ("remote {0} head: {1}" -f $Branch, $parent)
}
catch {
    Write-Host ("remote {0} has no head yet, this will be the first commit" -f $Branch) -ForegroundColor Yellow
}

# ---------------------------------------------------------------- blobs
Write-Host '=== upload blobs ===' -ForegroundColor Cyan
$items = @()
$n = 0
foreach ($p in $paths) {
    $n++
    $full = Join-Path $repoRoot ($p -replace '/', '\')
    $bytes = [IO.File]::ReadAllBytes($full)
    $r = Send-Json "$api/git/blobs" @{
        content  = [Convert]::ToBase64String($bytes)
        encoding = 'base64'
    }
    $items += [pscustomobject]@{
        path = $p
        mode = '100644'
        sha  = $r.sha
    }
    if ($n % 20 -eq 0 -or $n -eq $paths.Count) { Write-Host ("  {0}/{1}" -f $n, $paths.Count) }
}

# ---------------------------------------------------------------- trees
# GitHub requires each entry path to be a plain name, so the tree has to be
# built bottom up, one directory per call.
function New-Tree {
    param($Entries, [string]$Prefix)

    $files = @()
    $subs = @{}

    foreach ($e in $Entries) {
        $rel = if ($Prefix) { $e.path.Substring($Prefix.Length + 1) } else { $e.path }
        $slash = $rel.IndexOf('/')
        if ($slash -lt 0) {
            $files += @{ path = $rel; mode = $e.mode; type = 'blob'; sha = $e.sha }
        }
        else {
            $dir = $rel.Substring(0, $slash)
            if (-not $subs.ContainsKey($dir)) { $subs[$dir] = @() }
            $subs[$dir] += $e
        }
    }

    $entries = @()
    foreach ($f in $files) { $entries += $f }
    foreach ($dir in $subs.Keys) {
        $childPrefix = if ($Prefix) { "$Prefix/$dir" } else { $dir }
        $entries += @{
            path = $dir
            mode = '040000'
            type = 'tree'
            sha  = (New-Tree -Entries $subs[$dir] -Prefix $childPrefix)
        }
    }

    return (Send-Json "$api/git/trees" @{ tree = @($entries) }).sha
}

Write-Host '=== build tree ===' -ForegroundColor Cyan
$rootTree = New-Tree -Entries $items -Prefix ''
Write-Host ("root tree: {0}" -f $rootTree)

# ---------------------------------------------------------------- commit
Write-Host '=== create commit ===' -ForegroundColor Cyan
$name = (& git config user.name).Trim()
$email = (& git config user.email).Trim()
$now = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

$parents = @()
if ($parent) { $parents = @($parent) }

$payload = @{
    message   = $Message
    tree      = $rootTree
    parents   = $parents
    author    = @{ name = $name; email = $email; date = $now }
    committer = @{ name = $name; email = $email; date = $now }
}

$commit = Send-Json "$api/git/commits" $payload
Write-Host ("commit: {0}  parents: {1}" -f $commit.sha, $(if ($parents.Count) { $parents[0].Substring(0, 7) } else { '(root)' }))

if ($DryRun) {
    Write-Host 'dry run, nothing was published' -ForegroundColor Yellow
    exit 0
}

# ---------------------------------------------------------------- move branch
Write-Host '=== move branch ===' -ForegroundColor Cyan
if ($parent) {
    $moved = Send-Json "$api/git/refs/heads/$Branch" @{ sha = $commit.sha; force = $false } -Method Patch
}
else {
    $moved = Send-Json "$api/git/refs" @{ ref = "refs/heads/$Branch"; sha = $commit.sha }
}
Write-Host ("{0} -> {1}" -f $moved.ref, $moved.object.sha)

# ---------------------------------------------------------------- verify
Write-Host '=== verify ===' -ForegroundColor Cyan
$r = Invoke-RestMethod $api -Headers $headers -TimeoutSec 60

# Resolve the branch to its commit first: asking for a tree by branch name can
# answer 404 for a moment right after the ref was moved.
$head = (Invoke-RestMethod "$api/git/ref/heads/$Branch" -Headers $headers -TimeoutSec 60).object.sha
$tree = Invoke-RestMethod "$api/git/trees/$head`?recursive=1" -Headers $headers -TimeoutSec 60
$blobs = @($tree.tree | Where-Object { $_.type -eq 'blob' })

Write-Host ("  default branch : {0}" -f $r.default_branch)
Write-Host ("  blobs on remote: {0}" -f $blobs.Count)
Write-Host ("  expected       : {0}" -f $paths.Count)

$remotePaths = $blobs | ForEach-Object { $_.path }
$missing = $paths | Where-Object { $remotePaths -notcontains $_ }
$extra = $remotePaths | Where-Object { $paths -notcontains $_ }

if ($missing) { Write-Host '  MISSING on remote:' -ForegroundColor Red; $missing | ForEach-Object { Write-Host ("    {0}" -f $_) } }
if ($extra) { Write-Host '  EXTRA on remote:' -ForegroundColor Red; $extra | ForEach-Object { Write-Host ("    {0}" -f $_) } }
if (-not $missing -and -not $extra) { Write-Host '  remote matches the working tree exactly' -ForegroundColor Green }

$token = $null
