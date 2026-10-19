"""Tidewater's sea sounds for the @OP_TidewaterWildlife pack (owner, 2026-10-19: "port the ocean and wave sounds").

Run once against a Tidewater checkout (dgreenheck/tidewater); the output is committed under
content/default-packs/tidewater-wildlife/source/sound, so the engine build needs no Tidewater:

    python scripts/default-content/export_tidewater_sounds.py <tidewater checkout> content/default-packs/tidewater-wildlife/source/sound

Provenance: Tidewater's public/audio are field recordings from Freesound, every one released under CC0 1.0 (its
public/audio/CREDITS.md, copied alongside, names each recordist and source). No game's content is used.

What it writes:
* the loops, re-encoded as Vorbis (Tidewater crossfades them at the wrap; it ships them as Opus): the distant surf, water lapping and rushing past a
  hull, the humpback's song;
* the one-shot sprites cut into one file per slice (OFP plays a whole file; Tidewater plays a slice of a sheet): gull
  and tern calls, the
  waves breaking, the bores washing up, the backwash, hull slaps, the whale's blow, its splash and its surfacing;
* sounds.json -- per sound its files and each file's measured loudness (Tidewater's soundBank.js), for the mixer
  levels generate_tidewater_whale.py writes into the config.
"""

import json
import re
import shutil
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

LOOPS = ["surf_far", "boat_engine", "boat_lap", "boat_rush", "whale_song"]
SPRITES = ["surf_crash", "surf_wash", "surf_backwash", "hull_slap", "whale_blow", "big_splash", "emerge", "gull", "tern"]


def bank(root):
    """Tidewater's soundBank.js entries (file, loop, slices, lufs) for the sounds used here."""
    text = (root / "src" / "audio" / "soundBank.js").read_text()
    out = {}
    for name in LOOPS + SPRITES:
        m = re.search(rf"\b{name}: \{{(.*?)\}},\n", text)
        if not m:
            raise SystemExit(f"{name} is not in soundBank.js")
        body = m.group(1)
        entry = {"file": re.search(r"file: '([^']+)'", body).group(1)}
        if "loop: true" in body:
            entry["lufs"] = float(re.search(r"lufs: (-?[\d.]+)", body).group(1))
        else:
            entry["slices"] = json.loads(re.search(r"slices: (\[.*?\]) \]", body).group(1) + "]")
            entry["lufs"] = json.loads(re.search(r"lufs: (\[[^\]]*\])", body).group(1))
        out[name] = entry
    return out


def write_vorbis(path, data, rate):
    """Vorbis in Ogg, written a second at a time: libsndfile's Vorbis encoder overflows the stack on a long single
    write (a 52 s loop crashed Python silently)."""
    with sf.SoundFile(path, "w", rate, data.shape[1], format="OGG", subtype="VORBIS") as f:
        for i in range(0, len(data), rate):
            f.write(data[i:i + rate])


def main():
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    audio = root / "public" / "audio"
    manifest = {}
    for name, e in bank(root).items():
        src = audio / e["file"]
        if "slices" not in e:
            # re-encoded: Tidewater's loops are Opus in Ogg, and the engine's .ogg decoder is Vorbis only (the
            # copied files did not load: "unsupported format")
            data, rate = sf.read(src, always_2d=True)
            write_vorbis(out / e["file"], data, rate)
            manifest[name] = {"loop": True, "files": [e["file"]], "lufs": [e["lufs"]]}
            continue
        data, rate = sf.read(src, always_2d=True)
        files = []
        for i, (start, dur) in enumerate(e["slices"]):
            a, b = int(start * rate), min(int((start + dur) * rate), len(data))
            cut = data[a:b].copy()
            # 10 ms fades: a slice of a sheet starts and ends mid-waveform
            f = min(int(0.01 * rate), len(cut) // 4)
            ramp = np.linspace(0.0, 1.0, f)[:, None]
            cut[:f] *= ramp
            cut[-f:] *= ramp[::-1]
            name_i = f"{name}_{i + 1}.ogg"
            write_vorbis(out / name_i, cut, rate)
            files.append(name_i)
        manifest[name] = {"loop": False, "files": files, "lufs": e["lufs"]}
    shutil.copyfile(audio / "CREDITS.md", out / "CREDITS.md")
    (out / "sounds.json").write_text(json.dumps(manifest, indent=1))
    print(f"{sum(len(m['files']) for m in manifest.values())} files for {len(manifest)} sounds -> {out}")


if __name__ == "__main__":
    main()
