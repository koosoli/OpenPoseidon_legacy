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


def srgb_to_linear(c):
    c = np.asarray(c, dtype=float)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def tidewater_sand(rng, size, metres):
    """Tidewater's beach sand (dgreenheck/tidewater @ 4811ba48, MIT; src/world/Terrain.js, the
    `beach sand` block): pale coral sand with warmer, coarser drifts, grain, trodden patches and
    sparse shell grit. Its detail-texture channels are periodic noise at the same physical scales
    (macro 47 m, mid 6.7 m, near 1.9 m); the height-dependent parts (wet band, wrack line, wind
    ripples on the dry upper beach) stay in the renderer. Returns linear RGB."""
    def field(radius, std=0.17):
        return np.clip(0.5 + std * periodic_noise(rng, size, metres, radius), 0.0, 1.0)

    macro_a, macro_b = field(9.0), field(5.0)
    mcr = macro_a * 0.6 + macro_b * 0.4
    dm_w, dm_x, dm_y = field(1.1), field(0.9), field(0.35)
    dn_w, dn_y = field(0.3), field(0.12)
    # sand channel: fine grain (sub-texel here: what survives the texel is a speckle) and pebbles
    grain = np.minimum(0.45 + 0.2 * np.tanh(periodic_noise(rng, size, metres, metres[0] / size * 0.6)), 0.62)
    grain_f = np.minimum(0.45 + 0.2 * np.tanh(periodic_noise(rng, size, metres, metres[0] / size * 0.4)), 0.62)
    pebble = smoothstep(2.2, 3.2, periodic_noise(rng, size, metres, metres[0] / size * 0.8))
    dry_k = 0.6  # the tile cannot know its height above the sea: between the berm and the dunes
    srgb = lambda r, g, b: srgb_to_linear([r, g, b])[None, None, :]
    t = smoothstep(0.3, 0.72, mcr + dry_k * 0.2)[:, :, None]
    sand = srgb(0.83, 0.75, 0.6) * (1 - t) + srgb(0.9, 0.84, 0.72) * t
    d = (smoothstep(0.55, 0.8, dm_w + (macro_b - 0.5) * 0.6) * 0.45)[:, :, None]
    sand = sand * (1 - d) + srgb(0.84, 0.72, 0.55) * d
    sand = sand * (((dm_w - 0.5) * 0.16 + 1.0) * ((dn_w - 0.5) * 0.1 + 1.0))[:, :, None]
    sand = sand * (((grain - 0.45) * 0.3 + 0.97) * ((grain_f - 0.45) * 0.2 + 1.0))[:, :, None]
    trod = smoothstep(0.52, 0.7, dn_y + (dm_y - 0.5) * 0.6) * dry_k
    sand = sand * (1.0 - trod * 0.07)[:, :, None]
    peb_density = smoothstep(0.62, 0.85, macro_b + dm_w * 0.3) * 0.2 + 0.05
    peb_w = (pebble * np.clip(peb_density * 4.0, 0.0, 1.0) * 0.75)[:, :, None]
    shell = smoothstep(0.45, 0.75, dm_x)[:, :, None]
    dark = smoothstep(0.74, 0.82, dm_y)[:, :, None]
    peb_col = (srgb(0.86, 0.82, 0.74) * (1 - shell) + srgb(0.78, 0.64, 0.6) * shell) * (1 - dark) + srgb(0.36, 0.33, 0.3) * dark
    return sand * (1 - peb_w) + peb_col * peb_w


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
    if spec.get("tidewater", False):
        # its own rng: the relief above keeps exactly the fields it had
        rgb = linear_to_srgb(tidewater_sand(np.random.default_rng(spec["seed"] + 1), size, metres)) * 255.0
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
