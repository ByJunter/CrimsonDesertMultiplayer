# Builds the public release: dist\release\CrimsonDesertMultiplayer-v<version>.zip
# with the ASI loader, the mod, the host's and the guest's configs, the
# instructions (Spanish and English) and the third-party licenses; plus a
# separate zip with the debug symbols (cdmp.pdb), to keep and NOT to publish.
# The version is the one in CMakeLists.txt. Build first (.\scripts\build.ps1).
#
#   .\scripts\package_release.ps1

param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\bin\$Config"
foreach ($f in 'cdmp.asi', 'cdmp.pdb') {
    if (-not (Test-Path (Join-Path $bin $f))) { throw "Build first: $f not found in $bin" }
}
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(\w+ VERSION ([0-9.]+)') { throw 'No VERSION in CMakeLists.txt' }
$version = $Matches[1]

$name = "CrimsonDesertMultiplayer-v$version"
$out = Join-Path $root 'dist\release'
$staging = Join-Path $out $name
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null

$public = Join-Path $root 'kit\public'
Copy-Item (Join-Path $root 'third_party\ultimate-asi-loader\v9.7.4\dinput8.dll') (Join-Path $staging 'winmm.dll')
Copy-Item (Join-Path $bin 'cdmp.asi') $staging
# Two profiles; cdmp.ini starts as the guest's (the host renames theirs).
foreach ($f in 'cdmp.ini.anfitrion', 'cdmp.ini.invitado', 'LEEME.txt', 'README.txt', 'THIRD-PARTY-NOTICES.txt') {
    Copy-Item (Join-Path $public $f) $staging
}
Copy-Item (Join-Path $public 'cdmp.ini.invitado') (Join-Path $staging 'cdmp.ini')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $staging 'LICENSE.txt')

Get-ChildItem $staging -File | Where-Object Name -ne 'SHA256.txt' | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii (Join-Path $staging 'SHA256.txt')

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip

# Symbols of exactly this cdmp.asi: needed to read a crash dump later.
$symbols = Join-Path $out "$name-symbols-NO-PUBLICAR.zip"
if (Test-Path $symbols) { Remove-Item $symbols }
Compress-Archive -Path (Join-Path $bin 'cdmp.pdb'), (Join-Path $bin 'cdmp.asi') -DestinationPath $symbols

Write-Host "Release: $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
Write-Host "Symbols (keep, do not publish): $symbols"
