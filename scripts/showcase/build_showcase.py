"""Build the original, redistributable Showcase Lab addon (stdlib only).

No retail assets are copied. Output is deterministic: authored P3DM boxes,
uncompressed PAA material swatches and an uncompressed PBO container.
"""
import argparse
import math
from pathlib import Path
import struct


def z(value):
    return value.encode('ascii') + b'\0'


def tag(name, payload):
    return b'\1' + z(name) + struct.pack('<I', len(payload)) + payload


def box_lod(size, texture='', material='', resolution=1.0, geometry=False):
    x, y, zz = size
    points = [(-x, 0, -zz), (x, 0, -zz), (x, 0, zz), (-x, 0, zz),
              (-x, y, -zz), (x, y, -zz), (x, y, zz), (-x, y, zz)]
    quads = [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1),
             (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]
    normals = [(0, -1, 0), (0, 1, 0), (0, 0, -1), (1, 0, 0), (0, 0, 1), (-1, 0, 0)]
    data = b'P3DM' + struct.pack('<6I', 28, 256, 8, 6, 6, 0)
    data += b''.join(struct.pack('<3fI', *p, 0) for p in points)
    # Shape normals point inward. MeshBuild negates them for the GPU.
    data += b''.join(struct.pack('<3f', *[-c for c in n]) for n in normals)
    for face, q in enumerate(quads):
        # P3DM loader swaps 0/1 and 2/3. Keep visible and collision shells aligned.
        order = [q[1], q[0], q[3], q[2]]
        data += struct.pack('<I', 4)
        for p in order:
            px, py, pz = points[p]
            if face in (2, 4):
                u, v = (px + x) / (2*x), 1 - py / y
                if face == 4:
                    u = 1 - u
            elif face in (3, 5):
                u, v = (pz + zz) / (2*zz), 1 - py / y
            else:
                u, v = (px + x) / (2*x), (pz + zz) / (2*zz)
            data += struct.pack('<IIff', p, face, u, v)
        data += struct.pack('<I', 0) + z(texture) + z(material)
    data += b'TAGG'
    if geometry:
        data += tag('Component01', b'\1' * 14)
        data += tag('#Mass#', struct.pack('<8f', *([1000.0] * 8)))
    data += tag('#EndOfFile#', b'') + struct.pack('<f', resolution)
    return data


def model(size, texture, material):
    lods = [box_lod(size, texture, material),
            box_lod(size, resolution=1e13, geometry=True),
            box_lod(size, resolution=6e15, geometry=True),
            box_lod(size, resolution=7e15, geometry=True)]
    return b'MLOD' + struct.pack('<II', 257, len(lods)) + b''.join(lods)


def tga(normal=False):
    n = 128
    data = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, n, n, 32, 40)
    for y in range(n):
        for x in range(n):
            if normal:
                # Smooth crossed ribs, stored as DXT5nm/NOHQ: tangent X in A, Y in G.
                nx = 0.60 * math.sin(2 * math.pi * x / 32)
                ny = 0.45 * math.sin(2 * math.pi * y / 32)
                pixel = (255, round(127.5 * (ny + 1)), 0, round(127.5 * (nx + 1)))
            else:
                c = 160 if x % 32 < 2 or y % 32 < 2 else 185
                pixel = (c, c, c, 255)
            data += bytes(pixel)
    return data


def material(normal, specular):
    normal_path = r'showcase_lab\ribs_nohq.paa' if normal else '#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)'
    return f'''ambient[]={{1,1,1,1}};
diffuse[]={{1,1,1,1}};
forcedDiffuse[]={{0,0,0,0}};
emmisive[]={{0,0,0,0}};
specular[]={{{specular},{specular},{specular},1}};
specularPower=60;
PixelShaderID="NormalMapSpecularDIMap";
VertexShaderID="NormalMap";
class Stage1 {{texture="{normal_path}"; uvSource="tex";}};
class Stage2 {{texture="#(argb,8,8,3)color(0,{specular},0.7,1,SMDI)"; uvSource="tex";}};
'''.encode('ascii')


