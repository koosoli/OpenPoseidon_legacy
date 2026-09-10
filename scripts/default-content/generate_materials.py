"""Own periodic material sources. Build-time only; no original image inputs.

Albedo has material variation, not baked directional light. Height supplies
normals independently of road wear/dust colour. NOHQ R=0 deliberately disables
the renderer's height-in-red parallax convention for this content package.
"""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image


def periodic_noise(rng, size, metres, radius):
    """Periodically filtered noise, in physical rather than arbitrary UV units."""
    field = rng.standard_normal((size, size))
    fy = np.fft.fftfreq(size, d=metres[1] / size)[:, None]
    fx = np.fft.rfftfreq(size, d=metres[0] / size)[None, :]
    kernel = np.exp(-2.0 * np.pi**2 * radius**2 * (fx * fx + fy * fy))
    smooth = np.fft.irfft2(np.fft.rfft2(field) * kernel, s=field.shape)
    return smooth / max(float(smooth.std()), 1e-12)


def encode_nohq(height, metres):
    size_y, size_x = height.shape
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) / (2.0 * metres[0] / size_x)
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) / (2.0 * metres[1] / size_y)
    inv_length = 1.0 / np.sqrt(dx * dx + dy * dy + 1.0)
    normal = np.zeros((*height.shape, 4), dtype=np.uint8)
    normal[:, :, 1] = np.rint((0.5 - 0.5 * dy * inv_length) * 255).astype(np.uint8)
    normal[:, :, 2] = np.rint((0.5 + 0.5 * inv_length) * 255).astype(np.uint8)
    normal[:, :, 3] = np.rint((0.5 - 0.5 * dx * inv_length) * 255).astype(np.uint8)
    return normal


def generate(spec, size):
    if size < 32 or size > 2048 or size & (size - 1):
        raise ValueError("size must be a power of two from 32 to 2048")
    metres = spec["tileMetres"]
    if len(metres) != 2 or min(metres) <= 0 or spec["aggregateMetres"] <= 0:
        raise ValueError("physical material dimensions must be positive")
    rng = np.random.default_rng(spec["seed"])
    aggregate = periodic_noise(rng, size, metres, spec["aggregateMetres"] * 0.35)
    fine = periodic_noise(rng, size, metres, spec["aggregateMetres"] * 0.12)
    broad = periodic_noise(rng, size, metres, 0.35)
    mineral = 0.5 + 0.5 * np.tanh(aggregate)
    height = spec["heightMetres"] * (0.8 * mineral + 0.2 * np.tanh(fine))
    ripple = spec.get("rippleMetres", 0.0)
    if ripple > 0.0:
        # Integral cycles and periodic distortion preserve both tile boundaries.
        # Relief is independent of albedo: no painted-in highlight/shadow pair.
        cycles = max(1, round(metres[1] / ripple))
        v = np.arange(size)[:, None] / size
        warp = periodic_noise(rng, size, metres, ripple * 3.0)
        envelope = 0.35 + 0.65 * (0.5 + 0.5 * np.tanh(broad))
        height += spec.get("rippleHeightMetres", 0.006) * envelope * (
            0.5 + 0.5 * np.cos(2.0 * np.pi * cycles * v + 0.7 * warp)) ** 2
    variation = 14.0 * (mineral - 0.5) + 3.0 * np.tanh(fine) + 5.0 * np.tanh(broad)
    albedo = np.empty((size, size, 4), dtype=np.uint8)
    rgb = np.asarray(spec["baseSrgb"], dtype=float)[None, None, :] + variation[:, :, None]
    alpha = np.ones((size, size))
    if spec.get("road", False):
        u = (np.arange(size) + 0.5) / size
        edge_distance = np.minimum(u, 1.0 - u)
        # Authored shoulder fade and tyre wear, not copied from original pixels.
        alpha = np.broadcast_to(np.clip(edge_distance / 0.035, 0, 1), height.shape)
        dust = np.exp(-((edge_distance / 0.11) ** 2))
        tracks = spec.get("wheelTracks", [0.22, 0.40, 0.60, 0.78])
        wear = sum(np.exp(-(((u - centre) / 0.055) ** 2)) for centre in tracks)
        if spec.get("trackOnly", False):
            # Open centre and shoulders reveal the actual underlying landscape,
            # not a painted grass strip. Small periodic edge variation avoids a stencil.
            shifted_u = u[None, :] + 0.008 * np.tanh(broad)
            coverage = np.maximum.reduce([
                np.exp(-((shifted_u - centre) / 0.105) ** 4) for centre in tracks])
            alpha = alpha * coverage
        rgb = rgb + dust[None, :, None] * np.array([22, 18, 10])[None, None, :]
        rgb = rgb - wear[None, :, None] * 7.0
    albedo[:, :, :3] = np.rint(np.clip(rgb, 0, 255)).astype(np.uint8)
    albedo[:, :, 3] = np.rint(alpha * 255).astype(np.uint8)
    roughness = np.rint(np.clip(spec["roughness"] + 0.025 * np.tanh(fine), 0, 1) * 255).astype(np.uint8)
    return {"co": albedo, "nohq": encode_nohq(height, metres), "roughness": roughness, "height": height}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--size", type=int, default=1024)
    args = parser.parse_args()
    source = args.spec.read_bytes()
    config = json.loads(source)
    if config.get("sourceVersion") != 1:
        raise ValueError("unsupported material source version")
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {"sourceSha256": hashlib.sha256(source).hexdigest(), "size": args.size, "files": {}}
    for name, spec in config["materials"].items():
        if not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in name):
            raise ValueError("unsafe material name")
        maps = generate(spec, args.size)
        for kind in ("co", "nohq", "roughness"):
            path = args.out / f"{name}_{kind}.png"
            Image.fromarray(maps[kind]).save(path)
            manifest["files"][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        # Editable linear heights, not a redistributed source-game asset.
        np.save(args.out / f"{name}_height.npy", maps["height"].astype(np.float32), allow_pickle=False)
    (args.out / "generated.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
