# Rebuilds tests/fixtures/audio — the short clips player_tests plays in every format. The results are committed, so
# this only needs running to add a format. Three encoders, because no single one makes them all well:
#   1. make_audio_fixtures.py: tone.wav (2.5 s: 100 Hz + 1 kHz + 5 kHz), tone.mod (ProTracker), two.m3u
#   2. the full VLC in build/_deps (after one build): Ogg Vorbis, Opus, AAC, MPEG layer 2
#   3. Windows' own encoders (tools/mf_encode.cpp): FLAC, WMA, ALAC — VLC's FLAC had no length and its WMA was broken
#   4. ffmpeg (-FFmpeg path; ShareX ships one): MP3, WavPack, TTA, AC-3, E-AC-3, DTS, 24-bit and ADPCM WAV
# mf_encode.exe is built if missing; that step needs a "x64 Native Tools" prompt (cl.exe on PATH).
param([string]$FFmpeg = 'C:\Program Files\ShareX\ffmpeg.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = "$root\tests\fixtures\audio"
$tone = "$dir\tone.wav"
function report($name) {
  $f = Get-Item "$dir\$name" -ErrorAction SilentlyContinue
  "{0,-16} {1}" -f $name, $(if ($f -and $f.Length -gt 0) { "$($f.Length) bytes" } else { 'FAILED' })
}

& python "$PSScriptRoot\make_audio_fixtures.py" $dir

$vlc = "$root\build\_deps\vlc\vlc-3.0.24\vlc.exe"
foreach ($j in @(
    @{ out = 'tone.ogg';  sout = 'transcode{acodec=vorb,ab=96,channels=2}:std{access=file,mux=ogg' },
    @{ out = 'tone.opus'; sout = 'transcode{acodec=opus,ab=96,channels=2,samplerate=48000}:std{access=file,mux=ogg' },
    @{ out = 'tone.m4a';  sout = 'transcode{acodec=mp4a,ab=128,channels=2}:std{access=file,mux=mp4' },
    @{ out = 'tone.mp2';  sout = 'transcode{acodec=mpga,ab=128,channels=2}:std{access=file,mux=raw' })) {
  $dst = "$dir\$($j.out)" -replace '\\', '/'
  & $vlc -I dummy --no-repeat --no-loop $tone --sout "#$($j.sout),dst='$dst'}" vlc://quit | Out-Null
  report $j.out
}

$mf = "$root\build\mf_encode.exe"
if (-not (Test-Path $mf)) { & cl.exe /nologo /EHsc /std:c++20 /O2 "$PSScriptRoot\tools\mf_encode.cpp" /Fe:$mf /Fo:"$root\build\\" | Out-Null }
foreach ($out in 'tone.flac', 'tone.wma', 'tone-alac.m4a') { & $mf $tone "$dir\$out" | Out-Null; report $out }

foreach ($j in @(
    @('tone.mp3', @('-c:a', 'libmp3lame', '-b:a', '96k')),
    @('tone.wv', @('-c:a', 'wavpack')),
    @('tone.tta', @('-c:a', 'tta')),
    @('tone.ac3', @('-c:a', 'ac3', '-b:a', '96k')),
    @('tone.eac3', @('-c:a', 'eac3', '-b:a', '96k')),
    @('tone.dts', @('-c:a', 'dca', '-strict', '-2', '-ar', '48000', '-b:a', '768k')),
    @('tone-24bit.wav', @('-c:a', 'pcm_s24le')),
    @('tone-adpcm.wav', @('-c:a', 'adpcm_ima_wav')))) {
  & $FFmpeg -hide_banner -loglevel error -y -i $tone @($j[1]) "$dir\$($j[0])"
  report $j[0]
}
