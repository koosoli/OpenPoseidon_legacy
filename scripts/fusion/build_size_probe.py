"""Generate a sparse 102.4 km diagnostic world, not a dense-world benchmark."""
from array import array
from pathlib import Path
import struct
import sys

from build_fusion import compressed, pack_pbo


def build(output):
    size, cell = 2048, 50
    count = size * size
    heights = array('f', [-40]) * count
    for cx, cz in ((10000, 10000), (51200, 51200), (92000, 92000)):
        for z in range(cz // cell - 44, cz // cell + 45):
            for x in range(cx // cell - 44, cx // cell + 45):
                r2 = ((x * cell - cx)**2 + (z * cell - cz)**2) / 1800**2
                heights[z * size + x] = max(-40, 250 * (1-r2))
    if sys.byteorder != 'little':
        heights.byteswap()
    data = b'OPRW' + struct.pack('<5I', 3, size, size, size, size)
    data += compressed(bytes(count * 4)) + compressed(bytes(count))
    data += struct.pack('<I', 0)
    data += compressed(b'\1\0' * count) + compressed(bytes(count * 4))
    data += compressed(heights.tobytes())
    data += struct.pack('<I', 2) + b'landtext\\mo.pac\0\0landtext\\pi.paa\0\0'
    data += struct.pack('<Ii', 0, -1)
    config = r'''class CfgPatches {class FusionSizeProbe {units[]={}; weapons[]={}; requiredVersion=1.96; requiredAddons[]={};};};
class CfgWorlds {class Eden; class FusionSizeProbe: Eden {
 description="DIAGNOSTIC - 102.4 km sparse world"; worldName="\fusion_size_probe\FusionSizeProbe.wrp";
 worldSize=102400; landGrid=50; centerPosition[]={51200,51200,0};
 class Names {};
};}; class CfgWorldList {class FusionSizeProbe {};};
'''
    output.mkdir(parents=True, exist_ok=True)
    (output / 'fusion_size_probe.pbo').write_bytes(pack_pbo(
        {'config.cpp': config.encode('ascii'), 'FusionSizeProbe.wrp': data}, 'fusion_size_probe'))
    mission = output / 'SizeProbe.FusionSizeProbe'
    mission.mkdir(exist_ok=True)
    (mission / 'mission.sqm').write_text('''version=11;
class Mission {
 addOns[]={"FusionSizeProbe"}; randomSeed=1234;
 class Intel {briefingName="102.4 km size probe"; year=1985; month=6; day=21; hour=12; minute=0; startWeather=0; forecastWeather=0; startFog=0; forecastFog=0;};
 class Groups {items=1; class Item0 {side="WEST"; class Vehicles {items=1;
 class Item0 {position[]={10000,400,10000}; id=0; side="WEST"; vehicle="UH60"; player="PLAYER COMMANDER"; leader=1; skill=1; special="FLY";};
 };};};
};
class Intro {randomSeed=1; class Intel {};};
class OutroWin {randomSeed=2; class Intel {};};
class OutroLoose {randomSeed=3; class Intel {};};
''', encoding='ascii')
    print(f'Generated {size}x{size} cells, {size*cell} metres, three synthetic islands, zero objects.')


if __name__ == '__main__':
    build(Path(sys.argv[1]))
