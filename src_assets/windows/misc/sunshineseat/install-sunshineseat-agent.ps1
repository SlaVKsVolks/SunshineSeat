param(
    [string]$InstallRoot = "C:\Program Files\Sunshine",
    [string]$ServiceName = "SunshineSeatAgent"
)

$ErrorActionPreference = "Stop"

$agentCandidates = @(
    (Join-Path $InstallRoot "tools\SunshineSeatAgent.exe"),
    (Join-Path $InstallRoot "bin\SunshineSeatAgent.exe")
)

$agent = $agentCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $agent) {
    throw "SunshineSeatAgent.exe not found. Checked: $($agentCandidates -join ', ')"
}

$existing = & sc.exe query $ServiceName 2>$null
if ($LASTEXITCODE -eq 0) {
    & sc.exe stop $ServiceName | Out-Null
    Start-Sleep -Seconds 2
    & sc.exe delete $ServiceName | Out-Null
    Start-Sleep -Seconds 2
}

& sc.exe create $ServiceName binPath= "`"$agent`"" start= auto obj= LocalSystem DisplayName= "SunshineSeat Agent" | Out-Null
& sc.exe description $ServiceName "LocalSystem recovery agent for SunshineSeat and Apollo fallback availability." | Out-Null
& sc.exe failure $ServiceName reset= 300 actions= restart/5000/restart/15000/""/30000 | Out-Null
& sc.exe start $ServiceName | Out-Null

Write-Host "Installed and started $ServiceName"
