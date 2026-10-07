"""Check a separate release Git tree against its private export manifest."""
import argparse
import json
from pathlib import Path

from export_source import blobs, digest, run


def verify(repo, manifest, revision='HEAD'):
    source = json.loads(manifest.read_bytes())
    expected = {e['destination_path']: e for e in source['entries']
                if e['classification'].startswith('INCLUDE')}
    actual = []
    for record in run(repo, 'ls-tree', '-rz', '--long', revision).split(b'\0'):
        if not record:
            continue
        meta, name = record.split(b'\t', 1)
        mode, kind, blob, size = meta.decode().split()
        if kind != 'blob':
            raise ValueError('Non-blob in candidate')
        actual.append(dict(source_path=name.decode(), source_blob=blob, git_mode=mode))
    if {e['source_path'] for e in actual} != set(expected):
        raise ValueError('Release tree file set differs from reviewed export')
    for entry, data in blobs(repo, actual):
        reference = expected[entry['source_path']]
        if digest(data) != reference['export_sha256'] or entry['git_mode'] != reference['git_mode']:
            raise ValueError('Release tree bytes or mode differ: ' + entry['source_path'])
    return {'verified_files': len(actual), 'tree': run(repo, 'rev-parse', revision + '^{tree}').decode().strip()}


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--revision', default='HEAD')
    args = ap.parse_args()
    print(json.dumps(verify(args.repo, args.manifest, args.revision)))
