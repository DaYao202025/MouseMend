# Regression test for the deferred-release timer.
#
# It performs a synthetic long press (down -> 300 ms -> up). Because the press
# lasts longer than the "drag" threshold, the application swallows the release
# and re-injects it ~50 ms later. If the deferred-release timer were never armed,
# the button would stay logically pressed forever.
#
# WARNING: this really clicks the desktop, so it only runs with -Force.
#
# Usage:
#   pwsh -NoProfile -File tests\smoke\stuck_key.ps1 -Force [-Configuration Release]

param(
    [switch]$Force,
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

if (-not $Force) {
    Write-Host 'Refusing to run: this test clicks the desktop. Re-run with -Force.' -ForegroundColor Yellow
    exit 2
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Input {
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int cbSize);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x; public int y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    public static void Left(uint flags) {
        INPUT[] a = new INPUT[1];
        a[0].type = 0;
        a[0].mi.dwFlags = flags;
        SendInput(1, a, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

$failures = 0
function Check([string]$name, $actual, $expected) {
    if ("$actual" -eq "$expected") { Write-Host "PASS  $name = $actual" -ForegroundColor Green }
    else { Write-Host "FAIL  $name = $actual (expected $expected)" -ForegroundColor Red; $script:failures++ }
}

$exe = Join-Path $PSScriptRoot "..\..\bin\x64\$Configuration\MouseMend-x64.exe"
$exe = (Resolve-Path $exe).Path

Get-Process -Name 'MouseMend-x64' -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(2000) }

# Use a throwaway profile so the test never inherits user settings, and put the
# real profile back afterwards.
$regPath = 'HKCU:\Software\MouseMend'
$settings_existed = Test-Path $regPath
$backup_file = Join-Path $env:TEMP 'mousemend_settings_backup.reg'

if ($settings_existed) {
    & reg.exe export 'HKCU\Software\MouseMend' $backup_file /y | Out-Null
    Remove-Item -Path $regPath -Recurse -Force
}

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Milliseconds 1200
if ($proc.HasExited) {
    Write-Host 'FAIL: app exited early' -ForegroundColor Red
    exit 1
}

# Remember the cursor position and move it to an empty desktop corner
$origin = New-Object Input+POINT
[void][Input]::GetCursorPos([ref]$origin)

$targetX = [Input]::GetSystemMetrics(0) - 60
$targetY = [Input]::GetSystemMetrics(1) - 60
[void][Input]::SetCursorPos($targetX, $targetY)
Start-Sleep -Milliseconds 150

# Calibration: on a locked or non-interactive session SendInput is silently
# ignored, which would turn this test into a false failure. Detect that first.
[Input]::Left([Input]::LEFTDOWN)
Start-Sleep -Milliseconds 120
$calibration = [Input]::GetAsyncKeyState(0x01) -band 0x8000
[Input]::Left([Input]::LEFTUP)
Start-Sleep -Milliseconds 200

if (-not $calibration) {
    Write-Host 'SKIP: synthetic input is unavailable (session locked or input blocked)' -ForegroundColor Yellow
    $proc.Kill()
    [void]$proc.WaitForExit(3000)
    Remove-Item -Path $regPath -Recurse -Force -ErrorAction SilentlyContinue
    if ($settings_existed -and (Test-Path $backup_file)) {
        & reg.exe import $backup_file | Out-Null
        Remove-Item -Path $backup_file -Force
    }
    exit 2
}

# Long press: the release is swallowed and re-injected by the timer
[Input]::Left([Input]::LEFTDOWN)
Start-Sleep -Milliseconds 320
$downState = [Input]::GetAsyncKeyState(0x01) -band 0x8000
Check 'button is down before release' ([bool]$downState) $true

[Input]::Left([Input]::LEFTUP)
Start-Sleep -Milliseconds 400

$upState = [Input]::GetAsyncKeyState(0x01) -band 0x8000
Check 'button is released by the deferred-release timer' ([bool]$upState) $false

[void][Input]::SetCursorPos($origin.X, $origin.Y)

$proc.Kill()
[void]$proc.WaitForExit(3000)

Remove-Item -Path $regPath -Recurse -Force -ErrorAction SilentlyContinue

if ($settings_existed -and (Test-Path $backup_file)) {
    & reg.exe import $backup_file | Out-Null
    Remove-Item -Path $backup_file -Force
}

Write-Host ""
Write-Host "RESULT: $failures failure(s)" -ForegroundColor $(if ($failures -eq 0) { 'Green' } else { 'Red' })
if ($failures -eq 0) { exit 0 } else { exit 1 }