def paa(normal=False, label='', colour=None):
    pixels = bytearray(tga(normal)[18:] if colour is None else bytes(colour) * (128 * 128))
    if label:
        font = {
            'F': ['11111','10000','10000','11110','10000','10000','10000'],
            'L': ['10000','10000','10000','10000','10000','10000','11111'],
            'A': ['01110','10001','10001','11111','10001','10001','10001'],
            'T': ['11111','00100','00100','00100','00100','00100','00100'],
            'N': ['10001','11001','11001','10101','10011','10011','10001'],
            'O': ['01110','10001','10001','10001','10001','10001','01110'],
            'R': ['11110','10001','10001','11110','10100','10010','10001'],
            'M': ['10001','11011','10101','10101','10001','10001','10001'],
            'G': ['01110','10001','10000','10111','10001','10001','01110'],
            'S': ['01111','10000','10000','01110','00001','00001','11110'],
        }
        for y in range(30):
            for x in range(128):
                at = (y * 128 + x) * 4
                pixels[at:at+4] = bytes((24, 32, 40, 255))
        left = (128 - len(label) * 12) // 2
        for letter, char in enumerate(label):
            for y, row in enumerate(font[char]):
                for x, bit in enumerate(row):
                    if bit == '1':
                        for dy in range(2):
                            for dx in range(2):
                                at = ((y*2+dy+8)*128+left+letter*12+x*2+dx)*4
                                pixels[at:at+4] = bytes((90, 195, 240, 255))
    n = 128
    data = struct.pack('<HH', 0x8888, 0)
    while n:
        data += struct.pack('<HH', n, n) + len(pixels).to_bytes(3, 'little') + pixels
        smaller = bytearray()
        for y in range(n // 2):
            for x in range(n // 2):
                for c in range(4):
                    smaller.append(sum(pixels[((y*2+dy)*n+x*2+dx)*4+c]
                                       for dy in range(2) for dx in range(2)) // 4)
        pixels = bytes(smaller)
        n //= 2
    return data + bytes(4)


def build():
    files = {'grid_co.paa': paa(), 'ribs_nohq.paa': paa(True)}
    classes = []
    for name, size, normal, specular in [
        ('PanelFlat', (2, 3, .15), False, .05),
        ('PanelNormal', (2, 3, .15), True, .05),
        ('PanelGloss', (2, 3, .15), True, .8),
        ('PanelGlass', (2, 3, .025), False, .8),
        ('GlassFrame', (2.16, .12, .10), False, .2),
        ('GlassPost', (.08, 3, .10), False, .2),
        ('Wall', (.25, 4.5, 7), False, .05),
        ('Roof', (7, .3, 7), False, .05),
        ('Plinth', (1, .4, 1), False, .05),
        ('RoomBack', (7, 4.5, .25), False, .02),
        ('DoorSide', (3.15, 4.5, .25), False, .02),
    ]:
        files[name + '.rvmat'] = material(normal, specular)
        texture = 'grid_co.paa'
        if name.startswith('Panel'):
            texture = name + ('_ca.paa' if name == 'PanelGlass' else '_co.paa')
            files[texture] = (paa(colour=(220, 235, 200, 56)) if name == 'PanelGlass'
                              else paa(label=name[5:].upper()))
        files[name + '.p3d'] = model(size, 'showcase_lab\\' + texture, 'showcase_lab\\' + name + '.rvmat')
        damage = 'armor=1; removeOnDestruction=1;' if name == 'PanelGlass' else 'armor=10000;'
        classes.append(f'class Lab{name}: House {{scope=2; displayName="Lab {name}"; model="\\showcase_lab\\{name}.p3d"; {damage}}};')
    files['config.cpp'] = ('''class CfgPatches {class ShowcaseLab {units[]={"LabPanelFlat","LabPanelNormal","LabPanelGloss","LabPanelGlass","LabGlassFrame","LabGlassPost","LabWall","LabRoof","LabPlinth","LabRoomBack","LabDoorSide"}; weapons[]={}; requiredVersion=1.96; requiredAddons[]={};};};
class CfgVehicles {class House;
''' + '\n'.join(classes) + '\n};\n').encode('ascii')
    header = z('') + struct.pack('<5I', 0x56657273, 0, 0, 0, 0) + z('prefix') + z('showcase_lab') + b'\0'
    for name, payload in sorted(files.items()):
        header += z(name) + struct.pack('<5I', 0, len(payload), 0, 0, len(payload))
    return header + b'\0' + bytes(20) + b''.join(v for _, v in sorted(files.items()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    payload = build()
    args.output.write_bytes(payload)
    print(f'{args.output}: {len(payload)} bytes')
