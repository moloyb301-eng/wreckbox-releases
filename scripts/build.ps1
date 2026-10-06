# Configure, build and test. Uses the CMake and vcpkg that ship with Visual Studio 2022 / Build Tools,
# so nothing else needs installing. Usage: .\scripts\build.ps1 [-Debug] [-NoTest]
param([switch]$Debug, [switch]$NoTest)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$config = if ($Debug) { 'Debug' } else { 'Release' }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 (or Build Tools) with the C++ workload is required.' }

$cmakeBin = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$cmake = if (Get-Command cmake -ErrorAction SilentlyContinue) { 'cmake' } else { "$cmakeBin\cmake.exe" }
$ctest = if (Get-Command ctest -ErrorAction SilentlyContinue) { 'ctest' } else { "$cmakeBin\ctest.exe" }
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }

& $cmake -S $root -B "$root\build" -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
if ($LASTEXITCODE) { throw 'configure failed' }

& $cmake --build "$root\build" --config $config --parallel
if ($LASTEXITCODE) { throw 'build failed' }

if (-not $NoTest) {
  & $ctest --test-dir "$root\build" -C $config --output-on-failure
  if ($LASTEXITCODE) { throw 'tests failed' }
}
