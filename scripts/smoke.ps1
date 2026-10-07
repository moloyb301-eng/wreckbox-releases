# Smoke test of a built or unzipped WreckBox folder, as CI and a clean PC would run it:
#   .\scripts\smoke.ps1 -Dir <folder with wreckbox.exe>        (wbcore.exe is used if it is there)
# 1. the engine analyses a generated click track, writes tags and reads them back (needs wbcore.exe),
# 2. the app starts with a throwaway library, draws its window and stays up,
# 3. the Soulseek sidecar (if bundled) starts with its own Python and prints its help.
param([Parameter(Mandatory)][string]$Dir, [string]$WbCore = '')
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("wreckbox-smoke-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
try {
    # 1. The engine.
    $core = if ($WbCore) { $WbCore } elseif (Test-Path "$Dir\wbcore.exe") { "$Dir\wbcore.exe" } else { $null }
    if ($core) {
        $wav = "$work\t.wav"
        # A 20 s, 120 BPM click track.
        $rate = 22050; $period = [int]($rate * 60 / 120); $n = $rate * 20
        $pcm = New-Object byte[] ($n * 2)
        for ($i = 0; $i -lt $n; $i++) {
            if ($i % $period -lt 200) { $v = [int](12000 * [math]::Sin($i * 0.3)); $pcm[2 * $i] = $v -band 0xFF; $pcm[2 * $i + 1] = ($v -shr 8) -band 0xFF }
        }
        $fs = [IO.File]::Create($wav); $bw = New-Object IO.BinaryWriter $fs
        $bw.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $bw.Write([int](36 + $pcm.Length)); $bw.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]1); $bw.Write([int]$rate); $bw.Write([int]($rate * 2)); $bw.Write([int16]2); $bw.Write([int16]16)
        $bw.Write([Text.Encoding]::ASCII.GetBytes('data')); $bw.Write([int]$pcm.Length); $bw.Write($pcm); $bw.Close()
        $analysis = & $core analyze $wav | ConvertFrom-Json
        if ([math]::Abs($analysis.result.bpm - 120) -gt 2) { throw "analysis: expected ~120 BPM, got $($analysis.result.bpm)" }
        $job = '[{"path":"' + ($wav -replace '\\', '\\\\') + '","title":"Smoke","artists":["CI"],"bpm":120,"key":"A minor"}]'
        $job | & $core write-tags | Out-Null
        $tags = (& $core tags $wav) -join ''
        if ($tags -notmatch '"title":\s*"Smoke"') { throw "tag round trip failed: $tags" }
        "engine: $([math]::Round($analysis.result.bpm, 1)) BPM, key $($analysis.result.key), tags round-trip ok"
    } else { 'engine: wbcore.exe not here, skipped' }

    # 2. The app. A hidden window still has a handle; it must be up after 4 s and exit when closed.
    $env:WRECKBOX_NO_UPDATE_CHECK = '1'
    $p = Start-Process "$Dir\wreckbox.exe" -ArgumentList '--root', "$work\lib", '--background' -PassThru
    Start-Sleep 4
    if ($p.HasExited) { throw "wreckbox.exe exited with code $($p.ExitCode)" }
    $p.Refresh()
    if ($p.MainWindowHandle -eq 0) { throw 'wreckbox.exe is running but has no window' }
    $null = $p.CloseMainWindow()
    if (-not $p.WaitForExit(10000)) { $p.Kill(); throw 'wreckbox.exe did not close' }
    if ($p.ExitCode -ne 0) { throw "wreckbox.exe crashed on exit (code $($p.ExitCode))" }
    "app: started, drew its window, closed cleanly"

    # 3. The Soulseek sidecar.
    if (Test-Path "$Dir\soulseek\python\python.exe") {
        $help = & "$Dir\soulseek\python\python.exe" "$Dir\soulseek\slsk_sync.py" --help
        if ($LASTEXITCODE -or -not ($help -match 'usage: slsk-sync')) { throw 'the Soulseek sidecar does not start' }
        'soulseek: sidecar starts with its bundled Python'
    } else { 'soulseek: not bundled, skipped' }
    'SMOKE TEST PASSED'
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
