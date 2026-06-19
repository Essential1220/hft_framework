#Requires -RunAsAdministrator
# revert_tuning.ps1 - Revert all HFT OS tuning to Windows defaults
# Run as Administrator. Reboot required for bcdedit changes.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

Write-Host "=== Reverting HFT OS Tuning ===" -ForegroundColor Cyan

# ---- 1. Power Plan: restore Balanced ----
Write-Host "`n[1/4] Restoring Balanced power plan..." -ForegroundColor Yellow
powercfg -setactive 381b4222-f694-41f0-9685-ff5bb260df2e 2>$null
Write-Host "  Power plan set to Balanced" -ForegroundColor Green

# ---- 2. TCP: remove custom registry values ----
Write-Host "`n[2/4] Removing TCP overrides..." -ForegroundColor Yellow
$tcpParams = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters"
foreach ($prop in @("TcpAckFrequency", "TCPNoDelay", "TcpDelAckTicks")) {
    Remove-ItemProperty -Path $tcpParams -Name $prop -ErrorAction SilentlyContinue
}
$interfaces = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces"
Get-ChildItem $interfaces | ForEach-Object {
    Remove-ItemProperty -Path $_.PSPath -Name "TcpAckFrequency" -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $_.PSPath -Name "TCPNoDelay" -ErrorAction SilentlyContinue
}
Write-Host "  TCP registry overrides removed" -ForegroundColor Green

# ---- 3. bcdedit: restore defaults ----
Write-Host "`n[3/4] Reverting bcdedit..." -ForegroundColor Yellow
bcdedit /deletevalue disabledynamictick 2>$null | Out-Null
bcdedit /deletevalue useplatformtick 2>$null | Out-Null
bcdedit /deletevalue tscsyncpolicy 2>$null | Out-Null
Write-Host "  bcdedit values removed (OS defaults restored)" -ForegroundColor Green
Write-Host "  ** Reboot required **" -ForegroundColor DarkYellow

# ---- 4. Re-enable services ----
Write-Host "`n[4/4] Re-enabling services..." -ForegroundColor Yellow
$services = @("SysMain", "DiagTrack", "WSearch", "MapsBroker", "TabletInputService")
foreach ($name in $services) {
    $s = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($s) {
        Set-Service $name -StartupType Automatic -ErrorAction SilentlyContinue
        Start-Service $name -ErrorAction SilentlyContinue
        Write-Host "  Re-enabled: $name" -ForegroundColor Green
    }
}

Write-Host "`n=== Revert Complete ===" -ForegroundColor Cyan
Write-Host "Reboot to apply bcdedit changes" -ForegroundColor White
