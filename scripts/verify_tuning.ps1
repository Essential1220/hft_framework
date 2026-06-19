# verify_tuning.ps1 - Verify HFT OS tuning is active
# Does NOT require admin (read-only checks).

Set-StrictMode -Version Latest
$pass = 0; $fail = 0; $skip = 0

function Check($name, $condition) {
    if ($condition) {
        Write-Host "  [PASS] $name" -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host "  [FAIL] $name" -ForegroundColor Red
        $script:fail++
    }
}

Write-Host "=== HFT Tuning Verification ===" -ForegroundColor Cyan

# ---- Power Plan ----
Write-Host "`n-- Power Plan --" -ForegroundColor Yellow
$activePlan = (powercfg /getactivescheme 2>$null) -replace '.*:\s*',''
$isUltimate = $activePlan -match "e9a42b02|Ultimate|卓越"
$isHigh = $activePlan -match "8c5e7fda|High Performance|高性能"
Check "Power plan is Ultimate/High Performance" ($isUltimate -or $isHigh)

# ---- TCP Settings ----
Write-Host "`n-- TCP Stack --" -ForegroundColor Yellow
$tcpParams = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters"
$noDelay = (Get-ItemProperty $tcpParams -Name "TCPNoDelay" -ErrorAction SilentlyContinue).TCPNoDelay
$ackFreq = (Get-ItemProperty $tcpParams -Name "TcpAckFrequency" -ErrorAction SilentlyContinue).TcpAckFrequency
$delAck = (Get-ItemProperty $tcpParams -Name "TcpDelAckTicks" -ErrorAction SilentlyContinue).TcpDelAckTicks
Check "TCPNoDelay = 1 (Nagle disabled)" ($noDelay -eq 1)
Check "TcpAckFrequency = 1 (immediate ACK)" ($ackFreq -eq 1)
Check "TcpDelAckTicks = 0 (no delayed ACK)" ($delAck -eq 0)

# ---- bcdedit ----
Write-Host "`n-- Kernel Boot Config --" -ForegroundColor Yellow
$bcd = bcdedit /enum "{current}" 2>$null | Out-String
Check "disabledynamictick = Yes" ($bcd -match "disabledynamictick\s+Yes")
Check "useplatformtick = Yes" ($bcd -match "useplatformtick\s+Yes")
Check "tscsyncpolicy = Enhanced" ($bcd -match "tscsyncpolicy\s+Enhanced")

# ---- Services ----
Write-Host "`n-- Background Services --" -ForegroundColor Yellow
$svcNames = @("SysMain", "DiagTrack", "WSearch")
foreach ($name in $svcNames) {
    $s = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($s) {
        Check "$name is Disabled/Stopped" ($s.Status -ne "Running")
    } else {
        Write-Host "  [SKIP] $name (not found)" -ForegroundColor DarkGray
        $skip++
    }
}

# ---- NIC ----
Write-Host "`n-- NIC Settings --" -ForegroundColor Yellow
$adapters = Get-NetAdapter | Where-Object Status -eq 'Up'
if ($adapters) {
    $nic = $adapters[0].Name
    Write-Host "  Adapter: $nic" -ForegroundColor White
    $im = Get-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*InterruptModeration" -ErrorAction SilentlyContinue
    if ($im) {
        Check "InterruptModeration = OFF" ($im.RegistryValue -eq "0")
    } else {
        Write-Host "  [SKIP] InterruptModeration (not exposed)" -ForegroundColor DarkGray
        $skip++
    }
} else {
    Write-Host "  [SKIP] No active adapter" -ForegroundColor DarkGray
    $skip++
}

# ---- Summary ----
Write-Host "`n=== Results: $pass PASS, $fail FAIL, $skip SKIP ===" -ForegroundColor $(if ($fail -eq 0) {"Green"} else {"Red"})
if ($fail -gt 0) {
    Write-Host "Run windows_hft_tuning.ps1 and/or nic_hft_tuning.ps1 as admin to fix" -ForegroundColor Yellow
}
