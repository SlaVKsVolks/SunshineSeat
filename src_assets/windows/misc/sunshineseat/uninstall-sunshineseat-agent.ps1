param(
    [string]$ServiceName = "SunshineSeatAgent"
)

$ErrorActionPreference = "Stop"

& sc.exe stop $ServiceName 2>$null | Out-Null
Start-Sleep -Seconds 2
& sc.exe delete $ServiceName | Out-Null

Write-Host "Uninstalled $ServiceName"
