"""Records what sidecar/slsk_sync.py's matching and ranking do on a set of cases, so tests/slsk_tests.cpp can check the C++
port gives the same answers.   <bundled python> tests/fixtures/make_slsk_golden.py > tests/fixtures/slsk_golden.json
(needs the bundled interpreter, which has aioslsk: build\\Release\\soulseek\\python\\python.exe)."""
import json
import os
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

root = Path(tempfile.mkdtemp())
os.environ["WRECKBOX_ROOT"] = str(root)
(root / "_soulseek").mkdir()
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "sidecar"))
import slsk_sync as s  # noqa: E402

out = {}

TEXTS = [
    "Skrillex & BEAM", "Fred again.. - Danielle (smile on my face)", "RÜFÜS DU SOL", "Beyoncé – Halo", "Sigur Rós - Hoppípolla", "  spaced   out  ",
    "ÆON FLUX ß", "ﬁne ﬂow", "ＦＵＬＬ ＷＩＤＴＨ", "Mötley Crüe", "AC/DC", "a&b", "100% Pure", "", "!!!", "Ñandú café", "naïve", "Tyler, The Creator", "日本語 title",
    "Don't Stop", "x_y-z", "Zoë & the Boys feat. Ünïcode",
]
out["norm"] = [[t, s.norm(t)] for t in TEXTS]

TITLES = [
    "Selecta (feat. BEAM)", "Song ft. Someone", "Track (Original Mix)", "Track - Original Mix", "Hit - Remastered 2011", "Tune (Official Video)",
    "Tune [Free Download]", "Tune (Extended Mix)", "Title [feat. A & B] - Radio Edit", "Plain", "(With You)", "Love Me - Mono Version", "Song (Remastered 2009)",
    "Song [remaster]", "A feat. B", "A FT. B (Live)", "Day Tripper - Stereo", "  padded  ", "Name (feat. X) (Original Mix)",
]
out["clean"] = [[t, s.clean_title(t)] for t in TITLES]

TRACKS = [
    {"id": "t1", "artists": ["Skrillex", "BEAM"], "title": "Selecta", "durationMs": 190000},
    {"id": "t2", "artists": ["Fred again.."], "title": "Danielle (smile on my face)", "durationMs": 240000},
    {"id": "t3", "artists": ["RÜFÜS DU SOL"], "title": "Innerbloom - Original Mix", "durationMs": 577000},
    {"id": "t4", "artists": [], "title": "No Artist Song", "durationMs": None},
    {"id": "t5", "artists": ["Daft Punk"], "title": "One More Time (feat. Romanthony) - Remastered", "durationMs": 320000},
    {"id": "t6", "artists": ["A"], "title": "Remix Of Nothing", "durationMs": 100000},
    {"id": "t7", "artists": ["Beyoncé"], "title": "Halo [Live]", "durationMs": 261000},
]
queries = []
for t in TRACKS:
    for custom in ("", "  Custom Words & More  "):
        overrides = {t["id"]: {"query": custom}} if custom else {}
        (root / "_soulseek" / "overrides.json").write_text(json.dumps(overrides))
        queries.append({"track": t, "custom": custom, "queries": s.search_queries(t)})
out["queries"] = queries

out["quality"] = [
    [ext, br, mn, s.quality_of(ext, br, mn)]
    for ext in ["flac", "wav", "aiff", "aif", "alac", "m4a", "mp3", "aac", "ogg", "opus", "wma", "ape"]
    for br in [None, 128, 192, 256, 320, 500, 501, 900, 1411]
    for mn in [256]
]

