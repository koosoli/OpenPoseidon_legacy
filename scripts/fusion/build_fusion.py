"""Assemble locally owned OFP worlds. Generated retail data must not be committed.

Inputs are RVW4 exports from PoseidonTools terrain export-rvw4. No rescaling or
rotation: translations preserve road pieces, forests and authored building poses.
With --config (the debinarised BIN config and Noe's), every source world's airport,
map names and ambient sounds come along too, moved with their island.
"""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

CELL = 50
SIZE = 2048  # cells of 50 m: 102.4 km, the largest world the engine supports (TerrainSatmap caps landRange at 2048)
# Grid offsets (cells, x east / z north), each original world is 256 x 256 cells; adjacent tiles have at least
# 3.2 km (64 cells) of water between them. The layout follows the CWA 1985 campaign, then Reforger's
# Malden-islands map where CWA is silent (owner, 2026-10-09: "what would be closest to CWA"):
#   * Everon north-east of Malden ("Ile d'Everon, au nord-est de Malden", 01flashpoint) and the easternmost
#     island of the group ("Everon, the easternmost island in this group", x01iansoffice), within helicopter range
#     ("Choppers are waiting to ferry you to Everon", x01iansoffice). No campaign asks for open ocean around it:
#     it sits just north of Kolgujev, ~6 km coast to coast like the group's other neighbours (owner, 2026-10-09:
#     "do what fits best"; it was in the far north-east corner, 16 km from its nearest neighbour, 32 km from Malden);
#   * Kolgujev east-north-east of Malden: 31aplanesstart's A-10s leave Malden's runway north-east ("heading east to
#     Kolgujev"), and 31bmaverick starts them east-south-east of Kolgujev heading west-north-west, then along the
#     south coast -- due east of Malden's field, as here;
#   * Nogova one of "the Malden islands" (x06allies): US forces reach it from the south / south-west and Soviet
#     Su-25s from the south-east (x12gameover, x13badend), i.e. Malden south of it and Kolgujev south-east;
#   * Nogova north-north-west of Malden (Reforger, consistent with the above). Reforger also has it
#     south-south-west of Everon -- at this scale that cannot hold together with the above, so here Everon is
#     east of it;
#   * Desert Island (the menu intro island, in no campaign) is left out: owner 2026-10-09, "only a test island",
#     and it frees resources.
#
# Scale (owner 2026-10-09: "make the gaps realistic but lore accurate"): canon puts these islands 180-300 km
# apart, far beyond the engine's 102.4 km limit. The layout is the 51.2 km one scaled by 2 about the world's corner
# (every island's centre twice as far out), so every bearing above is unchanged and the gaps are those of a real
# island group: 20-25 km of sea between neighbours, ~68 km Malden to Everon centre to centre (a Black Hawk:
# ~15-20 min). Above 51.2 km the terrain refinement budget (8192 samples a side) gives 12.5 m instead of 6.25 m.
ISLANDS = [('eden', 'Everon', 1472, 1216), ('abel', 'Malden', 448, 320),
           ('cain', 'Kolgujev', 1088, 576), ('noe', 'Nogova', 192, 992)]
# the source worlds' CfgWorlds classes
WORLD_CLASS = {'eden': 'Eden', 'abel': 'Abel', 'cain': 'Cain', 'noe': 'Noe'}


def fixed(text, size):
    raw = text.encode('ascii')
    if len(raw) >= size:
        raise ValueError(f'Path too long: {text}')
    return raw + bytes(size-len(raw))


def read_world(path):
    raw = path.read_bytes()
    if raw[:4] != b'4WVR':
        raise ValueError(f'{path}: use PoseidonTools terrain export-rvw4 first')
    width, height = struct.unpack_from('<II', raw, 4)
    if width != 256 or height != 256:
        raise ValueError(f'{path}: expected original 256-cell OFP world')
    n = width*height
    heightmap = array('h'); heightmap.frombytes(raw[12:12+2*n])
    textures = array('H'); textures.frombytes(raw[12+2*n:12+4*n])
    if sys.byteorder != 'little':
        heightmap.byteswap(); textures.byteswap()
    table = [raw[12+4*n+i*32:12+4*n+(i+1)*32].split(b'\0')[0].decode('ascii') for i in range(512)]
    objects = raw[12+4*n+512*32:]
    if len(objects) % 128 or len(heightmap) != n or len(textures) != n:
        raise ValueError(f'{path}: truncated RVW4')
    if any(i >= 512 for i in textures):
        raise ValueError(f'{path}: invalid texture index')
    return heightmap, textures, table, objects, hashlib.sha256(raw).hexdigest()


