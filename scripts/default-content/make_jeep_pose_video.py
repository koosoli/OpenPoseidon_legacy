"""Timestamp-resample harness screenshots for motion inspection, never FPS data."""

import argparse
import json
from pathlib import Path

import cv2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    samples = json.loads((args.capture / 'motion-samples.json').read_text(encoding='utf-8-sig'))
    if len(samples) < 2 or any(b['time'] <= a['time'] for a, b in zip(samples, samples[1:])):
        raise ValueError('At least two monotonic samples required')
    first = cv2.imread(str(args.capture / samples[0]['file']))
    if first is None:
        raise ValueError('Missing first frame')
    height, width = first.shape[:2]
    out = args.capture / 'jeep-animation-draft.mp4'
    writer = cv2.VideoWriter(str(out), cv2.VideoWriter_fourcc(*'mp4v'), 24, (width, height))
    if not writer.isOpened():
        raise RuntimeError('Video encoder unavailable')
    index, previous, frame = 0, -1, None
    try:
        duration = samples[-1]['time'] - samples[0]['time']
        for n in range(int(duration * 24) + 1):
            time = samples[0]['time'] + n / 24
            while index + 1 < len(samples) and samples[index + 1]['time'] <= time:
                index += 1
            if index != previous:
                sample = samples[index]
                frame = cv2.imread(str(args.capture / sample['file']))
                if frame is None or frame.shape != first.shape:
                    raise ValueError('Missing or mismatched screenshot')
                cv2.putText(frame, 'DRAFT: '+sample['pose']+' | pose review, not a performance capture',
                            (16, height-20), cv2.FONT_HERSHEY_SIMPLEX, .55, (255, 255, 255), 1, cv2.LINE_AA)
                previous = index
            writer.write(frame)
    finally:
        writer.release()
    verify = cv2.VideoCapture(str(out))
    try:
        if not verify.isOpened() or verify.get(cv2.CAP_PROP_FRAME_COUNT) < 2:
            raise RuntimeError('Encoded video cannot be read')
    finally:
        verify.release()
    print(out)


if __name__ == '__main__':
    main()
