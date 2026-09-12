import csv
import json
import re
import statistics
import sys
from pathlib import Path

prefix, logfile = map(Path, sys.argv[1:3])
with Path(str(prefix) + '.main.csv').open() as f:
    rows = list(csv.DictReader(line for line in f if not line.startswith('#')))
phases = {}
log = logfile.read_text(errors='replace')
for phase, frame in re.findall(r'TERRAINFPS (BASE|EDITED|RESTORED|EDITBEGIN|EDITEND) [\d.]+ (\d+)', log):
    phases.setdefault(phase, []).append(int(frame))

def percentile(values, p):
    values = sorted(values)
    i = (len(values)-1)*p
    lo = int(i)
    return values[lo] + (values[min(lo+1,len(values)-1)]-values[lo])*(i-lo)

result = {}
for phase in ('BASE', 'EDIT', 'EDITED', 'RESTORED'):
    frames = phases.get('EDITBEGIN' if phase == 'EDIT' else phase, [])
    if not frames:
        continue
    lower, upper = min(frames), max(frames)
    if phase == 'EDIT':
        upper += 5
    samples = []
    for previous, row in zip(rows, rows[1:]):
        if lower < int(row['frame']) <= upper:
            samples.append(float(row['t_start_ms'])-float(previous['t_start_ms']))
    result[phase] = dict(frames=len(samples), fps=1000/statistics.mean(samples),
        median_ms=statistics.median(samples), p95_ms=percentile(samples,.95),
        p99_ms=percentile(samples,.99), max_ms=max(samples))
result['edits_ok'] = len(re.findall(r'TERRAINFPS EDITEND [\d.]+ \d+ OK',log))
result['restored_ok'] = 'TERRAINFPS RESTORE OK' in log
result['complete'] = 'TERRAINFPS COMPLETE' in log
print(json.dumps(result, indent=2))
if result['edits_ok'] != 10 or not result['restored_ok'] or not result['complete']:
    raise SystemExit('Incomplete terrain edit/restore run; not valid performance evidence')
