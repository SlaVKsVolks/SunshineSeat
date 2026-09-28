param(
    [string]$ApolloService = "ApolloService",
    [string]$ProcessName = "sunshine",
    [switch]$Execute,
    [switch]$ConfirmRollback
)

$ErrorActionPreference = "Stop"

function Add-Step {
    param([string]$Message)
    Write-Host "  - $Message"
}

Write-Host "SunshineSeat rollback"
Write-Host "  apollo_service: $ApolloService"
Write-Host "  process_name: $ProcessName"
Write-Host "  execute: $($Execute.IsPresent)"

$sunshineProcesses = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if ($sunshineProcesses) {
    foreach ($process in $sunshineProcesses) {
        Add-Step "Detected $ProcessName pid=$($process.Id) session=$($process.SessionId)."
    }
} else {
    Add-Step "No $ProcessName process detected."
}

$apollo = Get-Service -Name $ApolloService -ErrorAction Stop
Add-Step "Apollo service status is $($apollo.Status)."

if (-not $Execute) {
    Add-Step "Dry-run only. Re-run with -Execute -ConfirmRollback to stop SunshineSeat and start Apollo."
    exit 0
}

if (-not $ConfirmRollback) {
    throw "Refusing live rollback without -ConfirmRollback."
}

if ($sunshineProcesses) {
    foreach ($process in $sunshineProcesses) {
        Add-Step "Stopping $ProcessName pid=$($process.Id)."
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
    }
}

Start-Sleep -Seconds 2

$apollo = Get-Service -Name $ApolloService -ErrorAction Stop
if ($apollo.Status -ne "Running") {
    Add-Step "Starting Apollo service."
    Start-Service -Name $ApolloService -ErrorAction Stop
} else {
    Add-Step "Apollo service already running."
}

Start-Sleep -Seconds 4

$ports = @(47984, 47989, 47990, 48010)
$listeners = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort }
foreach ($port in $ports) {
    $owner = $listeners | Where-Object { $_.LocalPort -eq $port } | Select-Object -First 1
    if ($owner) {
        Add-Step "Default port $port is listening pid=$($owner.OwningProcess)."
    } else {
        Write-Warning "Default port $port is not listening after rollback."
    }
}

Write-Host "Rollback command completed."
