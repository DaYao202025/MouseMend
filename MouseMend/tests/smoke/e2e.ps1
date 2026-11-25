# End-to-end smoke test for the built tray application.
# It launches the real executable, drives it through WM_COMMAND, verifies the
# persisted settings and finally checks that it shuts down cleanly.
#
# Usage:
#   pwsh -NoProfile -File tests\smoke\e2e.ps1 [-Configuration Release] [-Platform x64]

param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Native {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);

    public static IntPtr Find(uint target, string className) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != target) return true;
            var cls = new StringBuilder(256); GetClassNameW(h, cls, 256);
            if (cls.ToString() == className) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // Visible windows only. The check-up window is the main window and closing it
    // hides it instead of destroying it, so a hidden dialog would otherwise be
    // found in place of the confirmation box and every later lookup would be wrong.
    public static IntPtr FindVisible(uint target, string className) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != target) return true;
            if (!IsWindowVisible(h)) return true;
            var cls = new StringBuilder(256); GetClassNameW(h, cls, 256);
            if (cls.ToString() == className) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_USER = 1024
$IDYES = 6

$IDM_TOGGLE_ENABLE = $WM_USER + 11
$IDM_TOGGLE_NOTIFY = $WM_USER + 15
$IDM_TOGGLE_LOG = $WM_USER + 16
$IDM_OPEN_SETTINGS = $WM_USER + 17
$IDM_RESTORE_DEFAULTS = $WM_USER + 80
$IDM_LANG_EN = $WM_USER + 50
$IDM_LANG_ZH = $WM_USER + 51
$IDM_RELEASE_DELAY_BASE = $WM_USER + 60
$IDM_DRAG_DISTANCE_BASE = $WM_USER + 70
$IDM_RESTORE_DEFAULTS = $WM_USER + 80
# Right button, first preset (40ms) - the range that used to hit the wrong button
$IDM_RIGHT_40MS = $WM_USER + 1000 + 1 * 10 + 0

$suffix = if ($Platform -eq 'x64') { 'x64' } else { 'x86' }
$exe = Join-Path $PSScriptRoot "..\..\bin\$suffix\$Configuration\MouseMend-$suffix.exe"
$exe = (Resolve-Path $exe).Path

$regPath = 'HKCU:\Software\MouseMend'
$failures = 0

function Check([string]$name, $actual, $expected) {
    if ("$actual" -eq "$expected") {
        Write-Host "PASS  $name = $actual" -ForegroundColor Green
    }
    else {
        Write-Host "FAIL  $name = $actual (expected $expected)" -ForegroundColor Red
        $script:failures++
    }
}

# The test needs a pristine profile. Export an existing one and put it back at the
# end, so running the suite on a working installation is harmless.
$settings_existed = Test-Path $regPath
$backup_file = Join-Path $env:TEMP 'mousemend_settings_backup.reg'

if ($settings_existed) {
    & reg.exe export 'HKCU\Software\MouseMend' $backup_file /y | Out-Null
    Remove-Item -Path $regPath -Recurse -Force
    Write-Host 'NOTE: existing settings exported and will be restored' -ForegroundColor Yellow
}

Get-Process -Name "MouseMend-$suffix" -ErrorAction SilentlyContinue | ForEach-Object {
    $_.Kill()
    $_.WaitForExit(2000)
}

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Milliseconds 1200
$proc.Refresh()

if ($proc.HasExited) {
    Write-Host "FAIL: process exited early ($($proc.ExitCode))" -ForegroundColor Red
    exit 1
}
Write-Host "RUNNING: pid $($proc.Id)" -ForegroundColor Cyan

$hwnd = [Native]::Find([uint32]$proc.Id, 'MouseMendWndClass')
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Host 'FAIL: hidden window not found' -ForegroundColor Red
    $proc.Kill()
    exit 1
}
Write-Host "WINDOW: 0x$($hwnd.ToInt64().ToString('X'))" -ForegroundColor Cyan

function Send-Command([int]$id) {
    [void][Native]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
}

# 0. The first-run balloon must be marked as shown
$settings = Get-ItemProperty -Path $regPath
Check 'IntroShown after first run' $settings.IntroShown 1

# 1. Toggle all off, then back on (also forces a save)
Send-Command $IDM_TOGGLE_ENABLE
$settings = Get-ItemProperty -Path $regPath
Check 'Btn0_Enabled (left, all off)' $settings.Btn0_Enabled 0
Check 'Btn6_Enabled (hwheel, all off)' $settings.Btn6_Enabled 0

Send-Command $IDM_TOGGLE_ENABLE
$settings = Get-ItemProperty -Path $regPath
Check 'Btn0_Enabled (all on)' $settings.Btn0_Enabled 1
Check 'Btn6_Enabled (all on)' $settings.Btn6_Enabled 1

# 2. The right button preset must only change the right button
Send-Command $IDM_RIGHT_40MS
$settings = Get-ItemProperty -Path $regPath
Check 'Btn1_Threshold (right preset)' $settings.Btn1_Threshold 40
Check 'Btn0_Threshold (left untouched)' $settings.Btn0_Threshold 50

# 3. Smart Drag tuning must persist
Send-Command ($IDM_RELEASE_DELAY_BASE + 2)
Send-Command ($IDM_DRAG_DISTANCE_BASE + 2)
$settings = Get-ItemProperty -Path $regPath
Check 'ReleaseDelay (third preset)' $settings.ReleaseDelay 80
Check 'DragDistance (third preset)' $settings.DragDistance 10

