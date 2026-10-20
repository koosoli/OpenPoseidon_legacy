"""What the threads were doing during the long frames of an Nsight Systems trace.

    python -I scripts/reforger-pc/nsys_long_frames.py <trace.sqlite> --bin <dir with exe+dll+pdbs>
           [--ms 50] [--skip-s 0] [--top 40]

<trace.sqlite> is `nsys export --type sqlite` of a capture made with CPU sampling on (an elevated
run; see .agents/PROFILING-WITH-NSIGHT.md). A long frame is a gap of more than --ms between two
vkQueuePresentKHR calls (gaps over 1 s -- loading -- are skipped). For the game's main thread
(busiest unnamed thread) and wgr-render it prints the functions on the stack in those windows,
inclusive (share of the thread's samples with the function anywhere on the stack) and as leaf,
then the busiest Vulkan calls of the whole capture.

Symbols. nsys on Windows leaves our frames as raw addresses and the export has no module-load
table, so the load addresses are recovered here: OpenPoseidon.exe's outermost frame is always
__scrt_common_main_seh, and the first wgpu_renderer.dll frame above an exe frame is always an
exported ffi::wgr_* function. Every 64 KB-aligned base in range is tried with llvm-symbolizer
until those anchors resolve, then all frames of both modules are symbolised (needs the exact
binaries + PDBs that were traced, i.e. the harness's bin-nsys copy, and llvm-symbolizer on PATH
or in D:/C:\\Program Files\\LLVM\\bin). Without --bin the addresses are printed raw.

Built for multi-GB exports: one pass over the samples per thread, joined in SQL through a temp
table, never one query per sample.
"""
import argparse
import collections
import json
import os
import shutil
import sqlite3
import subprocess


