"""Prepare explicit export replacements; never changes engine source or publishes.

Run after prepare_policy.py only when deliberately starting a new policy. This
extends the maintained policy, preserving curated documentation and licence holds.
The input manifest identifies the exact bytes that need privacy edits. Review the
resulting diff before committing. Final publication approval remains separate.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

from export_source import PATTERNS, classify, digest, scan


EXCLUDE = (
    'scripts/Test-SwimAndLockCache.ps1',
    'scripts/Test-OriginalOFPWeatherPlayerRuntime.ps1',
    'scripts/Test-RainWaterNaturalRuntime.ps1',
    'scripts/build_reference_fixture_index.py',
    'scripts/verify_p3dm_against_index.py',
    'scripts/verify_virtual_path_resolution.py',
    'scripts/generate_capability_matrix.py',
    'scripts/generate_compat_capability_matrix.py',
    'scripts/run_preview0_wgpu_check.ps1',
)


def anonymize_save(data):
    """Keep the binary string length/terminator and every non-path byte intact."""
    matches = list(re.finditer(PATTERNS['personal-build-path'], data))
    if len(matches) != 1:
        raise ValueError('Expected exactly one legacy save fixture source path')
    start = matches[0].start()
    end = data.index(b'\0', start)
    prefix = b'C:/synthetic-fixtures/'
    suffix = b'/missions/'
    replacement = prefix + b'x' * (end - start - len(prefix) - len(suffix)) + suffix
    if len(replacement) != end - start:
        raise ValueError('Fixture path replacement must preserve its byte length')
    return data[:start] + replacement + data[end:]


def prepare(repo, manifest, policy_path):
    source = json.loads(manifest.read_bytes())
    policy = json.loads(policy_path.read_bytes())
    sha = source['source_commit']

    def blob(path):
        return subprocess.check_output(['git', '-C', str(repo), 'show', f'{sha}:{path}'])

    for name in EXCLUDE:
        if name not in policy['exclude_files']:
            policy['exclude_files'].append(name)
    # Development plans are not build inputs, release docs or licence notices.
    if 'engine/WgpuRenderer/docs/' not in policy['exclude_prefixes']:
        policy['exclude_prefixes'].append('engine/WgpuRenderer/docs/')

    # Preserve the material test's authored expectations without its private ledger.
    census = json.loads(blob('docs/roadmap/modernisation/ORIGINAL-OFP-CONTACT-SOURCE-CENSUS-20261001.json'))
    names = {'abel\\pi.paa', 'o\\pt.paa', 'o\\ps.paa'}
    expected = {'scope': 'Surface classification expectations; contains no game assets', 'maps': {}}
    for world in ('abel', 'noe'):
        expected['maps'][world] = {'qualifyingPalette': [
            {k: p[k] for k in ('texture', 'pureSand', 'pureMud', 'classes')}
            for p in census['maps'][world]['qualifyingPalette'] if p['texture'] in names]}
    output = policy_path.parent / 'public-files/sanitized'
    expected_path = output / 'tests/fixtures/original-surfaces.json'
    expected_path.parent.mkdir(parents=True, exist_ok=True)
    # LF, as the repository stores it: a CRLF copy hashes differently from the committed blob.
    expected_path.write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8', newline='\n')
    policy['add_files']['tests/fixtures/original-surfaces.json'] = expected_path.relative_to(policy_path.parent).as_posix()

    changes = []
    for entry in source['entries']:
        name = entry['source_path']
        if not name or not classify(name, policy)[0].startswith('INCLUDE'):
            continue
        privacy = [f for f in entry['findings'] if f['kind'] != 'license-source-chain-hold']
        refresh = policy['rewrites'].get(name, '').startswith('public-files/sanitized/')
        if (not privacy and not refresh) or name == 'CREDITS.md' or name.endswith('.ico'):
            continue
        original = blob(name)
        if name == 'tests/fixtures/savegame/save.fps':
            data = anonymize_save(original)
        else:
            text = original.decode('utf-8')
            if name == 'scripts/default-content/test_materials.py':
                text = text.replace('docs/roadmap/modernisation/ORIGINAL-OFP-CONTACT-SOURCE-CENSUS-20261001.json',
                                    'tests/fixtures/original-surfaces.json')
            if name == 'apps/tools/BlenderAddon/README.md':
                text = text.replace('F:\\ofpisnotdead.com\\sources\\CWA\\OFPR', 'C:\\src\\OpenPoseidon')
            # Agent skills are not part of the release: drop lines that point at them.
            text = re.sub(r'(?m)^.*\.claude[\\/]skills[\\/].*(?:\r?\n|$)', '', text)
            text = re.sub(r'docs[\\/]roadmap(?:[\\/][^\s`\"\'<>),;]+)*', 'design notes', text)
            text = text.replace('CLAUDE.md', 'the development guidelines')
            text = text.replace('<main>/.claude/worktrees/<name>', '<main>/<worktree-directory>/<name>')
            text = text.replace('.claude/worktrees/', 'worktrees/')
            data = text.encode('utf-8')
        if scan(data):
            raise ValueError(f'Unresolved privacy finding in replacement: {name}')
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        policy['rewrites'][name] = path.relative_to(policy_path.parent).as_posix()
        changes.append({'path': name, 'before': digest(original), 'after': digest(data),
                        'purpose': 'Remove private document references or personal example paths'})

    # Use the existing project-owned logo for all app icons; retain RC notices.
    icon = blob('resources/icon-openposeidon.ico')
    for variant in ('demo', 'game', 'launcher', 'server', 'tool'):
        name = f'resources/icon-{variant}.ico'
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(icon)
        policy['rewrites'][name] = path.relative_to(policy_path.parent).as_posix()
        changes.append({'path': name, 'before': digest(blob(name)), 'after': digest(icon),
                        'purpose': 'Replace legacy icon art with project logo'})
    policy_path.write_text(json.dumps(policy, indent=2) + '\n', encoding='utf-8', newline='\n')
    return {'source_commit': sha, 'replacements': changes, 'extra_exclusions': list(EXCLUDE)}


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--policy', type=Path, required=True)
    ap.add_argument('--receipt', type=Path, required=True)
    args = ap.parse_args()
    result = prepare(args.repo, args.manifest, args.policy)
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'replacements': len(result['replacements']), 'source_commit': result['source_commit']}))
