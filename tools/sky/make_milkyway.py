#!/usr/bin/env python3
"""Convert a NASA SVS Deep Star Maps equirectangular EXR into the renderer's raw sky blob.

WHY OFFLINE. The renderer has no image decoder and should not grow one: it would decode the
same file identically on every launch, and a 4k EXR is 36 MB of PIZ-compressed float that
takes real time to unpack. Converting once into a blob that `std::fs::read` + a straight
buffer upload can consume is both faster and simpler, and it is the pattern tools/ already
follows for every other derived asset here.

WHY THE BLUR, which is the non-obvious part. The source is a STAR MAP: most of its energy is
in point sources. Our bright stars come from the Yale catalogue instead (starcat.rs), drawn as
real points with real magnitudes and colours, so keeping the texture's stars would draw every
bright star TWICE -- once sharp from the catalogue and once as a blurry blob underneath, in
slightly the wrong place wherever the two disagree. What the texture is actually needed for is
the one thing points cannot make: the continuous diffuse glow of the unresolved Milky Way. So
the point sources are removed and only that glow is kept.

Removing them is a median-style low pass, not a Gaussian: a Gaussian SPREADS a bright star
into a fat halo instead of deleting it, which is exactly the artefact this is trying to avoid.
A large-radius minimum-then-blur keeps the diffuse floor and drops the spikes.

Because what survives has no detail finer than the blur radius, the output resolution can be
small without losing anything -- which is what makes it affordable to compile into the binary.

    python tools/sky/make_milkyway.py <input.exr> -o resources/sky/milkyway.pskytex [-w 1024]

OUTPUT FORMAT (.pskytex), deliberately trivial:
    0   u8[4]   magic "PSKY"
    4   u32 LE  version (1)
    8   u32 LE  width
    12  u32 LE  height
    16  u32 LE  format (1 = RGBA16F, equirectangular, +X at u=0, +Y up, linear light)
    20  u32 LE  reserved (0)
    24  f16[]   width*height*4, row 0 = +90 deg declination

LICENCE. NASA SVS "Deep Star Maps 2020" (svs.gsfc.nasa.gov/4851), derived from Gaia DR2:
PUBLIC DOMAIN. Record it in THIRD_PARTY_NOTICES.md anyway -- NASA asks for credit even where
it does not require it.
"""
import argparse, os, struct, sys
os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import numpy as np
import cv2


def strip_point_sources(img, radius):
    """Keep the diffuse floor, drop the stars. See the module docstring for why not a Gaussian."""
    k = radius * 2 + 1
    # A minimum filter deletes a star outright (it is a local MAXIMUM); it also eats the glow's
    # own peak, so blur afterwards to put a smooth floor back rather than a blocky one.
    floor = cv2.erode(img, np.ones((k, k), np.uint8))
    return cv2.GaussianBlur(floor, (k, k), radius * 0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-w", "--width", type=int, default=1024, help="output width; height is half")
    ap.add_argument("-r", "--radius", type=int, default=6, help="point-source removal radius, source pixels")
    a = ap.parse_args()

    src = cv2.imread(a.input, cv2.IMREAD_UNCHANGED)
    if src is None:
        sys.exit(f"could not read {a.input} (OpenEXR support may be off)")
    if src.ndim != 3 or src.shape[2] < 3:
        sys.exit(f"expected an RGB image, got shape {src.shape}")
    src = src[:, :, :3].astype(np.float32)
    print(f"source {src.shape[1]}x{src.shape[0]}, max {src.max():.4f}, mean {src.mean():.6f}")

    glow = strip_point_sources(src, a.radius)
    print(f"after point-source removal: max {glow.max():.4f}, mean {glow.mean():.6f}")

    w, h = a.width, a.width // 2
    small = cv2.resize(glow, (w, h), interpolation=cv2.INTER_AREA)
    # cv2 is BGR; the renderer wants RGB.
    rgb = small[:, :, ::-1]
    rgba = np.dstack([rgb, np.ones((h, w, 1), np.float32)]).astype(np.float16)

    with open(a.output, "wb") as f:
        f.write(b"PSKY")
        f.write(struct.pack("<IIIII", 1, w, h, 1, 0))
        f.write(rgba.tobytes())
    print(f"wrote {a.output} - {w}x{h} RGBA16F, {os.path.getsize(a.output)} bytes")


if __name__ == "__main__":
    main()