def find_symbolizer():
    for p in (shutil.which('llvm-symbolizer'), r'D:\Program Files\LLVM\bin\llvm-symbolizer.exe',
              r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'):
        if p and os.path.exists(p):
            return p
    return None


def symbolize(tool, obj, image_base, vas):
    """vas: preferred-base virtual addresses. Returns function names (no inlines)."""
    if not vas:
        return []
    # JSON: exactly one line per address (the text format prints a varying number for '??').
    out = subprocess.run([tool, f'--obj={obj}', '--functions=short', '--no-inlines', '--output-style=JSON'],
                         input='\n'.join(hex(v) for v in vas), capture_output=True, text=True).stdout
    names = []
    for line in out.splitlines():
        if not line.strip():
            continue
        try:
            sym = json.loads(line).get('Symbol') or [{}]
            names.append(sym[0].get('FunctionName') or '??')
        except ValueError:
            names.append('??')
    return names


def image_base_of(path):
    with open(path, 'rb') as f:
        data = f.read(4096)
    pe = int.from_bytes(data[0x3c:0x40], 'little')
    return int.from_bytes(data[pe + 24 + 24:pe + 24 + 32], 'little'), int.from_bytes(data[pe + 24 + 56:pe + 24 + 60], 'little')


def find_base(tool, obj, addr, size, want):
    """The 64 KB-aligned load address at which `addr` lands in a function containing `want`."""
    pref, _ = image_base_of(obj)
    lo = (addr - size) & ~0xffff
    cands = list(range(lo, addr, 0x10000))
    names = symbolize(tool, obj, pref, [pref + addr - b for b in cands])
    for b, n in zip(cands, names):
        if want in n:
            return b
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db')
    ap.add_argument('--bin', help='folder with the traced OpenPoseidon.exe / wgpu_renderer.dll and their PDBs')
    ap.add_argument('--ms', type=float, default=50.0)
    ap.add_argument('--skip-s', type=float, default=0.0, help='ignore long frames before this many seconds')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--process', default='OpenPoseidon.exe')
    a = ap.parse_args()
    c = sqlite3.connect(a.db)
    S = dict(c.execute('select id, value from StringIds'))

    pid = c.execute('select pid from PROCESSES where name like ?', ('%' + a.process + '%',)).fetchone()[0]
    names = dict(c.execute('select t.globalTid, s.value from ThreadNames t join StringIds s on t.nameId = s.id'))
    present = [r[0] for r in c.execute(
        "select v.start from VULKAN_API v join StringIds s on v.nameId = s.id "
        "where s.value = 'vkQueuePresentKHR' order by v.start")]
    t0 = c.execute('select min(start) from VULKAN_API').fetchone()[0]
    gaps = [(x, y) for x, y in zip(present, present[1:]) if a.ms * 1e6 < y - x < 1e9 and (x - t0) / 1e9 >= a.skip_s]
    print(f'presents {len(present)}, long frames (> {a.ms:.0f} ms) {len(gaps)}, '
          f'time in them {sum(y - x for x, y in gaps) / 1e6:.0f} ms')

    busiest = c.execute(
        'select globalTid, count(*) from COMPOSITE_EVENTS where (globalTid >> 24) & 16777215 = ? '
        'group by globalTid order by 2 desc limit 12', (pid,)).fetchall()
    main_tid = next(g for g, _ in busiest if g not in names)
    render_tid = next((g for g, _ in busiest if names.get(g) == 'wgr-render'), None)

    c.execute('create temp table win(a integer, b integer)')
    c.executemany('insert into win values (?, ?)', gaps)
    stacks = {}
    for tid in (main_tid, render_tid):
        if tid is None:
            continue
        c.execute('drop table if exists temp.ids')
        c.execute('create temp table ids(id integer primary key)')
        c.execute('insert or ignore into ids select e.id from COMPOSITE_EVENTS e join win w '
                  'on e.start between w.a and w.b where e.globalTid = ?', (tid,))
        st = collections.defaultdict(list)
        for sid, sym, depth, mod in c.execute(
                'select s.id, s.symbol, s.stackDepth, s.module from SAMPLING_CALLCHAINS s join ids on s.id = ids.id'):
            st[sid].append((depth, S.get(sym, '?'), os.path.basename(S.get(mod, '') or '').lower()))
        for v in st.values():
            v.sort()
        stacks[tid] = st

    # Symbolise our two modules.
    resolved = {}
    tool = find_symbolizer()
    if a.bin and tool:
        mods = {'openposeidon.exe': os.path.join(a.bin, 'OpenPoseidon.exe'),
                'wgpu_renderer.dll': os.path.join(a.bin, 'wgpu_renderer.dll')}
        addrs = collections.defaultdict(set)
        exe_anchor = dll_anchor = None
        for st in stacks.values():
            for frames in st.values():
                for i, (_, n, m) in enumerate(frames):
                    if m in mods and n.startswith('0x'):
                        addrs[m].add(int(n, 16))
                        if m == 'openposeidon.exe' and i + 1 < len(frames) and frames[i + 1][2] == 'kernel32.dll':
                            exe_anchor = int(n, 16)
                        if (m == 'wgpu_renderer.dll' and i + 1 < len(frames) and frames[i + 1][2] == 'openposeidon.exe'
                                and dll_anchor is None):
                            dll_anchor = int(n, 16)
        bases = {}
        for m, anchor, want in (('openposeidon.exe', exe_anchor, '__scrt_common_main'),
                                ('wgpu_renderer.dll', dll_anchor, 'ffi::wgr_')):
            if anchor is None or not os.path.exists(mods[m]):
                continue
            _, size = image_base_of(mods[m])
            bases[m] = find_base(tool, mods[m], anchor, size, want)
            print(f'{m}: load address {hex(bases[m]) if bases[m] else "NOT FOUND"}')
        for m, base in bases.items():
            if not base:
                continue
            pref, _ = image_base_of(mods[m])
            lst = sorted(addrs[m])
            for addr, name in zip(lst, symbolize(tool, mods[m], pref, [pref + x - base for x in lst])):
                resolved[addr] = name
    elif a.bin:
        print('llvm-symbolizer not found: addresses stay raw')

    def label(n, m):
        if n.startswith('0x'):
            r = resolved.get(int(n, 16))
            return r if r else f'{n} [{m}]'
        return n

    for tid, title in ((main_tid, 'MAIN THREAD'), (render_tid, 'wgr-render')):
        if tid not in stacks:
            continue
        st = stacks[tid]
        n = len(st)
        incl, leaf = collections.Counter(), collections.Counter()
        for frames in st.values():
            seen = set()
            for depth, name, mod in frames:
                lab = label(name, mod)
                if depth == 0:
                    leaf[lab] += 1
                if lab not in seen:
                    seen.add(lab)
                    incl[lab] += 1
        print(f'\n==== {title}: {n} samples in long frames')
        for lab, k in incl.most_common(a.top):
            print(f'{k:7d} {100 * k / max(n, 1):5.1f}%  {lab[:170]}')
        print('  -- leaf')
        for lab, k in leaf.most_common(15):
            print(f'{k:7d} {100 * k / max(n, 1):5.1f}%  {lab[:170]}')

    print('\n==== Vulkan calls, whole capture (count, total ms)')
    for name, k, ms in c.execute(
            'select s.value, count(*), sum(v.end - v.start) / 1e6 from VULKAN_API v join StringIds s '
            'on v.nameId = s.id group by s.value order by 3 desc limit 15'):
        print(f'{k:9d} {ms:9.1f} ms  {name}')


if __name__ == '__main__':
    main()
