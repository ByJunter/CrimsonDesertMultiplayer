# Builds the co-op test kit: dist\cdmp-coop-<date>.zip with the ASI loader,
# the mod, the host's and the guest's configs, the relay (cdmp_peer.exe) and Spanish instructions.
#
#   .\scripts\package_coop.ps1            # uses the RelWithDebInfo build

param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\bin\$Config"
foreach ($f in 'cdmp.asi', 'cdmp_peer.exe') {
    if (-not (Test-Path (Join-Path $bin $f))) { throw "Build first: $f not found in $bin" }
}

$stamp = Get-Date -Format 'yyyyMMdd'
$staging = Join-Path $root "dist\cdmp-coop-$stamp"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null

Copy-Item (Join-Path $root 'third_party\ultimate-asi-loader\v9.7.4\dinput8.dll') (Join-Path $staging 'winmm.dll')
Copy-Item (Join-Path $bin 'cdmp.asi') $staging
Copy-Item (Join-Path $bin 'cdmp_peer.exe') $staging
# Two profiles; cdmp.ini starts as the guest's (the host renames theirs).
Copy-Item (Join-Path $root 'kit\coop\cdmp.ini.anfitrion') $staging
Copy-Item (Join-Path $root 'kit\coop\cdmp.ini.invitado') $staging
Copy-Item (Join-Path $root 'kit\coop\cdmp.ini.invitado') (Join-Path $staging 'cdmp.ini')
Copy-Item (Join-Path $root 'kit\coop\LEEME.txt') $staging

Get-ChildItem $staging -File | Where-Object Name -ne 'SHA256.txt' | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii (Join-Path $staging 'SHA256.txt')

$zip = "$staging.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip
Write-Host "Co-op kit: $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