PATHS = [
    ("Skrillex\\Selecta (feat. BEAM)\\01 Skrillex - Selecta.mp3", 190), ("music\\Selecta.flac", 210), ("a\\b\\c\\Selecta (Skrillex Remix).mp3", 190),
    ("Skrillex - Selecta (Live).mp3", None), ("Various\\Hits\\Selecta.wav", 189), ("x\\Skrillex - Selecta - karaoke version.mp3", 190),
    ("Fred again..\\Actual Life\\Danielle (smile on my face).flac", 241), ("Fred_again\\danielle smile on my face.mp3", 250), ("RUFUS DU SOL\\Innerbloom.m4a", 578),
    ("Rüfüs du Sol - Innerbloom (Extended Mix).mp3", 700), ("Other Artist\\Selecta.mp3", 190), ("Selecta", None), ("no\\title\\here.mp3", 1),
]
out["matches"] = [
    {"track": t, "path": p, "duration": d, "tol": tol, "result": s.file_matches(t, p, d, tol)}
    for t in TRACKS for p, d in PATHS for tol in (5,)
]


def result(username, files, free, speed, queue):
    return {"username": username, "free": free, "speed": speed, "queue": queue,
            "files": [{"filename": f[0], "size": f[1], "ext": f[2], "br": f[3], "dur": f[4]} for f in files]}


RESULTS = [
    result("alice", [("Skrillex\\Selecta.flac", 30_000_000, "flac", None, 190), ("Skrillex\\Selecta.mp3", 8_000_000, "mp3", 320, 190)], True, 2_000_000, 0),
    result("bob", [("m\\Skrillex - Selecta.mp3", 7_000_000, "mp3", 320, 191), ("m\\Skrillex - Selecta.mp3", 7_000_000, "mp3", 192, 191)], False, 500_000, 8),
    result("carol", [("x\\Selecta (Skrillex Remix).flac", 25_000_000, "flac", None, 190)], True, 100, 0),
    result("dave", [("Skrillex\\Selecta.wav", 60_000_000, "wav", None, 191), ("Skrillex\\Selecta.m4a", 9_000_000, "m4a", 1000, 191)], True, 3_000_000, 2),
    result("erin", [("S\\Skrillex Selecta.mp3", 400_000, "mp3", 320, 190)], True, 1, 0),
    result("frank", [("S\\Skrillex Selecta.ogg", 5_000_000, "ogg", 500, 190), ("S\\Skrillex Selecta.aac", 5_000_000, "", 256, 190)], False, 9, 80),
    result("gina", [("S\\Skrillex Selecta.mp3", 5_000_000, "mp3", None, 190), ("S\\Skrillex Selecta.mp3", 5_000_000, "mp3", 250, 190)], True, 9, 0),
    result("hank", [("Skrillex\\Selecta.FLAC", 28_000_000, ".FLAC", None, 400)], True, 9, 0),
    result("ivy", [("Skrillex\\Selecta.mp3", 8_000_000, "mp3", 320, 190)], True, 5_000_000, 0),
    result("jo", [("Skrillex\\Selecta.mp3", 8_000_000, "mp3", 320, 190), ("Skrillex\\Selecta.txt", 9_000_000, "txt", None, None)], True, 5_000_000, 0),
]
cfg = {"sync": {"min_lossy_kbps": 256, "duration_tolerance_seconds": 5}}


def fake(results):
    return [SimpleNamespace(username=r["username"], has_free_slots=r["free"], avg_speed=r["speed"], queue_size=r["queue"],
                            shared_items=[SimpleNamespace(filename=f["filename"], filesize=f["size"], extension=f["ext"],
                                                          attributes=[SimpleNamespace(key=k, value=v) for k, v in ((0, f["br"]), (1, f["dur"])) if v is not None])
                                          for f in r["files"]]) for r in results]


ranks = []
for t in TRACKS[:3] + TRACKS[4:5]:
    for loose in (None, "skrillex selecta"):
        cands = s.rank(t, fake(RESULTS), cfg, loose=loose)
        ranks.append({"track": t, "loose": loose, "results": RESULTS,
                      "expected": [{"username": c.username, "path": c.path, "ext": c.ext, "size": c.size, "quality": c.quality, "label": c.label} for c in cands]})
out["rank"] = ranks
json.dump(out, sys.stdout, indent=1, ensure_ascii=False, sort_keys=True)
