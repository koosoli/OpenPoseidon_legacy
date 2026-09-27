"""Deterministic, privately authored two-visual-LOD OFP-format pilot. No retail assets.

Fine has four coplanar quads per box face; coarse has one. This is an exact
authored planar-shell fallback, not a general simplifier/error certificate.
Offline selfcheck is run explicitly by the root runtime/build operator.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

HELPER = Path(__file__).resolve().parents[1] / 'showcase' / 'build_showcase.py'
spec = importlib.util.spec_from_file_location('geometry_page_authored_helper', HELPER)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)  # module only defines functions; guarded CLI stays inactive

SIZE = (2.0, 3.0, 2.0)

def fine_lod():
    x, y, z = SIZE
    corners = [(-x,0,-z),(x,0,-z),(x,0,z),(-x,0,z),
               (-x,y,-z),(x,y,-z),(x,y,z),(-x,y,z)]
    faces = [(0,1,2,3),(4,7,6,5),(0,4,5,1),
             (1,5,6,2),(2,6,7,3),(3,7,4,0)]
    normals = [(0,-1,0),(0,1,0),(0,0,-1),(1,0,0),(0,0,1),(-1,0,0)]
    points, quads = [], []
    for face, ids in enumerate(faces):
        a,b,c,d = [corners[i] for i in ids]
        def point(u,v):
            return tuple((1-v)*((1-u)*a[k]+u*b[k])+v*((1-u)*d[k]+u*c[k]) for k in range(3))
        for row in range(2):
            for col in range(2):
                uv = [(col/2,row/2),((col+1)/2,row/2),
                      ((col+1)/2,(row+1)/2),(col/2,(row+1)/2)]
                first = len(points)
                points.extend(point(u,v) for u,v in uv)
                quads.append((face, [first+1,first,first+3,first+2], uv))
    data = b'P3DM'+struct.pack('<6I',28,256,len(points),6,len(quads),0)
    data += b''.join(struct.pack('<3fI',*p,0) for p in points)
    data += b''.join(struct.pack('<3f',*[-c for c in n]) for n in normals)
    for face, ids, uv in quads:
        data += struct.pack('<I',4)
        for index, local in zip(ids, [1,0,3,2]):
            data += struct.pack('<IIff',index,face,*uv[local])
        data += struct.pack('<I',0)+b'\0\0'  # no texture/material dependency
    return data+b'TAGG'+helper.tag('#EndOfFile#',b'')+struct.pack('<f',1.0)

def build():
    physical = [helper.box_lod(SIZE,resolution=r,geometry=True) for r in (1e13,6e15,7e15)]
    lods = [fine_lod(),helper.box_lod(SIZE,resolution=10.0),*physical]
    return b'MLOD'+struct.pack('<II',257,len(lods))+b''.join(lods), lods

def check_lod(data, vertices, faces):
    assert data[:4] == b'P3DM'
    header = struct.unpack_from('<6I',data,4)
    assert header == (28,256,vertices,6,faces,0)
    at = 28+vertices*16+6*12
    for _ in range(faces):
        n, = struct.unpack_from('<I',data,at); at += 4
        assert n == 4
        for _ in range(n):
            index, normal, u, v = struct.unpack_from('<IIff',data,at); at += 16
            assert index < vertices and normal < 6 and 0 <= u <= 1 and 0 <= v <= 1
        flags, = struct.unpack_from('<I',data,at); at += 4
        assert flags == 0 and data[at:at+2] == b'\0\0'; at += 2
    assert data[at:at+4] == b'TAGG'

def selfcheck():
    data,lods = build()
    assert data == build()[0] and len(data) < 65536
    assert struct.unpack_from('<II',data,4) == (257,5)
    check_lod(lods[0],96,24); check_lod(lods[1],8,6)
    for i,r in enumerate((1e13,6e15,7e15)):
        assert lods[i+2] == helper.box_lod(SIZE,resolution=r,geometry=True)
    assert b'.paa' not in data and b'.rvmat' not in data
    return data

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path)
    parser.add_argument('--selfcheck',action='store_true')
    args = parser.parse_args()
    data = selfcheck()
    manifest = {'format':'MLOD257','authored':True,'sourceSha256':hashlib.sha256(data).hexdigest(),
        'generatorSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'helperSha256':hashlib.sha256(HELPER.read_bytes()).hexdigest(),
        'fineLevel':0,'coarseLevel':1,'fineVertices':96,'fineTriangles':48,
        'coarseVertices':8,'coarseTriangles':12,'physicalRoles':['Geometry','Fire','View'],
        'material':'untextured opaque','byteLength':len(data),
        'scope':'controlled authored planar shell; no retail provenance or runtime proof'}
    if args.output_dir:
        args.output_dir.mkdir(parents=True,exist_ok=True)
        (args.output_dir/'geometry_page_pilot.p3d').write_bytes(data)
        (args.output_dir/'geometry_page_pilot.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(manifest,sort_keys=True))
