# Registers this checkout's SteamVR driver (driver\questlhsync) with SteamVR, after build.bat. SteamVR closed.
#   powershell -executionpolicy bypass -file pico\register_driver.ps1          register
#   powershell -executionpolicy bypass -file pico\register_driver.ps1 -remove  unregister
param([switch]$Remove)
$driver = Join-Path (Split-Path $PSScriptRoot -Parent) 'driver\questlhsync'
$dll = Join-Path $driver 'bin\win64\driver_questlhsync.dll'
if (-not $Remove -and -not (Test-Path $dll)) { Write-Host "no $dll yet: run build.bat first"; exit 1 }
# SteamVR's runtime folder, from OpenVR's own path file
$paths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
if (-not (Test-Path $paths)) { Write-Host "SteamVR not found ($paths missing): run SteamVR once"; exit 1 }
$runtime = (Get-Content $paths -Raw | ConvertFrom-Json).runtime | Where-Object { Test-Path (Join-Path $_ 'bin\win64\vrpathreg.exe') } | Select-Object -First 1
if (-not $runtime) { Write-Host "vrpathreg.exe not found in the SteamVR runtime folders of $paths"; exit 1 }
$vrpathreg = Join-Path $runtime 'bin\win64\vrpathreg.exe'
if (Get-Process vrserver -ErrorAction SilentlyContinue) { Write-Host 'close SteamVR first'; exit 1 }
& $vrpathreg ($(if ($Remove) { 'removedriver' } else { 'adddriver' })) $driver
& $vrpathreg show | Select-String -SimpleMatch 'questlhsync'
Write-Host ($(if ($Remove) { "removed $driver" } else { "registered $driver" }))
