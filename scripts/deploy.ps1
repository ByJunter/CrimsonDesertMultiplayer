# Installs (or removes) the mod in the game folder.
#
#   .\scripts\deploy.ps1                 # copy cdmp.asi (+pdb) and the ASI loader
#   .\scripts\deploy.ps1 -Remove         # remove everything this script installed
#   .\scripts\deploy.ps1 -GameDir "E:\...\Crimson Desert\bin64"
#
# The game imports WINMM.dll, so Ultimate ASI Loader is installed as
# bin64\winmm.dll. cdmp.ini and cdmp.log live next to cdmp.asi; an existing
# cdmp.ini is never overwritten.

param(
    [string]$GameDir = $(if ($env:CDMP_GAME_DIR) { $env:CDMP_GAME_DIR } else { 'D:\SteamLibrary\steamapps\common\Crimson Desert\bin64' }),
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$loaderSrc = Join-Path $root 'third_party\ultimate-asi-loader\v9.7.4\dinput8.dll'
$loaderDst = Join-Path $GameDir 'winmm.dll'

if (-not (Test-Path (Join-Path $GameDir 'CrimsonDesert.exe'))) {
    throw "CrimsonDesert.exe not found in $GameDir"
}
if (Get-Process CrimsonDesert -ErrorAction SilentlyContinue) {
    throw 'The game is running; close it first (the files are locked).'
}

if ($Remove) {
    foreach ($f in 'cdmp.asi', 'cdmp.pdb') {
        $p = Join-Path $GameDir $f
        if (Test-Path $p) { Remove-Item $p; Write-Host "Removed $f" }
    }
    # Only remove winmm.dll if it is the loader we installed.
    if ((Test-Path $loaderDst) -and
        (Get-FileHash $loaderDst).Hash -eq (Get-FileHash $loaderSrc).Hash) {
        Remove-Item $loaderDst
        Write-Host 'Removed winmm.dll (Ultimate ASI Loader)'
    }
    Write-Host 'cdmp.ini / cdmp.log were left in place.'
    return
}

$bin = Join-Path $root "build\bin\$Config"
if (-not (Test-Path "$bin\cdmp.asi")) { throw "Build first: $bin\cdmp.asi not found" }

Copy-Item "$bin\cdmp.asi" $GameDir -Force
if (Test-Path "$bin\cdmp.pdb") { Copy-Item "$bin\cdmp.pdb" $GameDir -Force }
Write-Host "Installed cdmp.asi ($Config) -> $GameDir"

if (-not (Test-Path $loaderDst)) {
    Copy-Item $loaderSrc $loaderDst
    Write-Host 'Installed Ultimate ASI Loader as winmm.dll'
} elseif ((Get-FileHash $loaderDst).Hash -ne (Get-FileHash $loaderSrc).Hash) {
    Write-Warning 'A different winmm.dll already exists in the game folder; left it untouched.'
}
