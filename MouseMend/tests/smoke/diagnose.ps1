# Verifies the read-only command line switches used by support and deployment.
#
# Usage:
#   pwsh -NoProfile -File tests\smoke\diagnose.ps1 [-Configuration Release] [-Platform x64]

param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$suffix = if ($Platform -eq 'x64') { 'x64' } else { 'x86' }
$exe = Join-Path $PSScriptRoot "..\..\bin\$suffix\$Configuration\MouseMend-$suffix.exe"
$exe = (Resolve-Path $exe).Path

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

function Invoke-MouseMend([string[]]$arguments) {
    $output = Join-Path $env:TEMP ("mousemend_diag_" + [guid]::NewGuid().ToString('N') + ".txt")
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru `
        -NoNewWindow -RedirectStandardOutput $output
    $text = if (Test-Path $output) { Get-Content -Path $output -Raw } else { '' }
    if (Test-Path $output) { [System.IO.File]::Delete($output) }
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Text = $text }
}

# The switches must not require a running instance or touch any setting
$settings_before = $null
if (Test-Path 'HKCU:\Software\MouseMend') {
    $settings_before = (Get-ItemProperty 'HKCU:\Software\MouseMend' | Out-String)
}

$version = Invoke-MouseMend '--version'
Check 'version exit code' $version.ExitCode 0
Check 'version banner present' ($version.Text -match 'MouseMend \d+\.\d+\.\d+') $true

$report = Invoke-MouseMend '--diagnose'
Check 'diagnose exit code' $report.ExitCode 0

foreach ($key in @('exe=', 'language=', 'profile=', 'protective_buttons=', 'hybrid=',
        'release_delay_ms=', 'drag_distance_px=', 'notify_blocks=', 'logging=',
        'log_dir=', 'hook_install=', 'status=',
        'hover_text_chars=', 'settings_writable=', 'autostart=', 'result=')) {
    Check "report contains $key" ($report.Text -match [regex]::Escape($key)) $true
}

Check 'hook installs' ($report.Text -match 'hook_install=ok') $true
Check 'buttons are enabled' ($report.Text -match 'protective_buttons=[1-9]') $true
Check 'status token present' ($report.Text -match 'status=(enabled|partial|disabled|inactive)') $true
Check 'overall result OK' ($report.Text -match 'result=OK') $true

# The hover text must fit what the shell accepts (128 chars including terminator)
$hoverMatch = [regex]::Match($report.Text, 'hover_text_chars=(\d+)')
Check 'hover text length reported' $hoverMatch.Success $true
if ($hoverMatch.Success) {
    Check 'hover text fits the shell limit' ([int]$hoverMatch.Groups[1].Value -lt 128) $true
}

# The report must be plain ASCII so it survives pipes and ticket systems
$nonAscii = [regex]::Matches($report.Text, '[^\x00-\x7F]')
Check 'report is ASCII only' ($nonAscii.Count -eq 0) $true

# A diagnostic run must not change the profile
$settings_after = $null
if (Test-Path 'HKCU:\Software\MouseMend') {
    $settings_after = (Get-ItemProperty 'HKCU:\Software\MouseMend' | Out-String)
}
Check 'settings unchanged by diagnose' ($settings_after -eq $settings_before) $true

Write-Host ""
Write-Host "RESULT: $failures failure(s)" -ForegroundColor $(if ($failures -eq 0) { 'Green' } else { 'Red' })
if ($failures -eq 0) { exit 0 } else { exit 1 }
