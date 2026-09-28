param(
    [string]$Config = "C:\ProgramData\SunshineSeat\config\sunshine.conf",
    [string]$PrimaryConfigDir = "C:\ProgramData\SunshineSeat\primary",
    [string]$SunshineExe = "C:\Program Files\Sunshine\sunshine.exe",
    [string]$HealthExe = "C:\Program Files\Sunshine\tools\sunshineseat-health.exe",
    [string]$ApolloService = "ApolloService",
    [switch]$Verify,
    [switch]$Execute,
    [switch]$ConfirmCutover
)

$ErrorActionPreference = "Stop"

function Add-Step {
    param([string]$Message)
    Write-Host "  - $Message"
}

Write-Host "SunshineSeat cutover"
Write-Host "  sidecar_config: $Config"
Write-Host "  primary_config_dir: $PrimaryConfigDir"
Write-Host "  sunshine: $SunshineExe"
Write-Host "  health: $HealthExe"
Write-Host "  apollo_service: $ApolloService"
Write-Host "  execute: $($Execute.IsPresent)"

if (-not (Test-Path -LiteralPath $Config)) {
    throw "Missing SunshineSeat config: $Config"
}
if (-not (Test-Path -LiteralPath $SunshineExe)) {
    throw "Missing SunshineSeat executable: $SunshineExe"
}
if (-not (Test-Path -LiteralPath $HealthExe)) {
    throw "Missing SunshineSeat health tool: $HealthExe"
}

Add-Step "Running side-by-side health gate."
& $HealthExe --config $Config --expect-side-by-side
if ($LASTEXITCODE -ne 0) {
    throw "Health gate failed; refusing cutover."
}

$apollo = Get-Service -Name $ApolloService -ErrorAction Stop
Add-Step "Apollo service status is $($apollo.Status)."
if ($apollo.Status -ne "Running") {
    throw "Apollo service is not running; refusing cutover because rollback source is not healthy."
}

if ($Verify -and -not $Execute) {
    Add-Step "Verify-only mode passed. No service or process changes were made."
    exit 0
}

if (-not $Execute) {
    Add-Step "Dry-run only. Re-run with -Execute -ConfirmCutover after Moonlight side-by-side streaming is verified."
    exit 0
}

if (-not $ConfirmCutover) {
    throw "Refusing live cutover without -ConfirmCutover."
}

$sidecarDir = Split-Path -Parent $Config
$primaryConfig = Join-Path $PrimaryConfigDir "sunshine.conf"
Add-Step "Preparing primary config at $PrimaryConfigDir."
if (Test-Path -LiteralPath $PrimaryConfigDir) {
    Remove-Item -LiteralPath $PrimaryConfigDir -Recurse -Force
}
Copy-Item -LiteralPath $sidecarDir -Destination $PrimaryConfigDir -Recurse -Force

$configText = Get-Content -LiteralPath $primaryConfig -Raw
$configText = $configText -replace '(?m)^port\s*=.*$', 'port = 47989'
$configText = $configText -replace '(?m)^sunshine_name\s*=.*$', 'sunshine_name = SunshineSeat'
$configText = $configText -replace '(?m)^sunshineseat_mode\s*=.*$', 'sunshineseat_mode = primary'
$configText = $configText -replace [regex]::Escape($sidecarDir), $PrimaryConfigDir
Set-Content -LiteralPath $primaryConfig -Value $configText -Encoding ASCII

Add-Step "Stopping Apollo service."
Stop-Service -Name $ApolloService -ErrorAction Stop

Start-Sleep -Seconds 2

Add-Step "Starting SunshineSeat on the primary config."
$process = Start-Process -FilePath $SunshineExe -ArgumentList @($primaryConfig) -PassThru -WindowStyle Hidden
Add-Step "Started SunshineSeat pid=$($process.Id)."

Start-Sleep -Seconds 4

Add-Step "Running post-start health check."
& $HealthExe --config $primaryConfig
if ($LASTEXITCODE -ne 0) {
    Write-Warning "Post-start health failed. Run sunshineseat-rollback.ps1 immediately."
    exit 1
}

Write-Host "Cutover command completed. Confirm Moonlight before changing startup automation."