# 4. Restore Defaults must fall back to a clean profile (confirmation dialog)
[void][Native]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$IDM_RESTORE_DEFAULTS, [IntPtr]::Zero)
Start-Sleep -Milliseconds 500

$dialog = [Native]::FindVisible([uint32]$proc.Id, '#32770')
if ($dialog -eq [IntPtr]::Zero) {
    Write-Host 'FAIL: confirmation dialog not found' -ForegroundColor Red
    $failures++
}
else {
    [void][Native]::PostMessage($dialog, $WM_COMMAND, [IntPtr]$IDYES, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    $settings = Get-ItemProperty -Path $regPath
    # Mirrors RELEASE_DELAY_DEFAULT_MS in src/core/config.h
    Check 'ReleaseDelay restored' $settings.ReleaseDelay 20
    Check 'DragDistance restored' $settings.DragDistance 5
    Check 'Btn1_Threshold restored' $settings.Btn1_Threshold 50
    Check 'Btn0_Enabled still on' $settings.Btn0_Enabled 1
}

# 5. Language switching must persist
Send-Command $IDM_LANG_ZH
$settings = Get-ItemProperty -Path $regPath
Check 'Language (switched to Chinese)' $settings.Language 1

Send-Command $IDM_LANG_EN
$settings = Get-ItemProperty -Path $regPath
Check 'Language (switched back to English)' $settings.Language 0

# 6. The blocked-click notification is opt-in and must persist
$settings = Get-ItemProperty -Path $regPath
Check 'NotifyBlocked defaults to off' $settings.NotifyBlocked 0

Send-Command $IDM_TOGGLE_NOTIFY
$settings = Get-ItemProperty -Path $regPath
Check 'NotifyBlocked (enabled)' $settings.NotifyBlocked 1

Send-Command $IDM_TOGGLE_NOTIFY
$settings = Get-ItemProperty -Path $regPath
Check 'NotifyBlocked (disabled again)' $settings.NotifyBlocked 0

# 7. The blocked-click history is opt-in, and enabling it must NOT create the
#    Log folder: the folder appears on the first real blocked click only.
$settings = Get-ItemProperty -Path $regPath
Check 'LogBlocked defaults to off' $settings.LogBlocked 0

$logFolder = Join-Path (Split-Path $exe -Parent) 'Log'
$logFolderExisted = Test-Path $logFolder

Send-Command $IDM_TOGGLE_LOG
$settings = Get-ItemProperty -Path $regPath
Check 'LogBlocked (enabled)' $settings.LogBlocked 1

if (-not $logFolderExisted) {
    Check 'no Log folder before the first blocked click' (Test-Path $logFolder) $false
}

Send-Command $IDM_TOGGLE_LOG
$settings = Get-ItemProperty -Path $regPath
Check 'LogBlocked (disabled again)' $settings.LogBlocked 0

# 8. The settings window must open and close without disturbing the tray part
Send-Command $IDM_OPEN_SETTINGS
$settingsDialog = [Native]::FindVisible([uint32]$proc.Id, '#32770')
Check 'settings window opens' ($settingsDialog -ne [IntPtr]::Zero) $true

if ($settingsDialog -ne [IntPtr]::Zero) {
    [void][Native]::PostMessage($settingsDialog, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    # The window is the main window, so closing it puts it into the tray instead
    # of ending the program: no visible dialog may remain, and the process must
    # still be there to protect the mouse.
    Check 'settings window hides to the tray' ([Native]::FindVisible([uint32]$proc.Id, '#32770') -eq [IntPtr]::Zero) $true
    Check 'program still running after close' $proc.HasExited $false
}

# 9. Restore Defaults must switch the notification back off again
Send-Command $IDM_TOGGLE_NOTIFY
[void][Native]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$IDM_RESTORE_DEFAULTS, [IntPtr]::Zero)
Start-Sleep -Milliseconds 500

$dialog = [Native]::FindVisible([uint32]$proc.Id, '#32770')
if ($dialog -eq [IntPtr]::Zero) {
    Write-Host 'FAIL: confirmation dialog not found (second restore)' -ForegroundColor Red
    $failures++
}
else {
    [void][Native]::PostMessage($dialog, $WM_COMMAND, [IntPtr]$IDYES, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    $settings = Get-ItemProperty -Path $regPath
    Check 'NotifyBlocked (after restore defaults)' $settings.NotifyBlocked 0
    Check 'LogBlocked (after restore defaults)' $settings.LogBlocked 0
}

# 10. Clean shutdown
[void][Native]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
if (-not $proc.WaitForExit(5000)) {
    $proc.Kill()
    Write-Host 'FAIL: no clean exit after WM_CLOSE' -ForegroundColor Red
    exit 1
}
Check 'exit code' $proc.ExitCode 0

Remove-Item -Path $regPath -Recurse -Force

if ($settings_existed -and (Test-Path $backup_file)) {
    & reg.exe import $backup_file | Out-Null
    Remove-Item -Path $backup_file -Force
    Write-Host 'NOTE: previous settings restored' -ForegroundColor Yellow
}

Write-Host ""
Write-Host "RESULT: $failures failure(s)" -ForegroundColor $(if ($failures -eq 0) { 'Green' } else { 'Red' })
if ($failures -eq 0) { exit 0 } else { exit 1 }
