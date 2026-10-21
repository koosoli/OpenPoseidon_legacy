"""Reforger parity comparison sheets: one image per pose, columns side by side, plus a stats table.

  python3 compare.py <out-dir> <label>=<shots-dir> [<label>=<shots-dir> ...]

Each shots dir holds <pose>.png (Open Poseidon, triScreenshotNext) or <pose>.<ext> (the Workbench
capture). Writes <out-dir>/<pose>.png (columns at 960 px wide, labelled) and <out-dir>/stats.txt
(luma p10/p50/p90 and mean RGB per image, whole frame and the lower half -- ground and grass).
Run on the PC (needs Pillow + numpy); nothing here is committed with game imagery.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw


def find(directory, pose):
    for ext in ('png', 'jpg', 'jpeg', 'bmp', 'tga', 'dds'):
        hits = glob.glob(os.path.join(directory, pose + '.' + ext)) + glob.glob(
            os.path.join(directory, '**', pose + '.' + ext), recursive=True)
        if hits:
            return hits[0]
    return None


def stats(img):
    a = np.asarray(img.convert('RGB')).astype(np.float32)
    luma = a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    out = []
    for name, sl in (('all', slice(None)), ('low', slice(a.shape[0] // 2, None))):
        l = luma[sl]
        m = a[sl].reshape(-1, 3).mean(0)
        out.append('%s p10 %5.1f p50 %5.1f p90 %5.1f rgb %5.1f %5.1f %5.1f' % (
            name, np.percentile(l, 10), np.percentile(l, 50), np.percentile(l, 90), m[0], m[1], m[2]))
    return ' | '.join(out)


def main():
    out_dir = sys.argv[1]
    cols = [a.split('=', 1) for a in sys.argv[2:]]
    os.makedirs(out_dir, exist_ok=True)
    poses = []
    for _, d in cols:
        for f in glob.glob(os.path.join(d, '*.*')):
            name = os.path.splitext(os.path.basename(f))[0]
            if os.path.splitext(f)[1].lower() in ('.png', '.jpg', '.jpeg', '.bmp', '.tga') and name not in poses:
                poses.append(name)
    lines = []
    w, h = 960, 540
    for pose in sorted(poses):
        tiles = []
        for label, d in cols:
            path = find(d, pose)
            tile = Image.new('RGB', (w, h), (40, 0, 40))
            if path:
                img = Image.open(path).convert('RGB')
                lines.append('%-14s %-22s %s' % (pose, label, stats(img)))
                tile = img.resize((w, int(img.height * w / img.width))).crop((0, 0, w, h))
            ImageDraw.Draw(tile).text((8, 6), '%s  %s' % (label, pose), fill=(255, 255, 0))
            tiles.append(tile)
        sheet = Image.new('RGB', (w * len(tiles), h))
        for i, t in enumerate(tiles):
            sheet.paste(t, (i * w, 0))
        sheet.save(os.path.join(out_dir, pose + '.png'))
    with open(os.path.join(out_dir, 'stats.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