def translate_objects(raw, dx, dz, first_id):
    result = bytearray(raw)
    for i in range(len(raw)//128):
        at = i*128
        x, y, z = struct.unpack_from('<3f', raw, at+36)
        struct.pack_into('<3fI', result, at+36, x+dx, y, z+dz, first_id+i)
    return result


def pack_pbo(files, prefix='fusion_ofp'):
    def z(text): return text.encode('ascii')+b'\0'
    header = b'\0'+struct.pack('<5I', 0x56657273,0,0,0,0)+z('prefix')+z(prefix)+b'\0'
    ordered = sorted(files.items())
    for name, data in ordered:
        header += z(name)+struct.pack('<5I',0,len(data),0,0,len(data))
    return header+b'\0'+bytes(20)+b''.join(data for _, data in ordered)


def compressed(raw):
    # Valid literal-only SSCompress stream. No lossy compression, and no extra
    # runtime decoder: the existing legacy OPRW loader reads this directly.
    if len(raw) < 1024:
        return raw
    result = bytearray()
    for i in range(0,len(raw),8):
        result.append(255)
        result.extend(raw[i:i+8])
    result.extend(struct.pack('<I',sum(raw) & 0xffffffff))
    return result


# --- the source worlds' airports and map names, from debinarised config text ---------------------------------

_CLASS = re.compile(r'\bclass\s+(\w+)\s*(?::\s*\w+)?\s*\{')


def _close(text, i):
    """Index just past the brace that closes the one opened before `i`."""
    depth = 1
    while depth and i < len(text):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return i


def class_block(text, name, start=0):
    """The body (without braces) of the first `class <name>` at or after `start`; None when absent."""
    for m in _CLASS.finditer(text, start):
        if m.group(1).lower() == name.lower():
            return text[m.end():_close(text, m.end()) - 1]
    return None


def top_level(body):
    """`body` with its nested classes cut out: this class's own entries."""
    out, i = [], 0
    while True:
        m = _CLASS.search(body, i)
        if not m:
            out.append(body[i:])
            return ''.join(out)
        out.append(body[i:m.start()])
        i = _close(body, m.end())


def array_entry(body, key):
    """The values of `key[]={...};` among a class's own entries: numbers, or the arithmetic strings the taxi
    paths use ("4772+69"), evaluated. None when absent."""
    m = re.search(r'\b%s\[\]\s*=\s*\{(.*?)\};' % re.escape(key), top_level(body), re.S)
    if not m:
        return None
    values = []
    for quoted, bare in re.findall(r'"([^"]*)"|([-+]?[\d.]+(?:e[-+]?\d+)?)', m.group(1)):
        expr = quoted or bare
        if not re.fullmatch(r'[\d.+\-*/ ()e]+', expr):
            raise ValueError(f'unexpected config value {expr!r} in {key}')
        values.append(float(eval(expr, {'__builtins__': {}}, {})))
    return values


def scalar_entry(body, key):
    m = re.search(r'\b%s\s*=\s*(?:"([^"]*)"|([^;]*));' % re.escape(key), top_level(body))
    if not m:
        return None
    return m.group(1) if m.group(1) is not None else m.group(2).strip()


def world_data(config_texts, key, dx, dz):
    """A source world's airport (moved by dx, dz metres; None if it has none) and its map names."""
    for text in config_texts:
        worlds = class_block(text, 'CfgWorlds')
        body = class_block(worlds, WORLD_CLASS[key]) if worlds else None
        if body is None:
            continue
        airport = None
        pos, direction = array_entry(body, 'ilsPosition'), array_entry(body, 'ilsDirection')
        if pos and (pos[0] or pos[1]) and direction:
            def moved(path):
                return [v + (dx if i % 2 == 0 else dz) for i, v in enumerate(path or [])]
            airport = dict(pos=[pos[0] + dx, pos[1] + dz], dir=direction[:3],
                           taxi_in=moved(array_entry(body, 'ilsTaxiIn')),
                           taxi_off=moved(array_entry(body, 'ilsTaxiOff')))
        names = []
        names_body = class_block(body, 'Names')
        if names_body:
            for m in _CLASS.finditer(names_body):
                if names_body.count('{', 0, m.start()) != names_body.count('}', 0, m.start()):
                    continue  # nested deeper than one level
                entry = names_body[m.end():_close(names_body, m.end()) - 1]
                p = array_entry(entry, 'position')
                if not p:
                    continue
                item = dict(cls=m.group(1), name=scalar_entry(entry, 'name') or m.group(1),
                            position=[p[0] + dx, p[1] + dz] + p[2:])
                for k in ('type', 'radiusA', 'radiusB', 'angle'):
                    v = scalar_entry(entry, k)
                    if v is not None:
                        item[k] = v
                names.append(item)
        return airport, names
    return None, []


def replace_objects(config_texts):
    """Every source world's ReplaceObjects rules (DefaultWorld's, which Everon / Kolgujev use,
    plus Malden's and Nogova's own), one per replaced model. The engine swaps a forest block for its walk-through
    version by them (Geography.cpp); Fusion inherited only DefaultWorld's, so Malden's and Nogova's forests -- other
    models -- would have stayed solid to infantry and AI."""
    rules, seen = [], set()
    for text in config_texts:
        worlds = class_block(text, 'CfgWorlds')
        if not worlds:
            continue
        for world in ['DefaultWorld'] + list(WORLD_CLASS.values()):
            body = class_block(worlds, world)
            block = class_block(body, 'ReplaceObjects') if body else None
            if not block:
                continue
            for m in _CLASS.finditer(block):
                if block.count('{', 0, m.start()) != block.count('}', 0, m.start()):
                    continue
                rule = block[m.end():_close(block, m.end()) - 1]
                model = (scalar_entry(rule, 'replace') or '').lower()
                if model and model not in seen:
                    seen.add(model)
                    rules.append('class %s_%s {%s};' % (world, m.group(1), ' '.join(rule.split())))
    return rules


def env_sounds(config_texts, key):
    """A source world's own EnvSounds entries, as the body of a `class EnvSounds: CfgEnvSounds` (Malden overrides
    Sea, Trees, Meadows, Hills and Rain; the others keep CfgEnvSounds'). None without its world class."""
    for text in config_texts:
        worlds = class_block(text, 'CfgWorlds')
        body = class_block(worlds, WORLD_CLASS[key]) if worlds else None
        if body is None:
            continue
        block = class_block(body, 'EnvSounds')
        return ' '.join(block.split()) if block else ''
    return None


# Fusion's own ambience for the islands whose worlds have none (owner, 2026-10-09: "give the other islands their
# own sounds"). Not from OFP: the stock CfgEnvSounds is Everon's, and Malden keeps its own. Only CWA's own loops
# (DTA\Sound.pbo), and only those that loop cleanly, measured: level step across the seam within 2.5 dB and a steady
# level (p90-p10 of 20 ms frames <= 14 dB) -- wave_break, rain_hard, treesbirds, wind_heavy / _whistle / _thick,
# crickets01 / 03 / 07, frogs. The bird calls (skylark, n_gale, jay ...) and frog2_* are single calls with gaps, so
# they can't loop. Volumes are relative to CfgEnvSounds' (Sea 3.16e-4, Trees and Hills 1.78e-4, Meadows 5.62e-5,
# Rain 1.78e-3). Per file: wind_thick is ~6 dB hotter than wind_heavy, crickets01 ~5 dB quieter than crickets07.
# Per class: (day file, day volume, night file, night volume).
ISLAND_AMBIENCE = {
    # Kolgujev: the cold, windswept northern island -- a rougher sea, a hard wind, fewer birds and crickets
    'cain': {'Sea': ('environmental\\wave_break', 4.47e-4, 'environmental\\wave_break', 4.47e-4),
             'Trees': ('environmental\\treesbirds', 1.26e-4, 'animals\\crickets01', 1.78e-4),
             'Meadows': ('animals\\crickets01', 5.62e-5, 'animals\\crickets01', 3.16e-5),
             'Hills': ('environmental\\wind_thick', 1.26e-4, 'environmental\\wind_whistle', 2.51e-4)},
    # Nogova: green and sheltered, valleys and streams -- more birdsong, a calmer sea, frogs in the meadows at night
    'noe': {'Sea': ('environmental\\wave_break', 2.24e-4, 'environmental\\wave_break', 2.24e-4),
            'Trees': ('environmental\\treesbirds', 2.82e-4, 'animals\\crickets07', 1.26e-4),
            'Meadows': ('animals\\crickets03', 5.62e-5, 'animals\\frogs', 1.0e-4),
            'Hills': ('environmental\\wind_heavy', 1.0e-4, 'environmental\\wind_heavy', 1.0e-4)},
}


def ambience_cfg(key):
    """ISLAND_AMBIENCE's classes for an island, as EnvSounds entries ('' when it has none)."""
    return ' '.join('class %s { sound[]={"%s",%g,1}; soundNight[]={"%s",%g,1}; };' % (cls, day, dv, night, nv)
                    for cls, (day, dv, night, nv) in ISLAND_AMBIENCE.get(key, {}).items())


def island_env_sounds(config_texts, key):
    """An island's EnvSounds entries: its world's own (Malden; or a mod's), else Fusion's ISLAND_AMBIENCE, else
    none (CfgEnvSounds, Everon's)."""
    return env_sounds(config_texts, key) or ambience_cfg(key)


def sound_regions(config_texts):
    """One SoundRegions class per island: its tile and its EnvSounds (the engine's EnvSoundRegions.hpp)."""
    return ''.join('class %s {area[]={%d,%d,%d,%d}; class EnvSounds: CfgEnvSounds {%s};};'
                   % (label.replace(' ', ''), ox * CELL, oz * CELL, (ox + 256) * CELL, (oz + 256) * CELL,
                      island_env_sounds(config_texts, key))
                   for key, label, ox, oz in ISLANDS)


def num(v):
    return ('%.3f' % v).rstrip('0').rstrip('.')


def airport_cfg(a):
    def listed(vs):
        return ','.join(num(v) for v in vs)
    return (' ilsPosition[]={%s}; ilsDirection[]={%s}; ilsTaxiIn[]={%s}; ilsTaxiOff[]={%s};'
            % (listed(a['pos']), listed(a['dir']), listed(a['taxi_in']), listed(a['taxi_off'])))


def name_cfg(key, n):
    extra = ''.join(' type="%s";' % v if k == 'type' else ' %s=%s;' % (k, v)
                    for k, v in n.items() if k in ('type', 'radiusA', 'radiusB', 'angle'))
    return 'class %s_%s {name="%s"; position[]={%s};%s};' % (
        key, n['cls'], n['name'], ','.join(num(v) for v in n['position']), extra)


def build(source, output, config_texts=()):
    heights = array('f', [-100])*(SIZE*SIZE)
    tex = array('H', [0])*(SIZE*SIZE)
    geography = bytearray(SIZE*SIZE*4)
    sounds = bytearray(SIZE*SIZE)
    random = bytearray(SIZE*SIZE*4)
    mountains = []
    names = ['landtext\\mo.pac']
    lookup = {names[0]: 0}
    placements = bytearray()
    manifest = {'cell_metres': CELL, 'world_metres': SIZE*CELL, 'islands': []}
    for key, label, ox, oz in ISLANDS:
        h, t, table, objects, digest = read_world(source/f'{key}.wrp')
        meta=(source/f'{key}.wrp.meta').read_bytes()
        n=256*256
        if meta[:4] != b'FMD1' or len(meta) < 8+n*13:
            raise ValueError('Missing/invalid source metadata')
        exact = array('f'); exact.frombytes(meta[4+n*9:4+n*13])
        if sys.byteorder != 'little': exact.byteswap()
        peaks=struct.unpack_from('<I',meta,4+n*13)[0]
        if len(meta) != 8+n*13+peaks*12:
            raise ValueError('Truncated mountain metadata')
        for x,y,z in struct.iter_unpack('<3f',meta[8+n*13:]):
            mountains.append((x+ox*CELL,y,z+oz*CELL))
        if key == 'eden':
            sea=min(range(n),key=exact.__getitem__)
            geography[:]=meta[4+sea*4:8+sea*4]*(SIZE*SIZE)
            sounds[:]=bytes([meta[4+n*4+sea]])*(SIZE*SIZE)
        remap = {}
        for index in sorted(set(t)):
            name = table[index].replace('/', '\\').lower()
            if not name:
                name = names[0]
            if name not in lookup:
                lookup[name] = len(names); names.append(name)
            remap[index] = lookup[name]
        if len(names) > 32767:
            raise ValueError('Combined texture table exceeds signed index range')
        for z in range(256):
            at = (oz+z)*SIZE+ox
            heights[at:at+256] = exact[z*256:(z+1)*256]
            tex[at:at+256] = array('H', (remap[i] for i in t[z*256:(z+1)*256]))
            geography[at*4:(at+256)*4]=meta[4+z*1024:4+(z+1)*1024]
            sounds[at:at+256]=meta[4+n*4+z*256:4+n*4+(z+1)*256]
            random[at*4:(at+256)*4]=meta[4+n*5+z*1024:4+n*5+(z+1)*1024]
        first = len(placements)//128
        placements.extend(translate_objects(objects, ox*CELL, oz*CELL, first))
        manifest['islands'].append(dict(key=key,name=label,offset=[ox*CELL,oz*CELL],
            extent=256*CELL,objects=len(objects)//128,first_object_id=first,source_sha256=digest))
    data = b'OPRW'+struct.pack('<5I',3,SIZE,SIZE,SIZE,SIZE)
    if sys.byteorder != 'little': heights.byteswap(); tex.byteswap()
    data += compressed(geography)+compressed(sounds)
    data += struct.pack('<I',len(mountains))+b''.join(struct.pack('<3f',*p) for p in mountains)
    data += compressed(tex.tobytes())+compressed(random)+compressed(heights.tobytes())
    data += struct.pack('<I',len(names))+b''.join(n.encode('ascii')+b'\0\0' for n in names)
    models=sorted({bytes(placements[i+52:i+128]).split(b'\0')[0] for i in range(0,len(placements),128)})
    model_lookup={name:i for i,name in enumerate(models)}
    data += struct.pack('<I',len(models))+b''.join(m+b'\0' for m in models)
    encoded=bytearray()
    for i in range(0,len(placements),128):
        name=bytes(placements[i+52:i+128]).split(b'\0')[0]
        encoded.extend(struct.pack('<II',i//128,model_lookup[name]))
        encoded.extend(placements[i:i+48])
    data += encoded+struct.pack('<i',-1)
    manifest['textures'] = len(names)
    manifest['objects'] = len(placements)//128
    manifest['world_sha256'] = hashlib.sha256(data).hexdigest()
    labels = ''.join(f'class Island{i} {{name="{v[1]}"; position[]={{{(v[2]+128)*CELL},{(v[3]+128)*CELL}}}; type="NameLocal"; radiusA=4000; radiusB=4000;}};' for i,v in enumerate(ISLANDS))
    # every source world's own airport and map names (towns, villages, places), moved with its island
    airports, places = [], []
    for key, label, ox, oz in ISLANDS:
        airport, world_names = world_data(config_texts, key, ox * CELL, oz * CELL)
        if airport:
            airports.append((key, label, airport))
        places.extend(name_cfg(key, n) for n in world_names)
    manifest['airports'] = [dict(island=label, ils=a['pos']) for _, label, a in airports]
    manifest['map_names'] = len(places)
    # the main airport is Everon's (the campaign's); the others are SecondaryAirports, each AI plane using the
    # nearest (World::NearestAirport). Without config input: Everon's ILS point, moved, and no taxi paths.
    main = next((a for k, _, a in airports if k == 'eden'), None) or (airports[0][2] if airports else None)
    if main:
        # its name for the missions' setAirportClosed / setAirportSide (the SecondaryAirports go by their class)
        ils = ' airportName="%s";' % next(label for _, label, a in airports if a is main) + airport_cfg(main)
    else:
        ils = (' ilsPosition[]={%d,%d}; ilsDirection[]={0,0.08,-1}; ilsTaxiIn[]={}; ilsTaxiOff[]={};'
               % (4772 + ISLANDS[0][2] * CELL, 10923 + ISLANDS[0][3] * CELL))
    secondary = ''.join('class %s {%s};' % (label.replace(' ', ''), airport_cfg(a))
                        for _, label, a in airports if a is not main)
    replace = replace_objects(config_texts)
    manifest['replace_objects'] = len(replace)
    replace_cfg = (' class ReplaceObjects {%s};\n' % ''.join(replace)) if replace else ''
    # each island its own ambient loops, crossfaded over the water between them
    regions = sound_regions(config_texts)
    manifest['sound_regions'] = {label: ('world' if env_sounds(config_texts, key) else
                                         'fusion' if key in ISLAND_AMBIENCE else 'CfgEnvSounds')
                                 for key, label, _, _ in ISLANDS}
    config = ('class CfgPatches {class FusionOFP {units[]={}; weapons[]={}; requiredVersion=1.96; requiredAddons[]={"Noe"};};};\n'
              # the island sound classes derive from the game's CfgEnvSounds: an add-on must declare it before use
              # (the engine resolves a base class while it reads the file; undeclared, the parse of the rest of
              # FusionOFP failed -- found by the in-game test, 2026-10-09)
              'class CfgEnvSounds;\n'
              'class CfgWorlds {class Eden; class FusionOFP: Eden {\n'
              ' access=2; description="OFP Fusion - Four Islands"; worldName="\\fusion_ofp\\FusionOFP.wrp";\n'
              ' worldSize=%d; landGrid=50; centerPosition[]={%d,%d,0};\n'
              ' startTime="12:00"; startDate="21/6/1985"; startWeather=0.2; forecastWeather=0.2;\n'
              ' startFog=0; forecastFog=0; seagullPos[]={%d,%d,150};\n'
              '%s\n'
              # the map grid: the islands' "Aa"/"00" labels (base 10 per character, UIMap GridFormat) run out at 100
              # squares of 128 m, the far zoom's "A"/"0" at 10 of 1280 m; Fusion is 800 / 80 across
              ' class Grid {offsetX=0; offsetY=0;'
              ' class Zoom1 {zoomMax=0.2; format="XY"; formatX="Aa0"; formatY="000"; stepX=128; stepY=128;};'
              ' class Zoom2 {zoomMax=1e30; format="XY"; formatX="Aa"; formatY="00"; stepX=1280; stepY=1280;};};\n'
              ' class SecondaryAirports {%s};\n'
              '%s'
              ' class SoundRegions {%s};\n'
              ' class Names {%s};\n'
              '};}; class CfgWorldList {class FusionOFP {};};\n'
              % (SIZE * CELL, SIZE * CELL // 2, SIZE * CELL // 2,
                 (ISLANDS[1][2] + 128) * CELL, (ISLANDS[1][3] + 128) * CELL, ils, secondary, replace_cfg, regions,
                 labels + ''.join(places)))
    output.mkdir(parents=True, exist_ok=True)
    (output/'fusion.wrp').write_bytes(data)
    (output/'fusion_ofp.pbo').write_bytes(pack_pbo({'config.cpp':config.encode('latin-1'),'FusionOFP.wrp':data}))
    (output/'config.cpp').write_text(config, encoding='latin-1')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='ascii')
    print(json.dumps(manifest,indent=2))


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    parser.add_argument('--config',type=Path,action='append',default=[],
                        help='debinarised config text with the source worlds (BIN config, Noe): airports and map names')
    args=parser.parse_args()
    build(args.source,args.output,[c.read_text(encoding='latin-1') for c in args.config])
