"""Read-only retail rotor land source/opacity audit; no game or GPU launches.

Requires NumPy, Pillow, clang++, PoseidonTools and an existing original-map
export from Audit-OriginalOFPContactSources.py. Compiles only a tiny CPU probe
against the actual production RotorGroundPolicy.hpp. Asset outputs are scratch
copies; the installed originals are never rewritten.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess

import numpy as np
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def model_sphere(rig, payload):
    # Observe the existing bounded, checksum-checked ODOL7 reader immediately
    # after its actual LOD resolutions, where P3DStructures.hpp defines iffiii.
    count = struct.unpack_from('<I', payload, 8)[0]
    original = rig.Reader
    headers = []
    class ObservedReader(original):
        pending_header = False
        def value(self, fmt):
            result = super().value(fmt)
            if fmt == 'f' * count:
                self.pending_header = True
            return result
        def take(self, size):
            value = super().take(size)
            if self.pending_header:
                if size != 24:
                    raise ValueError('Unexpected actual model header layout')
                headers.append(struct.unpack('<iffiii', value))
                self.pending_header = False
            return value
    try:
        rig.Reader = ObservedReader
        rig.parse_odol7(payload)  # must consume and validate the entire member
    finally:
        rig.Reader = original
    if len(headers) != 1 or not math.isfinite(headers[0][1]) or headers[0][1] <= 0:
        raise ValueError('Unproven authored aircraft sphere')
    return headers[0][1]


def actual_cpu_probe(out, compiler, sphere):
    source = r'''#include "Poseidon/World/Weather/RotorGroundPolicy.hpp"
#include <cstdio>
int main(){
 std::printf("{\"footprint\":%.9g,\"rings\":[",double(Poseidon::RotorLandProofRadius));
 for(unsigned phase=0;phase<128;++phase){std::printf("%s[",phase?",":"");
  for(unsigned s=0;s<8;++s){auto p=Poseidon::RotorGroundRing(phase,(s+phase)%8,SPHERE*.8f);
   std::printf("%s[%.9g,%.9g]",s?",":"",double(p[0]),double(p[1]));}std::printf("]");}
 std::printf("],\"profiles\":[");
 for(unsigned i=0;i<19;++i){const float age=float(i)*.1f;
  const float d=Poseidon::RotorGroundPolicy(1,9.7f,1,.62f,0,0,0,0,false,true,false,false).density;
  auto a=Poseidon::RotorLandProfile(age,d,false),b=Poseidon::RotorLandProfile(age,d,true);
  std::printf("%s[%.9g,%.9g,%.9g,%.9g,%.9g]",i?",":"",double(age),double(a.radius),double(a.opacity),double(b.radius),double(b.opacity));}
 std::printf("]}\n");}'''.replace('SPHERE', repr(sphere)+'f')
    cpp = out/'actual-policy-probe.cpp'
    exe = out/'actual-policy-probe.exe'
    cpp.write_text(source)
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(ROOT/'engine'), str(cpp), '-o', str(exe)], check=True)
    return json.loads(subprocess.check_output([str(exe)], text=True))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-audit', type=Path, required=True)
    p.add_argument('--game-dir', type=Path, required=True)
    p.add_argument('--tools', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--compiler', default='clang++')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rig = module('rig', ROOT/'scripts/physics/inspect_stock_corpse_rig.py')
    census = module('census', ROOT/'scripts/Audit-OriginalOFPContactSources.py')
    terrain = module('height', ROOT/'scripts/Audit-NogovaChurchSightline.py')
    archives = {key: args.game_dir/('Dta/'+key+'.pbo') for key in ('Data', 'Data3D')}
    def member(archive, name):
        rows, _ = rig.archive_index(archive)
        matched = [row for row in rows if row['name'].lower() == name.lower()]
        if len(matched) != 1:
            raise ValueError('Missing/duplicate exact retail member '+name)
        with archive.open('rb') as stream:
            payload, _ = rig.read_member(stream, matched[0])
        return payload
    aircraft = member(archives['Data3D'], 'uh-60.p3d')
    sphere = model_sphere(rig, aircraft)
    sheet = member(archives['Data3D'], 'cl_basic.p3d')
    rig.parse_odol7(sheet)
    if sheet.count(b'data\\basic.06.paa\0') != 1:
        raise ValueError('Actual CloudletBasic texture identity changed')
    images = []
    rows, _ = rig.archive_index(archives['Data'])
    names = {row['name'].lower() for row in rows}
    # Actual TextureBank starts at authored .06, stopping at the first absent
    # consecutive sheet, rather than guessing a .00 start or a single frame.
    for phase in range(6, 38):
        name = 'basic.%02d.paa' % phase
        if name not in names:
            break
        payload = member(archives['Data'], name)
        paa = args.out/name
        paa.write_bytes(payload)
        png = paa.with_suffix('.png')
        subprocess.run([str(args.tools.resolve()), 'image', 'convert', str(paa.resolve()), str(png.resolve())],
                       check=True, capture_output=True)
        pixels = np.array(Image.open(png).convert('RGBA'), dtype=float)/255
        alpha = pixels[:, :, 3]
        images.append(dict(name=name, sha256=sha(paa), width=alpha.shape[1], height=alpha.shape[0],
            meanAlpha=float(alpha.mean()), maxAlpha=float(alpha.max()),
            p90Alpha=float(np.quantile(alpha, .90)), supportedFraction=float(np.mean(alpha>0))))
    if not images or len(images) == 32:
        raise ValueError('Missing or unbounded animated texture sequence')
    actual = actual_cpu_probe(args.out, args.compiler, sphere)
    folder = args.source_audit
    _, ids, textures, buckets = census.read_export(folder/'noe.rvw4')
    metadata = (folder/'noe.rvw4.meta').read_bytes()
    if metadata[:4] != b'FMD1':
        raise ValueError('Exact original float height metadata required')
    grid = np.frombuffer(metadata, dtype='<f4', count=65536, offset=4+65536*9).reshape(256,256)
    definitions = census.surfaces(json.loads((folder/'installed-config.json').read_text()),
                                  json.loads((folder/'noe-config.json').read_text()))
    def sample(x, z):
        cx, cz = math.floor(x/50), math.floor(z/50)
        if not (0 <= cx < 255 and 0 <= cz < 255):
            raise ValueError('Outside bounded original map source')
        texture = textures[ids[cz*256+cx]]
        q = census.quadrants(texture, definitions)[(0 if z/50-cz<.5 else 2)+(0 if x/50-cx<.5 else 1)]
        return dict(x=x, z=z, height=terrain.height_at(grid,x,z), texture=texture,
                    quadrant=q, dust=definitions[q['surfaceClass']]['dust'])
    def site(x, z):
        counts, failures = collections.Counter(), collections.Counter()
        maximum_delta, dusts = 0, set()
        for row in actual['rings']:
            spent, accepted = 0, 0
            for dx, dz in row:
                centre = sample(x+dx,z+dz)
                radius = actual['footprint']
                for ex, ez in ((0,0),(radius,0),(-radius,0),(0,radius),(0,-radius)):
                    if spent == 32:
                        failures['budget'] += 1; break
                    spent += 1
                    v = sample(x+dx+ex,z+dz+ez)
                    delta = abs(v['height']-centre['height'])
                    maximum_delta = max(maximum_delta,delta)
                    dusts.add(v['dust'])
                    if v['height'] <= .03:
                        failures['sea'] += 1; break
                    if delta > 1:
                        failures['slope'] += 1; break
                    density = max(0,1-(9.7+terrain.height_at(grid,x,z)-v['height'])/30)**2*min(1,v['dust']*2.5)
                    if density < .025:
                        failures['source-density'] += 1; break
                else:
                    accepted += 1; continue
                if spent == 32:
                    break
            counts[accepted] += 1
        nearest = sorted((o for values in buckets.values() for o in values),
            key=lambda o:math.hypot(o['position'][0]-x,o['position'][2]-z))[:3]
        return dict(x=x,z=z,source=sample(x,z),passPerPulse=dict(counts),
            meanPass=sum(k*v for k,v in counts.items())/128,failures=dict(failures),
            maxPerimeterHeightDelta=maximum_delta,dustValues=sorted(dusts),
            nearestOwnerCentres=[dict(**o,distance=math.hypot(o['position'][0]-x,o['position'][2]-z)) for o in nearest])
    sea = []
    for x,z in ((1750,3100),(900,4150)):
        points = [[x+dx,z+dz,terrain.height_at(grid,x+dx,z+dz)]
                  for dz in range(-32,33,16) for dx in range(-32,33,16)]
        sea.append(dict(x=x,z=z,centreHeight=terrain.height_at(grid,x,z),
            minHeight=min(v[2] for v in points),maxHeight=max(v[2] for v in points),
            all25Shallow=all(-8 <= v[2] < -.1 for v in points),samples=points))
    result = dict(status='source-inference-only-runtime-pending',
        provenance={str(path):sha(path) for path in (folder/'noe.rvw4',folder/'noe.rvw4.meta',
            folder/'noe/noe.wrp',folder/'installed-config.json',folder/'noe-config.json',
            ROOT/'engine/Poseidon/World/Weather/RotorGroundPolicy.hpp',
            ROOT/'engine/Poseidon/World/Entities/Vehicles/Air/Helicopter.cpp',
            ROOT/'scripts/physics/inspect_stock_corpse_rig.py',args.tools,*archives.values())},
        actualModels=dict(aircraftSha256=hashlib.sha256(aircraft).hexdigest(),
            cloudletSha256=hashlib.sha256(sheet).hexdigest(),authoredAircraftSphere=sphere,
            rotorDiscRadius=sphere*.8,maximumRingAndFootprint=sphere*.8*.95+actual['footprint']),
        textures=images,actualCpuProfiles=actual['profiles'],shallowSeaCandidates=sea,
        sites=[site(2699.62,5149.56),site(2475,5075)],
        flatCandidate=dict(cell=[49,101],cornerHeights=grid[101:103,49:51].flatten().tolist(),
            quadrants=census.quadrants(textures[ids[101*256+49]],definitions),
            cellEdgeMargin=25,minimumFootprintToCellEdge=25-(sphere*.8*.95+actual['footprint'])),
        limitations='Actual CPU ring/profile; original 50m float triangular bed and authored quadrant dust. Source gate reproduction is an upper bound: no live deformation, installed road/water/object bounds or ray tests, allocation pressure, GPU alpha/depth, flight or appearance acceptance. Owner centres are exporter ordinals, not live IDs; distance does not prove object sphere clearance.')
    output = args.out/'rotor-land-profile-audit.json'
    output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(output=str(output),models=result['actualModels'],
        sites=[{k:v[k] for k in ('x','z','meanPass','passPerPulse','maxPerimeterHeightDelta')} for v in result['sites']]),indent=2))


if __name__ == '__main__':
    main()
