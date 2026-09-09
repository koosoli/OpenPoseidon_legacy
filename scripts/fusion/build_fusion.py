"""Assemble locally owned OFP worlds. Generated retail data must not be committed.

Inputs are RVW4 exports from PoseidonTools terrain export-rvw4. No rescaling or
rotation: translations preserve road pieces, forests and authored building poses.
"""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import struct
import sys

CELL = 50
SIZE = 1024
# Grid offsets, each original world is 256 x 256 cells. Adjacent source tiles
# have at least 3.2 km of additional water between them.
ISLANDS = [('eden', 'Everon', 64, 160), ('abel', 'Malden', 384, 160),
           ('cain', 'Kolgujev', 704, 160), ('noe', 'Nogova', 224, 480),
           ('intro', 'Desert Island', 544, 480)]


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


def build(source, output):
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
    config = '''class CfgPatches {class FusionOFP {units[]={}; weapons[]={}; requiredVersion=1.96; requiredAddons[]={"Noe"};};};
class CfgWorlds {class Eden; class FusionOFP: Eden {
 access=2; description="OFP Fusion - Five Islands"; worldName="\\fusion_ofp\\FusionOFP.wrp";
 worldSize=51200; landGrid=50; centerPosition[]={25600,25600,0};
 startTime="12:00"; startDate="21/6/1985"; startWeather=0.2; forecastWeather=0.2;
 startFog=0; forecastFog=0; seagullPos[]={9600,14400,150};
 ilsPosition[]={7972,18923}; ilsDirection[]={0,0.08,-1}; ilsTaxiIn[]={}; ilsTaxiOff[]={};
 class Names {'''+labels+'''};
};}; class CfgWorldList {class FusionOFP {};};
'''
    output.mkdir(parents=True, exist_ok=True)
    (output/'fusion.wrp').write_bytes(data)
    (output/'fusion_ofp.pbo').write_bytes(pack_pbo({'config.cpp':config.encode('ascii'),'FusionOFP.wrp':data}))
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='ascii')
    print(json.dumps(manifest,indent=2))


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    build(args.source,args.output)
