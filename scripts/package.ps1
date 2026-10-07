# Assembles the release zip from a finished build: WreckBox-<version>-win-native-x64.zip in dist\.
#   .\scripts\package.ps1            (after .\scripts\build.ps1 and .\scripts\bundle_soulseek.ps1)
# The version comes from CMakeLists.txt (project(VERSION)): the one place it is set.
param([string]$Config = 'Release', [string]$Out = 'dist', [switch]$NoSoulseek)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build\$Config"
$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(wreckbox VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "WreckBox-$version-win-native-x64"
$stage = Join-Path $root "$Out\$name"
$zip = Join-Path $root "$Out\$name.zip"

# What a user needs, and nothing else (no tests, libraries or symbols from the build folder).
$files = 'wreckbox.exe', 'libvlc.dll', 'libvlccore.dll'
$folders = 'plugins', 'milkdrop', 'licenses'
if (-not $NoSoulseek) { $folders += 'soulseek' }
foreach ($f in $files + $folders) {
    if (-not (Test-Path (Join-Path $build $f))) { throw "missing $f in $build (run build.ps1$(if ($f -eq 'soulseek') { ' and bundle_soulseek.ps1' }) first)" }
}

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $build $f) $stage }
foreach ($f in $folders) { Copy-Item -Recurse (Join-Path $build $f) $stage }
Copy-Item (Join-Path $root 'docs\INSTALL.md') (Join-Path $stage 'README.md')

if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
"$zip ($size MB)"
