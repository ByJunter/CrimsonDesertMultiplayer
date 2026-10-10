# Builds the capture kit for helpers: dist\cdmp-kit-<date>.zip with the ASI
# loader, the mod and a sniffer-only config + Spanish instructions.
#
#   .\scripts\package_kit.ps1            # uses the RelWithDebInfo build

param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$asi = Join-Path $root "build\bin\$Config\cdmp.asi"
if (-not (Test-Path $asi)) { throw "Build first: $asi not found" }

$stamp = Get-Date -Format 'yyyyMMdd'
$staging = Join-Path $root "dist\cdmp-kit-$stamp"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null

Copy-Item (Join-Path $root 'third_party\ultimate-asi-loader\v9.7.4\dinput8.dll') (Join-Path $staging 'winmm.dll')
Copy-Item $asi $staging
Copy-Item (Join-Path $root 'kit\cdmp.ini') $staging
Copy-Item (Join-Path $root 'kit\LEEME.txt') $staging

# Checksums so the helper (or we) can verify the files arrived intact.
Get-ChildItem $staging -File | Where-Object Name -ne 'SHA256.txt' | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii (Join-Path $staging 'SHA256.txt')

$zip = "$staging.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip
Write-Host "Kit: $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
