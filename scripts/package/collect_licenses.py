"""Collect the licence texts of every dependency compiled into a binary package.

vcpkg ports: each installed port's share/<port>/copyright, as vcpkg resolved it for this build.
Rust crates: the licence files of every crate the renderer links (normal dependencies only,
resolved by `cargo metadata` for the Windows target), read from Cargo's source checkout.
Writes legal/vcpkg/<port>.txt and legal/rust/<crate>-<version>/<file>, plus an index.
"""
import argparse
import json
import shutil
import subprocess
from pathlib import Path

LICENCE_PREFIXES = ('LICENSE', 'LICENCE', 'COPYING', 'NOTICE', 'COPYRIGHT', 'UNLICENSE')


def vcpkg(build_dir, out):
    share = next(build_dir.glob('vcpkg_installed/*/share'), None)
    ports = []
    if share:
        for copyright in sorted(share.glob('*/copyright')):
            dest = out / 'vcpkg' / f'{copyright.parent.name}.txt'
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(copyright, dest)
            ports.append(copyright.parent.name)
    return ports


def rust(repo, manifest, target, out):
    meta = json.loads(subprocess.check_output(
        ['cargo', 'metadata', '--format-version', '1', '--locked', '--manifest-path', str(repo / manifest),
         '--filter-platform', target], cwd=repo))
    packages = {p['id']: p for p in meta['packages']}
    nodes = {n['id']: n for n in meta['resolve']['nodes']}
    root = meta['resolve']['root'] or next(i for i, p in packages.items() if Path(p['manifest_path']).parent == (repo / manifest).parent)
    seen, stack = set(), [root]
    while stack:
        node = nodes[stack.pop()]
        for dep in node['deps']:
            if any(k['kind'] in (None, 'build') for k in dep['dep_kinds']) and dep['pkg'] not in seen:
                seen.add(dep['pkg'])
                stack.append(dep['pkg'])
    crates = []
    for pid in sorted(seen, key=lambda i: (packages[i]['name'], packages[i]['version'])):
        pkg = packages[pid]
        if pkg['source'] is None:
            continue  # this repository's own crates: covered by its LICENSE
        src = Path(pkg['manifest_path']).parent
        files = [f for f in src.iterdir() if f.is_file() and f.name.upper().startswith(LICENCE_PREFIXES)]
        dest = out / 'rust' / f"{pkg['name']}-{pkg['version']}"
        dest.mkdir(parents=True, exist_ok=True)
        for f in files:
            shutil.copyfile(f, dest / f.name)
        crates.append({'name': pkg['name'], 'version': pkg['version'], 'license': pkg.get('license'),
                       'files': [f.name for f in files]})
    return crates


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--build-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--manifest', default='engine/WgpuRenderer/rust/Cargo.toml')
    ap.add_argument('--target', default='x86_64-pc-windows-msvc')
    args = ap.parse_args()
    ports = vcpkg(args.build_dir, args.out)
    crates = rust(args.repo, args.manifest, args.target, args.out)
    missing = [c for c in crates if not c['files']]
    lines = ['Licences of the dependencies compiled into this package.', '',
             f'vcpkg ports ({len(ports)}): see vcpkg/<port>.txt', *(f'  {p}' for p in ports), '',
             f'Rust crates ({len(crates)}): see rust/<crate>-<version>/']
    lines += [f"  {c['name']} {c['version']}  {c['license'] or '?'}" +
              ('' if c['files'] else '  (no licence file in the crate; licence per its manifest)') for c in crates]
    (args.out / 'DEPENDENCIES.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(json.dumps({'vcpkg_ports': len(ports), 'rust_crates': len(crates), 'crates_without_file': len(missing)}))


if __name__ == '__main__':
    main()
