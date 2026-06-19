#Requires -RunAsAdministrator
# windows_hft_tuning.ps1 - Windows OS tuning for HFT low-latency trading
# Run as Administrator. Reboot required for bcdedit changes to take effect.
# Use revert_tuning.ps1 to undo all changes.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

Write-Host "=== HFT Windows OS Tuning ===" -ForegroundColor Cyan

# ---- 1. Power Plan: Ultimate Performance ----
Write-Host "`n[1/4] Power Plan..." -ForegroundColor Yellow
$ultimate = "e9a42b02-d5df-448d-aa00-03f14749eb61"
$existing = powercfg -list | Select-String $ultimate
if (-not $existing) {
    powercfg -duplicatescheme $ultimate 2>$null
}
powercfg -setactive $ultimate 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  Ultimate Performance not available, using High Performance" -ForegroundColor DarkYellow
    powercfg -setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
}

# Lock CPU frequency to 100% (no throttling)
$sub = "54533251-82be-4824-96c1-47b60b740d00"
powercfg -setacvalueindex SCHEME_CURRENT $sub 893dee8e-2bef-41e0-89c6-b55d0929964c 100  # Min processor state
powercfg -setacvalueindex SCHEME_CURRENT $sub bc5038f7-23e0-4960-96da-33abaf5935ec 100  # Max processor state
# Disable core parking (keep all cores active)
powercfg -setacvalueindex SCHEME_CURRENT $sub 0cc5b647-c1df-4637-891a-dec35c318583 100  # Min cores parked = 100%
powercfg -setactive SCHEME_CURRENT
Write-Host "  Power plan configured: CPU 100%, core parking disabled" -ForegroundColor Green

# ---- 2. TCP Stack Optimization (system-wide, affects CTP SDK) ----
Write-Host "`n[2/4] TCP Stack..." -ForegroundColor Yellow
$tcpParams = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters"
New-ItemProperty -Path $tcpParams -Name "TcpAckFrequency" -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $tcpParams -Name "TCPNoDelay" -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $tcpParams -Name "TcpDelAckTicks" -Value 0 -PropertyType DWord -Force | Out-Null

$interfaces = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces"
Get-ChildItem $interfaces | ForEach-Object {
    New-ItemProperty -Path $_.PSPath -Name "TcpAckFrequency" -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $_.PSPath -Name "TCPNoDelay" -Value 1 -PropertyType DWord -Force | Out-Null
}
Write-Host "  TCP_NODELAY=1, TcpAckFrequency=1, TcpDelAckTicks=0" -ForegroundColor Green

# ---- 3. Kernel Boot Configuration ----
Write-Host "`n[3/4] bcdedit kernel tuning..." -ForegroundColor Yellow
bcdedit /set disabledynamictick yes 2>$null | Out-Null
bcdedit /set useplatformtick yes 2>$null | Out-Null
bcdedit /set tscsyncpolicy enhanced 2>$null | Out-Null
Write-Host "  disabledynamictick=yes, useplatformtick=yes, tscsyncpolicy=enhanced" -ForegroundColor Green
Write-Host "  ** Reboot required for bcdedit changes **" -ForegroundColor DarkYellow

# ---- 4. Disable noisy background services ----
Write-Host "`n[4/4] Disabling background services..." -ForegroundColor Yellow
$services = @(
    @{Name="SysMain";           Desc="Superfetch (memory bandwidth waste)"},
    @{Name="DiagTrack";         Desc="Diagnostics Tracking (background IO)"},
    @{Name="WSearch";           Desc="Windows Search Indexer (disk IO spikes)"},
    @{Name="MapsBroker";        Desc="Maps download service"},
    @{Name="TabletInputService"; Desc="Touch input service"}
)
foreach ($svc in $services) {
    $s = Get-Service -Name $svc.Name -ErrorAction SilentlyContinue
    if ($s) {
        if ($s.Status -eq "Running") {
            Stop-Service $svc.Name -Force -ErrorAction SilentlyContinue
        }
        Set-Service $svc.Name -StartupType Disabled -ErrorAction SilentlyContinue
        Write-Host "  Disabled: $($svc.Name) - $($svc.Desc)" -ForegroundColor Green
    } else {
        Write-Host "  Skipped: $($svc.Name) (not found)" -ForegroundColor DarkGray
    }
}

Write-Host "`n=== OS Tuning Complete ===" -ForegroundColor Cyan
Write-Host "Next: run nic_hft_tuning.ps1 for NIC-specific tuning" -ForegroundColor White
Write-Host "Reboot to apply bcdedit changes, then run verify_tuning.ps1" -ForegroundColor White
