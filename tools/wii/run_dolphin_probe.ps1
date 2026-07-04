param(
    [string]$DolphinExe = "D:\Users\TeMouRen\Desktop\Dolphin-x64\Dolphin.exe",
    [string]$DolPath = "F:\Wii Work\" + [char]0x63D0 + [char]0x53D6 + [char]0x6D4B + [char]0x8BD5 + "\reVC\sys\main.dol",
    [string]$LogPath = "C:\Users\20493\AppData\Roaming\Dolphin Emulator\Logs\dolphin.log",
    [int]$WindowTimeoutSec = 20,
    [int]$InitialMenuDelayMs = 2500,
    [int]$MenuGapMs = 1200,
    [int]$IntroSkipCount = 4,
    [int]$IntroSkipGapSec = 8,
    [int]$GameplaySettleSec = 12,
    [int]$StickHoldMs = 1200,
    [int]$StickGapMs = 350,
    [int]$StickCycles = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$KEYEVENTF_KEYUP = 0x0002
$VK_MENU = 0x12
$VK_F4 = 0x73
$KeyMap = @{
    x = 0x58
    i = 0x49
    k = 0x4B
}

Add-Type @"
using System.Runtime.InteropServices;
public static class DolphinProbeKeys {
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, int dwFlags, int dwExtraInfo);
}
"@

$wsh = New-Object -ComObject WScript.Shell

function Write-Step {
    param([string]$Message)
    Write-Output ("[DOLPHIN-PROBE] {0}" -f $Message)
}

function Press-Key {
    param(
        [byte]$VirtualKey,
        [int]$HoldMs = 80
    )

    [DolphinProbeKeys]::keybd_event($VirtualKey, 0, 0, 0)
    Start-Sleep -Milliseconds $HoldMs
    [DolphinProbeKeys]::keybd_event($VirtualKey, 0, $KEYEVENTF_KEYUP, 0)
}

function Press-KeyChar {
    param(
        [ValidateSet("x", "i", "k")]
        [string]$Char,
        [int]$HoldMs = 80
    )

    Press-Key -VirtualKey ([byte]$KeyMap[$Char]) -HoldMs $HoldMs
}

function Send-AltF4 {
    [DolphinProbeKeys]::keybd_event($VK_MENU, 0, 0, 0)
    Start-Sleep -Milliseconds 60
    Press-Key -VirtualKey $VK_F4 -HoldMs 80
    Start-Sleep -Milliseconds 60
    [DolphinProbeKeys]::keybd_event($VK_MENU, 0, $KEYEVENTF_KEYUP, 0)
}

function Wait-ForMainWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSec
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne 0) {
            return
        }
        Start-Sleep -Milliseconds 250
    }

    throw "Dolphin main window did not appear within $TimeoutSec seconds."
}

function Activate-DolphinWindow {
    param([System.Diagnostics.Process]$Process)

    $Process.Refresh()
    if ($Process.HasExited) {
        throw "Dolphin exited unexpectedly."
    }
    if ($Process.MainWindowHandle -eq 0) {
        throw "Dolphin main window handle is unavailable."
    }

    [void]$wsh.AppActivate($Process.Id)
    Start-Sleep -Milliseconds 250
}

function Close-Dolphin {
    param([System.Diagnostics.Process]$Process)

    if ($Process.HasExited) {
        return
    }

    Activate-DolphinWindow -Process $Process
    Write-Step "Closing Dolphin with Alt+F4"
    Send-AltF4

    if ($Process.WaitForExit(15000)) {
        return
    }

    Write-Step "Dolphin did not exit in time; forcing process stop"
    Stop-Process -Id $Process.Id -Force
    $Process.WaitForExit()
}

if (Get-Process -Name "Dolphin" -ErrorAction SilentlyContinue) {
    throw "A Dolphin process is already running. Refusing to start a second instance."
}

if (-not (Test-Path -LiteralPath $DolphinExe)) {
    throw "Dolphin executable not found: $DolphinExe"
}
if (-not (Test-Path -LiteralPath $DolPath)) {
    throw "main.dol not found: $DolPath"
}

if (Test-Path -LiteralPath $LogPath) {
    Write-Step "Removing old log: $LogPath"
    Remove-Item -LiteralPath $LogPath -Force
}

$workingDir = Split-Path -Path $DolphinExe -Parent
Write-Step "Launching Dolphin"
$process = Start-Process -FilePath $DolphinExe -ArgumentList ('"{0}"' -f $DolPath) -WorkingDirectory $workingDir -PassThru

try {
    Wait-ForMainWindow -Process $process -TimeoutSec $WindowTimeoutSec
    Activate-DolphinWindow -Process $process

    Write-Step "Waiting for menu"
    Start-Sleep -Milliseconds $InitialMenuDelayMs

    Write-Step "Pressing X for Start Game"
    Press-KeyChar -Char "x" -HoldMs 120
    Start-Sleep -Milliseconds $MenuGapMs

    Activate-DolphinWindow -Process $process
    Write-Step "Pressing X for New Game"
    Press-KeyChar -Char "x" -HoldMs 120

    for ($i = 0; $i -lt $IntroSkipCount; $i++) {
        Start-Sleep -Seconds $IntroSkipGapSec
        Activate-DolphinWindow -Process $process
        Write-Step ("Pressing X to skip intro/cutscene ({0}/{1})" -f ($i + 1), $IntroSkipCount)
        Press-KeyChar -Char "x" -HoldMs 120
    }

    Write-Step "Waiting for gameplay settle"
    Start-Sleep -Seconds $GameplaySettleSec

    for ($cycle = 0; $cycle -lt $StickCycles; $cycle++) {
        Activate-DolphinWindow -Process $process
        Write-Step ("Holding I ({0}/{1})" -f ($cycle + 1), $StickCycles)
        Press-KeyChar -Char "i" -HoldMs $StickHoldMs
        Start-Sleep -Milliseconds $StickGapMs

        Activate-DolphinWindow -Process $process
        Write-Step ("Holding K ({0}/{1})" -f ($cycle + 1), $StickCycles)
        Press-KeyChar -Char "k" -HoldMs $StickHoldMs
        Start-Sleep -Milliseconds $StickGapMs
    }
}
finally {
    Close-Dolphin -Process $process
}

if (Test-Path -LiteralPath $LogPath) {
    $logInfo = Get-Item -LiteralPath $LogPath
    Write-Step ("Run complete. Log size={0} bytes updated={1}" -f $logInfo.Length, $logInfo.LastWriteTime)
} else {
    Write-Step "Run complete, but dolphin.log was not created."
}
