# Builds the Soulseek sidecar folder: embedded Python 3.11 + aioslsk + slsk_sync.py, as the original build's CI does.
#   .\scripts\bundle_soulseek.ps1 [-OutDir build\Release\soulseek]
# Needs a Python 3 with pip on PATH (only to download aioslsk's wheels for 3.11 / win_amd64) and internet.
param([string]$OutDir = 'build\Release\soulseek')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }

$version = '3.11.9'
$sha256 = 'RECORDED-BY-FIRST-RUN'
$cache = Join-Path $root "build\_deps\python-$version-embed-amd64.zip"
New-Item -ItemType Directory -Force (Split-Path $cache) | Out-Null
if (-not (Test-Path $cache)) {
    Invoke-WebRequest "https://www.python.org/ftp/python/$version/python-$version-embed-amd64.zip" -OutFile $cache
}
$actual = (Get-FileHash $cache -Algorithm SHA256).Hash.ToLower()
$pin = Join-Path $PSScriptRoot 'python-embed.sha256'
if (Test-Path $pin) {
    if ($actual -ne (Get-Content $pin).Trim()) { Remove-Item $cache; throw "python embeddable zip changed: $actual" }
} else {
    Set-Content $pin $actual   # first run records it; commit the file
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\python" | Out-Null
Expand-Archive $cache -DestinationPath "$out\python"
$pth = "$out\python\python311._pth"
(Get-Content $pth) -replace '#import site', 'import site' | Set-Content $pth
Add-Content $pth 'Lib\site-packages'
# Any Python with pip will do (it only fetches the wheels); the first that works is used.
$pip = @('py -3', 'python', 'python3') | Where-Object { & cmd /c "$_ -m pip --version" 2>$null; $LASTEXITCODE -eq 0 } | Select-Object -First 1
if (-not $pip) { throw 'Needs a Python 3 with pip on PATH (py -3, python or python3)' }
& cmd /c "$pip -m pip install --quiet --no-compile --python-version 3.11 --platform win_amd64 --only-binary=:all: --target `"$out\python\Lib\site-packages`" aioslsk"
if ($LASTEXITCODE) { throw 'pip could not fetch aioslsk' }
Copy-Item (Join-Path $root 'sidecar\slsk_sync.py') $out

# Smoke test: the sidecar imports and prints its help with the bundled interpreter.
$help = & "$out\python\python.exe" "$out\slsk_sync.py" --help
if ($LASTEXITCODE -or -not ($help -match 'usage: slsk-sync')) { throw 'the bundled sidecar does not start' }
$help | Select-Object -First 1
"Soulseek sidecar ready in $out ($([math]::Round(((Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1)) MB)"
