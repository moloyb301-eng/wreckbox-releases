"""Compare the C++ engine with the original Rust engine on the same audio files.

    python scripts/parity.py FOLDER_OR_FILES... [--limit N] [--rust PATH] [--cpp PATH]

WAV/AIFF should match exactly (both decode bit-identically). Compressed formats can differ slightly because Media
Foundation and Symphonia trim encoder delay / padding differently; those differences are listed for review.
Also prints how long each engine took, which is a handy speed comparison.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AUDIO = {".mp3", ".wav", ".aif", ".aiff", ".flac", ".m4a", ".alac", ".aac", ".ogg", ".opus"}
EXACT = {".wav", ".aif", ".aiff"}


def collect(args):
    files = []
    for a in args:
        p = Path(a)
        if p.is_dir():
            files += sorted(f for f in p.rglob("*") if f.suffix.lower() in AUDIO and not f.name.startswith("."))
        elif p.suffix.lower() in AUDIO:
            files.append(p)
    return files


def run(exe, files):
    """One analyze call per batch (command lines are limited to ~32k chars). Returns results in input order."""
    out, start = [], time.perf_counter()
    for i in range(0, len(files), 20):
        batch = [str(f) for f in files[i : i + 20]]
        r = subprocess.run([str(exe), "analyze", *batch], capture_output=True, encoding="utf-8", errors="replace")
        lines = [json.loads(l) for l in r.stdout.splitlines() if l.strip()]
        if len(lines) != len(batch):
            sys.exit(f"{exe} returned {len(lines)} results for {len(batch)} files:\n{r.stderr}")
        out += lines
    return out, time.perf_counter() - start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--rust", default=ROOT / "reference/wreckbox/core/target/release/wbcore.exe")
    ap.add_argument("--cpp", default=ROOT / "build/Release/wbcore.exe")
    a = ap.parse_args()

    for exe in (a.rust, a.cpp):
        if not Path(exe).exists():
            sys.exit(f"missing {exe} (see README: Checking the engine against the original)")
    files = collect(a.paths)[: a.limit]
    if not files:
        sys.exit("no audio files found")

    rust, t_rust = run(a.rust, files)
    cpp, t_cpp = run(a.cpp, files)

    stats = {"exact": [0, 0], "compressed": [0, 0]}  # [same, total]
    rows = []
    for f, r, c in zip(files, rust, cpp):
        group = "exact" if f.suffix.lower() in EXACT else "compressed"
        if "error" in r or "error" in c:
            rows.append((f.name, f"rust: {r.get('error', 'ok')} | c++: {c.get('error', 'ok')}"))
            continue
        r, c = r["result"], c["result"]
        stats[group][1] += 1
        diffs = []
        if (r["bpm"] is None) != (c["bpm"] is None) or (r["bpm"] and abs(r["bpm"] - c["bpm"]) > 0.1):
            diffs.append(f"bpm {r['bpm']} vs {c['bpm']}")
        if r["camelot"] != c["camelot"]:
            diffs.append(f"key {r['camelot']} vs {c['camelot']}")
        if r["energy"] is not None and c["energy"] is not None and abs(r["energy"] - c["energy"]) > 0.011:
            diffs.append(f"energy {r['energy']} vs {c['energy']}")
        if abs(r["durationSec"] - c["durationSec"]) > 0.15:
            diffs.append(f"duration {r['durationSec']} vs {c['durationSec']}")
        if diffs:
            rows.append((f.name, "; ".join(diffs)))
        else:
            stats[group][0] += 1

    for name, why in rows:
        print(f"  {name}\n      {why}")
    print()
    for group, (same, total) in stats.items():
        if total:
            print(f"{group:>10}: {same}/{total} identical (BPM within 0.1, same key, energy within 0.01)")
    print(f"      time: rust {t_rust:.1f} s, c++ {t_cpp:.1f} s for {len(files)} files")
    exact_same, exact_total = stats["exact"]
    sys.exit(1 if exact_same != exact_total else 0)


if __name__ == "__main__":
    main()
