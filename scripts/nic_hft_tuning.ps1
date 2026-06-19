#Requires -RunAsAdministrator
# nic_hft_tuning.ps1 - NIC tuning for HFT low-latency trading
# Adjusts the primary active network adapter for minimum latency.
# Run as Administrator.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

Write-Host "=== HFT NIC Tuning ===" -ForegroundColor Cyan

$adapters = Get-NetAdapter | Where-Object Status -eq 'Up'
if (-not $adapters) {
    Write-Host "ERROR: No active network adapters found" -ForegroundColor Red
    exit 1
}
$nic = $adapters[0].Name
Write-Host "Target adapter: $nic" -ForegroundColor White

# ---- 1. Disable Interrupt Moderation ----
Write-Host "`n[1/5] Interrupt Moderation..." -ForegroundColor Yellow
$im = Get-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*InterruptModeration" -ErrorAction SilentlyContinue
if ($im) {
    Set-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*InterruptModeration" -RegistryValue 0
    Write-Host "  InterruptModeration = OFF (every packet triggers interrupt)" -ForegroundColor Green
} else {
    Write-Host "  Skipped: adapter does not expose InterruptModeration" -ForegroundColor DarkGray
}

# ---- 2. RSS Base Processor ----
Write-Host "`n[2/5] RSS (Receive Side Scaling)..." -ForegroundColor Yellow
try {
    Set-NetAdapterRss -Name $nic -BaseProcessorNumber 2 -MaxProcessorNumber 5 -ErrorAction Stop
    Write-Host "  RSS pinned to cores 2-5 (keep core 0-1 for engine/logger)" -ForegroundColor Green
} catch {
    Write-Host "  Skipped: RSS configuration not supported on this adapter" -ForegroundColor DarkGray
}

# ---- 3. Receive Buffers ----
Write-Host "`n[3/5] Receive Buffers..." -ForegroundColor Yellow
$rb = Get-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*ReceiveBuffers" -ErrorAction SilentlyContinue
if ($rb) {
    Set-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*ReceiveBuffers" -RegistryValue 2048
    Write-Host "  ReceiveBuffers = 2048" -ForegroundColor Green
} else {
    Write-Host "  Skipped: adapter does not expose ReceiveBuffers" -ForegroundColor DarkGray
}

# ---- 4. Disable Energy Efficient Ethernet ----
Write-Host "`n[4/5] Energy Efficient Ethernet..." -ForegroundColor Yellow
$eee = Get-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*EEE" -ErrorAction SilentlyContinue
if ($eee) {
    Set-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*EEE" -RegistryValue 0
    Write-Host "  EEE = OFF" -ForegroundColor Green
} else {
    Write-Host "  Skipped: adapter does not expose EEE" -ForegroundColor DarkGray
}

$fc = Get-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*FlowControl" -ErrorAction SilentlyContinue
if ($fc) {
    Set-NetAdapterAdvancedProperty -Name $nic -RegistryKeyword "*FlowControl" -RegistryValue 0
    Write-Host "  FlowControl = OFF" -ForegroundColor Green
}

# ---- 5. Disable Power Management ----
Write-Host "`n[5/5] Power Management..." -ForegroundColor Yellow
try {
    Disable-NetAdapterPowerManagement -Name $nic -ErrorAction Stop
    Write-Host "  Adapter power management disabled" -ForegroundColor Green
} catch {
    Write-Host "  Skipped: power management control not available" -ForegroundColor DarkGray
}

Write-Host "`n=== NIC Tuning Complete ===" -ForegroundColor Cyan
Write-Host "Adapter '$nic' configured for minimum latency" -ForegroundColor White
