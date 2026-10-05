"""Prepare private release review inputs; does not approve, publish or remove code."""
import argparse
import json
import re
import subprocess
from pathlib import Path


def prepare(repo, commit, output):
    def original(name):
        return subprocess.check_output(['git', '-C', str(repo), 'show', f'{commit}:{name}']).decode('utf-8').replace('\r\n', '\n')

    public = output / 'public-files'
    public.mkdir(parents=True, exist_ok=True)
    credits = original('CREDITS.md')
    credits = credits.replace('*Arma: Cold War Assault Remastered* (engine source, codename **Poseidon** / CWR)',
                              '*OpenPoseidon* (community engine continuation, codename **Poseidon**)')
    # Replace private document references only; retain attribution, tables and licenses.
    credits = re.sub(r'`docs/roadmap/decisions/[^`]+`', 'the development decision records', credits)
    (public / 'CREDITS.md').write_text(credits, encoding='utf-8')
    notices = original('THIRD_PARTY_NOTICES.md')
    notices = notices.replace('This file reproduces all third-party components used by the project.',
                              'This file records third-party notices. Binary packaging also requires the licenses of the exact resolved dependency versions.')
    notices = re.sub(r'3\. \*\*Rust dependencies \(Cargo\)\*\*.*?(?=\n\nThe `thirdparty/`)',
                     '''3. **Rust dependencies (Cargo)** include the WGPU renderer, the master
   server and the Trident test orchestrator. Their manifests and the workspace
   lockfile select their dependencies. Cargo fetches their source at build time.
   The WGPU renderer is a Rust library linked with the C++ engine; other Rust
   tools also build standalone programs. The older dependency summary in section
   3 needs an exact resolved-version inventory before any binary distribution.''',
                     notices, flags=re.S)
    notices += '''

## Additional renderer and physics components

The vendored meshoptimizer implementation is pinned in
`thirdparty/meshoptimizer-clod/PIN.json`. Its full license remains in
`thirdparty/meshoptimizer-clod/LICENSE.md` and must accompany distributions.
The vcpkg manifest also resolves Box3D, glslang, sse2neon and zstd. Preserve the
copyright files installed by their pinned ports, including modified OpenAL Soft
source notices. This source notice file does not replace a binary dependency
license inventory. Renderer Cargo dependencies are pinned by the workspace
`Cargo.lock`; include their applicable licenses in binary distributions.

### AMD FidelityFX Super Resolution 1

The WGSL implementation in `engine/WgpuRenderer/rust/src/fsr1.wgsl` is ported
from AMD FidelityFX `ffx_fsr1.h` and `ffx_a.h`:
https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
'''
    (public / 'THIRD_PARTY_NOTICES.md').write_text(notices, encoding='utf-8')
    (public / '.gitignore').write_text('''/build/
/dist/
/assets/generated/
/packages/
**/vcpkg_installed/
CMakeUserPresets.json
target/
**/target/
__pycache__/
.venv/
*.log
*.rpt
*.dmp
*.pdb
*.dll
*.so
*.dylib
imgui.ini
''', encoding='utf-8')
    policy = {
        'purpose': 'PRIVATE_DRAFT_NOT_APPROVED; path rules are candidates, not license clearance',
        'rewrites': {name: f'public-files/{name}' for name in
                     ('README.md', 'CONTRIBUTING.md', 'CREDITS.md', 'THIRD_PARTY_NOTICES.md', '.gitignore')},
        'add_files': {name: f'public-files/{name}' for name in ('BUILDING.md', 'COMPATIBILITY.md')},
        'include_files': ['LICENSE', 'CMakeLists.txt', 'CMakePresets.json', 'Cargo.toml', 'Cargo.lock',
                          'vcpkg.json', 'vcpkg-configuration.json', '.clang-format', '.clang-tidy',
                          '.gitattributes', 'lsan-suppressions.txt', 'tsan-suppressions.txt',
                          'ubsan-suppressions.txt', 'valgrind-suppressions.supp'],
        'exclude_prefixes': ['docs/', '.agents/', '.tmp-movecam/', '.tmp-shadow/', 'screenshots/',
                             'experiments/', '.github/', 'scripts/publishing/', 'scripts/githooks/'],
        'exclude_files': ['CLAUDE.md', 'HANDOVER-PARITY-NEXT-AI.md', 'HANDOVER-PROMPT.md',
                          'PROVENANCE.md', '.reference-corpora.json.example', '.trident.env.example',
                          'scripts/Use-RepoHooks.ps1', 'scripts/package_preview0.ps1',
                          'scripts/merge_ledger_seed.py', 'scripts/write_preview0_manifest.py',
                          'scripts/validate_execution_ledger.py', 'scripts/validate_preview0_ledger.py',
                          'scripts/validate_renderer_ledger.py', 'scripts/validate_compat_c1_ledger.py'],
        'include_prefixes': ['engine/', 'apps/', 'cmake/', 'thirdparty/', 'mserver/', 'tools/',
                             'tests/', 'content/', 'assets/', 'resources/', 'scripts/', 'docker/', 'dev-missions/'],
        'license_holds': ['CREDITS.md', 'engine/Poseidon/AI/AIHunt.hpp', 'engine/Poseidon/AI/AIHunt.cpp'],
    }
    (output / 'source-policy.json').write_text(json.dumps(policy, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--commit', required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    prepare(a.repo, a.commit, a.output)
